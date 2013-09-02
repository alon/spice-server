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

#ifndef _H_REDWORKER
#define _H_REDWORKER

#include <unistd.h>
#include <errno.h>
#include "spice_server_utils.h"
#include "red_common.h"
#include "red_dispatcher.h"

enum {
    RED_WORKER_PENDING_WAKEUP,
    RED_WORKER_PENDING_OOM,
};

enum {
    RED_RENDERER_INVALID,
    RED_RENDERER_SW,
    RED_RENDERER_OGL_PBUF,
    RED_RENDERER_OGL_PIXMAP,

    RED_RENDERER_LAST
};

typedef struct WorkerInitData {
    struct QXLInstance *qxl;
    int id;
    uint32_t *pending;
    uint32_t num_renderers;
    uint32_t renderers[RED_RENDERER_LAST];
    spice_image_compression_t image_compression;
    spice_wan_compression_t jpeg_state;
    spice_wan_compression_t zlib_glz_state;
    int streaming_video;
    uint32_t num_memslots;
    uint32_t num_memslots_groups;
    uint8_t memslot_gen_bits;
    uint8_t memslot_id_bits;
    uint8_t internal_groupslot_id;
    uint32_t n_surfaces;
    RedDispatcher *red_dispatcher;
} WorkerInitData;

void *red_worker_main(void *arg);

#endif
