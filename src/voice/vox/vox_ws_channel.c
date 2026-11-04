/*
 * vox_ws_channel.c -- the translation layer between the ai_vox protocol and
 * OpenVela's voice pipeline.  See vox_ws_channel.h for why it exists.
 */

#include "vox_ws_channel.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "agent_compat.h"

#define TAG "[vox-ch]"

struct vox_ws_channel
{
  vox_ws_transport_t     *tp;
  vox_ws_session_t        session;
  vox_sink_fn             sink;
  void                   *ctx;

  /* The protocol session's observer needs to reach the transport to send the
   * hello, and the transport's message callback needs to reach the session. */
  pthread_mutex_t         lock;
  char                    uuid[40];
  char                    hello[512];
  bool                    hello_ready;
  bool                    ended;
  uint32_t                sentences;
  uint32_t                stt_seen;
};

static const char *TAGP = "[vox-ch]";

/* ------------------------------------------------------- session observer */

/* The protocol layer builds the hello and asks for it to be sent.  The channel
 * holds the transport, so this is where the two meet. */
static int ch_send_text(void *ctx, const char *json, size_t len)
{
  vox_ws_channel_t *ch = (vox_ws_channel_t *)ctx;

  (void)len;

  if (ch == NULL || ch->tp == NULL)
    {
      return -1;
    }

  return vox_ws_transport_send_text(ch->tp, json) == VOX_WS_T_OK ? 0 : -1;
}

static int ch_send_binary(void *ctx, const uint8_t *data, size_t len)
{
  vox_ws_channel_t *ch = (vox_ws_channel_t *)ctx;

  if (ch == NULL || ch->tp == NULL)
    {
      return -1;
    }

  return vox_ws_transport_send_binary(ch->tp, data, len) == VOX_WS_T_OK ? 0
                                                                          : -1;
}

/* ---------------------------------------------------- transport callbacks */

static void emit(vox_ws_channel_t *ch, vox_action_e act, const char *text,
                 const char *json)
{
  vox_action_t a;

  if (ch->sink == NULL)
    {
      return;
    }

  a.action = act;
  a.text   = text;
  a.json   = json;
  ch->sink(ch->ctx, &a);
}

/* Called on the reader thread for every decoded message. */
static void on_message(void *ctx, uint8_t opcode, const uint8_t *data,
                       size_t len)
{
  vox_ws_channel_t *ch = (vox_ws_channel_t *)ctx;

  if (ch == NULL)
    {
      return;
    }

  if (opcode == VOX_WS_OP_BINARY)
    {
      /* Audio.  Opus decoding is not wired yet, so the payload is counted and
       * reported rather than silently dropped: a silent drop here would look
       * exactly like a broken speaker. */
      syslog(LOG_INFO, "%s audio frame %u bytes (not decoded yet)\n", TAGP,
             (unsigned)len);
      return;
    }

  if (opcode != VOX_WS_OP_TEXT)
    {
      return;
    }

  /* vox_ws_on_text takes a NUL-terminated buffer; the decoder's message is not,
   * so copy into a bounded scratch buffer.  The protocol messages are JSON
   * objects well under this. */
  {
    static __thread char scratch[VOX_WS_MAX_MESSAGE];
    size_t n = (len < sizeof(scratch) - 1) ? len : sizeof(scratch) - 1;

    memcpy(scratch, data, n);
    scratch[n] = '\0';

    pthread_mutex_lock(&ch->lock);
    vox_ws_on_text(&ch->session, scratch, n);
    pthread_mutex_unlock(&ch->lock);
  }
}

static void on_closed(void *ctx, vox_ws_t_e reason)
{
  vox_ws_channel_t *ch = (vox_ws_channel_t *)ctx;

  if (ch == NULL)
    {
      return;
    }

  syslog(LOG_INFO, "%s connection ended: %s\n", TAGP,
         vox_ws_transport_name(reason));

  pthread_mutex_lock(&ch->lock);
  ch->ended = true;
  pthread_mutex_unlock(&ch->lock);

  emit(ch, VOX_ACT_ENDED, NULL, NULL);
}

/* ------------------------------------------------------------------ API */

vox_ws_t_e vox_ws_channel_open(const vox_ws_channel_cfg_t *cfg,
                               vox_sink_fn sink, void *ctx,
                               vox_ws_channel_t **out)
{
  vox_ws_channel_t      *ch;
  vox_ws_transport_cfg_t tc;
  vox_ws_observer_t      obs;
  vox_ws_t_e             e;
  uint8_t                mac[6];

  if (cfg == NULL || out == NULL || cfg->ws_url[0] == '\0')
    {
      return VOX_WS_T_ERR_URL;
    }

  *out = NULL;

  ch = (vox_ws_channel_t *)calloc(1, sizeof(*ch));

  if (ch == NULL)
    {
      return VOX_WS_T_ERR_CONNECT;
    }

  ch->sink = sink;
  ch->ctx  = ctx;
  pthread_mutex_init(&ch->lock, NULL);

  /* The device id is a placeholder here: a real deployment passes the MAC the
   * caller already has.  Deriving it would duplicate platform lookups in a
   * module whose job is translation. */
  memset(mac, 0, sizeof(mac));

  memset(&obs, 0, sizeof(obs));
  obs.ctx           = ch;
  obs.on_send_text  = ch_send_text;
  obs.on_send_binary = ch_send_binary;

  if (vox_ws_init(&ch->session, &obs, mac) != 0)
    {
      free(ch);
      return VOX_WS_T_ERR_PROTO;
    }

  if (cfg->headers != NULL)
    {
      /* The transport keeps the pointer, so the caller must keep the array
       * alive.  Copying a variable-length list into a fixed buffer would cap
       * the header count; this is documented in the header. */
      tc.extra_headers = cfg->headers;
    }
  else
    {
      tc.extra_headers = NULL;
    }

  tc.ctx            = ch;
  tc.on_message     = on_message;
  tc.on_closed      = on_closed;
  tc.recv_timeout_s = 0; /* a listening session is idle by design */
  tc.reader_stack   = cfg->reader_stack;

  e = vox_ws_transport_open(cfg->ws_url, &tc, &ch->tp);

  if (e != VOX_WS_T_OK)
    {
      pthread_mutex_destroy(&ch->lock);
      free(ch);
      return e;
    }

  /* Build the hello and hand it to the session; the session's observer sends it
   * through the transport, which is already open. */
  if (vox_ws_build_hello(&ch->session, "1.0.0",
                         cfg->sample_rate ? cfg->sample_rate : 16000,
                         cfg->frame_duration_ms ? cfg->frame_duration_ms : 60)
      <= 0)
    {
      vox_ws_transport_close(ch->tp);
      pthread_mutex_destroy(&ch->lock);
      free(ch);
      return VOX_WS_T_ERR_HANDSHAKE;
    }

  vox_ws_on_connected(&ch->session);
  *out = ch;
  return VOX_WS_T_OK;
}

vox_state_e vox_ws_channel_advance(vox_ws_channel_t *ch)
{
  vox_state_e st;

  if (ch == NULL)
    {
      return VOX_STATE_IDLE;
    }

  pthread_mutex_lock(&ch->lock);
  st = vox_ws_advance(&ch->session);
  pthread_mutex_unlock(&ch->lock);

  if (st == VOX_STATE_STANDBY && ch->tp != NULL)
    {
      /* ai_vox drops the connection when a button press interrupts listening;
       * keeping it open would leave the peer sending into a session nobody is
       * driving. */
      vox_ws_transport_close(ch->tp);
      ch->tp = NULL;
    }

  return st;
}

vox_ws_t_e vox_ws_channel_send_audio(vox_ws_channel_t *ch, const uint8_t *pcm,
                                     size_t len)
{
  if (ch == NULL || ch->tp == NULL)
    {
      return VOX_WS_T_ERR_CLOSED;
    }

  return vox_ws_transport_send_binary(ch->tp, pcm, len);
}

void vox_ws_channel_playback_done(vox_ws_channel_t *ch)
{
  if (ch == NULL)
    {
      return;
    }

  pthread_mutex_lock(&ch->lock);
  vox_ws_on_playback_done(&ch->session);
  pthread_mutex_unlock(&ch->lock);

  emit(ch, VOX_ACT_PLAYBACK_DONE, NULL, NULL);
}

void vox_ws_channel_close(vox_ws_channel_t *ch)
{
  if (ch == NULL)
    {
      return;
    }

  if (ch->tp != NULL)
    {
      vox_ws_transport_close(ch->tp);
      ch->tp = NULL;
    }

  pthread_mutex_destroy(&ch->lock);
  free(ch);
}

const char *vox_ws_channel_state(vox_ws_channel_t *ch)
{
  if (ch == NULL)
    {
      return "none";
    }

  return vox_ws_state_name(ch->session.state);
}

void vox_ws_channel_stats(vox_ws_channel_t *ch, vox_ws_transport_stats_t *out)
{
  if (ch == NULL || ch->tp == NULL)
    {
      if (out != NULL)
        {
          memset(out, 0, sizeof(*out));
          out->first_msg_ms = -1;
        }

      return;
    }

  vox_ws_transport_get_stats(ch->tp, out);
}
