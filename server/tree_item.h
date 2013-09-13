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

struct _Container {
    TreeItem base;
    Ring items;
};

struct _DrawItem {
    TreeItem base;
    uint8_t effect;
    uint8_t container_root;
    Shadow *shadow;
};

#define IS_DRAW_ITEM(item) ((item)->type == TREE_ITEM_TYPE_DRAWABLE)

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

void       tree_item_dump                           (TreeItem *item);

#endif /* TREE_ITEM_H_ */
