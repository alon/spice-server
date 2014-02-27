#ifndef CACHE_ITEM_H_
# define CACHE_ITEM_H_

#include "red_channel.h"
#include "common/ring.h"

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

#endif /* CACHE_ITEM_H_ */
