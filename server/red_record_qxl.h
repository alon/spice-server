/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009,2010 Red Hat, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of
   the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef RED_ABI_RECORD_H
#define RED_ABI_RECORD_H

#include <spice/qxl_dev.h>
#include "red_common.h"
#include "red_memslots.h"

void red_record_drawable(FILE *fd, RedMemSlotInfo *slots, int group_id,
                         QXLPHYSICAL addr, uint32_t flags);

void red_record_update_cmd(FILE *fd, RedMemSlotInfo *slots, int group_id,
                           QXLPHYSICAL addr);

void red_record_message(FILE *fd, RedMemSlotInfo *slots, int group_id,
                        QXLPHYSICAL addr);

void red_record_surface_cmd(FILE *fd, RedMemSlotInfo *slots, int group_id,
                            QXLPHYSICAL addr);

void red_record_cursor_cmd(FILE *fd, RedMemSlotInfo *slots, int group_id,
                           QXLPHYSICAL addr);

void red_record_dev_input_primary_surface_create(
                           FILE *fd, QXLDevSurfaceCreate *surface, uint8_t *line_0);

#endif