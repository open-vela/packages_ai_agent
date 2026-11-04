/*
 * vox_ws_protocol.h -- the ai_vox server protocol, parsed independently of any
 * transport.
 *
 * WHY THIS EXISTS
 *
 * ai_vox (C:\Users\xxzx\Desktop\VOX3\ai_vox\ai_vox-main) speaks a single
 * long-lived WebSocket to the backend and the backend pushes messages as they
 * become available:
 *
 *     "hello"session_id        handshake reply, engine then starts listening
 *     "goodbye"           session_id
 *     "stt"               text                       user speech recognised
 *     "tts"state=start                            begin playback
 *     "tts"      state=sentence_start  text     one sentence arrived
 *     "tts"      state=sentence_end
 *     "tts"      state=stop                     end of playback
 *     "llm"              emotion
 *     "mcp"              payload
 *
 * The sentence_start message is the whole point: it carries text the moment the
 * model has produced a sentence, which is what lets playback start while the
 * rest of the answer is still being generated.
 *
 * OpenVela currently drives ASR, LLM and TTS as three independent REST calls,
 * so playback cannot begin until the model has finished the entire reply.  That
 * is a topology difference, not a server limitation -- this board already has
 * WebSocket streaming elsewhere (voice/volc_tts_ws.c, voice/volc_asr.c).
 *
 * DESIGN
 *
 * Parsing is separated from transport on purpose.  A WebSocket client needs a
 * TLS connection, a proxy tunnel and a reader thread, none of which can be
 * exercised on a host; the state machine and the message parsing can be, and
 * they are where the actual protocol bugs live.  So this file takes a byte
 * buffer and produces events, and vox_ws_session drives it.
 *
 * The state machine mirrors ai_vox's State enum:
 *
 *     kIdle -> kInitted -> kLoadingProtocol -> kStandby
 *            -> kWebsocketConnecting -> kWebsocketConnected
 *            -> kListening <-> kSpeaking
 *
 * with the failure states kLoadingProtocolFailed and kWebsocketConnectedFailed
 * that ai_vox also carries.  Advance() is ai_vox's Advance(): it is the
 * user-interrupt path, so pressing the button while listening drops the
 * connection and while speaking aborts the playback.
 */

#ifndef VOX_WS_PROTOCOL_H
#define VOX_WS_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Mirrors ai_vox::EngineImpl::State. */
typedef enum
{
  VOX_STATE_IDLE = 0,
  VOX_STATE_INITTED,
  VOX_STATE_LOADING_PROTOCOL,
  VOX_STATE_LOADING_PROTOCOL_FAILED,
  VOX_STATE_WEBSOCKET_CONNECTING,
  VOX_STATE_WEBSOCKET_CONNECTING_WITH_WAKEUP,
  VOX_STATE_WEBSOCKET_CONNECTED,
  VOX_STATE_WEBSOCKET_CONNECTED_WITH_WAKEUP,
  VOX_STATE_WEBSOCKET_CONNECTED_FAILED,
  VOX_STATE_STANDBY,
  VOX_STATE_LISTENING,
  VOX_STATE_SPEAKING
} vox_state_e;

/* Events delivered to the application.  The observer callbacks map one to one
 * onto these, which is what ai_vox's Observer interface does. */
typedef enum
{
  VOX_EV_NONE = 0,
  VOX_EV_SESSION_READY,  /* hello received; safe to start sending audio */
  VOX_EV_SESSION_ENDED,  /* goodbye, or the peer closed cleanly */
  VOX_EV_STT_TEXT,       /* stt.text  -- the recognised user utterance */
  VOX_EV_TTS_START,      /* tts state=start */
  VOX_EV_TTS_SENTENCE,   /* tts state=sentence_start, text != NULL */
  VOX_EV_TTS_SENTENCE_END, /* tts state=sentence_end */
  VOX_EV_TTS_STOP,       /* tts state=stop */
  VOX_EV_EMOTION,        /* llm.emotion */
  VOX_EV_MCP_CALL,       /* mcp payload, raw json for the tool manager */
  VOX_EV_AUDIO_DATA,     /* binary frame, opus payload */
  VOX_EV_CONNECTED,
  VOX_EV_DISCONNECTED,
  VOX_EV_ERROR
} vox_event_e;

/* One parsed server message. */
typedef struct
{
  vox_event_e event;
  const char *text; /* owned by the session, valid until the next parse */
  const char *session_id;
  const char *emotion;
  const char *mcp_json; /* raw payload string for VOX_EV_MCP_CALL */
  int         http_status; /* set on VOX_EV_ERROR when the peer sent one */
} vox_msg_t;

/* Observer, matching ai_vox's Observer but with the chat-state bits removed:
 * those are presentation concerns that belong to the UI layer here. */
typedef struct vox_ws_session vox_ws_session_t;

typedef struct
{
  void *ctx;
  void (*on_event)(void *ctx, const vox_msg_t *msg);
  /* Called when a message needs to go out.  The session never touches the
   * transport itself, so tests can capture the bytes instead of a socket. */
  int (*on_send_text)(void *ctx, const char *json, size_t len);
  int (*on_send_binary)(void *ctx, const uint8_t *data, size_t len);
} vox_ws_observer_t;

struct vox_ws_session
{
  vox_state_e           state;
  char                  session_id[64];
  char                  uuid[40];
  char                  ws_url[256];
  char                  hello_msg[512];
  bool                  wakeup_pending; /* connect was triggered by a wakeup */
  bool                  speak_started;  /* tts.start seen, stop not yet */
  uint32_t              t0_ms;          /* start of the current playback */
  uint32_t              latency_ms;     /* measured handshake+first reply */
  uint32_t              recv_frames;
  uint32_t              sent_frames;
  uint32_t              parse_errors;
  bool                  want_close;
  bool                  suppress_audio; /* [NOWEATHER-CLOUD] 本句被禁播，丢弃音频帧 */
  vox_ws_observer_t     obs;
};

/* ai_vox derives the device UUID from the MAC address (GetMacAddress/Uuid in
 * ai_vox_engine_impl.cpp).  The caller passes the MAC so this module stays free
 * of platform dependencies; it also makes the derivation testable. */
void vox_ws_make_uuid(const uint8_t mac[6], char *out, size_t cap);

/* Fills in the hello payload ai_vox sends on connect: device_id, uuid,
 * version and the audio parameters.  Returns the length written, or -1. */
int vox_ws_build_hello(vox_ws_session_t *s, const char *version,
                       uint32_t sample_rate, uint16_t frame_duration_ms);

int  vox_ws_init(vox_ws_session_t *s, const vox_ws_observer_t *obs,
                 const uint8_t mac[6]);
void vox_ws_set_url(vox_ws_session_t *s, const char *url);

/* Feeds one complete text frame (the websocket layer strips the framing). */
void vox_ws_on_text(vox_ws_session_t *s, const char *text, size_t len);

/* Feeds one complete binary frame. */
void vox_ws_on_binary(vox_ws_session_t *s, const uint8_t *data, size_t len);

void vox_ws_on_connected(vox_ws_session_t *s);
void vox_ws_on_disconnected(vox_ws_session_t *s);

/* ai_vox's StartListening(): switches to kListening. */
void vox_ws_start_listening(vox_ws_session_t *s);

/* ai_vox's Advance(): the interrupt path.  Returns the state it moved to. */
vox_state_e vox_ws_advance(vox_ws_session_t *s);

/* ai_vox's AbortSpeaking(). */
void vox_ws_abort_speaking(vox_ws_session_t *s);

/* Back to standby after playback finished; called from the playback-drained
 * callback exactly like ai_vox's OnAudioOutputDataConsumed(). */
void vox_ws_on_playback_done(vox_ws_session_t *s);

/* Exposed for tests and for the NSH shell. */
const char *vox_ws_state_name(vox_state_e st);

#ifdef __cplusplus
}
#endif

#endif /* VOX_WS_PROTOCOL_H */