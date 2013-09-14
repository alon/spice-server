#ifndef TREE_ITEM_H_
# define TREE_ITEM_H_

#include <stdint.h>
#include "common/region.h"
#include "common/ring.h"
#include "red_bitmap_utils.h"

enum {
    TREE_ITEM_TYPE_NONE,
    TREE_ITEM_TYPE_DRAWABLE,
    TREE_ITEM_TYPE_CONTAINER,
    TREE_ITEM_TYPE_SHADOW,

    TREE_ITEM_TYPE_LAST,
};

typedef struct _TreeItem TreeItem;
typedef struct _Shadow Shadow;
typedef struct _Container Container;
typedef struct _DrawItem DrawItem;

/* TODO consider GNode instead */
struct _TreeItem {
    RingItem siblings_link;
    uint32_t type;
    Container *container;
    QRegion rgn;
};

/* A region "below" a copy, or the src region of the copy */
struct _Shadow {
    TreeItem base;
    QRegion on_hold;
    DrawItem* owner;
};

#define IS_SHADOW(item) ((item)->type == TREE_ITEM_TYPE_SHADOW)
#define SHADOW(item) ((Container*)(item))

struct _Container {
    TreeItem base;
    Ring items;
};

#define IS_CONTAINER(item) ((item)->type == TREE_ITEM_TYPE_CONTAINER)
#define CONTAINER(item) ((Container*)(item))

struct _DrawItem {
    TreeItem base;
    uint8_t effect;
    uint8_t container_root;
    Shadow *shadow;
};

#define IS_DRAW_ITEM(item) ((item)->type == TREE_ITEM_TYPE_DRAWABLE)
#define DRAW_ITEM(item) ((DrawItem*)(item))

static inline int is_opaque_item(TreeItem *item)
{
    return item->type == TREE_ITEM_TYPE_CONTAINER ||
        (IS_DRAW_ITEM(item) && ((DrawItem *)item)->effect == QXL_EFFECT_OPAQUE);
}

void       tree_item_dump                           (TreeItem *item);
Shadow*    shadow_new                               (DrawItem *item, const SpicePoint *delta);
Container* container_new                            (DrawItem *item);

#endif /* TREE_ITEM_H_ */
