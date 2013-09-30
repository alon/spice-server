#ifndef CURSOR_CHANNEL_H_
# define CURSOR_CHANNEL_H_

#include "spice.h"
#include "reds.h"
#include "red_worker.h"
#include "red_parse_qxl.h"
#include "cache-item.h"
#include "stat.h"

typedef struct _CursorChannel CursorChannel;
typedef struct _CursorChannelClient CursorChannelClient;

#define CURSOR_CHANNEL_CLIENT(Client) ((CursorChannelClient*)(Client))

CursorChannel*       cursor_channel_new         (RedWorker *worker);
void                 cursor_channel_disconnect  (CursorChannel *cursor);
void                 cursor_channel_reset       (CursorChannel *cursor);
void                 cursor_channel_init        (CursorChannel *cursor, CursorChannelClient* client);
void                 cursor_channel_process_cmd (CursorChannel *cursor, RedCursorCmd *cursor_cmd,
                                                 uint32_t group_id);
void                 cursor_channel_set_mouse_mode(CursorChannel *cursor, uint32_t mode);

CursorChannelClient* cursor_channel_client_new  (CursorChannel *cursor,
                                                 RedClient *client, RedsStream *stream,
                                                 int mig_target,
                                                 uint32_t *common_caps, int num_common_caps,
                                                 uint32_t *caps, int num_caps);
void                 cursor_channel_client_migrate(CursorChannelClient* client);

#endif /* CURSOR_CHANNEL_H_ */
