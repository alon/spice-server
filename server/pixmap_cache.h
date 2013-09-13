#ifndef _PIXMAP_CACHE_H
# define _PIXMAP_CACHE_H

#include "red_channel.h"
#include "spice_server_utils.h"

#define MAX_CACHE_CLIENTS 4

#define BITS_CACHE_HASH_SHIFT 10
#define BITS_CACHE_HASH_SIZE (1 << BITS_CACHE_HASH_SHIFT)
#define BITS_CACHE_HASH_MASK (BITS_CACHE_HASH_SIZE - 1)
#define BITS_CACHE_HASH_KEY(id) ((id) & BITS_CACHE_HASH_MASK)

typedef struct _PixmapCache PixmapCache;
typedef struct _NewCacheItem NewCacheItem;

struct _NewCacheItem {
    RingItem lru_link;
    NewCacheItem *next;
    uint64_t id;
    uint64_t sync[MAX_CACHE_CLIENTS];
    size_t size;
    int lossy;
};

struct _PixmapCache {
    RingItem base;
    pthread_mutex_t lock;
    uint8_t id;
    uint32_t refs;
    NewCacheItem *hash_table[BITS_CACHE_HASH_SIZE];
    Ring lru;
    int64_t available;
    int64_t size;
    int32_t items;

    int freezed;
    RingItem *freezed_head;
    RingItem *freezed_tail;

    uint32_t generation;
    struct {
        uint8_t client;
        uint64_t message;
    } generation_initiator;
    uint64_t sync[MAX_CACHE_CLIENTS]; // here CLIENTS refer to different channel
                                      // clients of the same client
    RedClient *client;
};

PixmapCache *pixmap_cache_get(RedClient *client, uint8_t id, int64_t size);
void         pixmap_cache_unref(PixmapCache *cache);
void         pixmap_cache_clear(PixmapCache *cache);
int          pixmap_cache_set_lossy(PixmapCache *cache, uint64_t id, int lossy);
int          pixmap_cache_freeze(PixmapCache *cache);

#endif /* _PIXMAP_CACHE_H */
