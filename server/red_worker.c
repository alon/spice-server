/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define SPICE_LOG_DOMAIN "SpiceWorker"

/* Common variable abberiviations:
 *
 * rcc - RedChannelClient
 * ccc - CursorChannelClient (not to be confused with common_cc)
 * common_cc - CommonChannelClient
 * dcc - DisplayChannelClient
 * cursor_red_channel - downcast of CursorChannel to RedChannel
 * display_red_channel - downcast of DisplayChannel to RedChannel
 */

#include <stdio.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>
#include <netinet/tcp.h>
#include <openssl/ssl.h>
#include <inttypes.h>
#include <glib.h>

#include <spice/protocol.h>
#include <spice/qxl_dev.h>
#include "common/lz.h"
#include "common/marshaller.h"
#include "common/rect.h"
#include "common/region.h"
#include "common/ring.h"
#include "common/generated_server_marshallers.h"

#include "display_channel.h"
#include "stream.h"

#include "spice.h"
#include "red_worker.h"
#include "cursor_channel.h"
#include "tree_item.h"
#include "utils.h"

//#define COMPRESS_STAT
//#define DUMP_BITMAP
//#define COMPRESS_DEBUG

#define CMD_RING_POLL_TIMEOUT 10 //milli
#define CMD_RING_POLL_RETRIES 200

#define DISPLAY_CLIENT_SHORT_TIMEOUT 15000000000ULL //nano

#define VALIDATE_SURFACE_RET(worker, surface_id) \
    if (!validate_surface(worker, surface_id)) { \
        rendering_incorrect(__func__); \
        return; \
    }

#define VALIDATE_SURFACE_RETVAL(worker, surface_id, ret) \
    if (!validate_surface(worker, surface_id)) { \
        rendering_incorrect(__func__); \
        return ret; \
    }

#define VALIDATE_SURFACE_BREAK(worker, surface_id) \
    if (!validate_surface(worker, surface_id)) { \
        rendering_incorrect(__func__); \
        break; \
    }

static void rendering_incorrect(const char *msg)
{
    spice_warning("rendering incorrect from now on: %s", msg);
}

#define MAX_EVENT_SOURCES 20

#define MAX_LZ_ENCODERS MAX_CACHE_CLIENTS

#define MAX_PIPE_SIZE 50

#define WIDE_CLIENT_ACK_WINDOW 40
#define NARROW_CLIENT_ACK_WINDOW 20

pthread_mutex_t glz_dictionary_list_lock = PTHREAD_MUTEX_INITIALIZER;
Ring glz_dictionary_list = {&glz_dictionary_list, &glz_dictionary_list};

typedef struct RedWorker {
    pthread_t thread;
    clockid_t clockid;
    GMainContext *main_context;
    QXLInstance *qxl;
    RedDispatcher *red_dispatcher;
    int running;
    gint timeout;

    DisplayChannel *display_channel;
    uint32_t display_poll_tries;
    CursorChannel *cursor_channel;
    uint32_t cursor_poll_tries;

    uint32_t red_drawable_count;
    uint32_t bits_unique;
    RedMemSlotInfo mem_slots;

    spice_image_compression_t image_compression;
    spice_wan_compression_t jpeg_state;
    spice_wan_compression_t zlib_glz_state;

    uint32_t process_commands_generation;
#ifdef RED_STATISTICS
    StatNodeRef stat;
    uint64_t *wakeup_counter;
    uint64_t *command_counter;
#endif

    int driver_cap_monitors_config;
    int set_client_capabilities_pending;

    FILE *record_fd;
    bool wait_for_clients;
} RedWorker;

typedef enum {
    BITMAP_DATA_TYPE_INVALID,
    BITMAP_DATA_TYPE_CACHE,
    BITMAP_DATA_TYPE_SURFACE,
    BITMAP_DATA_TYPE_BITMAP,
    BITMAP_DATA_TYPE_BITMAP_TO_CACHE,
} BitmapDataType;

typedef struct BitmapData {
    BitmapDataType type;
    uint64_t id; // surface id or cache item id
    SpiceRect lossy_rect;
} BitmapData;

static void red_draw_qxl_drawable(DisplayChannel *display, Drawable *drawable);
static void red_draw_drawable(DisplayChannel *display, Drawable *item);
static void red_update_area(DisplayChannel *display, const SpiceRect *area, int surface_id);
static void red_update_area_till(DisplayChannel *display, const SpiceRect *area, int surface_id,
                                 Drawable *last);
static inline void display_begin_send_message(RedChannelClient *rcc);
static void dcc_release_glz(DisplayChannelClient *dcc);
static int red_display_free_some_independent_glz_drawables(DisplayChannelClient *dcc);
static void display_channel_client_release_item_before_push(DisplayChannelClient *dcc,
                                                            PipeItem *item);
static void display_channel_client_release_item_after_push(DisplayChannelClient *dcc,
                                                           PipeItem *item);
static void red_create_surface(DisplayChannel *display, uint32_t surface_id, uint32_t width,
                               uint32_t height, int32_t stride, uint32_t format,
                               void *line_0, int data_is_valid, int send_client);

void attach_stream(DisplayChannel *display, Drawable *drawable, Stream *stream)
{
    DisplayChannelClient *dcc;
    RingItem *item, *next;

    spice_assert(!drawable->stream && !stream->current);
    spice_assert(drawable && stream);
    stream->current = drawable;
    drawable->stream = stream;
    stream->last_time = drawable->creation_time;
    stream->num_input_frames++;

    FOREACH_DCC(display, item, next, dcc) {
        StreamAgent *agent;
        QRegion clip_in_draw_dest;

        agent = &dcc->stream_agents[get_stream_id(display, stream)];
        region_or(&agent->vis_region, &drawable->tree_item.base.rgn);

        region_init(&clip_in_draw_dest);
        region_add(&clip_in_draw_dest, &drawable->red_drawable->bbox);
        region_and(&clip_in_draw_dest, &agent->clip);

        if (!region_is_equal(&clip_in_draw_dest, &drawable->tree_item.base.rgn)) {
            region_remove(&agent->clip, &drawable->red_drawable->bbox);
            region_or(&agent->clip, &drawable->tree_item.base.rgn);
            dcc_stream_agent_clip(dcc, agent);
        }
#ifdef STREAM_STATS
        agent->stats.num_input_frames++;
#endif
    }
}

GMainContext* red_worker_get_context(RedWorker *worker)
{
    spice_return_val_if_fail(worker, NULL);

    return worker->main_context;
}

/* fixme: move to display channel */
DrawablePipeItem *drawable_pipe_item_new(DisplayChannelClient *dcc,
                                         Drawable *drawable)
{
    DrawablePipeItem *dpi;

    dpi = spice_malloc0(sizeof(*dpi));
    dpi->drawable = drawable;
    dpi->dcc = dcc;
    ring_item_init(&dpi->base);
    ring_add(&drawable->pipes, &dpi->base);
    red_channel_pipe_item_init(RED_CHANNEL_CLIENT(dcc)->channel,
                               &dpi->dpi_pipe_item, PIPE_ITEM_TYPE_DRAW);
    dpi->refs++;
    drawable->refs++;
    return dpi;
}

DrawablePipeItem *drawable_pipe_item_ref(DrawablePipeItem *dpi)
{
    dpi->refs++;
    return dpi;
}

void drawable_pipe_item_unref(DrawablePipeItem *dpi)
{
    DisplayChannel *display = DCC_TO_DC(dpi->dcc);

    if (--dpi->refs) {
        return;
    }

    spice_warn_if_fail(!ring_item_is_linked(&dpi->dpi_pipe_item.link));
    spice_warn_if_fail(!ring_item_is_linked(&dpi->base));
    display_channel_drawable_unref(display, dpi->drawable);
    free(dpi);
}

QXLInstance* red_worker_get_qxl(RedWorker *worker)
{
    spice_return_val_if_fail(worker != NULL, NULL);

    return worker->qxl;
}

static inline void __validate_surface(DisplayChannel *display, uint32_t surface_id)
{
    spice_warn_if(surface_id >= display->n_surfaces);
}

static inline int validate_surface(DisplayChannel *display, uint32_t surface_id)
{
    if (surface_id == G_MAXUINT32)
        return 1;

    spice_warn_if(surface_id >= display->n_surfaces);
    if (!display->surfaces[surface_id].context.canvas) {
        spice_warning("canvas address is %p for %d (and is NULL)\n",
                   &(display->surfaces[surface_id].context.canvas), surface_id);
        spice_warning("failed on %d", surface_id);
        return 0;
    }
    return 1;
}


static inline void red_handle_drawable_surfaces_client_synced(
                        DisplayChannelClient *dcc, Drawable *drawable)
{
    DisplayChannel *display = DCC_TO_DC(dcc);
    int x;

    for (x = 0; x < 3; ++x) {
        int surface_id;

        surface_id = drawable->surface_deps[x];
        if (surface_id != -1) {
            if (dcc->surface_client_created[surface_id] == TRUE) {
                continue;
            }
            dcc_create_surface(dcc, surface_id);
            display_channel_current_flush(display, surface_id);
            dcc_push_surface_image(dcc, surface_id);
        }
    }

    if (dcc->surface_client_created[drawable->surface_id] == TRUE) {
        return;
    }

    dcc_create_surface(dcc, drawable->surface_id);
    display_channel_current_flush(display, drawable->surface_id);
    dcc_push_surface_image(dcc, drawable->surface_id);
}

static int display_is_connected(RedWorker *worker)
{
    return worker->display_channel &&
        red_channel_is_connected(RED_CHANNEL(worker->display_channel));
}

static int cursor_is_connected(RedWorker *worker)
{
    return worker->cursor_channel &&
        red_channel_is_connected(RED_CHANNEL(worker->cursor_channel));
}

void dcc_add_drawable(DisplayChannelClient *dcc, Drawable *drawable)
{
    DrawablePipeItem *dpi;

    red_handle_drawable_surfaces_client_synced(dcc, drawable);
    dpi = drawable_pipe_item_new(dcc, drawable);
    red_channel_client_pipe_add(RED_CHANNEL_CLIENT(dcc), &dpi->dpi_pipe_item);
}

void red_pipes_add_drawable(DisplayChannel *display, Drawable *drawable)
{
    DisplayChannelClient *dcc;
    RingItem *dcc_ring_item, *next;

    spice_warn_if(!ring_is_empty(&drawable->pipes));
    FOREACH_DCC(display, dcc_ring_item, next, dcc) {
        dcc_add_drawable(dcc, drawable);
    }
}

static void dcc_add_drawable_to_tail(DisplayChannelClient *dcc, Drawable *drawable)
{
    DrawablePipeItem *dpi;

    if (!dcc) {
        return;
    }
    red_handle_drawable_surfaces_client_synced(dcc, drawable);
    dpi = drawable_pipe_item_new(dcc, drawable);
    red_channel_client_pipe_add_tail(RED_CHANNEL_CLIENT(dcc), &dpi->dpi_pipe_item);
}

void red_pipes_add_drawable_after(DisplayChannel *display,
                                  Drawable *drawable, Drawable *pos_after)
{
    DrawablePipeItem *dpi, *dpi_pos_after;
    RingItem *dpi_link, *dpi_next;
    DisplayChannelClient *dcc;
    int num_other_linked = 0;

    DRAWABLE_FOREACH_DPI_SAFE(pos_after, dpi_link, dpi_next, dpi_pos_after) {
        num_other_linked++;
        dcc = dpi_pos_after->dcc;
        red_handle_drawable_surfaces_client_synced(dcc, drawable);
        dpi = drawable_pipe_item_new(dcc, drawable);
        red_channel_client_pipe_add_after(RED_CHANNEL_CLIENT(dcc), &dpi->dpi_pipe_item,
                                          &dpi_pos_after->dpi_pipe_item);
    }
    if (num_other_linked == 0) {
        red_pipes_add_drawable(display, drawable);
        return;
    }
    if (num_other_linked != display->common.base.clients_num) {
        RingItem *item, *next;
        spice_debug("TODO: not O(n^2)");
        FOREACH_DCC(display, item, next, dcc) {
            int sent = 0;
            DRAWABLE_FOREACH_DPI_SAFE(pos_after, dpi_link, dpi_next, dpi_pos_after) {
                if (dpi_pos_after->dcc == dcc) {
                    sent = 1;
                    break;
                }
            }
            if (!sent) {
                dcc_add_drawable(dcc, drawable);
            }
        }
    }
}

static inline PipeItem *red_pipe_get_tail(DisplayChannelClient *dcc)
{
    if (!dcc) {
        return NULL;
    }

    return (PipeItem*)ring_get_tail(&RED_CHANNEL_CLIENT(dcc)->pipe);
}

void red_pipes_remove_drawable(Drawable *drawable)
{
    DrawablePipeItem *dpi;
    RingItem *item, *next;

    RING_FOREACH_SAFE(item, next, &drawable->pipes) {
        dpi = SPICE_CONTAINEROF(item, DrawablePipeItem, base);
        if (pipe_item_is_linked(&dpi->dpi_pipe_item)) {
            red_channel_client_pipe_remove_and_release(RED_CHANNEL_CLIENT(dpi->dcc),
                                                       &dpi->dpi_pipe_item);
        }
    }
}

static inline void red_pipe_add_image_item(DisplayChannelClient *dcc, ImageItem *item)
{
    if (!dcc) {
        return;
    }
    item->refs++;
    red_channel_client_pipe_add(RED_CHANNEL_CLIENT(dcc), &item->link);
}

static inline void red_pipe_add_image_item_after(DisplayChannelClient *dcc, ImageItem *item,
                                                 PipeItem *pos)
{
    if (!dcc) {
        return;
    }
    item->refs++;
    red_channel_client_pipe_add_after(RED_CHANNEL_CLIENT(dcc), &item->link, pos);
}

static void release_image_item(ImageItem *item)
{
    if (!--item->refs) {
        free(item);
    }
}

static void upgrade_item_unref(DisplayChannel *display, UpgradeItem *item)
{
    if (--item->refs)
        return;

    display_channel_drawable_unref(display, item->drawable);
    free(item->rects);
    free(item);
}

static uint8_t *common_alloc_recv_buf(RedChannelClient *rcc, uint16_t type, uint32_t size)
{
    CommonChannel *common = SPICE_CONTAINEROF(rcc->channel, CommonChannel, base);

    /* SPICE_MSGC_MIGRATE_DATA is the only client message whose size is dynamic */
    if (type == SPICE_MSGC_MIGRATE_DATA) {
        return spice_malloc(size);
    }

    if (size > CHANNEL_RECEIVE_BUF_SIZE) {
        spice_critical("unexpected message size %u (max is %d)", size, CHANNEL_RECEIVE_BUF_SIZE);
        return NULL;
    }
    return common->recv_buf;
}

static void common_release_recv_buf(RedChannelClient *rcc, uint16_t type, uint32_t size,
                                    uint8_t* msg)
{
    if (type == SPICE_MSGC_MIGRATE_DATA) {
        free(msg);
    }
}


static Drawable* drawable_try_new(DisplayChannel *display)
{
    Drawable *drawable;

    if (!display->free_drawables)
        return NULL;

    drawable = &display->free_drawables->u.drawable;
    display->free_drawables = display->free_drawables->u.next;
    display->drawable_count++;

    return drawable;
}

static void drawable_free(DisplayChannel *display, Drawable *drawable)
{
    ((_Drawable *)drawable)->u.next = display->free_drawables;
    display->free_drawables = (_Drawable *)drawable;
}

static void drawables_init(DisplayChannel *display)
{
    int i;

    display->free_drawables = NULL;
    for (i = 0; i < NUM_DRAWABLES; i++) {
        drawable_free(display, &display->drawables[i].u.drawable);
    }
}


void red_drawable_unref(RedWorker *worker, RedDrawable *red_drawable,
                        uint32_t group_id)
{
    QXLReleaseInfoExt release_info_ext;

    if (--red_drawable->refs) {
        return;
    }
    worker->red_drawable_count--;
    release_info_ext.group_id = group_id;
    release_info_ext.info = red_drawable->release_info;
    worker->qxl->st->qif->release_resource(worker->qxl, release_info_ext);
    red_put_drawable(red_drawable);
    free(red_drawable);
}

static void remove_depended_item(DependItem *item)
{
    spice_assert(item->drawable);
    spice_assert(ring_item_is_linked(&item->ring_item));
    item->drawable = NULL;
    ring_remove(&item->ring_item);
}

static void drawable_unref_surface_deps(DisplayChannel *display, Drawable *drawable)
{
    int x;
    int surface_id;

    for (x = 0; x < 3; ++x) {
        surface_id = drawable->surface_deps[x];
        if (surface_id == -1) {
            continue;
        }
        display_channel_surface_unref(display, surface_id);
    }
}

static void drawable_remove_dependencies(DisplayChannel *display, Drawable *drawable)
{
    int x;
    int surface_id;

    for (x = 0; x < 3; ++x) {
        surface_id = drawable->surface_deps[x];
        if (surface_id != -1 && drawable->depend_items[x].drawable) {
            remove_depended_item(&drawable->depend_items[x]);
        }
    }
}

void display_channel_drawable_unref(DisplayChannel *display, Drawable *drawable)
{
    RingItem *item, *next;

    if (--drawable->refs)
        return;

    spice_return_if_fail(!drawable->tree_item.shadow);
    spice_return_if_fail(ring_is_empty(&drawable->pipes));

    if (drawable->stream) {
        detach_stream(display, drawable->stream, TRUE);
    }
    region_destroy(&drawable->tree_item.base.rgn);

    drawable_remove_dependencies(display, drawable);
    drawable_unref_surface_deps(display, drawable);
    display_channel_surface_unref(display, drawable->surface_id);

    RING_FOREACH_SAFE(item, next, &drawable->glz_ring) {
        SPICE_CONTAINEROF(item, RedGlzDrawable, drawable_link)->drawable = NULL;
        ring_remove(item);
    }
    red_drawable_unref(COMMON_CHANNEL(display)->worker, drawable->red_drawable, drawable->group_id);
    drawable_free(display, drawable);
    display->drawable_count--;
}

static void display_stream_trace_add_drawable(DisplayChannel *display, Drawable *item)
{
    ItemTrace *trace;

    if (!item->stream || !item->streamable) {
        return;
    }

    trace = &display->items_trace[display->next_item_trace++ & ITEMS_TRACE_MASK];
    trace->time = item->creation_time;
    trace->frames_count = item->frames_count;
    trace->gradual_frames_count = item->gradual_frames_count;
    trace->last_gradual_frame = item->last_gradual_frame;
    SpiceRect* src_area = &item->red_drawable->u.copy.src_area;
    trace->width = src_area->right - src_area->left;
    trace->height = src_area->bottom - src_area->top;
    trace->dest_area = item->red_drawable->bbox;
}

static void surface_flush(DisplayChannel *display, int surface_id, SpiceRect *rect)
{
    red_update_area(display, rect, surface_id);
}

static void red_flush_source_surfaces(DisplayChannel *display, Drawable *drawable)
{
    int x;
    int surface_id;

    for (x = 0; x < 3; ++x) {
        surface_id = drawable->surface_deps[x];
        if (surface_id != -1 && drawable->depend_items[x].drawable) {
            remove_depended_item(&drawable->depend_items[x]);
            surface_flush(display, surface_id, &drawable->red_drawable->surfaces_rects[x]);
        }
    }
}

void current_remove_drawable(DisplayChannel *display, Drawable *item)
{
    /* todo: move all to unref? */
    display_stream_trace_add_drawable(display, item);
    draw_item_remove_shadow(&item->tree_item);
    ring_remove(&item->tree_item.base.siblings_link);
    ring_remove(&item->list_link);
    ring_remove(&item->surface_list_link);
    display_channel_drawable_unref(display, item);
    display->current_size--;
}

void current_remove(DisplayChannel *display, TreeItem *item)
{
    TreeItem *now = item;

    /* depth-first tree traversal, todo: do a to tree_foreach()? */
    for (;;) {
        Container *container = now->container;
        RingItem *ring_item;

        if (now->type == TREE_ITEM_TYPE_DRAWABLE) {
            Drawable *drawable = SPICE_CONTAINEROF(now, Drawable, tree_item);
            ring_item = now->siblings_link.prev;
            red_pipes_remove_drawable(drawable);
            current_remove_drawable(display, drawable);
        } else {
            Container *container = (Container *)now;

            spice_assert(now->type == TREE_ITEM_TYPE_CONTAINER);

            if ((ring_item = ring_get_head(&container->items))) {
                now = SPICE_CONTAINEROF(ring_item, TreeItem, siblings_link);
                continue;
            }
            ring_item = now->siblings_link.prev;
            container_free(container);
        }
        if (now == item) {
            return;
        }

        if ((ring_item = ring_next(&container->items, ring_item))) {
            now = SPICE_CONTAINEROF(ring_item, TreeItem, siblings_link);
        } else {
            now = (TreeItem *)container;
        }
    }
}

static void current_remove_all(DisplayChannel *display, int surface_id)
{
    Ring *ring = &display->surfaces[surface_id].current;
    RingItem *ring_item;

    while ((ring_item = ring_get_head(ring))) {
        TreeItem *now = SPICE_CONTAINEROF(ring_item, TreeItem, siblings_link);
        current_remove(display, now);
    }
}

/*
 * Return: TRUE if wait_if_used == FALSE, or otherwise, if all of the pipe items that
 * are related to the surface have been cleared (or sent) from the pipe.
 */
static int red_clear_surface_drawables_from_pipe(DisplayChannelClient *dcc, int surface_id,
                                                 int wait_if_used)
{
    Ring *ring;
    PipeItem *item;
    int x;
    RedChannelClient *rcc;

    if (!dcc) {
        return TRUE;
    }

    /* removing the newest drawables that their destination is surface_id and
       no other drawable depends on them */

    rcc = RED_CHANNEL_CLIENT(dcc);
    ring = &rcc->pipe;
    item = (PipeItem *) ring;
    while ((item = (PipeItem *)ring_next(ring, (RingItem *)item))) {
        Drawable *drawable;
        DrawablePipeItem *dpi = NULL;
        int depend_found = FALSE;

        if (item->type == PIPE_ITEM_TYPE_DRAW) {
            dpi = SPICE_CONTAINEROF(item, DrawablePipeItem, dpi_pipe_item);
            drawable = dpi->drawable;
        } else if (item->type == PIPE_ITEM_TYPE_UPGRADE) {
            drawable = ((UpgradeItem *)item)->drawable;
        } else {
            continue;
        }

        if (drawable->surface_id == surface_id) {
            PipeItem *tmp_item = item;
            item = (PipeItem *)ring_prev(ring, (RingItem *)item);
            red_channel_client_pipe_remove_and_release(rcc, tmp_item);
            if (!item) {
                item = (PipeItem *)ring;
            }
            continue;
        }

        for (x = 0; x < 3; ++x) {
            if (drawable->surface_deps[x] == surface_id) {
                depend_found = TRUE;
                break;
            }
        }

        if (depend_found) {
            spice_debug("surface %d dependent item found %p, %p", surface_id, drawable, item);
            if (wait_if_used) {
                break;
            } else {
                return TRUE;
            }
        }
    }

    if (!wait_if_used) {
        return TRUE;
    }

    if (item) {
        return red_channel_client_wait_pipe_item_sent(RED_CHANNEL_CLIENT(dcc), item,
                                                      DISPLAY_CLIENT_TIMEOUT);
    } else {
        /*
         * in case that the pipe didn't contain any item that is dependent on the surface, but
         * there is one during sending. Use a shorter timeout, since it is just one item
         */
        return red_channel_client_wait_outgoing_item(RED_CHANNEL_CLIENT(dcc),
                                                     DISPLAY_CLIENT_SHORT_TIMEOUT);
    }
    return TRUE;
}

static void red_clear_surface_drawables_from_pipes(DisplayChannel *display,
                                                   int surface_id,
                                                   int wait_if_used)
{
    RingItem *item, *next;
    DisplayChannelClient *dcc;

    FOREACH_DCC(display, item, next, dcc) {
        if (!red_clear_surface_drawables_from_pipe(dcc, surface_id, wait_if_used)) {
            red_channel_client_disconnect(RED_CHANNEL_CLIENT(dcc));
        }
    }
}

void detach_stream(DisplayChannel *display, Stream *stream,
                   int detach_sized)
{
    spice_assert(stream->current && stream->current->stream);
    spice_assert(stream->current->stream == stream);
    stream->current->stream = NULL;
    if (detach_sized) {
        stream->current->sized_stream = NULL;
    }
    stream->current = NULL;
}

static int red_display_drawable_is_in_pipe(DisplayChannelClient *dcc, Drawable *drawable)
{
    DrawablePipeItem *dpi;
    RingItem *dpi_link, *dpi_next;

    DRAWABLE_FOREACH_DPI_SAFE(drawable, dpi_link, dpi_next, dpi) {
        if (dpi->dcc == dcc) {
            return TRUE;
        }
    }

    return FALSE;
}

/*
 * after red_display_detach_stream_gracefully is called for all the display channel clients,
 * detach_stream should be called. See comment (1).
 */
static void dcc_detach_stream_gracefully(DisplayChannelClient *dcc,
                                         Stream *stream,
                                         Drawable *update_area_limit)
{
    DisplayChannel *display = DCC_TO_DC(dcc);
    int stream_id = get_stream_id(display, stream);
    StreamAgent *agent = &dcc->stream_agents[stream_id];

    /* stopping the client from playing older frames at once*/
    region_clear(&agent->clip);
    dcc_stream_agent_clip(dcc, agent);

    if (region_is_empty(&agent->vis_region)) {
        spice_debug("stream %d: vis region empty", stream_id);
        return;
    }

    if (stream->current &&
        region_contains(&stream->current->tree_item.base.rgn, &agent->vis_region)) {
        RedChannel *channel;
        RedChannelClient *rcc;
        UpgradeItem *upgrade_item;
        int n_rects;

        /* (1) The caller should detach the drawable from the stream. This will
         * lead to sending the drawable losslessly, as an ordinary drawable. */
        if (red_display_drawable_is_in_pipe(dcc, stream->current)) {
            spice_debug("stream %d: upgrade by linked drawable. sized %d, box ==>",
                        stream_id, stream->current->sized_stream != NULL);
            rect_debug(&stream->current->red_drawable->bbox);
            goto clear_vis_region;
        }
        spice_debug("stream %d: upgrade by drawable. sized %d, box ==>",
                    stream_id, stream->current->sized_stream != NULL);
        rect_debug(&stream->current->red_drawable->bbox);
        rcc = RED_CHANNEL_CLIENT(dcc);
        channel = rcc->channel;
        upgrade_item = spice_new(UpgradeItem, 1);
        upgrade_item->refs = 1;
        red_channel_pipe_item_init(channel,
                &upgrade_item->base, PIPE_ITEM_TYPE_UPGRADE);
        upgrade_item->drawable = stream->current;
        upgrade_item->drawable->refs++;
        n_rects = pixman_region32_n_rects(&upgrade_item->drawable->tree_item.base.rgn);
        upgrade_item->rects = spice_malloc_n_m(n_rects, sizeof(SpiceRect), sizeof(SpiceClipRects));
        upgrade_item->rects->num_rects = n_rects;
        region_ret_rects(&upgrade_item->drawable->tree_item.base.rgn,
                         upgrade_item->rects->rects, n_rects);
        red_channel_client_pipe_add(rcc, &upgrade_item->base);

    } else {
        SpiceRect upgrade_area;

        region_extents(&agent->vis_region, &upgrade_area);
        spice_debug("stream %d: upgrade by screenshot. has current %d. box ==>",
                    stream_id, stream->current != NULL);
        rect_debug(&upgrade_area);
        if (update_area_limit) {
            red_update_area_till(DCC_TO_DC(dcc), &upgrade_area, 0, update_area_limit);
        } else {
            red_update_area(DCC_TO_DC(dcc), &upgrade_area, 0);
        }
        dcc_add_surface_area_image(dcc, 0, &upgrade_area, NULL, FALSE);
    }
clear_vis_region:
    region_clear(&agent->vis_region);
}

static void detach_stream_gracefully(DisplayChannel *display, Stream *stream,
                                     Drawable *update_area_limit)
{
    RingItem *item, *next;
    DisplayChannelClient *dcc;

    FOREACH_DCC(display, item, next, dcc) {
        dcc_detach_stream_gracefully(dcc, stream, update_area_limit);
    }
    if (stream->current) {
        detach_stream(display, stream, TRUE);
    }
}

/*
 * region  : a primary surface region. Streams that intersects with the given
 *           region will be detached.
 * drawable: If detaching the stream is triggered by the addition of a new drawable
 *           that is dependent on the given region, and the drawable is already a part
 *           of the "current tree", the drawable parameter should be set with
 *           this drawable, otherwise, it should be NULL. Then, if detaching the stream
 *           involves sending an upgrade image to the client, this drawable won't be rendered
 *           (see red_display_detach_stream_gracefully).
 */
void detach_streams_behind(DisplayChannel *display, QRegion *region, Drawable *drawable)
{
    Ring *ring = &display->streams;
    RingItem *item = ring_get_head(ring);
    RingItem *dcc_ring_item, *next;
    DisplayChannelClient *dcc;
    bool is_connected = red_channel_is_connected(RED_CHANNEL(display));

    while (item) {
        Stream *stream = SPICE_CONTAINEROF(item, Stream, link);
        int detach = 0;
        item = ring_next(ring, item);

        FOREACH_DCC(display, dcc_ring_item, next, dcc) {
            StreamAgent *agent = &dcc->stream_agents[get_stream_id(display, stream)];

            if (region_intersects(&agent->vis_region, region)) {
                dcc_detach_stream_gracefully(dcc, stream, drawable);
                detach = 1;
                spice_debug("stream %d", get_stream_id(display, stream));
            }
        }
        if (detach && stream->current) {
            detach_stream(display, stream, TRUE);
        } else if (!is_connected) {
            if (stream->current &&
                region_intersects(&stream->current->tree_item.base.rgn, region)) {
                detach_stream(display, stream, TRUE);
            }
        }
    }
}

static uint64_t red_stream_get_initial_bit_rate(DisplayChannelClient *dcc,
                                                Stream *stream)
{
    char *env_bit_rate_str;
    uint64_t bit_rate = 0;

    env_bit_rate_str = getenv("SPICE_BIT_RATE");
    if (env_bit_rate_str != NULL) {
        double env_bit_rate;

        errno = 0;
        env_bit_rate = strtod(env_bit_rate_str, NULL);
        if (errno == 0) {
            bit_rate = env_bit_rate * 1024 * 1024;
        } else {
            spice_warning("error parsing SPICE_BIT_RATE: %s", strerror(errno));
        }
    }

    if (!bit_rate) {
        MainChannelClient *mcc;
        uint64_t net_test_bit_rate;

        mcc = red_client_get_main(RED_CHANNEL_CLIENT(dcc)->client);
        net_test_bit_rate = main_channel_client_is_network_info_initialized(mcc) ?
                                main_channel_client_get_bitrate_per_sec(mcc) :
                                0;
        bit_rate = MAX(dcc->streams_max_bit_rate, net_test_bit_rate);
        if (bit_rate == 0) {
            /*
             * In case we are after a spice session migration,
             * the low_bandwidth flag is retrieved from migration data.
             * If the network info is not initialized due to another reason,
             * the low_bandwidth flag is FALSE.
             */
            bit_rate = dcc->common.is_low_bandwidth ?
                RED_STREAM_DEFAULT_LOW_START_BIT_RATE :
                RED_STREAM_DEFAULT_HIGH_START_BIT_RATE;
        }
    }

    spice_debug("base-bit-rate %.2f (Mbps)", bit_rate / 1024.0 / 1024.0);
    /* dividing the available bandwidth among the active streams, and saving
     * (1-RED_STREAM_CHANNEL_CAPACITY) of it for other messages */
    return (RED_STREAM_CHANNEL_CAPACITY * bit_rate *
            stream->width * stream->height) / DCC_TO_DC(dcc)->streams_size_total;
}

static uint32_t red_stream_mjpeg_encoder_get_roundtrip(void *opaque)
{
    StreamAgent *agent = opaque;
    int roundtrip;

    spice_assert(agent);
    roundtrip = red_channel_client_get_roundtrip_ms(RED_CHANNEL_CLIENT(agent->dcc));
    if (roundtrip < 0) {
        MainChannelClient *mcc = red_client_get_main(RED_CHANNEL_CLIENT(agent->dcc)->client);

        /*
         * the main channel client roundtrip might not have been
         * calculated (e.g., after migration). In such case,
         * main_channel_client_get_roundtrip_ms returns 0.
         */
        roundtrip = main_channel_client_get_roundtrip_ms(mcc);
    }

    return roundtrip;
}

static uint32_t red_stream_mjpeg_encoder_get_source_fps(void *opaque)
{
    StreamAgent *agent = opaque;

    spice_assert(agent);
    return agent->stream->input_fps;
}

static void red_display_update_streams_max_latency(DisplayChannelClient *dcc, StreamAgent *remove_agent)
{
    uint32_t new_max_latency = 0;
    int i;

    if (dcc->streams_max_latency != remove_agent->client_required_latency) {
        return;
    }

    dcc->streams_max_latency = 0;
    if (DCC_TO_DC(dcc)->stream_count == 1) {
        return;
    }
    for (i = 0; i < NUM_STREAMS; i++) {
        StreamAgent *other_agent = &dcc->stream_agents[i];
        if (other_agent == remove_agent || !other_agent->mjpeg_encoder) {
            continue;
        }
        if (other_agent->client_required_latency > new_max_latency) {
            new_max_latency = other_agent->client_required_latency;
        }
    }
    dcc->streams_max_latency = new_max_latency;
}

static void red_display_stream_agent_stop(DisplayChannelClient *dcc, StreamAgent *agent)
{
    red_display_update_streams_max_latency(dcc, agent);
    if (agent->mjpeg_encoder) {
        mjpeg_encoder_destroy(agent->mjpeg_encoder);
        agent->mjpeg_encoder = NULL;
    }
}

static void red_stream_update_client_playback_latency(void *opaque, uint32_t delay_ms)
{
    StreamAgent *agent = opaque;
    DisplayChannelClient *dcc = agent->dcc;

    red_display_update_streams_max_latency(dcc, agent);

    agent->client_required_latency = delay_ms;
    if (delay_ms > agent->dcc->streams_max_latency) {
         agent->dcc->streams_max_latency = delay_ms;
    }
    spice_debug("reseting client latency: %u", agent->dcc->streams_max_latency);
    main_dispatcher_set_mm_time_latency(RED_CHANNEL_CLIENT(agent->dcc)->client, agent->dcc->streams_max_latency);
}

void dcc_create_stream(DisplayChannelClient *dcc, Stream *stream)
{
    StreamAgent *agent = &dcc->stream_agents[get_stream_id(DCC_TO_DC(dcc), stream)];

    stream->refs++;
    spice_assert(region_is_empty(&agent->vis_region));
    if (stream->current) {
        agent->frames = 1;
        region_clone(&agent->vis_region, &stream->current->tree_item.base.rgn);
        region_clone(&agent->clip, &agent->vis_region);
    } else {
        agent->frames = 0;
    }
    agent->drops = 0;
    agent->fps = MAX_FPS;
    agent->dcc = dcc;

    if (dcc->use_mjpeg_encoder_rate_control) {
        MJpegEncoderRateControlCbs mjpeg_cbs;
        uint64_t initial_bit_rate;

        mjpeg_cbs.get_roundtrip_ms = red_stream_mjpeg_encoder_get_roundtrip;
        mjpeg_cbs.get_source_fps = red_stream_mjpeg_encoder_get_source_fps;
        mjpeg_cbs.update_client_playback_delay = red_stream_update_client_playback_latency;

        initial_bit_rate = red_stream_get_initial_bit_rate(dcc, stream);
        agent->mjpeg_encoder = mjpeg_encoder_new(TRUE, initial_bit_rate, &mjpeg_cbs, agent);
    } else {
        agent->mjpeg_encoder = mjpeg_encoder_new(FALSE, 0, NULL, NULL);
    }
    red_channel_client_pipe_add(RED_CHANNEL_CLIENT(dcc), &agent->create_item);

    if (red_channel_client_test_remote_cap(RED_CHANNEL_CLIENT(dcc), SPICE_DISPLAY_CAP_STREAM_REPORT)) {
        StreamActivateReportItem *report_pipe_item = spice_malloc0(sizeof(*report_pipe_item));

        agent->report_id = rand();
        red_channel_pipe_item_init(RED_CHANNEL_CLIENT(dcc)->channel, &report_pipe_item->pipe_item,
                                   PIPE_ITEM_TYPE_STREAM_ACTIVATE_REPORT);
        report_pipe_item->stream_id = get_stream_id(DCC_TO_DC(dcc), stream);
        red_channel_client_pipe_add(RED_CHANNEL_CLIENT(dcc), &report_pipe_item->pipe_item);
    }
#ifdef STREAM_STATS
    memset(&agent->stats, 0, sizeof(StreamStats));
    if (stream->current) {
        agent->stats.start = stream->current->red_drawable->mm_time;
    }
#endif
}

static void dcc_destroy_stream_agents(DisplayChannelClient *dcc)
{
    int i;

    for (i = 0; i < NUM_STREAMS; i++) {
        StreamAgent *agent = &dcc->stream_agents[i];
        region_destroy(&agent->vis_region);
        region_destroy(&agent->clip);
        if (agent->mjpeg_encoder) {
            mjpeg_encoder_destroy(agent->mjpeg_encoder);
            agent->mjpeg_encoder = NULL;
        }
    }
}

static void red_get_area(DisplayChannel *display, int surface_id, const SpiceRect *area,
                         uint8_t *dest, int dest_stride, int update)
{
    SpiceCanvas *canvas;
    RedSurface *surface;

    surface = &display->surfaces[surface_id];
    if (update) {
        red_update_area(display, area, surface_id);
    }

    canvas = surface->context.canvas;
    canvas->ops->read_bits(canvas, dest, dest_stride, area);
}

static int rgb32_data_has_alpha(int width, int height, size_t stride,
                                uint8_t *data, int *all_set_out)
{
    uint32_t *line, *end, alpha;
    int has_alpha;

    has_alpha = FALSE;
    while (height-- > 0) {
        line = (uint32_t *)data;
        end = line + width;
        data += stride;
        while (line != end) {
            alpha = *line & 0xff000000U;
            if (alpha != 0) {
                has_alpha = TRUE;
                if (alpha != 0xff000000U) {
                    *all_set_out = FALSE;
                    return TRUE;
                }
            }
            line++;
        }
    }

    *all_set_out = has_alpha;
    return has_alpha;
}

static inline int red_handle_self_bitmap(RedWorker *worker, Drawable *drawable)
{
    DisplayChannel *display = worker->display_channel;
    SpiceImage *image;
    int32_t width;
    int32_t height;
    uint8_t *dest;
    int dest_stride;
    RedSurface *surface;
    int bpp;
    int all_set;
    RedDrawable *red_drawable = drawable->red_drawable;

    if (!red_drawable->self_bitmap) {
        return TRUE;
    }

    surface = &display->surfaces[drawable->surface_id];

    bpp = SPICE_SURFACE_FMT_DEPTH(surface->context.format) / 8;

    width = red_drawable->self_bitmap_area.right
            - red_drawable->self_bitmap_area.left;
    height = red_drawable->self_bitmap_area.bottom
            - red_drawable->self_bitmap_area.top;
    dest_stride = SPICE_ALIGN(width * bpp, 4);

    image = spice_new0(SpiceImage, 1);
    image->descriptor.type = SPICE_IMAGE_TYPE_BITMAP;
    image->descriptor.flags = 0;

    QXL_SET_IMAGE_ID(image, QXL_IMAGE_GROUP_RED, ++worker->bits_unique);
    image->u.bitmap.flags = surface->context.top_down ? SPICE_BITMAP_FLAGS_TOP_DOWN : 0;
    image->u.bitmap.format = spice_bitmap_from_surface_type(surface->context.format);
    image->u.bitmap.stride = dest_stride;
    image->descriptor.width = image->u.bitmap.x = width;
    image->descriptor.height = image->u.bitmap.y = height;
    image->u.bitmap.palette = NULL;

    dest = (uint8_t *)spice_malloc_n(height, dest_stride);
    image->u.bitmap.data = spice_chunks_new_linear(dest, height * dest_stride);
    image->u.bitmap.data->flags |= SPICE_CHUNKS_FLAGS_FREE;

    red_get_area(display, drawable->surface_id,
                 &red_drawable->self_bitmap_area, dest, dest_stride, TRUE);

    /* For 32bit non-primary surfaces we need to keep any non-zero
       high bytes as the surface may be used as source to an alpha_blend */
    if (!is_primary_surface(display, drawable->surface_id) &&
        image->u.bitmap.format == SPICE_BITMAP_FMT_32BIT &&
        rgb32_data_has_alpha(width, height, dest_stride, dest, &all_set)) {
        if (all_set) {
            image->descriptor.flags |= SPICE_IMAGE_FLAGS_HIGH_BITS_SET;
        } else {
            image->u.bitmap.format = SPICE_BITMAP_FMT_RGBA;
        }
    }

    red_drawable->self_bitmap_image = image;
    return TRUE;
}

static bool free_one_drawable(DisplayChannel *display, int force_glz_free)
{
    RingItem *ring_item = ring_get_tail(&display->current_list);
    Drawable *drawable;
    Container *container;

    if (!ring_item)
        return FALSE;

    drawable = SPICE_CONTAINEROF(ring_item, Drawable, list_link);
    if (force_glz_free) {
        RingItem *glz_item, *next_item;
        RedGlzDrawable *glz;
        DRAWABLE_FOREACH_GLZ_SAFE(drawable, glz_item, next_item, glz) {
            dcc_free_glz_drawable(glz->dcc, glz);
        }
    }
    red_draw_drawable(display, drawable);
    container = drawable->tree_item.base.container;

    current_remove_drawable(display, drawable);
    container_cleanup(container);
    return TRUE;
}

Drawable *display_channel_drawable_try_new(DisplayChannel *display,
                                           int group_id, int process_commands_generation)
{
    Drawable *drawable;

    while (!(drawable = drawable_try_new(display))) {
        if (!free_one_drawable(display, FALSE))
            return NULL;
    }

    bzero(drawable, sizeof(Drawable));
    drawable->refs = 1;
    drawable->creation_time = red_get_monotonic_time();
    ring_item_init(&drawable->list_link);
    ring_item_init(&drawable->surface_list_link);
    ring_item_init(&drawable->tree_item.base.siblings_link);
    drawable->tree_item.base.type = TREE_ITEM_TYPE_DRAWABLE;
    region_init(&drawable->tree_item.base.rgn);
    ring_init(&drawable->pipes);
    ring_init(&drawable->glz_ring);
    drawable->process_commands_generation = process_commands_generation;
    drawable->group_id = group_id;

    return drawable;
}

static inline int red_handle_depends_on_target_surface(DisplayChannel *display, uint32_t surface_id)
{
    RedSurface *surface;
    RingItem *ring_item;

    surface = &display->surfaces[surface_id];

    while ((ring_item = ring_get_tail(&surface->depend_on_me))) {
        Drawable *drawable;
        DependItem *depended_item = SPICE_CONTAINEROF(ring_item, DependItem, ring_item);
        drawable = depended_item->drawable;
        surface_flush(display, drawable->surface_id, &drawable->red_drawable->bbox);
    }

    return TRUE;
}

static inline void add_to_surface_dependency(DisplayChannel *display, int depend_on_surface_id,
                                             DependItem *depend_item, Drawable *drawable)
{
    RedSurface *surface;

    if (depend_on_surface_id == -1) {
        depend_item->drawable = NULL;
        return;
    }

    surface = &display->surfaces[depend_on_surface_id];

    depend_item->drawable = drawable;
    ring_add(&surface->depend_on_me, &depend_item->ring_item);
}

static inline int red_handle_surfaces_dependencies(DisplayChannel *display, Drawable *drawable)
{
    int x;

    for (x = 0; x < 3; ++x) {
        // surface self dependency is handled by shadows in "current", or by
        // handle_self_bitmap
        if (drawable->surface_deps[x] != drawable->surface_id) {
            add_to_surface_dependency(display, drawable->surface_deps[x],
                                      &drawable->depend_items[x], drawable);

            if (drawable->surface_deps[x] == 0) {
                QRegion depend_region;
                region_init(&depend_region);
                region_add(&depend_region, &drawable->red_drawable->surfaces_rects[x]);
                detach_streams_behind(display, &depend_region, NULL);
            }
        }
    }

    return TRUE;
}

static inline void red_inc_surfaces_drawable_dependencies(DisplayChannel *display, Drawable *drawable)
{
    int x;
    int surface_id;
    RedSurface *surface;

    for (x = 0; x < 3; ++x) {
        surface_id = drawable->surface_deps[x];
        if (surface_id == -1) {
            continue;
        }
        surface = &display->surfaces[surface_id];
        surface->refs++;
    }
}

static RedDrawable *red_drawable_new(void)
{
    RedDrawable * red = spice_new0(RedDrawable, 1);

    red->refs = 1;
    return red;
}

static gboolean red_process_draw(RedWorker *worker, QXLCommandExt *ext_cmd)
{
    DisplayChannel *display = worker->display_channel;
    RedDrawable *red_drawable = NULL;
    Drawable *drawable = NULL;
    int surface_id, x;
    gboolean success = FALSE;

    drawable = display_channel_drawable_try_new(display, ext_cmd->group_id,
                                                worker->process_commands_generation);
    if (!drawable)
        goto end;

    worker->red_drawable_count++;

    red_drawable = red_drawable_new();
    if (red_get_drawable(&worker->mem_slots, ext_cmd->group_id,
                         red_drawable, ext_cmd->cmd.data, ext_cmd->flags) != 0)
        goto end;

    drawable->tree_item.effect = red_drawable->effect;
    drawable->red_drawable = red_drawable_ref(red_drawable);
    drawable->surface_id = red_drawable->surface_id;
    region_add(&drawable->tree_item.base.rgn, &red_drawable->bbox);
    if (red_drawable->clip.type == SPICE_CLIP_TYPE_RECTS) {
        QRegion rgn;

        region_init(&rgn);
        region_add_clip_rects(&rgn, red_drawable->clip.rects);
        region_and(&drawable->tree_item.base.rgn, &rgn);
        region_destroy(&rgn);
    }

    if (!validate_surface(display, drawable->surface_id))
        goto end;
    for (x = 0; x < 3; ++x) {
        drawable->surface_deps[x] = red_drawable->surface_deps[x];
        if (!validate_surface(worker->display_channel, drawable->surface_deps[x]))
            goto end;
    }

    surface_id = drawable->surface_id;
    display->surfaces[surface_id].refs++;

    /*
        surface->refs is affected by a drawable (that is
        dependent on the surface) as long as the drawable is alive.
        However, surface->depend_on_me is affected by a drawable only
        as long as it is in the current tree (hasn't been rendered yet).
    */
    red_inc_surfaces_drawable_dependencies(worker->display_channel, drawable);

    if (region_is_empty(&drawable->tree_item.base.rgn)) {
        goto end;
    }

    if (!red_handle_self_bitmap(worker, drawable)) {
        goto end;
    }

    if (!red_handle_depends_on_target_surface(worker->display_channel, surface_id)) {
        goto end;
    }

    if (!red_handle_surfaces_dependencies(worker->display_channel, drawable)) {
        goto end;
    }

    if (display_channel_add_drawable(worker->display_channel, drawable)) {
        red_pipes_add_drawable(worker->display_channel, drawable);
    }

    success = TRUE;

end:
    if (drawable != NULL)
        display_channel_drawable_unref(display, drawable);
    if (red_drawable != NULL)
        red_drawable_unref(worker, red_drawable, ext_cmd->group_id);
    return success;
}


static inline void red_process_surface(RedWorker *worker, RedSurfaceCmd *surface,
                                       uint32_t group_id, int loadvm)
{
    DisplayChannel *display = worker->display_channel;
    int surface_id;
    RedSurface *red_surface;
    uint8_t *data;

    surface_id = surface->surface_id;
    __validate_surface(display, surface_id);

    red_surface = &display->surfaces[surface_id];

    switch (surface->type) {
    case QXL_SURFACE_CMD_CREATE: {
        uint32_t height = surface->u.surface_create.height;
        int32_t stride = surface->u.surface_create.stride;
        int reloaded_surface = loadvm || (surface->flags & QXL_SURF_FLAG_KEEP_DATA);

        data = surface->u.surface_create.data;
        if (stride < 0) {
            data -= (int32_t)(stride * (height - 1));
        }
        red_create_surface(worker->display_channel, surface_id, surface->u.surface_create.width,
                           height, stride, surface->u.surface_create.format, data,
                           reloaded_surface,
                           // reloaded surfaces will be sent on demand
                           !reloaded_surface);
        display_channel_set_surface_release_info(display, surface_id, 1, surface->release_info, group_id);
        break;
    }
    case QXL_SURFACE_CMD_DESTROY:
        spice_warn_if(!red_surface->context.canvas);
        display_channel_set_surface_release_info(display, surface_id, 0, surface->release_info, group_id);
        red_handle_depends_on_target_surface(display, surface_id);
        /* note that red_handle_depends_on_target_surface must be called before current_remove_all.
           otherwise "current" will hold items that other drawables may depend on, and then
           current_remove_all will remove them from the pipe. */
        current_remove_all(display, surface_id);
        red_clear_surface_drawables_from_pipes(display, surface_id, FALSE);
        display_channel_surface_unref(display, surface_id);
        break;
    default:
            spice_error("unknown surface command");
    };
    red_put_surface_cmd(surface);
    free(surface);
}

static SpiceCanvas *image_surfaces_get(SpiceImageSurfaces *surfaces,
                                       uint32_t surface_id)
{
    DisplayChannel *display;

    display = SPICE_CONTAINEROF(surfaces, DisplayChannel, image_surfaces);
    VALIDATE_SURFACE_RETVAL(display, surface_id, NULL);

    return display->surfaces[surface_id].context.canvas;
}

static void image_surface_init(DisplayChannel *display)
{
    static SpiceImageSurfacesOps image_surfaces_ops = {
        image_surfaces_get,
    };

    display->image_surfaces.ops = &image_surfaces_ops;
}

static void red_draw_qxl_drawable(DisplayChannel *display, Drawable *drawable)
{
    RedSurface *surface;
    SpiceCanvas *canvas;
    SpiceClip clip = drawable->red_drawable->clip;

    surface = &display->surfaces[drawable->surface_id];
    canvas = surface->context.canvas;

    image_cache_aging(&display->image_cache);

    if (!canvas) {
        spice_warning("ignoring drawable to destroyed surface %d\n", drawable->surface_id);
        return;
    }

    region_add(&surface->draw_dirty_region, &drawable->red_drawable->bbox);

    switch (drawable->red_drawable->type) {
    case QXL_DRAW_FILL: {
        SpiceFill fill = drawable->red_drawable->u.fill;
        SpiceImage img1, img2;
        image_cache_localize_brush(&display->image_cache, &fill.brush, &img1);
        image_cache_localize_mask(&display->image_cache, &fill.mask, &img2);
        canvas->ops->draw_fill(canvas, &drawable->red_drawable->bbox,
                               &clip, &fill);
        break;
    }
    case QXL_DRAW_OPAQUE: {
        SpiceOpaque opaque = drawable->red_drawable->u.opaque;
        SpiceImage img1, img2, img3;
        image_cache_localize_brush(&display->image_cache, &opaque.brush, &img1);
        image_cache_localize(&display->image_cache, &opaque.src_bitmap, &img2, drawable);
        image_cache_localize_mask(&display->image_cache, &opaque.mask, &img3);
        canvas->ops->draw_opaque(canvas, &drawable->red_drawable->bbox, &clip, &opaque);
        break;
    }
    case QXL_DRAW_COPY: {
        SpiceCopy copy = drawable->red_drawable->u.copy;
        SpiceImage img1, img2;
        image_cache_localize(&display->image_cache, &copy.src_bitmap, &img1, drawable);
        image_cache_localize_mask(&display->image_cache, &copy.mask, &img2);
        canvas->ops->draw_copy(canvas, &drawable->red_drawable->bbox,
                               &clip, &copy);
        break;
    }
    case QXL_DRAW_TRANSPARENT: {
        SpiceTransparent transparent = drawable->red_drawable->u.transparent;
        SpiceImage img1;
        image_cache_localize(&display->image_cache, &transparent.src_bitmap, &img1, drawable);
        canvas->ops->draw_transparent(canvas,
                                      &drawable->red_drawable->bbox, &clip, &transparent);
        break;
    }
    case QXL_DRAW_ALPHA_BLEND: {
        SpiceAlphaBlend alpha_blend = drawable->red_drawable->u.alpha_blend;
        SpiceImage img1;
        image_cache_localize(&display->image_cache, &alpha_blend.src_bitmap, &img1, drawable);
        canvas->ops->draw_alpha_blend(canvas,
                                      &drawable->red_drawable->bbox, &clip, &alpha_blend);
        break;
    }
    case QXL_COPY_BITS: {
        canvas->ops->copy_bits(canvas, &drawable->red_drawable->bbox,
                               &clip, &drawable->red_drawable->u.copy_bits.src_pos);
        break;
    }
    case QXL_DRAW_BLEND: {
        SpiceBlend blend = drawable->red_drawable->u.blend;
        SpiceImage img1, img2;
        image_cache_localize(&display->image_cache, &blend.src_bitmap, &img1, drawable);
        image_cache_localize_mask(&display->image_cache, &blend.mask, &img2);
        canvas->ops->draw_blend(canvas, &drawable->red_drawable->bbox,
                                &clip, &blend);
        break;
    }
    case QXL_DRAW_BLACKNESS: {
        SpiceBlackness blackness = drawable->red_drawable->u.blackness;
        SpiceImage img1;
        image_cache_localize_mask(&display->image_cache, &blackness.mask, &img1);
        canvas->ops->draw_blackness(canvas,
                                    &drawable->red_drawable->bbox, &clip, &blackness);
        break;
    }
    case QXL_DRAW_WHITENESS: {
        SpiceWhiteness whiteness = drawable->red_drawable->u.whiteness;
        SpiceImage img1;
        image_cache_localize_mask(&display->image_cache, &whiteness.mask, &img1);
        canvas->ops->draw_whiteness(canvas,
                                    &drawable->red_drawable->bbox, &clip, &whiteness);
        break;
    }
    case QXL_DRAW_INVERS: {
        SpiceInvers invers = drawable->red_drawable->u.invers;
        SpiceImage img1;
        image_cache_localize_mask(&display->image_cache, &invers.mask, &img1);
        canvas->ops->draw_invers(canvas,
                                 &drawable->red_drawable->bbox, &clip, &invers);
        break;
    }
    case QXL_DRAW_ROP3: {
        SpiceRop3 rop3 = drawable->red_drawable->u.rop3;
        SpiceImage img1, img2, img3;
        image_cache_localize_brush(&display->image_cache, &rop3.brush, &img1);
        image_cache_localize(&display->image_cache, &rop3.src_bitmap, &img2, drawable);
        image_cache_localize_mask(&display->image_cache, &rop3.mask, &img3);
        canvas->ops->draw_rop3(canvas, &drawable->red_drawable->bbox,
                               &clip, &rop3);
        break;
    }
    case QXL_DRAW_COMPOSITE: {
        SpiceComposite composite = drawable->red_drawable->u.composite;
        SpiceImage src, mask;
        image_cache_localize(&display->image_cache, &composite.src_bitmap, &src, drawable);
        if (composite.mask_bitmap)
            image_cache_localize(&display->image_cache, &composite.mask_bitmap, &mask, drawable);
        canvas->ops->draw_composite(canvas, &drawable->red_drawable->bbox,
                                    &clip, &composite);
        break;
    }
    case QXL_DRAW_STROKE: {
        SpiceStroke stroke = drawable->red_drawable->u.stroke;
        SpiceImage img1;
        image_cache_localize_brush(&display->image_cache, &stroke.brush, &img1);
        canvas->ops->draw_stroke(canvas,
                                 &drawable->red_drawable->bbox, &clip, &stroke);
        break;
    }
    case QXL_DRAW_TEXT: {
        SpiceText text = drawable->red_drawable->u.text;
        SpiceImage img1, img2;
        image_cache_localize_brush(&display->image_cache, &text.fore_brush, &img1);
        image_cache_localize_brush(&display->image_cache, &text.back_brush, &img2);
        canvas->ops->draw_text(canvas, &drawable->red_drawable->bbox,
                               &clip, &text);
        break;
    }
    default:
        spice_warning("invalid type");
    }
}

static void red_draw_drawable(DisplayChannel *display, Drawable *drawable)
{
    red_flush_source_surfaces(display, drawable);
    red_draw_qxl_drawable(display, drawable);
}

static void validate_area(DisplayChannel *display, const SpiceRect *area, uint32_t surface_id)
{
    RedSurface *surface;

    surface = &display->surfaces[surface_id];
    if (!surface->context.canvas_draws_on_surface) {
        SpiceCanvas *canvas = surface->context.canvas;
        int h;
        int stride = surface->context.stride;
        uint8_t *line_0 = surface->context.line_0;

        if (!(h = area->bottom - area->top)) {
            return;
        }

        spice_assert(stride < 0);
        uint8_t *dest = line_0 + (area->top * stride) + area->left * sizeof(uint32_t);
        dest += (h - 1) * stride;
        canvas->ops->read_bits(canvas, dest, -stride, area);
    }
}

/*
    Renders drawables for updating the requested area, but only drawables that are older
    than 'last' (exclusive).
*/
static void red_update_area_till(DisplayChannel *display, const SpiceRect *area, int surface_id,
                                 Drawable *last)
{
    RedSurface *surface;
    Drawable *surface_last = NULL;
    Ring *ring;
    RingItem *ring_item;
    Drawable *now;
    QRegion rgn;

    spice_assert(last);
    spice_assert(ring_item_is_linked(&last->list_link));

    surface = &display->surfaces[surface_id];

    if (surface_id != last->surface_id) {
        // find the nearest older drawable from the appropriate surface
        ring = &display->current_list;
        ring_item = &last->list_link;
        while ((ring_item = ring_next(ring, ring_item))) {
            now = SPICE_CONTAINEROF(ring_item, Drawable, list_link);
            if (now->surface_id == surface_id) {
                surface_last = now;
                break;
            }
        }
    } else {
        ring_item = ring_next(&surface->current_list, &last->surface_list_link);
        if (ring_item) {
            surface_last = SPICE_CONTAINEROF(ring_item, Drawable, surface_list_link);
        }
    }

    if (!surface_last) {
        return;
    }

    ring = &surface->current_list;
    ring_item = &surface_last->surface_list_link;

    region_init(&rgn);
    region_add(&rgn, area);

    // find the first older drawable that intersects with the area
    do {
        now = SPICE_CONTAINEROF(ring_item, Drawable, surface_list_link);
        if (region_intersects(&rgn, &now->tree_item.base.rgn)) {
            surface_last = now;
            break;
        }
    } while ((ring_item = ring_next(ring, ring_item)));

    region_destroy(&rgn);

    if (!surface_last) {
        return;
    }

    do {
        Container *container;

        ring_item = ring_get_tail(&surface->current_list);
        now = SPICE_CONTAINEROF(ring_item, Drawable, surface_list_link);
        now->refs++;
        container = now->tree_item.base.container;
        current_remove_drawable(display, now);
        container_cleanup(container);
        /* red_draw_drawable may call red_update_area for the surfaces 'now' depends on. Notice,
           that it is valid to call red_update_area in this case and not red_update_area_till:
           It is impossible that there was newer item then 'last' in one of the surfaces
           that red_update_area is called for, Otherwise, 'now' would have already been rendered.
           See the call for red_handle_depends_on_target_surface in red_process_draw */
        red_draw_drawable(display, now);
        display_channel_drawable_unref(display, now);
    } while (now != surface_last);
    validate_area(display, area, surface_id);
}

static void red_update_area(DisplayChannel *display, const SpiceRect *area, int surface_id)
{
    RedSurface *surface;
    Ring *ring;
    RingItem *ring_item;
    QRegion rgn;
    Drawable *last;
    Drawable *now;
    spice_debug("surface %d: area ==>", surface_id);
    rect_debug(area);

    spice_return_if_fail(surface_id >= 0 && surface_id < NUM_SURFACES);
    spice_return_if_fail(area);
    spice_return_if_fail(area->left >= 0 && area->top >= 0 &&
                         area->left < area->right && area->top < area->bottom);

    surface = &display->surfaces[surface_id];

    last = NULL;
    ring = &surface->current_list;
    ring_item = ring;

    region_init(&rgn);
    region_add(&rgn, area);
    while ((ring_item = ring_next(ring, ring_item))) {
        now = SPICE_CONTAINEROF(ring_item, Drawable, surface_list_link);
        if (region_intersects(&rgn, &now->tree_item.base.rgn)) {
            last = now;
            break;
        }
    }
    region_destroy(&rgn);

    if (!last) {
        validate_area(display, area, surface_id);
        return;
    }

    do {
        Container *container;

        ring_item = ring_get_tail(&surface->current_list);
        now = SPICE_CONTAINEROF(ring_item, Drawable, surface_list_link);
        now->refs++;
        container = now->tree_item.base.container;
        current_remove_drawable(display, now);
        container_cleanup(container);
        red_draw_drawable(display, now);
        display_channel_drawable_unref(display, now);
    } while (now != last);
    validate_area(display, area, surface_id);
}

static int red_process_cursor(RedWorker *worker, uint32_t max_pipe_size, int *ring_is_empty)
{
    QXLCommandExt ext_cmd;
    int n = 0;

    if (!worker->running) {
        *ring_is_empty = TRUE;
        return n;
    }

    *ring_is_empty = FALSE;
    while (!cursor_is_connected(worker) ||
           red_channel_min_pipe_size(RED_CHANNEL(worker->cursor_channel)) <= max_pipe_size) {
        if (!worker->qxl->st->qif->get_cursor_command(worker->qxl, &ext_cmd)) {
            *ring_is_empty = TRUE;
            if (worker->cursor_poll_tries < CMD_RING_POLL_RETRIES) {
                worker->cursor_poll_tries++;
                worker->timeout = worker->timeout == -1 ?
                    CMD_RING_POLL_TIMEOUT :
                    MIN(worker->timeout, CMD_RING_POLL_TIMEOUT);
                break;
            }
            if (worker->cursor_poll_tries > CMD_RING_POLL_RETRIES ||
                worker->qxl->st->qif->req_cursor_notification(worker->qxl)) {
                worker->cursor_poll_tries++;
                break;
            }
            continue;
        }
        worker->cursor_poll_tries = 0;
        switch (ext_cmd.cmd.type) {
        case QXL_CMD_CURSOR: {
            RedCursorCmd *cursor = spice_new0(RedCursorCmd, 1);

            if (red_get_cursor_cmd(&worker->mem_slots, ext_cmd.group_id,
                                    cursor, ext_cmd.cmd.data)) {
                free(cursor);
                break;
            }
            cursor_channel_process_cmd(worker->cursor_channel, cursor, ext_cmd.group_id);
            break;
        }
        default:
            spice_warning("bad command type");
        }
        n++;
    }
    return n;
}

static void red_record_event(RedWorker *worker, int what, uint32_t type)
{
    struct timespec ts;
    static int counter = 0;

    // TODO: record the size of the packet in the header. This would make
    // navigating it much faster (well, I can add an index while I'm at it..)
    // and make it trivial to get a histogram from a file.
    // But to implement that I would need some temporary buffer for each event.
    // (that can be up to VGA_FRAMEBUFFER large)
    clock_gettime(worker->clockid, &ts);
    fprintf(worker->record_fd, "event %d %d %d %ld\n", counter++, what, type,
        ts.tv_nsec + ts.tv_sec * 1000 * 1000 * 1000);
}

static void red_record_command(RedWorker *worker, QXLCommandExt ext_cmd)
{
    red_record_event(worker, 0, ext_cmd.cmd.type);
    switch (ext_cmd.cmd.type) {
    case QXL_CMD_DRAW:
        red_record_drawable(worker->record_fd, &worker->mem_slots, ext_cmd.group_id,
                            ext_cmd.cmd.data, ext_cmd.flags);
        break;
    case QXL_CMD_UPDATE:
        red_record_update_cmd(worker->record_fd, &worker->mem_slots, ext_cmd.group_id,
                           ext_cmd.cmd.data);
        break;
    case QXL_CMD_MESSAGE:
        red_record_message(worker->record_fd, &worker->mem_slots, ext_cmd.group_id,
                           ext_cmd.cmd.data);
        break;
    case QXL_CMD_SURFACE:
        red_record_surface_cmd(worker->record_fd, &worker->mem_slots, ext_cmd.group_id,
                               ext_cmd.cmd.data);
        break;
    }
}

static int red_process_commands(RedWorker *worker, uint32_t max_pipe_size, int *ring_is_empty)
{
    QXLCommandExt ext_cmd;
    int n = 0;

    if (!worker->running) {
        *ring_is_empty = TRUE;
        return n;
    }

    worker->process_commands_generation++;
    *ring_is_empty = FALSE;
    for (;;) {

        if (display_is_connected(worker) && worker->wait_for_clients) {

            if (red_channel_all_blocked(RED_CHANNEL(worker->display_channel))) {
                spice_info("all display clients are blocking");
                return n;
            }


            // TODO: change to average pipe size?
            if (red_channel_min_pipe_size(RED_CHANNEL(worker->display_channel)) > max_pipe_size) {
                spice_info("too much item in the display clients pipe already");
                return n;
            }
        }

        if (!worker->qxl->st->qif->get_command(worker->qxl, &ext_cmd)) {
            *ring_is_empty = TRUE;;
            if (worker->display_poll_tries < CMD_RING_POLL_RETRIES) {
                worker->display_poll_tries++;
                worker->timeout = worker->timeout == -1 ?
                    CMD_RING_POLL_TIMEOUT :
                    MIN(worker->timeout, CMD_RING_POLL_TIMEOUT);
                break;
            }
            if (worker->display_poll_tries > CMD_RING_POLL_RETRIES ||
                         worker->qxl->st->qif->req_cmd_notification(worker->qxl)) {
                worker->display_poll_tries++;
                break;
            }
            continue;
        }
        if (worker->record_fd) {
            red_record_command(worker, ext_cmd);
        }
        stat_inc_counter(worker->command_counter, 1);
        worker->display_poll_tries = 0;
        switch (ext_cmd.cmd.type) {
        case QXL_CMD_DRAW: {
            if (!red_process_draw(worker, &ext_cmd))
                break;
            break;
        }
        case QXL_CMD_UPDATE: {
            RedUpdateCmd update;
            QXLReleaseInfoExt release_info_ext;

            if (red_get_update_cmd(&worker->mem_slots, ext_cmd.group_id,
                                   &update, ext_cmd.cmd.data)) {
                break;
            }
            if (!validate_surface(worker->display_channel, update.surface_id)) {
                rendering_incorrect("QXL_CMD_UPDATE");
                break;
            }
            red_update_area(worker->display_channel, &update.area, update.surface_id);
            worker->qxl->st->qif->notify_update(worker->qxl, update.update_id);
            release_info_ext.group_id = ext_cmd.group_id;
            release_info_ext.info = update.release_info;
            worker->qxl->st->qif->release_resource(worker->qxl, release_info_ext);
            red_put_update_cmd(&update);
            break;
        }
        case QXL_CMD_MESSAGE: {
            RedMessage message;
            QXLReleaseInfoExt release_info_ext;

            if (red_get_message(&worker->mem_slots, ext_cmd.group_id,
                                &message, ext_cmd.cmd.data)) {
                break;
            }
#ifdef DEBUG
            /* alert: accessing message.data is insecure */
            spice_warning("MESSAGE: %s", message.data);
#endif
            release_info_ext.group_id = ext_cmd.group_id;
            release_info_ext.info = message.release_info;
            worker->qxl->st->qif->release_resource(worker->qxl, release_info_ext);
            red_put_message(&message);
            break;
        }
        case QXL_CMD_SURFACE: {
            RedSurfaceCmd *surface = spice_new0(RedSurfaceCmd, 1);

            if (red_get_surface_cmd(&worker->mem_slots, ext_cmd.group_id,
                                    surface, ext_cmd.cmd.data)) {
                free(surface);
                break;
            }
            red_process_surface(worker, surface, ext_cmd.group_id, FALSE);
            break;
        }
        default:
            spice_error("bad command type");
        }
        n++;
    }

    return n;
}

#define RED_RELEASE_BUNCH_SIZE 64

static void red_free_some(RedWorker *worker)
{
    DisplayChannel *display = worker->display_channel;
    int n = 0;
    DisplayChannelClient *dcc;
    RingItem *item, *next;

#if FIXME
    spice_debug("#draw=%d, #red_draw=%d, #glz_draw=%d", display->drawable_count,
                worker->red_drawable_count, worker->glz_drawable_count);
#endif
    FOREACH_DCC(worker->display_channel, item, next, dcc) {
        GlzSharedDictionary *glz_dict = dcc ? dcc->glz_dict : NULL;

        if (glz_dict) {
            // encoding using the dictionary is prevented since the following operations might
            // change the dictionary
            pthread_rwlock_wrlock(&glz_dict->encode_lock);
            n = red_display_free_some_independent_glz_drawables(dcc);
        }
    }

    while (!ring_is_empty(&display->current_list) && n++ < RED_RELEASE_BUNCH_SIZE) {
        free_one_drawable(display, TRUE);
    }

    FOREACH_DCC(worker->display_channel, item, next, dcc) {
        GlzSharedDictionary *glz_dict = dcc ? dcc->glz_dict : NULL;

        if (glz_dict) {
            pthread_rwlock_unlock(&glz_dict->encode_lock);
        }
    }
}

void display_channel_current_flush(DisplayChannel *display, int surface_id)
{
    while (!ring_is_empty(&display->surfaces[surface_id].current_list)) {
        free_one_drawable(display, FALSE);
    }
    current_remove_all(display, surface_id);
}

// adding the pipe item after pos. If pos == NULL, adding to head.
ImageItem *dcc_add_surface_area_image(DisplayChannelClient *dcc, int surface_id,
                                      SpiceRect *area, PipeItem *pos, int can_lossy)
{
    DisplayChannel *display = DCC_TO_DC(dcc);
    RedChannel *channel = RED_CHANNEL(display);
    RedSurface *surface = &display->surfaces[surface_id];
    SpiceCanvas *canvas = surface->context.canvas;
    ImageItem *item;
    int stride;
    int width;
    int height;
    int bpp;
    int all_set;

    spice_assert(area);

    width = area->right - area->left;
    height = area->bottom - area->top;
    bpp = SPICE_SURFACE_FMT_DEPTH(surface->context.format) / 8;
    stride = width * bpp;

    item = (ImageItem *)spice_malloc_n_m(height, stride, sizeof(ImageItem));

    red_channel_pipe_item_init(channel, &item->link, PIPE_ITEM_TYPE_IMAGE);

    item->refs = 1;
    item->surface_id = surface_id;
    item->image_format =
        spice_bitmap_from_surface_type(surface->context.format);
    item->image_flags = 0;
    item->pos.x = area->left;
    item->pos.y = area->top;
    item->width = width;
    item->height = height;
    item->stride = stride;
    item->top_down = surface->context.top_down;
    item->can_lossy = can_lossy;

    canvas->ops->read_bits(canvas, item->data, stride, area);

    /* For 32bit non-primary surfaces we need to keep any non-zero
       high bytes as the surface may be used as source to an alpha_blend */
    if (!is_primary_surface(display, surface_id) &&
        item->image_format == SPICE_BITMAP_FMT_32BIT &&
        rgb32_data_has_alpha(item->width, item->height, item->stride, item->data, &all_set)) {
        if (all_set) {
            item->image_flags |= SPICE_IMAGE_FLAGS_HIGH_BITS_SET;
        } else {
            item->image_format = SPICE_BITMAP_FMT_RGBA;
        }
    }

    if (!pos) {
        red_pipe_add_image_item(dcc, item);
    } else {
        red_pipe_add_image_item_after(dcc, item, pos);
    }

    release_image_item(item);

    return item;
}

static inline void fill_rects_clip(SpiceMarshaller *m, SpiceClipRects *data)
{
    int i;

    spice_marshaller_add_uint32(m, data->num_rects);
    for (i = 0; i < data->num_rects; i++) {
        spice_marshall_Rect(m, data->rects + i);
    }
}

static void fill_base(SpiceMarshaller *base_marshaller, Drawable *drawable)
{
    SpiceMsgDisplayBase base;

    base.surface_id = drawable->surface_id;
    base.box = drawable->red_drawable->bbox;
    base.clip = drawable->red_drawable->clip;

    spice_marshall_DisplayBase(base_marshaller, &base);
}

/*
 * Remove from the global lz dictionary some glz_drawables that have no reference to
 * Drawable (their qxl drawables are released too).
 * NOTE - the caller should prevent encoding using the dictionary during the operation
 */
static int red_display_free_some_independent_glz_drawables(DisplayChannelClient *dcc)
{
    RingItem *ring_link;
    int n = 0;

    if (!dcc) {
        return 0;
    }
    ring_link = ring_get_head(&dcc->glz_drawables);
    while ((n < RED_RELEASE_BUNCH_SIZE) && (ring_link != NULL)) {
        RedGlzDrawable *glz_drawable = SPICE_CONTAINEROF(ring_link, RedGlzDrawable, link);
        ring_link = ring_next(&dcc->glz_drawables, ring_link);
        if (!glz_drawable->drawable) {
            dcc_free_glz_drawable(dcc, glz_drawable);
            n++;
        }
    }
    return n;
}

static inline void red_display_add_image_to_pixmap_cache(RedChannelClient *rcc,
                                                         SpiceImage *image, SpiceImage *io_image,
                                                         int is_lossy)
{
    DisplayChannel *display_channel = SPICE_CONTAINEROF(rcc->channel, DisplayChannel, common.base);
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);

    if ((image->descriptor.flags & SPICE_IMAGE_FLAGS_CACHE_ME)) {
        spice_assert(image->descriptor.width * image->descriptor.height > 0);
        if (!(io_image->descriptor.flags & SPICE_IMAGE_FLAGS_CACHE_REPLACE_ME)) {
            if (dcc_pixmap_cache_add(dcc, image->descriptor.id,
                                     image->descriptor.width * image->descriptor.height,
                                     is_lossy)) {
                io_image->descriptor.flags |= SPICE_IMAGE_FLAGS_CACHE_ME;
                dcc->send_data.pixmap_cache_items[dcc->send_data.num_pixmap_cache_items++] =
                                                                               image->descriptor.id;
                stat_inc_counter(display_channel->add_to_cache_counter, 1);
            }
        }
    }

    if (!(io_image->descriptor.flags & SPICE_IMAGE_FLAGS_CACHE_ME)) {
        stat_inc_counter(display_channel->non_cache_counter, 1);
    }
}

static int dcc_pixmap_cache_hit(DisplayChannelClient *dcc, uint64_t id, int *lossy)
{
    PixmapCache *cache = dcc->pixmap_cache;
    NewCacheItem *item;
    uint64_t serial;

    serial = red_channel_client_get_message_serial(RED_CHANNEL_CLIENT(dcc));
    pthread_mutex_lock(&cache->lock);
    item = cache->hash_table[BITS_CACHE_HASH_KEY(id)];

    while (item) {
        if (item->id == id) {
            ring_remove(&item->lru_link);
            ring_add(&cache->lru, &item->lru_link);
            spice_assert(dcc->common.id < MAX_CACHE_CLIENTS);
            item->sync[dcc->common.id] = serial;
            cache->sync[dcc->common.id] = serial;
            *lossy = item->lossy;
            break;
        }
        item = item->next;
    }
    pthread_mutex_unlock(&cache->lock);

    return !!item;
}


typedef enum {
    FILL_BITS_TYPE_INVALID,
    FILL_BITS_TYPE_CACHE,
    FILL_BITS_TYPE_SURFACE,
    FILL_BITS_TYPE_COMPRESS_LOSSLESS,
    FILL_BITS_TYPE_COMPRESS_LOSSY,
    FILL_BITS_TYPE_BITMAP,
} FillBitsType;

/* if the number of times fill_bits can be called per one qxl_drawable increases -
   MAX_LZ_DRAWABLE_INSTANCES must be increased as well */
static FillBitsType fill_bits(DisplayChannelClient *dcc, SpiceMarshaller *m,
                              SpiceImage *simage, Drawable *drawable, int can_lossy)
{
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(dcc);
    DisplayChannel *display = DCC_TO_DC(dcc);
    SpiceImage image;
    compress_send_data_t comp_send_data = {0};
    SpiceMarshaller *bitmap_palette_out, *lzplt_palette_out;

    if (simage == NULL) {
        spice_assert(drawable->red_drawable->self_bitmap_image);
        simage = drawable->red_drawable->self_bitmap_image;
    }

    image.descriptor = simage->descriptor;
    image.descriptor.flags = 0;
    if (simage->descriptor.flags & SPICE_IMAGE_FLAGS_HIGH_BITS_SET) {
        image.descriptor.flags = SPICE_IMAGE_FLAGS_HIGH_BITS_SET;
    }

    if ((simage->descriptor.flags & SPICE_IMAGE_FLAGS_CACHE_ME)) {
        int lossy_cache_item;
        if (dcc_pixmap_cache_hit(dcc, image.descriptor.id, &lossy_cache_item)) {
            dcc->send_data.pixmap_cache_items[dcc->send_data.num_pixmap_cache_items++] =
                                                                               image.descriptor.id;
            if (can_lossy || !lossy_cache_item) {
                if (!display->enable_jpeg || lossy_cache_item) {
                    image.descriptor.type = SPICE_IMAGE_TYPE_FROM_CACHE;
                } else {
                    // making sure, in multiple monitor scenario, that lossy items that
                    // should have been replaced with lossless data by one display channel,
                    // will be retrieved as lossless by another display channel.
                    image.descriptor.type = SPICE_IMAGE_TYPE_FROM_CACHE_LOSSLESS;
                }
                spice_marshall_Image(m, &image,
                                     &bitmap_palette_out, &lzplt_palette_out);
                spice_assert(bitmap_palette_out == NULL);
                spice_assert(lzplt_palette_out == NULL);
                stat_inc_counter(display->cache_hits_counter, 1);
                return FILL_BITS_TYPE_CACHE;
            } else {
                pixmap_cache_set_lossy(dcc->pixmap_cache, simage->descriptor.id,
                                       FALSE);
                image.descriptor.flags |= SPICE_IMAGE_FLAGS_CACHE_REPLACE_ME;
            }
        }
    }

    switch (simage->descriptor.type) {
    case SPICE_IMAGE_TYPE_SURFACE: {
        int surface_id;
        RedSurface *surface;

        surface_id = simage->u.surface.surface_id;
        if (!validate_surface(display, surface_id)) {
            rendering_incorrect("SPICE_IMAGE_TYPE_SURFACE");
            return FILL_BITS_TYPE_SURFACE;
        }

        surface = &display->surfaces[surface_id];
        image.descriptor.type = SPICE_IMAGE_TYPE_SURFACE;
        image.descriptor.flags = 0;
        image.descriptor.width = surface->context.width;
        image.descriptor.height = surface->context.height;

        image.u.surface.surface_id = surface_id;
        spice_marshall_Image(m, &image,
                             &bitmap_palette_out, &lzplt_palette_out);
        spice_assert(bitmap_palette_out == NULL);
        spice_assert(lzplt_palette_out == NULL);
        return FILL_BITS_TYPE_SURFACE;
    }
    case SPICE_IMAGE_TYPE_BITMAP: {
        SpiceBitmap *bitmap = &image.u.bitmap;
#ifdef DUMP_BITMAP
        dump_bitmap(&simage->u.bitmap);
#endif
        /* Images must be added to the cache only after they are compressed
           in order to prevent starvation in the client between pixmap_cache and
           global dictionary (in cases of multiple monitors) */
        if (!dcc_compress_image(dcc, &image, &simage->u.bitmap,
                                drawable, can_lossy, &comp_send_data)) {
            SpicePalette *palette;

            red_display_add_image_to_pixmap_cache(rcc, simage, &image, FALSE);

            *bitmap = simage->u.bitmap;
            bitmap->flags = bitmap->flags & SPICE_BITMAP_FLAGS_TOP_DOWN;

            palette = bitmap->palette;
            dcc_palette_cache_palette(dcc, palette, &bitmap->flags);
            spice_marshall_Image(m, &image,
                                 &bitmap_palette_out, &lzplt_palette_out);
            spice_assert(lzplt_palette_out == NULL);

            if (bitmap_palette_out && palette) {
                spice_marshall_Palette(bitmap_palette_out, palette);
            }

            spice_marshaller_add_ref_chunks(m, bitmap->data);
            return FILL_BITS_TYPE_BITMAP;
        } else {
            red_display_add_image_to_pixmap_cache(rcc, simage, &image,
                                                  comp_send_data.is_lossy);

            spice_marshall_Image(m, &image,
                                 &bitmap_palette_out, &lzplt_palette_out);
            spice_assert(bitmap_palette_out == NULL);

            marshaller_add_compressed(m, comp_send_data.comp_buf,
                                      comp_send_data.comp_buf_size);

            if (lzplt_palette_out && comp_send_data.lzplt_palette) {
                spice_marshall_Palette(lzplt_palette_out, comp_send_data.lzplt_palette);
            }

            spice_assert(!comp_send_data.is_lossy || can_lossy);
            return (comp_send_data.is_lossy ? FILL_BITS_TYPE_COMPRESS_LOSSY :
                                              FILL_BITS_TYPE_COMPRESS_LOSSLESS);
        }
        break;
    }
    case SPICE_IMAGE_TYPE_QUIC:
        red_display_add_image_to_pixmap_cache(rcc, simage, &image, FALSE);
        image.u.quic = simage->u.quic;
        spice_marshall_Image(m, &image,
                             &bitmap_palette_out, &lzplt_palette_out);
        spice_assert(bitmap_palette_out == NULL);
        spice_assert(lzplt_palette_out == NULL);
        spice_marshaller_add_ref_chunks(m, image.u.quic.data);
        return FILL_BITS_TYPE_COMPRESS_LOSSLESS;
    default:
        spice_error("invalid image type %u", image.descriptor.type);
    }

    return 0;
}

static void fill_mask(RedChannelClient *rcc, SpiceMarshaller *m,
                      SpiceImage *mask_bitmap, Drawable *drawable)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);

    if (mask_bitmap && m) {
        if (dcc->image_compression != SPICE_IMAGE_COMPRESS_OFF) {
            /* todo: pass compression argument */
            spice_image_compression_t save_img_comp = dcc->image_compression;
            dcc->image_compression = SPICE_IMAGE_COMPRESS_OFF;
            fill_bits(dcc, m, mask_bitmap, drawable, FALSE);
            dcc->image_compression = save_img_comp;
        } else {
            fill_bits(dcc, m, mask_bitmap, drawable, FALSE);
        }
    }
}

static void fill_attr(SpiceMarshaller *m, SpiceLineAttr *attr, uint32_t group_id)
{
    int i;

    if (m && attr->style_nseg) {
        for (i = 0 ; i < attr->style_nseg; i++) {
            spice_marshaller_add_uint32(m, attr->style[i]);
        }
    }
}

static inline void red_display_reset_send_data(DisplayChannelClient *dcc)
{
    dcc->send_data.free_list.res->count = 0;
    dcc->send_data.num_pixmap_cache_items = 0;
    memset(dcc->send_data.free_list.sync, 0, sizeof(dcc->send_data.free_list.sync));
}

/* set area=NULL for testing the whole surface */
static int is_surface_area_lossy(DisplayChannelClient *dcc, uint32_t surface_id,
                                 const SpiceRect *area, SpiceRect *out_lossy_area)
{
    RedSurface *surface;
    QRegion *surface_lossy_region;
    QRegion lossy_region;
    DisplayChannel *display = DCC_TO_DC(dcc);

    VALIDATE_SURFACE_RETVAL(display, surface_id, FALSE);
    surface = &display->surfaces[surface_id];
    surface_lossy_region = &dcc->surface_client_lossy_region[surface_id];

    if (!area) {
        if (region_is_empty(surface_lossy_region)) {
            return FALSE;
        } else {
            out_lossy_area->top = 0;
            out_lossy_area->left = 0;
            out_lossy_area->bottom = surface->context.height;
            out_lossy_area->right = surface->context.width;
            return TRUE;
        }
    }

    region_init(&lossy_region);
    region_add(&lossy_region, area);
    region_and(&lossy_region, surface_lossy_region);
    if (!region_is_empty(&lossy_region)) {
        out_lossy_area->left = lossy_region.extents.x1;
        out_lossy_area->top = lossy_region.extents.y1;
        out_lossy_area->right = lossy_region.extents.x2;
        out_lossy_area->bottom = lossy_region.extents.y2;
        region_destroy(&lossy_region);
        return TRUE;
    } else {
        return FALSE;
    }
}
/* returns if the bitmap was already sent lossy to the client. If the bitmap hasn't been sent yet
   to the client, returns false. "area" is for surfaces. If area = NULL,
   all the surface is considered. out_lossy_data will hold info about the bitmap, and its lossy
   area in case it is lossy and part of a surface. */
static int is_bitmap_lossy(RedChannelClient *rcc, SpiceImage *image, SpiceRect *area,
                           Drawable *drawable, BitmapData *out_data)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);

    if (image == NULL) {
        // self bitmap
        out_data->type = BITMAP_DATA_TYPE_BITMAP;
        return FALSE;
    }

    if ((image->descriptor.flags & SPICE_IMAGE_FLAGS_CACHE_ME)) {
        int is_hit_lossy;

        out_data->id = image->descriptor.id;
        if (dcc_pixmap_cache_hit(dcc, image->descriptor.id, &is_hit_lossy)) {
            out_data->type = BITMAP_DATA_TYPE_CACHE;
            if (is_hit_lossy) {
                return TRUE;
            } else {
                return FALSE;
            }
        } else {
            out_data->type = BITMAP_DATA_TYPE_BITMAP_TO_CACHE;
        }
    } else {
         out_data->type = BITMAP_DATA_TYPE_BITMAP;
    }

    if (image->descriptor.type != SPICE_IMAGE_TYPE_SURFACE) {
        return FALSE;
    }

    out_data->type = BITMAP_DATA_TYPE_SURFACE;
    out_data->id = image->u.surface.surface_id;

    if (is_surface_area_lossy(dcc, out_data->id,
                              area, &out_data->lossy_rect))
    {
        return TRUE;
    } else {
        return FALSE;
    }
}

static int is_brush_lossy(RedChannelClient *rcc, SpiceBrush *brush,
                          Drawable *drawable, BitmapData *out_data)
{
    if (brush->type == SPICE_BRUSH_TYPE_PATTERN) {
        return is_bitmap_lossy(rcc, brush->u.pattern.pat, NULL,
                               drawable, out_data);
    } else {
        out_data->type = BITMAP_DATA_TYPE_INVALID;
        return FALSE;
    }
}

static void surface_lossy_region_update(DisplayChannelClient *dcc,
                                        Drawable *item, int has_mask, int lossy)
{
    QRegion *surface_lossy_region;
    RedDrawable *drawable;

    if (has_mask && !lossy) {
        return;
    }

    surface_lossy_region = &dcc->surface_client_lossy_region[item->surface_id];
    drawable = item->red_drawable;

    if (drawable->clip.type == SPICE_CLIP_TYPE_RECTS ) {
        QRegion clip_rgn;
        QRegion draw_region;
        region_init(&clip_rgn);
        region_init(&draw_region);
        region_add(&draw_region, &drawable->bbox);
        region_add_clip_rects(&clip_rgn, drawable->clip.rects);
        region_and(&draw_region, &clip_rgn);
        if (lossy) {
            region_or(surface_lossy_region, &draw_region);
        } else {
            region_exclude(surface_lossy_region, &draw_region);
        }

        region_destroy(&clip_rgn);
        region_destroy(&draw_region);
    } else { /* no clip */
        if (!lossy) {
            region_remove(surface_lossy_region, &drawable->bbox);
        } else {
            region_add(surface_lossy_region, &drawable->bbox);
        }
    }
}

static inline int drawable_intersects_with_areas(Drawable *drawable, int surface_ids[],
                                                 SpiceRect *surface_areas[],
                                                 int num_surfaces)
{
    int i;
    for (i = 0; i < num_surfaces; i++) {
        if (surface_ids[i] == drawable->red_drawable->surface_id) {
            if (rect_intersects(surface_areas[i], &drawable->red_drawable->bbox)) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

static inline int drawable_depends_on_areas(Drawable *drawable,
                                            int surface_ids[],
                                            SpiceRect surface_areas[],
                                            int num_surfaces)
{
    int i;
    RedDrawable *red_drawable;
    int drawable_has_shadow;
    SpiceRect shadow_rect = {0, 0, 0, 0};

    red_drawable = drawable->red_drawable;
    drawable_has_shadow = has_shadow(red_drawable);

    if (drawable_has_shadow) {
       int delta_x = red_drawable->u.copy_bits.src_pos.x - red_drawable->bbox.left;
       int delta_y = red_drawable->u.copy_bits.src_pos.y - red_drawable->bbox.top;

       shadow_rect.left = red_drawable->u.copy_bits.src_pos.x;
       shadow_rect.top = red_drawable->u.copy_bits.src_pos.y;
       shadow_rect.right = red_drawable->bbox.right + delta_x;
       shadow_rect.bottom = red_drawable->bbox.bottom + delta_y;
    }

    for (i = 0; i < num_surfaces; i++) {
        int x;
        int dep_surface_id;

         for (x = 0; x < 3; ++x) {
            dep_surface_id = drawable->surface_deps[x];
            if (dep_surface_id == surface_ids[i]) {
                if (rect_intersects(&surface_areas[i], &red_drawable->surfaces_rects[x])) {
                    return TRUE;
                }
            }
        }

        if (surface_ids[i] == red_drawable->surface_id) {
            if (drawable_has_shadow) {
                if (rect_intersects(&surface_areas[i], &shadow_rect)) {
                    return TRUE;
                }
            }

            // not dependent on dest
            if (red_drawable->effect == QXL_EFFECT_OPAQUE) {
                continue;
            }

            if (rect_intersects(&surface_areas[i], &red_drawable->bbox)) {
                return TRUE;
            }
        }

    }
    return FALSE;
}


static int pipe_rendered_drawables_intersect_with_areas(DisplayChannelClient *dcc,
                                                        int surface_ids[],
                                                        SpiceRect *surface_areas[],
                                                        int num_surfaces)
{
    PipeItem *pipe_item;
    Ring *pipe;

    spice_assert(num_surfaces);
    pipe = &RED_CHANNEL_CLIENT(dcc)->pipe;

    for (pipe_item = (PipeItem *)ring_get_head(pipe);
         pipe_item;
         pipe_item = (PipeItem *)ring_next(pipe, &pipe_item->link))
    {
        Drawable *drawable;

        if (pipe_item->type != PIPE_ITEM_TYPE_DRAW)
            continue;
        drawable = SPICE_CONTAINEROF(pipe_item, DrawablePipeItem, dpi_pipe_item)->drawable;

        if (ring_item_is_linked(&drawable->list_link))
            continue; // item hasn't been rendered

        if (drawable_intersects_with_areas(drawable, surface_ids, surface_areas, num_surfaces)) {
            return TRUE;
        }
    }

    return FALSE;
}

static void red_pipe_replace_rendered_drawables_with_images(DisplayChannelClient *dcc,
                                                            int first_surface_id,
                                                            SpiceRect *first_area)
{
    /* TODO: can't have those statics with multiple clients */
    static int resent_surface_ids[MAX_PIPE_SIZE];
    static SpiceRect resent_areas[MAX_PIPE_SIZE]; // not pointers since drawbales may be released
    int num_resent;
    PipeItem *pipe_item;
    Ring *pipe;

    resent_surface_ids[0] = first_surface_id;
    resent_areas[0] = *first_area;
    num_resent = 1;

    pipe = &RED_CHANNEL_CLIENT(dcc)->pipe;

    // going from the oldest to the newest
    for (pipe_item = (PipeItem *)ring_get_tail(pipe);
         pipe_item;
         pipe_item = (PipeItem *)ring_prev(pipe, &pipe_item->link)) {
        Drawable *drawable;
        DrawablePipeItem *dpi;
        ImageItem *image;

        if (pipe_item->type != PIPE_ITEM_TYPE_DRAW)
            continue;
        dpi = SPICE_CONTAINEROF(pipe_item, DrawablePipeItem, dpi_pipe_item);
        drawable = dpi->drawable;
        if (ring_item_is_linked(&drawable->list_link))
            continue; // item hasn't been rendered

        // When a drawable command, X, depends on bitmaps that were resent,
        // these bitmaps state at the client might not be synchronized with X
        // (i.e., the bitmaps can be more futuristic w.r.t X). Thus, X shouldn't
        // be rendered at the client, and we replace it with an image as well.
        if (!drawable_depends_on_areas(drawable,
                                       resent_surface_ids,
                                       resent_areas,
                                       num_resent)) {
            continue;
        }

        image = dcc_add_surface_area_image(dcc, drawable->red_drawable->surface_id,
                                           &drawable->red_drawable->bbox, pipe_item, TRUE);
        resent_surface_ids[num_resent] = drawable->red_drawable->surface_id;
        resent_areas[num_resent] = drawable->red_drawable->bbox;
        num_resent++;

        spice_assert(image);
        red_channel_client_pipe_remove_and_release(RED_CHANNEL_CLIENT(dcc), &dpi->dpi_pipe_item);
        pipe_item = &image->link;
    }
}

static void red_add_lossless_drawable_dependencies(RedChannelClient *rcc,
                                                   Drawable *item,
                                                   int deps_surfaces_ids[],
                                                   SpiceRect *deps_areas[],
                                                   int num_deps)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    DisplayChannel *display = DCC_TO_DC(dcc);
    RedDrawable *drawable = item->red_drawable;
    int sync_rendered = FALSE;
    int i;

    if (!ring_item_is_linked(&item->list_link)) {
        /* drawable was already rendered, we may not be able to retrieve the lossless data
           for the lossy areas */
        sync_rendered = TRUE;

        // checking if the drawable itself or one of the other commands
        // that were rendered, affected the areas that need to be resent
        if (!drawable_intersects_with_areas(item, deps_surfaces_ids,
                                            deps_areas, num_deps)) {
            if (pipe_rendered_drawables_intersect_with_areas(dcc,
                                                             deps_surfaces_ids,
                                                             deps_areas,
                                                             num_deps)) {
                sync_rendered = TRUE;
            }
        } else {
            sync_rendered = TRUE;
        }
    } else {
        sync_rendered = FALSE;
        for (i = 0; i < num_deps; i++) {
            red_update_area_till(display, deps_areas[i],
                                 deps_surfaces_ids[i], item);
        }
    }

    if (!sync_rendered) {
        // pushing the pipe item back to the pipe
        dcc_add_drawable_to_tail(dcc, item);
        // the surfaces areas will be sent as DRAW_COPY commands, that
        // will be executed before the current drawable
        for (i = 0; i < num_deps; i++) {
            dcc_add_surface_area_image(dcc, deps_surfaces_ids[i], deps_areas[i],
                                       red_pipe_get_tail(dcc), FALSE);

        }
    } else {
        int drawable_surface_id[1];
        SpiceRect *drawable_bbox[1];

        drawable_surface_id[0] = drawable->surface_id;
        drawable_bbox[0] = &drawable->bbox;

        // check if the other rendered images in the pipe have updated the drawable bbox
        if (pipe_rendered_drawables_intersect_with_areas(dcc,
                                                         drawable_surface_id,
                                                         drawable_bbox,
                                                         1)) {
            red_pipe_replace_rendered_drawables_with_images(dcc,
                                                            drawable->surface_id,
                                                            &drawable->bbox);
        }

        dcc_add_surface_area_image(dcc, drawable->surface_id, &drawable->bbox,
                                   red_pipe_get_tail(dcc), TRUE);
    }
}

static void red_marshall_qxl_draw_fill(RedChannelClient *rcc,
                                       SpiceMarshaller *base_marshaller,
                                       DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    SpiceMarshaller *brush_pat_out;
    SpiceMarshaller *mask_bitmap_out;
    SpiceFill fill;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_FILL, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    fill = drawable->u.fill;
    spice_marshall_Fill(base_marshaller,
                        &fill,
                        &brush_pat_out,
                        &mask_bitmap_out);

    if (brush_pat_out) {
        fill_bits(dcc, brush_pat_out, fill.brush.u.pattern.pat, item, FALSE);
    }

    fill_mask(rcc, mask_bitmap_out, fill.mask.bitmap, item);
}


static void red_lossy_marshall_qxl_draw_fill(RedChannelClient *rcc,
                                             SpiceMarshaller *m,
                                             DrawablePipeItem *dpi)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;

    int dest_allowed_lossy = FALSE;
    int dest_is_lossy = FALSE;
    SpiceRect dest_lossy_area;
    int brush_is_lossy;
    BitmapData brush_bitmap_data;
    uint16_t rop;

    rop = drawable->u.fill.rop_descriptor;

    dest_allowed_lossy = !((rop & SPICE_ROPD_OP_OR) ||
                           (rop & SPICE_ROPD_OP_AND) ||
                           (rop & SPICE_ROPD_OP_XOR));

    brush_is_lossy = is_brush_lossy(rcc, &drawable->u.fill.brush, item,
                                    &brush_bitmap_data);
    if (!dest_allowed_lossy) {
        dest_is_lossy = is_surface_area_lossy(dcc, item->surface_id, &drawable->bbox,
                                              &dest_lossy_area);
    }

    if (!dest_is_lossy &&
        !(brush_is_lossy && (brush_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE))) {
        int has_mask = !!drawable->u.fill.mask.bitmap;

        red_marshall_qxl_draw_fill(rcc, m, dpi);
        // either the brush operation is opaque, or the dest is not lossy
        surface_lossy_region_update(dcc, item, has_mask, FALSE);
    } else {
        int resend_surface_ids[2];
        SpiceRect *resend_areas[2];
        int num_resend = 0;

        if (dest_is_lossy) {
            resend_surface_ids[num_resend] = item->surface_id;
            resend_areas[num_resend] = &dest_lossy_area;
            num_resend++;
        }

        if (brush_is_lossy && (brush_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = brush_bitmap_data.id;
            resend_areas[num_resend] = &brush_bitmap_data.lossy_rect;
            num_resend++;
        }

        red_add_lossless_drawable_dependencies(rcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static FillBitsType red_marshall_qxl_draw_opaque(RedChannelClient *rcc,
                                                 SpiceMarshaller *base_marshaller,
                                                 DrawablePipeItem *dpi, int src_allowed_lossy)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *brush_pat_out;
    SpiceMarshaller *src_bitmap_out;
    SpiceMarshaller *mask_bitmap_out;
    SpiceOpaque opaque;
    FillBitsType src_send_type;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_OPAQUE, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    opaque = drawable->u.opaque;
    spice_marshall_Opaque(base_marshaller,
                          &opaque,
                          &src_bitmap_out,
                          &brush_pat_out,
                          &mask_bitmap_out);

    src_send_type = fill_bits(dcc, src_bitmap_out, opaque.src_bitmap, item,
                              src_allowed_lossy);

    if (brush_pat_out) {
        fill_bits(dcc, brush_pat_out, opaque.brush.u.pattern.pat, item, FALSE);
    }
    fill_mask(rcc, mask_bitmap_out, opaque.mask.bitmap, item);

    return src_send_type;
}

static void red_lossy_marshall_qxl_draw_opaque(RedChannelClient *rcc,
                                               SpiceMarshaller *m,
                                               DrawablePipeItem *dpi)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;

    int src_allowed_lossy;
    int rop;
    int src_is_lossy = FALSE;
    int brush_is_lossy = FALSE;
    BitmapData src_bitmap_data;
    BitmapData brush_bitmap_data;

    rop = drawable->u.opaque.rop_descriptor;
    src_allowed_lossy = !((rop & SPICE_ROPD_OP_OR) ||
                          (rop & SPICE_ROPD_OP_AND) ||
                          (rop & SPICE_ROPD_OP_XOR));

    brush_is_lossy = is_brush_lossy(rcc, &drawable->u.opaque.brush, item,
                                    &brush_bitmap_data);

    if (!src_allowed_lossy) {
        src_is_lossy = is_bitmap_lossy(rcc, drawable->u.opaque.src_bitmap,
                                       &drawable->u.opaque.src_area,
                                       item,
                                       &src_bitmap_data);
    }

    if (!(brush_is_lossy && (brush_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) &&
        !(src_is_lossy && (src_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE))) {
        FillBitsType src_send_type;
        int has_mask = !!drawable->u.opaque.mask.bitmap;

        src_send_type = red_marshall_qxl_draw_opaque(rcc, m, dpi, src_allowed_lossy);
        if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSY) {
            src_is_lossy = TRUE;
        } else if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSLESS) {
            src_is_lossy = FALSE;
        }

        surface_lossy_region_update(dcc, item, has_mask, src_is_lossy);
    } else {
        int resend_surface_ids[2];
        SpiceRect *resend_areas[2];
        int num_resend = 0;

        if (src_is_lossy && (src_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = src_bitmap_data.id;
            resend_areas[num_resend] = &src_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (brush_is_lossy && (brush_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = brush_bitmap_data.id;
            resend_areas[num_resend] = &brush_bitmap_data.lossy_rect;
            num_resend++;
        }

        red_add_lossless_drawable_dependencies(rcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static FillBitsType red_marshall_qxl_draw_copy(RedChannelClient *rcc,
                                               SpiceMarshaller *base_marshaller,
                                               DrawablePipeItem *dpi, int src_allowed_lossy)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *src_bitmap_out;
    SpiceMarshaller *mask_bitmap_out;
    SpiceCopy copy;
    FillBitsType src_send_type;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_COPY, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    copy = drawable->u.copy;
    spice_marshall_Copy(base_marshaller,
                        &copy,
                        &src_bitmap_out,
                        &mask_bitmap_out);

    src_send_type = fill_bits(dcc, src_bitmap_out, copy.src_bitmap, item, src_allowed_lossy);
    fill_mask(rcc, mask_bitmap_out, copy.mask.bitmap, item);

    return src_send_type;
}

static void red_lossy_marshall_qxl_draw_copy(RedChannelClient *rcc,
                                             SpiceMarshaller *base_marshaller,
                                             DrawablePipeItem *dpi)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    int has_mask = !!drawable->u.copy.mask.bitmap;
    int src_is_lossy;
    BitmapData src_bitmap_data;
    FillBitsType src_send_type;

    src_is_lossy = is_bitmap_lossy(rcc, drawable->u.copy.src_bitmap,
                                   &drawable->u.copy.src_area, item, &src_bitmap_data);

    src_send_type = red_marshall_qxl_draw_copy(rcc, base_marshaller, dpi, TRUE);
    if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSY) {
        src_is_lossy = TRUE;
    } else if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSLESS) {
        src_is_lossy = FALSE;
    }
    surface_lossy_region_update(dcc, item, has_mask,
                                src_is_lossy);
}

static void red_marshall_qxl_draw_transparent(RedChannelClient *rcc,
                                              SpiceMarshaller *base_marshaller,
                                              DrawablePipeItem *dpi)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *src_bitmap_out;
    SpiceTransparent transparent;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_TRANSPARENT,
                                      &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    transparent = drawable->u.transparent;
    spice_marshall_Transparent(base_marshaller,
                               &transparent,
                               &src_bitmap_out);
    fill_bits(dcc, src_bitmap_out, transparent.src_bitmap, item, FALSE);
}

static void red_lossy_marshall_qxl_draw_transparent(RedChannelClient *rcc,
                                                    SpiceMarshaller *base_marshaller,
                                                    DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    int src_is_lossy;
    BitmapData src_bitmap_data;

    src_is_lossy = is_bitmap_lossy(rcc, drawable->u.transparent.src_bitmap,
                                   &drawable->u.transparent.src_area, item, &src_bitmap_data);

    if (!src_is_lossy || (src_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE)) {
        red_marshall_qxl_draw_transparent(rcc, base_marshaller, dpi);
        // don't update surface lossy region since transperent areas might be lossy
    } else {
        int resend_surface_ids[1];
        SpiceRect *resend_areas[1];

        resend_surface_ids[0] = src_bitmap_data.id;
        resend_areas[0] = &src_bitmap_data.lossy_rect;

        red_add_lossless_drawable_dependencies(rcc, item,
                                               resend_surface_ids, resend_areas, 1);
    }
}

static FillBitsType red_marshall_qxl_draw_alpha_blend(RedChannelClient *rcc,
                                                      SpiceMarshaller *base_marshaller,
                                                      DrawablePipeItem *dpi,
                                                      int src_allowed_lossy)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *src_bitmap_out;
    SpiceAlphaBlend alpha_blend;
    FillBitsType src_send_type;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_ALPHA_BLEND,
                                      &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    alpha_blend = drawable->u.alpha_blend;
    spice_marshall_AlphaBlend(base_marshaller,
                              &alpha_blend,
                              &src_bitmap_out);
    src_send_type = fill_bits(dcc, src_bitmap_out, alpha_blend.src_bitmap, item,
                              src_allowed_lossy);

    return src_send_type;
}

static void red_lossy_marshall_qxl_draw_alpha_blend(RedChannelClient *rcc,
                                                    SpiceMarshaller *base_marshaller,
                                                    DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    int src_is_lossy;
    BitmapData src_bitmap_data;
    FillBitsType src_send_type;

    src_is_lossy = is_bitmap_lossy(rcc, drawable->u.alpha_blend.src_bitmap,
                                   &drawable->u.alpha_blend.src_area, item, &src_bitmap_data);

    src_send_type = red_marshall_qxl_draw_alpha_blend(rcc, base_marshaller, dpi, TRUE);

    if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSY) {
        src_is_lossy = TRUE;
    } else if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSLESS) {
        src_is_lossy = FALSE;
    }

    if (src_is_lossy) {
        surface_lossy_region_update(dcc, item, FALSE, src_is_lossy);
    } // else, the area stays lossy/lossless as the destination
}

static void red_marshall_qxl_copy_bits(RedChannelClient *rcc,
                                       SpiceMarshaller *base_marshaller,
                                       DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpicePoint copy_bits;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_COPY_BITS, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    copy_bits = drawable->u.copy_bits.src_pos;
    spice_marshall_Point(base_marshaller,
                         &copy_bits);
}

static void red_lossy_marshall_qxl_copy_bits(RedChannelClient *rcc,
                                             SpiceMarshaller *base_marshaller,
                                             DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceRect src_rect;
    int horz_offset;
    int vert_offset;
    int src_is_lossy;
    SpiceRect src_lossy_area;

    red_marshall_qxl_copy_bits(rcc, base_marshaller, dpi);

    horz_offset = drawable->u.copy_bits.src_pos.x - drawable->bbox.left;
    vert_offset = drawable->u.copy_bits.src_pos.y - drawable->bbox.top;

    src_rect.left = drawable->u.copy_bits.src_pos.x;
    src_rect.top = drawable->u.copy_bits.src_pos.y;
    src_rect.right = drawable->bbox.right + horz_offset;
    src_rect.bottom = drawable->bbox.bottom + vert_offset;

    src_is_lossy = is_surface_area_lossy(dcc, item->surface_id,
                                         &src_rect, &src_lossy_area);

    surface_lossy_region_update(dcc, item, FALSE, src_is_lossy);
}

static void red_marshall_qxl_draw_blend(RedChannelClient *rcc,
                                        SpiceMarshaller *base_marshaller,
                                        DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *src_bitmap_out;
    SpiceMarshaller *mask_bitmap_out;
    SpiceBlend blend;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_BLEND, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    blend = drawable->u.blend;
    spice_marshall_Blend(base_marshaller,
                         &blend,
                         &src_bitmap_out,
                         &mask_bitmap_out);

    fill_bits(dcc, src_bitmap_out, blend.src_bitmap, item, FALSE);

    fill_mask(rcc, mask_bitmap_out, blend.mask.bitmap, item);
}

static void red_lossy_marshall_qxl_draw_blend(RedChannelClient *rcc,
                                              SpiceMarshaller *base_marshaller,
                                              DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    int src_is_lossy;
    BitmapData src_bitmap_data;
    int dest_is_lossy;
    SpiceRect dest_lossy_area;

    src_is_lossy = is_bitmap_lossy(rcc, drawable->u.blend.src_bitmap,
                                   &drawable->u.blend.src_area, item, &src_bitmap_data);
    dest_is_lossy = is_surface_area_lossy(dcc, drawable->surface_id,
                                          &drawable->bbox, &dest_lossy_area);

    if (!dest_is_lossy &&
        (!src_is_lossy || (src_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE))) {
        red_marshall_qxl_draw_blend(rcc, base_marshaller, dpi);
    } else {
        int resend_surface_ids[2];
        SpiceRect *resend_areas[2];
        int num_resend = 0;

        if (src_is_lossy && (src_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = src_bitmap_data.id;
            resend_areas[num_resend] = &src_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (dest_is_lossy) {
            resend_surface_ids[num_resend] = item->surface_id;
            resend_areas[num_resend] = &dest_lossy_area;
            num_resend++;
        }

        red_add_lossless_drawable_dependencies(rcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static void red_marshall_qxl_draw_blackness(RedChannelClient *rcc,
                                            SpiceMarshaller *base_marshaller,
                                            DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *mask_bitmap_out;
    SpiceBlackness blackness;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_BLACKNESS, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    blackness = drawable->u.blackness;

    spice_marshall_Blackness(base_marshaller,
                             &blackness,
                             &mask_bitmap_out);

    fill_mask(rcc, mask_bitmap_out, blackness.mask.bitmap, item);
}

static void red_lossy_marshall_qxl_draw_blackness(RedChannelClient *rcc,
                                                  SpiceMarshaller *base_marshaller,
                                                  DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    int has_mask = !!drawable->u.blackness.mask.bitmap;

    red_marshall_qxl_draw_blackness(rcc, base_marshaller, dpi);

    surface_lossy_region_update(dcc, item, has_mask, FALSE);
}

static void red_marshall_qxl_draw_whiteness(RedChannelClient *rcc,
                                            SpiceMarshaller *base_marshaller,
                                            DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *mask_bitmap_out;
    SpiceWhiteness whiteness;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_WHITENESS, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    whiteness = drawable->u.whiteness;

    spice_marshall_Whiteness(base_marshaller,
                             &whiteness,
                             &mask_bitmap_out);

    fill_mask(rcc, mask_bitmap_out, whiteness.mask.bitmap, item);
}

static void red_lossy_marshall_qxl_draw_whiteness(RedChannelClient *rcc,
                                                  SpiceMarshaller *base_marshaller,
                                                  DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    int has_mask = !!drawable->u.whiteness.mask.bitmap;

    red_marshall_qxl_draw_whiteness(rcc, base_marshaller, dpi);

    surface_lossy_region_update(dcc, item, has_mask, FALSE);
}

static void red_marshall_qxl_draw_inverse(RedChannelClient *rcc,
                                          SpiceMarshaller *base_marshaller,
                                          Drawable *item)
{
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *mask_bitmap_out;
    SpiceInvers inverse;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_INVERS, NULL);
    fill_base(base_marshaller, item);
    inverse = drawable->u.invers;

    spice_marshall_Invers(base_marshaller,
                          &inverse,
                          &mask_bitmap_out);

    fill_mask(rcc, mask_bitmap_out, inverse.mask.bitmap, item);
}

static void red_lossy_marshall_qxl_draw_inverse(RedChannelClient *rcc,
                                                SpiceMarshaller *base_marshaller,
                                                Drawable *item)
{
    red_marshall_qxl_draw_inverse(rcc, base_marshaller, item);
}

static void red_marshall_qxl_draw_rop3(RedChannelClient *rcc,
                                       SpiceMarshaller *base_marshaller,
                                       DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceRop3 rop3;
    SpiceMarshaller *src_bitmap_out;
    SpiceMarshaller *brush_pat_out;
    SpiceMarshaller *mask_bitmap_out;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_ROP3, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    rop3 = drawable->u.rop3;
    spice_marshall_Rop3(base_marshaller,
                        &rop3,
                        &src_bitmap_out,
                        &brush_pat_out,
                        &mask_bitmap_out);

    fill_bits(dcc, src_bitmap_out, rop3.src_bitmap, item, FALSE);

    if (brush_pat_out) {
        fill_bits(dcc, brush_pat_out, rop3.brush.u.pattern.pat, item, FALSE);
    }
    fill_mask(rcc, mask_bitmap_out, rop3.mask.bitmap, item);
}

static void red_lossy_marshall_qxl_draw_rop3(RedChannelClient *rcc,
                                             SpiceMarshaller *base_marshaller,
                                             DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    int src_is_lossy;
    BitmapData src_bitmap_data;
    int brush_is_lossy;
    BitmapData brush_bitmap_data;
    int dest_is_lossy;
    SpiceRect dest_lossy_area;

    src_is_lossy = is_bitmap_lossy(rcc, drawable->u.rop3.src_bitmap,
                                   &drawable->u.rop3.src_area, item, &src_bitmap_data);
    brush_is_lossy = is_brush_lossy(rcc, &drawable->u.rop3.brush, item,
                                    &brush_bitmap_data);
    dest_is_lossy = is_surface_area_lossy(dcc, drawable->surface_id,
                                          &drawable->bbox, &dest_lossy_area);

    if ((!src_is_lossy || (src_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE)) &&
        (!brush_is_lossy || (brush_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE)) &&
        !dest_is_lossy) {
        int has_mask = !!drawable->u.rop3.mask.bitmap;
        red_marshall_qxl_draw_rop3(rcc, base_marshaller, dpi);
        surface_lossy_region_update(dcc, item, has_mask, FALSE);
    } else {
        int resend_surface_ids[3];
        SpiceRect *resend_areas[3];
        int num_resend = 0;

        if (src_is_lossy && (src_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = src_bitmap_data.id;
            resend_areas[num_resend] = &src_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (brush_is_lossy && (brush_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = brush_bitmap_data.id;
            resend_areas[num_resend] = &brush_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (dest_is_lossy) {
            resend_surface_ids[num_resend] = item->surface_id;
            resend_areas[num_resend] = &dest_lossy_area;
            num_resend++;
        }

        red_add_lossless_drawable_dependencies(rcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static void red_marshall_qxl_draw_composite(RedChannelClient *rcc,
                                            SpiceMarshaller *base_marshaller,
                                            DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *src_bitmap_out;
    SpiceMarshaller *mask_bitmap_out;
    SpiceComposite composite;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_COMPOSITE, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    composite = drawable->u.composite;
    spice_marshall_Composite(base_marshaller,
                             &composite,
                             &src_bitmap_out,
                             &mask_bitmap_out);

    fill_bits(dcc, src_bitmap_out, composite.src_bitmap, item, FALSE);
    if (mask_bitmap_out) {
        fill_bits(dcc, mask_bitmap_out, composite.mask_bitmap, item, FALSE);
    }
}

static void red_lossy_marshall_qxl_draw_composite(RedChannelClient *rcc,
                                                  SpiceMarshaller *base_marshaller,
                                                  DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    int src_is_lossy;
    BitmapData src_bitmap_data;
    int mask_is_lossy;
    BitmapData mask_bitmap_data;
    int dest_is_lossy;
    SpiceRect dest_lossy_area;

    src_is_lossy = is_bitmap_lossy(rcc, drawable->u.composite.src_bitmap,
                                   NULL, item, &src_bitmap_data);
    mask_is_lossy = drawable->u.composite.mask_bitmap &&
        is_bitmap_lossy(rcc, drawable->u.composite.mask_bitmap, NULL, item, &mask_bitmap_data);

    dest_is_lossy = is_surface_area_lossy(dcc, drawable->surface_id,
                                          &drawable->bbox, &dest_lossy_area);

    if ((!src_is_lossy || (src_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE))   &&
        (!mask_is_lossy || (mask_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE)) &&
        !dest_is_lossy) {
        red_marshall_qxl_draw_composite(rcc, base_marshaller, dpi);
        surface_lossy_region_update(dcc, item, FALSE, FALSE);
    }
    else {
        int resend_surface_ids[3];
        SpiceRect *resend_areas[3];
        int num_resend = 0;

        if (src_is_lossy && (src_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = src_bitmap_data.id;
            resend_areas[num_resend] = &src_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (mask_is_lossy && (mask_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = mask_bitmap_data.id;
            resend_areas[num_resend] = &mask_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (dest_is_lossy) {
            resend_surface_ids[num_resend] = item->surface_id;
            resend_areas[num_resend] = &dest_lossy_area;
            num_resend++;
        }

        red_add_lossless_drawable_dependencies(rcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static void red_marshall_qxl_draw_stroke(RedChannelClient *rcc,
                                         SpiceMarshaller *base_marshaller,
                                         DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceStroke stroke;
    SpiceMarshaller *brush_pat_out;
    SpiceMarshaller *style_out;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_STROKE, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    stroke = drawable->u.stroke;
    spice_marshall_Stroke(base_marshaller,
                          &stroke,
                          &style_out,
                          &brush_pat_out);

    fill_attr(style_out, &stroke.attr, item->group_id);
    if (brush_pat_out) {
        fill_bits(dcc, brush_pat_out, stroke.brush.u.pattern.pat, item, FALSE);
    }
}

static void red_lossy_marshall_qxl_draw_stroke(RedChannelClient *rcc,
                                               SpiceMarshaller *base_marshaller,
                                               DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    int brush_is_lossy;
    BitmapData brush_bitmap_data;
    int dest_is_lossy = FALSE;
    SpiceRect dest_lossy_area;
    int rop;

    brush_is_lossy = is_brush_lossy(rcc, &drawable->u.stroke.brush, item,
                                    &brush_bitmap_data);

    // back_mode is not used at the client. Ignoring.
    rop = drawable->u.stroke.fore_mode;

    // assuming that if the brush type is solid, the destination can
    // be lossy, no matter what the rop is.
    if (drawable->u.stroke.brush.type != SPICE_BRUSH_TYPE_SOLID &&
        ((rop & SPICE_ROPD_OP_OR) || (rop & SPICE_ROPD_OP_AND) ||
        (rop & SPICE_ROPD_OP_XOR))) {
        dest_is_lossy = is_surface_area_lossy(dcc, drawable->surface_id,
                                              &drawable->bbox, &dest_lossy_area);
    }

    if (!dest_is_lossy &&
        (!brush_is_lossy || (brush_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE)))
    {
        red_marshall_qxl_draw_stroke(rcc, base_marshaller, dpi);
    } else {
        int resend_surface_ids[2];
        SpiceRect *resend_areas[2];
        int num_resend = 0;

        if (brush_is_lossy && (brush_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = brush_bitmap_data.id;
            resend_areas[num_resend] = &brush_bitmap_data.lossy_rect;
            num_resend++;
        }

        // TODO: use the path in order to resend smaller areas
        if (dest_is_lossy) {
            resend_surface_ids[num_resend] = drawable->surface_id;
            resend_areas[num_resend] = &dest_lossy_area;
            num_resend++;
        }

        red_add_lossless_drawable_dependencies(rcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static void red_marshall_qxl_draw_text(RedChannelClient *rcc,
                                       SpiceMarshaller *base_marshaller,
                                       DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceText text;
    SpiceMarshaller *brush_pat_out;
    SpiceMarshaller *back_brush_pat_out;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_TEXT, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    text = drawable->u.text;
    spice_marshall_Text(base_marshaller,
                        &text,
                        &brush_pat_out,
                        &back_brush_pat_out);

    if (brush_pat_out) {
        fill_bits(dcc, brush_pat_out, text.fore_brush.u.pattern.pat, item, FALSE);
    }
    if (back_brush_pat_out) {
        fill_bits(dcc, back_brush_pat_out, text.back_brush.u.pattern.pat, item, FALSE);
    }
}

static void red_lossy_marshall_qxl_draw_text(RedChannelClient *rcc,
                                             SpiceMarshaller *base_marshaller,
                                             DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    int fg_is_lossy;
    BitmapData fg_bitmap_data;
    int bg_is_lossy;
    BitmapData bg_bitmap_data;
    int dest_is_lossy = FALSE;
    SpiceRect dest_lossy_area;
    int rop = 0;

    fg_is_lossy = is_brush_lossy(rcc, &drawable->u.text.fore_brush, item,
                                 &fg_bitmap_data);
    bg_is_lossy = is_brush_lossy(rcc, &drawable->u.text.back_brush, item,
                                 &bg_bitmap_data);

    // assuming that if the brush type is solid, the destination can
    // be lossy, no matter what the rop is.
    if (drawable->u.text.fore_brush.type != SPICE_BRUSH_TYPE_SOLID) {
        rop = drawable->u.text.fore_mode;
    }

    if (drawable->u.text.back_brush.type != SPICE_BRUSH_TYPE_SOLID) {
        rop |= drawable->u.text.back_mode;
    }

    if ((rop & SPICE_ROPD_OP_OR) || (rop & SPICE_ROPD_OP_AND) ||
        (rop & SPICE_ROPD_OP_XOR)) {
        dest_is_lossy = is_surface_area_lossy(dcc, drawable->surface_id,
                                              &drawable->bbox, &dest_lossy_area);
    }

    if (!dest_is_lossy &&
        (!fg_is_lossy || (fg_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE)) &&
        (!bg_is_lossy || (bg_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE))) {
        red_marshall_qxl_draw_text(rcc, base_marshaller, dpi);
    } else {
        int resend_surface_ids[3];
        SpiceRect *resend_areas[3];
        int num_resend = 0;

        if (fg_is_lossy && (fg_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = fg_bitmap_data.id;
            resend_areas[num_resend] = &fg_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (bg_is_lossy && (bg_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = bg_bitmap_data.id;
            resend_areas[num_resend] = &bg_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (dest_is_lossy) {
            resend_surface_ids[num_resend] = drawable->surface_id;
            resend_areas[num_resend] = &dest_lossy_area;
            num_resend++;
        }
        red_add_lossless_drawable_dependencies(rcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static void red_lossy_marshall_qxl_drawable(RedChannelClient *rcc,
                                            SpiceMarshaller *base_marshaller,
                                            DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    switch (item->red_drawable->type) {
    case QXL_DRAW_FILL:
        red_lossy_marshall_qxl_draw_fill(rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_OPAQUE:
        red_lossy_marshall_qxl_draw_opaque(rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_COPY:
        red_lossy_marshall_qxl_draw_copy(rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_TRANSPARENT:
        red_lossy_marshall_qxl_draw_transparent(rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_ALPHA_BLEND:
        red_lossy_marshall_qxl_draw_alpha_blend(rcc, base_marshaller, dpi);
        break;
    case QXL_COPY_BITS:
        red_lossy_marshall_qxl_copy_bits(rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_BLEND:
        red_lossy_marshall_qxl_draw_blend(rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_BLACKNESS:
        red_lossy_marshall_qxl_draw_blackness(rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_WHITENESS:
        red_lossy_marshall_qxl_draw_whiteness(rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_INVERS:
        red_lossy_marshall_qxl_draw_inverse(rcc, base_marshaller, item);
        break;
    case QXL_DRAW_ROP3:
        red_lossy_marshall_qxl_draw_rop3(rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_COMPOSITE:
        red_lossy_marshall_qxl_draw_composite(rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_STROKE:
        red_lossy_marshall_qxl_draw_stroke(rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_TEXT:
        red_lossy_marshall_qxl_draw_text(rcc, base_marshaller, dpi);
        break;
    default:
        spice_error("invalid type");
    }
}

static inline void red_marshall_qxl_drawable(RedChannelClient *rcc,
                                             SpiceMarshaller *m, DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;

    switch (drawable->type) {
    case QXL_DRAW_FILL:
        red_marshall_qxl_draw_fill(rcc, m, dpi);
        break;
    case QXL_DRAW_OPAQUE:
        red_marshall_qxl_draw_opaque(rcc, m, dpi, FALSE);
        break;
    case QXL_DRAW_COPY:
        red_marshall_qxl_draw_copy(rcc, m, dpi, FALSE);
        break;
    case QXL_DRAW_TRANSPARENT:
        red_marshall_qxl_draw_transparent(rcc, m, dpi);
        break;
    case QXL_DRAW_ALPHA_BLEND:
        red_marshall_qxl_draw_alpha_blend(rcc, m, dpi, FALSE);
        break;
    case QXL_COPY_BITS:
        red_marshall_qxl_copy_bits(rcc, m, dpi);
        break;
    case QXL_DRAW_BLEND:
        red_marshall_qxl_draw_blend(rcc, m, dpi);
        break;
    case QXL_DRAW_BLACKNESS:
        red_marshall_qxl_draw_blackness(rcc, m, dpi);
        break;
    case QXL_DRAW_WHITENESS:
        red_marshall_qxl_draw_whiteness(rcc, m, dpi);
        break;
    case QXL_DRAW_INVERS:
        red_marshall_qxl_draw_inverse(rcc, m, item);
        break;
    case QXL_DRAW_ROP3:
        red_marshall_qxl_draw_rop3(rcc, m, dpi);
        break;
    case QXL_DRAW_STROKE:
        red_marshall_qxl_draw_stroke(rcc, m, dpi);
        break;
    case QXL_DRAW_COMPOSITE:
        red_marshall_qxl_draw_composite(rcc, m, dpi);
        break;
    case QXL_DRAW_TEXT:
        red_marshall_qxl_draw_text(rcc, m, dpi);
        break;
    default:
        spice_error("invalid type");
    }
}

static inline void display_marshal_sub_msg_inval_list(SpiceMarshaller *m,
                                                       FreeList *free_list)
{
    /* type + size + submessage */
    spice_marshaller_add_uint16(m, SPICE_MSG_DISPLAY_INVAL_LIST);
    spice_marshaller_add_uint32(m, sizeof(*free_list->res) +
                                free_list->res->count * sizeof(free_list->res->resources[0]));
    spice_marshall_msg_display_inval_list(m, free_list->res);
}

static inline void display_marshal_sub_msg_inval_list_wait(SpiceMarshaller *m,
                                                            FreeList *free_list)

{
    /* type + size + submessage */
    spice_marshaller_add_uint16(m, SPICE_MSG_WAIT_FOR_CHANNELS);
    spice_marshaller_add_uint32(m, sizeof(free_list->wait.header) +
                                free_list->wait.header.wait_count * sizeof(free_list->wait.buf[0]));
    spice_marshall_msg_wait_for_channels(m, &free_list->wait.header);
}

/* use legacy SpiceDataHeader (with sub_list) */
static inline void display_channel_send_free_list_legacy(RedChannelClient *rcc)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    FreeList *free_list = &dcc->send_data.free_list;
    SpiceMarshaller *marshaller;
    int sub_list_len = 1;
    SpiceMarshaller *wait_m = NULL;
    SpiceMarshaller *inval_m;
    SpiceMarshaller *sub_list_m;

    marshaller = red_channel_client_get_marshaller(rcc);
    inval_m = spice_marshaller_get_submarshaller(marshaller);

    display_marshal_sub_msg_inval_list(inval_m, free_list);

    if (free_list->wait.header.wait_count) {
        wait_m = spice_marshaller_get_submarshaller(marshaller);
        display_marshal_sub_msg_inval_list_wait(wait_m, free_list);
        sub_list_len++;
    }

    sub_list_m = spice_marshaller_get_submarshaller(marshaller);
    spice_marshaller_add_uint16(sub_list_m, sub_list_len);
    if (wait_m) {
        spice_marshaller_add_uint32(sub_list_m, spice_marshaller_get_offset(wait_m));
    }
    spice_marshaller_add_uint32(sub_list_m, spice_marshaller_get_offset(inval_m));
    red_channel_client_set_header_sub_list(rcc, spice_marshaller_get_offset(sub_list_m));
}

/* use mini header and SPICE_MSG_LIST */
static inline void display_channel_send_free_list(RedChannelClient *rcc)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    FreeList *free_list = &dcc->send_data.free_list;
    int sub_list_len = 1;
    SpiceMarshaller *urgent_marshaller;
    SpiceMarshaller *wait_m = NULL;
    SpiceMarshaller *inval_m;
    uint32_t sub_arr_offset;
    uint32_t wait_offset = 0;
    uint32_t inval_offset = 0;
    int i;

    urgent_marshaller = red_channel_client_switch_to_urgent_sender(rcc);
    for (i = 0; i < dcc->send_data.num_pixmap_cache_items; i++) {
        int dummy;
        /* When using the urgent marshaller, the serial number of the message that is
         * going to be sent right after the SPICE_MSG_LIST, is increased by one.
         * But all this message pixmaps cache references used its old serial.
         * we use pixmap_cache_items to collect these pixmaps, and we update their serial
         * by calling pixmap_cache_hit. */
        dcc_pixmap_cache_hit(dcc, dcc->send_data.pixmap_cache_items[i], &dummy);
    }

    if (free_list->wait.header.wait_count) {
        red_channel_client_init_send_data(rcc, SPICE_MSG_LIST, NULL);
    } else { /* only one message, no need for a list */
        red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_INVAL_LIST, NULL);
        spice_marshall_msg_display_inval_list(urgent_marshaller, free_list->res);
        return;
    }

    inval_m = spice_marshaller_get_submarshaller(urgent_marshaller);
    display_marshal_sub_msg_inval_list(inval_m, free_list);

    if (free_list->wait.header.wait_count) {
        wait_m = spice_marshaller_get_submarshaller(urgent_marshaller);
        display_marshal_sub_msg_inval_list_wait(wait_m, free_list);
        sub_list_len++;
    }

    sub_arr_offset = sub_list_len * sizeof(uint32_t);

    spice_marshaller_add_uint16(urgent_marshaller, sub_list_len);
    inval_offset = spice_marshaller_get_offset(inval_m); // calc the offset before
                                                         // adding the sub list
                                                         // offsets array to the marshaller
    /* adding the array of offsets */
    if (wait_m) {
        wait_offset = spice_marshaller_get_offset(wait_m);
        spice_marshaller_add_uint32(urgent_marshaller, wait_offset + sub_arr_offset);
    }
    spice_marshaller_add_uint32(urgent_marshaller, inval_offset + sub_arr_offset);
}

static inline void display_begin_send_message(RedChannelClient *rcc)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    FreeList *free_list = &dcc->send_data.free_list;

    if (free_list->res->count) {
        int sync_count = 0;
        int i;

        for (i = 0; i < MAX_CACHE_CLIENTS; i++) {
            if (i != dcc->common.id && free_list->sync[i] != 0) {
                free_list->wait.header.wait_list[sync_count].channel_type = SPICE_CHANNEL_DISPLAY;
                free_list->wait.header.wait_list[sync_count].channel_id = i;
                free_list->wait.header.wait_list[sync_count++].message_serial = free_list->sync[i];
            }
        }
        free_list->wait.header.wait_count = sync_count;

        if (rcc->is_mini_header) {
            display_channel_send_free_list(rcc);
        } else {
            display_channel_send_free_list_legacy(rcc);
        }
    }
    red_channel_client_begin_send_message(rcc);
}

static inline uint8_t *red_get_image_line(SpiceChunks *chunks, size_t *offset,
                                          int *chunk_nr, int stride)
{
    uint8_t *ret;
    SpiceChunk *chunk;

    chunk = &chunks->chunk[*chunk_nr];

    if (*offset == chunk->len) {
        if (*chunk_nr == chunks->num_chunks - 1) {
            return NULL; /* Last chunk */
        }
        *offset = 0;
        (*chunk_nr)++;
        chunk = &chunks->chunk[*chunk_nr];
    }

    if (chunk->len - *offset < stride) {
        spice_warning("bad chunk alignment");
        return NULL;
    }
    ret = chunk->data + *offset;
    *offset += stride;
    return ret;
}

static int encode_frame(DisplayChannelClient *dcc, const SpiceRect *src,
                        const SpiceBitmap *image, Stream *stream)
{
    SpiceChunks *chunks;
    uint32_t image_stride;
    size_t offset;
    int i, chunk;
    StreamAgent *agent = &dcc->stream_agents[get_stream_id(DCC_TO_DC(dcc), stream)];

    chunks = image->data;
    offset = 0;
    chunk = 0;
    image_stride = image->stride;

    const int skip_lines = stream->top_down ? src->top : image->y - (src->bottom - 0);
    for (i = 0; i < skip_lines; i++) {
        red_get_image_line(chunks, &offset, &chunk, image_stride);
    }

    const unsigned int stream_height = src->bottom - src->top;
    const unsigned int stream_width = src->right - src->left;

    for (i = 0; i < stream_height; i++) {
        uint8_t *src_line =
            (uint8_t *)red_get_image_line(chunks, &offset, &chunk, image_stride);

        if (!src_line) {
            return FALSE;
        }

        src_line += src->left * mjpeg_encoder_get_bytes_per_pixel(agent->mjpeg_encoder);
        if (mjpeg_encoder_encode_scanline(agent->mjpeg_encoder, src_line, stream_width) == 0)
            return FALSE;
    }

    return TRUE;
}

static inline int red_marshall_stream_data(RedChannelClient *rcc,
                  SpiceMarshaller *base_marshaller, Drawable *drawable)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    DisplayChannel *display = DCC_TO_DC(dcc);
    Stream *stream = drawable->stream;
    SpiceImage *image;
    uint32_t frame_mm_time;
    int n;
    int width, height;
    int ret;

    if (!stream) {
        spice_assert(drawable->sized_stream);
        stream = drawable->sized_stream;
    }
    spice_assert(drawable->red_drawable->type == QXL_DRAW_COPY);

    image = drawable->red_drawable->u.copy.src_bitmap;

    if (image->descriptor.type != SPICE_IMAGE_TYPE_BITMAP) {
        return FALSE;
    }

    if (drawable->sized_stream) {
        if (red_channel_client_test_remote_cap(rcc, SPICE_DISPLAY_CAP_SIZED_STREAM)) {
            SpiceRect *src_rect = &drawable->red_drawable->u.copy.src_area;

            width = src_rect->right - src_rect->left;
            height = src_rect->bottom - src_rect->top;
        } else {
            return FALSE;
        }
    } else {
        width = stream->width;
        height = stream->height;
    }

    StreamAgent *agent = &dcc->stream_agents[get_stream_id(display, stream)];
    uint64_t time_now = red_get_monotonic_time();
    size_t outbuf_size;

    if (!dcc->use_mjpeg_encoder_rate_control) {
        if (time_now - agent->last_send_time < (1000 * 1000 * 1000) / agent->fps) {
            agent->frames--;
#ifdef STREAM_STATS
            agent->stats.num_drops_fps++;
#endif
            return TRUE;
        }
    }

    /* workaround for vga streams */
    frame_mm_time =  drawable->red_drawable->mm_time ?
                        drawable->red_drawable->mm_time :
                        reds_get_mm_time();
    outbuf_size = dcc->send_data.stream_outbuf_size;
    ret = mjpeg_encoder_start_frame(agent->mjpeg_encoder, image->u.bitmap.format,
                                    width, height,
                                    &dcc->send_data.stream_outbuf,
                                    &outbuf_size,
                                    frame_mm_time);
    switch (ret) {
    case MJPEG_ENCODER_FRAME_DROP:
        spice_assert(dcc->use_mjpeg_encoder_rate_control);
#ifdef STREAM_STATS
        agent->stats.num_drops_fps++;
#endif
        return TRUE;
    case MJPEG_ENCODER_FRAME_UNSUPPORTED:
        return FALSE;
    case MJPEG_ENCODER_FRAME_ENCODE_START:
        break;
    default:
        spice_error("bad return value (%d) from mjpeg_encoder_start_frame", ret);
        return FALSE;
    }

    if (!encode_frame(dcc, &drawable->red_drawable->u.copy.src_area,
                      &image->u.bitmap, stream)) {
        return FALSE;
    }
    n = mjpeg_encoder_end_frame(agent->mjpeg_encoder);
    dcc->send_data.stream_outbuf_size = outbuf_size;

    if (!drawable->sized_stream) {
        SpiceMsgDisplayStreamData stream_data;

        red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_STREAM_DATA, NULL);

        stream_data.base.id = get_stream_id(display, stream);
        stream_data.base.multi_media_time = frame_mm_time;
        stream_data.data_size = n;

        spice_marshall_msg_display_stream_data(base_marshaller, &stream_data);
    } else {
        SpiceMsgDisplayStreamDataSized stream_data;

        red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_STREAM_DATA_SIZED, NULL);

        stream_data.base.id = get_stream_id(display, stream);
        stream_data.base.multi_media_time = frame_mm_time;
        stream_data.data_size = n;
        stream_data.width = width;
        stream_data.height = height;
        stream_data.dest = drawable->red_drawable->bbox;

        spice_debug("stream %d: sized frame: dest ==> ", stream_data.base.id);
        rect_debug(&stream_data.dest);
        spice_marshall_msg_display_stream_data_sized(base_marshaller, &stream_data);
    }
    spice_marshaller_add_ref(base_marshaller,
                             dcc->send_data.stream_outbuf, n);
    agent->last_send_time = time_now;
#ifdef STREAM_STATS
    agent->stats.num_frames_sent++;
    agent->stats.size_sent += n;
    agent->stats.end = frame_mm_time;
#endif

    return TRUE;
}

static inline void marshall_qxl_drawable(RedChannelClient *rcc,
    SpiceMarshaller *m, DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannel *display_channel = SPICE_CONTAINEROF(rcc->channel, DisplayChannel, common.base);

    spice_assert(display_channel && rcc);
    /* allow sized frames to be streamed, even if they where replaced by another frame, since
     * newer frames might not cover sized frames completely if they are bigger */
    if ((item->stream || item->sized_stream) && red_marshall_stream_data(rcc, m, item)) {
        return;
    }
    if (!display_channel->enable_jpeg)
        red_marshall_qxl_drawable(rcc, m, dpi);
    else
        red_lossy_marshall_qxl_drawable(rcc, m, dpi);
}

static void display_channel_marshall_migrate_data_surfaces(DisplayChannelClient *dcc,
                                                           SpiceMarshaller *m,
                                                           int lossy)
{
    SpiceMarshaller *m2 = spice_marshaller_get_ptr_submarshaller(m, 0);
    uint32_t *num_surfaces_created;
    uint32_t i;

    num_surfaces_created = (uint32_t *)spice_marshaller_reserve_space(m2, sizeof(uint32_t));
    *num_surfaces_created = 0;
    for (i = 0; i < NUM_SURFACES; i++) {
        SpiceRect lossy_rect;

        if (!dcc->surface_client_created[i]) {
            continue;
        }
        spice_marshaller_add_uint32(m2, i);
        (*num_surfaces_created)++;

        if (!lossy) {
            continue;
        }
        region_extents(&dcc->surface_client_lossy_region[i], &lossy_rect);
        spice_marshaller_add_int32(m2, lossy_rect.left);
        spice_marshaller_add_int32(m2, lossy_rect.top);
        spice_marshaller_add_int32(m2, lossy_rect.right);
        spice_marshaller_add_int32(m2, lossy_rect.bottom);
    }
}

static void display_channel_marshall_migrate_data(RedChannelClient *rcc,
                                                  SpiceMarshaller *base_marshaller)
{
    DisplayChannel *display_channel;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    SpiceMigrateDataDisplay display_data = {0,};

    display_channel = SPICE_CONTAINEROF(rcc->channel, DisplayChannel, common.base);

    red_channel_client_init_send_data(rcc, SPICE_MSG_MIGRATE_DATA, NULL);
    spice_marshaller_add_uint32(base_marshaller, SPICE_MIGRATE_DATA_DISPLAY_MAGIC);
    spice_marshaller_add_uint32(base_marshaller, SPICE_MIGRATE_DATA_DISPLAY_VERSION);

    spice_assert(dcc->pixmap_cache);
    spice_assert(MIGRATE_DATA_DISPLAY_MAX_CACHE_CLIENTS == 4 &&
                 MIGRATE_DATA_DISPLAY_MAX_CACHE_CLIENTS == MAX_CACHE_CLIENTS);

    display_data.message_serial = red_channel_client_get_message_serial(rcc);
    display_data.low_bandwidth_setting = dcc->common.is_low_bandwidth;

    display_data.pixmap_cache_freezer = pixmap_cache_freeze(dcc->pixmap_cache);
    display_data.pixmap_cache_id = dcc->pixmap_cache->id;
    display_data.pixmap_cache_size = dcc->pixmap_cache->size;
    memcpy(display_data.pixmap_cache_clients, dcc->pixmap_cache->sync,
           sizeof(display_data.pixmap_cache_clients));

    spice_assert(dcc->glz_dict);
    dcc_freeze_glz(dcc);
    display_data.glz_dict_id = dcc->glz_dict->id;
    glz_enc_dictionary_get_restore_data(dcc->glz_dict->dict,
                                        &display_data.glz_dict_data,
                                        &dcc->glz_data.usr);

    /* all data besided the surfaces ref */
    spice_marshaller_add(base_marshaller,
                         (uint8_t *)&display_data, sizeof(display_data) - sizeof(uint32_t));
    display_channel_marshall_migrate_data_surfaces(dcc, base_marshaller,
                                                   display_channel->enable_jpeg);
}

static void display_channel_marshall_pixmap_sync(RedChannelClient *rcc,
                                                 SpiceMarshaller *base_marshaller)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    SpiceMsgWaitForChannels wait;
    PixmapCache *pixmap_cache;

    red_channel_client_init_send_data(rcc, SPICE_MSG_WAIT_FOR_CHANNELS, NULL);
    pixmap_cache = dcc->pixmap_cache;

    pthread_mutex_lock(&pixmap_cache->lock);

    wait.wait_count = 1;
    wait.wait_list[0].channel_type = SPICE_CHANNEL_DISPLAY;
    wait.wait_list[0].channel_id = pixmap_cache->generation_initiator.client;
    wait.wait_list[0].message_serial = pixmap_cache->generation_initiator.message;
    dcc->pixmap_cache_generation = pixmap_cache->generation;
    dcc->pending_pixmaps_sync = FALSE;

    pthread_mutex_unlock(&pixmap_cache->lock);

    spice_marshall_msg_wait_for_channels(base_marshaller, &wait);
}

static void dcc_pixmap_cache_reset(DisplayChannelClient *dcc, SpiceMsgWaitForChannels* sync_data)
{
    PixmapCache *cache = dcc->pixmap_cache;
    uint8_t wait_count;
    uint64_t serial;
    uint32_t i;

    serial = red_channel_client_get_message_serial(RED_CHANNEL_CLIENT(dcc));
    pthread_mutex_lock(&cache->lock);
    pixmap_cache_clear(cache);

    dcc->pixmap_cache_generation = ++cache->generation;
    cache->generation_initiator.client = dcc->common.id;
    cache->generation_initiator.message = serial;
    cache->sync[dcc->common.id] = serial;

    wait_count = 0;
    for (i = 0; i < MAX_CACHE_CLIENTS; i++) {
        if (cache->sync[i] && i != dcc->common.id) {
            sync_data->wait_list[wait_count].channel_type = SPICE_CHANNEL_DISPLAY;
            sync_data->wait_list[wait_count].channel_id = i;
            sync_data->wait_list[wait_count++].message_serial = cache->sync[i];
        }
    }
    sync_data->wait_count = wait_count;
    pthread_mutex_unlock(&cache->lock);
}

static void display_channel_marshall_reset_cache(RedChannelClient *rcc,
                                                 SpiceMarshaller *base_marshaller)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    SpiceMsgWaitForChannels wait;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_INVAL_ALL_PIXMAPS, NULL);
    dcc_pixmap_cache_reset(dcc, &wait);

    spice_marshall_msg_display_inval_all_pixmaps(base_marshaller,
                                                 &wait);
}

static void red_marshall_image(RedChannelClient *rcc, SpiceMarshaller *m, ImageItem *item)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    SpiceImage red_image;
    RedWorker *worker;
    SpiceBitmap bitmap;
    SpiceChunks *chunks;
    QRegion *surface_lossy_region;
    int comp_succeeded;
    int lossy_comp = FALSE;
    int lz_comp = FALSE;
    spice_image_compression_t comp_mode;
    SpiceMsgDisplayDrawCopy copy;
    SpiceMarshaller *src_bitmap_out, *mask_bitmap_out;
    SpiceMarshaller *bitmap_palette_out, *lzplt_palette_out;

    spice_assert(rcc && display_channel && item);
    worker = display_channel->common.worker;

    QXL_SET_IMAGE_ID(&red_image, QXL_IMAGE_GROUP_RED, ++worker->bits_unique);
    red_image.descriptor.type = SPICE_IMAGE_TYPE_BITMAP;
    red_image.descriptor.flags = item->image_flags;
    red_image.descriptor.width = item->width;
    red_image.descriptor.height = item->height;

    bitmap.format = item->image_format;
    bitmap.flags = 0;
    if (item->top_down) {
        bitmap.flags |= SPICE_BITMAP_FLAGS_TOP_DOWN;
    }
    bitmap.x = item->width;
    bitmap.y = item->height;
    bitmap.stride = item->stride;
    bitmap.palette = 0;
    bitmap.palette_id = 0;

    chunks = spice_chunks_new_linear(item->data, bitmap.stride * bitmap.y);
    bitmap.data = chunks;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_COPY, &item->link);

    copy.base.surface_id = item->surface_id;
    copy.base.box.left = item->pos.x;
    copy.base.box.top = item->pos.y;
    copy.base.box.right = item->pos.x + bitmap.x;
    copy.base.box.bottom = item->pos.y + bitmap.y;
    copy.base.clip.type = SPICE_CLIP_TYPE_NONE;
    copy.data.rop_descriptor = SPICE_ROPD_OP_PUT;
    copy.data.src_area.left = 0;
    copy.data.src_area.top = 0;
    copy.data.src_area.right = bitmap.x;
    copy.data.src_area.bottom = bitmap.y;
    copy.data.scale_mode = 0;
    copy.data.src_bitmap = 0;
    copy.data.mask.flags = 0;
    copy.data.mask.flags = 0;
    copy.data.mask.pos.x = 0;
    copy.data.mask.pos.y = 0;
    copy.data.mask.bitmap = 0;

    spice_marshall_msg_display_draw_copy(m, &copy,
                                         &src_bitmap_out, &mask_bitmap_out);

    compress_send_data_t comp_send_data = {0};

    comp_mode = dcc->image_compression;

    if (((comp_mode == SPICE_IMAGE_COMPRESS_AUTO_LZ) ||
        (comp_mode == SPICE_IMAGE_COMPRESS_AUTO_GLZ)) && !bitmap_has_extra_stride(&bitmap)) {

        if (bitmap_fmt_has_graduality(item->image_format)) {
            BitmapGradualType grad_level;

            grad_level = bitmap_get_graduality_level(&bitmap);
            if (grad_level == BITMAP_GRADUAL_HIGH) {
                // if we use lz for alpha, the stride can't be extra
                lossy_comp = display_channel->enable_jpeg && item->can_lossy;
            } else {
                lz_comp = TRUE;
            }
        } else {
            lz_comp = TRUE;
        }
    }

    if (lossy_comp) {
        comp_succeeded = dcc_compress_image_jpeg(dcc, &red_image,
                                                 &bitmap, &comp_send_data,
                                                 worker->mem_slots.internal_groupslot_id);
    } else {
        if (!lz_comp) {
            comp_succeeded = dcc_compress_image_quic(dcc, &red_image, &bitmap,
                                                     &comp_send_data,
                                                     worker->mem_slots.internal_groupslot_id);
        } else {
            comp_succeeded = dcc_compress_image_lz(dcc, &red_image, &bitmap,
                                                   &comp_send_data,
                                                   worker->mem_slots.internal_groupslot_id);
        }
    }

    surface_lossy_region = &dcc->surface_client_lossy_region[item->surface_id];
    if (comp_succeeded) {
        spice_marshall_Image(src_bitmap_out, &red_image,
                             &bitmap_palette_out, &lzplt_palette_out);

        marshaller_add_compressed(src_bitmap_out,
                                  comp_send_data.comp_buf, comp_send_data.comp_buf_size);

        if (lzplt_palette_out && comp_send_data.lzplt_palette) {
            spice_marshall_Palette(lzplt_palette_out, comp_send_data.lzplt_palette);
        }

        if (lossy_comp) {
            region_add(surface_lossy_region, &copy.base.box);
        } else {
            region_remove(surface_lossy_region, &copy.base.box);
        }
    } else {
        red_image.descriptor.type = SPICE_IMAGE_TYPE_BITMAP;
        red_image.u.bitmap = bitmap;

        spice_marshall_Image(src_bitmap_out, &red_image,
                             &bitmap_palette_out, &lzplt_palette_out);
        spice_marshaller_add_ref(src_bitmap_out, item->data,
                                 bitmap.y * bitmap.stride);
        region_remove(surface_lossy_region, &copy.base.box);
    }
    spice_chunks_destroy(chunks);
}

static void red_display_marshall_upgrade(RedChannelClient *rcc, SpiceMarshaller *m,
                                         UpgradeItem *item)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *red_drawable;
    SpiceMsgDisplayDrawCopy copy;
    SpiceMarshaller *src_bitmap_out, *mask_bitmap_out;

    spice_assert(rcc && rcc->channel && item && item->drawable);
    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_COPY, &item->base);

    red_drawable = item->drawable->red_drawable;
    spice_assert(red_drawable->type == QXL_DRAW_COPY);
    spice_assert(red_drawable->u.copy.rop_descriptor == SPICE_ROPD_OP_PUT);
    spice_assert(red_drawable->u.copy.mask.bitmap == 0);

    copy.base.surface_id = 0;
    copy.base.box = red_drawable->bbox;
    copy.base.clip.type = SPICE_CLIP_TYPE_RECTS;
    copy.base.clip.rects = item->rects;
    copy.data = red_drawable->u.copy;

    spice_marshall_msg_display_draw_copy(m, &copy,
                                         &src_bitmap_out, &mask_bitmap_out);

    fill_bits(dcc, src_bitmap_out, copy.data.src_bitmap, item->drawable, FALSE);
}

static void red_display_marshall_stream_start(RedChannelClient *rcc,
                     SpiceMarshaller *base_marshaller, StreamAgent *agent)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    Stream *stream = agent->stream;

    agent->last_send_time = 0;
    spice_assert(stream);
    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_STREAM_CREATE, NULL);
    SpiceMsgDisplayStreamCreate stream_create;
    SpiceClipRects clip_rects;

    stream_create.surface_id = 0;
    stream_create.id = get_stream_id(DCC_TO_DC(dcc), stream);
    stream_create.flags = stream->top_down ? SPICE_STREAM_FLAGS_TOP_DOWN : 0;
    stream_create.codec_type = SPICE_VIDEO_CODEC_TYPE_MJPEG;

    stream_create.src_width = stream->width;
    stream_create.src_height = stream->height;
    stream_create.stream_width = stream_create.src_width;
    stream_create.stream_height = stream_create.src_height;
    stream_create.dest = stream->dest_area;

    if (stream->current) {
        RedDrawable *red_drawable = stream->current->red_drawable;
        stream_create.clip = red_drawable->clip;
    } else {
        stream_create.clip.type = SPICE_CLIP_TYPE_RECTS;
        clip_rects.num_rects = 0;
        stream_create.clip.rects = &clip_rects;
    }

    stream_create.stamp = 0;

    spice_marshall_msg_display_stream_create(base_marshaller, &stream_create);
}

static void red_display_marshall_stream_clip(RedChannelClient *rcc,
                                         SpiceMarshaller *base_marshaller,
                                         StreamClipItem *item)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    StreamAgent *agent = item->stream_agent;

    spice_assert(agent->stream);

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_STREAM_CLIP, &item->base);
    SpiceMsgDisplayStreamClip stream_clip;

    stream_clip.id = get_stream_id(DCC_TO_DC(dcc), agent->stream);
    stream_clip.clip.type = item->clip_type;
    stream_clip.clip.rects = item->rects;

    spice_marshall_msg_display_stream_clip(base_marshaller, &stream_clip);
}

static void red_display_marshall_stream_end(RedChannelClient *rcc,
                   SpiceMarshaller *base_marshaller, StreamAgent* agent)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    SpiceMsgDisplayStreamDestroy destroy;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_STREAM_DESTROY, NULL);
    destroy.id = get_stream_id(DCC_TO_DC(dcc), agent->stream);
    red_display_stream_agent_stop(dcc, agent);
    spice_marshall_msg_display_stream_destroy(base_marshaller, &destroy);
}

static void red_marshall_surface_create(RedChannelClient *rcc,
    SpiceMarshaller *base_marshaller, SpiceMsgSurfaceCreate *surface_create)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);

    region_init(&dcc->surface_client_lossy_region[surface_create->surface_id]);
    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_SURFACE_CREATE, NULL);

    spice_marshall_msg_display_surface_create(base_marshaller, surface_create);
}

static void red_marshall_surface_destroy(RedChannelClient *rcc,
       SpiceMarshaller *base_marshaller, uint32_t surface_id)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    SpiceMsgSurfaceDestroy surface_destroy;

    region_destroy(&dcc->surface_client_lossy_region[surface_id]);
    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_SURFACE_DESTROY, NULL);

    surface_destroy.surface_id = surface_id;

    spice_marshall_msg_display_surface_destroy(base_marshaller, &surface_destroy);
}

static void red_marshall_monitors_config(RedChannelClient *rcc, SpiceMarshaller *base_marshaller,
                                         MonitorsConfig *monitors_config)
{
    int heads_size = sizeof(SpiceHead) * monitors_config->count;
    int i;
    SpiceMsgDisplayMonitorsConfig *msg = spice_malloc0(sizeof(*msg) + heads_size);
    int count = 0; // ignore monitors_config->count, it may contain zero width monitors, remove them now

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_MONITORS_CONFIG, NULL);
    for (i = 0 ; i < monitors_config->count; ++i) {
        if (monitors_config->heads[i].width == 0 || monitors_config->heads[i].height == 0) {
            continue;
        }
        msg->heads[count].id = monitors_config->heads[i].id;
        msg->heads[count].surface_id = monitors_config->heads[i].surface_id;
        msg->heads[count].width = monitors_config->heads[i].width;
        msg->heads[count].height = monitors_config->heads[i].height;
        msg->heads[count].x = monitors_config->heads[i].x;
        msg->heads[count].y = monitors_config->heads[i].y;
        count++;
    }
    msg->count = count;
    msg->max_allowed = monitors_config->max_allowed;
    spice_marshall_msg_display_monitors_config(base_marshaller, msg);
    free(msg);
}

static void red_marshall_stream_activate_report(RedChannelClient *rcc,
                                                SpiceMarshaller *base_marshaller,
                                                uint32_t stream_id)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    StreamAgent *agent = &dcc->stream_agents[stream_id];
    SpiceMsgDisplayStreamActivateReport msg;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_STREAM_ACTIVATE_REPORT, NULL);
    msg.stream_id = stream_id;
    msg.unique_id = agent->report_id;
    msg.max_window_size = RED_STREAM_CLIENT_REPORT_WINDOW;
    msg.timeout_ms = RED_STREAM_CLIENT_REPORT_TIMEOUT;
    spice_marshall_msg_display_stream_activate_report(base_marshaller, &msg);
}

static void display_channel_send_item(RedChannelClient *rcc, PipeItem *pipe_item)
{
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);

    red_display_reset_send_data(dcc);
    switch (pipe_item->type) {
    case PIPE_ITEM_TYPE_DRAW: {
        DrawablePipeItem *dpi = SPICE_CONTAINEROF(pipe_item, DrawablePipeItem, dpi_pipe_item);
        marshall_qxl_drawable(rcc, m, dpi);
        break;
    }
    case PIPE_ITEM_TYPE_STREAM_CREATE: {
        StreamAgent *agent = SPICE_CONTAINEROF(pipe_item, StreamAgent, create_item);
        red_display_marshall_stream_start(rcc, m, agent);
        break;
    }
    case PIPE_ITEM_TYPE_STREAM_CLIP: {
        StreamClipItem* clip_item = (StreamClipItem *)pipe_item;
        red_display_marshall_stream_clip(rcc, m, clip_item);
        break;
    }
    case PIPE_ITEM_TYPE_STREAM_DESTROY: {
        StreamAgent *agent = SPICE_CONTAINEROF(pipe_item, StreamAgent, destroy_item);
        red_display_marshall_stream_end(rcc, m, agent);
        break;
    }
    case PIPE_ITEM_TYPE_UPGRADE:
        red_display_marshall_upgrade(rcc, m, (UpgradeItem *)pipe_item);
        break;
    case PIPE_ITEM_TYPE_VERB:
        red_marshall_verb(rcc, (VerbItem*)pipe_item);
        break;
    case PIPE_ITEM_TYPE_MIGRATE_DATA:
        display_channel_marshall_migrate_data(rcc, m);
        break;
    case PIPE_ITEM_TYPE_IMAGE:
        red_marshall_image(rcc, m, (ImageItem *)pipe_item);
        break;
    case PIPE_ITEM_TYPE_PIXMAP_SYNC:
        display_channel_marshall_pixmap_sync(rcc, m);
        break;
    case PIPE_ITEM_TYPE_PIXMAP_RESET:
        display_channel_marshall_reset_cache(rcc, m);
        break;
    case PIPE_ITEM_TYPE_INVAL_PALLET_CACHE:
        dcc_palette_cache_reset(dcc);
        red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_INVAL_ALL_PALETTES, NULL);
        break;
    case PIPE_ITEM_TYPE_CREATE_SURFACE: {
        SurfaceCreateItem *surface_create = SPICE_CONTAINEROF(pipe_item, SurfaceCreateItem,
                                                              pipe_item);
        red_marshall_surface_create(rcc, m, &surface_create->surface_create);
        break;
    }
    case PIPE_ITEM_TYPE_DESTROY_SURFACE: {
        SurfaceDestroyItem *surface_destroy = SPICE_CONTAINEROF(pipe_item, SurfaceDestroyItem,
                                                                pipe_item);
        red_marshall_surface_destroy(rcc, m, surface_destroy->surface_destroy.surface_id);
        break;
    }
    case PIPE_ITEM_TYPE_MONITORS_CONFIG: {
        MonitorsConfigItem *monconf_item = SPICE_CONTAINEROF(pipe_item,
                                                             MonitorsConfigItem, pipe_item);
        red_marshall_monitors_config(rcc, m, monconf_item->monitors_config);
        break;
    }
    case PIPE_ITEM_TYPE_STREAM_ACTIVATE_REPORT: {
        StreamActivateReportItem *report_item = SPICE_CONTAINEROF(pipe_item,
                                                                  StreamActivateReportItem,
                                                                  pipe_item);
        red_marshall_stream_activate_report(rcc, m, report_item->stream_id);
        break;
    }
    default:
        spice_error("invalid pipe item type");
    }

    display_channel_client_release_item_before_push(dcc, pipe_item);

    // a message is pending
    if (red_channel_client_send_message_pending(rcc)) {
        display_begin_send_message(rcc);
    }
}

static inline void red_push(RedWorker *worker)
{
    if (worker->cursor_channel) {
        red_channel_push(RED_CHANNEL(worker->cursor_channel));
    }
    if (worker->display_channel) {
        red_channel_push(RED_CHANNEL(worker->display_channel));
    }
}

static void display_channel_client_on_disconnect(RedChannelClient *rcc)
{
    DisplayChannel *display;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    CommonChannel *common;
    RedWorker *worker;

    if (!rcc) {
        return;
    }
    spice_info(NULL);
    common = SPICE_CONTAINEROF(rcc->channel, CommonChannel, base);
    worker = common->worker;
    display = (DisplayChannel *)rcc->channel;
    spice_assert(display == worker->display_channel);
    display_channel_compress_stats_print(display);
    pixmap_cache_unref(dcc->pixmap_cache);
    dcc->pixmap_cache = NULL;
    dcc_release_glz(dcc);
    dcc_palette_cache_reset(dcc);
    free(dcc->send_data.stream_outbuf);
    free(dcc->send_data.free_list.res);
    dcc_destroy_stream_agents(dcc);

    // this was the last channel client
#if FIXME
    spice_debug("#draw=%d, #red_draw=%d, #glz_draw=%d",
                display->drawable_count, worker->red_drawable_count,
                worker->glz_drawable_count);
#endif
}

void red_disconnect_all_display_TODO_remove_me(RedChannel *channel)
{
    // TODO: we need to record the client that actually causes the timeout. So
    // we need to check the locations of the various pipe heads when counting,
    // and disconnect only those/that.
    if (!channel) {
        return;
    }
    red_channel_apply_clients(channel, red_channel_client_disconnect);
}

static void detach_and_stop_streams(DisplayChannel *display)
{
    RingItem *stream_item;

    spice_debug(NULL);
    while ((stream_item = ring_get_head(&display->streams))) {
        Stream *stream = SPICE_CONTAINEROF(stream_item, Stream, link);

        detach_stream_gracefully(display, stream, NULL);
        stream_stop(display, stream);
    }
}

static void red_migrate_display(DisplayChannel *display, RedChannelClient *rcc)
{
    /* We need to stop the streams, and to send upgrade_items to the client.
     * Otherwise, (1) the client might display lossy regions that we don't track
     * (streams are not part of the migration data) (2) streams_timeout may occur
     * after the MIGRATE message has been sent. This can result in messages
     * being sent to the client after MSG_MIGRATE and before MSG_MIGRATE_DATA (e.g.,
     * STREAM_CLIP, STREAM_DESTROY, DRAW_COPY)
     * No message besides MSG_MIGRATE_DATA should be sent after MSG_MIGRATE.
     * Notice that detach_and_stop_streams won't lead to any dev ram changes, since
     * handle_dev_stop already took care of releasing all the dev ram resources.
     */
    detach_and_stop_streams(display);
    if (red_channel_client_is_connected(rcc)) {
        red_channel_client_default_migrate(rcc);
    }
}

#ifdef USE_OPENGL
static SpiceCanvas *create_ogl_context_common(DisplayChannel *display, OGLCtx *ctx,
                                              uint32_t width, uint32_t height,
                                              int32_t stride, uint8_t depth)
{
    SpiceCanvas *canvas;

    oglctx_make_current(ctx);
    if (!(canvas = gl_canvas_create(width, height, depth, &display->image_cache.base,
                                    &display->image_surfaces, NULL, NULL, NULL))) {
        return NULL;
    }

    spice_canvas_set_usr_data(canvas, ctx, (spice_destroy_fn_t)oglctx_destroy);

    canvas->ops->clear(canvas);

    return canvas;
}

static SpiceCanvas *create_ogl_pbuf_context(DisplayChannel *display, uint32_t width,
                                            uint32_t height, int32_t stride, uint8_t depth)
{
    OGLCtx *ctx;
    SpiceCanvas *canvas;

    if (!(ctx = pbuf_create(width, height))) {
        return NULL;
    }

    if (!(canvas = create_ogl_context_common(display, ctx, width, height, stride, depth))) {
        oglctx_destroy(ctx);
        return NULL;
    }

    return canvas;
}

static SpiceCanvas *create_ogl_pixmap_context(DisplayChannel *display, uint32_t width,
                                              uint32_t height, int32_t stride, uint8_t depth)
{
    OGLCtx *ctx;
    SpiceCanvas *canvas;

    if (!(ctx = pixmap_create(width, height))) {
        return NULL;
    }

    if (!(canvas = create_ogl_context_common(display, ctx, width, height, stride, depth))) {
        oglctx_destroy(ctx);
        return NULL;
    }

    return canvas;
}
#endif

static inline void *create_canvas_for_surface(DisplayChannel *display, RedSurface *surface,
                                              uint32_t renderer, uint32_t width, uint32_t height,
                                              int32_t stride, uint32_t format, void *line_0)
{
    SpiceCanvas *canvas;

    switch (renderer) {
    case RED_RENDERER_SW:
        canvas = canvas_create_for_data(width, height, format,
                                        line_0, stride,
                                        &display->image_cache.base,
                                        &display->image_surfaces, NULL, NULL, NULL);
        surface->context.top_down = TRUE;
        surface->context.canvas_draws_on_surface = TRUE;
        return canvas;
#ifdef USE_OPENGL
    case RED_RENDERER_OGL_PBUF:
        canvas = create_ogl_pbuf_context(display, width, height, stride,
                                         SPICE_SURFACE_FMT_DEPTH(format));
        surface->context.top_down = FALSE;
        return canvas;
    case RED_RENDERER_OGL_PIXMAP:
        canvas = create_ogl_pixmap_context(display, width, height, stride,
                                           SPICE_SURFACE_FMT_DEPTH(format));
        surface->context.top_down = FALSE;
        return canvas;
#endif
    default:
        spice_error("invalid renderer type");
    };

    return NULL;
}

static void red_worker_create_surface_item(DisplayChannel *display, int surface_id)
{
    DisplayChannelClient *dcc;
    RingItem *item, *next;

    FOREACH_DCC(display, item, next, dcc) {
        dcc_create_surface(dcc, surface_id);
    }
}


static void red_worker_push_surface_image(DisplayChannel *display, int surface_id)
{
    DisplayChannelClient *dcc;
    RingItem *item, *next;

    FOREACH_DCC(display, item, next, dcc) {
        dcc_push_surface_image(dcc, surface_id);
    }
}

static void red_create_surface(DisplayChannel *display, uint32_t surface_id, uint32_t width,
                               uint32_t height, int32_t stride, uint32_t format,
                               void *line_0, int data_is_valid, int send_client)
{
    RedSurface *surface = &display->surfaces[surface_id];
    uint32_t i;

    spice_warn_if(surface->context.canvas);

    surface->context.canvas_draws_on_surface = FALSE;
    surface->context.width = width;
    surface->context.height = height;
    surface->context.format = format;
    surface->context.stride = stride;
    surface->context.line_0 = line_0;
    if (!data_is_valid) {
        memset((char *)line_0 + (int32_t)(stride * (height - 1)), 0, height*abs(stride));
    }
    surface->create.info = NULL;
    surface->destroy.info = NULL;
    ring_init(&surface->current);
    ring_init(&surface->current_list);
    ring_init(&surface->depend_on_me);
    region_init(&surface->draw_dirty_region);
    surface->refs = 1;
    if (display->renderer != RED_RENDERER_INVALID) {
        surface->context.canvas = create_canvas_for_surface(display, surface, display->renderer,
                                                            width, height, stride,
                                                            surface->context.format, line_0);
        if (!surface->context.canvas) {
            spice_critical("drawing canvas creating failed - can`t create same type canvas");
        }

        if (send_client) {
            red_worker_create_surface_item(display, surface_id);
            if (data_is_valid) {
                red_worker_push_surface_image(display, surface_id);
            }
        }
        return;
    }

    for (i = 0; i < display->num_renderers; i++) {
        surface->context.canvas = create_canvas_for_surface(display, surface, display->renderers[i],
                                                            width, height, stride,
                                                            surface->context.format, line_0);
        if (surface->context.canvas) { //no need canvas check
            display->renderer = display->renderers[i];
            if (send_client) {
                red_worker_create_surface_item(display, surface_id);
                if (data_is_valid) {
                    red_worker_push_surface_image(display, surface_id);
                }
            }
            return;
        }
    }

    spice_critical("unable to create drawing canvas");
}

static inline void flush_display_commands(RedWorker *worker)
{
    RedChannel *display_red_channel = RED_CHANNEL(worker->display_channel);

    for (;;) {
        uint64_t end_time;
        int ring_is_empty;

        red_process_commands(worker, MAX_PIPE_SIZE, &ring_is_empty);
        if (ring_is_empty) {
            break;
        }

        while (red_process_commands(worker, MAX_PIPE_SIZE, &ring_is_empty)) {
            red_channel_push(RED_CHANNEL(worker->display_channel));
        }

        if (ring_is_empty) {
            break;
        }
        end_time = red_get_monotonic_time() + DISPLAY_CLIENT_TIMEOUT;
        int sleep_count = 0;
        for (;;) {
            red_channel_push(RED_CHANNEL(worker->display_channel));
            if (!display_is_connected(worker) ||
                red_channel_max_pipe_size(display_red_channel) <= MAX_PIPE_SIZE) {
                break;
            }
            RedChannel *channel = (RedChannel *)worker->display_channel;
            red_channel_receive(channel);
            red_channel_send(channel);
            // TODO: MC: the whole timeout will break since it takes lowest timeout, should
            // do it client by client.
            if (red_get_monotonic_time() >= end_time) {
                spice_warning("update timeout");
                red_disconnect_all_display_TODO_remove_me(channel);
            } else {
                sleep_count++;
                usleep(DISPLAY_CLIENT_RETRY_INTERVAL);
            }
        }
    }
}

static inline void flush_cursor_commands(RedWorker *worker)
{
    RedChannel *cursor_red_channel = RED_CHANNEL(worker->cursor_channel);

    for (;;) {
        uint64_t end_time;
        int ring_is_empty = FALSE;

        red_process_cursor(worker, MAX_PIPE_SIZE, &ring_is_empty);
        if (ring_is_empty) {
            break;
        }

        while (red_process_cursor(worker, MAX_PIPE_SIZE, &ring_is_empty)) {
            red_channel_push(RED_CHANNEL(worker->cursor_channel));
        }

        if (ring_is_empty) {
            break;
        }
        end_time = red_get_monotonic_time() + DISPLAY_CLIENT_TIMEOUT;
        int sleep_count = 0;
        for (;;) {
            red_channel_push(RED_CHANNEL(worker->cursor_channel));
            if (!cursor_is_connected(worker)
                || red_channel_min_pipe_size(cursor_red_channel) <= MAX_PIPE_SIZE) {
                break;
            }
            RedChannel *channel = (RedChannel *)worker->cursor_channel;
            red_channel_receive(channel);
            red_channel_send(channel);
            if (red_get_monotonic_time() >= end_time) {
                spice_warning("flush cursor timeout");
                cursor_channel_disconnect(worker->cursor_channel);
                worker->cursor_channel = NULL;
            } else {
                sleep_count++;
                usleep(DISPLAY_CLIENT_RETRY_INTERVAL);
            }
        }
    }
}

// TODO: on timeout, don't disconnect all channels immediatly - try to disconnect the slowest ones
// first and maybe turn timeouts to several timeouts in order to disconnect channels gradually.
// Should use disconnect or shutdown?
static inline void flush_all_qxl_commands(RedWorker *worker)
{
    flush_display_commands(worker);
    flush_cursor_commands(worker);
}

static GlzSharedDictionary *_red_find_glz_dictionary(RedClient *client, uint8_t dict_id)
{
    RingItem *now;
    GlzSharedDictionary *ret = NULL;

    now = &glz_dictionary_list;
    while ((now = ring_next(&glz_dictionary_list, now))) {
        GlzSharedDictionary *dict = (GlzSharedDictionary *)now;
        if ((dict->client == client) && (dict->id == dict_id)) {
            ret = dict;
            break;
        }
    }

    return ret;
}

static GlzSharedDictionary *_red_create_glz_dictionary(RedClient *client, uint8_t id,
                                                       GlzEncDictContext *opaque_dict)
{
    GlzSharedDictionary *shared_dict = spice_new0(GlzSharedDictionary, 1);
    shared_dict->dict = opaque_dict;
    shared_dict->id = id;
    shared_dict->refs = 1;
    shared_dict->migrate_freeze = FALSE;
    shared_dict->client = client;
    ring_item_init(&shared_dict->base);
    pthread_rwlock_init(&shared_dict->encode_lock, NULL);
    return shared_dict;
}

static GlzSharedDictionary *red_create_glz_dictionary(DisplayChannelClient *dcc,
                                                      uint8_t id, int window_size)
{
    GlzEncDictContext *glz_dict = glz_enc_dictionary_create(window_size,
                                                            MAX_LZ_ENCODERS,
                                                            &dcc->glz_data.usr);
#ifdef COMPRESS_DEBUG
    spice_info("Lz Window %d Size=%d", id, window_size);
#endif
    if (!glz_dict) {
        spice_critical("failed creating lz dictionary");
        return NULL;
    }
    return _red_create_glz_dictionary(RED_CHANNEL_CLIENT(dcc)->client, id, glz_dict);
}

static GlzSharedDictionary *red_create_restored_glz_dictionary(DisplayChannelClient *dcc,
                                                               uint8_t id,
                                                               GlzEncDictRestoreData *restore_data)
{
    GlzEncDictContext *glz_dict = glz_enc_dictionary_restore(restore_data,
                                                             &dcc->glz_data.usr);
    if (!glz_dict) {
        spice_critical("failed creating lz dictionary");
        return NULL;
    }
    return _red_create_glz_dictionary(RED_CHANNEL_CLIENT(dcc)->client, id, glz_dict);
}

static GlzSharedDictionary *red_get_glz_dictionary(DisplayChannelClient *dcc,
                                                   uint8_t id, int window_size)
{
    GlzSharedDictionary *shared_dict = NULL;

    pthread_mutex_lock(&glz_dictionary_list_lock);

    shared_dict = _red_find_glz_dictionary(RED_CHANNEL_CLIENT(dcc)->client, id);

    if (!shared_dict) {
        shared_dict = red_create_glz_dictionary(dcc, id, window_size);
        ring_add(&glz_dictionary_list, &shared_dict->base);
    } else {
        shared_dict->refs++;
    }
    pthread_mutex_unlock(&glz_dictionary_list_lock);
    return shared_dict;
}

static GlzSharedDictionary *red_restore_glz_dictionary(DisplayChannelClient *dcc,
                                                       uint8_t id,
                                                       GlzEncDictRestoreData *restore_data)
{
    GlzSharedDictionary *shared_dict = NULL;

    pthread_mutex_lock(&glz_dictionary_list_lock);

    shared_dict = _red_find_glz_dictionary(RED_CHANNEL_CLIENT(dcc)->client, id);

    if (!shared_dict) {
        shared_dict = red_create_restored_glz_dictionary(dcc, id, restore_data);
        ring_add(&glz_dictionary_list, &shared_dict->base);
    } else {
        shared_dict->refs++;
    }
    pthread_mutex_unlock(&glz_dictionary_list_lock);
    return shared_dict;
}

/* destroy encoder, and dictionary if no one uses it*/
static void dcc_release_glz(DisplayChannelClient *dcc)
{
    GlzSharedDictionary *shared_dict;

    dcc_free_glz_drawables(dcc);

    glz_encoder_destroy(dcc->glz);
    dcc->glz = NULL;

    if (!(shared_dict = dcc->glz_dict)) {
        return;
    }

    dcc->glz_dict = NULL;
    pthread_mutex_lock(&glz_dictionary_list_lock);
    if (--shared_dict->refs) {
        pthread_mutex_unlock(&glz_dictionary_list_lock);
        return;
    }
    ring_remove(&shared_dict->base);
    pthread_mutex_unlock(&glz_dictionary_list_lock);
    glz_enc_dictionary_destroy(shared_dict->dict, &dcc->glz_data.usr);
    free(shared_dict);
}

static int display_channel_init_cache(DisplayChannelClient *dcc, SpiceMsgcDisplayInit *init_info)
{
    spice_assert(!dcc->pixmap_cache);
    return !!(dcc->pixmap_cache = pixmap_cache_get(RED_CHANNEL_CLIENT(dcc)->client,
                                                   init_info->pixmap_cache_id,
                                                   init_info->pixmap_cache_size));
}

static int display_channel_init_glz_dictionary(DisplayChannelClient *dcc,
                                               SpiceMsgcDisplayInit *init_info)
{
    spice_assert(!dcc->glz_dict);
    ring_init(&dcc->glz_drawables);
    ring_init(&dcc->glz_drawables_inst_to_free);
    pthread_mutex_init(&dcc->glz_drawables_inst_to_free_lock, NULL);
    return !!(dcc->glz_dict = red_get_glz_dictionary(dcc,
                                                     init_info->glz_dictionary_id,
                                                     init_info->glz_dictionary_window_size));
}

static int display_channel_init(DisplayChannelClient *dcc, SpiceMsgcDisplayInit *init_info)
{
    return (display_channel_init_cache(dcc, init_info) &&
            display_channel_init_glz_dictionary(dcc, init_info));
}

static int display_channel_handle_migrate_glz_dictionary(DisplayChannelClient *dcc,
                                                         SpiceMigrateDataDisplay *migrate_info)
{
    spice_assert(!dcc->glz_dict);
    ring_init(&dcc->glz_drawables);
    ring_init(&dcc->glz_drawables_inst_to_free);
    pthread_mutex_init(&dcc->glz_drawables_inst_to_free_lock, NULL);
    return !!(dcc->glz_dict = red_restore_glz_dictionary(dcc,
                                                         migrate_info->glz_dict_id,
                                                         &migrate_info->glz_dict_data));
}

static int display_channel_handle_migrate_mark(RedChannelClient *rcc)
{
    DisplayChannel *display_channel = SPICE_CONTAINEROF(rcc->channel, DisplayChannel, common.base);
    RedChannel *channel = RED_CHANNEL(display_channel);

    red_channel_pipes_add_type(channel, PIPE_ITEM_TYPE_MIGRATE_DATA);
    return TRUE;
}

static uint64_t display_channel_handle_migrate_data_get_serial(
                RedChannelClient *rcc, uint32_t size, void *message)
{
    SpiceMigrateDataDisplay *migrate_data;

    migrate_data = (SpiceMigrateDataDisplay *)((uint8_t *)message + sizeof(SpiceMigrateDataHeader));

    return migrate_data->message_serial;
}

static int display_channel_client_restore_surface(DisplayChannelClient *dcc, uint32_t surface_id)
{
    /* we don't process commands till we receive the migration data, thus,
     * we should have not sent any surface to the client. */
    if (dcc->surface_client_created[surface_id]) {
        spice_warning("surface %u is already marked as client_created", surface_id);
        return FALSE;
    }
    dcc->surface_client_created[surface_id] = TRUE;
    return TRUE;
}

static int display_channel_client_restore_surfaces_lossless(DisplayChannelClient *dcc,
                                                            MigrateDisplaySurfacesAtClientLossless *mig_surfaces)
{
    uint32_t i;

    spice_debug(NULL);
    for (i = 0; i < mig_surfaces->num_surfaces; i++) {
        uint32_t surface_id = mig_surfaces->surfaces[i].id;

        if (!display_channel_client_restore_surface(dcc, surface_id)) {
            return FALSE;
        }
    }
    return TRUE;
}

static int display_channel_client_restore_surfaces_lossy(DisplayChannelClient *dcc,
                                                          MigrateDisplaySurfacesAtClientLossy *mig_surfaces)
{
    uint32_t i;

    spice_debug(NULL);
    for (i = 0; i < mig_surfaces->num_surfaces; i++) {
        uint32_t surface_id = mig_surfaces->surfaces[i].id;
        SpiceMigrateDataRect *mig_lossy_rect;
        SpiceRect lossy_rect;

        if (!display_channel_client_restore_surface(dcc, surface_id)) {
            return FALSE;
        }
        spice_assert(dcc->surface_client_created[surface_id]);

        mig_lossy_rect = &mig_surfaces->surfaces[i].lossy_rect;
        lossy_rect.left = mig_lossy_rect->left;
        lossy_rect.top = mig_lossy_rect->top;
        lossy_rect.right = mig_lossy_rect->right;
        lossy_rect.bottom = mig_lossy_rect->bottom;
        region_init(&dcc->surface_client_lossy_region[surface_id]);
        region_add(&dcc->surface_client_lossy_region[surface_id], &lossy_rect);
    }
    return TRUE;
}
static int display_channel_handle_migrate_data(RedChannelClient *rcc, uint32_t size,
                                               void *message)
{
    SpiceMigrateDataHeader *header;
    SpiceMigrateDataDisplay *migrate_data;
    DisplayChannel *display_channel = SPICE_CONTAINEROF(rcc->channel, DisplayChannel, common.base);
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    uint8_t *surfaces;
    int surfaces_restored = FALSE;
    int i;

    spice_debug(NULL);
    if (size < sizeof(*migrate_data) + sizeof(SpiceMigrateDataHeader)) {
        spice_error("bad message size");
        return FALSE;
    }
    header = (SpiceMigrateDataHeader *)message;
    migrate_data = (SpiceMigrateDataDisplay *)(header + 1);
    if (!migration_protocol_validate_header(header,
                                            SPICE_MIGRATE_DATA_DISPLAY_MAGIC,
                                            SPICE_MIGRATE_DATA_DISPLAY_VERSION)) {
        spice_error("bad header");
        return FALSE;
    }
    /* size is set to -1 in order to keep the cache frozen until the original
     * channel client that froze the cache on the src size receives the migrate
     * data and unfreezes the cache by setting its size > 0 and by triggering
     * pixmap_cache_reset */
    dcc->pixmap_cache = pixmap_cache_get(RED_CHANNEL_CLIENT(dcc)->client,
                                         migrate_data->pixmap_cache_id, -1);
    if (!dcc->pixmap_cache) {
        return FALSE;
    }
    pthread_mutex_lock(&dcc->pixmap_cache->lock);
    for (i = 0; i < MAX_CACHE_CLIENTS; i++) {
        dcc->pixmap_cache->sync[i] = MAX(dcc->pixmap_cache->sync[i],
                                         migrate_data->pixmap_cache_clients[i]);
    }
    pthread_mutex_unlock(&dcc->pixmap_cache->lock);

    if (migrate_data->pixmap_cache_freezer) {
        /* activating the cache. The cache will start to be active after
         * pixmap_cache_reset is called, when handling PIPE_ITEM_TYPE_PIXMAP_RESET */
        dcc->pixmap_cache->size = migrate_data->pixmap_cache_size;
        red_channel_client_pipe_add_type(rcc,
                                         PIPE_ITEM_TYPE_PIXMAP_RESET);
    }

    if (display_channel_handle_migrate_glz_dictionary(dcc, migrate_data)) {
        dcc->glz = glz_encoder_create(dcc->common.id,
                                      dcc->glz_dict->dict, &dcc->glz_data.usr);
        if (!dcc->glz) {
            spice_critical("create global lz failed");
        }
    } else {
        spice_critical("restoring global lz dictionary failed");
    }

    dcc->common.is_low_bandwidth = migrate_data->low_bandwidth_setting;

    if (migrate_data->low_bandwidth_setting) {
        red_channel_client_ack_set_client_window(rcc, WIDE_CLIENT_ACK_WINDOW);
        if (dcc->jpeg_state == SPICE_WAN_COMPRESSION_AUTO) {
            display_channel->enable_jpeg = TRUE;
        }
        if (dcc->zlib_glz_state == SPICE_WAN_COMPRESSION_AUTO) {
            display_channel->enable_zlib_glz_wrap = TRUE;
        }
    }

    surfaces = (uint8_t *)message + migrate_data->surfaces_at_client_ptr;
    if (display_channel->enable_jpeg) {
        surfaces_restored = display_channel_client_restore_surfaces_lossy(dcc,
                                (MigrateDisplaySurfacesAtClientLossy *)surfaces);
    } else {
        surfaces_restored = display_channel_client_restore_surfaces_lossless(dcc,
                                (MigrateDisplaySurfacesAtClientLossless*)surfaces);
    }

    if (!surfaces_restored) {
        return FALSE;
    }
    red_channel_client_pipe_add_type(rcc, PIPE_ITEM_TYPE_INVAL_PALLET_CACHE);
    /* enable sending messages */
    red_channel_client_ack_zero_messages_window(rcc);
    return TRUE;
}

static int display_channel_handle_stream_report(DisplayChannelClient *dcc,
                                                SpiceMsgcDisplayStreamReport *stream_report)
{
    StreamAgent *stream_agent;

    if (stream_report->stream_id >= NUM_STREAMS) {
        spice_warning("stream_report: invalid stream id %u", stream_report->stream_id);
        return FALSE;
    }
    stream_agent = &dcc->stream_agents[stream_report->stream_id];
    if (!stream_agent->mjpeg_encoder) {
        spice_info("stream_report: no encoder for stream id %u."
                    "Probably the stream has been destroyed", stream_report->stream_id);
        return TRUE;
    }

    if (stream_report->unique_id != stream_agent->report_id) {
        spice_warning("local reoprt-id (%u) != msg report-id (%u)",
                      stream_agent->report_id, stream_report->unique_id);
        return TRUE;
    }
    mjpeg_encoder_client_stream_report(stream_agent->mjpeg_encoder,
                                       stream_report->num_frames,
                                       stream_report->num_drops,
                                       stream_report->start_frame_mm_time,
                                       stream_report->end_frame_mm_time,
                                       stream_report->last_frame_delay,
                                       stream_report->audio_delay);
    return TRUE;
}

static int display_channel_handle_message(RedChannelClient *rcc, uint32_t size, uint16_t type,
                                          void *message)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);

    switch (type) {
    case SPICE_MSGC_DISPLAY_INIT:
        if (!dcc->expect_init) {
            spice_warning("unexpected SPICE_MSGC_DISPLAY_INIT");
            return FALSE;
        }
        dcc->expect_init = FALSE;
        return display_channel_init(dcc, (SpiceMsgcDisplayInit *)message);
    case SPICE_MSGC_DISPLAY_STREAM_REPORT:
        return display_channel_handle_stream_report(dcc,
                                                    (SpiceMsgcDisplayStreamReport *)message);
    default:
        return red_channel_client_handle_message(rcc, size, type, message);
    }
}

static int common_channel_config_socket(RedChannelClient *rcc)
{
    RedClient *client = red_channel_client_get_client(rcc);
    MainChannelClient *mcc = red_client_get_main(client);
    RedsStream *stream = red_channel_client_get_stream(rcc);
    CommonChannelClient *ccc = COMMON_CHANNEL_CLIENT(rcc);
    int flags;
    int delay_val;

    if ((flags = fcntl(stream->socket, F_GETFL)) == -1) {
        spice_warning("accept failed, %s", strerror(errno));
        return FALSE;
    }

    if (fcntl(stream->socket, F_SETFL, flags | O_NONBLOCK) == -1) {
        spice_warning("accept failed, %s", strerror(errno));
        return FALSE;
    }

    // TODO - this should be dynamic, not one time at channel creation
    ccc->is_low_bandwidth = main_channel_client_is_low_bandwidth(mcc);
    delay_val = ccc->is_low_bandwidth ? 0 : 1;
    /* FIXME: Using Nagle's Algorithm can lead to apparent delays, depending
     * on the delayed ack timeout on the other side.
     * Instead of using Nagle's, we need to implement message buffering on
     * the application level.
     * see: http://www.stuartcheshire.org/papers/NagleDelayedAck/
     */
    if (setsockopt(stream->socket, IPPROTO_TCP, TCP_NODELAY, &delay_val,
                   sizeof(delay_val)) == -1) {
        if (errno != ENOTSUP) {
            spice_warning("setsockopt failed, %s", strerror(errno));
        }
    }
    return TRUE;
}

typedef struct SpiceTimer {
    SpiceTimerFunc func;
    void *opaque;
    guint source_id;
} SpiceTimer;

static SpiceTimer* worker_timer_add(SpiceTimerFunc func, void *opaque)
{
    SpiceTimer *timer = g_new0(SpiceTimer, 1);

    timer->func = func;
    timer->opaque = opaque;

    return timer;
}

static gboolean worker_timer_func(gpointer user_data)
{
    SpiceTimer *timer = user_data;

    timer->source_id = 0;
    timer->func(timer->opaque);
    /* timer might be free after func(), don't touch */

    return FALSE;
}

static void worker_timer_cancel(SpiceTimer *timer)
{
    if (timer->source_id == 0)
        return;

    g_source_remove(timer->source_id);
    timer->source_id = 0;
}

static void worker_timer_start(SpiceTimer *timer, uint32_t ms)
{
    worker_timer_cancel(timer);

    timer->source_id = g_timeout_add(ms, worker_timer_func, timer);
}

static void worker_timer_remove(SpiceTimer *timer)
{
    worker_timer_cancel(timer);
    g_free(timer);
}

static GIOCondition spice_event_to_giocondition(int event_mask)
{
    GIOCondition condition = 0;

    if (event_mask & SPICE_WATCH_EVENT_READ)
        condition |= G_IO_IN;
    if (event_mask & SPICE_WATCH_EVENT_WRITE)
        condition |= G_IO_OUT;

    return condition;
}

static int giocondition_to_spice_event(GIOCondition condition)
{
    int event = 0;

    if (condition & G_IO_IN)
        event |= SPICE_WATCH_EVENT_READ;
    if (condition & G_IO_OUT)
        event |= SPICE_WATCH_EVENT_WRITE;

    return event;
}

struct SpiceWatch {
    GIOChannel *channel;
    GSource *source;
    RedChannelClient *rcc;
    SpiceWatchFunc func;
};

static gboolean watch_func(GIOChannel *source, GIOCondition condition,
                           gpointer data)
{
    SpiceWatch *watch = data;
    int fd = g_io_channel_unix_get_fd(source);

    spice_return_val_if_fail(!g_source_is_destroyed(watch->source), FALSE);

    watch->func(fd, giocondition_to_spice_event(condition), watch->rcc);

    return TRUE;
}

static void worker_watch_update_mask(SpiceWatch *watch, int events)
{
    RedWorker *worker;

    spice_return_if_fail(watch != NULL);
    worker = SPICE_CONTAINEROF(watch->rcc->channel, CommonChannel, base)->worker;

    if (watch->source) {
        g_source_destroy(watch->source);
        watch->source = NULL;
    }

    if (!events)
        return;

    watch->source = g_io_create_watch(watch->channel, spice_event_to_giocondition(events));
    g_source_set_callback(watch->source, (GSourceFunc)watch_func, watch, NULL);
    g_source_attach(watch->source, worker->main_context);
}

static SpiceWatch* worker_watch_add(int fd, int events, SpiceWatchFunc func, void *opaque)
{
    RedChannelClient *rcc = opaque;
    RedWorker *worker;
    SpiceWatch *watch;

    spice_return_val_if_fail(rcc != NULL, NULL);
    spice_return_val_if_fail(fd != -1, NULL);
    spice_return_val_if_fail(func != NULL, NULL);

    /* Since we are called from red_channel_client_create()
       CommonChannelClient->worker has not been set yet! */
    worker = SPICE_CONTAINEROF(rcc->channel, CommonChannel, base)->worker;
    spice_return_val_if_fail(worker != NULL, NULL);
    spice_return_val_if_fail(worker->main_context != NULL, NULL);

    watch = g_new0(SpiceWatch, 1);
    watch->channel = g_io_channel_unix_new(fd);
    watch->rcc = rcc;
    watch->func = func;

    worker_watch_update_mask(watch, events);

    return watch;
}

static void worker_watch_remove(SpiceWatch *watch)
{
    spice_return_if_fail(watch != NULL);

    if (watch->source)
        g_source_destroy(watch->source);

    g_io_channel_unref(watch->channel);
    g_free(watch);
}

static const SpiceCoreInterface worker_core = {
    .timer_add = worker_timer_add,
    .timer_start = worker_timer_start,
    .timer_cancel = worker_timer_cancel,
    .timer_remove = worker_timer_remove,

    .watch_update_mask = worker_watch_update_mask,
    .watch_add = worker_watch_add,
    .watch_remove = worker_watch_remove,
};

CommonChannelClient *common_channel_new_client(CommonChannel *common,
                                               int size,
                                               RedClient *client,
                                               RedsStream *stream,
                                               int mig_target,
                                               int monitor_latency,
                                               uint32_t *common_caps,
                                               int num_common_caps,
                                               uint32_t *caps,
                                               int num_caps)
{
    RedChannelClient *rcc =
        red_channel_client_create(size, &common->base, client, stream, monitor_latency,
                                  num_common_caps, common_caps, num_caps, caps);
    if (!rcc) {
        return NULL;
    }
    CommonChannelClient *common_cc = (CommonChannelClient*)rcc;
    common_cc->worker = common->worker;
    common_cc->id = common->worker->qxl->id;
    common->during_target_migrate = mig_target;

    // TODO: move wide/narrow ack setting to red_channel.
    red_channel_client_ack_set_client_window(rcc,
        common_cc->is_low_bandwidth ?
        WIDE_CLIENT_ACK_WINDOW : NARROW_CLIENT_ACK_WINDOW);
    return common_cc;
}


RedChannel *red_worker_new_channel(RedWorker *worker, int size,
                                   const char *name,
                                   uint32_t channel_type, int migration_flags,
                                   ChannelCbs *channel_cbs,
                                   channel_handle_parsed_proc handle_parsed)
{
    RedChannel *channel = NULL;
    CommonChannel *common;

    spice_return_val_if_fail(worker, NULL);
    spice_return_val_if_fail(channel_cbs, NULL);
    spice_return_val_if_fail(!channel_cbs->config_socket, NULL);
    spice_return_val_if_fail(!channel_cbs->alloc_recv_buf, NULL);
    spice_return_val_if_fail(!channel_cbs->release_recv_buf, NULL);

    channel_cbs->config_socket = common_channel_config_socket;
    channel_cbs->alloc_recv_buf = common_alloc_recv_buf;
    channel_cbs->release_recv_buf = common_release_recv_buf;

    channel = red_channel_create_parser(size, &worker_core,
                                        channel_type, worker->qxl->id,
                                        TRUE /* handle_acks */,
                                        spice_get_client_channel_parser(channel_type, NULL),
                                        handle_parsed,
                                        channel_cbs,
                                        migration_flags);
    spice_return_val_if_fail(channel, NULL);
    red_channel_set_stat_node(channel, stat_add_node(worker->stat, name, TRUE));

    common = (CommonChannel *)channel;
    common->worker = worker;
    return channel;
}

static void display_channel_hold_pipe_item(RedChannelClient *rcc, PipeItem *item)
{
    spice_assert(item);
    switch (item->type) {
    case PIPE_ITEM_TYPE_DRAW:
        drawable_pipe_item_ref(SPICE_CONTAINEROF(item, DrawablePipeItem, dpi_pipe_item));
        break;
    case PIPE_ITEM_TYPE_STREAM_CLIP:
        ((StreamClipItem *)item)->refs++;
        break;
    case PIPE_ITEM_TYPE_UPGRADE:
        ((UpgradeItem *)item)->refs++;
        break;
    case PIPE_ITEM_TYPE_IMAGE:
        ((ImageItem *)item)->refs++;
        break;
    default:
        spice_critical("invalid item type");
    }
}

static void display_channel_client_release_item_after_push(DisplayChannelClient *dcc,
                                                           PipeItem *item)
{
    DisplayChannel *display = DCC_TO_DC(dcc);

    switch (item->type) {
    case PIPE_ITEM_TYPE_DRAW:
        drawable_pipe_item_unref(SPICE_CONTAINEROF(item, DrawablePipeItem, dpi_pipe_item));
        break;
    case PIPE_ITEM_TYPE_STREAM_CLIP:
        stream_clip_item_unref(dcc, (StreamClipItem *)item);
        break;
    case PIPE_ITEM_TYPE_UPGRADE:
        upgrade_item_unref(display, (UpgradeItem *)item);
        break;
    case PIPE_ITEM_TYPE_IMAGE:
        release_image_item((ImageItem *)item);
        break;
    case PIPE_ITEM_TYPE_VERB:
        free(item);
        break;
    case PIPE_ITEM_TYPE_MONITORS_CONFIG: {
        MonitorsConfigItem *monconf_item = SPICE_CONTAINEROF(item,
                                                             MonitorsConfigItem, pipe_item);
        monitors_config_unref(monconf_item->monitors_config);
        free(item);
        break;
    }
    default:
        spice_critical("invalid item type");
    }
}

// TODO: share code between before/after_push since most of the items need the same
// release
static void display_channel_client_release_item_before_push(DisplayChannelClient *dcc,
                                                            PipeItem *item)
{
    DisplayChannel *display = DCC_TO_DC(dcc);

    switch (item->type) {
    case PIPE_ITEM_TYPE_DRAW: {
        DrawablePipeItem *dpi = SPICE_CONTAINEROF(item, DrawablePipeItem, dpi_pipe_item);
        ring_remove(&dpi->base);
        drawable_pipe_item_unref(dpi);
        break;
    }
    case PIPE_ITEM_TYPE_STREAM_CREATE: {
        StreamAgent *agent = SPICE_CONTAINEROF(item, StreamAgent, create_item);
        stream_agent_unref(display, agent);
        break;
    }
    case PIPE_ITEM_TYPE_STREAM_CLIP:
        stream_clip_item_unref(dcc, (StreamClipItem *)item);
        break;
    case PIPE_ITEM_TYPE_STREAM_DESTROY: {
        StreamAgent *agent = SPICE_CONTAINEROF(item, StreamAgent, destroy_item);
        stream_agent_unref(display, agent);
        break;
    }
    case PIPE_ITEM_TYPE_UPGRADE:
        upgrade_item_unref(display, (UpgradeItem *)item);
        break;
    case PIPE_ITEM_TYPE_IMAGE:
        release_image_item((ImageItem *)item);
        break;
    case PIPE_ITEM_TYPE_CREATE_SURFACE: {
        SurfaceCreateItem *surface_create = SPICE_CONTAINEROF(item, SurfaceCreateItem,
                                                              pipe_item);
        free(surface_create);
        break;
    }
    case PIPE_ITEM_TYPE_DESTROY_SURFACE: {
        SurfaceDestroyItem *surface_destroy = SPICE_CONTAINEROF(item, SurfaceDestroyItem,
                                                                pipe_item);
        free(surface_destroy);
        break;
    }
    case PIPE_ITEM_TYPE_MONITORS_CONFIG: {
        MonitorsConfigItem *monconf_item = SPICE_CONTAINEROF(item,
                                                             MonitorsConfigItem, pipe_item);
        monitors_config_unref(monconf_item->monitors_config);
        free(item);
        break;
    }
    case PIPE_ITEM_TYPE_VERB:
    case PIPE_ITEM_TYPE_MIGRATE_DATA:
    case PIPE_ITEM_TYPE_PIXMAP_SYNC:
    case PIPE_ITEM_TYPE_PIXMAP_RESET:
    case PIPE_ITEM_TYPE_INVAL_PALLET_CACHE:
    case PIPE_ITEM_TYPE_STREAM_ACTIVATE_REPORT:
        free(item);
        break;
    default:
        spice_critical("invalid item type");
    }
}

static void display_channel_release_item(RedChannelClient *rcc, PipeItem *item, int item_pushed)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);

    spice_assert(item);
    if (item_pushed) {
        display_channel_client_release_item_after_push(dcc, item);
    } else {
        spice_debug("not pushed (%d)", item->type);
        display_channel_client_release_item_before_push(dcc, item);
    }

    RedWorker *worker = DCC_TO_WORKER(dcc);
    worker->timeout = 0;
}

static void display_channel_create(RedWorker *worker, int migrate, uint32_t n_surfaces)
{
    DisplayChannel *display_channel;
    ChannelCbs cbs = {
        .on_disconnect = display_channel_client_on_disconnect,
        .send_item = display_channel_send_item,
        .hold_item = display_channel_hold_pipe_item,
        .release_item = display_channel_release_item,
        .handle_migrate_flush_mark = display_channel_handle_migrate_mark,
        .handle_migrate_data = display_channel_handle_migrate_data,
        .handle_migrate_data_get_serial = display_channel_handle_migrate_data_get_serial
    };

    spice_return_if_fail(num_renderers > 0);

    spice_info("create display channel");
    if (!(display_channel = (DisplayChannel *)red_worker_new_channel(
            worker, sizeof(*display_channel), "display_channel",
            SPICE_CHANNEL_DISPLAY,
            SPICE_MIGRATE_NEED_FLUSH | SPICE_MIGRATE_NEED_DATA_TRANSFER,
            &cbs, display_channel_handle_message))) {
        spice_warning("failed to create display channel");
        return;
    }
    worker->display_channel = display_channel;
    stat_init(&display_channel->add_stat, "add", worker->clockid);
    stat_init(&display_channel->exclude_stat, "exclude", worker->clockid);
    stat_init(&display_channel->__exclude_stat, "__exclude", worker->clockid);
#ifdef RED_STATISTICS
    RedChannel *channel = RED_CHANNEL(display_channel);
    display_channel->cache_hits_counter = stat_add_counter(channel->stat,
                                                           "cache_hits", TRUE);
    display_channel->add_to_cache_counter = stat_add_counter(channel->stat,
                                                             "add_to_cache", TRUE);
    display_channel->non_cache_counter = stat_add_counter(channel->stat,
                                                          "non_cache", TRUE);
#endif
    stat_compress_init(&display_channel->lz_stat, "lz");
    stat_compress_init(&display_channel->glz_stat, "glz");
    stat_compress_init(&display_channel->quic_stat, "quic");
    stat_compress_init(&display_channel->jpeg_stat, "jpeg");
    stat_compress_init(&display_channel->zlib_glz_stat, "zlib");
    stat_compress_init(&display_channel->jpeg_alpha_stat, "jpeg_alpha");

    display_channel->n_surfaces = n_surfaces;
    display_channel->num_renderers = num_renderers;
    memcpy(display_channel->renderers, renderers, sizeof(display_channel->renderers));
    display_channel->renderer = RED_RENDERER_INVALID;

    ring_init(&display_channel->current_list);
    image_surface_init(display_channel);
    drawables_init(display_channel);
    image_cache_init(&display_channel->image_cache);
    stream_init(display_channel);
}

static void guest_set_client_capabilities(RedWorker *worker)
{
    int i;
    DisplayChannelClient *dcc;
    RedChannelClient *rcc;
    RingItem *link, *next;
    uint8_t caps[58] = { 0 }; /* FIXME: 58?? */
    int caps_available[] = {
        SPICE_DISPLAY_CAP_SIZED_STREAM,
        SPICE_DISPLAY_CAP_MONITORS_CONFIG,
        SPICE_DISPLAY_CAP_COMPOSITE,
        SPICE_DISPLAY_CAP_A8_SURFACE,
    };

    if (worker->qxl->st->qif->base.major_version < 3 ||
        (worker->qxl->st->qif->base.major_version == 3 &&
        worker->qxl->st->qif->base.minor_version < 2) ||
        !worker->qxl->st->qif->set_client_capabilities) {
        return;
    }
#define SET_CAP(a,c)                                                    \
        ((a)[(c) / 8] |= (1 << ((c) % 8)))

#define CLEAR_CAP(a,c)                                                  \
        ((a)[(c) / 8] &= ~(1 << ((c) % 8)))

    if (!worker->running) {
        worker->set_client_capabilities_pending = 1;
        return;
    }
    if ((worker->display_channel == NULL) ||
        (RED_CHANNEL(worker->display_channel)->clients_num == 0)) {
        worker->qxl->st->qif->set_client_capabilities(worker->qxl, FALSE, caps);
    } else {
        // Take least common denominator
        for (i = 0 ; i < sizeof(caps_available) / sizeof(caps_available[0]); ++i) {
            SET_CAP(caps, caps_available[i]);
        }
        DCC_FOREACH_SAFE(link, next, dcc, RED_CHANNEL(worker->display_channel)) {
            rcc = (RedChannelClient *)dcc;
            for (i = 0 ; i < sizeof(caps_available) / sizeof(caps_available[0]); ++i) {
                if (!red_channel_client_test_remote_cap(rcc, caps_available[i]))
                    CLEAR_CAP(caps, caps_available[i]);
            }
        }
        worker->qxl->st->qif->set_client_capabilities(worker->qxl, TRUE, caps);
    }
    worker->set_client_capabilities_pending = 0;
}

static void handle_new_display_channel(RedWorker *worker, RedClient *client, RedsStream *stream,
                                       int migrate,
                                       uint32_t *common_caps, int num_common_caps,
                                       uint32_t *caps, int num_caps)
{
    DisplayChannel *display_channel;
    DisplayChannelClient *dcc;

    spice_return_if_fail(worker->display_channel);

    display_channel = worker->display_channel;
    spice_info("add display channel client");
    dcc = dcc_new(display_channel, client, stream, migrate,
                  common_caps, num_common_caps, caps, num_caps,
                  worker->image_compression, worker->jpeg_state, worker->zlib_glz_state);

    if (dcc->jpeg_state == SPICE_WAN_COMPRESSION_AUTO) {
        display_channel->enable_jpeg = dcc->common.is_low_bandwidth;
    } else {
        display_channel->enable_jpeg = (dcc->jpeg_state == SPICE_WAN_COMPRESSION_ALWAYS);
    }

    if (dcc->zlib_glz_state == SPICE_WAN_COMPRESSION_AUTO) {
        display_channel->enable_zlib_glz_wrap = dcc->common.is_low_bandwidth;
    } else {
        display_channel->enable_zlib_glz_wrap = (dcc->zlib_glz_state ==
                                                 SPICE_WAN_COMPRESSION_ALWAYS);
    }
    spice_info("jpeg %s", display_channel->enable_jpeg ? "enabled" : "disabled");
    spice_info("zlib-over-glz %s", display_channel->enable_zlib_glz_wrap ? "enabled" : "disabled");

    guest_set_client_capabilities(worker);
    dcc_start(dcc);
}

static void red_connect_cursor(RedWorker *worker, RedClient *client, RedsStream *stream,
                               int migrate,
                               uint32_t *common_caps, int num_common_caps,
                               uint32_t *caps, int num_caps)
{
    CursorChannel *channel;
    CursorChannelClient *ccc;

    spice_return_if_fail(worker->cursor_channel != NULL);

    channel = worker->cursor_channel;
    spice_info("add cursor channel client");
    ccc = cursor_channel_client_new(channel, client, stream,
                                    migrate,
                                    common_caps, num_common_caps,
                                    caps, num_caps);
    spice_return_if_fail(ccc != NULL);

    RedChannelClient *rcc = RED_CHANNEL_CLIENT(ccc);
    red_channel_client_ack_zero_messages_window(rcc);
    red_channel_client_push_set_ack(rcc);

    // TODO: why do we check for context.canvas? defer this to after display cc is connected
    // and test it's canvas? this is just a test to see if there is an active renderer?
    if (display_channel_surface_has_canvas(worker->display_channel, 0))
        cursor_channel_init(channel, ccc);
}

static void surface_dirty_region_to_rects(RedSurface *surface,
                                          QXLRect *qxl_dirty_rects,
                                          uint32_t num_dirty_rects)
{
    QRegion *surface_dirty_region;
    SpiceRect *dirty_rects;
    int i;

    surface_dirty_region = &surface->draw_dirty_region;
    dirty_rects = spice_new0(SpiceRect, num_dirty_rects);
    region_ret_rects(surface_dirty_region, dirty_rects, num_dirty_rects);
    for (i = 0; i < num_dirty_rects; i++) {
        qxl_dirty_rects[i].top    = dirty_rects[i].top;
        qxl_dirty_rects[i].left   = dirty_rects[i].left;
        qxl_dirty_rects[i].bottom = dirty_rects[i].bottom;
        qxl_dirty_rects[i].right  = dirty_rects[i].right;
    }
    free(dirty_rects);
}

void display_channel_update(DisplayChannel *display,
                            uint32_t surface_id, const QXLRect *area, uint32_t clear_dirty,
                            QXLRect **qxl_dirty_rects, uint32_t *num_dirty_rects)
{
    SpiceRect rect;
    RedSurface *surface;

    VALIDATE_SURFACE_RET(display, surface_id);

    red_get_rect_ptr(&rect, area);
    red_update_area(display, &rect, surface_id);

    surface = &display->surfaces[surface_id];
    if (!*qxl_dirty_rects) {
        *num_dirty_rects = pixman_region32_n_rects(&surface->draw_dirty_region);
        *qxl_dirty_rects = spice_new0(QXLRect, *num_dirty_rects);
    }

    surface_dirty_region_to_rects(surface, *qxl_dirty_rects, *num_dirty_rects);
    if (clear_dirty)
        region_clear(&surface->draw_dirty_region);
}

static void handle_dev_update_async(void *opaque, uint32_t message_type, void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageUpdateAsync *msg = payload;
    QXLRect *qxl_dirty_rects = NULL;
    uint32_t num_dirty_rects = 0;

    spice_return_if_fail(worker->running);
    spice_return_if_fail(worker->qxl->st->qif->update_area_complete);

    flush_display_commands(worker);
    display_channel_update(worker->display_channel,
                           msg->surface_id, &msg->qxl_area, msg->clear_dirty_region,
                           &qxl_dirty_rects, &num_dirty_rects);

    worker->qxl->st->qif->update_area_complete(worker->qxl, msg->surface_id,
                                                qxl_dirty_rects, num_dirty_rects);
    free(qxl_dirty_rects);
}

static void handle_dev_update(void *opaque, uint32_t message_type, void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageUpdate *msg = payload;

    spice_return_if_fail(worker->running);

    flush_display_commands(worker);
    display_channel_update(worker->display_channel,
                           msg->surface_id, msg->qxl_area, msg->clear_dirty_region,
                           &msg->qxl_dirty_rects, &msg->num_dirty_rects);
}

static void handle_dev_del_memslot(void *opaque, uint32_t message_type, void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageDelMemslot *msg = payload;
    uint32_t slot_id = msg->slot_id;
    uint32_t slot_group_id = msg->slot_group_id;

    red_memslot_info_del_slot(&worker->mem_slots, slot_group_id, slot_id);
}

void display_channel_destroy_surface_wait(DisplayChannel *display, int surface_id)
{
    if (!display->surfaces[surface_id].context.canvas)
        return;

    red_handle_depends_on_target_surface(display, surface_id);
    /* note that red_handle_depends_on_target_surface must be called before current_remove_all.
       otherwise "current" will hold items that other drawables may depend on, and then
       current_remove_all will remove them from the pipe. */
    current_remove_all(display, surface_id);
    red_clear_surface_drawables_from_pipes(display, surface_id, TRUE);
}

static void handle_dev_destroy_surface_wait(void *opaque, uint32_t message_type, void *payload)
{
    RedWorkerMessageDestroySurfaceWait *msg = payload;
    RedWorker *worker = opaque;

    spice_return_if_fail(msg->surface_id == 0);

    flush_all_qxl_commands(worker);
    display_channel_destroy_surface_wait(worker->display_channel, msg->surface_id);
}

/* called upon device reset */

/* TODO: split me*/
void display_channel_destroy_surfaces(DisplayChannel *display)
{
    int i;

    spice_debug(NULL);
    //to handle better
    for (i = 0; i < NUM_SURFACES; ++i) {
        if (display->surfaces[i].context.canvas) {
            display_channel_destroy_surface_wait(display, i);
            if (display->surfaces[i].context.canvas) {
                display_channel_surface_unref(display, i);
            }
            spice_assert(!display->surfaces[i].context.canvas);
        }
    }
    spice_warn_if_fail(ring_is_empty(&display->streams));

    if (red_channel_is_connected(RED_CHANNEL(display))) {
        red_channel_pipes_add_type(RED_CHANNEL(display), PIPE_ITEM_TYPE_INVAL_PALLET_CACHE);
        red_pipes_add_verb(RED_CHANNEL(display), SPICE_MSG_DISPLAY_STREAM_DESTROY_ALL);
    }

    display_channel_free_glz_drawables(display);
}

static void handle_dev_destroy_surfaces(void *opaque, uint32_t message_type, void *payload)
{
    RedWorker *worker = opaque;

    flush_all_qxl_commands(worker);
    display_channel_destroy_surfaces(worker->display_channel);
    cursor_channel_reset(worker->cursor_channel);
}

static void display_update_monitors_config(DisplayChannel *display,
                                           QXLMonitorsConfig *config)
{
    if (display->monitors_config)
        monitors_config_unref(display->monitors_config);

    display->monitors_config =
        monitors_config_new(config->heads, config->count, config->max_allowed);

}

static void red_worker_push_monitors_config(RedWorker *worker)
{
    DisplayChannelClient *dcc;
    RingItem *item, *next;

    FOREACH_DCC(worker->display_channel, item, next, dcc) {
        dcc_push_monitors_config(dcc);
    }
}

static void set_monitors_config_to_primary(DisplayChannel *display)
{
    DrawContext *context = &display->surfaces[0].context;
    QXLHead head = { 0, };

    spice_return_if_fail(display->surfaces[0].context.canvas);

    if (display->monitors_config)
        monitors_config_unref(display->monitors_config);

    head.width = context->width;
    head.height = context->height;
    display->monitors_config = monitors_config_new(&head, 1, 1);
}

static void dev_create_primary_surface(RedWorker *worker, uint32_t surface_id,
                                       QXLDevSurfaceCreate surface)
{
    DisplayChannel *display = worker->display_channel;
    uint8_t *line_0;
    int error;

    spice_debug(NULL);
    spice_warn_if(surface_id != 0);
    spice_warn_if(surface.height == 0);
    spice_warn_if(((uint64_t)abs(surface.stride) * (uint64_t)surface.height) !=
             abs(surface.stride) * surface.height);

    line_0 = (uint8_t*)get_virt(&worker->mem_slots, surface.mem,
                                surface.height * abs(surface.stride),
                                surface.group_id, &error);
    if (error) {
        return;
    }
    if (surface.stride < 0) {
        line_0 -= (int32_t)(surface.stride * (surface.height -1));
    }

    if (worker->record_fd) {
        red_record_dev_input_primary_surface_create(worker->record_fd,
                    &surface, line_0);
    }

    red_create_surface(display, 0, surface.width, surface.height, surface.stride, surface.format,
                       line_0, surface.flags & QXL_SURF_FLAG_KEEP_DATA, TRUE);
    set_monitors_config_to_primary(display);

    if (display_is_connected(worker) && !worker->display_channel->common.during_target_migrate) {
        /* guest created primary, so it will (hopefully) send a monitors_config
         * now, don't send our own temporary one */
        if (!worker->driver_cap_monitors_config) {
            red_worker_push_monitors_config(worker);
        }
        red_pipes_add_verb(&worker->display_channel->common.base,
                           SPICE_MSG_DISPLAY_MARK);
        red_channel_push(&worker->display_channel->common.base);
    }

    cursor_channel_init(worker->cursor_channel, NULL);
}

static void handle_dev_create_primary_surface(void *opaque, uint32_t message_type, void *payload)
{
    RedWorkerMessageCreatePrimarySurface *msg = payload;
    RedWorker *worker = opaque;

    dev_create_primary_surface(worker, msg->surface_id, msg->surface);
}

static void destroy_primary_surface(RedWorker *worker, uint32_t surface_id)
{
    DisplayChannel *display = worker->display_channel;
    spice_warn_if(surface_id != 0);

    spice_debug(NULL);
    if (!display->surfaces[surface_id].context.canvas) {
        spice_warning("double destroy of primary surface");
        return;
    }

    flush_all_qxl_commands(worker);
    display_channel_destroy_surface_wait(display, 0);
    display_channel_surface_unref(display, 0);

    spice_warn_if_fail(ring_is_empty(&display->streams));
    spice_warn_if_fail(!display->surfaces[surface_id].context.canvas);

    cursor_channel_reset(worker->cursor_channel);
}

static void handle_dev_destroy_primary_surface(void *opaque, uint32_t message_type, void *payload)
{
    RedWorkerMessageDestroyPrimarySurface *msg = payload;
    RedWorker *worker = opaque;
    uint32_t surface_id = msg->surface_id;

    destroy_primary_surface(worker, surface_id);
}

static void handle_dev_destroy_primary_surface_async(void *opaque, uint32_t message_type, void *payload)
{
    RedWorkerMessageDestroyPrimarySurfaceAsync *msg = payload;
    RedWorker *worker = opaque;
    uint32_t surface_id = msg->surface_id;

    destroy_primary_surface(worker, surface_id);
}

static void handle_dev_flush_surfaces_async(void *opaque, uint32_t message_type, void *payload)
{
    RedWorker *worker = opaque;

    flush_all_qxl_commands(worker);
    display_channel_flush_all_surfaces(worker->display_channel);
}

static void handle_dev_stop(void *opaque, uint32_t message_type, void *payload)
{
    RedWorker *worker = opaque;

    spice_info("stop");
    spice_return_if_fail(worker->running);

    worker->running = FALSE;

    display_channel_free_glz_drawables(worker->display_channel);
    display_channel_flush_all_surfaces(worker->display_channel);

    /* todo: when the waiting is expected to take long (slow connection and
     * overloaded pipe), don't wait, and in case of migration,
     * purge the pipe, send destroy_all_surfaces
     * to the client (there is no such message right now), and start
     * from scratch on the destination side */
    if (!red_channel_wait_all_sent(RED_CHANNEL(worker->display_channel),
                                   DISPLAY_CLIENT_TIMEOUT)) {
        red_channel_apply_clients(RED_CHANNEL(worker->display_channel),
                                 red_channel_client_disconnect_if_pending_send);
    }
    if (!red_channel_wait_all_sent(RED_CHANNEL(worker->cursor_channel),
                                   DISPLAY_CLIENT_TIMEOUT)) {
        red_channel_apply_clients(RED_CHANNEL(worker->cursor_channel),
                                 red_channel_client_disconnect_if_pending_send);
    }
}

static void handle_dev_start(void *opaque, uint32_t message_type, void *payload)
{
    RedWorker *worker = opaque;

    spice_assert(!worker->running);
    if (worker->cursor_channel) {
        COMMON_CHANNEL(worker->cursor_channel)->during_target_migrate = FALSE;
    }
    if (worker->display_channel) {
        worker->display_channel->common.during_target_migrate = FALSE;
        if (red_channel_waits_for_migrate_data(&worker->display_channel->common.base)) {
            display_channel_wait_for_migrate_data(worker->display_channel);
        }
    }
    worker->running = TRUE;
    guest_set_client_capabilities(worker);
}

static void handle_dev_wakeup(void *opaque, uint32_t message_type, void *payload)
{
    RedWorker *worker = opaque;

    stat_inc_counter(worker->wakeup_counter, 1);
    red_dispatcher_clear_pending(worker->red_dispatcher, RED_DISPATCHER_PENDING_WAKEUP);
}

static void handle_dev_oom(void *opaque, uint32_t message_type, void *payload)
{
    RedWorker *worker = opaque;
    int ring_is_empty;

    spice_return_if_fail(worker->running);
    // streams? but without streams also leak

#if FIXME
    DisplayChannel *display = worker->display_channel;
    RedChannel *display_red_channel = &worker->display_channel->common.base;
    spice_debug("OOM1 #draw=%u, #red_draw=%u, #glz_draw=%u current %u pipes %u",
                display->drawable_count,
                worker->red_drawable_count,
                worker->glz_drawable_count,
                display->current_size,
                worker->display_channel ?
                red_channel_sum_pipes_size(display_red_channel) : 0);
#endif
    while (red_process_commands(worker, MAX_PIPE_SIZE, &ring_is_empty)) {
        red_channel_push(&worker->display_channel->common.base);
    }
    if (worker->qxl->st->qif->flush_resources(worker->qxl) == 0) {
        red_free_some(worker);
        worker->qxl->st->qif->flush_resources(worker->qxl);
    }
#if FIXME
    spice_debug("OOM2 #draw=%u, #red_draw=%u, #glz_draw=%u current %u pipes %u",
                display->drawable_count,
                worker->red_drawable_count,
                worker->glz_drawable_count,
                display->current_size,
                worker->display_channel ?
                red_channel_sum_pipes_size(display_red_channel) : 0);
#endif
    red_dispatcher_clear_pending(worker->red_dispatcher, RED_DISPATCHER_PENDING_OOM);
}

static void handle_dev_reset_cursor(void *opaque, uint32_t message_type, void *payload)
{
    RedWorker *worker = opaque;

    cursor_channel_reset(worker->cursor_channel);
}

static void handle_dev_reset_image_cache(void *opaque, uint32_t message_type, void *payload)
{
    RedWorker *worker = opaque;
    DisplayChannel *display = worker->display_channel;

    image_cache_reset(&display->image_cache);
}

static void handle_dev_destroy_surface_wait_async(void *opaque, uint32_t message_type, void *payload)
{
    RedWorkerMessageDestroySurfaceWaitAsync *msg = payload;
    RedWorker *worker = opaque;

    display_channel_destroy_surface_wait(worker->display_channel, msg->surface_id);
}

static void handle_dev_destroy_surfaces_async(void *opaque, uint32_t message_type, void *payload)
{
    RedWorker *worker = opaque;

    display_channel_destroy_surfaces(worker->display_channel);
}

static void handle_dev_create_primary_surface_async(void *opaque, uint32_t message_type, void *payload)
{
    RedWorkerMessageCreatePrimarySurfaceAsync *msg = payload;
    RedWorker *worker = opaque;

    dev_create_primary_surface(worker, msg->surface_id, msg->surface);
}

static void handle_dev_display_connect(void *opaque, uint32_t message_type, void *payload)
{
    RedWorkerMessageDisplayConnect *msg = payload;
    RedWorker *worker = opaque;
    RedsStream *stream = msg->stream;
    RedClient *client = msg->client;
    int migration = msg->migration;

    spice_info("connect");
    handle_new_display_channel(worker, client, stream, migration,
                               msg->common_caps, msg->num_common_caps,
                               msg->caps, msg->num_caps);
    free(msg->caps);
    free(msg->common_caps);
}

static void handle_dev_display_disconnect(void *opaque, uint32_t message_type, void *payload)
{
    RedWorkerMessageDisplayDisconnect *msg = payload;
    RedChannelClient *rcc = msg->rcc;
    RedWorker *worker = opaque;

    spice_info("disconnect display client");
    spice_assert(rcc);

    guest_set_client_capabilities(worker);

    red_channel_client_disconnect(rcc);
}

static void handle_dev_display_migrate(void *opaque, uint32_t message_type, void *payload)
{
    RedWorkerMessageDisplayMigrate *msg = payload;
    RedWorker *worker = opaque;

    RedChannelClient *rcc = msg->rcc;
    spice_info("migrate display client");
    spice_assert(rcc);
    red_migrate_display(worker->display_channel, rcc);
}

static void handle_dev_monitors_config_async(void *opaque,
                                             uint32_t message_type,
                                             void *payload)
{
    RedWorkerMessageMonitorsConfigAsync *msg = payload;
    RedWorker *worker = opaque;
    int min_size = sizeof(QXLMonitorsConfig) + sizeof(QXLHead);
    int error;
    QXLMonitorsConfig *dev_monitors_config =
        (QXLMonitorsConfig*)get_virt(&worker->mem_slots, msg->monitors_config,
                                     min_size, msg->group_id, &error);

    if (error) {
        /* TODO: raise guest bug (requires added QXL interface) */
        return;
    }
    worker->driver_cap_monitors_config = 1;
    if (dev_monitors_config->count == 0) {
        spice_warning("ignoring an empty monitors config message from driver");
        return;
    }
    if (dev_monitors_config->count > dev_monitors_config->max_allowed) {
        spice_warning("ignoring malformed monitors_config from driver, "
                      "count > max_allowed %d > %d",
                      dev_monitors_config->count,
                      dev_monitors_config->max_allowed);
        return;
    }
    display_update_monitors_config(worker->display_channel, dev_monitors_config);
    red_worker_push_monitors_config(worker);
}

/* TODO: special, perhaps use another dispatcher? */
static void handle_dev_cursor_connect(void *opaque, uint32_t message_type, void *payload)
{
    RedWorkerMessageCursorConnect *msg = payload;
    RedWorker *worker = opaque;
    RedsStream *stream = msg->stream;
    RedClient *client = msg->client;
    int migration = msg->migration;

    spice_info("cursor connect");
    red_connect_cursor(worker, client, stream, migration,
                       msg->common_caps, msg->num_common_caps,
                       msg->caps, msg->num_caps);
    free(msg->caps);
    free(msg->common_caps);
}

static void handle_dev_cursor_disconnect(void *opaque, uint32_t message_type, void *payload)
{
    RedWorkerMessageCursorDisconnect *msg = payload;
    RedChannelClient *rcc = msg->rcc;

    spice_info("disconnect cursor client");
    red_channel_client_disconnect(rcc);
}

static void handle_dev_cursor_migrate(void *opaque, uint32_t message_type, void *payload)
{
    RedWorkerMessageCursorMigrate *msg = payload;
    RedChannelClient *rcc = msg->rcc;

    spice_info("migrate cursor client");
    cursor_channel_client_migrate(CURSOR_CHANNEL_CLIENT(rcc));
}

static void handle_dev_set_compression(void *opaque, uint32_t message_type, void *payload)
{
    RedWorkerMessageSetCompression *msg = payload;
    RedWorker *worker = opaque;

    worker->image_compression = msg->image_compression;
    switch (worker->image_compression) {
    case SPICE_IMAGE_COMPRESS_AUTO_LZ:
        spice_info("ic auto_lz");
        break;
    case SPICE_IMAGE_COMPRESS_AUTO_GLZ:
        spice_info("ic auto_glz");
        break;
    case SPICE_IMAGE_COMPRESS_QUIC:
        spice_info("ic quic");
        break;
    case SPICE_IMAGE_COMPRESS_LZ:
        spice_info("ic lz");
        break;
    case SPICE_IMAGE_COMPRESS_GLZ:
        spice_info("ic glz");
        break;
    case SPICE_IMAGE_COMPRESS_OFF:
        spice_info("ic off");
        break;
    default:
        spice_warning("ic invalid");
    }

    display_channel_compress_stats_print(worker->display_channel);
    display_channel_compress_stats_reset(worker->display_channel);
}

static void handle_dev_set_streaming_video(void *opaque, uint32_t message_type, void *payload)
{
    RedWorkerMessageSetStreamingVideo *msg = payload;
    RedWorker *worker = opaque;

    display_channel_set_stream_video(worker->display_channel, msg->streaming_video);
}

static void handle_dev_set_mouse_mode(void *opaque, uint32_t message_type, void *payload)
{
    RedWorkerMessageSetMouseMode *msg = payload;
    RedWorker *worker = opaque;

    spice_info("mouse mode %u", msg->mode);
    cursor_channel_set_mouse_mode(worker->cursor_channel, msg->mode);
}

static void dev_add_memslot(RedWorker *worker, QXLDevMemSlot mem_slot)
{
    red_memslot_info_add_slot(&worker->mem_slots, mem_slot.slot_group_id, mem_slot.slot_id,
                              mem_slot.addr_delta, mem_slot.virt_start, mem_slot.virt_end,
                              mem_slot.generation);
}

static void handle_dev_add_memslot(void *opaque, uint32_t message_type, void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageAddMemslot *msg = payload;
    QXLDevMemSlot mem_slot = msg->mem_slot;

    red_memslot_info_add_slot(&worker->mem_slots, mem_slot.slot_group_id, mem_slot.slot_id,
                              mem_slot.addr_delta, mem_slot.virt_start, mem_slot.virt_end,
                              mem_slot.generation);
}

static void handle_dev_add_memslot_async(void *opaque, uint32_t message_type, void *payload)
{
    RedWorkerMessageAddMemslotAsync *msg = payload;
    RedWorker *worker = opaque;

    dev_add_memslot(worker, msg->mem_slot);
}

static void handle_dev_reset_memslots(void *opaque, uint32_t message_type, void *payload)
{
    RedWorker *worker = opaque;

    red_memslot_info_reset(&worker->mem_slots);
}

static void handle_dev_driver_unload(void *opaque, uint32_t message_type, void *payload)
{
    RedWorker *worker = opaque;

    worker->driver_cap_monitors_config = 0;
}

static int loadvm_command(RedWorker *worker, QXLCommandExt *ext)
{
    RedCursorCmd *cursor_cmd;
    RedSurfaceCmd *surface_cmd;

    switch (ext->cmd.type) {
    case QXL_CMD_CURSOR:
        cursor_cmd = spice_new0(RedCursorCmd, 1);
        if (red_get_cursor_cmd(&worker->mem_slots, ext->group_id, cursor_cmd, ext->cmd.data)) {
            free(cursor_cmd);
            return FALSE;
        }
        cursor_channel_process_cmd(worker->cursor_channel, cursor_cmd, ext->group_id);
        break;
    case QXL_CMD_SURFACE:
        surface_cmd = spice_new0(RedSurfaceCmd, 1);
        if (red_get_surface_cmd(&worker->mem_slots, ext->group_id, surface_cmd, ext->cmd.data)) {
            free(surface_cmd);
            return FALSE;
        }
        red_process_surface(worker, surface_cmd, ext->group_id, TRUE);
        break;
    default:
        spice_warning("unhandled loadvm command type (%d)", ext->cmd.type);
    }

    return TRUE;
}

static void handle_dev_loadvm_commands(void *opaque, uint32_t message_type, void *payload)
{
    RedWorkerMessageLoadvmCommands *msg = payload;
    RedWorker *worker = opaque;
    uint32_t i;
    uint32_t count = msg->count;
    QXLCommandExt *ext = msg->ext;

    spice_info("loadvm_commands");
    for (i = 0 ; i < count ; ++i) {
        if (!loadvm_command(worker, &ext[i])) {
            /* XXX allow failure in loadvm? */
            spice_warning("failed loadvm command type (%d)", ext[i].cmd.type);
        }
    }
}

static void worker_handle_dispatcher_async_done(void *opaque,
                                                uint32_t message_type,
                                                void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageAsync *msg_async = payload;

    spice_debug(NULL);
    red_dispatcher_async_complete(worker->red_dispatcher, msg_async->cmd);
}

static void worker_dispatcher_record(void *opaque, uint32_t message_type, void *payload)
{
    RedWorker *worker = opaque;

    if (worker->record_fd) {
        red_record_event(worker, 1, message_type);
    }
}

static void worker_dispatcher_register(RedWorker *worker, Dispatcher *dispatcher)
{
    dispatcher_set_opaque(dispatcher, worker);

    dispatcher_register_extra_handler(dispatcher, worker_dispatcher_record);
    dispatcher_register_async_done_callback(dispatcher,
                                            worker_handle_dispatcher_async_done);

    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DISPLAY_CONNECT,
                                handle_dev_display_connect,
                                sizeof(RedWorkerMessageDisplayConnect),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DISPLAY_DISCONNECT,
                                handle_dev_display_disconnect,
                                sizeof(RedWorkerMessageDisplayDisconnect),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DISPLAY_MIGRATE,
                                handle_dev_display_migrate,
                                sizeof(RedWorkerMessageDisplayMigrate),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_CURSOR_CONNECT,
                                handle_dev_cursor_connect,
                                sizeof(RedWorkerMessageCursorConnect),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_CURSOR_DISCONNECT,
                                handle_dev_cursor_disconnect,
                                sizeof(RedWorkerMessageCursorDisconnect),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_CURSOR_MIGRATE,
                                handle_dev_cursor_migrate,
                                sizeof(RedWorkerMessageCursorMigrate),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_UPDATE,
                                handle_dev_update,
                                sizeof(RedWorkerMessageUpdate),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_UPDATE_ASYNC,
                                handle_dev_update_async,
                                sizeof(RedWorkerMessageUpdateAsync),
                                DISPATCHER_ASYNC);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_ADD_MEMSLOT,
                                handle_dev_add_memslot,
                                sizeof(RedWorkerMessageAddMemslot),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_ADD_MEMSLOT_ASYNC,
                                handle_dev_add_memslot_async,
                                sizeof(RedWorkerMessageAddMemslotAsync),
                                DISPATCHER_ASYNC);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DEL_MEMSLOT,
                                handle_dev_del_memslot,
                                sizeof(RedWorkerMessageDelMemslot),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_SURFACES,
                                handle_dev_destroy_surfaces,
                                sizeof(RedWorkerMessageDestroySurfaces),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_SURFACES_ASYNC,
                                handle_dev_destroy_surfaces_async,
                                sizeof(RedWorkerMessageDestroySurfacesAsync),
                                DISPATCHER_ASYNC);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_PRIMARY_SURFACE,
                                handle_dev_destroy_primary_surface,
                                sizeof(RedWorkerMessageDestroyPrimarySurface),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_PRIMARY_SURFACE_ASYNC,
                                handle_dev_destroy_primary_surface_async,
                                sizeof(RedWorkerMessageDestroyPrimarySurfaceAsync),
                                DISPATCHER_ASYNC);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_CREATE_PRIMARY_SURFACE_ASYNC,
                                handle_dev_create_primary_surface_async,
                                sizeof(RedWorkerMessageCreatePrimarySurfaceAsync),
                                DISPATCHER_ASYNC);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_CREATE_PRIMARY_SURFACE,
                                handle_dev_create_primary_surface,
                                sizeof(RedWorkerMessageCreatePrimarySurface),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_RESET_IMAGE_CACHE,
                                handle_dev_reset_image_cache,
                                sizeof(RedWorkerMessageResetImageCache),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_RESET_CURSOR,
                                handle_dev_reset_cursor,
                                sizeof(RedWorkerMessageResetCursor),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_WAKEUP,
                                handle_dev_wakeup,
                                sizeof(RedWorkerMessageWakeup),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_OOM,
                                handle_dev_oom,
                                sizeof(RedWorkerMessageOom),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_START,
                                handle_dev_start,
                                sizeof(RedWorkerMessageStart),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_FLUSH_SURFACES_ASYNC,
                                handle_dev_flush_surfaces_async,
                                sizeof(RedWorkerMessageFlushSurfacesAsync),
                                DISPATCHER_ASYNC);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_STOP,
                                handle_dev_stop,
                                sizeof(RedWorkerMessageStop),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_LOADVM_COMMANDS,
                                handle_dev_loadvm_commands,
                                sizeof(RedWorkerMessageLoadvmCommands),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_SET_COMPRESSION,
                                handle_dev_set_compression,
                                sizeof(RedWorkerMessageSetCompression),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_SET_STREAMING_VIDEO,
                                handle_dev_set_streaming_video,
                                sizeof(RedWorkerMessageSetStreamingVideo),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_SET_MOUSE_MODE,
                                handle_dev_set_mouse_mode,
                                sizeof(RedWorkerMessageSetMouseMode),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_SURFACE_WAIT,
                                handle_dev_destroy_surface_wait,
                                sizeof(RedWorkerMessageDestroySurfaceWait),
                                DISPATCHER_ACK);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_SURFACE_WAIT_ASYNC,
                                handle_dev_destroy_surface_wait_async,
                                sizeof(RedWorkerMessageDestroySurfaceWaitAsync),
                                DISPATCHER_ASYNC);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_RESET_MEMSLOTS,
                                handle_dev_reset_memslots,
                                sizeof(RedWorkerMessageResetMemslots),
                                DISPATCHER_NONE);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_MONITORS_CONFIG_ASYNC,
                                handle_dev_monitors_config_async,
                                sizeof(RedWorkerMessageMonitorsConfigAsync),
                                DISPATCHER_ASYNC);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DRIVER_UNLOAD,
                                handle_dev_driver_unload,
                                sizeof(RedWorkerMessageDriverUnload),
                                DISPATCHER_NONE);

    dispatcher_attach(dispatcher, worker->main_context);
}


typedef struct _RedWorkerSource {
    GSource source;
    RedWorker *worker;
} RedWorkerSource;

static gboolean worker_source_prepare(GSource *source, gint *timeout)
{
    RedWorkerSource *wsource = (RedWorkerSource *)source;
    RedWorker *worker = wsource->worker;

    *timeout = worker->timeout;
    *timeout = MIN(worker->timeout,
                   display_channel_get_streams_timout(worker->display_channel));
    if (*timeout == 0)
        return TRUE;

    return FALSE; /* do no timeout poll */
}

static gboolean worker_source_check(GSource *source)
{
    RedWorkerSource *wsource = (RedWorkerSource *)source;
    RedWorker *worker = wsource->worker;

    return worker->running /* TODO && worker->pending_process */;
}

static void display_channel_streams_timout(DisplayChannel *display)
{
    Ring *ring = &display->streams;
    RingItem *item;

    red_time_t now = red_get_monotonic_time();
    item = ring_get_head(ring);
    while (item) {
        Stream *stream = SPICE_CONTAINEROF(item, Stream, link);
        item = ring_next(ring, item);
        if (now >= (stream->last_time + RED_STREAM_TIMOUT)) {
            detach_stream_gracefully(display, stream, NULL);
            stream_stop(display, stream);
        }
    }
}
static gboolean worker_source_dispatch(GSource *source, GSourceFunc callback,
                                       gpointer user_data)
{
    RedWorkerSource *wsource = (RedWorkerSource *)source;
    RedWorker *worker = wsource->worker;
    DisplayChannel *display = worker->display_channel;
    int ring_is_empty;

    /* during migration, in the dest, the display channel can be initialized
       while the global lz data not since migrate data msg hasn't been
       received yet */
    /* FIXME: why is this here, and not in display_channel_create */
    display_channel_free_glz_drawables_to_free(display);

    /* FIXME: could use its own source */
    display_channel_streams_timout(display);

    worker->timeout = -1;
    red_process_cursor(worker, MAX_PIPE_SIZE, &ring_is_empty);
    red_process_commands(worker, MAX_PIPE_SIZE, &ring_is_empty);

    /* FIXME: remove me? that should be handled by watch out condition */
    red_push(worker);

    return TRUE;
}

/* cannot be const */
static GSourceFuncs worker_source_funcs = {
    .prepare = worker_source_prepare,
    .check = worker_source_check,
    .dispatch = worker_source_dispatch,
};

RedWorker* red_worker_new(QXLInstance *qxl, RedDispatcher *red_dispatcher)
{
    QXLDevInitInfo init_info;
    RedWorker *worker;
    Dispatcher *dispatcher;
    const char *record_filename;

    qxl->st->qif->get_init_info(qxl, &init_info);

    worker = spice_new0(RedWorker, 1);
    worker->main_context = g_main_context_new();

    record_filename = getenv("SPICE_WORKER_RECORD_FILENAME");
    if (record_filename) {
        static const char *header = "SPICE_REPLAY 1\n";

        worker->record_fd = fopen(record_filename, "w+");
        if (worker->record_fd == NULL) {
            spice_error("failed to open recording file %s\n", record_filename);
        }
        if (fwrite(header, sizeof(header), 1, worker->record_fd) != 1) {
            spice_error("failed to write replay header");
        }
    }
    dispatcher = red_dispatcher_get_dispatcher(red_dispatcher);
    worker_dispatcher_register(worker, dispatcher);

    worker->red_dispatcher = red_dispatcher;
    worker->qxl = qxl;
    worker->image_compression = image_compression;
    worker->jpeg_state = jpeg_state;
    worker->zlib_glz_state = zlib_glz_state;
    worker->driver_cap_monitors_config = 0;
#ifdef RED_STATISTICS
    char worker_str[20];
    sprintf(worker_str, "display[%d]", worker->qxl->id);
    worker->stat = stat_add_node(INVALID_STAT_REF, worker_str, TRUE);
    worker->wakeup_counter = stat_add_counter(worker->stat, "wakeups", TRUE);
    worker->command_counter = stat_add_counter(worker->stat, "commands", TRUE);
#endif

    GSource *source = g_source_new(&worker_source_funcs, sizeof(RedWorkerSource));
    RedWorkerSource *wsource = (RedWorkerSource *)source;
    wsource->worker = worker;
    g_source_attach(source, worker->main_context);
    g_source_unref(source);

    red_memslot_info_init(&worker->mem_slots,
                          init_info.num_memslots_groups,
                          init_info.num_memslots,
                          init_info.memslot_gen_bits,
                          init_info.memslot_id_bits,
                          init_info.internal_groupslot_id);

    spice_warn_if(init_info.n_surfaces > NUM_SURFACES);

    worker->timeout = -1;
    worker->set_client_capabilities_pending = TRUE;

    worker->cursor_channel = cursor_channel_new(worker);
    // TODO: handle seemless migration. Temp, setting migrate to FALSE
    display_channel_create(worker, FALSE, init_info.n_surfaces);

    worker->wait_for_clients = !g_getenv("SPICE_NOWAIT_CLIENTS");

    return worker;
}


SPICE_GNUC_NORETURN static void *red_worker_main(void *arg)
{
    RedWorker *worker = arg;

    spice_info("begin");
    spice_assert(MAX_PIPE_SIZE > WIDE_CLIENT_ACK_WINDOW &&
           MAX_PIPE_SIZE > NARROW_CLIENT_ACK_WINDOW); //ensure wakeup by ack message

    if (pthread_getcpuclockid(pthread_self(), &worker->clockid)) {
        spice_warning("getcpuclockid failed");
    }

    RED_CHANNEL(worker->cursor_channel)->thread_id = pthread_self();
    RED_CHANNEL(worker->display_channel)->thread_id = pthread_self();

    GMainLoop *loop = g_main_loop_new(worker->main_context, FALSE);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);

    /* FIXME: free worker, and join threads */
    abort();
}

bool red_worker_run(RedWorker *worker)
{
    sigset_t thread_sig_mask;
    sigset_t curr_sig_mask;
    int r;

    spice_return_val_if_fail(worker, FALSE);
    spice_return_val_if_fail(!worker->thread, FALSE);

    sigfillset(&thread_sig_mask);
    sigdelset(&thread_sig_mask, SIGILL);
    sigdelset(&thread_sig_mask, SIGFPE);
    sigdelset(&thread_sig_mask, SIGSEGV);
    pthread_sigmask(SIG_SETMASK, &thread_sig_mask, &curr_sig_mask);
    if ((r = pthread_create(&worker->thread, NULL, red_worker_main, worker))) {
        spice_error("create thread failed %d", r);
    }
    pthread_sigmask(SIG_SETMASK, &curr_sig_mask, NULL);

    return r == 0;
}

RedChannel* red_worker_get_cursor_channel(RedWorker *worker)
{
    spice_return_val_if_fail(worker, NULL);

    return RED_CHANNEL(worker->cursor_channel);
}

RedChannel* red_worker_get_display_channel(RedWorker *worker)
{
    spice_return_val_if_fail(worker, NULL);

    return RED_CHANNEL(worker->display_channel);
}

clockid_t red_worker_get_clockid(RedWorker *worker)
{
    spice_return_val_if_fail(worker, 0);

    return worker->clockid;
}
