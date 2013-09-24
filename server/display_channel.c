#include "display_channel.h"

static stat_time_t display_channel_stat_now(DisplayChannel *display)
{
#ifdef RED_WORKER_STAT
    RedWorker *worker = COMMON_CHANNEL(display)->worker;

    return stat_now(red_worker_get_clockid(worker));

#else
    return 0;
#endif
}

#define stat_start(display, var)                                        \
    G_GNUC_UNUSED stat_time_t var = display_channel_stat_now((display));

void display_channel_compress_stats_reset(DisplayChannel *display)
{
    spice_return_if_fail(display);

#ifdef COMPRESS_STAT
    stat_reset(&display->quic_stat);
    stat_reset(&display->lz_stat);
    stat_reset(&display->glz_stat);
    stat_reset(&display->jpeg_stat);
    stat_reset(&display->zlib_glz_stat);
    stat_reset(&display->jpeg_alpha_stat);
#endif
}

void display_channel_compress_stats_print(const DisplayChannel *display_channel)
{
    spice_return_if_fail(display_channel);

#ifdef COMPRESS_STAT
    uint64_t glz_enc_size;

    glz_enc_size = display_channel->enable_zlib_glz_wrap ?
                       display_channel->zlib_glz_stat.comp_size :
                       display_channel->glz_stat.comp_size;

    spice_info("==> Compression stats for display %u", display_channel->common.id);
    spice_info("Method   \t  count  \torig_size(MB)\tenc_size(MB)\tenc_time(s)");
    spice_info("QUIC     \t%8d\t%13.2f\t%12.2f\t%12.2f",
               display_channel->quic_stat.count,
               stat_byte_to_mega(display_channel->quic_stat.orig_size),
               stat_byte_to_mega(display_channel->quic_stat.comp_size),
               stat_cpu_time_to_sec(display_channel->quic_stat.total)
               );
    spice_info("GLZ      \t%8d\t%13.2f\t%12.2f\t%12.2f",
               display_channel->glz_stat.count,
               stat_byte_to_mega(display_channel->glz_stat.orig_size),
               stat_byte_to_mega(display_channel->glz_stat.comp_size),
               stat_cpu_time_to_sec(display_channel->glz_stat.total)
               );
    spice_info("ZLIB GLZ \t%8d\t%13.2f\t%12.2f\t%12.2f",
               display_channel->zlib_glz_stat.count,
               stat_byte_to_mega(display_channel->zlib_glz_stat.orig_size),
               stat_byte_to_mega(display_channel->zlib_glz_stat.comp_size),
               stat_cpu_time_to_sec(display_channel->zlib_glz_stat.total)
               );
    spice_info("LZ       \t%8d\t%13.2f\t%12.2f\t%12.2f",
               display_channel->lz_stat.count,
               stat_byte_to_mega(display_channel->lz_stat.orig_size),
               stat_byte_to_mega(display_channel->lz_stat.comp_size),
               stat_cpu_time_to_sec(display_channel->lz_stat.total)
               );
    spice_info("JPEG     \t%8d\t%13.2f\t%12.2f\t%12.2f",
               display_channel->jpeg_stat.count,
               stat_byte_to_mega(display_channel->jpeg_stat.orig_size),
               stat_byte_to_mega(display_channel->jpeg_stat.comp_size),
               stat_cpu_time_to_sec(display_channel->jpeg_stat.total)
               );
    spice_info("JPEG-RGBA\t%8d\t%13.2f\t%12.2f\t%12.2f",
               display_channel->jpeg_alpha_stat.count,
               stat_byte_to_mega(display_channel->jpeg_alpha_stat.orig_size),
               stat_byte_to_mega(display_channel->jpeg_alpha_stat.comp_size),
               stat_cpu_time_to_sec(display_channel->jpeg_alpha_stat.total)
               );
    spice_info("-------------------------------------------------------------------");
    spice_info("Total    \t%8d\t%13.2f\t%12.2f\t%12.2f",
               display_channel->lz_stat.count + display_channel->glz_stat.count +
                                                display_channel->quic_stat.count +
                                                display_channel->jpeg_stat.count +
                                                display_channel->jpeg_alpha_stat.count,
               stat_byte_to_mega(display_channel->lz_stat.orig_size +
                                 display_channel->glz_stat.orig_size +
                                 display_channel->quic_stat.orig_size +
                                 display_channel->jpeg_stat.orig_size +
                                 display_channel->jpeg_alpha_stat.orig_size),
               stat_byte_to_mega(display_channel->lz_stat.comp_size +
                                 glz_enc_size +
                                 display_channel->quic_stat.comp_size +
                                 display_channel->jpeg_stat.comp_size +
                                 display_channel->jpeg_alpha_stat.comp_size),
               stat_cpu_time_to_sec(display_channel->lz_stat.total +
                                    display_channel->glz_stat.total +
                                    display_channel->zlib_glz_stat.total +
                                    display_channel->quic_stat.total +
                                    display_channel->jpeg_stat.total +
                                    display_channel->jpeg_alpha_stat.total)
               );
#endif
}

MonitorsConfig* monitors_config_ref(MonitorsConfig *monitors_config)
{
    monitors_config->refs++;

    return monitors_config;
}

void monitors_config_unref(MonitorsConfig *monitors_config)
{
    if (!monitors_config) {
        return;
    }
    if (--monitors_config->refs > 0) {
        return;
    }

    spice_debug("freeing monitors config");
    free(monitors_config);
}

static void monitors_config_debug(MonitorsConfig *mc)
{
    int i;

    spice_debug("monitors config count:%d max:%d", mc->count, mc->max_allowed);
    for (i = 0; i < mc->count; i++)
        spice_debug("+%d+%d %dx%d",
                    mc->heads[i].x, mc->heads[i].y,
                    mc->heads[i].width, mc->heads[i].height);
}

MonitorsConfig* monitors_config_new(QXLHead *heads, ssize_t nheads, ssize_t max)
{
    MonitorsConfig *mc;

    mc = spice_malloc(sizeof(MonitorsConfig) + nheads * sizeof(QXLHead));
    mc->refs = 1;
    mc->count = nheads;
    mc->max_allowed = max;
    memcpy(mc->heads, heads, nheads * sizeof(QXLHead));
    monitors_config_debug(mc);

    return mc;
}

int display_channel_get_streams_timout(DisplayChannel *display)
{
    int timout = INT_MAX;
    Ring *ring = &display->streams;
    RingItem *item = ring;

    red_time_t now = red_get_monotonic_time();
    while ((item = ring_next(ring, item))) {
        Stream *stream;

        stream = SPICE_CONTAINEROF(item, Stream, link);
        red_time_t delta = (stream->last_time + RED_STREAM_TIMOUT) - now;

        if (delta < 1000 * 1000) {
            return 0;
        }
        timout = MIN(timout, (unsigned int)(delta / (1000 * 1000)));
    }
    return timout;
}

void display_channel_set_stream_video(DisplayChannel *display, int stream_video)
{
    spice_return_if_fail(display);
    spice_return_if_fail(stream_video != STREAM_VIDEO_INVALID);

    switch (stream_video) {
    case STREAM_VIDEO_ALL:
        spice_info("sv all");
        break;
    case STREAM_VIDEO_FILTER:
        spice_info("sv filter");
        break;
    case STREAM_VIDEO_OFF:
        spice_info("sv off");
        break;
    default:
        spice_warn_if_reached();
        return;
    }

    display->stream_video = stream_video;
}

static void stop_streams(DisplayChannel *display)
{
    Ring *ring = &display->streams;
    RingItem *item = ring_get_head(ring);

    while (item) {
        Stream *stream = SPICE_CONTAINEROF(item, Stream, link);
        item = ring_next(ring, item);
        if (!stream->current) {
            stream_stop(display, stream);
        } else {
            spice_info("attached stream");
        }
    }

    display->next_item_trace = 0;
    memset(display->items_trace, 0, sizeof(display->items_trace));
}

void display_channel_surface_unref(DisplayChannel *display, uint32_t surface_id)
{
    RedSurface *surface = &display->surfaces[surface_id];
    RedWorker *worker = COMMON_CHANNEL(display)->worker;
    QXLInstance *qxl = red_worker_get_qxl(worker);
    DisplayChannelClient *dcc;
    RingItem *link, *next;

    if (--surface->refs)
        return;

    // only primary surface streams are supported
    if (is_primary_surface(display, surface_id)) {
        stop_streams(display);
    }
    spice_assert(surface->context.canvas);

    surface->context.canvas->ops->destroy(surface->context.canvas);
    if (surface->create.info) {
        qxl->st->qif->release_resource(qxl, surface->create);
    }
    if (surface->destroy.info) {
        qxl->st->qif->release_resource(qxl, surface->destroy);
    }

    region_destroy(&surface->draw_dirty_region);
    surface->context.canvas = NULL;
    FOREACH_DCC(display, link, next, dcc) {
        dcc_destroy_surface(dcc, surface_id);
    }

    spice_warn_if(!ring_is_empty(&surface->depend_on_me));
}

void display_channel_set_surface_release_info(DisplayChannel *display, uint32_t surface_id,
                                              int is_create, QXLReleaseInfo *release_info,
                                              uint32_t group_id)
{
    RedSurface *surface = &display->surfaces[surface_id];

    if (is_create) {
        surface->create.info = release_info;
        surface->create.group_id = group_id;
    } else {
        surface->destroy.info = release_info;
        surface->destroy.group_id = group_id;
    }
}

void display_channel_show_tree(DisplayChannel *display)
{
    int x;

    for (x = 0; x < NUM_SURFACES; ++x) {
        if (!display->surfaces[x].context.canvas)
            continue;

        RingItem *it;
        Ring *ring = &display->surfaces[x].current;
        RING_FOREACH(it, ring) {
            TreeItem *now = SPICE_CONTAINEROF(it, TreeItem, siblings_link);
            tree_item_dump(now);
        }

    }
}

/* TODO: perhaps rename to "ready" or "realized" ? */
bool display_channel_surface_has_canvas(DisplayChannel *display,
                                        uint32_t surface_id)
{
    return display->surfaces[surface_id].context.canvas != NULL;
}

static void streams_update_visible_region(DisplayChannel *display, Drawable *drawable)
{
    Ring *ring;
    RingItem *item;
    RingItem *dcc_ring_item, *next;
    DisplayChannelClient *dcc;

    if (!red_channel_is_connected(RED_CHANNEL(display))) {
        return;
    }

    if (!is_primary_surface(display, drawable->surface_id)) {
        return;
    }

    ring = &display->streams;
    item = ring_get_head(ring);

    while (item) {
        Stream *stream = SPICE_CONTAINEROF(item, Stream, link);
        StreamAgent *agent;

        item = ring_next(ring, item);

        if (stream->current == drawable) {
            continue;
        }

        FOREACH_DCC(display, dcc_ring_item, next, dcc) {
            agent = &dcc->stream_agents[get_stream_id(display, stream)];

            if (region_intersects(&agent->vis_region, &drawable->tree_item.base.rgn)) {
                region_exclude(&agent->vis_region, &drawable->tree_item.base.rgn);
                region_exclude(&agent->clip, &drawable->tree_item.base.rgn);
                dcc_stream_agent_clip(dcc, agent);
            }
        }
    }
}


static void current_add_drawable(DisplayChannel *display,
                                 Drawable *drawable, RingItem *pos)
{
    RedSurface *surface;
    uint32_t surface_id = drawable->surface_id;

    surface = &display->surfaces[surface_id];
    ring_add_after(&drawable->tree_item.base.siblings_link, pos);
    ring_add(&display->current_list, &drawable->list_link);
    ring_add(&surface->current_list, &drawable->surface_list_link);
    display->current_size++;
    drawable->refs++;
}

static int current_add_equal(DisplayChannel *display, DrawItem *item, TreeItem *other)
{
    DrawItem *other_draw_item;
    Drawable *drawable;
    Drawable *other_drawable;

    if (other->type != TREE_ITEM_TYPE_DRAWABLE) {
        return FALSE;
    }
    other_draw_item = (DrawItem *)other;

    if (item->shadow || other_draw_item->shadow || item->effect != other_draw_item->effect) {
        return FALSE;
    }

    drawable = SPICE_CONTAINEROF(item, Drawable, tree_item);
    other_drawable = SPICE_CONTAINEROF(other_draw_item, Drawable, tree_item);

    if (item->effect == QXL_EFFECT_OPAQUE) {
        int add_after = !!other_drawable->stream &&
                        is_drawable_independent_from_surfaces(drawable);
        stream_maintenance(display, drawable, other_drawable);
        current_add_drawable(display, drawable, &other->siblings_link);
        other_drawable->refs++;
        current_remove_drawable(display, other_drawable);
        if (add_after) {
            red_pipes_add_drawable_after(display, drawable, other_drawable);
        } else {
            red_pipes_add_drawable(display, drawable);
        }
        red_pipes_remove_drawable(other_drawable);
        display_channel_drawable_unref(display, other_drawable);
        return TRUE;
    }

    switch (item->effect) {
    case QXL_EFFECT_REVERT_ON_DUP:
        if (is_same_drawable(drawable, other_drawable)) {

            DisplayChannelClient *dcc;
            DrawablePipeItem *dpi;
            RingItem *worker_ring_item, *dpi_ring_item;

            other_drawable->refs++;
            current_remove_drawable(display, other_drawable);

            /* sending the drawable to clients that already received
             * (or will receive) other_drawable */
            worker_ring_item = ring_get_head(&RED_CHANNEL(display)->clients);
            dpi_ring_item = ring_get_head(&other_drawable->pipes);
            /* dpi contains a sublist of dcc's, ordered the same */
            while (worker_ring_item) {
                dcc = SPICE_CONTAINEROF(worker_ring_item, DisplayChannelClient,
                                        common.base.channel_link);
                dpi = SPICE_CONTAINEROF(dpi_ring_item, DrawablePipeItem, base);
                while (worker_ring_item && (!dpi || dcc != dpi->dcc)) {
                    dcc_add_drawable(dcc, drawable);
                    worker_ring_item = ring_next(&RED_CHANNEL(display)->clients,
                                                 worker_ring_item);
                    dcc = SPICE_CONTAINEROF(worker_ring_item, DisplayChannelClient,
                                            common.base.channel_link);
                }

                if (dpi_ring_item) {
                    dpi_ring_item = ring_next(&other_drawable->pipes, dpi_ring_item);
                }
                if (worker_ring_item) {
                    worker_ring_item = ring_next(&RED_CHANNEL(display)->clients,
                                                 worker_ring_item);
                }
            }
            /* not sending other_drawable where possible */
            red_pipes_remove_drawable(other_drawable);

            display_channel_drawable_unref(display, other_drawable);
            return TRUE;
        }
        break;
    case QXL_EFFECT_OPAQUE_BRUSH:
        if (is_same_geometry(drawable, other_drawable)) {
            current_add_drawable(display, drawable, &other->siblings_link);
            red_pipes_remove_drawable(other_drawable);
            current_remove_drawable(display, other_drawable);
            red_pipes_add_drawable(display, drawable);
            return TRUE;
        }
        break;
    case QXL_EFFECT_NOP_ON_DUP:
        if (is_same_drawable(drawable, other_drawable)) {
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void __exclude_region(DisplayChannel *display, Ring *ring, TreeItem *item, QRegion *rgn,
                             Ring **top_ring, Drawable *frame_candidate)
{
    QRegion and_rgn;
    stat_start(display, start_time);

    region_clone(&and_rgn, rgn);
    region_and(&and_rgn, &item->rgn);
    if (!region_is_empty(&and_rgn)) {
        if (IS_DRAW_ITEM(item)) {
            DrawItem *draw = (DrawItem *)item;

            if (draw->effect == QXL_EFFECT_OPAQUE) {
                region_exclude(rgn, &and_rgn);
            }

            if (draw->shadow) {
                Shadow *shadow;
                int32_t x = item->rgn.extents.x1;
                int32_t y = item->rgn.extents.y1;

                region_exclude(&draw->base.rgn, &and_rgn);
                shadow = draw->shadow;
                region_offset(&and_rgn, shadow->base.rgn.extents.x1 - x,
                              shadow->base.rgn.extents.y1 - y);
                region_exclude(&shadow->base.rgn, &and_rgn);
                region_and(&and_rgn, &shadow->on_hold);
                if (!region_is_empty(&and_rgn)) {
                    region_exclude(&shadow->on_hold, &and_rgn);
                    region_or(rgn, &and_rgn);
                    // in flat representation of current, shadow is always his owner next
                    if (!tree_item_contained_by((TreeItem*)shadow, *top_ring)) {
                        *top_ring = tree_item_container_items((TreeItem*)shadow, ring);
                    }
                }
            } else {
                if (frame_candidate) {
                    Drawable *drawable = SPICE_CONTAINEROF(draw, Drawable, tree_item);
                    stream_maintenance(display, frame_candidate, drawable);
                }
                region_exclude(&draw->base.rgn, &and_rgn);
            }
        } else if (item->type == TREE_ITEM_TYPE_CONTAINER) {
            region_exclude(&item->rgn, &and_rgn);

            if (region_is_empty(&item->rgn)) {  //assume container removal will follow
                Shadow *shadow;

                region_exclude(rgn, &and_rgn);
                if ((shadow = tree_item_find_shadow(item))) {
                    region_or(rgn, &shadow->on_hold);
                    if (!tree_item_contained_by((TreeItem*)shadow, *top_ring)) {
                        *top_ring = tree_item_container_items((TreeItem*)shadow, ring);
                    }
                }
            }
        } else {
            Shadow *shadow;

            spice_assert(item->type == TREE_ITEM_TYPE_SHADOW);
            shadow = (Shadow *)item;
            region_exclude(rgn, &and_rgn);
            region_or(&shadow->on_hold, &and_rgn);
        }
    }
    region_destroy(&and_rgn);
    stat_add(&display->__exclude_stat, start_time);
}

static void exclude_region(DisplayChannel *display, Ring *ring, RingItem *ring_item,
                           QRegion *rgn, TreeItem **last, Drawable *frame_candidate)
{
    Ring *top_ring;
    stat_start(display, start_time);

    if (!ring_item) {
        return;
    }

    top_ring = ring;

    for (;;) {
        TreeItem *now = SPICE_CONTAINEROF(ring_item, TreeItem, siblings_link);
        Container *container = now->container;

        spice_assert(!region_is_empty(&now->rgn));

        if (region_intersects(rgn, &now->rgn)) {
            __exclude_region(display, ring, now, rgn, &top_ring, frame_candidate);

            if (region_is_empty(&now->rgn)) {
                spice_assert(now->type != TREE_ITEM_TYPE_SHADOW);
                ring_item = now->siblings_link.prev;
                current_remove(display, now);
                if (last && *last == now) {
                    *last = (TreeItem *)ring_next(ring, ring_item);
                }
            } else if (now->type == TREE_ITEM_TYPE_CONTAINER) {
                Container *container = (Container *)now;
                if ((ring_item = ring_get_head(&container->items))) {
                    ring = &container->items;
                    spice_assert(((TreeItem *)ring_item)->container);
                    continue;
                }
                ring_item = &now->siblings_link;
            }

            if (region_is_empty(rgn)) {
                stat_add(&display->exclude_stat, start_time);
                return;
            }
        }

        while ((last && *last == (TreeItem *)ring_item) ||
               !(ring_item = ring_next(ring, ring_item))) {
            if (ring == top_ring) {
                stat_add(&display->exclude_stat, start_time);
                return;
            }
            ring_item = &container->base.siblings_link;
            container = container->base.container;
            ring = (container) ? &container->items : top_ring;
        }
    }
}

static int current_add_with_shadow(DisplayChannel *display, Ring *ring, Drawable *item)
{
    stat_start(display, start_time);
    ++display->add_with_shadow_count;

    RedDrawable *red_drawable = item->red_drawable;
    SpicePoint delta = {
        .x = red_drawable->u.copy_bits.src_pos.x - red_drawable->bbox.left,
        .y = red_drawable->u.copy_bits.src_pos.y - red_drawable->bbox.top
    };

    Shadow *shadow = shadow_new(&item->tree_item, &delta);
    if (!shadow) {
        stat_add(&display->add_stat, start_time);
        return FALSE;
    }
    // item and his shadow must initially be placed in the same container.
    // for now putting them on root.

    // only primary surface streams are supported
    if (is_primary_surface(display, item->surface_id)) {
        detach_streams_behind(display, &shadow->base.rgn, NULL);
    }

    ring_add(ring, &shadow->base.siblings_link);
    current_add_drawable(display, item, ring);
    if (item->tree_item.effect == QXL_EFFECT_OPAQUE) {
        QRegion exclude_rgn;
        region_clone(&exclude_rgn, &item->tree_item.base.rgn);
        exclude_region(display, ring, &shadow->base.siblings_link, &exclude_rgn, NULL, NULL);
        region_destroy(&exclude_rgn);
        streams_update_visible_region(display, item);
    } else {
        if (is_primary_surface(display, item->surface_id)) {
            detach_streams_behind(display, &item->tree_item.base.rgn, item);
        }
    }
    stat_add(&display->add_stat, start_time);
    return TRUE;
}

static int current_add(DisplayChannel *display, Ring *ring, Drawable *drawable)
{
    DrawItem *item = &drawable->tree_item;
    RingItem *now;
    QRegion exclude_rgn;
    RingItem *exclude_base = NULL;
    stat_start(display, start_time);

    spice_return_val_if_fail(!region_is_empty(&item->base.rgn), FALSE);
    region_init(&exclude_rgn);
    now = ring_next(ring, ring);

    while (now) {
        TreeItem *sibling = SPICE_CONTAINEROF(now, TreeItem, siblings_link);
        int test_res;

        if (!region_bounds_intersects(&item->base.rgn, &sibling->rgn)) {
            now = ring_next(ring, now);
            continue;
        }
        test_res = region_test(&item->base.rgn, &sibling->rgn, REGION_TEST_ALL);
        if (!(test_res & REGION_TEST_SHARED)) {
            now = ring_next(ring, now);
            continue;
        } else if (sibling->type != TREE_ITEM_TYPE_SHADOW) {
            if (!(test_res & REGION_TEST_RIGHT_EXCLUSIVE) &&
                                                   !(test_res & REGION_TEST_LEFT_EXCLUSIVE) &&
                                                   current_add_equal(display, item, sibling)) {
                stat_add(&display->add_stat, start_time);
                return FALSE;
            }

            if (!(test_res & REGION_TEST_RIGHT_EXCLUSIVE) && item->effect == QXL_EFFECT_OPAQUE) {
                Shadow *shadow;
                int skip = now == exclude_base;

                if ((shadow = tree_item_find_shadow(sibling))) {
                    if (exclude_base) {
                        TreeItem *next = sibling;
                        exclude_region(display, ring, exclude_base, &exclude_rgn, &next, NULL);
                        if (next != sibling) {
                            now = next ? &next->siblings_link : NULL;
                            exclude_base = NULL;
                            continue;
                        }
                    }
                    region_or(&exclude_rgn, &shadow->on_hold);
                }
                now = now->prev;
                current_remove(display, sibling);
                now = ring_next(ring, now);
                if (shadow || skip) {
                    exclude_base = now;
                }
                continue;
            }

            if (!(test_res & REGION_TEST_LEFT_EXCLUSIVE) && is_opaque_item(sibling)) {
                Container *container;

                if (exclude_base) {
                    exclude_region(display, ring, exclude_base, &exclude_rgn, NULL, NULL);
                    region_clear(&exclude_rgn);
                    exclude_base = NULL;
                }
                if (sibling->type == TREE_ITEM_TYPE_CONTAINER) {
                    container = (Container *)sibling;
                    ring = &container->items;
                    item->base.container = container;
                    now = ring_next(ring, ring);
                    continue;
                }
                spice_assert(IS_DRAW_ITEM(sibling));
                if (!DRAW_ITEM(sibling)->container_root) {
                    container = container_new(DRAW_ITEM(sibling));
                    if (!container) {
                        spice_warning("create new container failed");
                        region_destroy(&exclude_rgn);
                        return FALSE;
                    }
                    item->base.container = container;
                    ring = &container->items;
                }
            }
        }
        if (!exclude_base) {
            exclude_base = now;
        }
        break;
    }
    if (item->effect == QXL_EFFECT_OPAQUE) {
        region_or(&exclude_rgn, &item->base.rgn);
        exclude_region(display, ring, exclude_base, &exclude_rgn, NULL, drawable);
        stream_trace_update(display, drawable);
        streams_update_visible_region(display, drawable);
        /*
         * Performing the insertion after exclude_region for
         * safety (todo: Not sure if exclude_region can affect the drawable
         * if it is added to the tree before calling exclude_region).
         */
        current_add_drawable(display, drawable, ring);
    } else {
        /*
         * red_detach_streams_behind can affect the current tree since it may
         * trigger calls to update_area. Thus, the drawable should be added to the tree
         * before calling red_detach_streams_behind
         */
        current_add_drawable(display, drawable, ring);
        if (is_primary_surface(display, drawable->surface_id)) {
            detach_streams_behind(display, &drawable->tree_item.base.rgn, drawable);
        }
    }
    region_destroy(&exclude_rgn);
    stat_add(&display->add_stat, start_time);
    return TRUE;
}

static bool drawable_can_stream(DisplayChannel *display, Drawable *drawable)
{
    RedDrawable *red_drawable = drawable->red_drawable;
    SpiceImage *image;

    if (display->stream_video == STREAM_VIDEO_OFF)
        return FALSE;

    if (!is_primary_surface(display, drawable->surface_id))
        return FALSE;

    if (drawable->tree_item.effect != QXL_EFFECT_OPAQUE ||
        red_drawable->type != QXL_DRAW_COPY ||
        red_drawable->u.copy.rop_descriptor != SPICE_ROPD_OP_PUT)
        return FALSE;

    image = red_drawable->u.copy.src_bitmap;
    if (image == NULL ||
        image->descriptor.type != SPICE_IMAGE_TYPE_BITMAP)
        return FALSE;

    if (display->stream_video == STREAM_VIDEO_FILTER) {
        SpiceRect* rect;
        int size;

        rect = &drawable->red_drawable->u.copy.src_area;
        size = (rect->right - rect->left) * (rect->bottom - rect->top);
        if (size < RED_STREAM_MIN_SIZE)
            return FALSE;
    }

    return TRUE;
}

void display_channel_print_stats(DisplayChannel *display)
{
#ifdef RED_WORKER_STAT
    stat_time_t total = display->add_stat.total;
    spice_info("add with shadow count %u",
               display->add_with_shadow_count);
    display->add_with_shadow_count = 0;
    spice_info("add[%u] %f exclude[%u] %f __exclude[%u] %f",
               display->add_stat.count,
               stat_cpu_time_to_sec(total),
               display->exclude_stat.count,
               stat_cpu_time_to_sec(display->exclude_stat.total),
               display->__exclude_stat.count,
               stat_cpu_time_to_sec(display->__exclude_stat.total));
    spice_info("add %f%% exclude %f%% exclude2 %f%% __exclude %f%%",
               (double)(total - display->exclude_stat.total) / total * 100,
               (double)(display->exclude_stat.total) / total * 100,
               (double)(display->exclude_stat.total -
                        display->__exclude_stat.total) / display->exclude_stat.total * 100,
               (double)(display->__exclude_stat.total) / display->exclude_stat.total * 100);
    stat_reset(&display->add_stat);
    stat_reset(&display->exclude_stat);
    stat_reset(&display->__exclude_stat);
#endif
}

int display_channel_add_drawable(DisplayChannel *display, Drawable *drawable)
{
    int ret = FALSE, surface_id = drawable->surface_id;
    RedDrawable *red_drawable = drawable->red_drawable;
    Ring *ring = &display->surfaces[surface_id].current;

    if (has_shadow(red_drawable)) {
        ret = current_add_with_shadow(display, ring, drawable);
    } else {
        drawable->streamable = drawable_can_stream(display, drawable);
        ret = current_add(display, ring, drawable);
    }

    return ret;
}

int display_channel_wait_for_migrate_data(DisplayChannel *display)
{
    uint64_t end_time = red_get_monotonic_time() + DISPLAY_CLIENT_MIGRATE_DATA_TIMEOUT;
    RedChannel *channel = &display->common.base;
    RedChannelClient *rcc;

    spice_debug(NULL);
    spice_assert(channel->clients_num == 1);

    rcc = SPICE_CONTAINEROF(ring_get_head(&channel->clients), RedChannelClient, channel_link);
    spice_assert(red_channel_client_waits_for_migrate_data(rcc));

    for (;;) {
        red_channel_client_receive(rcc);
        if (!red_channel_client_is_connected(rcc)) {
            break;
        }

        if (!red_channel_client_waits_for_migrate_data(rcc)) {
            return TRUE;
        }
        if (red_get_monotonic_time() > end_time) {
            spice_warning("timeout");
            red_channel_client_disconnect(rcc);
            break;
        }
        usleep(DISPLAY_CLIENT_RETRY_INTERVAL);
    }
    return FALSE;
}

void display_channel_flush_all_surfaces(DisplayChannel *display)
{
    int x;

    for (x = 0; x < NUM_SURFACES; ++x) {
        if (display->surfaces[x].context.canvas) {
            display_channel_current_flush(display, x);
        }
    }
}

static void rcc_free_glz_drawables_to_free(RedChannelClient *rcc)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);

    dcc_free_glz_drawables_to_free(dcc);
}

void display_channel_free_glz_drawables_to_free(DisplayChannel *display)
{
    red_channel_apply_clients(RED_CHANNEL(display), rcc_free_glz_drawables_to_free);
}
