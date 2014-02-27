#ifndef CURSOR_CHANNEL_H_
# define CURSOR_CHANNEL_H_

#include "spice.h"
#include "reds.h"
#include "red_worker.h"
#include "red_parse_qxl.h"
#include "cache_item.h"
#include "stat.h"

#define CLIENT_CURSOR_CACHE_SIZE 256

#define CURSOR_CACHE_HASH_SHIFT 8
#define CURSOR_CACHE_HASH_SIZE (1 << CURSOR_CACHE_HASH_SHIFT)
#define CURSOR_CACHE_HASH_MASK (CURSOR_CACHE_HASH_SIZE - 1)
#define CURSOR_CACHE_HASH_KEY(id) ((id) & CURSOR_CACHE_HASH_MASK)

enum {
    PIPE_ITEM_TYPE_CURSOR = PIPE_ITEM_TYPE_COMMON_LAST,
    PIPE_ITEM_TYPE_CURSOR_INIT,
    PIPE_ITEM_TYPE_INVAL_CURSOR_CACHE,
};

typedef struct CursorItem {
    QXLInstance *qxl;
    uint32_t group_id;
    int refs;
    RedCursorCmd *red_cursor;
} CursorItem;

typedef struct CursorPipeItem {
    PipeItem base;
    CursorItem *cursor_item;
    int refs;
} CursorPipeItem;

typedef struct LocalCursor {
    CursorItem base;
    SpicePoint16 position;
    uint32_t data_size;
    SpiceCursor red_cursor;
} LocalCursor;

typedef struct CursorChannelClient {
    CommonChannelClient common;

    CacheItem *cursor_cache[CURSOR_CACHE_HASH_SIZE];
    Ring cursor_cache_lru;
    long cursor_cache_available;
    uint32_t cursor_cache_items;
} CursorChannelClient;

typedef struct CursorChannel {
    CommonChannel common; // Must be the first thing

    CursorItem *item;
    int cursor_visible;
    SpicePoint16 cursor_position;
    uint16_t cursor_trail_length;
    uint16_t cursor_trail_frequency;
    uint32_t mouse_mode;

#ifdef RED_STATISTICS
    StatNodeRef stat;
#endif
} CursorChannel;

G_STATIC_ASSERT(sizeof(CursorItem) <= QXL_CURSUR_DEVICE_DATA_SIZE);

CursorChannel*       cursor_channel_new         (RedWorker *worker);
void                 cursor_channel_disconnect  (CursorChannel *cursor);
void                 cursor_channel_reset       (CursorChannel *cursor);
void                 cursor_channel_process_cmd (CursorChannel *cursor, RedCursorCmd *cursor_cmd,
                                                 uint32_t group_id);

CursorChannelClient* cursor_channel_client_new  (CursorChannel *cursor,
                                                 RedClient *client, RedsStream *stream,
                                                 int mig_target,
                                                 uint32_t *common_caps, int num_common_caps,
                                                 uint32_t *caps, int num_caps);

CursorItem*          cursor_item_new            (QXLInstance *qxl, RedCursorCmd *cmd, uint32_t group_id);
CursorItem*          cursor_item_ref            (CursorItem *cursor);
void                 cursor_item_unref          (CursorItem *cursor);

#endif /* CURSOR_CHANNEL_H_ */
