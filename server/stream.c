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
