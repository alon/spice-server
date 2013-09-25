#ifndef STREAM_H_
#define STREAM_H_

#include <glib.h>
#include "utils.h"
#include "mjpeg_encoder.h"
#include "common/region.h"
#include "red_channel.h"
#include "spice_image_cache.h"

#define RED_STREAM_DETACTION_MAX_DELTA ((1000 * 1000 * 1000) / 5) // 1/5 sec
#define RED_STREAM_CONTINUS_MAX_DELTA (1000 * 1000 * 1000)
#define RED_STREAM_TIMOUT (1000 * 1000 * 1000)
#define RED_STREAM_FRAMES_START_CONDITION 20
#define RED_STREAM_GRADUAL_FRAMES_START_CONDITION 0.2
#define RED_STREAM_FRAMES_RESET_CONDITION 100
#define RED_STREAM_MIN_SIZE (96 * 96)
#define RED_STREAM_INPUT_FPS_TIMEOUT (5 * 1000) // 5 sec
#define RED_STREAM_CHANNEL_CAPACITY 0.8
/* the client's stream report frequency is the minimum of the 2 values below */
#define RED_STREAM_CLIENT_REPORT_WINDOW 5 // #frames
#define RED_STREAM_CLIENT_REPORT_TIMEOUT 1000 // milliseconds
#define RED_STREAM_DEFAULT_HIGH_START_BIT_RATE (10 * 1024 * 1024) // 10Mbps
#define RED_STREAM_DEFAULT_LOW_START_BIT_RATE (2.5 * 1024 * 1024) // 2.5Mbps
#define MAX_FPS 30
#define NUM_STREAMS 50

/* move back to display_channel once struct private */
typedef struct DisplayChannel DisplayChannel;

typedef struct Stream Stream;

typedef struct StreamActivateReportItem {
    PipeItem pipe_item;
    uint32_t stream_id;
} StreamActivateReportItem;

enum {
    STREAM_FRAME_NONE,
    STREAM_FRAME_NATIVE,
    STREAM_FRAME_CONTAINER,
};

#define STREAM_STATS
#ifdef STREAM_STATS
typedef struct StreamStats {
    uint64_t num_drops_pipe;
    uint64_t num_drops_fps;
    uint64_t num_frames_sent;
    uint64_t num_input_frames;
    uint64_t size_sent;

    uint64_t start;
    uint64_t end;
} StreamStats;
#endif

typedef struct StreamAgent {
    QRegion vis_region; /* the part of the surface area that is currently occupied by video
                           fragments */
    QRegion clip;       /* the current video clipping. It can be different from vis_region:
                           for example, let c1 be the clip area at time t1, and c2
                           be the clip area at time t2, where t1 < t2. If c1 contains c2, and
                           at least part of c1/c2, hasn't been covered by a non-video images,
                           vis_region will contain c2 and also the part of c1/c2 that still
                           displays fragments of the video */

    PipeItem create_item;
    PipeItem destroy_item;
    Stream *stream;
    uint64_t last_send_time;
    MJpegEncoder *mjpeg_encoder;
    DisplayChannelClient *dcc;

    int frames;
    int drops;
    int fps;

    uint32_t report_id;
    uint32_t client_required_latency;
#ifdef STREAM_STATS
    StreamStats stats;
#endif
} StreamAgent;

typedef struct StreamClipItem {
    PipeItem base;
    int refs;
    StreamAgent *stream_agent;
    int clip_type;
    SpiceClipRects *rects;
} StreamClipItem;

StreamClipItem *      stream_clip_item_new                          (DisplayChannelClient* dcc,
                                                                     StreamAgent *agent);
void                  stream_clip_item_unref                        (DisplayChannelClient *dcc,
                                                                     StreamClipItem *item);

typedef struct ItemTrace {
    red_time_t time;
    int frames_count;
    int gradual_frames_count;
    int last_gradual_frame;
    int width;
    int height;
    SpiceRect dest_area;
} ItemTrace;

struct Stream {
    uint8_t refs;
    Drawable *current;
    red_time_t last_time;
    int width;
    int height;
    SpiceRect dest_area;
    int top_down;
    Stream *next;
    RingItem link;

    guint input_fps_timer;
    uint32_t num_input_frames;
    uint64_t input_fps_timer_start;
    uint32_t input_fps;
};

void                  stream_init                                   (DisplayChannel *display);
void                  stream_stop                                   (DisplayChannel *display,
                                                                     Stream *stream);
void                  stream_unref                                  (DisplayChannel *display,
                                                                     Stream *stream);
void                  stream_agent_unref                            (DisplayChannel *display,
                                                                     StreamAgent *agent);
void                  stream_agent_stats_print                      (StreamAgent *agent);
void                  stream_trace_update                           (DisplayChannel *display,
                                                                     Drawable *drawable);
void                  stream_maintenance                            (DisplayChannel *display,
                                                                     Drawable *candidate,
                                                                     Drawable *prev);

void detach_stream(DisplayChannel *display, Stream *stream, int detach_sized);

#endif /* STREAM_H */
