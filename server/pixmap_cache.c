#include "pixmap_cache.h"

int pixmap_cache_set_lossy(PixmapCache *cache, uint64_t id, int lossy)
{
    NewCacheItem *item;
    pthread_mutex_lock(&cache->lock);

    item = cache->hash_table[BITS_CACHE_HASH_KEY(id)];

    while (item) {
        if (item->id == id) {
            item->lossy = lossy;
            break;
        }
        item = item->next;
    }
    pthread_mutex_unlock(&cache->lock);
    return !!item;
}

void pixmap_cache_clear(PixmapCache *cache)
{
    NewCacheItem *item;

    if (cache->freezed) {
        cache->lru.next = cache->freezed_head;
        cache->lru.prev = cache->freezed_tail;
        cache->freezed = FALSE;
    }

    while ((item = (NewCacheItem *)ring_get_head(&cache->lru))) {
        ring_remove(&item->lru_link);
        free(item);
    }
    memset(cache->hash_table, 0, sizeof(*cache->hash_table) * BITS_CACHE_HASH_SIZE);

    cache->available = cache->size;
    cache->items = 0;
}

int pixmap_cache_freeze(PixmapCache *cache)
{
    pthread_mutex_lock(&cache->lock);

    if (cache->freezed) {
        pthread_mutex_unlock(&cache->lock);
        return FALSE;
    }

    cache->freezed_head = cache->lru.next;
    cache->freezed_tail = cache->lru.prev;
    ring_init(&cache->lru);
    memset(cache->hash_table, 0, sizeof(*cache->hash_table) * BITS_CACHE_HASH_SIZE);
    cache->available = -1;
    cache->freezed = TRUE;

    pthread_mutex_unlock(&cache->lock);
    return TRUE;
}

static void pixmap_cache_destroy(PixmapCache *cache)
{
    spice_assert(cache);

    pthread_mutex_lock(&cache->lock);
    pixmap_cache_clear(cache);
    pthread_mutex_unlock(&cache->lock);
}


static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
static Ring pixmap_cache_list = {&pixmap_cache_list, &pixmap_cache_list};

static PixmapCache *pixmap_cache_new(RedClient *client, uint8_t id, int64_t size)
{
    PixmapCache *cache = spice_new0(PixmapCache, 1);

    ring_item_init(&cache->base);
    pthread_mutex_init(&cache->lock, NULL);
    cache->id = id;
    cache->refs = 1;
    ring_init(&cache->lru);
    cache->available = size;
    cache->size = size;
    cache->client = client;

    return cache;
}

PixmapCache *pixmap_cache_get(RedClient *client, uint8_t id, int64_t size)
{
    PixmapCache *ret = NULL;
    RingItem *now;
    pthread_mutex_lock(&cache_lock);

    now = &pixmap_cache_list;
    while ((now = ring_next(&pixmap_cache_list, now))) {
        PixmapCache *cache = (PixmapCache *)now;
        if ((cache->client == client) && (cache->id == id)) {
            ret = cache;
            ret->refs++;
            break;
        }
    }
    if (!ret) {
        ret = pixmap_cache_new(client, id, size);
        ring_add(&pixmap_cache_list, &ret->base);
    }
    pthread_mutex_unlock(&cache_lock);
    return ret;
}


void pixmap_cache_unref(PixmapCache *cache)
{
    if (!cache)
        return;

    pthread_mutex_lock(&cache_lock);
    if (--cache->refs) {
        pthread_mutex_unlock(&cache_lock);
        return;
    }
    ring_remove(&cache->base);
    pthread_mutex_unlock(&cache_lock);
    pixmap_cache_destroy(cache);
    free(cache);
}
