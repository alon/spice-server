#include "spice-bitmap-utils.h"

#define RED_BITMAP_UTILS_RGB16
#include "spice-bitmap-utils.c.template"
#define RED_BITMAP_UTILS_RGB24
#include "spice-bitmap-utils.c.template"
#define RED_BITMAP_UTILS_RGB32
#include "spice-bitmap-utils.c.template"

#define GRADUAL_HIGH_RGB24_TH -0.03
#define GRADUAL_HIGH_RGB16_TH 0

// setting a more permissive threshold for stream identification in order
// not to miss streams that were artificially scaled on the guest (e.g., full screen view
// in window media player 12). see red_stream_add_frame
#define GRADUAL_MEDIUM_SCORE_TH 0.002

// assumes that stride doesn't overflow
BitmapGradualType bitmap_get_graduality_level(SpiceBitmap *bitmap)
{
    double score = 0.0;
    int num_samples = 0;
    int num_lines;
    double chunk_score = 0.0;
    int chunk_num_samples = 0;
    uint32_t x, i;
    SpiceChunk *chunk;

    chunk = bitmap->data->chunk;
    for (i = 0; i < bitmap->data->num_chunks; i++) {
        num_lines = chunk[i].len / bitmap->stride;
        x = bitmap->x;
        switch (bitmap->format) {
        case SPICE_BITMAP_FMT_16BIT:
            compute_lines_gradual_score_rgb16((rgb16_pixel_t *)chunk[i].data, x, num_lines,
                                              &chunk_score, &chunk_num_samples);
            break;
        case SPICE_BITMAP_FMT_24BIT:
            compute_lines_gradual_score_rgb24((rgb24_pixel_t *)chunk[i].data, x, num_lines,
                                              &chunk_score, &chunk_num_samples);
            break;
        case SPICE_BITMAP_FMT_32BIT:
        case SPICE_BITMAP_FMT_RGBA:
            compute_lines_gradual_score_rgb32((rgb32_pixel_t *)chunk[i].data, x, num_lines,
                                              &chunk_score, &chunk_num_samples);
            break;
        default:
            spice_error("invalid bitmap format (not RGB) %u", bitmap->format);
        }
        score += chunk_score;
        num_samples += chunk_num_samples;
    }

    spice_assert(num_samples);
    score /= num_samples;

    if (bitmap->format == SPICE_BITMAP_FMT_16BIT) {
        if (score < GRADUAL_HIGH_RGB16_TH) {
            return BITMAP_GRADUAL_HIGH;
        }
    } else {
        if (score < GRADUAL_HIGH_RGB24_TH) {
            return BITMAP_GRADUAL_HIGH;
        }
    }

    if (score < GRADUAL_MEDIUM_SCORE_TH) {
        return BITMAP_GRADUAL_MEDIUM;
    } else {
        return BITMAP_GRADUAL_LOW;
    }
}

int bitmap_has_extra_stride(SpiceBitmap *bitmap)
{
    spice_assert(bitmap);
    if (bitmap_fmt_is_rgb(bitmap->format)) {
        return ((bitmap->x * bitmap_fmt_get_bytes_per_pixel(bitmap->format)) < bitmap->stride);
    } else {
        switch (bitmap->format) {
        case SPICE_BITMAP_FMT_8BIT:
            return (bitmap->x < bitmap->stride);
        case SPICE_BITMAP_FMT_4BIT_BE:
        case SPICE_BITMAP_FMT_4BIT_LE: {
            int bytes_width = SPICE_ALIGN(bitmap->x, 2) >> 1;
            return bytes_width < bitmap->stride;
        }
        case SPICE_BITMAP_FMT_1BIT_BE:
        case SPICE_BITMAP_FMT_1BIT_LE: {
            int bytes_width = SPICE_ALIGN(bitmap->x, 8) >> 3;
            return bytes_width < bitmap->stride;
        }
        default:
            spice_error("invalid image type %u", bitmap->format);
            return 0;
        }
    }
    return 0;
}

int spice_bitmap_from_surface_type(uint32_t surface_format)
{
    switch (surface_format) {
    case SPICE_SURFACE_FMT_16_555:
        return SPICE_BITMAP_FMT_16BIT;
    case SPICE_SURFACE_FMT_32_xRGB:
        return SPICE_BITMAP_FMT_32BIT;
    case SPICE_SURFACE_FMT_32_ARGB:
        return SPICE_BITMAP_FMT_RGBA;
    case SPICE_SURFACE_FMT_8_A:
        return SPICE_BITMAP_FMT_8BIT_A;
    default:
        spice_critical("Unsupported surface format");
    }
    return 0;
}

#define RAM_PATH "/tmp/tmpfs"

static void dump_palette(FILE *f, SpicePalette* plt)
{
    int i;
    for (i = 0; i < plt->num_ents; i++) {
        fwrite(plt->ents + i, sizeof(uint32_t), 1, f);
    }
}

static void dump_line(FILE *f, uint8_t* line, uint16_t n_pixel_bits, int width, int row_size)
{
    int i;
    int copy_bytes_size = SPICE_ALIGN(n_pixel_bits * width, 8) / 8;

    fwrite(line, 1, copy_bytes_size, f);
    if (row_size > copy_bytes_size) {
        // each line should be 4 bytes aligned
        for (i = copy_bytes_size; i < row_size; i++) {
            fprintf(f, "%c", 0);
        }
    }
}
void dump_bitmap(SpiceBitmap *bitmap)
{
    static uint32_t file_id = 0;

    char file_str[200];
    int rgb = TRUE;
    uint16_t n_pixel_bits;
    SpicePalette *plt = NULL;
    uint32_t id;
    int row_size;
    uint32_t file_size;
    int alpha = 0;
    uint32_t header_size = 14 + 40;
    uint32_t bitmap_data_offset;
    uint32_t tmp_u32;
    int32_t tmp_32;
    uint16_t tmp_u16;
    FILE *f;
    int i, j;

    switch (bitmap->format) {
    case SPICE_BITMAP_FMT_1BIT_BE:
    case SPICE_BITMAP_FMT_1BIT_LE:
        rgb = FALSE;
        n_pixel_bits = 1;
        break;
    case SPICE_BITMAP_FMT_4BIT_BE:
    case SPICE_BITMAP_FMT_4BIT_LE:
        rgb = FALSE;
        n_pixel_bits = 4;
        break;
    case SPICE_BITMAP_FMT_8BIT:
        rgb = FALSE;
        n_pixel_bits = 8;
        break;
    case SPICE_BITMAP_FMT_16BIT:
        n_pixel_bits = 16;
        break;
    case SPICE_BITMAP_FMT_24BIT:
        n_pixel_bits = 24;
        break;
    case SPICE_BITMAP_FMT_32BIT:
        n_pixel_bits = 32;
        break;
    case SPICE_BITMAP_FMT_RGBA:
        n_pixel_bits = 32;
        alpha = 1;
        break;
    default:
        spice_error("invalid bitmap format  %u", bitmap->format);
    }

    if (!rgb) {
        if (!bitmap->palette) {
            return; // dont dump masks.
        }
        plt = bitmap->palette;
    }
    row_size = (((bitmap->x * n_pixel_bits) + 31) / 32) * 4;
    bitmap_data_offset = header_size;

    if (plt) {
        bitmap_data_offset += plt->num_ents * 4;
    }
    file_size = bitmap_data_offset + (bitmap->y * row_size);

    id = ++file_id;
    sprintf(file_str, "%s/%u.bmp", RAM_PATH, id);

    f = fopen(file_str, "wb");
    if (!f) {
        spice_error("Error creating bmp");
        return;
    }

    /* writing the bmp v3 header */
    fprintf(f, "BM");
    fwrite(&file_size, sizeof(file_size), 1, f);
    tmp_u16 = alpha ? 1 : 0;
    fwrite(&tmp_u16, sizeof(tmp_u16), 1, f); // reserved for application
    tmp_u16 = 0;
    fwrite(&tmp_u16, sizeof(tmp_u16), 1, f);
    fwrite(&bitmap_data_offset, sizeof(bitmap_data_offset), 1, f);
    tmp_u32 = header_size - 14;
    fwrite(&tmp_u32, sizeof(tmp_u32), 1, f); // sub header size
    tmp_32 = bitmap->x;
    fwrite(&tmp_32, sizeof(tmp_32), 1, f);
    tmp_32 = bitmap->y;
    fwrite(&tmp_32, sizeof(tmp_32), 1, f);

    tmp_u16 = 1;
    fwrite(&tmp_u16, sizeof(tmp_u16), 1, f); // color plane
    fwrite(&n_pixel_bits, sizeof(n_pixel_bits), 1, f); // pixel depth

    tmp_u32 = 0;
    fwrite(&tmp_u32, sizeof(tmp_u32), 1, f); // compression method

    tmp_u32 = 0; //file_size - bitmap_data_offset;
    fwrite(&tmp_u32, sizeof(tmp_u32), 1, f); // image size
    tmp_32 = 0;
    fwrite(&tmp_32, sizeof(tmp_32), 1, f);
    fwrite(&tmp_32, sizeof(tmp_32), 1, f);
    tmp_u32 = (!plt) ? 0 : plt->num_ents; // plt entries
    fwrite(&tmp_u32, sizeof(tmp_u32), 1, f);
    tmp_u32 = 0;
    fwrite(&tmp_u32, sizeof(tmp_u32), 1, f);

    if (plt) {
        dump_palette(f, plt);
    }
    /* writing the data */
    for (i = 0; i < bitmap->data->num_chunks; i++) {
        SpiceChunk *chunk = &bitmap->data->chunk[i];
        int num_lines = chunk->len / bitmap->stride;
        for (j = 0; j < num_lines; j++) {
            dump_line(f, chunk->data + (j * bitmap->stride), n_pixel_bits, bitmap->x, row_size);
        }
    }
    fclose(f);
}

uint8_t *spice_bitmap_get_line(const SpiceBitmap *image, size_t *offset,
                               int *chunk_nr, int stride)
{
    SpiceChunks *chunks = image->data;
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
