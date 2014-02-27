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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <signal.h>
#include <inttypes.h>

#include <spice/qxl_dev.h>
#include "common/quic.h"

#include "spice.h"
#include "red_worker.h"
#include "canvas.h"
#include "reds.h"
#include "dispatcher.h"
#include "red_parse_qxl.h"
#include "stream.h"

#include "red_dispatcher.h"

static int num_active_workers = 0;

struct AsyncCommand {
    RingItem link;
    RedWorkerMessage message;
    uint64_t cookie;
};

struct RedDispatcher {
    QXLWorker base;
    QXLInstance *qxl;
    Dispatcher dispatcher;
    uint32_t pending;
    int primary_active;
    int x_res;
    int y_res;
    int use_hardware_cursor;
    RedDispatcher *next;
    Ring async_commands;
    pthread_mutex_t  async_lock;
    QXLDevSurfaceCreate surface_create;
};

typedef struct RedWorkeState {
    uint8_t *io_base;
    unsigned long phys_delta;

    uint32_t x_res;
    uint32_t y_res;
    uint32_t bits;
    uint32_t stride;
} RedWorkeState;

static RedDispatcher *dispatchers = NULL;

static int red_dispatcher_check_qxl_version(RedDispatcher *rd, int major, int minor)
{
    int qxl_major = rd->qxl->st->qif->base.major_version;
    int qxl_minor = rd->qxl->st->qif->base.minor_version;

    return ((qxl_major > major) ||
            ((qxl_major == major) && (qxl_minor >= minor)));
}

static void red_dispatcher_set_display_peer(RedChannel *channel, RedClient *client,
                                            RedsStream *stream, int migration,
                                            int num_common_caps, uint32_t *common_caps, int num_caps,
                                            uint32_t *caps)
{
    RedWorkerMessageDisplayConnect payload = {0,};
    RedDispatcher *dispatcher;

    spice_debug("%s", "");
    dispatcher = (RedDispatcher *)channel->data;
    payload.client = client;
    payload.stream = stream;
    payload.migration = migration;
    payload.num_common_caps = num_common_caps;
    payload.common_caps = spice_malloc(sizeof(uint32_t)*num_common_caps);
    payload.num_caps = num_caps;
    payload.caps = spice_malloc(sizeof(uint32_t)*num_caps);

    memcpy(payload.common_caps, common_caps, sizeof(uint32_t)*num_common_caps);
    memcpy(payload.caps, caps, sizeof(uint32_t)*num_caps);

    dispatcher_send_message(&dispatcher->dispatcher,
                            RED_WORKER_MESSAGE_DISPLAY_CONNECT,
                            &payload);
}

static void red_dispatcher_disconnect_display_peer(RedChannelClient *rcc)
{
    RedWorkerMessageDisplayDisconnect payload;
    RedDispatcher *dispatcher;

    if (!rcc->channel) {
        return;
    }

    dispatcher = (RedDispatcher *)rcc->channel->data;

    spice_printerr("");
    payload.rcc = rcc;

    // TODO: we turned it to be sync, due to client_destroy . Should we support async? - for this we will need ref count
    // for channels
    dispatcher_send_message(&dispatcher->dispatcher,
                            RED_WORKER_MESSAGE_DISPLAY_DISCONNECT,
                            &payload);
}

static void red_dispatcher_display_migrate(RedChannelClient *rcc)
{
    RedWorkerMessageDisplayMigrate payload;
    RedDispatcher *dispatcher;
    if (!rcc->channel) {
        return;
    }
    dispatcher = (RedDispatcher *)rcc->channel->data;
    spice_printerr("channel type %u id %u", rcc->channel->type, rcc->channel->id);
    payload.rcc = rcc;
    dispatcher_send_message(&dispatcher->dispatcher,
                            RED_WORKER_MESSAGE_DISPLAY_MIGRATE,
                            &payload);
}

static void red_dispatcher_set_cursor_peer(RedChannel *channel, RedClient *client, RedsStream *stream,
                                           int migration, int num_common_caps,
                                           uint32_t *common_caps, int num_caps,
                                           uint32_t *caps)
{
    RedWorkerMessageCursorConnect payload = {0,};
    RedDispatcher *dispatcher = (RedDispatcher *)channel->data;
    spice_printerr("");
    payload.client = client;
    payload.stream = stream;
    payload.migration = migration;
    payload.num_common_caps = num_common_caps;
    payload.common_caps = spice_malloc(sizeof(uint32_t)*num_common_caps);
    payload.num_caps = num_caps;
    payload.caps = spice_malloc(sizeof(uint32_t)*num_caps);

    memcpy(payload.common_caps, common_caps, sizeof(uint32_t)*num_common_caps);
    memcpy(payload.caps, caps, sizeof(uint32_t)*num_caps);

    /* TODO serialize it all, no dangling pointers */
    dispatcher_send_message(&dispatcher->dispatcher,
                            RED_WORKER_MESSAGE_CURSOR_CONNECT,
                            &payload);
}

static void red_dispatcher_disconnect_cursor_peer(RedChannelClient *rcc)
{
    RedWorkerMessageCursorDisconnect payload;
    RedDispatcher *dispatcher;

    if (!rcc->channel) {
        return;
    }

    dispatcher = (RedDispatcher *)rcc->channel->data;
    spice_printerr("");
    payload.rcc = rcc;

    dispatcher_send_message(&dispatcher->dispatcher,
                            RED_WORKER_MESSAGE_CURSOR_DISCONNECT,
                            &payload);
}

static void red_dispatcher_cursor_migrate(RedChannelClient *rcc)
{
    RedWorkerMessageCursorMigrate payload;
    RedDispatcher *dispatcher;

    if (!rcc->channel) {
        return;
    }
    dispatcher = (RedDispatcher *)rcc->channel->data;
    spice_printerr("channel type %u id %u", rcc->channel->type, rcc->channel->id);
    payload.rcc = rcc;
    dispatcher_send_message(&dispatcher->dispatcher,
                            RED_WORKER_MESSAGE_CURSOR_MIGRATE,
                            &payload);
}

int red_dispatcher_qxl_count(void)
{
    return num_active_workers;
}

static void update_client_mouse_allowed(void)
{
    static int allowed = FALSE;
    int allow_now = FALSE;
    int x_res = 0;
    int y_res = 0;

    if (num_active_workers > 0) {
        allow_now = TRUE;
        RedDispatcher *now = dispatchers;
        while (now && allow_now) {
            if (now->primary_active) {
                allow_now = now->use_hardware_cursor;
                if (num_active_workers == 1) {
                    if (allow_now) {
                        x_res = now->x_res;
                        y_res = now->y_res;
                    }
                    break;
                }
            }
            now = now->next;
        }
    }

    if (allow_now || allow_now != allowed) {
        allowed = allow_now;
        reds_set_client_mouse_allowed(allowed, x_res, y_res);
    }
}

static void red_dispatcher_update_area(RedDispatcher *dispatcher, uint32_t surface_id,
                                   QXLRect *qxl_area, QXLRect *qxl_dirty_rects,
                                   uint32_t num_dirty_rects, uint32_t clear_dirty_region)
{
    RedWorkerMessageUpdate payload = {0,};

    payload.surface_id = surface_id;
    payload.qxl_area = qxl_area;
    payload.qxl_dirty_rects = qxl_dirty_rects;
    payload.num_dirty_rects = num_dirty_rects;
    payload.clear_dirty_region = clear_dirty_region;
    dispatcher_send_message(&dispatcher->dispatcher,
                            RED_WORKER_MESSAGE_UPDATE,
                            &payload);
}

int red_dispatcher_use_client_monitors_config(void)
{
    RedDispatcher *now = dispatchers;

    if (num_active_workers == 0) {
        return FALSE;
    }

    for (; now ; now = now->next) {
        if (!red_dispatcher_check_qxl_version(now, 3, 3) ||
            !now->qxl->st->qif->client_monitors_config ||
            !now->qxl->st->qif->client_monitors_config(now->qxl, NULL)) {
            return FALSE;
        }
    }
    return TRUE;
}

void red_dispatcher_client_monitors_config(VDAgentMonitorsConfig *monitors_config)
{
    RedDispatcher *now = dispatchers;

    while (now) {
        if (!now->qxl->st->qif->client_monitors_config ||
            !now->qxl->st->qif->client_monitors_config(now->qxl,
                                                       monitors_config)) {
            spice_warning("spice bug: QXLInterface::client_monitors_config"
                          " failed/missing unexpectedly\n");
        }
        now = now->next;
    }
}

static AsyncCommand *async_command_alloc(RedDispatcher *dispatcher,
                                         RedWorkerMessage message,
                                         uint64_t cookie)
{
    AsyncCommand *async_command = spice_new0(AsyncCommand, 1);

    pthread_mutex_lock(&dispatcher->async_lock);
    async_command->cookie = cookie;
    async_command->message = message;
    ring_add(&dispatcher->async_commands, &async_command->link);
    pthread_mutex_unlock(&dispatcher->async_lock);
    spice_debug("%p", async_command);
    return async_command;
}

static void red_dispatcher_update_area_async(RedDispatcher *dispatcher,
                                         uint32_t surface_id,
                                         QXLRect *qxl_area,
                                         uint32_t clear_dirty_region,
                                         uint64_t cookie)
{
    RedWorkerMessage message = RED_WORKER_MESSAGE_UPDATE_ASYNC;
    RedWorkerMessageUpdateAsync payload;

    payload.base.cmd = async_command_alloc(dispatcher, message, cookie);
    payload.surface_id = surface_id;
    payload.qxl_area = *qxl_area;
    payload.clear_dirty_region = clear_dirty_region;
    dispatcher_send_message(&dispatcher->dispatcher,
                            message,
                            &payload);
}

static void qxl_worker_update_area(QXLWorker *qxl_worker, uint32_t surface_id,
                                   QXLRect *qxl_area, QXLRect *qxl_dirty_rects,
                                   uint32_t num_dirty_rects, uint32_t clear_dirty_region)
{
    red_dispatcher_update_area((RedDispatcher*)qxl_worker, surface_id, qxl_area,
                               qxl_dirty_rects, num_dirty_rects, clear_dirty_region);
}

static void red_dispatcher_add_memslot(RedDispatcher *dispatcher, QXLDevMemSlot *mem_slot)
{
    RedWorkerMessageAddMemslot payload;

    payload.mem_slot = *mem_slot;
    dispatcher_send_message(&dispatcher->dispatcher,
                            RED_WORKER_MESSAGE_ADD_MEMSLOT,
                            &payload);
}

static void qxl_worker_add_memslot(QXLWorker *qxl_worker, QXLDevMemSlot *mem_slot)
{
    red_dispatcher_add_memslot((RedDispatcher*)qxl_worker, mem_slot);
}

static void red_dispatcher_add_memslot_async(RedDispatcher *dispatcher, QXLDevMemSlot *mem_slot, uint64_t cookie)
{
    RedWorkerMessageAddMemslotAsync payload;
    RedWorkerMessage message = RED_WORKER_MESSAGE_ADD_MEMSLOT_ASYNC;

    payload.base.cmd = async_command_alloc(dispatcher, message, cookie);
    payload.mem_slot = *mem_slot;
    dispatcher_send_message(&dispatcher->dispatcher, message, &payload);
}

static void red_dispatcher_del_memslot(RedDispatcher *dispatcher, uint32_t slot_group_id, uint32_t slot_id)
{
    RedWorkerMessageDelMemslot payload;
    RedWorkerMessage message = RED_WORKER_MESSAGE_DEL_MEMSLOT;

    payload.slot_group_id = slot_group_id;
    payload.slot_id = slot_id;
    dispatcher_send_message(&dispatcher->dispatcher, message, &payload);
}

static void qxl_worker_del_memslot(QXLWorker *qxl_worker, uint32_t slot_group_id, uint32_t slot_id)
{
    red_dispatcher_del_memslot((RedDispatcher*)qxl_worker, slot_group_id, slot_id);
}

static void red_dispatcher_destroy_surfaces(RedDispatcher *dispatcher)
{
    RedWorkerMessageDestroySurfaces payload;

    dispatcher_send_message(&dispatcher->dispatcher,
                            RED_WORKER_MESSAGE_DESTROY_SURFACES,
                            &payload);
}

static void qxl_worker_destroy_surfaces(QXLWorker *qxl_worker)
{
    red_dispatcher_destroy_surfaces((RedDispatcher*)qxl_worker);
}

static void red_dispatcher_destroy_surfaces_async(RedDispatcher *dispatcher, uint64_t cookie)
{
    RedWorkerMessageDestroySurfacesAsync payload;
    RedWorkerMessage message = RED_WORKER_MESSAGE_DESTROY_SURFACES_ASYNC;

    payload.base.cmd = async_command_alloc(dispatcher, message, cookie);
    dispatcher_send_message(&dispatcher->dispatcher, message, &payload);
}

static void red_dispatcher_destroy_primary_surface_complete(RedDispatcher *dispatcher)
{
    dispatcher->x_res = 0;
    dispatcher->y_res = 0;
    dispatcher->use_hardware_cursor = FALSE;
    dispatcher->primary_active = FALSE;

    update_client_mouse_allowed();
}

static void
red_dispatcher_destroy_primary_surface_sync(RedDispatcher *dispatcher,
                                            uint32_t surface_id)
{
    RedWorkerMessageDestroyPrimarySurface payload;
    payload.surface_id = surface_id;
    dispatcher_send_message(&dispatcher->dispatcher,
                            RED_WORKER_MESSAGE_DESTROY_PRIMARY_SURFACE,
                            &payload);
    red_dispatcher_destroy_primary_surface_complete(dispatcher);
}

static void
red_dispatcher_destroy_primary_surface_async(RedDispatcher *dispatcher,
                                             uint32_t surface_id, uint64_t cookie)
{
    RedWorkerMessageDestroyPrimarySurfaceAsync payload;
    RedWorkerMessage message = RED_WORKER_MESSAGE_DESTROY_PRIMARY_SURFACE_ASYNC;

    payload.base.cmd = async_command_alloc(dispatcher, message, cookie);
    payload.surface_id = surface_id;
    dispatcher_send_message(&dispatcher->dispatcher, message, &payload);
}

static void
red_dispatcher_destroy_primary_surface(RedDispatcher *dispatcher,
                                       uint32_t surface_id, int async, uint64_t cookie)
{
    if (async) {
        red_dispatcher_destroy_primary_surface_async(dispatcher, surface_id, cookie);
    } else {
        red_dispatcher_destroy_primary_surface_sync(dispatcher, surface_id);
    }
}

static void qxl_worker_destroy_primary_surface(QXLWorker *qxl_worker, uint32_t surface_id)
{
    red_dispatcher_destroy_primary_surface((RedDispatcher*)qxl_worker, surface_id, 0, 0);
}

static void red_dispatcher_create_primary_surface_complete(RedDispatcher *dispatcher)
{
    QXLDevSurfaceCreate *surface = &dispatcher->surface_create;

    dispatcher->x_res = surface->width;
    dispatcher->y_res = surface->height;
    dispatcher->use_hardware_cursor = surface->mouse_mode;
    dispatcher->primary_active = TRUE;

    update_client_mouse_allowed();
    memset(&dispatcher->surface_create, 0, sizeof(QXLDevSurfaceCreate));
}

static void
red_dispatcher_create_primary_surface_async(RedDispatcher *dispatcher, uint32_t surface_id,
                                            QXLDevSurfaceCreate *surface, uint64_t cookie)
{
    RedWorkerMessageCreatePrimarySurfaceAsync payload;
    RedWorkerMessage message = RED_WORKER_MESSAGE_CREATE_PRIMARY_SURFACE_ASYNC;

    dispatcher->surface_create = *surface;
    payload.base.cmd = async_command_alloc(dispatcher, message, cookie);
    payload.surface_id = surface_id;
    payload.surface = *surface;
    dispatcher_send_message(&dispatcher->dispatcher, message, &payload);
}

static void
red_dispatcher_create_primary_surface_sync(RedDispatcher *dispatcher, uint32_t surface_id,
                                           QXLDevSurfaceCreate *surface)
{
    RedWorkerMessageCreatePrimarySurface payload = {0,};

    dispatcher->surface_create = *surface;
    payload.surface_id = surface_id;
    payload.surface = *surface;
    dispatcher_send_message(&dispatcher->dispatcher,
                            RED_WORKER_MESSAGE_CREATE_PRIMARY_SURFACE,
                            &payload);
    red_dispatcher_create_primary_surface_complete(dispatcher);
}

static void
red_dispatcher_create_primary_surface(RedDispatcher *dispatcher, uint32_t surface_id,
                                      QXLDevSurfaceCreate *surface, int async, uint64_t cookie)
{
    if (async) {
        red_dispatcher_create_primary_surface_async(dispatcher, surface_id, surface, cookie);
    } else {
        red_dispatcher_create_primary_surface_sync(dispatcher, surface_id, surface);
    }
}

static void qxl_worker_create_primary_surface(QXLWorker *qxl_worker, uint32_t surface_id,
                                      QXLDevSurfaceCreate *surface)
{
    red_dispatcher_create_primary_surface((RedDispatcher*)qxl_worker, surface_id, surface, 0, 0);
}

static void red_dispatcher_reset_image_cache(RedDispatcher *dispatcher)
{
    RedWorkerMessageResetImageCache payload;

    dispatcher_send_message(&dispatcher->dispatcher,
                            RED_WORKER_MESSAGE_RESET_IMAGE_CACHE,
                            &payload);
}

static void qxl_worker_reset_image_cache(QXLWorker *qxl_worker)
{
    red_dispatcher_reset_image_cache((RedDispatcher*)qxl_worker);
}

static void red_dispatcher_reset_cursor(RedDispatcher *dispatcher)
{
    RedWorkerMessageResetCursor payload;

    dispatcher_send_message(&dispatcher->dispatcher,
                            RED_WORKER_MESSAGE_RESET_CURSOR,
                            &payload);
}

static void qxl_worker_reset_cursor(QXLWorker *qxl_worker)
{
    red_dispatcher_reset_cursor((RedDispatcher*)qxl_worker);
}

static void red_dispatcher_destroy_surface_wait_sync(RedDispatcher *dispatcher,
                                                     uint32_t surface_id)
{
    RedWorkerMessageDestroySurfaceWait payload;

    payload.surface_id = surface_id;
    dispatcher_send_message(&dispatcher->dispatcher,
                            RED_WORKER_MESSAGE_DESTROY_SURFACE_WAIT,
                            &payload);
}

static void red_dispatcher_destroy_surface_wait_async(RedDispatcher *dispatcher,
                                                      uint32_t surface_id,
                                                      uint64_t cookie)
{
    RedWorkerMessageDestroySurfaceWaitAsync payload;
    RedWorkerMessage message = RED_WORKER_MESSAGE_DESTROY_SURFACE_WAIT_ASYNC;

    payload.base.cmd = async_command_alloc(dispatcher, message, cookie);
    payload.surface_id = surface_id;
    dispatcher_send_message(&dispatcher->dispatcher, message, &payload);
}

static void red_dispatcher_destroy_surface_wait(RedDispatcher *dispatcher,
                                                uint32_t surface_id,
                                                int async, uint64_t cookie)
{
    if (async) {
        red_dispatcher_destroy_surface_wait_async(dispatcher, surface_id, cookie);
    } else {
        red_dispatcher_destroy_surface_wait_sync(dispatcher, surface_id);
    }
}

static void qxl_worker_destroy_surface_wait(QXLWorker *qxl_worker, uint32_t surface_id)
{
    red_dispatcher_destroy_surface_wait((RedDispatcher*)qxl_worker, surface_id, 0, 0);
}

static void red_dispatcher_reset_memslots(RedDispatcher *dispatcher)
{
    RedWorkerMessageResetMemslots payload;

    dispatcher_send_message(&dispatcher->dispatcher,
                            RED_WORKER_MESSAGE_RESET_MEMSLOTS,
                            &payload);
}

static void qxl_worker_reset_memslots(QXLWorker *qxl_worker)
{
    red_dispatcher_reset_memslots((RedDispatcher*)qxl_worker);
}

static bool red_dispatcher_set_pending(RedDispatcher *dispatcher, int pending)
{
    // TODO: this is not atomic, do we need atomic?
    if (test_bit(pending, dispatcher->pending)) {
        return TRUE;
    }

    set_bit(pending, &dispatcher->pending);
    return FALSE;
}

static void red_dispatcher_wakeup(RedDispatcher *dispatcher)
{
    RedWorkerMessageWakeup payload;

    if (red_dispatcher_set_pending(dispatcher, RED_DISPATCHER_PENDING_WAKEUP))
        return;

    dispatcher_send_message(&dispatcher->dispatcher,
                            RED_WORKER_MESSAGE_WAKEUP,
                            &payload);
}

static void qxl_worker_wakeup(QXLWorker *qxl_worker)
{
    red_dispatcher_wakeup((RedDispatcher*)qxl_worker);
}

static void red_dispatcher_oom(RedDispatcher *dispatcher)
{
    RedWorkerMessageOom payload;

    if (red_dispatcher_set_pending(dispatcher, RED_DISPATCHER_PENDING_OOM))
        return;

    dispatcher_send_message(&dispatcher->dispatcher,
                            RED_WORKER_MESSAGE_OOM,
                            &payload);
}

static void qxl_worker_oom(QXLWorker *qxl_worker)
{
    red_dispatcher_oom((RedDispatcher*)qxl_worker);
}

static void red_dispatcher_start(RedDispatcher *dispatcher)
{
    RedWorkerMessageStart payload;

    dispatcher_send_message(&dispatcher->dispatcher,
                            RED_WORKER_MESSAGE_START,
                            &payload);
}

static void qxl_worker_start(QXLWorker *qxl_worker)
{
    red_dispatcher_start((RedDispatcher*)qxl_worker);
}

static void red_dispatcher_flush_surfaces_async(RedDispatcher *dispatcher, uint64_t cookie)
{
    RedWorkerMessageFlushSurfacesAsync payload;
    RedWorkerMessage message = RED_WORKER_MESSAGE_FLUSH_SURFACES_ASYNC;

    payload.base.cmd = async_command_alloc(dispatcher, message, cookie);
    dispatcher_send_message(&dispatcher->dispatcher, message, &payload);
}

static void red_dispatcher_monitors_config_async(RedDispatcher *dispatcher,
                                                 QXLPHYSICAL monitors_config,
                                                 int group_id,
                                                 uint64_t cookie)
{
    RedWorkerMessageMonitorsConfigAsync payload;
    RedWorkerMessage message = RED_WORKER_MESSAGE_MONITORS_CONFIG_ASYNC;

    payload.base.cmd = async_command_alloc(dispatcher, message, cookie);
    payload.monitors_config = monitors_config;
    payload.group_id = group_id;

    dispatcher_send_message(&dispatcher->dispatcher, message, &payload);
}

static void red_dispatcher_driver_unload(RedDispatcher *dispatcher)
{
    RedWorkerMessageDriverUnload payload;

    dispatcher_send_message(&dispatcher->dispatcher,
                            RED_WORKER_MESSAGE_DRIVER_UNLOAD,
                            &payload);
}

static void red_dispatcher_stop(RedDispatcher *dispatcher)
{
    RedWorkerMessageStop payload;

    dispatcher_send_message(&dispatcher->dispatcher,
                            RED_WORKER_MESSAGE_STOP,
                            &payload);
}

static void qxl_worker_stop(QXLWorker *qxl_worker)
{
    red_dispatcher_stop((RedDispatcher*)qxl_worker);
}

static void red_dispatcher_loadvm_commands(RedDispatcher *dispatcher,
                                           struct QXLCommandExt *ext,
                                           uint32_t count)
{
    RedWorkerMessageLoadvmCommands payload;

    spice_printerr("");
    payload.count = count;
    payload.ext = ext;
    dispatcher_send_message(&dispatcher->dispatcher,
                            RED_WORKER_MESSAGE_LOADVM_COMMANDS,
                            &payload);
}

static void qxl_worker_loadvm_commands(QXLWorker *qxl_worker,
                                       struct QXLCommandExt *ext,
                                       uint32_t count)
{
    red_dispatcher_loadvm_commands((RedDispatcher*)qxl_worker, ext, count);
}

void red_dispatcher_set_mm_time(uint32_t mm_time)
{
    RedDispatcher *now = dispatchers;
    while (now) {
        now->qxl->st->qif->set_mm_time(now->qxl, mm_time);
        now = now->next;
    }
}

static int calc_compression_level(void)
{
    spice_return_val_if_fail(streaming_video != STREAM_VIDEO_INVALID, -1);

    if ((streaming_video != STREAM_VIDEO_OFF) ||
        (image_compression != SPICE_IMAGE_COMPRESS_QUIC)) {
        return 0;
    } else {
        return 1;
    }
}

void red_dispatcher_on_ic_change(void)
{
    RedWorkerMessageSetCompression payload;
    int compression_level = calc_compression_level();
    RedDispatcher *now = dispatchers;

    while (now) {
        now->qxl->st->qif->set_compression_level(now->qxl, compression_level);
        payload.image_compression = image_compression;
        dispatcher_send_message(&now->dispatcher,
                                RED_WORKER_MESSAGE_SET_COMPRESSION,
                                &payload);
        now = now->next;
    }
}

void red_dispatcher_on_sv_change(void)
{
    RedWorkerMessageSetStreamingVideo payload;
    int compression_level = calc_compression_level();
    RedDispatcher *now = dispatchers;
    while (now) {
        now->qxl->st->qif->set_compression_level(now->qxl, compression_level);
        payload.streaming_video = streaming_video;
        dispatcher_send_message(&now->dispatcher,
                                RED_WORKER_MESSAGE_SET_STREAMING_VIDEO,
                                &payload);
        now = now->next;
    }
}

void red_dispatcher_set_mouse_mode(uint32_t mode)
{
    RedWorkerMessageSetMouseMode payload;
    RedDispatcher *now = dispatchers;
    while (now) {
        payload.mode = mode;
        dispatcher_send_message(&now->dispatcher,
                                RED_WORKER_MESSAGE_SET_MOUSE_MODE,
                                &payload);
        now = now->next;
    }
}

void red_dispatcher_on_vm_stop(void)
{
    RedDispatcher *now = dispatchers;

    spice_debug(NULL);
    while (now) {
        red_dispatcher_stop(now);
        now = now->next;
    }
}

void red_dispatcher_on_vm_start(void)
{
    RedDispatcher *now = dispatchers;

    spice_debug(NULL);
    while (now) {
        red_dispatcher_start(now);
        now = now->next;
    }
}

int red_dispatcher_count(void)
{
    RedDispatcher *now = dispatchers;
    int ret = 0;

    while (now) {
        ret++;
        now = now->next;
    }
    return ret;
}

uint32_t red_dispatcher_qxl_ram_size(void)
{
    QXLDevInitInfo qxl_info;
    if (!dispatchers) {
        return 0;
    }
    dispatchers->qxl->st->qif->get_init_info(dispatchers->qxl, &qxl_info);
    return qxl_info.qxl_ram_size;
}

SPICE_GNUC_VISIBLE
void spice_qxl_wakeup(QXLInstance *instance)
{
    red_dispatcher_wakeup(instance->st->dispatcher);
}

SPICE_GNUC_VISIBLE
void spice_qxl_oom(QXLInstance *instance)
{
    red_dispatcher_oom(instance->st->dispatcher);
}

SPICE_GNUC_VISIBLE
void spice_qxl_start(QXLInstance *instance)
{
    red_dispatcher_start(instance->st->dispatcher);
}

SPICE_GNUC_VISIBLE
void spice_qxl_stop(QXLInstance *instance)
{
    red_dispatcher_stop(instance->st->dispatcher);
}

SPICE_GNUC_VISIBLE
void spice_qxl_update_area(QXLInstance *instance, uint32_t surface_id,
                    struct QXLRect *area, struct QXLRect *dirty_rects,
                    uint32_t num_dirty_rects, uint32_t clear_dirty_region)
{
    red_dispatcher_update_area(instance->st->dispatcher, surface_id, area, dirty_rects,
                               num_dirty_rects, clear_dirty_region);
}

SPICE_GNUC_VISIBLE
void spice_qxl_add_memslot(QXLInstance *instance, QXLDevMemSlot *slot)
{
    red_dispatcher_add_memslot(instance->st->dispatcher, slot);
}

SPICE_GNUC_VISIBLE
void spice_qxl_del_memslot(QXLInstance *instance, uint32_t slot_group_id, uint32_t slot_id)
{
    red_dispatcher_del_memslot(instance->st->dispatcher, slot_group_id, slot_id);
}

SPICE_GNUC_VISIBLE
void spice_qxl_reset_memslots(QXLInstance *instance)
{
    red_dispatcher_reset_memslots(instance->st->dispatcher);
}

SPICE_GNUC_VISIBLE
void spice_qxl_destroy_surfaces(QXLInstance *instance)
{
    red_dispatcher_destroy_surfaces(instance->st->dispatcher);
}

SPICE_GNUC_VISIBLE
void spice_qxl_destroy_primary_surface(QXLInstance *instance, uint32_t surface_id)
{
    red_dispatcher_destroy_primary_surface(instance->st->dispatcher, surface_id, 0, 0);
}

SPICE_GNUC_VISIBLE
void spice_qxl_create_primary_surface(QXLInstance *instance, uint32_t surface_id,
                                QXLDevSurfaceCreate *surface)
{
    red_dispatcher_create_primary_surface(instance->st->dispatcher, surface_id, surface, 0, 0);
}

SPICE_GNUC_VISIBLE
void spice_qxl_reset_image_cache(QXLInstance *instance)
{
    red_dispatcher_reset_image_cache(instance->st->dispatcher);
}

SPICE_GNUC_VISIBLE
void spice_qxl_reset_cursor(QXLInstance *instance)
{
    red_dispatcher_reset_cursor(instance->st->dispatcher);
}

SPICE_GNUC_VISIBLE
void spice_qxl_destroy_surface_wait(QXLInstance *instance, uint32_t surface_id)
{
    red_dispatcher_destroy_surface_wait(instance->st->dispatcher, surface_id, 0, 0);
}

SPICE_GNUC_VISIBLE
void spice_qxl_loadvm_commands(QXLInstance *instance, struct QXLCommandExt *ext, uint32_t count)
{
    red_dispatcher_loadvm_commands(instance->st->dispatcher, ext, count);
}

SPICE_GNUC_VISIBLE
void spice_qxl_update_area_async(QXLInstance *instance, uint32_t surface_id, QXLRect *qxl_area,
                                 uint32_t clear_dirty_region, uint64_t cookie)
{
    red_dispatcher_update_area_async(instance->st->dispatcher, surface_id, qxl_area,
                                     clear_dirty_region, cookie);
}

SPICE_GNUC_VISIBLE
void spice_qxl_add_memslot_async(QXLInstance *instance, QXLDevMemSlot *slot, uint64_t cookie)
{
    red_dispatcher_add_memslot_async(instance->st->dispatcher, slot, cookie);
}

SPICE_GNUC_VISIBLE
void spice_qxl_destroy_surfaces_async(QXLInstance *instance, uint64_t cookie)
{
    red_dispatcher_destroy_surfaces_async(instance->st->dispatcher, cookie);
}

SPICE_GNUC_VISIBLE
void spice_qxl_destroy_primary_surface_async(QXLInstance *instance, uint32_t surface_id, uint64_t cookie)
{
    red_dispatcher_destroy_primary_surface(instance->st->dispatcher, surface_id, 1, cookie);
}

SPICE_GNUC_VISIBLE
void spice_qxl_create_primary_surface_async(QXLInstance *instance, uint32_t surface_id,
                                QXLDevSurfaceCreate *surface, uint64_t cookie)
{
    red_dispatcher_create_primary_surface(instance->st->dispatcher, surface_id, surface, 1, cookie);
}

SPICE_GNUC_VISIBLE
void spice_qxl_destroy_surface_async(QXLInstance *instance, uint32_t surface_id, uint64_t cookie)
{
    red_dispatcher_destroy_surface_wait(instance->st->dispatcher, surface_id, 1, cookie);
}

SPICE_GNUC_VISIBLE
void spice_qxl_flush_surfaces_async(QXLInstance *instance, uint64_t cookie)
{
    red_dispatcher_flush_surfaces_async(instance->st->dispatcher, cookie);
}

SPICE_GNUC_VISIBLE
void spice_qxl_monitors_config_async(QXLInstance *instance, QXLPHYSICAL monitors_config,
                                     int group_id, uint64_t cookie)
{
    red_dispatcher_monitors_config_async(instance->st->dispatcher, monitors_config, group_id, cookie);
}

SPICE_GNUC_VISIBLE
void spice_qxl_driver_unload(QXLInstance *instance)
{
    red_dispatcher_driver_unload(instance->st->dispatcher);
}

void red_dispatcher_async_complete(struct RedDispatcher *dispatcher,
                                   AsyncCommand *async_command)
{
    pthread_mutex_lock(&dispatcher->async_lock);
    ring_remove(&async_command->link);
    spice_debug("%p: cookie %" PRId64, async_command, async_command->cookie);
    if (ring_is_empty(&dispatcher->async_commands)) {
        spice_debug("no more async commands");
    }
    pthread_mutex_unlock(&dispatcher->async_lock);
    switch (async_command->message) {
    case RED_WORKER_MESSAGE_UPDATE_ASYNC:
        break;
    case RED_WORKER_MESSAGE_ADD_MEMSLOT_ASYNC:
        break;
    case RED_WORKER_MESSAGE_DESTROY_SURFACES_ASYNC:
        break;
    case RED_WORKER_MESSAGE_CREATE_PRIMARY_SURFACE_ASYNC:
        red_dispatcher_create_primary_surface_complete(dispatcher);
        break;
    case RED_WORKER_MESSAGE_DESTROY_PRIMARY_SURFACE_ASYNC:
        red_dispatcher_destroy_primary_surface_complete(dispatcher);
        break;
    case RED_WORKER_MESSAGE_DESTROY_SURFACE_WAIT_ASYNC:
        break;
    case RED_WORKER_MESSAGE_FLUSH_SURFACES_ASYNC:
        break;
    case RED_WORKER_MESSAGE_MONITORS_CONFIG_ASYNC:
        break;
    default:
        spice_warning("unexpected message %d", async_command->message);
    }
    dispatcher->qxl->st->qif->async_complete(dispatcher->qxl,
                                             async_command->cookie);
    free(async_command);
}

RedDispatcher *red_dispatcher_new(QXLInstance *qxl)
{
    RedDispatcher *red_dispatcher;
    RedChannel *channel;
    ClientCbs client_cbs = { NULL, };

    spice_return_val_if_fail(qxl != NULL, NULL);
    spice_return_val_if_fail(qxl->st->dispatcher == NULL, NULL);

    static gsize initialized = FALSE;
    if (g_once_init_enter(&initialized)) {
        quic_init();
        sw_canvas_init();
        g_once_init_leave(&initialized, TRUE);
    }

    red_dispatcher = spice_new0(RedDispatcher, 1);
    red_dispatcher->qxl = qxl;
    ring_init(&red_dispatcher->async_commands);
    spice_debug("red_dispatcher->async_commands.next %p", red_dispatcher->async_commands.next);
    dispatcher_init(&red_dispatcher->dispatcher, RED_WORKER_MESSAGE_COUNT, NULL);
    pthread_mutex_init(&red_dispatcher->async_lock, NULL);
    red_dispatcher->base.major_version = SPICE_INTERFACE_QXL_MAJOR;
    red_dispatcher->base.minor_version = SPICE_INTERFACE_QXL_MINOR;
    red_dispatcher->base.wakeup = qxl_worker_wakeup;
    red_dispatcher->base.oom = qxl_worker_oom;
    red_dispatcher->base.start = qxl_worker_start;
    red_dispatcher->base.stop = qxl_worker_stop;
    red_dispatcher->base.update_area = qxl_worker_update_area;
    red_dispatcher->base.add_memslot = qxl_worker_add_memslot;
    red_dispatcher->base.del_memslot = qxl_worker_del_memslot;
    red_dispatcher->base.reset_memslots = qxl_worker_reset_memslots;
    red_dispatcher->base.destroy_surfaces = qxl_worker_destroy_surfaces;
    red_dispatcher->base.create_primary_surface = qxl_worker_create_primary_surface;
    red_dispatcher->base.destroy_primary_surface = qxl_worker_destroy_primary_surface;
    red_dispatcher->base.reset_image_cache = qxl_worker_reset_image_cache;
    red_dispatcher->base.reset_cursor = qxl_worker_reset_cursor;
    red_dispatcher->base.destroy_surface_wait = qxl_worker_destroy_surface_wait;
    red_dispatcher->base.loadvm_commands = qxl_worker_loadvm_commands;

    // TODO: reference and free
    RedWorker *worker = red_worker_new(qxl, red_dispatcher);

    // TODO: move to their respective channel files
    channel = red_worker_get_cursor_channel(worker);
    client_cbs.connect = red_dispatcher_set_cursor_peer;
    client_cbs.disconnect = red_dispatcher_disconnect_cursor_peer;
    client_cbs.migrate = red_dispatcher_cursor_migrate;
    red_channel_register_client_cbs(channel, &client_cbs);
    red_channel_set_data(channel, red_dispatcher);
    reds_register_channel(channel);

    channel = red_worker_get_display_channel(worker);
    client_cbs.connect = red_dispatcher_set_display_peer;
    client_cbs.disconnect = red_dispatcher_disconnect_display_peer;
    client_cbs.migrate = red_dispatcher_display_migrate;
    red_channel_register_client_cbs(channel, &client_cbs);
    red_channel_set_data(channel, red_dispatcher);
    red_channel_set_cap(channel, SPICE_DISPLAY_CAP_MONITORS_CONFIG);
    red_channel_set_cap(channel, SPICE_DISPLAY_CAP_STREAM_REPORT);
    reds_register_channel(channel);

    red_worker_run(worker);
    num_active_workers = 1;

    qxl->st->dispatcher = red_dispatcher;
    red_dispatcher->next = dispatchers;
    dispatchers = red_dispatcher;

    qxl->st->qif->attache_worker(qxl, &red_dispatcher->base);
    qxl->st->qif->set_compression_level(qxl, calc_compression_level());

    return red_dispatcher;
}

struct Dispatcher *red_dispatcher_get_dispatcher(RedDispatcher *red_dispatcher)
{
    return &red_dispatcher->dispatcher;
}

void red_dispatcher_set_dispatcher_opaque(RedDispatcher *red_dispatcher,
                                          void *opaque)
{
    dispatcher_set_opaque(&red_dispatcher->dispatcher, opaque);
}

void red_dispatcher_clear_pending(RedDispatcher *red_dispatcher, int pending)
{
    spice_return_if_fail(red_dispatcher != NULL);

    clear_bit(pending, &red_dispatcher->pending);
}
