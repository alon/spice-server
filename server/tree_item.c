#include <spice/qxl_dev.h>
#include "red_parse_qxl.h"
#include "display_channel.h"

#include "tree_item.h"

static const char *draw_type_to_str(uint8_t type)
{
    switch (type) {
    case QXL_DRAW_FILL:
        return "QXL_DRAW_FILL";
    case QXL_DRAW_OPAQUE:
        return "QXL_DRAW_OPAQUE";
    case QXL_DRAW_COPY:
        return "QXL_DRAW_COPY";
    case QXL_DRAW_TRANSPARENT:
        return "QXL_DRAW_TRANSPARENT";
    case QXL_DRAW_ALPHA_BLEND:
        return "QXL_DRAW_ALPHA_BLEND";
    case QXL_COPY_BITS:
        return "QXL_COPY_BITS";
    case QXL_DRAW_BLEND:
        return "QXL_DRAW_BLEND";
    case QXL_DRAW_BLACKNESS:
        return "QXL_DRAW_BLACKNESS";
    case QXL_DRAW_WHITENESS:
        return "QXL_DRAW_WHITENESS";
    case QXL_DRAW_INVERS:
        return "QXL_DRAW_INVERS";
    case QXL_DRAW_ROP3:
        return "QXL_DRAW_ROP3";
    case QXL_DRAW_COMPOSITE:
        return "QXL_DRAW_COMPOSITE";
    case QXL_DRAW_STROKE:
        return "QXL_DRAW_STROKE";
    case QXL_DRAW_TEXT:
        return "QXL_DRAW_TEXT";
    default:
        return "?";
    }
}

static void show_red_drawable(RedDrawable *drawable, const char *prefix)
{
    if (prefix) {
        printf("%s: ", prefix);
    }

    printf("%s effect %d bbox(%d %d %d %d)",
           draw_type_to_str(drawable->type),
           drawable->effect,
           drawable->bbox.top,
           drawable->bbox.left,
           drawable->bbox.bottom,
           drawable->bbox.right);

    switch (drawable->type) {
    case QXL_DRAW_FILL:
    case QXL_DRAW_OPAQUE:
    case QXL_DRAW_COPY:
    case QXL_DRAW_TRANSPARENT:
    case QXL_DRAW_ALPHA_BLEND:
    case QXL_COPY_BITS:
    case QXL_DRAW_BLEND:
    case QXL_DRAW_BLACKNESS:
    case QXL_DRAW_WHITENESS:
    case QXL_DRAW_INVERS:
    case QXL_DRAW_ROP3:
    case QXL_DRAW_COMPOSITE:
    case QXL_DRAW_STROKE:
    case QXL_DRAW_TEXT:
        break;
    default:
        spice_error("bad drawable type");
    }
    printf("\n");
}

static void show_draw_item(DrawItem *draw_item, const char *prefix)
{
    if (prefix) {
        printf("%s: ", prefix);
    }
    printf("effect %d bbox(%d %d %d %d)\n",
           draw_item->effect,
           draw_item->base.rgn.extents.x1,
           draw_item->base.rgn.extents.y1,
           draw_item->base.rgn.extents.x2,
           draw_item->base.rgn.extents.y2);
}

typedef struct DumpItem {
    int level;
    Container *container;
} DumpItem;

static void dump_item(TreeItem *item, void *data)
{
    DumpItem *di = data;
    const char *item_prefix = "|--";
    int i;

    if (di->container) {
        while (di->container != item->container) {
            di->level--;
            di->container = di->container->base.container;
        }
    }

    switch (item->type) {
    case TREE_ITEM_TYPE_DRAWABLE: {
        Drawable *drawable = SPICE_CONTAINEROF(item, Drawable, tree_item);
        const int max_indent = 200;
        char indent_str[max_indent + 1];
        int indent_str_len;

        for (i = 0; i < di->level; i++) {
            printf("  ");
        }
        printf(item_prefix, 0);
        show_red_drawable(drawable->red_drawable, NULL);
        for (i = 0; i < di->level; i++) {
            printf("  ");
        }
        printf("|  ");
        show_draw_item(&drawable->tree_item, NULL);
        indent_str_len = MIN(max_indent, strlen(item_prefix) + di->level * 2);
        memset(indent_str, ' ', indent_str_len);
        indent_str[indent_str_len] = 0;
        region_dump(&item->rgn, indent_str);
        printf("\n");
        break;
    }
    case TREE_ITEM_TYPE_CONTAINER:
        di->level++;
        di->container = (Container *)item;
        break;
    case TREE_ITEM_TYPE_SHADOW:
        break;
    }
}

static void tree_foreach(TreeItem *item, void (*f)(TreeItem *, void *), void * data)
{
    if (!item)
        return;

    f(item, data);

    if (item->type == TREE_ITEM_TYPE_CONTAINER) {
        Container *container = (Container*)item;
        RingItem *it;

        RING_FOREACH(it, &container->items) {
            tree_foreach(SPICE_CONTAINEROF(it, TreeItem, siblings_link), f, data);
        }
    }
}

void tree_item_dump(TreeItem *item)
{
    DumpItem di = { 0, };

    spice_return_if_fail(item != NULL);
    tree_foreach(item, dump_item, &di);
}
