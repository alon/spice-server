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

#ifndef _H_COMMON
#define _H_COMMON

#include <glib.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <spice/macros.h>
#include "common/log.h"
#include "common/lz_common.h"
#include "common/marshaller.h"
#include "common/mem.h"
#include "common/messages.h"
#include "common/ring.h"
#include "common/spice_common.h"
#include "common/draw.h"

#include "spice.h"
#include "utils.h"

#define SPICE_GNUC_VISIBLE __attribute__ ((visibility ("default")))

#endif
