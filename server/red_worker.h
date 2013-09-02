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

typedef struct RedWorker RedWorker;

typedef struct CommonChannelClient {
    RedChannelClient base;
    uint32_t id;
    struct RedWorker *worker;
    int is_low_bandwidth;
} CommonChannelClient;

#define CHANNEL_RECEIVE_BUF_SIZE 1024
typedef struct CommonChannel {
    RedChannel base; // Must be the first thing
    struct RedWorker *worker;
    uint8_t recv_buf[CHANNEL_RECEIVE_BUF_SIZE];
    uint32_t id_alloc; // bitfield. TODO - use this instead of shift scheme.
    int during_target_migrate; /* TRUE when the client that is associated with the channel
                                  is during migration. Turned off when the vm is started.
                                  The flag is used to avoid sending messages that are artifacts
                                  of the transition from stopped vm to loaded vm (e.g., recreation
                                  of the primary surface) */
} CommonChannel;

RedWorker* red_worker_new(QXLInstance *qxl, RedDispatcher *red_dispatcher);
bool       red_worker_run(RedWorker *worker);

#endif
