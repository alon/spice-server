#include "canvas.h"

#define SPICE_CANVAS_INTERNAL
#define SW_CANVAS_IMAGE_CACHE
#include "common/sw_canvas.c"
#undef SW_CANVAS_IMAGE_CACHE
#undef SPICE_CANVAS_INTERNAL
