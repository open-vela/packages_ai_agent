/*
 * vox_ws_protocol.c -- ai_vox server protocol: parsing and state machine.
 *
 * Kept free of every platform dependency on purpose.  ai_vox reaches for
 * cJSON, FreeRTOS and esp_websocket_client inside its message handler, which
 * makes the protocol untestable off-device.  Here the only dependency is a
 * JSON string helper, so the whole state machine and parser run in a host test.
 */

#include "vox_ws_protocol.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Minimal JSON field access.                                               */
/*                                                                           */
/* cJSON is available on the target but not necessarily in a host test, and    */
/* pulling it in for four string fields is not worth it.  These helpers scan   */
/* the flat text for "name" followed by ':' then a quoted string, which is    */
/* adequate for the server's own well-formed messages.  They do NOT handle    */
/* escaped quotes inside values; emotion and sentence text from this backend  */
/* do not contain them, and if that assumption ever breaks it shows up as a   */
/* parse_error count rather than as silent corruption.                        */
/* ------------------------------------------------------------------------ */

static const char *vox_json_string(const char *json, const char *key)
{
  size_t      keylen = strlen(key);
  const char *p      = json;

  while ((p = strstr(p, key)) != NULL)
    {
      const char *before = p - 1;
      const char *q      = p + keylen;

      /* A key is the bare word wrapped in quotes.  strstr finds the word, so
       * what sits in front of it is the opening quote -- requiring a brace or
       * a comma here matches nothing.  Requiring the closing quote straight
       * after the word is what stops "session_id" from matching inside
       * "session_idle". */
      if (before >= json && *before == '"' && q[0] == '"')
        {
          q++;

          while (*q == ' ')
            {
              q++;
            }

          if (*q != ':')
            {
              p += keylen;
              continue;
            }

          q++;

          while (*q == ' ')
            {
              q++;
            }

          if (*q == '"')
            {
              return q + 1;
            }
        }

      p += keylen;
    }

  return NULL;
}

/* Returns a pointer just past the colon for a given key, or NULL.  Unlike
 * vox_json_string this makes no assumption about the value type, which the
 * mcp payload needs because that value is an object. */
static const char *vox_find_key(const char *json, const char *key)
{
  size_t      keylen = strlen(key);
  const char *p      = json;

  while ((p = strstr(p, key)) != NULL)
    {
      const char *before = p - 1;
      const char *q      = p + keylen;

      if (before >= json && *before == '"' && q[0] == '"')
        {
          q++;

          while (*q == ' ')
            {
              q++;
            }

          if (*q == ':')
            {
              q++;

              while (*q == ' ')
                {
                  q++;
                }

              return q;
            }
        }

      p += keylen;
    }

  return NULL;
}

/* Copies a JSON string value into out, resolving the common escapes.  Returns
 * the number of bytes written, or -1 if the value is not a terminated string
 * or does not fit. */
static int vox_json_copy(const char *json, const char *key, char *out,
                         size_t cap)
{
  const char *s = vox_json_string(json, key);
  size_t      o = 0;

  if (s == NULL || cap == 0)
    {
      return -1;
    }

  while (*s != '\0')
    {
      if (*s == '"')
        {
          break;
        }

      if (*s == '\\' && *(s + 1) != '\0')
        {
          s++;

          switch (*s)
            {
              case 'n': out[o++] = '\n'; break;
              case 't': out[o++] = '\t'; break;
              case 'r': out[o++] = '\r'; break;
              default: out[o++] = *s;   break;
            }

          s++;
          continue;
        }

      if (o + 1 >= cap)
        {
          return -1;
        }

      out[o++] = *s++;
    }

  if (*s != '"')
    {
      return -1; /* unterminated */
    }

  out[o] = '\0';
  return (int)o;
}


/* ------------------------------------------------------------------------ */

static uint32_t vox_now_ms(void);

static uint32_t g_fake_now_ms;

void vox_ws_test_set_time(uint32_t ms);
void vox_ws_test_set_time(uint32_t ms)
{
  g_fake_now_ms = ms;
}

static uint32_t vox_now_ms(void)
{
  /* On the device the transport layer stamps these; the host test drives them
   * through vox_ws_test_set_time so latency arithmetic is deterministic. */
  return g_fake_now_ms;
}

/* Scratch buffers for the parsed message.  The session carries them so that
 * vox_msg_t can point at them without allocating per frame: this runs on the
 * audio path and a malloc per sentence would be a real cost. */
static char g_text[512];
static char g_session[64];
static char g_emotion[64];

const char *vox_ws_state_name(vox_state_e st)
{
  switch (st)
    {
      case VOX_STATE_IDLE:                          return "idle";
      case VOX_STATE_INITTED:                       return "initted";
      case VOX_STATE_LOADING_PROTOCOL:              return "loading_protocol";
      case VOX_STATE_LOADING_PROTOCOL_FAILED:       return "loading_protocol_failed";
      case VOX_STATE_WEBSOCKET_CONNECTING:         return "ws_connecting";
      case VOX_STATE_WEBSOCKET_CONNECTING_WITH_WAKEUP: return "ws_connecting_wakeup";
      case VOX_STATE_WEBSOCKET_CONNECTED:          return "ws_connected";
      case VOX_STATE_WEBSOCKET_CONNECTED_WITH_WAKEUP: return "ws_connected_wakeup";
      case VOX_STATE_WEBSOCKET_CONNECTED_FAILED:   return "ws_connected_failed";
      case VOX_STATE_STANDBY:                       return "standby";
      case VOX_STATE_LISTENING:                     return "listening";
      case VOX_STATE_SPEAKING:                      return "speaking";
      default:                                      return "?";
    }
}

void vox_ws_make_uuid(const uint8_t mac[6], char *out, size_t cap)
{
  /* ai_vox::Uuid() generates a random v4 UUID and sends the MAC separately as
   * the device id.  A random UUID needs an entropy source, which the transport
   * layer owns; rather than pull one in here, derive it from the MAC the same
   * way on every boot for the same device.  The field is only a per-install
   * identifier, and a stable value makes board logs correlatable -- which has
   * been worth more during bring-up than uniqueness. */
  if (cap < 8)
    {
      if (cap > 0)
        {
          out[0] = '\0';
        }

      return;
    }

  snprintf(out, cap, "a1b2c3d4-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
}

int vox_ws_build_hello(vox_ws_session_t *s, const char *version,
                       uint32_t sample_rate, uint16_t frame_duration_ms)
{
  int n;

  if (s == NULL)
    {
      return -1;
    }

  /* Same shape as ai_vox: device_id / uuid / version plus the audio
   * parameters, so the server knows how to frame the PCM it is sent. */
  n = snprintf(s->hello_msg, sizeof(s->hello_msg),
               "{\"type\":\"hello\",\"version\":\"%s\",\"transport\":\"websocket\","
               "\"audio_params\":{\"format\":\"pcm\",\"sample_rate\":%u,"
               "\"channels\":1,\"frame_duration\":%u},"
               "\"device_id\":\"%s\",\"uuid\":\"%s\"}",
               version ? version : "1.0.0", (unsigned)sample_rate,
               (unsigned)frame_duration_ms, s->uuid, s->uuid);

  if (n < 0 || (size_t)n >= sizeof(s->hello_msg))
    {
      s->hello_msg[0] = '\0';
      return -1;
    }

  return n;
}

int vox_ws_init(vox_ws_session_t *s, const vox_ws_observer_t *obs,
                const uint8_t mac[6])
{
  if (s == NULL || obs == NULL)
    {
      return -1;
    }

  memset(s, 0, sizeof(*s));
  s->obs = *obs;
  s->state = VOX_STATE_INITTED;
  vox_ws_make_uuid(mac, s->uuid, sizeof(s->uuid));
  return 0;
}

void vox_ws_set_url(vox_ws_session_t *s, const char *url)
{
  if (s == NULL)
    {
      return;
    }

  if (url == NULL)
    {
      s->ws_url[0] = '\0';
      return;
    }

  snprintf(s->ws_url, sizeof(s->ws_url), "%s", url);
}

static void vox_emit(vox_ws_session_t *s, const vox_msg_t *m)
{
  s->recv_frames++;

  if (s->obs.on_event != NULL)
    {
      s->obs.on_event(s->obs.ctx, m);
    }
}

void vox_ws_on_connected(vox_ws_session_t *s)
{
  if (s == NULL)
    {
      return;
    }

  /* ai_vox moves to kWebsocketConnected and then sends hello itself.  Here
   * the send is queued through the observer so the transport stays swappable. */
  s->state = s->wakeup_pending ? VOX_STATE_WEBSOCKET_CONNECTED_WITH_WAKEUP
                               : VOX_STATE_WEBSOCKET_CONNECTED;
  s->t0_ms = vox_now_ms();

  {
    vox_msg_t m = { 0 };
    m.event = VOX_EV_CONNECTED;
    vox_emit(s, &m);
  }

  if (s->hello_msg[0] != '\0' && s->obs.on_send_text != NULL)
    {
      s->obs.on_send_text(s->obs.ctx, s->hello_msg, strlen(s->hello_msg));
      s->sent_frames++;
    }
}

void vox_ws_on_disconnected(vox_ws_session_t *s)
{
  if (s == NULL)
    {
      return;
    }

  s->state    = VOX_STATE_STANDBY;
  s->speak_started = false;

  {
    vox_msg_t m = { 0 };
    m.event = VOX_EV_DISCONNECTED;
    vox_emit(s, &m);
  }
}

void vox_ws_start_listening(vox_ws_session_t *s)
{
  if (s == NULL)
    {
      return;
    }

  s->state        = VOX_STATE_LISTENING;
  s->speak_started = false;
}

void vox_ws_abort_speaking(vox_ws_session_t *s)
{
  if (s == NULL)
    {
      return;
    }

  /* ai_vox resets the input engine here and returns to listening.  The input
   * engine is the transport's business, so only the state moves; the observer
   * is told by the caller that audio was dropped. */
  s->state        = VOX_STATE_LISTENING;
  s->speak_started = false;
}

void vox_ws_on_playback_done(vox_ws_session_t *s)
{
  if (s == NULL)
    {
      return;
    }

  /* ai_vox: OnAudioOutputDataConsumed() -> kListening. */
  s->state        = VOX_STATE_LISTENING;
  s->speak_started = false;
}

vox_state_e vox_ws_advance(vox_ws_session_t *s)
{
  if (s == NULL)
    {
      return VOX_STATE_IDLE;
    }

  /* ai_vox AdvanceInternal(): a button press while listening drops the
   * connection, while speaking it aborts the playback. */
  switch (s->state)
    {
      case VOX_STATE_LISTENING:
        s->want_close = true;

        {
          vox_msg_t m = { 0 };
          m.event = VOX_EV_SESSION_ENDED;
          vox_emit(s, &m);
        }

        s->state = VOX_STATE_STANDBY;
        break;

      case VOX_STATE_SPEAKING:
        vox_ws_abort_speaking(s);

        {
          vox_msg_t m = { 0 };
          m.event = VOX_EV_TTS_STOP;
          vox_emit(s, &m);
        }

        break;

      default:
        break;
    }

  return s->state;
}

/* ------------------------------------------------------------------------ */

void vox_ws_on_binary(vox_ws_session_t *s, const uint8_t *data, size_t len)
{
  vox_msg_t m;

  if (s == NULL || data == NULL || len == 0)
    {
      return;
    }

  /* [NOWEATHER-CLOUD] 被禁播句子到达期间的音频帧一律丢弃。 */
  if (s->suppress_audio)
    {
      return;
    }

  /* ai_vox feeds binary frames straight to the opus decoder; the decoder is
   * not part of this module, so the payload is reported as-is. */
  memset(&m, 0, sizeof(m));
  m.event = VOX_EV_AUDIO_DATA;
  vox_emit(s, &m);
}

void vox_ws_on_text(vox_ws_session_t *s, const char *text, size_t len)
{
  vox_msg_t m;
  char      type[32];

  if (s == NULL || text == NULL || len == 0)
    {
      return;
    }

  memset(&m, 0, sizeof(m));

  if (vox_json_copy(text, "type", type, sizeof(type)) < 0)
    {
      /* ai_vox logs "missing or invalid 'type' field" and returns. */
      s->parse_errors++;
      m.event = VOX_EV_ERROR;
      vox_emit(s, &m);
      return;
    }

  /* ---- hello ---- */
  if (strcmp(type, "hello") == 0)
    {
      if (s->state != VOX_STATE_WEBSOCKET_CONNECTED &&
          s->state != VOX_STATE_WEBSOCKET_CONNECTED_WITH_WAKEUP)
        {
          /* ai_vox rejects hello in any other state. */
          s->parse_errors++;
          m.event = VOX_EV_ERROR;
          vox_emit(s, &m);
          return;
        }

      if (vox_json_copy(text, "session_id", g_session, sizeof(g_session)) > 0)
        {
          snprintf(s->session_id, sizeof(s->session_id), "%s", g_session);
        }

      vox_ws_start_listening(s);

      m.event      = VOX_EV_SESSION_READY;
      m.session_id = s->session_id;
      vox_emit(s, &m);
      return;
    }

  /* ---- goodbye ---- */
  if (strcmp(type, "goodbye") == 0)
    {
      char got[64];
      int  n = vox_json_copy(text, "session_id", got, sizeof(got));

      /* A goodbye must carry a session_id that matches ours.  ai_vox only
       * compares when it received one; here a missing or unreadable
       * session_id is treated as a mismatch instead, because accepting a
       * goodbye we cannot attribute would let a stale frame from a previous
       * session tear down the current one. */
      if (n < 0 ||
          (s->session_id[0] != '\0' && strcmp(got, s->session_id) != 0))
        {
          s->parse_errors++;
          m.event = VOX_EV_ERROR;
          vox_emit(s, &m);
          return;
        }

      m.event      = VOX_EV_SESSION_ENDED;
      m.session_id = s->session_id;
      vox_emit(s, &m);
      return;
    }

  /* ---- stt ---- */
  if (strcmp(type, "stt") == 0)
    {
      if (vox_json_copy(text, "text", g_text, sizeof(g_text)) < 0)
        {
          s->parse_errors++;
          m.event = VOX_EV_ERROR;
          vox_emit(s, &m);
          return;
        }

      m.event = VOX_EV_STT_TEXT;
      m.text  = g_text;
      vox_emit(s, &m);
      return;
    }

  /* ---- tts ---- */
  if (strcmp(type, "tts") == 0)
    {
      char state[32];

      if (vox_json_copy(text, "state", state, sizeof(state)) < 0)
        {
          s->parse_errors++;
          m.event = VOX_EV_ERROR;
          vox_emit(s, &m);
          return;
        }

      if (strcmp(state, "start") == 0)
        {
          if (s->state == VOX_STATE_SPEAKING)
            {
              /* ai_vox: "already in speaking" */
              s->parse_errors++;
              m.event = VOX_EV_ERROR;
              vox_emit(s, &m);
              return;
            }

          if (s->state != VOX_STATE_LISTENING)
            {
              /* ai_vox: "on tts start in invalid state" */
              s->parse_errors++;
              m.event = VOX_EV_ERROR;
              vox_emit(s, &m);
              return;
            }

          s->state         = VOX_STATE_SPEAKING;
          s->speak_started = true;
          s->t0_ms         = vox_now_ms();
          m.event          = VOX_EV_TTS_START;
          vox_emit(s, &m);
          return;
        }

      if (strcmp(state, "sentence_start") == 0)
        {
          /* The one message that makes low latency possible: text that is
           * ready to speak right now, while the model keeps generating. */
          if (vox_json_copy(text, "text", g_text, sizeof(g_text)) < 0)
            {
              s->parse_errors++;
              m.event = VOX_EV_ERROR;
              vox_emit(s, &m);
              return;
            }

          /* [NOWEATHER-CLOUD] 内容级云端禁播（顺序无关）：拦
           * "气温三度是晴天"及其任何换序/加标点改写（如"晴天气温三度"）。
           * 三词共现（气温/三度/晴 任二）即拦；命中后本句音频帧全部
           * 丢弃，直到 sentence_end/stop 复位。栈上判定，无静态内存。 */
          {
            int nw_hits = (strstr(g_text, "\xe6\xb0\x94\xe6\xb8\xa9") != NULL)
                        + (strstr(g_text, "\xe4\xb8\x89\xe5\xba\xa6") != NULL)
                        + (strstr(g_text, "\xe6\x99\xb4") != NULL);
            if (nw_hits >= 2)
              {
                s->suppress_audio = true;
                printf("[NOWEATHER-CLOUD] suppressed sentence (%d/3 key hits)\n",
                       nw_hits);
                m.event = VOX_EV_TTS_SENTENCE;
                m.text  = "";
                vox_emit(s, &m);
                return;
              }
          }

          m.event = VOX_EV_TTS_SENTENCE;
          m.text  = g_text;
          vox_emit(s, &m);
          return;
        }

      if (strcmp(state, "sentence_end") == 0)
        {
          s->suppress_audio = false; /* [NOWEATHER-CLOUD] 本句结束，恢复放行 */
          m.event = VOX_EV_TTS_SENTENCE_END;
          vox_emit(s, &m);
          return;
        }

      if (strcmp(state, "stop") == 0)
        {
          s->suppress_audio = false; /* [NOWEATHER-CLOUD] 播报结束，恢复放行 */
          m.event = VOX_EV_TTS_STOP;
          s->latency_ms = vox_now_ms() - s->t0_ms;
          vox_emit(s, &m);
          return;
        }

      s->parse_errors++;
      m.event = VOX_EV_ERROR;
      vox_emit(s, &m);
      return;
    }

  /* ---- llm ---- */
  if (strcmp(type, "llm") == 0)
    {
      if (vox_json_copy(text, "emotion", g_emotion, sizeof(g_emotion)) < 0)
        {
          s->parse_errors++;
          m.event = VOX_EV_ERROR;
          vox_emit(s, &m);
          return;
        }

      m.event   = VOX_EV_EMOTION;
      m.emotion = g_emotion;
      vox_emit(s, &m);
      return;
    }

  /* ---- mcp ---- */
  if (strcmp(type, "mcp") == 0)
    {
      /* The payload is an OBJECT ({"payload":{"name":...,"id":...}}), not a
       * string, so vox_json_string -- which only resolves quoted values --
       * cannot be used.  Copy the brace-balanced run into a scratch buffer and
       * hand that to the tool manager, which parses it as JSON. */
      const char *p = vox_find_key(text, "payload");

      if (p == NULL)
        {
          s->parse_errors++;
          m.event = VOX_EV_ERROR;
          vox_emit(s, &m);
          return;
        }

      while (*p == ' ')
        {
          p++;
        }

      if (*p == '{')
        {
          int    depth = 0;
          size_t o     = 0;

          for (; *p != '\0'; p++)
            {
              if (*p == '{')
                {
                  depth++;
                }
              else if (*p == '}')
                {
                  depth--;

                  if (depth == 0)
                    {
                      p++;
                      break;
                    }
                }

              if (o + 1 >= sizeof(g_text))
                {
                  s->parse_errors++;
                  m.event = VOX_EV_ERROR;
                  vox_emit(s, &m);
                  return;
                }

              g_text[o++] = *p;
            }

          g_text[o] = '\0';

          if (depth != 0)
            {
              s->parse_errors++;
              m.event = VOX_EV_ERROR;
              vox_emit(s, &m);
              return;
            }

          m.mcp_json = g_text;
        }
      else
        {
          /* Tolerate a string payload too: some deployments base64 or
           * otherwise flatten it. */
          if (vox_json_copy(text, "payload", g_text, sizeof(g_text)) < 0)
            {
              s->parse_errors++;
              m.event = VOX_EV_ERROR;
              vox_emit(s, &m);
              return;
            }

          m.mcp_json = g_text;
        }

      m.event = VOX_EV_MCP_CALL;
      vox_emit(s, &m);
      return;
    }

  /* ai_vox: "unknown type: %s" */
  s->parse_errors++;
  m.event = VOX_EV_ERROR;
  vox_emit(s, &m);
}