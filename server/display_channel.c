#include "display_channel.h"

DisplayChannelClient *display_channel_client_new(DisplayChannel *display,
                                                 RedClient *client, RedsStream *stream,
                                                 int mig_target,
                                                 uint32_t *common_caps, int num_common_caps,
                                                 uint32_t *caps, int num_caps)
{
    DisplayChannelClient *dcc;

    dcc = (DisplayChannelClient*)common_channel_new_client(
        COMMON_CHANNEL(display), sizeof(DisplayChannelClient),
        client, stream, mig_target, TRUE,
        common_caps, num_common_caps,
        caps, num_caps);
    spice_return_val_if_fail(dcc, NULL);

    ring_init(&dcc->palette_cache_lru);
    dcc->palette_cache_available = CLIENT_PALETTE_CACHE_SIZE;

    return dcc;
}
