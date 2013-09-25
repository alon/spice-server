#include "stream.h"
#include "display_channel.h"

#define FPS_TEST_INTERVAL 1
#define FOREACH_STREAMS(display, item)                  \
    for (item = ring_get_head(&(display)->streams);     \
         item != NULL;                                  \
         item = ring_next(&(display)->streams, item))

void stream_agent_stats_print(StreamAgent *agent)
{
#ifdef STREAM_STATS
    StreamStats *stats = &agent->stats;
    double passed_mm_time = (stats->end - stats->start) / 1000.0;
    MJpegEncoderStats encoder_stats = {0};

    if (agent->mjpeg_encoder) {
        mjpeg_encoder_get_stats(agent->mjpeg_encoder, &encoder_stats);
    }

    spice_debug("stream=%p dim=(%dx%d) #in-frames=%lu #in-avg-fps=%.2f #out-frames=%lu "
                "out/in=%.2f #drops=%lu (#pipe=%lu #fps=%lu) out-avg-fps=%.2f "
                "passed-mm-time(sec)=%.2f size-total(MB)=%.2f size-per-sec(Mbps)=%.2f "
                "size-per-frame(KBpf)=%.2f avg-quality=%.2f "
                "start-bit-rate(Mbps)=%.2f end-bit-rate(Mbps)=%.2f",
                agent, agent->stream->width, agent->stream->height,
                stats->num_input_frames,
                stats->num_input_frames / passed_mm_time,
                stats->num_frames_sent,
                (stats->num_frames_sent + 0.0) / stats->num_input_frames,
                stats->num_drops_pipe +
                stats->num_drops_fps,
                stats->num_drops_pipe,
                stats->num_drops_fps,
                stats->num_frames_sent / passed_mm_time,
                passed_mm_time,
                stats->size_sent / 1024.0 / 1024.0,
                ((stats->size_sent * 8.0) / (1024.0 * 1024)) / passed_mm_time,
                stats->size_sent / 1000.0 / stats->num_frames_sent,
                encoder_stats.avg_quality,
                encoder_stats.starting_bit_rate / (1024.0 * 1024),
                encoder_stats.cur_bit_rate / (1024.0 * 1024));
#endif
}

void stream_stop(DisplayChannel *display, Stream *stream)
{
    DisplayChannelClient *dcc;
    RingItem *item, *next;

    spice_return_if_fail(ring_item_is_linked(&stream->link));
    spice_return_if_fail(!stream->current);

    spice_debug("stream %d", get_stream_id(display, stream));
    FOREACH_DCC(display, item, next, dcc) {
        StreamAgent *stream_agent;

        stream_agent = &dcc->stream_agents[get_stream_id(display, stream)];
        region_clear(&stream_agent->vis_region);
        region_clear(&stream_agent->clip);
        spice_assert(!pipe_item_is_linked(&stream_agent->destroy_item));
        if (stream_agent->mjpeg_encoder && dcc->use_mjpeg_encoder_rate_control) {
            uint64_t stream_bit_rate = mjpeg_encoder_get_bit_rate(stream_agent->mjpeg_encoder);

            if (stream_bit_rate > dcc->streams_max_bit_rate) {
                spice_debug("old max-bit-rate=%.2f new=%.2f",
                dcc->streams_max_bit_rate / 8.0 / 1024.0 / 1024.0,
                stream_bit_rate / 8.0 / 1024.0 / 1024.0);
                dcc->streams_max_bit_rate = stream_bit_rate;
            }
        }
        stream->refs++;
        red_channel_client_pipe_add(RED_CHANNEL_CLIENT(dcc), &stream_agent->destroy_item);
        stream_agent_stats_print(stream_agent);
    }
    display->streams_size_total -= stream->width * stream->height;
    ring_remove(&stream->link);
    stream_unref(display, stream);
}

static void stream_free(DisplayChannel *display, Stream *stream)
{
    stream->next = display->free_streams;
    display->free_streams = stream;
}

void stream_init(DisplayChannel *display)
{
    int i;

    ring_init(&display->streams);
    display->free_streams = NULL;
    for (i = 0; i < NUM_STREAMS; i++) {
        Stream *stream = &display->streams_buf[i];
        ring_item_init(&stream->link);
        stream_free(display, stream);
    }
}

void stream_unref(DisplayChannel *display, Stream *stream)
{
    if (--stream->refs)
        return;

    spice_warn_if_fail(!ring_item_is_linked(&stream->link));

    if (stream->input_fps_timer) {
        g_source_remove(stream->input_fps_timer);
        stream->input_fps_timer = 0;
    }
    stream_free(display, stream);
    display->stream_count--;
}

void stream_agent_unref(DisplayChannel *display, StreamAgent *agent)
{
    stream_unref(display, agent->stream);
}

StreamClipItem *stream_clip_item_new(DisplayChannelClient* dcc, StreamAgent *agent)
{
    StreamClipItem *item = spice_new(StreamClipItem, 1);
    red_channel_pipe_item_init(RED_CHANNEL_CLIENT(dcc)->channel,
                               (PipeItem *)item, PIPE_ITEM_TYPE_STREAM_CLIP);

    item->stream_agent = agent;
    agent->stream->refs++;
    item->refs = 1;
    return item;
}

void stream_clip_item_unref(DisplayChannelClient *dcc, StreamClipItem *item)
{
    DisplayChannel *display = DCC_TO_DC(dcc);

    if (--item->refs)
        return;

    stream_agent_unref(display, item->stream_agent);
    free(item->rects);
    free(item);
}

static int is_stream_start(Drawable *drawable)
{
    return ((drawable->frames_count >= RED_STREAM_FRAMES_START_CONDITION) &&
            (drawable->gradual_frames_count >=
             (RED_STREAM_GRADUAL_FRAMES_START_CONDITION * drawable->frames_count)));
}

static void update_copy_graduality(Drawable *drawable)
{
    SpiceBitmap *bitmap;
    spice_return_if_fail(drawable->red_drawable->type == QXL_DRAW_COPY);

    /* TODO: global property -> per dc/dcc */
    if (streaming_video != STREAM_VIDEO_FILTER) {
        drawable->copy_bitmap_graduality = BITMAP_GRADUAL_INVALID;
        return;
    }

    if (drawable->copy_bitmap_graduality != BITMAP_GRADUAL_INVALID) {
        return; // already set
    }

    bitmap = &drawable->red_drawable->u.copy.src_bitmap->u.bitmap;

    if (!bitmap_fmt_has_graduality(bitmap->format) || bitmap_has_extra_stride(bitmap) ||
        (bitmap->data->flags & SPICE_CHUNKS_FLAGS_UNSTABLE)) {
        drawable->copy_bitmap_graduality = BITMAP_GRADUAL_NOT_AVAIL;
    } else  {
        drawable->copy_bitmap_graduality = bitmap_get_graduality_level(bitmap);
    }
}

static int is_next_stream_frame(DisplayChannel *display,
                                const Drawable *candidate,
                                const int other_src_width,
                                const int other_src_height,
                                const SpiceRect *other_dest,
                                const red_time_t other_time,
                                const Stream *stream,
                                int container_candidate_allowed)
{
    RedDrawable *red_drawable;
    int is_frame_container = FALSE;

    if (!candidate->streamable) {
        return STREAM_FRAME_NONE;
    }

    if (candidate->creation_time - other_time >
            (stream ? RED_STREAM_CONTINUS_MAX_DELTA : RED_STREAM_DETACTION_MAX_DELTA)) {
        return STREAM_FRAME_NONE;
    }

    red_drawable = candidate->red_drawable;
    if (!container_candidate_allowed) {
        SpiceRect* candidate_src;

        if (!rect_is_equal(&red_drawable->bbox, other_dest)) {
            return STREAM_FRAME_NONE;
        }

        candidate_src = &red_drawable->u.copy.src_area;
        if (candidate_src->right - candidate_src->left != other_src_width ||
            candidate_src->bottom - candidate_src->top != other_src_height) {
            return STREAM_FRAME_NONE;
        }
    } else {
        if (rect_contains(&red_drawable->bbox, other_dest)) {
            int candidate_area = rect_get_area(&red_drawable->bbox);
            int other_area = rect_get_area(other_dest);
            /* do not stream drawables that are significantly
             * bigger than the original frame */
            if (candidate_area > 2 * other_area) {
                spice_debug("too big candidate:");
                spice_debug("prev box ==>");
                rect_debug(other_dest);
                spice_debug("new box ==>");
                rect_debug(&red_drawable->bbox);
                return STREAM_FRAME_NONE;
            }

            if (candidate_area > other_area) {
                is_frame_container = TRUE;
            }
        } else {
            return STREAM_FRAME_NONE;
        }
    }

    if (stream) {
        SpiceBitmap *bitmap = &red_drawable->u.copy.src_bitmap->u.bitmap;
        if (stream->top_down != !!(bitmap->flags & SPICE_BITMAP_FLAGS_TOP_DOWN)) {
            return STREAM_FRAME_NONE;
        }
    }
    if (is_frame_container) {
        return STREAM_FRAME_CONTAINER;
    } else {
        return STREAM_FRAME_NATIVE;
    }
}

static void before_reattach_stream(DisplayChannel *display,
                                   Stream *stream, Drawable *new_frame)
{
    DrawablePipeItem *dpi;
    DisplayChannelClient *dcc;
    int index;
    StreamAgent *agent;
    RingItem *ring_item, *next;

    spice_return_if_fail(stream->current);

    if (!red_channel_is_connected(RED_CHANNEL(display))) {
        return;
    }

    if (new_frame->process_commands_generation == stream->current->process_commands_generation) {
        spice_debug("ignoring drop, same process_commands_generation as previous frame");
        return;
    }

    index = get_stream_id(display, stream);
    DRAWABLE_FOREACH_DPI_SAFE(stream->current, ring_item, next, dpi) {
        dcc = dpi->dcc;
        agent = &dcc->stream_agents[index];

        if (!dcc->use_mjpeg_encoder_rate_control &&
            !dcc->common.is_low_bandwidth) {
            continue;
        }

        if (pipe_item_is_linked(&dpi->dpi_pipe_item)) {
#ifdef STREAM_STATS
            agent->stats.num_drops_pipe++;
#endif
            if (dcc->use_mjpeg_encoder_rate_control) {
                mjpeg_encoder_notify_server_frame_drop(agent->mjpeg_encoder);
            } else {
                ++agent->drops;
            }
        }
    }


    FOREACH_DCC(display, ring_item, next, dcc) {
        double drop_factor;

        agent = &dcc->stream_agents[index];

        if (dcc->use_mjpeg_encoder_rate_control) {
            continue;
        }
        if (agent->frames / agent->fps < FPS_TEST_INTERVAL) {
            agent->frames++;
            continue;
        }
        drop_factor = ((double)agent->frames - (double)agent->drops) /
            (double)agent->frames;
        spice_debug("stream %d: #frames %u #drops %u", index, agent->frames, agent->drops);
        if (drop_factor == 1) {
            if (agent->fps < MAX_FPS) {
                agent->fps++;
                spice_debug("stream %d: fps++ %u", index, agent->fps);
            }
        } else if (drop_factor < 0.9) {
            if (agent->fps > 1) {
                agent->fps--;
                spice_debug("stream %d: fps--%u", index, agent->fps);
            }
        }
        agent->frames = 1;
        agent->drops = 0;
    }
}

static gboolean red_stream_input_fps_timer_cb(void *opaque)
{
    Stream *stream = opaque;
    uint64_t now = red_get_monotonic_time();
    double duration_sec;

    spice_return_val_if_fail(opaque != NULL, TRUE);
    spice_return_val_if_fail(now != stream->input_fps_timer_start, TRUE);

    duration_sec = (now - stream->input_fps_timer_start)/(1000.0*1000*1000);
    stream->input_fps = stream->num_input_frames / duration_sec;
    spice_debug("input-fps=%u", stream->input_fps);
    stream->num_input_frames = 0;
    stream->input_fps_timer_start = now;

    return TRUE;
}

static Stream *display_channel_stream_try_new(DisplayChannel *display)
{
    Stream *stream;
    if (!display->free_streams) {
        return NULL;
    }
    stream = display->free_streams;
    display->free_streams = display->free_streams->next;
    return stream;
}

static void display_channel_create_stream(DisplayChannel *display, Drawable *drawable)
{
    DisplayChannelClient *dcc;
    RingItem *dcc_ring_item, *next;
    Stream *stream;
    SpiceRect* src_rect;

    spice_assert(!drawable->stream);

    if (!(stream = display_channel_stream_try_new(display))) {
        return;
    }

    spice_assert(drawable->red_drawable->type == QXL_DRAW_COPY);
    src_rect = &drawable->red_drawable->u.copy.src_area;

    ring_add(&display->streams, &stream->link);
    stream->current = drawable;
    stream->last_time = drawable->creation_time;
    stream->width = src_rect->right - src_rect->left;
    stream->height = src_rect->bottom - src_rect->top;
    stream->dest_area = drawable->red_drawable->bbox;
    stream->refs = 1;
    SpiceBitmap *bitmap = &drawable->red_drawable->u.copy.src_bitmap->u.bitmap;
    stream->top_down = !!(bitmap->flags & SPICE_BITMAP_FLAGS_TOP_DOWN);
    drawable->stream = stream;

    GSource *source = g_timeout_source_new(RED_STREAM_INPUT_FPS_TIMEOUT);
    g_source_set_callback(source, red_stream_input_fps_timer_cb, stream, NULL);
    stream->input_fps_timer =
        g_source_attach(source, red_worker_get_context(COMMON_CHANNEL(display)->worker));
    g_source_unref(source);

    stream->num_input_frames = 0;
    stream->input_fps_timer_start = red_get_monotonic_time();
    stream->input_fps = MAX_FPS;
    display->streams_size_total += stream->width * stream->height;
    display->stream_count++;
    FOREACH_DCC(display, dcc_ring_item, next, dcc) {
        dcc_create_stream(dcc, stream);
    }
    spice_debug("stream %d %dx%d (%d, %d) (%d, %d)",
                (int)(stream - display->streams_buf), stream->width,
                stream->height, stream->dest_area.left, stream->dest_area.top,
                stream->dest_area.right, stream->dest_area.bottom);
    return;
}

// returns whether a stream was created
static int stream_add_frame(DisplayChannel *display,
                            Drawable *frame_drawable,
                            int frames_count,
                            int gradual_frames_count,
                            int last_gradual_frame)
{
    update_copy_graduality(frame_drawable);
    frame_drawable->frames_count = frames_count + 1;
    frame_drawable->gradual_frames_count  = gradual_frames_count;

    if (frame_drawable->copy_bitmap_graduality != BITMAP_GRADUAL_LOW) {
        if ((frame_drawable->frames_count - last_gradual_frame) >
            RED_STREAM_FRAMES_RESET_CONDITION) {
            frame_drawable->frames_count = 1;
            frame_drawable->gradual_frames_count = 1;
        } else {
            frame_drawable->gradual_frames_count++;
        }

        frame_drawable->last_gradual_frame = frame_drawable->frames_count;
    } else {
        frame_drawable->last_gradual_frame = last_gradual_frame;
    }

    if (is_stream_start(frame_drawable)) {
        display_channel_create_stream(display, frame_drawable);
        return TRUE;
    }
    return FALSE;
}

/* TODO: document the difference between the 2 functions below */
void stream_trace_update(DisplayChannel *display, Drawable *drawable)
{
    ItemTrace *trace;
    ItemTrace *trace_end;
    RingItem *item;

    if (drawable->stream || !drawable->streamable || drawable->frames_count) {
        return;
    }

    FOREACH_STREAMS(display, item) {
        Stream *stream = SPICE_CONTAINEROF(item, Stream, link);
        int is_next_frame = is_next_stream_frame(display,
                                                 drawable,
                                                 stream->width,
                                                 stream->height,
                                                 &stream->dest_area,
                                                 stream->last_time,
                                                 stream,
                                                 TRUE);
        if (is_next_frame != STREAM_FRAME_NONE) {
            if (stream->current) {
                stream->current->streamable = FALSE; //prevent item trace
                before_reattach_stream(display, stream, drawable);
                detach_stream(display, stream, FALSE);
            }
            attach_stream(display, drawable, stream);
            if (is_next_frame == STREAM_FRAME_CONTAINER) {
                drawable->sized_stream = stream;
            }
            return;
        }
    }

    trace = display->items_trace;
    trace_end = trace + NUM_TRACE_ITEMS;
    for (; trace < trace_end; trace++) {
        if (is_next_stream_frame(display, drawable, trace->width, trace->height,
                                       &trace->dest_area, trace->time, NULL, FALSE) !=
                                       STREAM_FRAME_NONE) {
            if (stream_add_frame(display, drawable,
                                 trace->frames_count,
                                 trace->gradual_frames_count,
                                 trace->last_gradual_frame)) {
                return;
            }
        }
    }
}

void stream_maintenance(DisplayChannel *display,
                        Drawable *candidate, Drawable *prev)
{
    int is_next_frame;

    if (candidate->stream) {
        return;
    }

    if (prev->stream) {
        Stream *stream = prev->stream;

        is_next_frame = is_next_stream_frame(display, candidate,
                                             stream->width, stream->height,
                                             &stream->dest_area, stream->last_time,
                                             stream, TRUE);
        if (is_next_frame != STREAM_FRAME_NONE) {
            before_reattach_stream(display, stream, candidate);
            detach_stream(display, stream, FALSE);
            prev->streamable = FALSE; //prevent item trace
            attach_stream(display, candidate, stream);
            if (is_next_frame == STREAM_FRAME_CONTAINER) {
                candidate->sized_stream = stream;
            }
        }
    } else if (candidate->streamable) {
        SpiceRect* prev_src = &prev->red_drawable->u.copy.src_area;

        is_next_frame =
            is_next_stream_frame(display, candidate, prev_src->right - prev_src->left,
                                 prev_src->bottom - prev_src->top,
                                 &prev->red_drawable->bbox, prev->creation_time,
                                 prev->stream,
                                 FALSE);
        if (is_next_frame != STREAM_FRAME_NONE) {
            stream_add_frame(display, candidate,
                             prev->frames_count,
                             prev->gradual_frames_count,
                             prev->last_gradual_frame);
        }
    }
}
