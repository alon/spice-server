#ifndef H_SPICE_IMAGE_CACHE
#define H_SPICE_IMAGE_CACHE

#include <inttypes.h>

#include "common/pixman_utils.h"
#include "common/canvas_base.h"
#include "common/ring.h"

/* FIXME: move back to display_channel.h (once structs are private) */
typedef struct Drawable Drawable;
typedef struct _DisplayChannelClient DisplayChannelClient;

typedef struct ImageCacheItem {
    RingItem lru_link;
    uint64_t id;
#ifdef IMAGE_CACHE_AGE
    uint32_t age;
#endif
    struct ImageCacheItem *next;
    pixman_image_t *image;
} ImageCacheItem;

#define IMAGE_CACHE_HASH_SIZE 1024

typedef struct ImageCache {
    SpiceImageCache base;
    ImageCacheItem *hash_table[IMAGE_CACHE_HASH_SIZE];
    Ring lru;
#ifdef IMAGE_CACHE_AGE
    uint32_t age;
#else
    uint32_t num_items;
#endif
} ImageCache;

int          image_cache_hit               (ImageCache *cache, uint64_t id);
void         image_cache_init              (ImageCache *cache);
void         image_cache_reset             (ImageCache *cache);
void         image_cache_aging             (ImageCache *cache);
void         image_cache_localize          (ImageCache *cache, SpiceImage **image_ptr,
                                            SpiceImage *image_store, Drawable *drawable);
void         image_cache_localize_brush    (ImageCache *cache, SpiceBrush *brush,
                                            SpiceImage *image_store);
void         image_cache_localize_mask     (ImageCache *cache, SpiceQMask *mask,
                                            SpiceImage *image_store);

#endif
