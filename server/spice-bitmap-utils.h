#ifndef H_SPICE_BITMAP_UTILS
#define H_SPICE_BITMAP_UTILS

#include "common.h"

typedef enum {
    BITMAP_GRADUAL_INVALID,
    BITMAP_GRADUAL_NOT_AVAIL,
    BITMAP_GRADUAL_LOW,
    BITMAP_GRADUAL_MEDIUM,
    BITMAP_GRADUAL_HIGH,
} BitmapGradualType;

typedef struct {
    uint8_t b;
    uint8_t g;
    uint8_t r;
    uint8_t pad;
} rgb32_pixel_t;

G_STATIC_ASSERT(sizeof(rgb32_pixel_t) == 4);

typedef struct {
    uint8_t b;
    uint8_t g;
    uint8_t r;
} rgb24_pixel_t;

G_STATIC_ASSERT(sizeof(rgb24_pixel_t) == 3);

typedef uint16_t rgb16_pixel_t;


static inline int bitmap_fmt_get_bytes_per_pixel(uint8_t fmt)
{
    static const int bytes_per_pixel[] = {0, 0, 0, 0, 0, 1, 2, 3, 4, 4, 1};

    spice_return_val_if_fail(fmt < SPICE_N_ELEMENTS(bytes_per_pixel), 0);

    return bytes_per_pixel[fmt];
}


static inline int bitmap_fmt_is_plt(uint8_t fmt)
{
    static const int fmt_is_plt[] = {0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};

    spice_return_val_if_fail(fmt < SPICE_N_ELEMENTS(fmt_is_plt), 0);

    return fmt_is_plt[fmt];
}

static inline int bitmap_fmt_is_rgb(uint8_t fmt)
{
    static const int fmt_is_rgb[] = {0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1};

    spice_return_val_if_fail(fmt < SPICE_N_ELEMENTS(fmt_is_rgb), 0);

    return fmt_is_rgb[fmt];
}

static inline int bitmap_fmt_has_graduality(uint8_t fmt)
{
    return bitmap_fmt_is_rgb(fmt) && fmt != SPICE_BITMAP_FMT_8BIT_A;
}


BitmapGradualType bitmap_get_graduality_level     (SpiceBitmap *bitmap);
int               bitmap_has_extra_stride         (SpiceBitmap *bitmap);

void dump_bitmap(SpiceBitmap *bitmap);

int spice_bitmap_from_surface_type(uint32_t surface_format);

uint8_t *spice_bitmap_get_line(const SpiceBitmap *image, size_t *offset,
                               int *chunk_nr, int stride);

#endif
