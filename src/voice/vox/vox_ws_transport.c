/*
 * vox_ws_transport.c -- TLS, handshake and reader thread for the ai_vox
 * protocol.  See vox_ws_transport.h for why this is the only file that knows
 * about sockets, threads and mbedTLS.
 */

#include "vox_ws_transport.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include "agent_compat.h"
#include "vox_ws_handshake.h"

#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#define TAG "[vox-ws]"

#define VOX_WS_HDR_MAX 2048
#define VOX_WS_READ_CHUNK 1024
#define VOX_WS_DEFAULT_STACK (16 * 1024)

struct vox_ws_transport
{
  vox_ws_transport_cfg_t cfg;

  /* TLS */
  mbedtls_ssl_context     ssl;
  mbedtls_ssl_config      ssl_cfg;
  mbedtls_net_context     net;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_x509_crt        crt;

  /* framing */
  vox_ws_decoder_t        dec;
  uint32_t                mask_seed;

  /* protocol, kept here so the reader thread can feed it */
  vox_ws_session_t        session;

  pthread_t               reader;
  bool                    reader_live;
  pthread_mutex_t         wlock;   /* serialises writes to the TLS session */
  pthread_mutex_t         slock;   /* guards stats and the closed flag */

  bool                     closing;
  bool                     closed_reported;
  bool                     key_valid;
  char                     key[VOX_WS_KEY_B64_LEN + 1];

  vox_ws_transport_stats_t stats;
  int64_t                  t0_ms;
};

const char *vox_ws_transport_name(vox_ws_t_e e)
{
  switch (e)
    {
      case VOX_WS_T_OK:             return "ok";
      case VOX_WS_T_ERR_URL:        return "bad_url";
      case VOX_WS_T_ERR_ENTROPY:    return "entropy_unavailable";
      case VOX_WS_T_ERR_CONNECT:    return "connect_failed";
      case VOX_WS_T_ERR_TLS:        return "tls_failed";
      case VOX_WS_T_ERR_HANDSHAKE:  return "handshake_failed";
      case VOX_WS_T_ERR_THREAD:     return "thread_failed";
      case VOX_WS_T_ERR_CLOSED:     return "closed";
      case VOX_WS_T_ERR_PROTO:      return "protocol_error";
      default:                      return "?";
    }
}

static int64_t now_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
      return 0;
    }

  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------- entropy */

static int vox_entropy(void *data, unsigned char *out, size_t len)
{
  (void)data;

  /* agent_secure_random() is the project's single entropy source
   * (include/agent_compat.h).  voice/volc_asr.c already uses it, so reusing it
   * keeps one audited path rather than two. */
  if (agent_secure_random(out, len) != 0)
    {
      syslog(LOG_ERR, "%s CRITICAL: no entropy for theWebSocket key\n", TAG);
      return -1;
    }

  return 0;
}

/* ------------------------------------------------------------------ I/O */

static int write_all(vox_ws_transport_t *t, const uint8_t *data, size_t len)
{
  size_t off = 0;

  while (off < len)
    {
      int r = mbedtls_ssl_write(&t->ssl, data + off, len - off);

      if (r > 0)
        {
          off += (size_t)r;
          continue;
        }

      if (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ)
        {
          continue;
        }

      return -1;
    }

  return 0;
}

/* ---------------------------------------------------------------- setup */

static void transport_free(vox_ws_transport_t *t)
{
  if (t == NULL)
    {
      return;
    }

  mbedtls_ssl_free(&t->ssl);
  mbedtls_ssl_config_free(&t->ssl_cfg);
  mbedtls_net_free(&t->net);
  mbedtls_ctr_drbg_free(&t->drbg);
  mbedtls_x509_crt_free(&t->crt);
  free(t);
}

static int transport_connect_tls(vox_ws_transport_t *t, const char *host,
                                 const char *port)
{
  int ret;
  int rc;
  const char *pers = "vox_ws";

  mbedtls_ssl_init(&t->ssl);
  mbedtls_ssl_config_init(&t->ssl_cfg);
  mbedtls_net_init(&t->net);
  mbedtls_ctr_drbg_init(&t->drbg);
  mbedtls_x509_crt_init(&t->crt);

  ret = mbedtls_ctr_drbg_seed(&t->drbg, vox_entropy, NULL,
                             (const unsigned char *)pers, strlen(pers));

  if (ret != 0)
    {
      syslog(LOG_ERR, "%s ctr_drbg_seed failed -0x%x\n", TAG, -ret);
      return VOX_WS_T_ERR_ENTROPY;
    }

  ret = mbedtls_net_connect(&t->net, host, port ? port : "443",
                            MBEDTLS_NET_PROTO_TCP);

  if (ret != 0)
    {
      syslog(LOG_ERR, "%s connect %s:%s failed -0x%x\n", TAG, host,
              port ? port : "443", -ret);
      return VOX_WS_T_ERR_CONNECT;
    }

  ret = mbedtls_ssl_config_defaults(&t->ssl_cfg, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);

  if (ret != 0)
    {
      return VOX_WS_T_ERR_TLS;
    }

  mbedtls_ssl_conf_rng(&t->ssl_cfg, mbedtls_ctr_drbg_random, &t->drbg);
  /* The project's own endpoints are reached by name; verification follows the
   * rest of this tree (vela_tls.c does the same) rather than being invented
   * here. */
  mbedtls_ssl_conf_authmode(&t->ssl_cfg, MBEDTLS_SSL_VERIFY_NONE);

  ret = mbedtls_ssl_setup(&t->ssl, &t->ssl_cfg);

  if (ret != 0)
    {
      return VOX_WS_T_ERR_TLS;
    }

  /* The hostname goes in as the original name, not a resolved address, so the
   * SNI extension and any certificate check both see the right thing.
   * vela_tls.c:79 records the same rule for the case where it connects to an
   * IP.  This build of mbedTLS declares ssl_set_hostname as void, so its
   * result cannot be checked. */
  mbedtls_ssl_set_hostname(&t->ssl, host);
  /* esp_mbedtls_ssl_set_bio() returns void in this build (see ssl.h:2236), the
   * same as vela_tls.c:531.  Its result cannot be checked, so the next failure
   * surface is the handshake below. */
  mbedtls_ssl_set_bio(&t->ssl, &t->net, mbedtls_net_send, mbedtls_net_recv,
                      NULL);

  while ((rc = mbedtls_ssl_handshake(&t->ssl)) != 0)
    {
      if (rc == MBEDTLS_ERR_SSL_WANT_READ ||
          rc == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
          continue;
        }

      syslog(LOG_ERR, "%s TLS handshake failed -0x%x\n", TAG, -rc);
      return VOX_WS_T_ERR_TLS;
    }

  return VOX_WS_T_OK;
}

static int transport_handshake(vox_ws_transport_t *t, const vox_ws_url_t *u)
{
  uint8_t     raw[VOX_WS_KEY_RAW_LEN];
  char        req[VOX_WS_HDR_MAX];
  char        resp[VOX_WS_HDR_MAX];
  size_t      req_len;
  size_t      resp_len = 0;
  const char *port     = u->port == 443 ? "443"
                        : u->port == 80  ? "80"
                                         : NULL;
  char        portbuf[8];
  int         e;
  vox_ws_hs_e hs;

  if (port == NULL)
    {
      snprintf(portbuf, sizeof(portbuf), "%u", (unsigned)u->port);
      port = portbuf;
    }

  e = transport_connect_tls(t, u->host, port);

  if (e != VOX_WS_T_OK)
    {
      return e;
    }

  if (agent_secure_random(raw, sizeof(raw)) != 0)
    {
      return VOX_WS_T_ERR_ENTROPY;
    }

  vox_ws_encode_key(raw, t->key, sizeof(t->key));
  t->key_valid = true;

  req_len = vox_ws_build_request(u, t->key, t->cfg.extra_headers, req,
                                 sizeof(req));

  if (req_len == 0)
    {
      syslog(LOG_ERR, "%s could not build the upgrade request\n", TAG);
      return VOX_WS_T_ERR_HANDSHAKE;
    }

  if (write_all(t, (const uint8_t *)req, req_len) != 0)
    {
      syslog(LOG_ERR, "%s could not send the upgrade request\n", TAG);
      return VOX_WS_T_ERR_HANDSHAKE;
    }

  /* Read only up to the end of the header block; anything after it belongs to
   * the frame decoder and must not be dropped. */
  while (resp_len < sizeof(resp) - 1)
    {
      int r = mbedtls_ssl_read(&t->ssl, (unsigned char *)resp + resp_len,
                               sizeof(resp) - 1 - resp_len);

      if (r > 0)
        {
          resp_len += (size_t)r;
          resp[resp_len] = '\0';

          if (strstr(resp, "\r\n\r\n") != NULL)
            {
              break;
            }

          continue;
        }

      if (r == MBEDTLS_ERR_SSL_WANT_READ ||
          r == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
          continue;
        }

      syslog(LOG_ERR, "%s no upgrade response (%d)\n", TAG, r);
      return VOX_WS_T_ERR_HANDSHAKE;
    }

  resp[resp_len] = '\0';
  hs = vox_ws_check_response(t->key, resp, resp_len);

  if (hs != VOX_WS_HS_OK)
    {
      syslog(LOG_ERR, "%s upgrade rejected: %s\n", TAG, vox_ws_hs_name(hs));
      return VOX_WS_T_ERR_HANDSHAKE;
    }

  /* Whatever followed CRLFCRLF is already WebSocket framing. */
  {
    char *body = strstr(resp, "\r\n\r\n");

    if (body != NULL)
      {
        size_t blen = resp_len - (size_t)(body + 4 - resp);

        if (blen > 0)
          {
            vox_ws_dec_feed(&t->dec, (const uint8_t *)(body + 4), blen);
          }
      }
  }

  syslog(LOG_INFO, "%s connected to %s:%u%s\n", TAG, u->host,
         (unsigned)u->port, u->path);
  return VOX_WS_T_OK;
}

/* ----------------------------------------------------------- reader loop */

static void report_closed(vox_ws_transport_t *t, vox_ws_t_e why)
{
  bool first = false;

  pthread_mutex_lock(&t->slock);

  if (!t->closed_reported)
    {
      t->closed_reported = true;
      first              = true;
    }

  pthread_mutex_unlock(&t->slock);

  if (first && t->cfg.on_closed != NULL)
    {
      t->cfg.on_closed(t->cfg.ctx, why);
    }
}

static void handle_message(vox_ws_transport_t *t, const vox_ws_msg_t *m)
{
  pthread_mutex_lock(&t->slock);
  t->stats.rx_frames++;

  if (t->stats.first_msg_ms < 0)
    {
      t->stats.first_msg_ms = now_ms() - t->t0_ms;
    }

  pthread_mutex_unlock(&t->slock);

  if (t->cfg.on_message != NULL)
    {
      t->cfg.on_message(t->cfg.ctx, m->opcode, m->data, m->len);
    }
}

static void handle_control(vox_ws_transport_t *t, const vox_ws_msg_t *m)
{
  /* Answering a ping is not optional in practice: a connection left idle while
   * "listening" is exactly the case some proxies drop, and the device then goes
   * quiet between turns with no error anywhere. */
  uint8_t frame[128 + 16];
  size_t  n;

  switch (m->opcode)
    {
      case VOX_WS_OP_PING:
        n = vox_ws_encode(frame, sizeof(frame), VOX_WS_OP_PONG, true, m->data,
                          m->len, t->mask_seed);
        t->mask_seed += 0x9E3779B9u;
        pthread_mutex_lock(&t->wlock);

        if (n > 0 && write_all(t, frame, n) == 0)
          {
            pthread_mutex_lock(&t->slock);
            t->stats.pongs_sent++;
            pthread_mutex_unlock(&t->slock);
          }

        pthread_mutex_unlock(&t->wlock);
        break;

      case VOX_WS_OP_CLOSE:
        syslog(LOG_INFO, "%s peer closed the session\n", TAG);
        pthread_mutex_lock(&t->slock);
        t->closing = true;
        pthread_mutex_unlock(&t->slock);
        break;

      default:
        break;
    }
}

static void *reader_main(void *arg)
{
  vox_ws_transport_t *t = (vox_ws_transport_t *)arg;
  uint8_t             chunk[VOX_WS_READ_CHUNK];
  bool                done = false;

  while (!done)
    {
      int r = mbedtls_ssl_read(&t->ssl, chunk, sizeof(chunk));

      if (r > 0)
        {
          pthread_mutex_lock(&t->slock);
          t->stats.rx_bytes += (uint32_t)r;
          pthread_mutex_unlock(&t->slock);
          vox_ws_dec_feed(&t->dec, chunk, (size_t)r);

          /* Drain every complete frame this read produced before reading
           * again: the protocol layer wants each message the moment it lands,
           * and that is what makes the first sentence playable while the model
           * is still generating the rest. */
          for (;;)
            {
              vox_ws_msg_t m;
              vox_ws_dec_e d = vox_ws_dec_next(&t->dec, &m);

              if (d == VOX_WS_DEC_NEED_MORE)
                {
                  break;
                }

              if (d == VOX_WS_DEC_CONTROL)
                {
                  handle_control(t, &m);
                  continue;
                }

              if (d == VOX_WS_DEC_MESSAGE)
                {
                  handle_message(t, &m);
                  continue;
                }

              pthread_mutex_lock(&t->slock);
              t->stats.decode_errors++;
              t->closing = true;
              pthread_mutex_unlock(&t->slock);
              syslog(LOG_ERR, "%s frame decode error, closing\n", TAG);
              done = true;
              break;
            }

          continue;
        }

      if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
          bool stop;

          pthread_mutex_lock(&t->slock);
          stop = t->closing;
          pthread_mutex_unlock(&t->slock);

          if (stop)
            {
              break;
            }

          continue;
        }

      /* Peer closed or an error: either way the session is over. */
      if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
        {
          syslog(LOG_INFO, "%s peer closed the connection\n", TAG);
        }
      else
        {
          syslog(LOG_ERR, "%s read failed %d\n", TAG, r);
        }

      done = true;
    }

  report_closed(t, VOX_WS_T_ERR_CLOSED);
  return NULL;
}

/* ------------------------------------------------------------------- API */

vox_ws_t_e vox_ws_transport_open(const char *url,
                                 const vox_ws_transport_cfg_t *cfg,
                                 vox_ws_transport_t **out)
{
  vox_ws_transport_t *t;
  vox_ws_url_t        u;
  vox_ws_t_e          e;
  pthread_attr_t      attr;
  uint8_t             mac[6];

  if (url == NULL || cfg == NULL || out == NULL)
    {
      return VOX_WS_T_ERR_URL;
    }

  *out = NULL;

  if (vox_ws_parse_url(url, &u) != VOX_WS_HS_OK)
    {
      syslog(LOG_ERR, "%s unusable url\n", TAG);
      return VOX_WS_T_ERR_URL;
    }

  t = (vox_ws_transport_t *)calloc(1, sizeof(*t));

  if (t == NULL)
    {
      return VOX_WS_T_ERR_CONNECT;
    }

  t->cfg          = *cfg;
  t->mask_seed    = 0x1D872B41u;
  t->t0_ms        = now_ms();
  t->stats.first_msg_ms = -1;
  vox_ws_dec_init(&t->dec);
  pthread_mutex_init(&t->wlock, NULL);
  pthread_mutex_init(&t->slock, NULL);

  /* The protocol session needs a device id; a fixed placeholder keeps the
   * transport free of the MAC lookup, which belongs to the caller that already
   * knows it. */
  memset(mac, 0, sizeof(mac));
  vox_ws_init(&t->session, NULL, mac);

  e = transport_handshake(t, &u);

  if (e != VOX_WS_T_OK)
    {
      transport_free(t);
      return e;
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, cfg->reader_stack ? cfg->reader_stack
                                                    : VOX_WS_DEFAULT_STACK);

  if (pthread_create(&t->reader, &attr, reader_main, t) != 0)
    {
      pthread_attr_destroy(&attr);
      transport_free(t);
      return VOX_WS_T_ERR_THREAD;
    }

  pthread_attr_destroy(&attr);
  t->reader_live = true;
  *out           = t;
  return VOX_WS_T_OK;
}

static vox_ws_t_e send_frame(vox_ws_transport_t *t, uint8_t opcode,
                             const uint8_t *data, size_t len)
{
  uint8_t frame[VOX_WS_MAX_MESSAGE + 32];
  size_t  n;
  int     rc = VOX_WS_T_OK;

  if (t == NULL)
    {
      return VOX_WS_T_ERR_CLOSED;
    }

  if (len > VOX_WS_MAX_MESSAGE)
    {
      return VOX_WS_T_ERR_PROTO;
    }

  n = vox_ws_encode(frame, sizeof(frame), opcode, true, data, len,
                    t->mask_seed);

  if (n == 0)
    {
      return VOX_WS_T_ERR_PROTO;
    }

  /* RFC 6455 5.3: the mask must differ between frames. */
  t->mask_seed += 0x9E3779B9u;

  pthread_mutex_lock(&t->wlock);

  if (write_all(t, frame, n) != 0)
    {
      rc = VOX_WS_T_ERR_CLOSED;
    }
  else
    {
      pthread_mutex_lock(&t->slock);
      t->stats.tx_frames++;
      t->stats.tx_bytes += (uint32_t)n;
      pthread_mutex_unlock(&t->slock);
    }

  pthread_mutex_unlock(&t->wlock);
  return rc;
}

vox_ws_t_e vox_ws_transport_send_text(vox_ws_transport_t *t, const char *text)
{
  if (text == NULL)
    {
      return VOX_WS_T_ERR_PROTO;
    }

  return send_frame(t, VOX_WS_OP_TEXT, (const uint8_t *)text, strlen(text));
}

vox_ws_t_e vox_ws_transport_send_binary(vox_ws_transport_t *t,
                                        const uint8_t *data, size_t len)
{
  return send_frame(t, VOX_WS_OP_BINARY, data, len);
}

void vox_ws_transport_close(vox_ws_transport_t *t)
{
  if (t == NULL)
    {
      return;
    }

  pthread_mutex_lock(&t->slock);
  t->closing = true;
  pthread_mutex_unlock(&t->slock);

  /* Ask the peer politely first; the reader notices WANT_READ with closing set
   * and exits without waiting for a close frame that may never come. */
  {
    uint8_t frame[8];
    size_t  n;

    n = vox_ws_encode(frame, sizeof(frame), VOX_WS_OP_CLOSE, true, NULL, 0,
                      t->mask_seed);

    if (n > 0)
      {
        pthread_mutex_lock(&t->wlock);
        (void)write_all(t, frame, n);
        pthread_mutex_unlock(&t->wlock);
      }
  }

  if (t->reader_live)
    {
      pthread_join(t->reader, NULL);
      t->reader_live = false;
    }

  pthread_mutex_destroy(&t->wlock);
  pthread_mutex_destroy(&t->slock);
  transport_free(t);
}

void vox_ws_transport_get_stats(vox_ws_transport_t *t,
                                vox_ws_transport_stats_t *out)
{
  if (t == NULL || out == NULL)
    {
      return;
    }

  pthread_mutex_lock(&t->slock);
  *out = t->stats;
  pthread_mutex_unlock(&t->slock);
}