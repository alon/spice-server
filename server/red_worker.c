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
#include "common/rect.h"
#include "common/region.h"
#include "common/ring.h"

#include "display-channel.h"
#include "stream.h"

#include "spice.h"
#include "red_worker.h"
#include "cursor-channel.h"
#include "tree.h"
#include "utils.h"

#define CMD_RING_POLL_TIMEOUT 10 //milli
#define CMD_RING_POLL_RETRIES 200

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

GMainContext* red_worker_get_context(RedWorker *worker)
{
    spice_return_val_if_fail(worker, NULL);

    return worker->main_context;
}

QXLInstance* red_worker_get_qxl(RedWorker *worker)
{
    spice_return_val_if_fail(worker != NULL, NULL);

    return worker->qxl;
}

RedMemSlotInfo* red_worker_get_memslot(RedWorker *worker)
{
    spice_return_val_if_fail(worker != NULL, NULL);

    return &worker->mem_slots;
}

void red_worker_update_timeout(RedWorker *worker, gint timeout)
{
    spice_return_if_fail(worker != NULL);
    spice_return_if_fail(timeout >= 0);

    worker->timeout = MIN(worker->timeout, timeout);
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

static RedDrawable *red_drawable_new(void)
{
    RedDrawable * red = spice_new0(RedDrawable, 1);

    red->refs = 1;
    return red;
}

static bool red_process_draw(RedWorker *worker, QXLCommandExt *ext_cmd)
{
    DisplayChannel *display = worker->display_channel;
    RedDrawable *red_drawable = NULL;
    Drawable *drawable = NULL;
    bool success = FALSE;

    drawable = display_channel_drawable_try_new(display, ext_cmd->group_id,
                                                worker->process_commands_generation);
    if (!drawable)
        goto end;

    worker->red_drawable_count++;

    red_drawable = red_drawable_new();
    if (red_get_drawable(&worker->mem_slots, ext_cmd->group_id,
                         red_drawable, ext_cmd->cmd.data, ext_cmd->flags) != 0)
        goto end;

    success = display_channel_add_drawable(worker->display_channel, drawable, red_drawable);
    spice_warn_if_fail(success);

end:
    if (drawable != NULL)
        display_channel_drawable_unref(display, drawable);
    if (red_drawable != NULL)
        red_drawable_unref(worker, red_drawable, ext_cmd->group_id);
    return success;
}


static void red_process_surface(RedWorker *worker, RedSurfaceCmd *surface,
                                uint32_t group_id, int loadvm)
{
    DisplayChannel *display = worker->display_channel;
    int surface_id;
    RedSurface *red_surface;
    uint8_t *data;

    surface_id = surface->surface_id;
    spice_return_if_fail(surface_id < display->n_surfaces);

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
        display_channel_create_surface(worker->display_channel, surface_id, surface->u.surface_create.width,
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
        display_channel_destroy_surface(display, surface_id);
        break;
    default:
        spice_warn_if_reached();
    };
    red_put_surface_cmd(surface);
    free(surface);
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
            display_channel_draw(worker->display_channel, &update.area, update.surface_id);
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

static void red_push(RedWorker *worker)
{
    if (worker->cursor_channel) {
        red_channel_push(RED_CHANNEL(worker->cursor_channel));
    }
    if (worker->display_channel) {
        red_channel_push(RED_CHANNEL(worker->display_channel));
    }
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
    stream_detach_and_stop(display);
    if (red_channel_client_is_connected(rcc)) {
        red_channel_client_default_migrate(rcc);
    }
}

static void flush_display_commands(RedWorker *worker)
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

static void flush_cursor_commands(RedWorker *worker)
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
static void flush_all_qxl_commands(RedWorker *worker)
{
    flush_display_commands(worker);
    flush_cursor_commands(worker);
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

    memslot_info_del_slot(&worker->mem_slots, slot_group_id, slot_id);
}

static void handle_dev_destroy_surface_wait(void *opaque, uint32_t message_type, void *payload)
{
    RedWorkerMessageDestroySurfaceWait *msg = payload;
    RedWorker *worker = opaque;

    spice_return_if_fail(msg->surface_id == 0);

    flush_all_qxl_commands(worker);
    display_channel_destroy_surface_wait(worker->display_channel, msg->surface_id);
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

    line_0 = (uint8_t*)memslot_get_virt(&worker->mem_slots, surface.mem,
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

    display_channel_create_surface(display, 0, surface.width, surface.height, surface.stride, surface.format,
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
        display_channel_free_some(worker->display_channel);
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
        (QXLMonitorsConfig*)memslot_get_virt(&worker->mem_slots, msg->monitors_config,
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

static void cursor_connect(RedWorker *worker, RedClient *client, RedsStream *stream,
                           int migrate,
                           uint32_t *common_caps, int num_common_caps,
                           uint32_t *caps, int num_caps)
{
    CursorChannel *channel = worker->cursor_channel;
    CursorChannelClient *ccc;

    spice_return_if_fail(worker->cursor_channel != NULL);

    spice_info("add cursor channel client");
    ccc = cursor_channel_client_new(channel, client, stream,
                                    migrate,
                                    common_caps, num_common_caps,
                                    caps, num_caps);

    // TODO: why do we check for context.canvas? defer this to after display cc is connected
    // and test it's canvas? this is just a test to see if there is an active renderer?
    if (display_channel_surface_has_canvas(worker->display_channel, 0))
        cursor_channel_init(channel, ccc);
}

/* TODO: special, perhaps use another dispatcher? */
static void handle_dev_cursor_connect(void *opaque, uint32_t message_type, void *payload)
{
    RedWorkerMessageCursorConnect *msg = payload;
    RedWorker *worker = opaque;

    spice_info("cursor connect");
    cursor_connect(worker,
                   msg->client, msg->stream, msg->migration,
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
    memslot_info_add_slot(&worker->mem_slots, mem_slot.slot_group_id, mem_slot.slot_id,
                              mem_slot.addr_delta, mem_slot.virt_start, mem_slot.virt_end,
                              mem_slot.generation);
}

static void handle_dev_add_memslot(void *opaque, uint32_t message_type, void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageAddMemslot *msg = payload;
    QXLDevMemSlot mem_slot = msg->mem_slot;

    memslot_info_add_slot(&worker->mem_slots, mem_slot.slot_group_id, mem_slot.slot_id,
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

    memslot_info_reset(&worker->mem_slots);
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

    /* TODO: register cursor & display specific msg in respective channel files */
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
    stream_timout(display);

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

    memslot_info_init(&worker->mem_slots,
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
    worker->display_channel = display_channel_new(worker, FALSE, init_info.n_surfaces);

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
