#include "stream.h"
#include "display_channel.h"

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
