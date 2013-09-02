#ifndef RED_WORKER_CLIENT_H_
# define RED_WORKER_CLIENT_H_

#include "red_worker.h"
#include "pixmap_cache.h"

typedef int64_t red_time_t;

typedef struct Drawable Drawable;

#define PALETTE_CACHE_HASH_SHIFT 8
#define PALETTE_CACHE_HASH_SIZE (1 << PALETTE_CACHE_HASH_SHIFT)
#define PALETTE_CACHE_HASH_MASK (PALETTE_CACHE_HASH_SIZE - 1)
#define PALETTE_CACHE_HASH_KEY(id) ((id) & PALETTE_CACHE_HASH_MASK)

/* Each drawable can refer to at most 3 images: src, brush and mask */
#define MAX_DRAWABLE_PIXMAP_CACHE_ITEMS 3

#define NUM_STREAMS 50
#define NUM_SURFACES 10000

typedef struct CacheItem CacheItem;

struct CacheItem {
    union {
        PipeItem pipe_data;
        struct {
            RingItem lru_link;
            CacheItem *next;
        } cache_data;
    } u;
    uint64_t id;
    size_t size;
    uint32_t inval_type;
};

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

typedef struct Stream Stream;
struct Stream {
    uint8_t refs;
    Drawable *current;
    red_time_t last_time;
    int width;
    int height;
    SpiceRect dest_area;
    int top_down;
    Stream *next;
    RingItem link;

    guint input_fps_timer;
    uint32_t num_input_frames;
    uint64_t input_fps_timer_start;
    uint32_t input_fps;
};

#define STREAM_STATS
#ifdef STREAM_STATS
typedef struct StreamStats {
   uint64_t num_drops_pipe;
   uint64_t num_drops_fps;
   uint64_t num_frames_sent;
   uint64_t num_input_frames;
   uint64_t size_sent;

   uint64_t start;
   uint64_t end;
} StreamStats;
#endif

typedef struct StreamAgent {
    QRegion vis_region; /* the part of the surface area that is currently occupied by video
                           fragments */
    QRegion clip;       /* the current video clipping. It can be different from vis_region:
                           for example, let c1 be the clip area at time t1, and c2
                           be the clip area at time t2, where t1 < t2. If c1 contains c2, and
                           at least part of c1/c2, hasn't been covered by a non-video images,
                           vis_region will contain c2 and also the part of c1/c2 that still
                           displays fragments of the video */

    PipeItem create_item;
    PipeItem destroy_item;
    Stream *stream;
    uint64_t last_send_time;
    MJpegEncoder *mjpeg_encoder;
    DisplayChannelClient *dcc;

    int frames;
    int drops;
    int fps;

    uint32_t report_id;
    uint32_t client_required_latency;
#ifdef STREAM_STATS
    StreamStats stats;
#endif
} StreamAgent;

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

#endif /* RED_WORKER_CLIENT_H_ */
