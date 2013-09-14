#ifndef DISPLAY_CHANNEL_H_
# define DISPLAY_CHANNEL_H_

#include <setjmp.h>

#include "red_worker.h"
#include "reds_stream.h"
#include "cache_item.h"
#include "pixmap_cache.h"
#ifdef USE_OPENGL
#include "common/ogl_ctx.h"
#include "reds_gl_canvas.h"
#endif /* USE_OPENGL */
#include "reds_sw_canvas.h"
#include "glz_encoder_dictionary.h"
#include "glz_encoder.h"
#include "stat.h"
#include "reds.h"
#include "mjpeg_encoder.h"
#include "red_memslots.h"
#include "red_parse_qxl.h"
#include "red_record_qxl.h"
#include "jpeg_encoder.h"
#include "demarshallers.h"
#include "zlib_encoder.h"
#include "red_channel.h"
#include "red_dispatcher.h"
#include "dispatcher.h"
#include "main_channel.h"
#include "migration_protocol.h"
#include "main_dispatcher.h"
#include "spice_server_utils.h"
#include "spice_bitmap_utils.h"
#include "spice_image_cache.h"
#include "pixmap_cache.h"
#include "utils.h"
#include "tree_item.h"
#include "stream.h"

typedef struct DisplayChannel DisplayChannel;

#define PALETTE_CACHE_HASH_SHIFT 8
#define PALETTE_CACHE_HASH_SIZE (1 << PALETTE_CACHE_HASH_SHIFT)
#define PALETTE_CACHE_HASH_MASK (PALETTE_CACHE_HASH_SIZE - 1)
#define PALETTE_CACHE_HASH_KEY(id) ((id) & PALETTE_CACHE_HASH_MASK)

#define CLIENT_PALETTE_CACHE_SIZE 128

/* Each drawable can refer to at most 3 images: src, brush and mask */
#define MAX_DRAWABLE_PIXMAP_CACHE_ITEMS 3

#define NUM_STREAMS 50
#define NUM_SURFACES 10000

#define RED_COMPRESS_BUF_SIZE (1024 * 64)
typedef struct RedCompressBuf RedCompressBuf;
struct RedCompressBuf {
    uint32_t buf[RED_COMPRESS_BUF_SIZE / 4];
    RedCompressBuf *next;
    RedCompressBuf *send_next;
};

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

typedef struct GlzSharedDictionary {
    RingItem base;
    GlzEncDictContext *dict;
    uint32_t refs;
    uint8_t id;
    pthread_rwlock_t encode_lock;
    int migrate_freeze;
    RedClient *client; // channel clients of the same client share the dict
} GlzSharedDictionary;

typedef struct  {
    DisplayChannelClient *dcc;
    RedCompressBuf *bufs_head;
    RedCompressBuf *bufs_tail;
    jmp_buf jmp_env;
    union {
        struct {
            SpiceChunks *chunks;
            int next;
            int stride;
            int reverse;
        } lines_data;
        struct {
            RedCompressBuf* next;
            int size_left;
        } compressed_data; // for encoding data that was already compressed by another method
    } u;
    char message_buf[512];
} EncoderData;

typedef struct {
    GlzEncoderUsrContext usr;
    EncoderData data;
} GlzData;

typedef struct DependItem {
    Drawable *drawable;
    RingItem ring_item;
} DependItem;

struct Drawable {
    uint8_t refs;
    RingItem surface_list_link;
    RingItem list_link;
    DrawItem tree_item;
    Ring pipes;
    PipeItem *pipe_item_rest;
    uint32_t size_pipe_item_rest;
    RedDrawable *red_drawable;

    Ring glz_ring;

    red_time_t creation_time;
    int frames_count;
    int gradual_frames_count;
    int last_gradual_frame;
    Stream *stream;
    Stream *sized_stream;
    int streamable;
    BitmapGradualType copy_bitmap_graduality;
    uint32_t group_id;
    DependItem depend_items[3];

    uint8_t *backed_surface_data;
    DependItem pipe_depend_items[3];

    int surface_id;
    int surfaces_dest[3];

    uint32_t process_commands_generation;
};

struct _DisplayChannelClient {
    CommonChannelClient common;

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

        RedCompressBuf *used_compress_bufs;

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
};

#define DCC_TO_WORKER(dcc)                                              \
    (SPICE_CONTAINEROF((dcc)->common.base.channel, CommonChannel, base)->worker)
#define DCC_TO_DC(dcc) SPICE_CONTAINEROF((dcc)->common.base.channel,    \
                                         DisplayChannel, common.base)
#define RCC_TO_DCC(rcc) SPICE_CONTAINEROF((rcc), DisplayChannelClient, common.base)


enum {
    PIPE_ITEM_TYPE_DRAW = PIPE_ITEM_TYPE_COMMON_LAST,
    PIPE_ITEM_TYPE_IMAGE,
    PIPE_ITEM_TYPE_STREAM_CREATE,
    PIPE_ITEM_TYPE_STREAM_CLIP,
    PIPE_ITEM_TYPE_STREAM_DESTROY,
    PIPE_ITEM_TYPE_UPGRADE,
    PIPE_ITEM_TYPE_MIGRATE_DATA,
    PIPE_ITEM_TYPE_PIXMAP_SYNC,
    PIPE_ITEM_TYPE_PIXMAP_RESET,
    PIPE_ITEM_TYPE_INVAL_PALLET_CACHE,
    PIPE_ITEM_TYPE_CREATE_SURFACE,
    PIPE_ITEM_TYPE_DESTROY_SURFACE,
    PIPE_ITEM_TYPE_MONITORS_CONFIG,
    PIPE_ITEM_TYPE_STREAM_ACTIVATE_REPORT,
};

DisplayChannelClient*      dcc_new                                   (DisplayChannel *display,
                                                                      RedClient *client,
                                                                      RedsStream *stream,
                                                                      int mig_target,
                                                                      uint32_t *common_caps,
                                                                      int num_common_caps,
                                                                      uint32_t *caps,
                                                                      int num_caps);
void                       dcc_push_monitors_config                  (DisplayChannelClient *dcc);
void                       dcc_push_destroy_surface                  (DisplayChannelClient *dcc,
                                                                      uint32_t surface_id);

typedef struct DrawablePipeItem {
    RingItem base;  /* link for a list of pipe items held by Drawable */
    PipeItem dpi_pipe_item; /* link for the client's pipe itself */
    Drawable *drawable;
    DisplayChannelClient *dcc;
    uint8_t refs;
} DrawablePipeItem;

DrawablePipeItem*          drawable_pipe_item_new                    (DisplayChannelClient *dcc,
                                                                      Drawable *drawable);
void                       drawable_pipe_item_unref                  (DrawablePipeItem *dpi);
DrawablePipeItem*          drawable_pipe_item_ref                    (DrawablePipeItem *dpi);

typedef struct MonitorsConfig {
    int refs;
    int count;
    int max_allowed;
    QXLHead heads[0];
} MonitorsConfig;

typedef struct MonitorsConfigItem {
    PipeItem pipe_item;
    MonitorsConfig *monitors_config;
} MonitorsConfigItem;

MonitorsConfig*            monitors_config_new                       (QXLHead *heads, ssize_t nheads,
                                                                      ssize_t max);
MonitorsConfig *           monitors_config_ref                       (MonitorsConfig *config);
void                       monitors_config_unref                     (MonitorsConfig *config);

#define TRACE_ITEMS_SHIFT 3
#define NUM_TRACE_ITEMS (1 << TRACE_ITEMS_SHIFT)
#define ITEMS_TRACE_MASK (NUM_TRACE_ITEMS - 1)

/* TODO: move to .c */
struct DisplayChannel {
    CommonChannel common; // Must be the first thing

    MonitorsConfig *monitors_config;

    uint32_t num_renderers;
    uint32_t renderers[RED_RENDERER_LAST];
    uint32_t renderer;

    Ring current_list; // of TreeItem
    uint32_t current_size;

    int enable_jpeg;
    int jpeg_quality;
    int enable_zlib_glz_wrap;
    int zlib_level;

    RedCompressBuf *free_compress_bufs;

    int stream_video;
    uint32_t stream_count;
    Stream streams_buf[NUM_STREAMS];
    Stream *free_streams;
    Ring streams;
    ItemTrace items_trace[NUM_TRACE_ITEMS];
    uint32_t next_item_trace;
    uint64_t streams_size_total;

    ImageCache image_cache;

#ifdef RED_STATISTICS
    uint64_t *cache_hits_counter;
    uint64_t *add_to_cache_counter;
    uint64_t *non_cache_counter;
#endif
#ifdef COMPRESS_STAT
    stat_info_t lz_stat;
    stat_info_t glz_stat;
    stat_info_t quic_stat;
    stat_info_t jpeg_stat;
    stat_info_t zlib_glz_stat;
    stat_info_t jpeg_alpha_stat;
#endif
};
typedef struct SurfaceDestroyItem {
    SpiceMsgSurfaceDestroy surface_destroy;
    PipeItem pipe_item;
} SurfaceDestroyItem;

typedef struct SurfaceCreateItem {
    SpiceMsgSurfaceCreate surface_create;
    PipeItem pipe_item;
} SurfaceCreateItem;

typedef struct UpgradeItem {
    PipeItem base;
    int refs;
    Drawable *drawable;
    SpiceClipRects *rects;
} UpgradeItem;


void                       display_channel_set_stream_video          (DisplayChannel *display,
                                                                      int stream_video);
void                       display_channel_attach_stream             (DisplayChannel *display,
                                                                      Drawable *drawable,
                                                                      Stream *stream);
int                        display_channel_get_streams_timout        (DisplayChannel *display);
void                       display_channel_compress_stats_print      (const DisplayChannel *display);
void                       display_channel_compress_stats_reset      (DisplayChannel *display);

#endif /* DISPLAY_CHANNEL_H_ */
