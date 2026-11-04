/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * minimax_tts.c - MiniMax T2A v2 ("text to audio") TTS backend.
 *
 * Protocol source of truth (read 2026-10-12):
 *   https://platform.minimax.cn/docs/api-reference/speech-t2a-http    (中国站)
 *   https://platform.minimax.io/docs/api-reference/speech-t2a-http    (国际站)
 *   https://platform.minimax.cn/docs/guides/speech-t2a-websocket      (接入地址)
 *   https://platform.minimaxi.com/docs/faq/system-voice-id            (系统音色)
 *   https://platform.minimaxi.com/docs/api-reference/errorcode        (错误码)
 *
 * Verified on those pages:
 *   POST https://api.minimaxi.com/v1/t2a_v2      (华北: api-bj.minimaxi.com)
 *   Authorization: Bearer <api_key>              (HTTP bearer auth)
 *   Content-Type: application/json
 *   request:
 *     {"model":"speech-02-turbo","text":"...","stream":false,
 *      "output_format":"hex",
 *      "voice_setting":{"voice_id":"female-shaonv","speed":1,"vol":1,
 *                       "pitch":0},
 *      "audio_setting":{"sample_rate":24000,"bitrate":64000,"format":"mp3",
 *                       "channel":1}}
 *   response (HTTP 200, application/json):
 *     {"data":{"audio":"<hex encoded audio>","status":2},
 *      "extra_info":{"audio_length":11124,"audio_sample_rate":32000,
 *                    "audio_size":179926,"audio_format":"mp3",
 *                    "audio_channel":1,...},
 *      "trace_id":"...","base_resp":{"status_code":0,"status_msg":"success"}}
 *
 *   - data.audio is the audio *hex* encoded.  output_format accepts
 *     [url, hex] and defaults to hex; with stream:false the whole audio comes
 *     back in one body.  There is no base64 variant in the current API (the
 *     field description only mentions hex/url), but mm_decide_encoding()
 *     below also accepts base64 so a base64-era deployment cannot silently
 *     break us.
 *   - base_resp.status_code != 0 is the API level error signal, and it
 *     arrives with HTTP 200: an unauthenticated POST returns
 *     200 {"base_resp":{"status_code":1004,"status_msg":"login fail: Please
 *     carry the API secret key in the 'Authorization' field of the request
 *     header"}} (verified with curl from the build host, 2026-10-12).
 *     1004/2049 = auth, 1002/1039 = rate limit, 1001 = timeout,
 *     2013 = bad parameters, 1008 = insufficient balance.
 *   - Model enum: speech-2.8-hd/turbo, speech-2.6-hd/turbo, speech-02-hd/
 *     turbo, speech-01-hd/turbo.  Voice ids include female-shaonv
 *     (少女音色) in the system voice list.
 *
 * Design notes for this board (SF32LB52, SRAM ~89% full):
 *   - Output contract is identical to the Edge backend: 24 kHz/16-bit/mono
 *     PCM chunks through voice_tts_chunk_cb, decoded from the returned MP3 by
 *     the existing minimp3 wrapper (edge_mp3.c).  24 kHz is what the playback
 *     device is opened with (AGENT_TTS_WS_SAMPLE_RATE), so no resampling.
 *   - The response body is big: 180 KB of audio is 360 KB of hex.  It is
 *     therefore never buffered whole - the JSON is scanned as it streams in
 *     and the hex payload is decoded straight into edge_mp3_feed().  cJSON
 *     builds the request and parses the (small) envelope of an error body;
 *     the audio string itself never goes through cJSON.
 *   - Every wait is bounded: connect, first byte, between-body-chunks and a
 *     total deadline.  No retry loop, no assert; partial audio that already
 *     reached the caller counts as success so the fallback chain does not
 *     speak the same sentence twice.
 *   - Called from the speak worker thread only (voice_channel.c); no I/O
 *     happens in the HCI event pump.
 */

#include "voice/minimax_tts.h"

#include "agent_compat.h"
#include "agent_config.h"
#include "infra/config_store.h"
#include "infra/http_proxy.h"
#include "voice/edge_mp3.h"
#include "voice/voice_tts.h"

#include "cJSON.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

static const char* TAG = "minimax_tts";

/* ── Bounded I/O budget ──────────────────────────────────────────
 *
 * Mirrors the Edge backend's EDGE_RECV_TIMEOUT_*: a stalled link must never
 * hold speak_lock (the UI thread waits on it when the user presses PTT). */

#define MM_TTS_TIMEOUT_FIRST_MS 10000 /* connect -> response head -> 1st body */
#define MM_TTS_TIMEOUT_BODY_MS 3000 /* between body chunks */
#define MM_TTS_TIMEOUT_TOTAL_MS 20000 /* whole call, hard cap */
#define MM_MAX_CONSEC_TIMEOUTS 3

#define MM_READ_BUF_SIZE 2048 /* socket read buffer (heap) */
#define MM_HDR_BUF_SIZE 1024 /* HTTP response head (stack, bounded) */
#define MM_CHUNK_LINE_MAX 32 /* chunk-size line */
#define MM_SNIFF_CHARS 512 /* chars buffered to pick hex vs base64 */
#define MM_OUT_CHUNK 512 /* decoded MP3 staging before edge_mp3_feed() */
#define MM_PREFIX_MAX 1024 /* body prefix kept for the cJSON error path */

#define MM_KEY_MAX 512 /* MiniMax keys are JWTs (~300 chars) */
#define MM_VOICE_MAX 64
#define MM_MODEL_MAX 48
#define MM_HOST_MAX 96
#define MM_BACKEND_JSON_MAX 1024 /* raw llm_backend_0 config value */

/* ── TLS transport (same pattern as edge_tts.c's edge_tls_*) ───── */

typedef struct
{
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config cfg;
  mbedtls_net_context net;
  mbedtls_ctr_drbg_context ctr_drbg;
} mm_tls_t;

static int mm_entropy(void* data, unsigned char* output, size_t len)
{
  (void)data;

  if (agent_secure_random(output, len) == 0)
    {
      return 0;
    }

  syslog(LOG_ERR, "[%s] no secure entropy source\n", TAG);
  return -1;
}

static void mm_tls_free(mm_tls_t* ctx)
{
  mbedtls_ssl_close_notify(&ctx->ssl);
  mbedtls_net_free(&ctx->net);
  mbedtls_ssl_free(&ctx->ssl);
  mbedtls_ssl_config_free(&ctx->cfg);
  mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
}

static void mm_set_recv_timeout(mm_tls_t* ctx, int ms)
{
  struct timeval tv;

  if (ctx->net.fd < 0)
    {
      return;
    }

  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;

  if (setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
    {
      syslog(LOG_WARNING, "[%s] SO_RCVTIMEO(%d) failed: %d\n", TAG, ms, errno);
    }
}

static int mm_tls_connect(mm_tls_t* ctx, const char* host, const char* port,
                          int timeout_ms)
{
  int ret;

  mbedtls_ssl_init(&ctx->ssl);
  mbedtls_ssl_config_init(&ctx->cfg);
  mbedtls_net_init(&ctx->net);
  mbedtls_ctr_drbg_init(&ctx->ctr_drbg);

  ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, mm_entropy, NULL,
                              (const unsigned char*)"minimax_tts", 11);
  if (ret != 0)
    {
      goto fail;
    }

  if (http_proxy_is_enabled())
    {
      int tunnel_fd = proxy_open_tunnel(host, atoi(port), timeout_ms);

      if (tunnel_fd < 0)
        {
          syslog(LOG_ERR, "[%s] proxy tunnel to %s failed\n", TAG, host);
          ret = -ECONNREFUSED;
          goto fail;
        }

      ctx->net.fd = tunnel_fd;
    }
  else
    {
      ret = mbedtls_net_connect(&ctx->net, host, port, MBEDTLS_NET_PROTO_TCP);
      if (ret != 0)
        {
          syslog(LOG_ERR, "[%s] connect %s:%s failed (-0x%04x)\n", TAG, host,
                 port, -ret);
          ret = -ECONNREFUSED;
          goto fail;
        }
    }

  mbedtls_net_set_block(&ctx->net);

  if (ctx->net.fd >= 0)
    {
      int rcvbuf = 4096;
      struct timeval tv;

      /* Same BT-PAN throttling as the Edge backend: a large receive window
       * makes the LCPU/HCPU ring drop H4 fragments and stalls TCP. */
      tv.tv_sec = timeout_ms / 1000;
      tv.tv_usec = (timeout_ms % 1000) * 1000;
      setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

      if (setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf,
                     sizeof(rcvbuf)) < 0)
        {
          syslog(LOG_WARNING, "[%s] SO_RCVBUF=%d failed: %d\n", TAG, rcvbuf,
                 errno);
        }
    }

  ret = mbedtls_ssl_config_defaults(&ctx->cfg, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0)
    {
      ret = -EIO;
      goto fail;
    }

  mbedtls_ssl_conf_min_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_2);
#if defined(MBEDTLS_SSL_PROTO_TLS1_3)
  mbedtls_ssl_conf_max_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_3);
#else
  mbedtls_ssl_conf_max_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_2);
#endif

  /* No CA store on the board; the API key is the real gate. */
  mbedtls_ssl_conf_authmode(&ctx->cfg, MBEDTLS_SSL_VERIFY_OPTIONAL);
  mbedtls_ssl_conf_rng(&ctx->cfg, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);

  ret = mbedtls_ssl_setup(&ctx->ssl, &ctx->cfg);
  if (ret != 0)
    {
      ret = -EIO;
      goto fail;
    }

  mbedtls_ssl_set_hostname(&ctx->ssl, host);
  mbedtls_ssl_set_bio(&ctx->ssl, &ctx->net, mbedtls_net_send, mbedtls_net_recv,
                      NULL);

  syslog(LOG_INFO, "[%s] TLS handshake: %s:%s\n", TAG, host, port);

  while ((ret = mbedtls_ssl_handshake(&ctx->ssl)) != 0)
    {
      if (ret != MBEDTLS_ERR_SSL_WANT_READ
          && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
          syslog(LOG_ERR, "[%s] TLS handshake failed: -0x%04x\n", TAG, -ret);
          ret = -EIO;
          goto fail;
        }
    }

  syslog(LOG_INFO, "[%s] TLS connected\n", TAG);
  return 0;

fail:
  mm_tls_free(ctx);
  return ret;
}

static int mm_tls_write_all(mm_tls_t* ctx, const unsigned char* buf, size_t len)
{
  size_t written = 0;
  int guard = 0;

  while (written < len)
    {
      int ret = mbedtls_ssl_write(&ctx->ssl, buf + written, len - written);

      if (ret > 0)
        {
          written += (size_t)ret;
          guard = 0;
        }
      else if (ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
          if (++guard > 1000)
            {
              return -ETIMEDOUT;
            }
        }
      else
        {
          return -EIO;
        }
    }

  return 0;
}

/* One TLS read.  Returns >0 bytes, 0 on orderly close, negative errno. */
static int mm_tls_read(mm_tls_t* ctx, unsigned char* buf, size_t len)
{
  int ret = mbedtls_ssl_read(&ctx->ssl, buf, len);

  if (ret > 0)
    {
      return ret;
    }

  if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
    {
      return 0;
    }

  if (ret == -0x004C /* MBEDTLS_ERR_NET_RECV_FAILED */
      && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT))
    {
      return -ETIMEDOUT;
    }

  if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
    {
      return -EAGAIN;
    }

  return -EIO;
}

/* ── HTTP response reader ────────────────────────────────────────
 *
 * Splits the response into body bytes: content-length, chunked, or
 * read-until-close.  The raw byte source is a callback so the framing logic
 * can be exercised on a host (see the tests/ note in the commit message). */

typedef int (*mm_raw_fn)(void* ctx, unsigned char* buf, size_t cap);

#define MM_CH_SIZE 0
#define MM_CH_DATA 1
#define MM_CH_CRLF 2
#define MM_CH_DONE 3

typedef struct
{
  mm_raw_fn raw;
  void* raw_ctx;
  unsigned char buf[MM_READ_BUF_SIZE];
  size_t len;
  size_t pos;
  int chunked;
  int content_known;
  long content_remaining;
  int state; /* chunked sub-state */
  size_t chunk_left;
} mm_reader_t;

static int mm_reader_fill(mm_reader_t* rd)
{
  int n;

  if (rd->pos < rd->len)
    {
      return 1;
    }

  rd->pos = 0;
  rd->len = 0;

  n = rd->raw(rd->raw_ctx, rd->buf, sizeof(rd->buf));

  if (n <= 0)
    {
      return n;
    }

  rd->len = (size_t)n;
  return 1;
}

/* Next byte, or negative errno / -ECONNRESET at EOF. */
static int mm_reader_getc(mm_reader_t* rd)
{
  int f = mm_reader_fill(rd);

  if (f <= 0)
    {
      return f < 0 ? f : -ECONNRESET;
    }

  return rd->buf[rd->pos++];
}

/* Read up to want body bytes.  >0 bytes, 0 = end of body, <0 errno. */
static int mm_reader_read(mm_reader_t* rd, unsigned char* out, size_t want)
{
  size_t got = 0;

  while (got < want)
    {
      size_t avail;
      size_t n;
      int f;

      if (rd->chunked)
        {
          if (rd->state == MM_CH_DONE)
            {
              break;
            }

          if (rd->state == MM_CH_CRLF)
            {
              int i;

              for (i = 0; i < 2; i++)
                {
                  int c = mm_reader_getc(rd);

                  if (c < 0)
                    {
                      return got > 0 ? (int)got : c;
                    }
                }

              rd->state = MM_CH_SIZE;
              continue;
            }

          if (rd->state == MM_CH_SIZE)
            {
              char line[MM_CHUNK_LINE_MAX];
              size_t n2 = 0;
              char* semi;
              unsigned long sz;

              for (;;)
                {
                  int c = mm_reader_getc(rd);

                  if (c < 0)
                    {
                      return got > 0 ? (int)got : c;
                    }

                  if (c == '\n')
                    {
                      break;
                    }

                  if (c == '\r')
                    {
                      continue;
                    }

                  if (n2 < sizeof(line) - 1)
                    {
                      line[n2++] = (char)c;
                    }
                }

              line[n2] = '\0';
              semi = strchr(line, ';'); /* chunk extensions */
              if (semi)
                {
                  *semi = '\0';
                }

              sz = strtoul(line, NULL, 16);

              if (sz == 0)
                {
                  rd->state = MM_CH_DONE;
                  continue;
                }

              rd->chunk_left = (size_t)sz;
              rd->state = MM_CH_DATA;
            }

          /* MM_CH_DATA */
          if (rd->chunk_left == 0)
            {
              rd->state = MM_CH_CRLF;
              continue;
            }

          f = mm_reader_fill(rd);
          if (f <= 0)
            {
              if (got > 0)
                {
                  return (int)got;
                }

              return f < 0 ? f : -ECONNRESET;
            }

          avail = rd->len - rd->pos;
          n = avail;

          if (n > rd->chunk_left)
            {
              n = rd->chunk_left;
            }

          if (n > want - got)
            {
              n = want - got;
            }

          memcpy(out + got, rd->buf + rd->pos, n);
          rd->pos += n;
          rd->chunk_left -= n;
          got += n;
          continue;
        }

      /* content-length, or read until the peer closes */
      if (rd->content_known && rd->content_remaining <= 0)
        {
          break;
        }

      f = mm_reader_fill(rd);
      if (f <= 0)
        {
          if (got > 0)
            {
              return (int)got;
            }

          /* 0 = orderly EOF ends the body; a read error is reported. */
          return f < 0 ? f : 0;
        }

      avail = rd->len - rd->pos;
      n = avail;

      if (n > want - got)
        {
          n = want - got;
        }

      if (rd->content_known && (long)n > rd->content_remaining)
        {
          n = (size_t)rd->content_remaining;
        }

      memcpy(out + got, rd->buf + rd->pos, n);
      rd->pos += n;

      if (rd->content_known)
        {
          rd->content_remaining -= (long)n;
        }

      got += n;
    }

  return (int)got;
}

static int mm_ieq(const char* a, const char* b, size_t n)
{
  size_t i;

  for (i = 0; i < n; i++)
    {
      if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
        {
          return 0;
        }
    }

  return 1;
}

static int mm_icontains(const char* hay, const char* needle)
{
  size_t nlen = strlen(needle);
  const char* p;

  for (p = hay; *p != '\0'; p++)
    {
      if (mm_ieq(p, needle, nlen))
        {
          return 1;
        }
    }

  return 0;
}

/* Value of a header (points into hdr, NUL terminated at the line end is the
 * caller's problem - the callers only strtol/strstr it). */
static const char* mm_header(const char* hdr, const char* name)
{
  size_t nlen = strlen(name);
  const char* p = hdr;

  while ((p = strchr(p, '\n')) != NULL)
    {
      p++;

      if (mm_ieq(p, name, nlen) && p[nlen] == ':')
        {
          p += nlen + 1;

          while (*p == ' ' || *p == '\t')
            {
              p++;
            }

          return p;
        }
    }

  return NULL;
}

/* Read the status line + headers into a bounded buffer (the leftover body
 * bytes stay in rd->buf). */
static int mm_read_response_head(mm_reader_t* rd, int* status)
{
  char hdr[MM_HDR_BUF_SIZE];
  size_t n = 0;
  const char* v;
  char* sp;

  for (;;)
    {
      int f;

      if (n >= sizeof(hdr) - 1)
        {
          syslog(LOG_ERR, "[%s] response head too large\n", TAG);
          return -EOVERFLOW;
        }

      f = mm_reader_fill(rd);
      if (f <= 0)
        {
          return f < 0 ? f : -ECONNRESET;
        }

      while (rd->pos < rd->len && n < sizeof(hdr) - 1)
        {
          hdr[n++] = (char)rd->buf[rd->pos++];

          if (n >= 4 && memcmp(hdr + n - 4, "\r\n\r\n", 4) == 0)
            {
              goto done;
            }
        }
    }

done:
  hdr[n] = '\0';

  sp = strchr(hdr, ' ');
  if (sp == NULL || strncmp(hdr, "HTTP/1.", 7) != 0)
    {
      syslog(LOG_ERR, "[%s] malformed HTTP response\n", TAG);
      return -EPROTO;
    }

  *status = atoi(sp + 1);

  v = mm_header(hdr, "Transfer-Encoding");
  if (v != NULL && mm_icontains(v, "chunked"))
    {
      rd->chunked = 1;
      rd->state = MM_CH_SIZE;
    }

  v = mm_header(hdr, "Content-Length");
  if (v != NULL && !rd->chunked)
    {
      long clen = strtol(v, NULL, 10);

      if (clen >= 0)
        {
          rd->content_known = 1;
          rd->content_remaining = clen;
        }
    }

  syslog(LOG_INFO, "[%s] HTTP %d%s%s\n", TAG, *status,
         rd->chunked ? ", chunked" : "",
         rd->content_known ? ", content-length" : ", read-to-close");
  return 0;
}

/* ── JSON body scanner ───────────────────────────────────────────
 *
 * The body carries the audio as one enormous string, so it is scanned
 * incrementally: find "audio", decode its value (hex or base64) into MP3
 * bytes as they arrive, and keep an eye out for "status_code"/"status" on
 * the way.  Nothing bigger than MM_OUT_CHUNK + MM_SNIFF_CHARS is held. */

typedef void (*mm_mp3_cb)(const unsigned char* data, size_t len,
                          void* user_data);

#define MM_ENC_UNKNOWN 0
#define MM_ENC_HEX 1
#define MM_ENC_B64 2

#define MM_S_SCAN 0 /* outside any value we care about */
#define MM_S_COLON 1 /* matched "audio", expect ':' */
#define MM_S_QUOTE 2 /* expect the opening quote */
#define MM_S_VALUE 3 /* inside the audio string */
#define MM_S_SC_COLON 4
#define MM_S_SC_VAL 5
#define MM_S_ST_COLON 6
#define MM_S_ST_VAL 7

#define MM_PAT_AUDIO "\"audio\""
#define MM_PAT_STATUS_CODE "\"status_code\""
#define MM_PAT_STATUS "\"status\""

typedef struct
{
  int state;

  /* "pattern" matchers */
  size_t audio_pos;
  size_t sc_pos;
  size_t st_pos;

  /* value sniffing */
  char sniff[MM_SNIFF_CHARS];
  size_t sniff_len;
  int sniff_done;
  int enc;

  /* decoders */
  int nibble; /* hex high nibble, -1 = none */
  unsigned int b64_acc;
  int b64_n;
  int b64_done;
  int bad_chars;

  /* output staging */
  unsigned char out[MM_OUT_CHUNK];
  size_t out_len;

  /* number capture */
  long num;
  int num_seen;
  int num_sign;

  /* results */
  size_t audio_bytes;
  int found_audio;
  int status_code;
  int data_status;
  int is_url;
  int truncated;

  /* body prefix, for the cJSON error path (+1 for the NUL) */
  char prefix[MM_PREFIX_MAX + 1];
  size_t prefix_len;
  char status_msg[160];

  mm_mp3_cb on_mp3;
  void* user_data;
} mm_scan_t;

static void mm_match(size_t* pos, const char* pat, size_t n, int c,
                     int* complete)
{
  *complete = 0;

  if (c == (unsigned char)pat[*pos])
    {
      (*pos)++;

      if (*pos == n)
        {
          *pos = 0;
          *complete = 1;
        }
    }
  else
    {
      *pos = (c == (unsigned char)pat[0]) ? 1 : 0;
    }
}

static void mm_scan_init(mm_scan_t* sc, mm_mp3_cb on_mp3, void* user_data)
{
  memset(sc, 0, sizeof(*sc));
  sc->state = MM_S_SCAN;
  sc->nibble = -1;
  sc->num_sign = 1;
  sc->status_code = -1;
  sc->data_status = -1;
  sc->on_mp3 = on_mp3;
  sc->user_data = user_data;
}

static void mm_scan_flush(mm_scan_t* sc)
{
  if (sc->out_len > 0)
    {
      sc->audio_bytes += sc->out_len;
      sc->on_mp3(sc->out, sc->out_len, sc->user_data);
      sc->out_len = 0;
    }
}

static void mm_scan_emit(mm_scan_t* sc, unsigned char b)
{
  sc->out[sc->out_len++] = b;

  if (sc->out_len == sizeof(sc->out))
    {
      mm_scan_flush(sc);
    }
}

/* Pick the value encoding once, from the head of the string.  Hex is what
 * the API documents and returns; base64 is accepted as a fallback so an
 * older deployment cannot silently produce noise. */
static void mm_decide_encoding(mm_scan_t* sc)
{
  size_t i;

  sc->sniff_done = 1;
  sc->enc = MM_ENC_HEX;

  if (sc->sniff_len >= 4 && mm_ieq(sc->sniff, "http", 4))
    {
      /* output_format=url: we asked for hex, there is nothing to decode. */
      sc->is_url = 1;
      return;
    }

  for (i = 0; i < sc->sniff_len; i++)
    {
      if (!isxdigit((unsigned char)sc->sniff[i]))
        {
          sc->enc = MM_ENC_B64;
          break;
        }
    }
}

static void mm_scan_byte_value(mm_scan_t* sc, int c)
{
  if (sc->is_url)
    {
      return; /* output_format=url: the string is a link, not audio */
    }

  if (sc->enc == MM_ENC_HEX)
    {
      int v;

      if (c >= '0' && c <= '9')
        {
          v = c - '0';
        }
      else if (c >= 'a' && c <= 'f')
        {
          v = c - 'a' + 10;
        }
      else if (c >= 'A' && c <= 'F')
        {
          v = c - 'A' + 10;
        }
      else
        {
          if (++sc->bad_chars == 1)
            {
              syslog(LOG_WARNING,
                     "[%s] non-hex character in the audio string - wrong "
                     "encoding guess?\n",
                     TAG);
            }

          return;
        }

      if (sc->nibble < 0)
        {
          sc->nibble = v;
        }
      else
        {
          mm_scan_emit(sc, (unsigned char)((sc->nibble << 4) | v));
          sc->nibble = -1;
        }

      return;
    }

  /* base64 (standard or URL safe alphabet) */
  {
    int v;

    if (c >= 'A' && c <= 'Z')
      {
        v = c - 'A';
      }
    else if (c >= 'a' && c <= 'z')
      {
        v = c - 'a' + 26;
      }
    else if (c >= '0' && c <= '9')
      {
        v = c - '0' + 52;
      }
    else if (c == '+' || c == '-')
      {
        v = 62;
      }
    else if (c == '/' || c == '_')
      {
        v = 63;
      }
    else if (c == '=')
      {
        sc->b64_done = 1;
        return;
      }
    else if (isspace(c))
      {
        return;
      }
    else
      {
        if (++sc->bad_chars == 1)
          {
            syslog(LOG_WARNING,
                   "[%s] invalid base64 character 0x%02x in the audio "
                   "string\n",
                   TAG, c);
          }

        return;
      }

    if (sc->b64_done)
      {
        return;
      }

    sc->b64_acc = (sc->b64_acc << 6) | (unsigned int)v;

    if (++sc->b64_n == 4)
      {
        mm_scan_emit(sc, (unsigned char)((sc->b64_acc >> 16) & 0xff));
        mm_scan_emit(sc, (unsigned char)((sc->b64_acc >> 8) & 0xff));
        mm_scan_emit(sc, (unsigned char)(sc->b64_acc & 0xff));
        sc->b64_acc = 0;
        sc->b64_n = 0;
      }
  }
}

static void mm_scan_feed(mm_scan_t* sc, const unsigned char* data, size_t len)
{
  size_t i;

  for (i = 0; i < len; i++)
    {
      int c = data[i];

      if (sc->prefix_len < MM_PREFIX_MAX)
        {
          sc->prefix[sc->prefix_len++] = (char)c;
        }

      if (sc->state == MM_S_VALUE)
        {
          if (c == '"')
            {
              mm_scan_flush(sc);

              if (!sc->sniff_done)
                {
                  size_t k;

                  mm_decide_encoding(sc);

                  if (!sc->is_url)
                    {
                      for (k = 0; k < sc->sniff_len; k++)
                        {
                          mm_scan_byte_value(sc,
                                             (unsigned char)sc->sniff[k]);
                        }
                    }
                }

              sc->nibble = -1; /* a trailing half byte is dropped */
              sc->state = MM_S_SCAN;
              continue;
            }

          if (!sc->sniff_done)
            {
              sc->sniff[sc->sniff_len++] = (char)c;

              if (sc->sniff_len == MM_SNIFF_CHARS)
                {
                  size_t k;

                  mm_decide_encoding(sc);

                  for (k = 0; k < sc->sniff_len; k++)
                    {
                      mm_scan_byte_value(sc, (unsigned char)sc->sniff[k]);
                    }
                }

              continue;
            }

          mm_scan_byte_value(sc, c);
          continue;
        }

      if (sc->state == MM_S_COLON)
        {
          if (isspace(c))
            {
              continue;
            }

          if (c == ':')
            {
              sc->state = MM_S_QUOTE;
              continue;
            }

          sc->state = MM_S_SCAN; /* fall through and rescan */
        }
      else if (sc->state == MM_S_QUOTE)
        {
          if (isspace(c))
            {
              continue;
            }

          if (c == '"')
            {
              sc->state = MM_S_VALUE;
              sc->sniff_len = 0;
              sc->sniff_done = 0;
              sc->enc = MM_ENC_UNKNOWN;
              sc->found_audio = 1;
              continue;
            }

          sc->state = MM_S_SCAN;
        }
      else if (sc->state == MM_S_SC_COLON)
        {
          if (isspace(c))
            {
              continue;
            }

          if (c == ':')
            {
              sc->state = MM_S_SC_VAL;
              continue;
            }

          sc->state = MM_S_SCAN;
        }
      else if (sc->state == MM_S_SC_VAL || sc->state == MM_S_ST_VAL)
        {
          int is_sc = (sc->state == MM_S_SC_VAL);

          if (!sc->num_seen && c == '-')
            {
              sc->num_sign = -1;
              continue;
            }

          if (isdigit(c))
            {
              sc->num = sc->num * 10 + (c - '0');
              sc->num_seen = 1;
              continue;
            }

          if (sc->num_seen)
            {
              long v = sc->num_sign < 0 ? -sc->num : sc->num;

              if (is_sc)
                {
                  sc->status_code = (int)v;
                }
              else
                {
                  sc->data_status = (int)v;
                }
            }

          sc->num = 0;
          sc->num_seen = 0;
          sc->num_sign = 1;
          sc->state = MM_S_SCAN;
        }
      else if (sc->state == MM_S_ST_COLON)
        {
          if (isspace(c))
            {
              continue;
            }

          if (c == ':')
            {
              sc->state = MM_S_ST_VAL;
              continue;
            }

          sc->state = MM_S_SCAN;
        }

      /* MM_S_SCAN: look for the patterns of interest. */
      if (sc->state == MM_S_SCAN)
        {
          int done;

          mm_match(&sc->audio_pos, MM_PAT_AUDIO, sizeof(MM_PAT_AUDIO) - 1, c,
                   &done);
          if (done)
            {
              sc->state = MM_S_COLON;
              continue;
            }

          mm_match(&sc->sc_pos, MM_PAT_STATUS_CODE,
                   sizeof(MM_PAT_STATUS_CODE) - 1, c, &done);
          if (done)
            {
              sc->state = MM_S_SC_COLON;
              continue;
            }

          mm_match(&sc->st_pos, MM_PAT_STATUS, sizeof(MM_PAT_STATUS) - 1, c,
                   &done);
          if (done)
            {
              sc->state = MM_S_ST_COLON;
              continue;
            }
        }
    }

  if (sc->bad_chars > 8)
    {
      /* The encoding guess is wrong (or the payload is not audio at all);
       * give up instead of feeding noise to the decoder. */
      sc->truncated = 1;
    }
}

/* Small-envelope parse with cJSON: only run when no audio arrived, i.e. the
 * body is an error object small enough to fit in MM_PREFIX_MAX. */
static void mm_scan_parse_error(mm_scan_t* sc)
{
  cJSON* root;
  cJSON* br;
  cJSON* item;

  if (sc->prefix_len == 0 || sc->audio_bytes > 0)
    {
      return;
    }

  sc->prefix[sc->prefix_len] = '\0';
  root = cJSON_Parse(sc->prefix);

  if (root == NULL)
    {
      return; /* truncated prefix, not valid JSON */
    }

  br = cJSON_GetObjectItemCaseSensitive(root, "base_resp");

  if (cJSON_IsObject(br))
    {
      item = cJSON_GetObjectItemCaseSensitive(br, "status_code");

      if (cJSON_IsNumber(item))
        {
          sc->status_code = (int)item->valuedouble;
        }

      item = cJSON_GetObjectItemCaseSensitive(br, "status_msg");

      if (cJSON_IsString(item) && item->valuestring != NULL)
        {
          strncpy(sc->status_msg, item->valuestring,
                  sizeof(sc->status_msg) - 1);
          sc->status_msg[sizeof(sc->status_msg) - 1] = '\0';
        }
    }

  cJSON_Delete(root);
}

/* ── Request build (cJSON) ───────────────────────────────────── */

static char* mm_build_request(const char* text, const char* model,
                              const char* voice)
{
  cJSON* root = cJSON_CreateObject();
  cJSON* vs;
  cJSON* as;
  char* out;

  if (root == NULL)
    {
      return NULL;
    }

  cJSON_AddStringToObject(root, "model", model);
  cJSON_AddStringToObject(root, "text", text);
  cJSON_AddBoolToObject(root, "stream", 0);
  cJSON_AddStringToObject(root, "output_format", "hex");
  cJSON_AddStringToObject(root, "language_boost", "auto");

  vs = cJSON_AddObjectToObject(root, "voice_setting");

  if (vs != NULL)
    {
      cJSON_AddStringToObject(vs, "voice_id", voice);
      cJSON_AddNumberToObject(vs, "speed", 1);
      cJSON_AddNumberToObject(vs, "vol", 1);
      cJSON_AddNumberToObject(vs, "pitch", 0);
    }

  as = cJSON_AddObjectToObject(root, "audio_setting");

  if (as != NULL)
    {
      cJSON_AddNumberToObject(as, "sample_rate", AGENT_MINIMAX_TTS_SAMPLE_RATE);
      cJSON_AddNumberToObject(as, "bitrate", AGENT_MINIMAX_TTS_BITRATE);
      cJSON_AddStringToObject(as, "format", "mp3");
      cJSON_AddNumberToObject(as, "channel", 1);
    }

  out = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return out;
}

/* ── Config ──────────────────────────────────────────────────── */

/* Flat config key holding the LLM router's backend 0 blob, e.g.
 * {"host":"...","api_key":"sk-..."} (written by llm_router.c and
 * api_handler.c).  Only backend 0 is consulted here: it is the rung this
 * board has always managed to write. */

#define MM_CFG_KEY_LLM_BACKEND_0 "llm_backend_0"

typedef struct
{
  char key[MM_KEY_MAX];
  char voice[MM_VOICE_MAX];
  char model[MM_MODEL_MAX];
  char host[MM_HOST_MAX];
} mm_cfg_t;

static void mm_cfg_str(const char* cfg_key, char* dst, size_t cap,
                       const char* dflt)
{
  if (claw_config_get(cfg_key, dst, cap) != OK || dst[0] == '\0')
    {
      strncpy(dst, dflt, cap - 1);
      dst[cap - 1] = '\0';
    }
}

/* Last key rung: the copy stored inside the llm_backend_0 blob.  This
 * board's nsh console is line buffered at ~64 characters, so the ~60
 * character MiniMax key cannot be typed into the flat config - but the
 * llm_backend_0 value (itself a JSON string) was written over HTTP and
 * carries the very same key.  Returns the source name, or NULL when the
 * key is absent, empty, or the blob does not parse: never a hard failure. */

static const char* mm_key_from_backend(char* dst, size_t cap)
{
  char json[MM_BACKEND_JSON_MAX];
  cJSON* obj;
  cJSON* item;
  const char* source = NULL;

  if (claw_config_get(MM_CFG_KEY_LLM_BACKEND_0, json, sizeof(json)) != OK)
    {
      return NULL;
    }

  obj = cJSON_Parse(json);

  if (obj == NULL)
    {
      syslog(LOG_WARNING, "[%s] %s is not valid JSON, key not taken from it\n",
             TAG, MM_CFG_KEY_LLM_BACKEND_0);
      return NULL;
    }

  item = cJSON_GetObjectItem(obj, AGENT_CFG_KEY_API_KEY);

  if (cJSON_IsString(item) && item->valuestring[0] != '\0')
    {
      strncpy(dst, item->valuestring, cap - 1);
      dst[cap - 1] = '\0';
      source = MM_CFG_KEY_LLM_BACKEND_0;
    }

  cJSON_Delete(obj);
  return source;
}

/* Optional, git-ignored local header that carries the MiniMax key compiled
 * into this image (the fourth rung in mm_load_config() below).  It exists
 * only in a developer tree that generated it from .v3_work/.llm_key; a
 * clean checkout has no such header and the "" fallback below keeps the
 * config-only behaviour.  The key itself is never logged. */

#if defined(__has_include)
#  if __has_include("voice/minimax_tts_key_local.h")
#    include "voice/minimax_tts_key_local.h"
#  endif
#endif
#ifndef MINIMAX_TTS_KEY_LOCAL
#  define MINIMAX_TTS_KEY_LOCAL ""
#endif

static void mm_load_config(mm_cfg_t* cfg)
{
  const char* source = NULL;

  memset(cfg, 0, sizeof(*cfg));

  /* Key order: the TTS specific key first, then the MiniMax key the LLM
   * already uses (both are the same account key on this device), then the
   * copy inside llm_backend_0 - the only config rung that survives the ~64
   * character nsh line buffer - and finally the key compiled into the image
   * by the fourth rung below.  The first non-empty value wins. */
  if (claw_config_get(AGENT_CFG_KEY_TTS_API_KEY, cfg->key, sizeof(cfg->key))
        == OK
      && cfg->key[0] != '\0')
    {
      source = AGENT_CFG_KEY_TTS_API_KEY;
    }
  else if (claw_config_get(AGENT_CFG_KEY_API_KEY, cfg->key,
                           sizeof(cfg->key)) == OK
           && cfg->key[0] != '\0')
    {
      source = AGENT_CFG_KEY_API_KEY;
    }
  else
    {
      source = mm_key_from_backend(cfg->key, sizeof(cfg->key));

      /* Fourth and last rung: the key compiled into the image, used only
       * when all three config rungs above came back empty.  This board's
       * nsh console cannot carry the ~125 character key past its ~64
       * character line buffer, so for a local image the key arrives in
       * .rodata instead.  A clean checkout has an empty constant here and
       * falls through to the original "no API key" path below. */

      if (source == NULL && MINIMAX_TTS_KEY_LOCAL[0] != '\0')
        {
          strncpy(cfg->key, MINIMAX_TTS_KEY_LOCAL, sizeof(cfg->key) - 1);
          cfg->key[sizeof(cfg->key) - 1] = '\0';
          source = "build-time constant";
        }
    }

  if (source != NULL)
    {
      /* The key itself is never logged. */
      syslog(LOG_INFO, "[%s] key source: %s\n", TAG, source);
    }

  mm_cfg_str(AGENT_CFG_KEY_MINIMAX_VOICE, cfg->voice, sizeof(cfg->voice),
             AGENT_MINIMAX_TTS_DEFAULT_VOICE);
  mm_cfg_str(AGENT_CFG_KEY_MINIMAX_MODEL, cfg->model, sizeof(cfg->model),
             AGENT_MINIMAX_TTS_DEFAULT_MODEL);
  mm_cfg_str(AGENT_CFG_KEY_MINIMAX_HOST, cfg->host, sizeof(cfg->host),
             AGENT_MINIMAX_TTS_HOST);
}

static int mm_errno_of(int code)
{
  switch (code)
    {
      case 1004: /* authentication failed */
      case 2049: /* invalid API key */
        return -EACCES;

      case 1002: /* rate limit */
      case 1039: /* TPM rate limit */
      case 2045: /* rate growth limit */
        return -EAGAIN;

      case 1001: /* timeout */
        return -ETIMEDOUT;

      case 1008: /* insufficient balance */
        return -ENOSPC;

      case 2013: /* invalid parameters */
        return -EINVAL;

      default:
        return -EIO;
    }
}

/* ── Request send ────────────────────────────────────────────── */

static int mm_send_request(mm_tls_t* tls, const char* host, const char* key,
                           const char* body, size_t body_len)
{
  char hdr[512];
  int n;
  int ret;

  n = snprintf(hdr, sizeof(hdr),
               "POST " AGENT_MINIMAX_TTS_PATH " HTTP/1.1\r\n"
               "Host: %s\r\n"
               "Content-Type: application/json\r\n"
               "Accept: application/json\r\n"
               "Content-Length: %zu\r\n"
               "Connection: close\r\n"
               "User-Agent: " AGENT_MINIMAX_TTS_USER_AGENT "\r\n"
               "Authorization: Bearer ",
               host, body_len);

  if (n <= 0 || n >= (int)sizeof(hdr))
    {
      return -EOVERFLOW;
    }

  ret = mm_tls_write_all(tls, (const unsigned char*)hdr, (size_t)n);
  if (ret != 0)
    {
      return ret;
    }

  /* The key is written separately so MM_KEY_MAX cannot overflow the header
   * buffer above. */
  ret = mm_tls_write_all(tls, (const unsigned char*)key, strlen(key));
  if (ret != 0)
    {
      return ret;
    }

  ret = mm_tls_write_all(tls, (const unsigned char*)"\r\n\r\n", 4);
  if (ret != 0)
    {
      return ret;
    }

  return mm_tls_write_all(tls, (const unsigned char*)body, body_len);
}

/* ── Glue: TLS -> reader -> scanner -> minimp3 -> caller ─────── */

typedef struct
{
  voice_tts_chunk_cb cb;
  void* user_data;
  size_t pcm_bytes;
} mm_sink_t;

static void mm_mp3_pcm(const unsigned char* pcm, size_t len, void* user_data)
{
  mm_sink_t* sink = (mm_sink_t*)user_data;

  if (pcm == NULL || len == 0)
    {
      return;
    }

  sink->pcm_bytes += len;
  sink->cb(pcm, len, 0, sink->user_data);
}

static void mm_mp3_audio(const unsigned char* data, size_t len,
                         void* user_data)
{
  edge_mp3_feed((edge_mp3_t*)user_data, data, len);
}

static int mm_tls_raw(void* ctx, unsigned char* buf, size_t cap)
{
  return mm_tls_read((mm_tls_t*)ctx, buf, cap);
}

static long mm_elapsed_ms(const struct timespec* t0)
{
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);
  return (long)((now.tv_sec - t0->tv_sec) * 1000
                + (now.tv_nsec - t0->tv_nsec) / 1000000);
}

int minimax_tts_synthesize_stream(const char* text, voice_tts_chunk_cb cb,
                                  void* user_data)
{
  mm_cfg_t* cfg = NULL;
  mm_tls_t tls;
  mm_reader_t* rd = NULL;
  mm_scan_t* sc = NULL;
  edge_mp3_t* mp3 = NULL;
  mm_sink_t sink;
  unsigned char* body = NULL;
  unsigned char* buf = NULL;
  struct timespec t0;
  size_t text_len;
  int tls_open = 0;
  int status = 0;
  int timeouts = 0;
  int truncated = 0;
  int first_body = 1;
  int ret = -EIO;

  if (text == NULL || cb == NULL)
    {
      return -EINVAL;
    }

  text_len = strlen(text);

  if (text_len == 0)
    {
      return -EINVAL;
    }

  memset(&sink, 0, sizeof(sink));
  sink.cb = cb;
  sink.user_data = user_data;

  cfg = malloc(sizeof(*cfg));
  if (cfg == NULL)
    {
      return -ENOMEM;
    }

  mm_load_config(cfg);

  if (cfg->key[0] == '\0')
    {
      /* Nothing to send: a request without a key only wastes a round trip. */
      syslog(LOG_ERR,
             "[%s] no API key: set config 'tts_api_key' or 'api_key' "
             "(skipping, no request sent)\n",
             TAG);
      ret = -EACCES;
      goto out;
    }

  syslog(LOG_INFO, "[%s] T2A v2: model=%s voice=%s host=%s key=%zu chars\n",
         TAG, cfg->model, cfg->voice, cfg->host, strlen(cfg->key));

  body = (unsigned char*)mm_build_request(text, cfg->model, cfg->voice);

  if (body == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  mp3 = edge_mp3_open(mm_mp3_pcm, &sink);

  if (mp3 == NULL)
    {
      syslog(LOG_ERR, "[%s] no memory for the MP3 decoder\n", TAG);
      ret = -ENOMEM;
      goto out;
    }

  rd = malloc(sizeof(*rd));
  sc = malloc(sizeof(*sc));
  buf = malloc(MM_READ_BUF_SIZE);

  if (rd == NULL || sc == NULL || buf == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  memset(rd, 0, sizeof(*rd));
  mm_scan_init(sc, mm_mp3_audio, mp3);

  clock_gettime(CLOCK_MONOTONIC, &t0);

  ret = mm_tls_connect(&tls, cfg->host, AGENT_MINIMAX_TTS_PORT,
                       MM_TTS_TIMEOUT_FIRST_MS);
  if (ret != 0)
    {
      goto out;
    }

  tls_open = 1;

  ret = mm_send_request(&tls, cfg->host, cfg->key, (const char*)body,
                        strlen((const char*)body));
  if (ret != 0)
    {
      syslog(LOG_ERR, "[%s] request send failed: %d\n", TAG, ret);
      goto out;
    }

  rd->raw = mm_tls_raw;
  rd->raw_ctx = &tls;

  ret = mm_read_response_head(rd, &status);
  if (ret != 0)
    {
      syslog(LOG_ERR, "[%s] no response head: %d\n", TAG, ret);
      goto out;
    }

  if (status != 200)
    {
      syslog(LOG_ERR, "[%s] HTTP %d\n", TAG, status);
      ret = (status == 401 || status == 403) ? -EACCES : -EIO;
      goto out;
    }

  /* Body.  Bounded by the total deadline; a stalled read costs
   * MM_TTS_TIMEOUT_BODY_MS and is tolerated MM_MAX_CONSEC_TIMEOUTS times. */
  for (;;)
    {
      int n;

      if (mm_elapsed_ms(&t0) > MM_TTS_TIMEOUT_TOTAL_MS)
        {
          syslog(LOG_WARNING, "[%s] total deadline (%d ms) reached\n", TAG,
                 MM_TTS_TIMEOUT_TOTAL_MS);
          truncated = 1;
          break;
        }

      /* The first body bytes pay for server side synthesis, so they get the
       * same budget as the response head; afterwards MM_TTS_TIMEOUT_BODY_MS
       * is the stall detector. */
      mm_set_recv_timeout(&tls,
                          first_body ? MM_TTS_TIMEOUT_FIRST_MS
                                     : MM_TTS_TIMEOUT_BODY_MS);

      n = mm_reader_read(rd, buf, MM_READ_BUF_SIZE);

      if (n < 0)
        {
          if (n == -ETIMEDOUT || n == -EAGAIN)
            {
              if (++timeouts >= MM_MAX_CONSEC_TIMEOUTS)
                {
                  syslog(LOG_ERR, "[%s] %d consecutive read timeouts\n", TAG,
                         timeouts);
                  truncated = 1;
                  break;
                }

              continue;
            }

          if (sink.pcm_bytes > 0)
            {
              syslog(LOG_WARNING, "[%s] body read failed (%d) after %zu PCM "
                     "bytes, keeping partial audio\n",
                     TAG, n, sink.pcm_bytes);
              truncated = 1;
              break;
            }

          syslog(LOG_ERR, "[%s] body read failed: %d\n", TAG, n);
          ret = n;
          goto out;
        }

      if (n == 0)
        {
          break; /* end of body */
        }

      timeouts = 0;
      first_body = 0;
      mm_scan_feed(sc, buf, (size_t)n);

      if (sc->bad_chars > 8)
        {
          syslog(LOG_ERR,
                 "[%s] audio payload is neither hex nor base64 (%d bad "
                 "characters), giving up\n",
                 TAG, sc->bad_chars);
          truncated = 1;
          break;
        }
    }

  mm_scan_flush(sc);
  mm_scan_parse_error(sc);

  syslog(LOG_INFO,
         "[%s] body: %zu MP3 bytes -> %zu PCM bytes (%zu frames, %d Hz, "
         "%d ch, %s), api status_code=%d data.status=%d\n",
         TAG, edge_mp3_input_bytes(mp3), sink.pcm_bytes, edge_mp3_frames(mp3),
         edge_mp3_sample_rate(mp3), edge_mp3_channels(mp3),
         sc->enc == MM_ENC_HEX ? "hex"
                               : (sc->enc == MM_ENC_B64 ? "base64" : "none"),
         sc->status_code, sc->data_status);

  if (sc->is_url)
    {
      syslog(LOG_ERR,
             "[%s] service returned a URL instead of inline audio "
             "(output_format=hex was requested)\n",
             TAG);
      ret = -EPROTO;
      goto out;
    }

  if (sc->status_code > 0)
    {
      syslog(LOG_ERR, "[%s] API error %d: %s\n", TAG, sc->status_code,
             sc->status_msg[0] != '\0' ? sc->status_msg : "(no message)");

      if (sink.pcm_bytes > 0)
        {
          /* Audio already played: report success so the fallback chain does
           * not speak the sentence again. */
          truncated = 1;
        }
      else
        {
          ret = mm_errno_of(sc->status_code);
          goto out;
        }
    }

  if (sink.pcm_bytes == 0)
    {
      syslog(LOG_ERR, "[%s] no audio received (%zu MP3 bytes)\n", TAG,
             edge_mp3_input_bytes(mp3));
      ret = truncated ? -ETIMEDOUT : -EPROTO;
      goto out;
    }

  cb(NULL, 0, 1, user_data);

  syslog(LOG_INFO, "[%s] delivered %zu PCM bytes in %ld ms%s\n", TAG,
         sink.pcm_bytes, mm_elapsed_ms(&t0),
         truncated ? " (truncated, partial audio delivered)" : "");
  ret = 0;

out:
  if (mp3 != NULL)
    {
      edge_mp3_close(mp3);
    }

  if (tls_open)
    {
      mm_tls_free(&tls);
    }

  free(rd);
  free(sc);
  free(buf);
  free(body);
  free(cfg);
  return ret;
}

/* ── One-shot API (voice_tts_ops_t.synthesize) ────────────────── */

typedef struct
{
  unsigned char* buf;
  size_t cap;
  size_t len;
  int overflow;
} mm_accum_t;

static void mm_accum_cb(const unsigned char* pcm, size_t len, int is_last,
                        void* user_data)
{
  mm_accum_t* acc = (mm_accum_t*)user_data;

  (void)is_last;

  if (pcm == NULL || len == 0)
    {
      return;
    }

  if (acc->len + len > acc->cap)
    {
      acc->overflow = 1;
      return;
    }

  memcpy(acc->buf + acc->len, pcm, len);
  acc->len += len;
}

static int minimax_tts_synthesize(const char* text, unsigned char* pcm_out,
                                  size_t pcm_cap, size_t* pcm_len)
{
  mm_accum_t acc;
  int ret;

  if (text == NULL || pcm_out == NULL || pcm_len == NULL)
    {
      return -EINVAL;
    }

  *pcm_len = 0;

  memset(&acc, 0, sizeof(acc));
  acc.buf = pcm_out;
  acc.cap = pcm_cap;

  ret = minimax_tts_synthesize_stream(text, mm_accum_cb, &acc);

  if (acc.overflow)
    {
      syslog(LOG_WARNING, "[%s] output truncated at %zu bytes\n", TAG,
             pcm_cap);
    }

  if (acc.len == 0)
    {
      return ret != 0 ? ret : -ENOSPC;
    }

  *pcm_len = acc.len;
  return 0;
}

static int minimax_tts_init(void)
{
  mm_cfg_t cfg;

  mm_load_config(&cfg);

  syslog(LOG_INFO,
         "[%s] init: model=%s voice=%s host=%s key=%s (%zu chars, from "
         "'%s' / 'api_key')\n",
         TAG, cfg.model, cfg.voice, cfg.host,
         cfg.key[0] != '\0' ? "present" : "MISSING", strlen(cfg.key),
         AGENT_CFG_KEY_TTS_API_KEY);

  return 0;
}

static const voice_tts_ops_t s_minimax_tts_ops =
{
  .name = "minimax",
  .init = minimax_tts_init,
  .synthesize = minimax_tts_synthesize,
  .deinit = NULL,
  .synthesize_stream = minimax_tts_synthesize_stream,
  .sample_rate = AGENT_MINIMAX_TTS_SAMPLE_RATE,
};

int minimax_tts_register(void)
{
  return voice_tts_register(&s_minimax_tts_ops);
}

/****************************************************************************
 * 供 voice/minimax_asr.c 复用 (见 include/voice/minimax_tts.h 的说明)
 ****************************************************************************/

int minimax_get_api_key(char* out, size_t cap)
{
  mm_cfg_t cfg;

  if (out == NULL || cap == 0)
    {
      return -EINVAL;
    }

  out[0] = '\0';

  mm_load_config(&cfg);

  if (cfg.key[0] == '\0')
    {
      return -ENOENT;
    }

  strlcpy(out, cfg.key, cap);
  return 0;
}

int minimax_https_post(const char* path, const char* content_type,
                       const char* api_key,
                       const unsigned char* body, size_t body_len,
                       char* resp, size_t resp_cap, int timeout_ms)
{
  mm_tls_t tls;
  mm_reader_t rd;
  char hdr[512];
  int tls_open = 0;
  int status = 0;
  size_t got = 0;
  int n;
  int ret;

  if (path == NULL || content_type == NULL || api_key == NULL
      || body == NULL || body_len == 0 || resp == NULL || resp_cap < 2
      || timeout_ms <= 0)
    {
      return -EINVAL;
    }

  memset(&tls, 0, sizeof(tls));
  memset(&rd, 0, sizeof(rd));
  resp[0] = '\0';

  ret = mm_tls_connect(&tls, AGENT_MINIMAX_TTS_HOST, AGENT_MINIMAX_TTS_PORT,
                       timeout_ms);
  if (ret != 0)
    {
      syslog(LOG_ERR, "[minimax_https] connect failed: %d\n", ret);
      goto out;
    }

  tls_open = 1;

  n = snprintf(hdr, sizeof(hdr),
               "POST %s HTTP/1.1\r\n"
               "Host: %s\r\n"
               "Content-Type: %s\r\n"
               "Accept: application/json\r\n"
               "Content-Length: %zu\r\n"
               "Connection: close\r\n"
               "User-Agent: " AGENT_MINIMAX_TTS_USER_AGENT "\r\n"
               "Authorization: Bearer ",
               path, AGENT_MINIMAX_TTS_HOST, content_type, body_len);

  if (n <= 0 || n >= (int)sizeof(hdr))
    {
      ret = -EOVERFLOW;
      goto out;
    }

  /* key 单独写, 免得 ~125 字符的 key 撑爆上面的 header 缓冲 */
  ret = mm_tls_write_all(&tls, (const unsigned char*)hdr, (size_t)n);
  if (ret == 0)
    {
      ret = mm_tls_write_all(&tls, (const unsigned char*)api_key,
                             strlen(api_key));
    }

  if (ret == 0)
    {
      ret = mm_tls_write_all(&tls, (const unsigned char*)"\r\n\r\n", 4);
    }

  if (ret == 0)
    {
      /* 这里是"上行吞吐"最直接的测量点: 从 body 的第一个字节写到最后一个。
       * 蓝牙 PAN 的上行决定语音能不能实时上传(16kHz/16bit 原始 PCM 需要
       * 32KB/s), 所以留一条固定格式、可 grep 的日志:
       *   [minimax_https] upload: <bytes> bytes in <ms> ms = <KB/s> KB/s
       * 改动前后用同样的 payload 对比即可。 */
      struct timespec t0;
      struct timespec t1;

      clock_gettime(CLOCK_MONOTONIC, &t0);
      ret = mm_tls_write_all(&tls, body, body_len);
      clock_gettime(CLOCK_MONOTONIC, &t1);

      if (ret == 0)
        {
          double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0
                      + (double)(t1.tv_nsec - t0.tv_nsec) / 1000000.0;

          syslog(LOG_INFO,
                 "[minimax_https] upload: %u bytes in %.0f ms = %.1f KB/s\n",
                 (unsigned)body_len, ms,
                 ms > 0.0 ? ((double)body_len / 1024.0) / (ms / 1000.0)
                          : 0.0);
        }
    }

  if (ret != 0)
    {
      syslog(LOG_ERR, "[minimax_https] send failed: %d\n", ret);
      goto out;
    }

  rd.raw = mm_tls_raw;
  rd.raw_ctx = &tls;

  ret = mm_read_response_head(&rd, &status);
  if (ret != 0)
    {
      syslog(LOG_ERR, "[minimax_https] no response head: %d\n", ret);
      goto out;
    }

  if (status != 200)
    {
      syslog(LOG_ERR, "[minimax_https] HTTP %d\n", status);
      ret = (status == 401 || status == 403) ? -EACCES : -EIO;
      goto out;
    }

  /* JSON 响应很小, 一次读满即可; 读不满也不算错, 交给调用者解析 */
  while (got < resp_cap - 1)
    {
      n = mm_reader_read(&rd, (unsigned char*)resp + got,
                         resp_cap - 1 - got);
      if (n <= 0)
        {
          break;
        }

      got += (size_t)n;
    }

  resp[got] = '\0';
  ret = 0;

out:
  if (tls_open)
    {
      mm_tls_free(&tls);
    }

  return ret;
}
