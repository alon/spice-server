#ifndef RED_REPLAY_QXL_H
#define RED_REPLAY_QXL_H

#include <stdio.h>
#include <spice/qxl_dev.h>
#include <spice.h>

typedef struct SpiceReplay SpiceReplay;

/* reads until encountering a cmd, processing any recorded messages (io) on the
 * way */
QXLCommandExt*  spice_replay_next_cmd(SpiceReplay *replay, QXLWorker *worker);
void            spice_replay_free_cmd(SpiceReplay *replay, QXLCommandExt *cmd);
void            spice_replay_free(SpiceReplay *replay);
SpiceReplay *   spice_replay_new(FILE *file, int nsurfaces);

#endif // RED_REPLAY_QXL_H
