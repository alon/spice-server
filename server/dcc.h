#ifndef DCC_H_
# define DCC_H_

#include "red_worker.h"
#include "pixmap_cache.h"
#include "cache_item.h"
#include "dcc_encoders.h"
#include "stream.h"

#define PALETTE_CACHE_HASH_SHIFT 8
#define PALETTE_CACHE_HASH_SIZE (1 << PALETTE_CACHE_HASH_SHIFT)
#define PALETTE_CACHE_HASH_MASK (PALETTE_CACHE_HASH_SIZE - 1)
#define PALETTE_CACHE_HASH_KEY(id) ((id) & PALETTE_CACHE_HASH_MASK)
#define CLIENT_PALETTE_CACHE_SIZE 128

#define DISPLAY_CLIENT_TIMEOUT 30000000000ULL //nano
#define DISPLAY_CLIENT_MIGRATE_DATA_TIMEOUT 10000000000ULL //nano, 10 sec
#define DISPLAY_CLIENT_RETRY_INTERVAL 10000 //micro

/* Each drawable can refer to at most 3 images: src, brush and mask */
#define MAX_DRAWABLE_PIXMAP_CACHE_ITEMS 3

typedef struct WaitForChannels {
    SpiceMsgWaitForChannels header;
    SpiceWaitForChannel buf[MAX_CACHE_CLIENTS];
} WaitForChannels;

typedef struct FreeList {
    int res_size;
    SpiceResourceList *res;
    uint64_t sync[MAX_CACHE_CLIENTS];
    WaitForChannels wait;
} FreeList;

struct _DisplayChannelClient {
    CommonChannelClient common;
    spice_image_compression_t image_compression;
    spice_wan_compression_t jpeg_state;
    spice_wan_compression_t zlib_glz_state;
    int jpeg_quality;
    int zlib_level;

    QuicData quic_data;
    QuicContext *quic;
    LzData lz_data;
    LzContext  *lz;
    JpegData jpeg_data;
    JpegEncoderContext *jpeg;
    ZlibData zlib_data;
    ZlibEncoder *zlib;

    int expect_init;

    PixmapCache *pixmap_cache;
    uint32_t pixmap_cache_generation;
    int pending_pixmaps_sync;

    CacheItem *palette_cache[PALETTE_CACHE_HASH_SIZE];
    Ring palette_cache_lru;
    long palette_cache_available;
    uint32_t palette_cache_items;

    struct {
        uint32_t stream_outbuf_size;
        uint8_t *stream_outbuf; // caution stream buffer is also used as compress bufs!!!

        FreeList free_list;
        uint64_t pixmap_cache_items[MAX_DRAWABLE_PIXMAP_CACHE_ITEMS];
        int num_pixmap_cache_items;
    } send_data;

    /* global lz encoding entities */
    GlzSharedDictionary *glz_dict;
    GlzEncoderContext   *glz;
    GlzData glz_data;

    Ring glz_drawables;               // all the living lz drawable, ordered by encoding time
    Ring glz_drawables_inst_to_free;               // list of instances to be freed
    pthread_mutex_t glz_drawables_inst_to_free_lock;

    uint8_t surface_client_created[NUM_SURFACES];
    QRegion surface_client_lossy_region[NUM_SURFACES];

    StreamAgent stream_agents[NUM_STREAMS];
    int use_mjpeg_encoder_rate_control;
    uint32_t streams_max_latency;
    uint64_t streams_max_bit_rate;

    uint32_t glz_drawable_count;
};

#define DCC_TO_WORKER(dcc)                                              \
    (SPICE_CONTAINEROF((dcc)->common.base.channel, CommonChannel, base)->worker)
#define DCC_TO_DC(dcc)                                                  \
     SPICE_CONTAINEROF((dcc)->common.base.channel, DisplayChannel, common.base)
#define RCC_TO_DCC(rcc) SPICE_CONTAINEROF((rcc), DisplayChannelClient, common.base)

typedef struct SurfaceCreateItem {
    SpiceMsgSurfaceCreate surface_create;
    PipeItem pipe_item;
} SurfaceCreateItem;

typedef struct ImageItem {
    PipeItem link;
    int refs;
    SpicePoint pos;
    int width;
    int height;
    int stride;
    int top_down;
    int surface_id;
    int image_format;
    uint32_t image_flags;
    int can_lossy;
    uint8_t data[0];
} ImageItem;

typedef struct DrawablePipeItem {
    RingItem base;  /* link for a list of pipe items held by Drawable */
    PipeItem dpi_pipe_item; /* link for the client's pipe itself */
    Drawable *drawable;
    DisplayChannelClient *dcc;
    uint8_t refs;
} DrawablePipeItem;

void                       drawable_pipe_item_unref                  (DrawablePipeItem *dpi);
DrawablePipeItem*          drawable_pipe_item_ref                    (DrawablePipeItem *dpi);

DisplayChannelClient*      dcc_new                                   (DisplayChannel *display,
                                                                      RedClient *client,
                                                                      RedsStream *stream,
                                                                      int mig_target,
                                                                      uint32_t *common_caps,
                                                                      int num_common_caps,
                                                                      uint32_t *caps,
                                                                      int num_caps,
                                                                      spice_image_compression_t image_compression,
                                                                      spice_wan_compression_t jpeg_state,
                                                                      spice_wan_compression_t zlib_glz_state);
void                       dcc_start                                 (DisplayChannelClient *dcc);
void                       dcc_push_monitors_config                  (DisplayChannelClient *dcc);
void                       dcc_destroy_surface                       (DisplayChannelClient *dcc,
                                                                      uint32_t surface_id);
void                       dcc_stream_agent_clip                     (DisplayChannelClient* dcc,
                                                                      StreamAgent *agent);
void                       dcc_create_stream                         (DisplayChannelClient *dcc,
                                                                      Stream *stream);
void                       dcc_create_surface                        (DisplayChannelClient *dcc,
                                                                      int surface_id);
void                       dcc_push_surface_image                    (DisplayChannelClient *dcc,
                                                                      int surface_id);
ImageItem *                dcc_add_surface_area_image                (DisplayChannelClient *dcc,
                                                                      int surface_id,
                                                                      SpiceRect *area,
                                                                      PipeItem *pos,
                                                                      int can_lossy);
void                       dcc_palette_cache_reset                   (DisplayChannelClient *dcc);
void                       dcc_palette_cache_palette                 (DisplayChannelClient *dcc,
                                                                      SpicePalette *palette,
                                                                      uint8_t *flags);
int                        dcc_pixmap_cache_add                      (DisplayChannelClient *dcc,
                                                                      uint64_t id, uint32_t size, int lossy);
void                       dcc_add_drawable                          (DisplayChannelClient *dcc,
                                                                      Drawable *drawable,
                                                                      bool to_tail);
void                       dcc_add_drawable_after                    (DisplayChannelClient *dcc,
                                                                      Drawable *drawable,
                                                                      PipeItem *pos);

typedef struct compress_send_data_t {
    void*    comp_buf;
    uint32_t comp_buf_size;
    SpicePalette *lzplt_palette;
    int is_lossy;
} compress_send_data_t;

int                        dcc_compress_image                        (DisplayChannelClient *dcc,
                                                                      SpiceImage *dest, SpiceBitmap *src, Drawable *drawable,
                                                                      int can_lossy,
                                                                      compress_send_data_t* o_comp_data);
int                        dcc_compress_image_glz                    (DisplayChannelClient *dcc,
                                                                      SpiceImage *dest, SpiceBitmap *src, Drawable *drawable,
                                                                      compress_send_data_t* o_comp_data);
int                        dcc_compress_image_lz                     (DisplayChannelClient *dcc,
                                                                      SpiceImage *dest, SpiceBitmap *src,
                                                                      compress_send_data_t* o_comp_data, uint32_t group_id);
int                        dcc_compress_image_jpeg                   (DisplayChannelClient *dcc, SpiceImage *dest,
                                                                      SpiceBitmap *src, compress_send_data_t* o_comp_data,
                                                                      uint32_t group_id);
int                        dcc_compress_image_quic                   (DisplayChannelClient *dcc, SpiceImage *dest,
                                                                      SpiceBitmap *src, compress_send_data_t* o_comp_data,
                                                                      uint32_t group_id);

#endif /* DCC_H_ */
