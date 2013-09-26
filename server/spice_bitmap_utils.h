#ifndef H_SPICE_BITMAP_UTILS
#define H_SPICE_BITMAP_UTILS

void dump_bitmap(SpiceBitmap *bitmap);

int spice_bitmap_from_surface_type(uint32_t surface_format);

uint8_t *spice_bitmap_get_line(const SpiceBitmap *image, size_t *offset,
                               int *chunk_nr, int stride);

#endif
