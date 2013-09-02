#ifndef CURSOR_CHANNEL_H_
# define CURSOR_CHANNEL_H_

#include "red_worker.h"
#include "stat.h"

#define CLIENT_CURSOR_CACHE_SIZE 256

#define CURSOR_CACHE_HASH_SHIFT 8
#define CURSOR_CACHE_HASH_SIZE (1 << CURSOR_CACHE_HASH_SHIFT)
#define CURSOR_CACHE_HASH_MASK (CURSOR_CACHE_HASH_SIZE - 1)
#define CURSOR_CACHE_HASH_KEY(id) ((id) & CURSOR_CACHE_HASH_MASK)

typedef struct CursorItem {
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

#ifdef RED_STATISTICS
    StatNodeRef stat;
#endif
} CursorChannel;

typedef struct _CursorItem _CursorItem;

struct _CursorItem {
    union {
        CursorItem cursor_item;
        _CursorItem *next;
    } u;
};

G_STATIC_ASSERT(sizeof(CursorItem) <= QXL_CURSUR_DEVICE_DATA_SIZE);


#endif /* CURSOR_CHANNEL_H_ */
