/*
 * vox_ws_action.h -- the mapping from a protocol event to what the application
 * must do.
 *
 * WHY IT IS ITS OWN FILE
 *
 * The mapping is the decision layer of the WebSocket path, and it is the part
 * where a mistake is silent: a sentence that never reaches the speaker looks
 * exactly like a slow network, and a state machine that accepts tts.start
 * while not listening produces audio nobody asked for.
 *
 * It is therefore kept as pure functions with no socket, no threads and no
 * globals, so it can be tested on a build host.  vox_ws_channel.c is the thin
 * wiring around it and needs a real connection to run at all; keeping the two
 * apart means the logic is verified without either pretending to verify the
 * transport.
 *
 * The end-to-end socket path is covered separately by
 * server/verify_ws_client.py against a live server.
 */

#ifndef VOX_WS_ACTION_H
#define VOX_WS_ACTION_H

#include "vox_ws_channel.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * What the application should do about a protocol event.
 *
 * VOX_ACT_NONE means the event needs nothing from the application: the session
 * is ready, a sentence ended, or playback started.  It is a real value rather
 * than a "handled" boolean because a caller that ignores one of these has done
 * the right thing, and saying so explicitly keeps the mapping total.
 */
vox_action_e vox_ws_action_for(const vox_msg_t *msg);

/* A short name for an action, for logs. */
const char *vox_ws_action_name(vox_action_e a);

#ifdef __cplusplus
}
#endif

#endif /* VOX_WS_ACTION_H */
