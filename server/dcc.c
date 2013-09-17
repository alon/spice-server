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
