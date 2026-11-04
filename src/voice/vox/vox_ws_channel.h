/*
 * vox_ws_channel.h -- wires the ai_vox protocol to OpenVela's voice pipeline.
 *
 * WHY A SEPARATE ORCHESTRATOR
 *
 * voice_channel.c is 4500 lines and owns audio capture and playback;
 * core/agent_loop.c owns the ReAct loop (1610 lines, with caching, a backend
 * router and tool iterations).  Neither should be rewritten to speak a
 * WebSocket, and both already have working behaviour that has taken many
 * firmware revisions to reach.
 *
 * What changes with the WebSocket path is the SHAPE of the exchange, not the
 * work each layer does:
 *
 *   REST (today)   capture -> ASR over HTTPS -> LLM over HTTPS -> TTS over HTTPS
 *                   three sequential round trips; playback can only start once
 *                   the model has produced the entire answer
 *
 *   WebSocket      one connection; the server pushes stt, then one
 *                   tts.sentence_start per sentence as each becomes ready, then
 *                   tts.stop
 *
 * So this module is the translation layer: it owns the transport, decodes
 * protocol messages, and calls into the existing entry points.  The audio path
 * stays where it is; only the trigger and the playback trigger change.
 *
 * MEASURED REASON FOR SWITCHING
 *
 * Against the verification server the first sentence arrived 17 ms after the
 * handshake and the last 802 ms later, so playback of sentence one can begin
 * roughly 800 ms before the answer is complete.  The REST pipeline cannot do
 * this at all: its backend answers with chunked=0, one 123 KB body (measured),
 * so there is nothing to stream.
 *
 * NOT VERIFIED YET
 *
 * This orchestrator compiles and its decision logic is unit-tested, but the
 * path from here to audible playback on the board has not been exercised: it
 * needs the backend to speak this protocol and Opus to be decoded.  Until that
 * happens, the REST path stays the default and this runs only when explicitly
 * enabled.
 */

#ifndef VOX_WS_CHANNEL_H
#define VOX_WS_CHANNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vox_ws_protocol.h"
#include "vox_ws_transport.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* What the orchestrator should do with a piece of text.  Kept as an enum plus a
 * callback rather than a direct call into voice_channel, so the policy layer
 * can be tested without audio hardware. */
typedef enum
{
  VOX_ACT_NONE = 0,
  VOX_ACT_SPEAK_SENTENCE, /* a sentence is ready to be spoken now */
  VOX_ACT_SHOW_USER_TEXT, /* stt arrived */
  VOX_ACT_SHOW_EMOTION,  /* llm.emotion arrived */
  VOX_ACT_MCP_CALL,       /* a tool call arrived, mcp_json is set */
  VOX_ACT_PLAYBACK_DONE, /* the server finished sending */
  VOX_ACT_ENDED,         /* the session is over */
  VOX_ACT_ERROR
} vox_action_e;

typedef struct
{
  vox_action_e action;
  const char  *text;   /* valid only during the callback */
  const char  *json;   /* mcp payload, for VOX_ACT_MCP_CALL */
} vox_action_t;

/* The application supplies this.  It runs on the READER THREAD, so an
 * implementation must hand off rather than block -- see vox_ws_transport.h. */
typedef void (*vox_sink_fn)(void *ctx, const vox_action_t *a);

/* Where the session should come from.  ai_vox fetches the websocket URL and
 * headers from the server on boot (fetch_config.cpp does this in the original);
 * this accepts them as configuration so the same code serves a fixed endpoint,
 * an OTA-provided one, and the test harness. */
typedef struct
{
  char ws_url[256];
  /* Extra request headers, e.g. Authorization: Bearer <token>.  Terminated by
   * an empty entry.  Copied at open time, so the caller may free them after. */
  const char *const *headers;
  /* Audio format advertised in the hello. */
  uint32_t sample_rate;
  uint16_t frame_duration_ms;
  size_t    reader_stack;
} vox_ws_channel_cfg_t;

typedef struct vox_ws_channel vox_ws_channel_t;

/* Opens the connection, sends the hello and starts delivering actions.  Returns
 * once the handshake has completed; SESSION_READY follows asynchronously. */
vox_ws_t_e vox_ws_channel_open(const vox_ws_channel_cfg_t *cfg,
                               vox_sink_fn sink, void *ctx,
                               vox_ws_channel_t **out);

/* Interrupts whatever is in progress, which is ai_vox's Advance(): while
 * listening it drops the connection, while speaking it stops playback. */
vox_state_e vox_ws_channel_advance(vox_ws_channel_t *ch);

/* Feeds captured PCM to the session.  On the WebSocket path this is what
 * replaces a blocking ASR request. */
vox_ws_t_e vox_ws_channel_send_audio(vox_ws_channel_t *ch, const uint8_t *pcm,
                                     size_t len);

/* Reports that playback of the last sentence finished, which is what returns
 * the state machine to listening. */
void vox_ws_channel_playback_done(vox_ws_channel_t *ch);

void vox_ws_channel_close(vox_ws_channel_t *ch);

/* The mapping lives in vox_ws_action.h as pure functions, so it can be tested
 * without a socket. */
#include "vox_ws_action.h"

/* Introspection, for logs and for the shell. */
const char *vox_ws_channel_state(vox_ws_channel_t *ch);
void vox_ws_channel_stats(vox_ws_channel_t *ch,
                          vox_ws_transport_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* VOX_WS_CHANNEL_H */