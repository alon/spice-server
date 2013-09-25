#include "dcc.h"
#include "display_channel.h"

static SurfaceCreateItem *surface_create_item_new(RedChannel* channel,
                                                  uint32_t surface_id, uint32_t width,
                                                  uint32_t height, uint32_t format, uint32_t flags)
{
    SurfaceCreateItem *create;

    create = (SurfaceCreateItem *)malloc(sizeof(SurfaceCreateItem));
    spice_warn_if(!create);

    create->surface_create.surface_id = surface_id;
    create->surface_create.width = width;
    create->surface_create.height = height;
    create->surface_create.flags = flags;
    create->surface_create.format = format;

    red_channel_pipe_item_init(channel,
                               &create->pipe_item, PIPE_ITEM_TYPE_CREATE_SURFACE);
    return create;
}

void dcc_create_surface(DisplayChannelClient *dcc, int surface_id)
{
    DisplayChannel *display = dcc ? DCC_TO_DC(dcc) : NULL;
    RedSurface *surface;
    SurfaceCreateItem *create;
    uint32_t flags = is_primary_surface(DCC_TO_DC(dcc), surface_id) ? SPICE_SURFACE_FLAGS_PRIMARY : 0;

    /* don't send redundant create surface commands to client */
    if (!dcc || display->common.during_target_migrate ||
        dcc->surface_client_created[surface_id]) {
        return;
    }
    surface = &display->surfaces[surface_id];
    create = surface_create_item_new(RED_CHANNEL_CLIENT(dcc)->channel,
                                     surface_id, surface->context.width, surface->context.height,
                                     surface->context.format, flags);
    dcc->surface_client_created[surface_id] = TRUE;
    red_channel_client_pipe_add(RED_CHANNEL_CLIENT(dcc), &create->pipe_item);
}

void dcc_push_surface_image(DisplayChannelClient *dcc, int surface_id)
{
    DisplayChannel *display = DCC_TO_DC(dcc);
    SpiceRect area;
    RedSurface *surface;

    surface = &display->surfaces[surface_id];
    if (!surface->context.canvas) {
        return;
    }
    area.top = area.left = 0;
    area.right = surface->context.width;
    area.bottom = surface->context.height;

    /* not allowing lossy compression because probably, especially if it is a primary surface,
       it combines both "picture-like" areas with areas that are more "artificial"*/
    dcc_add_surface_area_image(dcc, surface_id, &area, NULL, FALSE);
    red_channel_client_push(RED_CHANNEL_CLIENT(dcc));
}

static void add_drawable_surface_images(DisplayChannelClient *dcc, Drawable *drawable)
{
    DisplayChannel *display = DCC_TO_DC(dcc);
    int x;

    for (x = 0; x < 3; ++x) {
        int surface_id;

        surface_id = drawable->surface_deps[x];
        if (surface_id != -1) {
            if (dcc->surface_client_created[surface_id] == TRUE) {
                continue;
            }
            dcc_create_surface(dcc, surface_id);
            display_channel_current_flush(display, surface_id);
            dcc_push_surface_image(dcc, surface_id);
        }
    }

    if (dcc->surface_client_created[drawable->surface_id] == TRUE) {
        return;
    }

    dcc_create_surface(dcc, drawable->surface_id);
    display_channel_current_flush(display, drawable->surface_id);
    dcc_push_surface_image(dcc, drawable->surface_id);
}

DrawablePipeItem *drawable_pipe_item_ref(DrawablePipeItem *dpi)
{
    dpi->refs++;
    return dpi;
}

void drawable_pipe_item_unref(DrawablePipeItem *dpi)
{
    DisplayChannel *display = DCC_TO_DC(dpi->dcc);

    if (--dpi->refs)
        return;

    spice_return_if_fail(!ring_item_is_linked(&dpi->dpi_pipe_item.link));
    spice_return_if_fail(!ring_item_is_linked(&dpi->base));
    display_channel_drawable_unref(display, dpi->drawable);
    free(dpi);
}

static DrawablePipeItem *drawable_pipe_item_new(DisplayChannelClient *dcc, Drawable *drawable)
{
    DrawablePipeItem *dpi;

    dpi = spice_malloc0(sizeof(*dpi));
    dpi->drawable = drawable;
    dpi->dcc = dcc;
    ring_item_init(&dpi->base);
    ring_add(&drawable->pipes, &dpi->base);
    red_channel_pipe_item_init(RED_CHANNEL_CLIENT(dcc)->channel,
                               &dpi->dpi_pipe_item, PIPE_ITEM_TYPE_DRAW);
    dpi->refs++;
    drawable->refs++;
    return dpi;
}

void dcc_add_drawable(DisplayChannelClient *dcc, Drawable *drawable, bool to_tail)
{
    DrawablePipeItem *dpi = drawable_pipe_item_new(dcc, drawable);

    add_drawable_surface_images(dcc, drawable);
    if (to_tail)
        red_channel_client_pipe_add_tail(RED_CHANNEL_CLIENT(dcc), &dpi->dpi_pipe_item);
    else
        red_channel_client_pipe_add(RED_CHANNEL_CLIENT(dcc), &dpi->dpi_pipe_item);
}

void dcc_add_drawable_after(DisplayChannelClient *dcc, Drawable *drawable, PipeItem *pos)
{
    DrawablePipeItem *dpi = drawable_pipe_item_new(dcc, drawable);

    add_drawable_surface_images(dcc, drawable);
    red_channel_client_pipe_add_after(RED_CHANNEL_CLIENT(dcc), &dpi->dpi_pipe_item, pos);
}

static void dcc_init_stream_agents(DisplayChannelClient *dcc)
{
    int i;
    DisplayChannel *display = DCC_TO_DC(dcc);
    RedChannel *channel = RED_CHANNEL_CLIENT(dcc)->channel;

    for (i = 0; i < NUM_STREAMS; i++) {
        StreamAgent *agent = &dcc->stream_agents[i];
        agent->stream = &display->streams_buf[i];
        region_init(&agent->vis_region);
        region_init(&agent->clip);
        red_channel_pipe_item_init(channel, &agent->create_item, PIPE_ITEM_TYPE_STREAM_CREATE);
        red_channel_pipe_item_init(channel, &agent->destroy_item, PIPE_ITEM_TYPE_STREAM_DESTROY);
    }
    dcc->use_mjpeg_encoder_rate_control =
        red_channel_client_test_remote_cap(RED_CHANNEL_CLIENT(dcc), SPICE_DISPLAY_CAP_STREAM_REPORT);
}

#define DISPLAY_FREE_LIST_DEFAULT_SIZE 128

DisplayChannelClient *dcc_new(DisplayChannel *display,
                              RedClient *client, RedsStream *stream,
                              int mig_target,
                              uint32_t *common_caps, int num_common_caps,
                              uint32_t *caps, int num_caps,
                              spice_image_compression_t image_compression,
                              spice_wan_compression_t jpeg_state,
                              spice_wan_compression_t zlib_glz_state)

{
    DisplayChannelClient *dcc;

    dcc = (DisplayChannelClient*)common_channel_new_client(
        COMMON_CHANNEL(display), sizeof(DisplayChannelClient),
        client, stream, mig_target, TRUE,
        common_caps, num_common_caps,
        caps, num_caps);
    spice_return_val_if_fail(dcc, NULL);
    spice_info("New display (client %p) dcc %p stream %p", client, dcc, stream);

    ring_init(&dcc->palette_cache_lru);
    dcc->palette_cache_available = CLIENT_PALETTE_CACHE_SIZE;
    dcc->image_compression = image_compression;
    dcc->jpeg_state = jpeg_state;
    dcc->zlib_glz_state = zlib_glz_state;
    // todo: tune quality according to bandwidth
    dcc->jpeg_quality = 85;

    size_t stream_buf_size;
    stream_buf_size = 32*1024;
    dcc->send_data.stream_outbuf = spice_malloc(stream_buf_size);
    dcc->send_data.stream_outbuf_size = stream_buf_size;
    dcc->send_data.free_list.res =
        spice_malloc(sizeof(SpiceResourceList) +
                     DISPLAY_FREE_LIST_DEFAULT_SIZE * sizeof(SpiceResourceID));
    dcc->send_data.free_list.res_size = DISPLAY_FREE_LIST_DEFAULT_SIZE;

    dcc_init_stream_agents(dcc);

    dcc_encoders_init(dcc);

    return dcc;
}

static void dcc_create_all_streams(DisplayChannelClient *dcc)
{
    Ring *ring = &DCC_TO_DC(dcc)->streams;
    RingItem *item = ring;

    while ((item = ring_next(ring, item))) {
        Stream *stream = SPICE_CONTAINEROF(item, Stream, link);
        dcc_create_stream(dcc, stream);
    }
}

/* TODO: this function is evil^Wsynchronous, fix */
static int display_channel_client_wait_for_init(DisplayChannelClient *dcc)
{
    dcc->expect_init = TRUE;
    uint64_t end_time = red_get_monotonic_time() + DISPLAY_CLIENT_TIMEOUT;
    for (;;) {
        red_channel_client_receive(RED_CHANNEL_CLIENT(dcc));
        if (!red_channel_client_is_connected(RED_CHANNEL_CLIENT(dcc))) {
            break;
        }
        if (dcc->pixmap_cache && dcc->glz_dict) {
            dcc->pixmap_cache_generation = dcc->pixmap_cache->generation;
            /* TODO: move common.id? if it's used for a per client structure.. */
            spice_info("creating encoder with id == %d", dcc->common.id);
            dcc->glz = glz_encoder_create(dcc->common.id, dcc->glz_dict->dict, &dcc->glz_data.usr);
            if (!dcc->glz) {
                spice_critical("create global lz failed");
            }
            return TRUE;
        }
        if (red_get_monotonic_time() > end_time) {
            spice_warning("timeout");
            red_channel_client_disconnect(RED_CHANNEL_CLIENT(dcc));
            break;
        }
        usleep(DISPLAY_CLIENT_RETRY_INTERVAL);
    }
    return FALSE;
}

void dcc_start(DisplayChannelClient *dcc)
{
    DisplayChannel *display = DCC_TO_DC(dcc);
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(dcc);

    red_channel_client_push_set_ack(RED_CHANNEL_CLIENT(dcc));

    if (red_channel_client_waits_for_migrate_data(rcc))
        return;

    if (!display_channel_client_wait_for_init(dcc))
        return;

    red_channel_client_ack_zero_messages_window(RED_CHANNEL_CLIENT(dcc));
    if (display->surfaces[0].context.canvas) {
        display_channel_current_flush(display, 0);
        red_channel_client_pipe_add_type(rcc, PIPE_ITEM_TYPE_INVAL_PALLET_CACHE);
        dcc_create_surface(dcc, 0);
        dcc_push_surface_image(dcc, 0);
        dcc_push_monitors_config(dcc);
        red_pipe_add_verb(rcc, SPICE_MSG_DISPLAY_MARK);
        dcc_create_all_streams(dcc);
    }
}


void dcc_stream_agent_clip(DisplayChannelClient* dcc, StreamAgent *agent)
{
    StreamClipItem *item = stream_clip_item_new(dcc, agent);
    int n_rects;

    item->clip_type = SPICE_CLIP_TYPE_RECTS;

    n_rects = pixman_region32_n_rects(&agent->clip);
    item->rects = spice_malloc_n_m(n_rects, sizeof(SpiceRect), sizeof(SpiceClipRects));
    item->rects->num_rects = n_rects;
    region_ret_rects(&agent->clip, item->rects->rects, n_rects);

    red_channel_client_pipe_add(RED_CHANNEL_CLIENT(dcc), (PipeItem *)item);
}

static MonitorsConfigItem *monitors_config_item_new(RedChannel* channel,
                                                    MonitorsConfig *monitors_config)
{
    MonitorsConfigItem *mci;

    mci = (MonitorsConfigItem *)spice_malloc(sizeof(*mci));
    mci->monitors_config = monitors_config;

    red_channel_pipe_item_init(channel,
                               &mci->pipe_item, PIPE_ITEM_TYPE_MONITORS_CONFIG);
    return mci;
}

void dcc_push_monitors_config(DisplayChannelClient *dcc)
{
    DisplayChannel *dc = DCC_TO_DC(dcc);
    MonitorsConfig *monitors_config = dc->monitors_config;
    MonitorsConfigItem *mci;

    if (monitors_config == NULL) {
        spice_warning("monitors_config is NULL");
        return;
    }

    if (!red_channel_client_test_remote_cap(&dcc->common.base,
                                            SPICE_DISPLAY_CAP_MONITORS_CONFIG)) {
        return;
    }

    mci = monitors_config_item_new(dcc->common.base.channel,
                                   monitors_config_ref(dc->monitors_config));
    red_channel_client_pipe_add(&dcc->common.base, &mci->pipe_item);
    red_channel_client_push(&dcc->common.base);
}

static SurfaceDestroyItem *surface_destroy_item_new(RedChannel *channel,
                                                    uint32_t surface_id)
{
    SurfaceDestroyItem *destroy;

    destroy = (SurfaceDestroyItem *)malloc(sizeof(SurfaceDestroyItem));
    destroy->surface_destroy.surface_id = surface_id;
    red_channel_pipe_item_init(channel, &destroy->pipe_item,
                               PIPE_ITEM_TYPE_DESTROY_SURFACE);

    return destroy;
}

void dcc_destroy_surface(DisplayChannelClient *dcc, uint32_t surface_id)
{
    DisplayChannel *display = DCC_TO_DC(dcc);
    RedChannel *channel = RED_CHANNEL(display);
    SurfaceDestroyItem *destroy;

    if (!dcc || COMMON_CHANNEL(display)->during_target_migrate ||
        !dcc->surface_client_created[surface_id]) {
        return;
    }

    dcc->surface_client_created[surface_id] = FALSE;
    destroy = surface_destroy_item_new(channel, surface_id);
    red_channel_client_pipe_add(RED_CHANNEL_CLIENT(dcc), &destroy->pipe_item);
}

/* if already exists, returns it. Otherwise allocates and adds it (1) to the ring tail
   in the channel (2) to the Drawable*/
static RedGlzDrawable *get_glz_drawable(DisplayChannelClient *dcc, Drawable *drawable)
{
    RedGlzDrawable *ret;
    RingItem *item, *next;

    // TODO - I don't really understand what's going on here, so doing the technical equivalent
    // now that we have multiple glz_dicts, so the only way to go from dcc to drawable glz is to go
    // over the glz_ring (unless adding some better data structure then a ring)
    DRAWABLE_FOREACH_GLZ_SAFE(drawable, item, next, ret) {
        if (ret->dcc == dcc) {
            return ret;
        }
    }

    ret = spice_new(RedGlzDrawable, 1);

    ret->dcc = dcc;
    ret->red_drawable = red_drawable_ref(drawable->red_drawable);
    ret->drawable = drawable;
    ret->group_id = drawable->group_id;
    ret->instances_count = 0;
    ring_init(&ret->instances);

    ring_item_init(&ret->link);
    ring_item_init(&ret->drawable_link);
    ring_add_before(&ret->link, &dcc->glz_drawables);
    ring_add(&drawable->glz_ring, &ret->drawable_link);
    dcc->glz_drawable_count++;
    return ret;
}

/* allocates new instance and adds it to instances in the given drawable.
   NOTE - the caller should set the glz_instance returned by the encoder by itself.*/
static GlzDrawableInstanceItem *add_glz_drawable_instance(RedGlzDrawable *glz_drawable)
{
    spice_return_val_if_fail(glz_drawable->instances_count < MAX_GLZ_DRAWABLE_INSTANCES, NULL);

    // NOTE: We assume the additions are performed consecutively, without removals in the middle
    GlzDrawableInstanceItem *ret = glz_drawable->instances_pool + glz_drawable->instances_count;
    glz_drawable->instances_count++;

    ring_item_init(&ret->free_link);
    ring_item_init(&ret->glz_link);
    ring_add(&glz_drawable->instances, &ret->glz_link);
    ret->context = NULL;
    ret->glz_drawable = glz_drawable;

    return ret;
}

#define MIN_GLZ_SIZE_FOR_ZLIB 100

int dcc_compress_image_glz(DisplayChannelClient *dcc,
                           SpiceImage *dest, SpiceBitmap *src, Drawable *drawable,
                           compress_send_data_t* o_comp_data)
{
    spice_return_val_if_fail(bitmap_fmt_is_rgb(src->format), FALSE);

    DisplayChannel *display_channel = DCC_TO_DC(dcc);
#ifdef COMPRESS_STAT
    stat_time_t start_time = stat_now(worker);
#endif
    GlzData *glz_data = &dcc->glz_data;
    ZlibData *zlib_data;
    LzImageType type = MAP_BITMAP_FMT_TO_LZ_IMAGE_TYPE[src->format];
    RedGlzDrawable *glz_drawable;
    GlzDrawableInstanceItem *glz_drawable_instance;
    int glz_size;
    int zlib_size;

    glz_data->data.bufs_tail = compress_buf_new();
    glz_data->data.bufs_head = glz_data->data.bufs_tail;
    glz_data->data.dcc = dcc;

    glz_drawable = get_glz_drawable(dcc, drawable);
    glz_drawable_instance = add_glz_drawable_instance(glz_drawable);

    glz_data->data.u.lines_data.chunks = src->data;
    glz_data->data.u.lines_data.stride = src->stride;
    glz_data->data.u.lines_data.next = 0;
    glz_data->data.u.lines_data.reverse = 0;
    /* fixme: remove? glz_data->usr.more_lines = glz_usr_more_lines; */

    glz_size = glz_encode(dcc->glz, type, src->x, src->y,
                          (src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN), NULL, 0,
                          src->stride, glz_data->data.bufs_head->buf,
                          sizeof(glz_data->data.bufs_head->buf),
                          glz_drawable_instance,
                          &glz_drawable_instance->context);

    stat_compress_add(&display_channel->glz_stat, start_time, src->stride * src->y, glz_size);

    if (!display_channel->enable_zlib_glz_wrap || (glz_size < MIN_GLZ_SIZE_FOR_ZLIB)) {
        goto glz;
    }
#ifdef COMPRESS_STAT
    start_time = stat_now(worker);
#endif
    zlib_data = &dcc->zlib_data;

    zlib_data->data.bufs_tail = compress_buf_new();
    zlib_data->data.bufs_head = zlib_data->data.bufs_tail;
    zlib_data->data.dcc = dcc;

    zlib_data->data.u.compressed_data.next = glz_data->data.bufs_head;
    zlib_data->data.u.compressed_data.size_left = glz_size;

    zlib_size = zlib_encode(dcc->zlib, dcc->zlib_level,
                            glz_size, (uint8_t*)zlib_data->data.bufs_head->buf,
                            sizeof(zlib_data->data.bufs_head->buf));

    // the compressed buffer is bigger than the original data
    if (zlib_size >= glz_size) {
        while (zlib_data->data.bufs_head) {
            RedCompressBuf *buf = zlib_data->data.bufs_head;
            zlib_data->data.bufs_head = buf->send_next;
            compress_buf_free(buf);
        }
        goto glz;
    }

    dest->descriptor.type = SPICE_IMAGE_TYPE_ZLIB_GLZ_RGB;
    dest->u.zlib_glz.glz_data_size = glz_size;
    dest->u.zlib_glz.data_size = zlib_size;

    o_comp_data->comp_buf = zlib_data->data.bufs_head;
    o_comp_data->comp_buf_size = zlib_size;

    stat_compress_add(&display_channel->zlib_glz_stat, start_time, glz_size, zlib_size);
    return TRUE;
glz:
    dest->descriptor.type = SPICE_IMAGE_TYPE_GLZ_RGB;
    dest->u.lz_rgb.data_size = glz_size;

    o_comp_data->comp_buf = glz_data->data.bufs_head;
    o_comp_data->comp_buf_size = glz_size;

    return TRUE;
}

int dcc_compress_image_lz(DisplayChannelClient *dcc,
                          SpiceImage *dest, SpiceBitmap *src,
                          compress_send_data_t* o_comp_data, uint32_t group_id)
{
    LzData *lz_data = &dcc->lz_data;
    LzContext *lz = dcc->lz;
    LzImageType type = MAP_BITMAP_FMT_TO_LZ_IMAGE_TYPE[src->format];
    int size;            // size of the compressed data

#ifdef COMPRESS_STAT
    stat_time_t start_time = stat_now(worker);
#endif

    lz_data->data.bufs_tail = compress_buf_new();
    lz_data->data.bufs_head = lz_data->data.bufs_tail;
    lz_data->data.dcc = dcc;

    if (setjmp(lz_data->data.jmp_env)) {
        while (lz_data->data.bufs_head) {
            RedCompressBuf *buf = lz_data->data.bufs_head;
            lz_data->data.bufs_head = buf->send_next;
            compress_buf_free(buf);
        }
        return FALSE;
    }

    lz_data->data.u.lines_data.chunks = src->data;
    lz_data->data.u.lines_data.stride = src->stride;
    lz_data->data.u.lines_data.next = 0;
    lz_data->data.u.lines_data.reverse = 0;
    /* fixme: remove? lz_data->usr.more_lines = lz_usr_more_lines; */

    size = lz_encode(lz, type, src->x, src->y,
                     !!(src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN),
                     NULL, 0, src->stride,
                     lz_data->data.bufs_head->buf,
                     sizeof(lz_data->data.bufs_head->buf));

    // the compressed buffer is bigger than the original data
    if (size > (src->y * src->stride)) {
        longjmp(lz_data->data.jmp_env, 1);
    }

    if (bitmap_fmt_is_rgb(src->format)) {
        dest->descriptor.type = SPICE_IMAGE_TYPE_LZ_RGB;
        dest->u.lz_rgb.data_size = size;

        o_comp_data->comp_buf = lz_data->data.bufs_head;
        o_comp_data->comp_buf_size = size;
    } else {
        /* masks are 1BIT bitmaps without palettes, but they are not compressed
         * (see fill_mask) */
        spice_return_val_if_fail(src->palette, FALSE);
        dest->descriptor.type = SPICE_IMAGE_TYPE_LZ_PLT;
        dest->u.lz_plt.data_size = size;
        dest->u.lz_plt.flags = src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN;
        dest->u.lz_plt.palette = src->palette;
        dest->u.lz_plt.palette_id = src->palette->unique;
        o_comp_data->comp_buf = lz_data->data.bufs_head;
        o_comp_data->comp_buf_size = size;

        dcc_palette_cache_palette(dcc, dest->u.lz_plt.palette, &(dest->u.lz_plt.flags));
        o_comp_data->lzplt_palette = dest->u.lz_plt.palette;
    }

    stat_compress_add(&display_channel->lz_stat, start_time, src->stride * src->y,
                      o_comp_data->comp_buf_size);
    return TRUE;
}

int dcc_compress_image_jpeg(DisplayChannelClient *dcc, SpiceImage *dest,
                            SpiceBitmap *src, compress_send_data_t* o_comp_data,
                            uint32_t group_id)
{
    JpegData *jpeg_data = &dcc->jpeg_data;
    LzData *lz_data = &dcc->lz_data;
    JpegEncoderContext *jpeg = dcc->jpeg;
    LzContext *lz = dcc->lz;
    volatile JpegEncoderImageType jpeg_in_type;
    int jpeg_size = 0;
    volatile int has_alpha = FALSE;
    int alpha_lz_size = 0;
    int comp_head_filled;
    int comp_head_left;
    int stride;
    uint8_t *lz_out_start_byte;

#ifdef COMPRESS_STAT
    stat_time_t start_time = stat_now(worker);
#endif
    switch (src->format) {
    case SPICE_BITMAP_FMT_16BIT:
        jpeg_in_type = JPEG_IMAGE_TYPE_RGB16;
        break;
    case SPICE_BITMAP_FMT_24BIT:
        jpeg_in_type = JPEG_IMAGE_TYPE_BGR24;
        break;
    case SPICE_BITMAP_FMT_32BIT:
        jpeg_in_type = JPEG_IMAGE_TYPE_BGRX32;
        break;
    case SPICE_BITMAP_FMT_RGBA:
        jpeg_in_type = JPEG_IMAGE_TYPE_BGRX32;
        has_alpha = TRUE;
        break;
    default:
        return FALSE;
    }

    jpeg_data->data.bufs_tail = compress_buf_new();
    jpeg_data->data.bufs_head = jpeg_data->data.bufs_tail;
    jpeg_data->data.dcc = dcc;

    if (setjmp(jpeg_data->data.jmp_env)) {
        while (jpeg_data->data.bufs_head) {
            RedCompressBuf *buf = jpeg_data->data.bufs_head;
            jpeg_data->data.bufs_head = buf->send_next;
            compress_buf_free(buf);
        }
        return FALSE;
    }

    if (src->data->flags & SPICE_CHUNKS_FLAGS_UNSTABLE) {
        spice_chunks_linearize(src->data);
    }

    jpeg_data->data.u.lines_data.chunks = src->data;
    jpeg_data->data.u.lines_data.stride = src->stride;
    /* fixme: remove? jpeg_data->usr.more_lines = jpeg_usr_more_lines; */
    if ((src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN)) {
        jpeg_data->data.u.lines_data.next = 0;
        jpeg_data->data.u.lines_data.reverse = 0;
        stride = src->stride;
    } else {
        jpeg_data->data.u.lines_data.next = src->data->num_chunks - 1;
        jpeg_data->data.u.lines_data.reverse = 1;
        stride = -src->stride;
    }
    jpeg_size = jpeg_encode(jpeg, dcc->jpeg_quality, jpeg_in_type,
                            src->x, src->y, NULL,
                            0, stride, jpeg_data->data.bufs_head->buf,
                            sizeof(jpeg_data->data.bufs_head->buf));

    // the compressed buffer is bigger than the original data
    if (jpeg_size > (src->y * src->stride)) {
        longjmp(jpeg_data->data.jmp_env, 1);
    }

    if (!has_alpha) {
        dest->descriptor.type = SPICE_IMAGE_TYPE_JPEG;
        dest->u.jpeg.data_size = jpeg_size;

        o_comp_data->comp_buf = jpeg_data->data.bufs_head;
        o_comp_data->comp_buf_size = jpeg_size;
        o_comp_data->is_lossy = TRUE;

        stat_compress_add(&display_channel->jpeg_stat, start_time, src->stride * src->y,
                          o_comp_data->comp_buf_size);
        return TRUE;
    }

    lz_data->data.bufs_head = jpeg_data->data.bufs_tail;
    lz_data->data.bufs_tail = lz_data->data.bufs_head;

    comp_head_filled = jpeg_size % sizeof(lz_data->data.bufs_head->buf);
    comp_head_left = sizeof(lz_data->data.bufs_head->buf) - comp_head_filled;
    lz_out_start_byte = lz_data->data.bufs_head->buf + comp_head_filled;

    lz_data->data.dcc = dcc;

    lz_data->data.u.lines_data.chunks = src->data;
    lz_data->data.u.lines_data.stride = src->stride;
    lz_data->data.u.lines_data.next = 0;
    lz_data->data.u.lines_data.reverse = 0;
    /* fixme: remove? lz_data->usr.more_lines = lz_usr_more_lines; */

    alpha_lz_size = lz_encode(lz, LZ_IMAGE_TYPE_XXXA, src->x, src->y,
                               !!(src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN),
                               NULL, 0, src->stride,
                               lz_out_start_byte,
                               comp_head_left);

    // the compressed buffer is bigger than the original data
    if ((jpeg_size + alpha_lz_size) > (src->y * src->stride)) {
        longjmp(jpeg_data->data.jmp_env, 1);
    }

    dest->descriptor.type = SPICE_IMAGE_TYPE_JPEG_ALPHA;
    dest->u.jpeg_alpha.flags = 0;
    if (src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN) {
        dest->u.jpeg_alpha.flags |= SPICE_JPEG_ALPHA_FLAGS_TOP_DOWN;
    }

    dest->u.jpeg_alpha.jpeg_size = jpeg_size;
    dest->u.jpeg_alpha.data_size = jpeg_size + alpha_lz_size;

    o_comp_data->comp_buf = jpeg_data->data.bufs_head;
    o_comp_data->comp_buf_size = jpeg_size + alpha_lz_size;
    o_comp_data->is_lossy = TRUE;
    stat_compress_add(&display_channel->jpeg_alpha_stat, start_time, src->stride * src->y,
                      o_comp_data->comp_buf_size);
    return TRUE;
}

int dcc_compress_image_quic(DisplayChannelClient *dcc, SpiceImage *dest,
                            SpiceBitmap *src, compress_send_data_t* o_comp_data,
                            uint32_t group_id)
{
    QuicData *quic_data = &dcc->quic_data;
    QuicContext *quic = dcc->quic;
    volatile QuicImageType type;
    int size, stride;

#ifdef COMPRESS_STAT
    stat_time_t start_time = stat_now(worker);
#endif

    switch (src->format) {
    case SPICE_BITMAP_FMT_32BIT:
        type = QUIC_IMAGE_TYPE_RGB32;
        break;
    case SPICE_BITMAP_FMT_RGBA:
        type = QUIC_IMAGE_TYPE_RGBA;
        break;
    case SPICE_BITMAP_FMT_16BIT:
        type = QUIC_IMAGE_TYPE_RGB16;
        break;
    case SPICE_BITMAP_FMT_24BIT:
        type = QUIC_IMAGE_TYPE_RGB24;
        break;
    default:
        return FALSE;
    }

    quic_data->data.bufs_tail = compress_buf_new();
    quic_data->data.bufs_head = quic_data->data.bufs_tail;
    quic_data->data.dcc = dcc;

    if (setjmp(quic_data->data.jmp_env)) {
        while (quic_data->data.bufs_head) {
            RedCompressBuf *buf = quic_data->data.bufs_head;
            quic_data->data.bufs_head = buf->send_next;
            compress_buf_free(buf);
        }
        return FALSE;
    }

    if (src->data->flags & SPICE_CHUNKS_FLAGS_UNSTABLE) {
        spice_chunks_linearize(src->data);
    }

    quic_data->data.u.lines_data.chunks = src->data;
    quic_data->data.u.lines_data.stride = src->stride;
    /* fixme: remove? quic_data->usr.more_lines = quic_usr_more_lines; */
    if ((src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN)) {
        quic_data->data.u.lines_data.next = 0;
        quic_data->data.u.lines_data.reverse = 0;
        stride = src->stride;
    } else {
        quic_data->data.u.lines_data.next = src->data->num_chunks - 1;
        quic_data->data.u.lines_data.reverse = 1;
        stride = -src->stride;
    }
    size = quic_encode(quic, type, src->x, src->y, NULL, 0, stride,
                       (uint32_t*)quic_data->data.bufs_head->buf,
                       sizeof(quic_data->data.bufs_head->buf) / sizeof(uint32_t));

    // the compressed buffer is bigger than the original data
    if ((size << 2) > (src->y * src->stride)) {
        longjmp(quic_data->data.jmp_env, 1);
    }

    dest->descriptor.type = SPICE_IMAGE_TYPE_QUIC;
    dest->u.quic.data_size = size << 2;

    o_comp_data->comp_buf = quic_data->data.bufs_head;
    o_comp_data->comp_buf_size = size << 2;

    stat_compress_add(&display_channel->quic_stat, start_time, src->stride * src->y,
                      o_comp_data->comp_buf_size);
    return TRUE;
}

#define MIN_SIZE_TO_COMPRESS 54
#define MIN_DIMENSION_TO_QUIC 3
int dcc_compress_image(DisplayChannelClient *dcc,
                       SpiceImage *dest, SpiceBitmap *src, Drawable *drawable,
                       int can_lossy,
                       compress_send_data_t* o_comp_data)
{
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    spice_image_compression_t image_compression = dcc->image_compression;
    int quic_compress = FALSE;

    if ((image_compression == SPICE_IMAGE_COMPRESS_OFF) ||
        ((src->y * src->stride) < MIN_SIZE_TO_COMPRESS)) { // TODO: change the size cond
        return FALSE;
    } else if (image_compression == SPICE_IMAGE_COMPRESS_QUIC) {
        if (bitmap_fmt_is_plt(src->format)) {
            return FALSE;
        } else {
            quic_compress = TRUE;
        }
    } else {
        /*
            lz doesn't handle (1) bitmaps with strides that are larger than the width
            of the image in bytes (2) unstable bitmaps
        */
        if (bitmap_has_extra_stride(src) || (src->data->flags & SPICE_CHUNKS_FLAGS_UNSTABLE)) {
            if ((image_compression == SPICE_IMAGE_COMPRESS_LZ) ||
                (image_compression == SPICE_IMAGE_COMPRESS_GLZ) ||
                bitmap_fmt_is_plt(src->format)) {
                return FALSE;
            } else {
                quic_compress = TRUE;
            }
        } else {
            if ((image_compression == SPICE_IMAGE_COMPRESS_AUTO_LZ) ||
                (image_compression == SPICE_IMAGE_COMPRESS_AUTO_GLZ)) {
                if ((src->x < MIN_DIMENSION_TO_QUIC) || (src->y < MIN_DIMENSION_TO_QUIC)) {
                    quic_compress = FALSE;
                } else {
                    if (drawable->copy_bitmap_graduality == BITMAP_GRADUAL_INVALID) {
                        quic_compress = bitmap_fmt_has_graduality(src->format) &&
                            bitmap_get_graduality_level(src) == BITMAP_GRADUAL_HIGH;
                    } else {
                        quic_compress = (drawable->copy_bitmap_graduality == BITMAP_GRADUAL_HIGH);
                    }
                }
            } else {
                quic_compress = FALSE;
            }
        }
    }

    if (quic_compress) {
#ifdef COMPRESS_DEBUG
        spice_info("QUIC compress");
#endif
        // if bitmaps is picture-like, compress it using jpeg
        if (can_lossy && display_channel->enable_jpeg &&
            ((image_compression == SPICE_IMAGE_COMPRESS_AUTO_LZ) ||
            (image_compression == SPICE_IMAGE_COMPRESS_AUTO_GLZ))) {
            // if we use lz for alpha, the stride can't be extra
            if (src->format != SPICE_BITMAP_FMT_RGBA || !bitmap_has_extra_stride(src)) {
                return dcc_compress_image_jpeg(dcc, dest,
                                               src, o_comp_data, drawable->group_id);
            }
        }
        return dcc_compress_image_quic(dcc, dest,
                                       src, o_comp_data, drawable->group_id);
    } else {
        int glz;
        int ret;
        if ((image_compression == SPICE_IMAGE_COMPRESS_AUTO_GLZ) ||
            (image_compression == SPICE_IMAGE_COMPRESS_GLZ)) {
            glz = bitmap_fmt_has_graduality(src->format) && (
                    (src->x * src->y) < glz_enc_dictionary_get_size(
                        dcc->glz_dict->dict));
        } else if ((image_compression == SPICE_IMAGE_COMPRESS_AUTO_LZ) ||
                   (image_compression == SPICE_IMAGE_COMPRESS_LZ)) {
            glz = FALSE;
        } else {
            spice_error("invalid image compression type %u", image_compression);
            return FALSE;
        }

        if (glz) {
            /* using the global dictionary only if it is not frozen */
            pthread_rwlock_rdlock(&dcc->glz_dict->encode_lock);
            if (!dcc->glz_dict->migrate_freeze) {
                ret = dcc_compress_image_glz(dcc,
                                             dest, src,
                                             drawable, o_comp_data);
            } else {
                glz = FALSE;
            }
            pthread_rwlock_unlock(&dcc->glz_dict->encode_lock);
        }

        if (!glz) {
            ret = dcc_compress_image_lz(dcc, dest, src, o_comp_data,
                                        drawable->group_id);
#ifdef COMPRESS_DEBUG
            spice_info("LZ LOCAL compress");
#endif
        }
#ifdef COMPRESS_DEBUG
        else {
            spice_info("LZ global compress fmt=%d", src->format);
        }
#endif
        return ret;
    }
}

#define CLIENT_PALETTE_CACHE
#include "cache_item.tmpl.c"
#undef CLIENT_PALETTE_CACHE

void dcc_palette_cache_palette(DisplayChannelClient *dcc, SpicePalette *palette,
                               uint8_t *flags)
{
    if (palette == NULL) {
        return;
    }
    if (palette->unique) {
        if (red_palette_cache_find(dcc, palette->unique)) {
            *flags |= SPICE_BITMAP_FLAGS_PAL_FROM_CACHE;
            return;
        }
        if (red_palette_cache_add(dcc, palette->unique, 1)) {
            *flags |= SPICE_BITMAP_FLAGS_PAL_CACHE_ME;
        }
    }
}

void dcc_palette_cache_reset(DisplayChannelClient *dcc)
{
    red_palette_cache_reset(dcc, CLIENT_PALETTE_CACHE_SIZE);
}

static void dcc_push_release(DisplayChannelClient *dcc, uint8_t type, uint64_t id,
                             uint64_t* sync_data)
{
    FreeList *free_list = &dcc->send_data.free_list;
    int i;

    for (i = 0; i < MAX_CACHE_CLIENTS; i++) {
        free_list->sync[i] = MAX(free_list->sync[i], sync_data[i]);
    }

    if (free_list->res->count == free_list->res_size) {
        SpiceResourceList *new_list;
        new_list = spice_malloc(sizeof(*new_list) +
                                free_list->res_size * sizeof(SpiceResourceID) * 2);
        new_list->count = free_list->res->count;
        memcpy(new_list->resources, free_list->res->resources,
               new_list->count * sizeof(SpiceResourceID));
        free(free_list->res);
        free_list->res = new_list;
        free_list->res_size *= 2;
    }
    free_list->res->resources[free_list->res->count].type = type;
    free_list->res->resources[free_list->res->count++].id = id;
}

int dcc_pixmap_cache_add(DisplayChannelClient *dcc, uint64_t id, uint32_t size, int lossy)
{
    PixmapCache *cache = dcc->pixmap_cache;
    NewCacheItem *item;
    uint64_t serial;
    int key;

    spice_return_val_if_fail(size > 0, FALSE);

    item = spice_new(NewCacheItem, 1);
    serial = red_channel_client_get_message_serial(RED_CHANNEL_CLIENT(dcc));

    pthread_mutex_lock(&cache->lock);

    if (cache->generation != dcc->pixmap_cache_generation) {
        if (!dcc->pending_pixmaps_sync) {
            red_channel_client_pipe_add_type(
                                             RED_CHANNEL_CLIENT(dcc), PIPE_ITEM_TYPE_PIXMAP_SYNC);
            dcc->pending_pixmaps_sync = TRUE;
        }
        pthread_mutex_unlock(&cache->lock);
        free(item);
        return FALSE;
    }

    cache->available -= size;
    while (cache->available < 0) {
        NewCacheItem *tail;
        NewCacheItem **now;

        if (!(tail = (NewCacheItem *)ring_get_tail(&cache->lru)) ||
                                                   tail->sync[dcc->common.id] == serial) {
            cache->available += size;
            pthread_mutex_unlock(&cache->lock);
            free(item);
            return FALSE;
        }

        now = &cache->hash_table[BITS_CACHE_HASH_KEY(tail->id)];
        for (;;) {
            spice_return_val_if_fail(*now, FALSE);
            if (*now == tail) {
                *now = tail->next;
                break;
            }
            now = &(*now)->next;
        }
        ring_remove(&tail->lru_link);
        cache->items--;
        cache->available += tail->size;
        cache->sync[dcc->common.id] = serial;
        dcc_push_release(dcc, SPICE_RES_TYPE_PIXMAP, tail->id, tail->sync);
        free(tail);
    }
    ++cache->items;
    item->next = cache->hash_table[(key = BITS_CACHE_HASH_KEY(id))];
    cache->hash_table[key] = item;
    ring_item_init(&item->lru_link);
    ring_add(&cache->lru, &item->lru_link);
    item->id = id;
    item->size = size;
    item->lossy = lossy;
    memset(item->sync, 0, sizeof(item->sync));
    item->sync[dcc->common.id] = serial;
    cache->sync[dcc->common.id] = serial;
    pthread_mutex_unlock(&cache->lock);
    return TRUE;
}
