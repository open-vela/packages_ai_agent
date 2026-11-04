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
 * edge_tts.c - Microsoft Edge "read aloud" online TTS backend (unofficial).
 *
 * Free and credential-less: the endpoint needs no account.  It is used as the
 * last link of the voice_tts fallback chain, after the Volcengine backends.
 *
 * Protocol source of truth (read 2026-10-11, edge-tts master):
 *   https://cdn.jsdelivr.net/gh/rany2/edge-tts@master/src/edge_tts/constants.py
 *   https://cdn.jsdelivr.net/gh/rany2/edge-tts@master/src/edge_tts/communicate.py
 *   https://cdn.jsdelivr.net/gh/rany2/edge-tts@master/src/edge_tts/drm.py
 *
 *   constants.py:
 *     BASE_URL  = "speech.platform.bing.com/consumer/speech/synthesize/readaloud"
 *     TRUSTED_CLIENT_TOKEN = "6A5AA1D4EAFF4E9FB37E23D68491D6F4"
 *     WSS_URL   = f"wss://{BASE_URL}/edge/v1?TrustedClientToken={TRUSTED_CLIENT_TOKEN}"
 *     SEC_MS_GEC_VERSION = f"1-{CHROMIUM_FULL_VERSION}"     # "1-143.0.3650.75"
 *     WSS_HEADERS = Pragma/Cache-Control no-cache,
 *                   Origin: chrome-extension://jdiccldimpdaibmpdkjnbmckianbfold,
 *                   Sec-WebSocket-Version: 13, Edge/Chrome User-Agent
 *
 *   communicate.py:
 *     session.ws_connect(f"{WSS_URL}&ConnectionId={connect_id()}"
 *                        f"&Sec-MS-GEC={DRM.generate_sec_ms_gec()}"
 *                        f"&Sec-MS-GEC-Version={SEC_MS_GEC_VERSION}", ...)
 *     speech.config text frame:
 *         "X-Timestamp:{date_to_string()}\r\n"
 *         "Content-Type:application/json; charset=utf-8\r\n"
 *         "Path:speech.config\r\n\r\n"
 *         '{"context":{"synthesis":{"audio":{"metadataoptions":{...},'
 *         '"outputFormat":"audio-24khz-48kbitrate-mono-mp3"}}}}\r\n'
 *     ssml text frame (ssml_headers_plus_data):
 *         "X-RequestId:{connect_id()}\r\n"
 *         "Content-Type:application/ssml+xml\r\n"
 *         "X-Timestamp:{date_to_string()}Z\r\n"   # "not a mistake, MS bug"
 *         "Path:ssml\r\n\r\n" + SSML
 *     binary frames: first 2 bytes = big-endian header length, then the header
 *         text (ends with CRLFCRLF), then the audio payload:
 *         header_length = int.from_bytes(received.data[:2], "big")
 *         parameters, data = get_headers_and_data(received.data, header_length)
 *     text frame "Path:turn.end" ends the turn.
 *
 *   drm.py:
 *     ticks  = unix_time + 11644473600          # Windows file time epoch
 *     ticks -= ticks % 300                      # 5-minute bucket
 *     ticks *= 10_000_000                       # 100 ns intervals
 *     Sec-MS-GEC = SHA256(f"{ticks:.0f}{TRUSTED_CLIENT_TOKEN}").hexdigest().upper()
 *     A 403 handshake is retried once after correcting the clock from the
 *     response "Date:" header (DRM.handle_client_response_error).
 *
 * Device specifics:
 *   - outputFormat raw-16khz-16bit-mono-pcm: headerless PCM, exactly the
 *     format /dev/audio/pcm0p wants, so no MP3 decoder is needed.
 *   - The board clock is local time (time_sync.c stores UTC+AGENT_UTC_OFFSET_SEC
 *     because CONFIG_LIBC_LOCALTIME is off), so edge_utc_now() subtracts the
 *     offset back out.  AGENT_UTC_OFFSET_SEC is documented in agent_config.h.
 */

#include "voice/edge_tts.h"

#include "agent_compat.h"
#include "agent_config.h"
#include "infra/config_store.h"
#include "infra/http_proxy.h"
#include "voice/edge_mp3.h"
#include "voice/voice_tts.h"

#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

static const char* TAG = "edge_tts";

/* ── Protocol constants ──────────────────────────────────────── */

#define EDGE_WS_BUF_SIZE (32 * 1024)

#define EDGE_WS_FIN 0x80
#define EDGE_WS_MASK_BIT 0x80
#define EDGE_WS_MASK_LEN 4
#define EDGE_WS_OP_CONT 0x00
#define EDGE_WS_OP_TEXT 0x01
#define EDGE_WS_OP_BIN 0x02
#define EDGE_WS_OP_CLOSE 0x08
#define EDGE_WS_OP_PING 0x09
#define EDGE_WS_OP_PONG 0x0A

#define EDGE_RECV_TIMEOUT_FIRST_MS 10000
#define EDGE_RECV_TIMEOUT_AUDIO_MS 2000
#define EDGE_MAX_CONSEC_TIMEOUTS 3

/* Windows file time epoch offset (1601-01-01 -> 1970-01-01), drm.py */
#define EDGE_WIN_EPOCH 11644473600LL

/* Output formats tried in order until one of them produces audio.
 *
 * The Edge read-aloud endpoint refuses PCM outright: on hardware it answered
 * the first rung with WS close 1007 "Unsupported Edge output format:
 * raw-24khz-16bit-mono-pcm." (and the same for riff-24khz).  Only MP3 works,
 * so that rung is decoded on the device with minimp3 (see edge_mp3.c) and the
 * decoded 24 kHz PCM goes to the normal playback path - no rate conversion,
 * because 24 kHz is what the Volcengine WS stream uses too.
 *
 * MP3 comes first. Measured on hardware (2026-10-11): this endpoint rejects
 * both PCM rungs with WS close 1007 'Unsupported Edge output format', and each
 * rejected rung costs a full TLS+WS round trip (~2 s on the BT-PAN link), so
 * trying PCM first made the first sentence take 24 s. The two PCM rungs are
 * kept behind MP3 so they are still probed if Microsoft ever re-enables raw
 * output -- by then the MP3 rung will have failed and the cached rung index
 * moves on. */
typedef struct
{
  const char* format;
  int is_mp3; /* 1 = decode with minimp3, 0 = raw PCM */
} edge_format_t;

static const edge_format_t EDGE_FORMATS[] =
{
  { AGENT_EDGE_TTS_FORMAT_MP3, 1 },      /* the one it does serve (measured) */
  { AGENT_EDGE_TTS_FORMAT, 0 },          /* raw-24khz PCM  (service: 1007) */
  { AGENT_EDGE_TTS_FORMAT_FALLBACK, 0 }, /* riff-24khz PCM (service: 1007) */
};

#define EDGE_NFORMATS ((int)(sizeof(EDGE_FORMATS) / sizeof(EDGE_FORMATS[0])))

/* Rung that produced audio last time; later segments start there. */
static int s_format_index;

/* P2d: the winning rung is persisted in the config store so a reboot does not
 * re-probe it.  The probe order is "cached rung first, then the rest of the
 * table" (the ladder below wraps around), so a stale cached rung can never
 * wedge synthesis the way a strictly-suffix ladder would. */
static int s_format_loaded;

static void edge_format_load(void)
{
  char v[8];
  int idx = 0;

  s_format_loaded = 1;

  if (claw_config_get(AGENT_CFG_KEY_EDGE_FMT, v, sizeof(v)) == OK
      && v[0] != '\0')
    {
      idx = atoi(v);

      if (idx < 0 || idx >= EDGE_NFORMATS)
        {
          syslog(LOG_WARNING,
                 "[%s] %s=%d out of range 0..%d, falling back to 0\n", TAG,
                 AGENT_CFG_KEY_EDGE_FMT, idx, EDGE_NFORMATS - 1);
          idx = 0;
        }
      else
        {
          syslog(LOG_INFO, "[%s] cached output format index %d (%s)\n", TAG,
                 idx, EDGE_FORMATS[idx].format);
        }
    }

  s_format_index = idx;
}

static void edge_format_save(int idx)
{
  char v[8];

  if (idx < 0 || idx >= EDGE_NFORMATS || !s_format_loaded)
    {
      return;
    }

  snprintf(v, sizeof(v), "%d", idx);

  if (claw_config_set(AGENT_CFG_KEY_EDGE_FMT, v) != OK)
    {
      syslog(LOG_WARNING, "[%s] cannot persist %s=%d\n", TAG,
             AGENT_CFG_KEY_EDGE_FMT, idx);
      return;
    }

  syslog(LOG_INFO, "[%s] output format index %d (%s) persisted as %s\n", TAG,
         idx, EDGE_FORMATS[idx].format, AGENT_CFG_KEY_EDGE_FMT);
}

/* Expected spoken duration of a text, used only as a sanity bound by the
 * truncation check (P2c).  Measured on this device: the zh-CN neural voices
 * run at ~3.5 Han characters per second; ASCII is counted at ~12 characters
 * per second (about 150 wpm) so an English sentence is not mistaken for a
 * truncated Chinese one.  Never a hard limit - a wrong estimate only costs one
 * extra synthesis attempt. */
#define EDGE_CJK_CHARS_PER_SEC 3.5
#define EDGE_ASCII_CHARS_PER_SEC 12.0

static double edge_expected_seconds(const char* text)
{
  const unsigned char* p = (const unsigned char*)text;
  size_t han = 0;
  size_t ascii = 0;

  for (; p && *p; p++)
    {
      if (*p < 0x80)
        {
          if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
            {
              ascii++;
            }
        }
      else if ((*p & 0xC0) != 0x80)
        {
          /* Lead byte of a multi-byte character (Han or full-width
           * punctuation); UTF-8 continuation bytes are not counted. */
          han++;
        }
    }

  return (double)han / EDGE_CJK_CHARS_PER_SEC
    + (double)ascii / EDGE_ASCII_CHARS_PER_SEC;
}

/* PCM handed to the caller -> seconds of speech (all rungs are 24 kHz mono
 * 16-bit, the MP3 one after minimp3 decoding). */
static double edge_pcm_seconds(size_t pcm_bytes)
{
  return (double)pcm_bytes / 2.0 / (double)AGENT_EDGE_TTS_SAMPLE_RATE;
}

/* P2c thresholds.  EDGE_MIN_DELIVERED_SECS is the "0.12s and done" case seen
 * on a flaky BT-PAN link; EDGE_SHORT_DELIVERY_RATIO catches a turn that broke
 * off later but still before half of the sentence. */
#define EDGE_MIN_DELIVERED_SECS 0.3

/* TCP 接收缓冲上限(字节): 见 edge_tls_connect() 里的说明 —— 压小突发,
 * 规避 LCPU 邮箱 ring 在大流量下的字节丢失。取 4KB: .config 里
 * CONFIG_IOB_NBUFFERS=64, 16KB 的接收窗口比整个 IOB 池还大, 会把 BT-PAN
 * 数据路径一起饿死, 反而加重它本想修的抖动。 */
#define EDGE_TCP_RCVBUF_BYTES 4096
/* 0.7: 真机实测链路抖动时交付 8.64s/14.3s = 60%, 用 0.5 不会重试 ✗。
 * 代价: 阈值放宽后每条被截断的长句会多花约一次合成的等待(十几秒),
 * 但换回完整语音; 短句(常见)基本不受影响。 */
#define EDGE_SHORT_DELIVERY_RATIO 0.7

/* speech.config payload.  sentenceBoundaryEnabled is "true" and
 * wordBoundaryEnabled is "false" - the reference defaults; the trailing CRLF
 * is in the reference too.  Metadata frames are ignored on receive. */
static int edge_build_speech_config(char* buf, size_t cap, const char* format)
{
  int n = snprintf(buf, cap,
    "{\"context\":{\"synthesis\":{\"audio\":{\"metadataoptions\":{"
    "\"sentenceBoundaryEnabled\":\"true\",\"wordBoundaryEnabled\":\"false\"},"
    "\"outputFormat\":\"%s\"}}}}\r\n",
    format);

  if (n <= 0 || (size_t)n >= cap)
    {
      return -EOVERFLOW;
    }

  return n;
}

/* ── Clock / DRM ─────────────────────────────────────────────── */

/* Skew between our estimate of UTC and the service clock, learned from the
 * HTTP "Date:" header.  Persists for the lifetime of the module. */
static long s_skew_secs;

/* Voice name (SSML <voice name=...>), from config store or the default. */
static char s_voice[64];

/* Wire dump of the Edge protocol.  Default on ("edge_debug" = "0" turns it
 * off); it is the only way to see what the service actually answered when a
 * turn ends without audio.  CR/LF are escaped so frame boundaries are visible. */
static int s_debug = 1;
static int s_debug_known;

static void edge_dump(const char* what, const unsigned char* data, size_t len)
{
  char buf[1200];
  size_t n = len < 280 ? len : 280;
  size_t o = 0;
  size_t i;

  if (!s_debug)
    {
      return;
    }

  if (!data)
    {
      syslog(LOG_INFO, "[%s] %s (%zu bytes): <null>\n", TAG, what, len);
      return;
    }

  for (i = 0; i < n && o + 6 < sizeof(buf); i++)
    {
      unsigned char c = data[i];

      if (c == '\r')
        {
          memcpy(buf + o, "\\r", 2);
          o += 2;
        }
      else if (c == '\n')
        {
          memcpy(buf + o, "\\n", 2);
          o += 2;
        }
      else if (c >= 0x20 && c < 0x7f)
        {
          buf[o++] = (char)c;
        }
      else
        {
          o += (size_t)snprintf(buf + o, sizeof(buf) - o, "\\x%02x", c);
        }
    }

  buf[o] = '\0';
  syslog(LOG_INFO, "[%s] %s (%zu bytes)%s: %s\n", TAG, what, len,
         len > n ? " [truncated]" : "", buf);
}

static time_t edge_local_now(void)
{
  return time(NULL);
}

/* Board time() is local (see AGENT_UTC_OFFSET_SEC note); Edge needs UTC. */
static time_t edge_utc_now(void)
{
  return edge_local_now() - (time_t)AGENT_UTC_OFFSET_SEC + (time_t)s_skew_secs;
}

static int64_t edge_days_from_civil(int y, unsigned m, unsigned d)
{
  int64_t era;
  unsigned yoe, doy, doe;

  y -= (m <= 2);
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = (unsigned)(y - (int)(era * 400));
  doy = (153u * (m + (m > 2 ? (unsigned)-3 : 9u)) + 2u) / 5u + d - 1u;
  doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097LL + (int64_t)doe - 719468LL;
}

/* Inverse of the above, plus time-of-day split.  week day 0 = Sunday. */
static void edge_civil_from_unix(time_t t, int* year, int* mon, int* day,
                                 int* hour, int* min, int* sec, int* wday)
{
  int64_t days = (int64_t)t / 86400;
  int64_t rem = (int64_t)t % 86400;
  int64_t z, era;
  unsigned doe, yoe, doy, mp, d, m;
  int64_t y, wd;

  if (rem < 0)
    {
      rem += 86400;
      days -= 1;
    }

  *hour = (int)(rem / 3600);
  *min = (int)((rem % 3600) / 60);
  *sec = (int)(rem % 60);

  z = days + 719468;
  era = (z >= 0 ? z : z - 146096) / 146097;
  doe = (unsigned)(z - era * 146097);
  yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
  y = (int64_t)yoe + era * 400;
  doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
  mp = (5u * doy + 2u) / 153u;
  d = doy - (153u * mp + 2u) / 5u + 1u;
  m = (mp < 10u ? mp + 3u : mp - 9u);

  *year = (int)(y + (m <= 2u));
  *mon = (int)m;
  *day = (int)d;

  wd = (days + 4) % 7; /* 1970-01-01 was a Thursday */
  if (wd < 0)
    {
      wd += 7;
    }

  *wday = (int)wd;
}

/* date_to_string(): "%a %b %d %Y %H:%M:%S GMT+0000 (Coordinated Universal Time)"
 * Built by hand: CONFIG_LIBC_LOCALTIME is off and %a/%b are locale dependent. */
static void edge_js_date(time_t t, char* out, size_t cap)
{
  static const char* const wd_names[7] =
  {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
  };

  static const char* const mon_names[12] =
  {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
  };

  int year, mon, day, hour, min, sec, wday;

  edge_civil_from_unix(t, &year, &mon, &day, &hour, &min, &sec, &wday);

  snprintf(out, cap, "%s %s %02d %04d %02d:%02d:%02d GMT+0000 "
           "(Coordinated Universal Time)",
           wd_names[wday], mon_names[mon - 1], day, year, hour, min, sec);
}

/* generate_sec_ms_gec().  The 5-minute bucket is computed in integers (the
 * reference does it in float, which is exact for these magnitudes), then the
 * single "ticks *= 10_000_000" multiply is left in IEEE double on purpose:
 * the tick count is above 2^53, so its rounding is part of the wire format. */
static void edge_sec_ms_gec(char out[65])
{
  int64_t secs;
  double ticks;
  char ticks_str[32];
  char hash_in[96];
  unsigned char digest[32];
  size_t i;

  secs = (int64_t)edge_utc_now() + EDGE_WIN_EPOCH;
  secs -= secs % 300; /* round down to the nearest 5 minutes */
  ticks = (double)secs * 10000000.0; /* seconds -> 100 ns intervals */

  /* ticks is an exact integer at this magnitude; %llu avoids float printf
   * (and therefore any libc float-formatting rounding differences). */
  snprintf(ticks_str, sizeof(ticks_str), "%llu", (unsigned long long)ticks);
  snprintf(hash_in, sizeof(hash_in), "%s%s", ticks_str,
           AGENT_EDGE_TTS_TRUSTED_TOKEN);

  mbedtls_sha256((const unsigned char*)hash_in, strlen(hash_in), digest, 0);

  for (i = 0; i < sizeof(digest); i++)
    {
      snprintf(out + i * 2, 3, "%02X", digest[i]);
    }

  out[64] = '\0';
}

static void edge_hex_lower(const unsigned char* in, size_t len, char* out)
{
  static const char hex[] = "0123456789abcdef";
  size_t i;

  for (i = 0; i < len; i++)
    {
      out[i * 2] = hex[(in[i] >> 4) & 0x0f];
      out[i * 2 + 1] = hex[in[i] & 0x0f];
    }

  out[len * 2] = '\0';
}

/* ── TLS plumbing ────────────────────────────────────────────────
 * Same shape as the transport inside volc_tts_ws.c.  It is duplicated here
 * on purpose: the Volcengine path is verified on hardware and must not be
 * touched by an Edge-only change. */

typedef struct
{
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config cfg;
  mbedtls_net_context net;
  mbedtls_ctr_drbg_context ctr_drbg;
} edge_tls_t;

static int edge_entropy(void* data, unsigned char* output, size_t len)
{
  (void)data;

  if (agent_secure_random(output, len) == 0)
    {
      return 0;
    }

  syslog(LOG_ERR, "[%s] CRITICAL: no secure entropy source\n", TAG);
  return -1;
}

static void edge_tls_free(edge_tls_t* ctx);

static int edge_tls_connect(edge_tls_t* ctx, const char* host,
                            const char* port)
{
  int ret;
  struct timeval tv;

  mbedtls_ssl_init(&ctx->ssl);
  mbedtls_ssl_config_init(&ctx->cfg);
  mbedtls_net_init(&ctx->net);
  mbedtls_ctr_drbg_init(&ctx->ctr_drbg);

  ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, edge_entropy, NULL,
                              (const unsigned char*)"edge_tts", 8);
  if (ret != 0)
    {
      goto fail;
    }

  if (http_proxy_is_enabled())
    {
      int tunnel_fd = proxy_open_tunnel(host, atoi(port), 30000);

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
      int rcvbuf = EDGE_TCP_RCVBUF_BYTES;

      tv.tv_sec = EDGE_RECV_TIMEOUT_FIRST_MS / 1000;
      tv.tv_usec = 0;
      setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

      /* 限流: 本板蓝牙走 LCPU 邮箱 ring, 大突发会触发 ring 读指针回写竞争
       * (见 vendor/.../sf32lb52_bth4.c:40-48), 实测表现为 `drop invalid h4`
       * → TCP 停摆 → 我们这边 recv 超时 → 音频被截断。把接收窗口压小后,
       * 数据以更小的突发到达, 给 LCPU/HCPU 之间的搬运留出余量。 */
      if (setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf,
                     sizeof(rcvbuf)) < 0)
        {
          syslog(LOG_WARNING, "[%s] SO_RCVBUF=%d failed: %d\n", TAG,
                 rcvbuf, errno);
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

  /* The board carries no CA store; the WS upgrade is the real gate. */
  mbedtls_ssl_conf_authmode(&ctx->cfg, MBEDTLS_SSL_VERIFY_OPTIONAL);
  mbedtls_ssl_conf_rng(&ctx->cfg, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);

  ret = mbedtls_ssl_setup(&ctx->ssl, &ctx->cfg);
  if (ret != 0)
    {
      ret = -EIO;
      goto fail;
    }

  mbedtls_ssl_set_hostname(&ctx->ssl, host);
  mbedtls_ssl_set_bio(&ctx->ssl, &ctx->net, mbedtls_net_send,
                      mbedtls_net_recv, NULL);

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
  edge_tls_free(ctx);
  return ret;
}

static void edge_tls_free(edge_tls_t* ctx)
{
  mbedtls_ssl_close_notify(&ctx->ssl);
  mbedtls_net_free(&ctx->net);
  mbedtls_ssl_free(&ctx->ssl);
  mbedtls_ssl_config_free(&ctx->cfg);
  mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
}

static int edge_tls_write_all(edge_tls_t* ctx, const unsigned char* buf,
                              size_t len)
{
  size_t written = 0;

  while (written < len)
    {
      int ret = mbedtls_ssl_write(&ctx->ssl, buf + written, len - written);

      if (ret > 0)
        {
          written += (size_t)ret;
        }
      else if (ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
          return -EIO;
        }
    }

  return 0;
}

static int edge_tls_read_all(edge_tls_t* ctx, unsigned char* buf, size_t len)
{
  size_t got = 0;

  while (got < len)
    {
      int ret = mbedtls_ssl_read(&ctx->ssl, buf + got, len - got);

      if (ret > 0)
        {
          got += (size_t)ret;
        }
      else if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
        {
          return -ECONNRESET;
        }
      else if (ret != MBEDTLS_ERR_SSL_WANT_READ)
        {
          /* SO_RCVTIMEO expiry surfaces as MBEDTLS_ERR_NET_RECV_FAILED. */
          if (ret == -0x004C
              && (errno == EAGAIN || errno == EWOULDBLOCK
                  || errno == ETIMEDOUT))
            {
              return -ETIMEDOUT;
            }

          return -EIO;
        }
    }

  return 0;
}

static void edge_set_recv_timeout(edge_tls_t* ctx, int ms)
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

/* ── Header helpers ──────────────────────────────────────────── */

/* Case-insensitive "Name:value" lookup over a raw header block.  Returns a
 * pointer to the value (leading spaces skipped) or NULL. */
static const char* edge_header_value(const char* hdrs, size_t hlen,
                                     const char* name)
{
  size_t nlen = strlen(name);
  size_t i = 0;

  while (i + nlen + 1 <= hlen)
    {
      size_t j;

      /* Match at the start of a line only. */
      if (i != 0 && hdrs[i - 1] != '\n')
        {
          i++;
          continue;
        }

      for (j = 0; j < nlen; j++)
        {
          char a = hdrs[i + j];
          char b = name[j];

          if (a >= 'A' && a <= 'Z')
            {
              a = (char)(a - 'A' + 'a');
            }

          if (b >= 'A' && b <= 'Z')
            {
              b = (char)(b - 'A' + 'a');
            }

          if (a != b)
            {
              break;
            }
        }

      if (j == nlen && hdrs[i + nlen] == ':')
        {
          size_t v = i + nlen + 1;

          while (v < hlen && (hdrs[v] == ' ' || hdrs[v] == '\t'))
            {
              v++;
            }

          return hdrs + v;
        }

      i++;
    }

  return NULL;
}

/* True when the header block carries "Name: expected" (case-insensitive,
 * value may have trailing CR/LF). */
static int edge_header_is(const char* hdrs, size_t hlen, const char* name,
                          const char* expected)
{
  const char* v = edge_header_value(hdrs, hlen, name);
  size_t elen = strlen(expected);

  if (!v)
    {
      return 0;
    }

  if ((size_t)((hdrs + hlen) - v) < elen)
    {
      return 0;
    }

  return strncmp(v, expected, elen) == 0;
}

/* Parse an RFC1123 "Date:" value into a Unix timestamp (UTC). */
static time_t edge_parse_http_date(const char* date)
{
  static const char* const mon_names[12] =
  {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
  };

  char mon[4];
  int day = 0, year = 0, hh = 0, mm = 0, ss = 0, i;
  int64_t days;

  if (sscanf(date, "%*3s, %d %3s %d %d:%d:%d", &day, mon, &year, &hh, &mm,
             &ss) != 6)
    {
      return (time_t)-1;
    }

  for (i = 0; i < 12; i++)
    {
      if (strncmp(mon, mon_names[i], 3) == 0)
        {
          break;
        }
    }

  if (i == 12 || year < 1970 || day < 1 || day > 31)
    {
      return (time_t)-1;
    }

  days = edge_days_from_civil(year, (unsigned)(i + 1), (unsigned)day);
  return (time_t)(days * 86400 + hh * 3600 + mm * 60 + ss);
}

/* DRM.handle_client_response_error(): learn the clock skew from the server. */
static void edge_note_server_date(const char* resp, size_t rlen)
{
  const char* v = edge_header_value(resp, rlen, "Date");
  time_t server, local;

  if (!v)
    {
      return;
    }

  server = edge_parse_http_date(v);
  if (server == (time_t)-1)
    {
      syslog(LOG_WARNING, "[%s] unparsable server date\n", TAG);
      return;
    }

  local = edge_local_now() - (time_t)AGENT_UTC_OFFSET_SEC;
  s_skew_secs = (long)(server - local);
  syslog(LOG_INFO, "[%s] clock skew adjusted to %lds (server %ld)\n", TAG,
         s_skew_secs, (long)server);
}

/* ── WebSocket framing ───────────────────────────────────────── */

static int edge_ws_send_frame(edge_tls_t* ctx, unsigned char opcode,
                              const unsigned char* payload, size_t plen)
{
  unsigned char hdr[14];
  unsigned char mask[EDGE_WS_MASK_LEN];
  unsigned char chunk[1024];
  size_t hdr_len = 0;
  size_t sent = 0;
  int ret;

  hdr[0] = (unsigned char)(EDGE_WS_FIN | opcode);

  if (plen < 126)
    {
      hdr[1] = (unsigned char)(EDGE_WS_MASK_BIT | plen);
      hdr_len = 2;
    }
  else if (plen <= 0xFFFF)
    {
      hdr[1] = (unsigned char)(EDGE_WS_MASK_BIT | 126);
      hdr[2] = (unsigned char)(plen >> 8);
      hdr[3] = (unsigned char)(plen & 0xFF);
      hdr_len = 4;
    }
  else
    {
      hdr[1] = (unsigned char)(EDGE_WS_MASK_BIT | 127);
      memset(hdr + 2, 0, 4);
      hdr[6] = (unsigned char)((plen >> 24) & 0xFF);
      hdr[7] = (unsigned char)((plen >> 16) & 0xFF);
      hdr[8] = (unsigned char)((plen >> 8) & 0xFF);
      hdr[9] = (unsigned char)(plen & 0xFF);
      hdr_len = 10;
    }

  if (agent_secure_random(mask, sizeof(mask)) != 0)
    {
      return -EIO;
    }

  memcpy(hdr + hdr_len, mask, sizeof(mask));
  hdr_len += sizeof(mask);

  ret = edge_tls_write_all(ctx, hdr, hdr_len);
  if (ret != 0)
    {
      return ret;
    }

  while (sent < plen)
    {
      size_t clen = plen - sent;
      size_t i;

      if (clen > sizeof(chunk))
        {
          clen = sizeof(chunk);
        }

      for (i = 0; i < clen; i++)
        {
          chunk[i] = payload[sent + i] ^ mask[(sent + i) % 4];
        }

      ret = edge_tls_write_all(ctx, chunk, clen);
      if (ret != 0)
        {
          return ret;
        }

      sent += clen;
    }

  return 0;
}

/* Reads one frame.  Oversized frames are drained and reported as dropped so
 * that the connection stays usable (the caller sees *out_len == 0). */
static int edge_ws_recv_frame(edge_tls_t* ctx, unsigned char* buf, size_t cap,
                              size_t* out_len, int* out_opcode,
                              int* out_dropped)
{
  unsigned char hdr[2];
  unsigned char ext[8];
  unsigned char mask[EDGE_WS_MASK_LEN];
  unsigned char scratch[512];
  size_t plen;
  int masked;
  int ret;

  *out_len = 0;
  *out_dropped = 0;

  ret = edge_tls_read_all(ctx, hdr, 2);
  if (ret != 0)
    {
      return ret;
    }

  *out_opcode = hdr[0] & 0x0F;
  masked = (hdr[1] & EDGE_WS_MASK_BIT) != 0;
  plen = (size_t)(hdr[1] & 0x7F);

  if (plen == 126)
    {
      ret = edge_tls_read_all(ctx, ext, 2);
      if (ret != 0)
        {
          return ret;
        }

      plen = ((size_t)ext[0] << 8) | (size_t)ext[1];
    }
  else if (plen == 127)
    {
      ret = edge_tls_read_all(ctx, ext, 8);
      if (ret != 0)
        {
          return ret;
        }

      plen = ((size_t)ext[4] << 24) | ((size_t)ext[5] << 16)
             | ((size_t)ext[6] << 8) | (size_t)ext[7];
    }

  if (masked)
    {
      ret = edge_tls_read_all(ctx, mask, sizeof(mask));
      if (ret != 0)
        {
          return ret;
        }
    }

  if (plen > cap)
    {
      size_t left = plen;

      syslog(LOG_WARNING, "[%s] frame too large (%zu > %zu), dropping\n", TAG,
             plen, cap);

      while (left > 0)
        {
          size_t n = left > sizeof(scratch) ? sizeof(scratch) : left;

          ret = edge_tls_read_all(ctx, scratch, n);
          if (ret != 0)
            {
              return ret;
            }

          left -= n;
        }

      *out_dropped = 1;
      return 0;
    }

  if (plen > 0)
    {
      ret = edge_tls_read_all(ctx, buf, plen);
      if (ret != 0)
        {
          return ret;
        }

      if (masked)
        {
          size_t i;

          for (i = 0; i < plen; i++)
            {
              buf[i] ^= mask[i % 4];
            }
        }
    }

  *out_len = plen;
  return 0;
}

/* ── HTTP upgrade ────────────────────────────────────────────── */

/* Returns 0 on HTTP 101, -EACCES when the service answered 403 (DRM clock
 * skew), -EPROTO for any other status.  *out_status carries the code. */
static int edge_ws_upgrade(edge_tls_t* ctx, const char* host, const char* path,
                           int* out_status)
{
  unsigned char key_raw[16];
  char key_b64[32];
  size_t key_b64_len = 0;
  unsigned char muid_raw[16];
  char muid[33];
  char req[1024];
  char resp[1024];
  size_t rlen = 0;
  int status = 0;
  int n;
  int ret;

  *out_status = 0;

  if (agent_secure_random(key_raw, sizeof(key_raw)) != 0
      || agent_secure_random(muid_raw, sizeof(muid_raw)) != 0)
    {
      return -EIO;
    }

  mbedtls_base64_encode((unsigned char*)key_b64, sizeof(key_b64), &key_b64_len,
                        key_raw, sizeof(key_raw));

  /* DRM.headers_with_muid(): Cookie: muid=<32 uppercase hex>; */
  edge_hex_lower(muid_raw, sizeof(muid_raw), muid);
  {
    size_t i;

    for (i = 0; i < 32; i++)
      {
        if (muid[i] >= 'a' && muid[i] <= 'f')
          {
            muid[i] = (char)(muid[i] - 'a' + 'A');
          }
      }
  }

  n = snprintf(req, sizeof(req),
               "GET %s HTTP/1.1\r\n"
               "Host: %s\r\n"
               "Upgrade: websocket\r\n"
               "Connection: Upgrade\r\n"
               "Sec-WebSocket-Key: %.*s\r\n"
               "Sec-WebSocket-Version: 13\r\n"
               "Origin: %s\r\n"
               "Pragma: no-cache\r\n"
               "Cache-Control: no-cache\r\n"
               "User-Agent: %s\r\n"
               "Cookie: muid=%s;\r\n"
               "\r\n",
               path, host, (int)key_b64_len, key_b64, AGENT_EDGE_TTS_ORIGIN,
               AGENT_EDGE_TTS_USER_AGENT, muid);

  if (n <= 0 || n >= (int)sizeof(req))
    {
      return -EOVERFLOW;
    }

  ret = edge_tls_write_all(ctx, (const unsigned char*)req, (size_t)n);
  if (ret != 0)
    {
      return ret;
    }

  while (rlen < sizeof(resp) - 1)
    {
      int r = mbedtls_ssl_read(&ctx->ssl, (unsigned char*)resp + rlen,
                               sizeof(resp) - 1 - rlen);

      if (r > 0)
        {
          rlen += (size_t)r;
          resp[rlen] = '\0';

          if (strstr(resp, "\r\n\r\n"))
            {
              break;
            }
        }
      else if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
        {
          return -ECONNRESET;
        }
      else if (r != MBEDTLS_ERR_SSL_WANT_READ)
        {
          if (r == -0x004C
              && (errno == EAGAIN || errno == EWOULDBLOCK
                  || errno == ETIMEDOUT))
            {
              return -ETIMEDOUT;
            }

          return -EIO;
        }
    }

  if (sscanf(resp, "HTTP/1.1 %d", &status) != 1)
    {
      syslog(LOG_ERR, "[%s] malformed upgrade response\n", TAG);
      return -EPROTO;
    }

  *out_status = status;

  /* Keep the DRM skew fresh from every response (including the 101). */
  edge_note_server_date(resp, rlen);

  if (status != 101)
    {
      syslog(LOG_ERR, "[%s] WS upgrade failed: HTTP %d\n", TAG, status);

      if (status == 403)
        {
          return -EACCES;
        }

      return -EPROTO;
    }

  syslog(LOG_INFO, "[%s] WebSocket upgrade OK\n", TAG);
  return 0;
}

/* ── Text preparation ────────────────────────────────────────── */

/* remove_incompatible_characters(): control characters the service rejects. */
static void edge_clean_text(char* s)
{
  for (; *s != '\0'; s++)
    {
      unsigned char c = (unsigned char)*s;

      if (c <= 8 || c == 11 || c == 12 || (c >= 14 && c <= 31))
        {
          *s = ' ';
        }
    }
}

/* xml.sax.saxutils.escape() for the characters that matter in SSML text. */
static char* edge_xml_escape(const char* text)
{
  size_t len = strlen(text);
  char* out = malloc(len * 6 + 1);
  size_t o = 0;
  size_t i;

  if (!out)
    {
      return NULL;
    }

  for (i = 0; i < len; i++)
    {
      switch (text[i])
        {
          case '&':
            memcpy(out + o, "&amp;", 5);
            o += 5;
            break;

          case '<':
            memcpy(out + o, "&lt;", 4);
            o += 4;
            break;

          case '>':
            memcpy(out + o, "&gt;", 4);
            o += 4;
            break;

          default:
            out[o++] = text[i];
            break;
        }
    }

  out[o] = '\0';
  return out;
}

/* split_text_by_byte_length(): at most AGENT_EDGE_MAX_TEXT_SEGMENT bytes and
 * never inside a UTF-8 sequence.  Splitting happens before escaping so an
 * entity can never be cut in half. */
static size_t edge_segment_len(const char* text, size_t len)
{
  size_t cut;

  if (len <= AGENT_EDGE_MAX_TEXT_SEGMENT)
    {
      return len;
    }

  cut = AGENT_EDGE_MAX_TEXT_SEGMENT;

  /* Prefer a newline / space boundary, like the reference implementation. */
  while (cut > 0 && text[cut] != '\n' && text[cut] != ' ')
    {
      cut--;
    }

  if (cut == 0)
    {
      cut = AGENT_EDGE_MAX_TEXT_SEGMENT;
    }
  else
    {
      cut++;
    }

  /* Never split a multi-byte UTF-8 character. */
  while (cut > 0 && ((unsigned char)text[cut] & 0xC0) == 0x80)
    {
      cut--;
    }

  if (cut == 0)
    {
      cut = AGENT_EDGE_MAX_TEXT_SEGMENT;
    }

  return cut;
}

/* ── Request payloads ────────────────────────────────────────── */

static int edge_send_speech_config(edge_tls_t* ctx, const char* format)
{
  char date[64];
  char json[256];
  size_t cap = sizeof(date) + sizeof(json) + 128;
  char* msg = malloc(cap);
  int jlen;
  int n;
  int ret;

  if (!msg)
    {
      return -ENOMEM;
    }

  jlen = edge_build_speech_config(json, sizeof(json), format);
  if (jlen < 0)
    {
      free(msg);
      return jlen;
    }

  edge_js_date(edge_utc_now(), date, sizeof(date));

  n = snprintf(msg, cap,
               "X-Timestamp:%s\r\n"
               "Content-Type:application/json; charset=utf-8\r\n"
               "Path:speech.config\r\n\r\n"
               "%s",
               date, json);

  if (n <= 0 || (size_t)n >= cap)
    {
      free(msg);
      return -EOVERFLOW;
    }

  edge_dump(">> speech.config", (const unsigned char*)msg, (size_t)n);

  ret = edge_ws_send_frame(ctx, EDGE_WS_OP_TEXT, (const unsigned char*)msg,
                           (size_t)n);
  free(msg);
  return ret;
}

static int edge_send_ssml(edge_tls_t* ctx, const char* escaped, size_t elen)
{
  char date[64];
  unsigned char reqid_raw[16];
  char reqid[33];
  size_t cap = elen + sizeof(date) + 512;
  char* msg = malloc(cap);
  int n;
  int ret;

  if (!msg)
    {
      return -ENOMEM;
    }

  if (agent_secure_random(reqid_raw, sizeof(reqid_raw)) != 0)
    {
      free(msg);
      return -EIO;
    }

  edge_hex_lower(reqid_raw, sizeof(reqid_raw), reqid);
  edge_js_date(edge_utc_now(), date, sizeof(date));

  /* mkssml(): single quotes so the text may contain double quotes.  The
   * trailing "Z" glued onto the human-readable date is in the reference
   * ("This is not a mistake, Microsoft Edge bug."). */
  n = snprintf(msg, cap,
               "X-RequestId:%s\r\n"
               "Content-Type:application/ssml+xml\r\n"
               "X-Timestamp:%sZ\r\n"
               "Path:ssml\r\n\r\n"
               "<speak version='1.0' "
               "xmlns='http://www.w3.org/2001/10/synthesis' "
               "xml:lang='en-US'>"
               "<voice name='%s'>"
               "<prosody pitch='+0Hz' rate='+0%%' volume='+0%%'>"
               "%s"
               "</prosody></voice></speak>",
               reqid, date, s_voice, escaped);

  if (n <= 0 || (size_t)n >= cap)
    {
      free(msg);
      return -EOVERFLOW;
    }

  edge_dump(">> ssml", (const unsigned char*)msg, (size_t)n);

  ret = edge_ws_send_frame(ctx, EDGE_WS_OP_TEXT, (const unsigned char*)msg,
                           (size_t)n);
  free(msg);
  return ret;
}

/* ── Receive loop ────────────────────────────────────────────── */

typedef struct
{
  voice_tts_chunk_cb cb;
  void* user_data;
  size_t pcm_bytes; /* PCM handed to the caller (decoded when the rung is MP3) */
  size_t mp3_bytes; /* MP3 bytes fed to the decoder (mp3 rung only) */
  int riff_checked;
  int truncated; /* P2c: audio started but Path:turn.end never arrived */
  edge_mp3_t* mp3; /* set while the MP3 rung is running */
} edge_sink_t;

/* Any payload bytes at all mean the turn produced audio. */
#define EDGE_SINK_GOT_AUDIO(s) (((s)->pcm_bytes + (s)->mp3_bytes) > 0)

static void edge_sink_pcm(edge_sink_t* sink, const unsigned char* pcm,
                          size_t len)
{
  if (len == 0)
    {
      return;
    }

  if (!sink->riff_checked)
    {
      sink->riff_checked = 1;

      /* raw-* formats are headerless; a RIFF prefix would mean the service
       * ignored outputFormat.  Drop it rather than play 44 bytes of noise. */
      if (len >= 44 && memcmp(pcm, "RIFF", 4) == 0
          && memcmp(pcm + 8, "WAVE", 4) == 0)
        {
          syslog(LOG_WARNING, "[%s] dropping 44-byte RIFF header\n", TAG);
          pcm += 44;
          len -= 44;
        }
    }

  if (len == 0)
    {
      return;
    }

  sink->pcm_bytes += len;
  sink->cb(pcm, len, 0, sink->user_data);
}

/* Decoded MP3 output: same contract as the raw PCM rungs. */
static void edge_sink_decoded(const unsigned char* pcm, size_t len,
                              void* user_data)
{
  edge_sink_t* sink = (edge_sink_t*)user_data;

  if (!pcm || len == 0)
    {
      return;
    }

  sink->pcm_bytes += len;
  sink->cb(pcm, len, 0, sink->user_data);
}

/* One Path:audio payload: either raw PCM (the raw and riff rungs) or MP3
 * that the minimp3 wrapper turns into PCM (the mp3 rung). */
static void edge_sink_audio(edge_sink_t* sink, const unsigned char* data,
                            size_t len)
{
  if (sink->mp3)
    {
      sink->mp3_bytes += len;
      edge_mp3_feed(sink->mp3, data, len);
      return;
    }

  edge_sink_pcm(sink, data, len);
}

static int edge_recv_loop(edge_tls_t* ctx, edge_sink_t* sink)
{
  unsigned char* buf = malloc(EDGE_WS_BUF_SIZE);
  int consecutive_timeouts = 0;
  int err = 0;

  if (!buf)
    {
      return -ENOMEM;
    }

  for (; ; )
    {
      size_t flen = 0;
      int opcode = 0;
      int dropped = 0;
      int ret = edge_ws_recv_frame(ctx, buf, EDGE_WS_BUF_SIZE, &flen, &opcode,
                                   &dropped);

      if (ret != 0)
        {
          if (ret == -ETIMEDOUT)
            {
              /* Silence after audio started is either the end of the turn
               * (turn.end lost) or a BT-PAN stall.  P2c: it must NOT be
               * reported as success - that is how a 0.12s fragment used to
               * pass for a finished sentence and never got retried.  Probe a
               * few extra timeouts, then mark the turn truncated (-EPIPE) and
               * let the caller compare delivered vs expected duration. */
              if (EDGE_SINK_GOT_AUDIO(sink)
                  && ++consecutive_timeouts < EDGE_MAX_CONSEC_TIMEOUTS)
                {
                  syslog(LOG_DEBUG, "[%s] recv timeout %d/%d\n", TAG,
                         consecutive_timeouts, EDGE_MAX_CONSEC_TIMEOUTS);
                  continue;
                }

              if (EDGE_SINK_GOT_AUDIO(sink))
                {
                  sink->truncated = 1;
                  syslog(LOG_WARNING,
                         "[%s] no turn.end after %zu PCM bytes (%.2fs), "
                         "truncated\n",
                         TAG, sink->pcm_bytes,
                         edge_pcm_seconds(sink->pcm_bytes));
                  err = -EPIPE;
                  break;
                }
            }
          else if (ret == -ECONNRESET && EDGE_SINK_GOT_AUDIO(sink))
            {
              sink->truncated = 1;
              syslog(LOG_WARNING,
                     "[%s] peer closed after %zu PCM bytes (%.2fs), truncated\n",
                     TAG, sink->pcm_bytes,
                     edge_pcm_seconds(sink->pcm_bytes));
              err = -EPIPE;
              break;
            }

          syslog(LOG_ERR, "[%s] recv failed: %d (pcm=%zu)\n", TAG, ret,
                 sink->pcm_bytes);
          err = ret;
          break;
        }

      consecutive_timeouts = 0;

      if (dropped)
        {
          continue;
        }

      if (opcode == EDGE_WS_OP_CLOSE)
        {
          /* RFC 6455 close payload: 2-byte status code + UTF-8 reason.  This
           * is where the service explains an aborted turn - always log it. */
          if (flen >= 2)
            {
              unsigned code = ((unsigned)buf[0] << 8) | (unsigned)buf[1];

              syslog(LOG_WARNING,
                     "[%s] server closed the WebSocket: code=%u reason='%.*s'\n",
                     TAG, code, (int)(flen - 2), (const char*)buf + 2);
            }
          else
            {
              syslog(LOG_WARNING, "[%s] server closed the WebSocket\n", TAG);
            }

          /* P2c: a close after audio without turn.end is a truncation too
           * (e.g. the service aborted mid-turn). */
          if (EDGE_SINK_GOT_AUDIO(sink))
            {
              sink->truncated = 1;
              err = -EPIPE;
            }

          edge_dump("< close", buf, flen);
          break;
        }

      if (opcode == EDGE_WS_OP_PING || opcode == EDGE_WS_OP_PONG)
        {
          edge_dump(opcode == EDGE_WS_OP_PING ? "< ping" : "< pong", buf, flen);
          continue;
        }

      if (opcode == EDGE_WS_OP_BIN || opcode == EDGE_WS_OP_CONT)
        {
          size_t hdr_len;
          const unsigned char* payload;

          if (flen < 2)
            {
              continue;
            }

          /* First two bytes, big endian: length of the header text that
           * follows (it ends with CRLFCRLF); audio starts after it. */
          hdr_len = ((size_t)buf[0] << 8) | (size_t)buf[1];

          if (flen < hdr_len + 2)
            {
              syslog(LOG_WARNING, "[%s] short binary frame (%zu/%zu)\n", TAG,
                     flen, hdr_len);
              continue;
            }

          payload = buf + hdr_len + 2;

          if (!edge_header_is((const char*)buf + 2, hdr_len, "Path", "audio"))
            {
              /* Never drop a frame silently: this is how a server-side error
               * payload would hide. */
              syslog(LOG_WARNING,
                     "[%s] binary frame without Path:audio (%zu bytes)\n", TAG,
                     flen - hdr_len - 2);
              edge_dump("< binary hdr", buf + 2, hdr_len);
              edge_dump("< binary body", payload, flen - hdr_len - 2);
              continue;
            }

          if (s_debug && !EDGE_SINK_GOT_AUDIO(sink))
            {
              edge_dump("< audio hdr", buf + 2, hdr_len);
            }

          edge_sink_audio(sink, payload, flen - hdr_len - 2);

          if (EDGE_SINK_GOT_AUDIO(sink))
            {
              /* From here on a stall is treated as end of turn. */
              edge_set_recv_timeout(ctx, EDGE_RECV_TIMEOUT_AUDIO_MS);
            }

          continue;
        }

      /* Text frame: header block, CRLFCRLF, optional body.  Dump the whole
       * frame (headers included) - this is where an error shows up. */
      {
        const char* body = NULL;
        size_t hlen = flen;

        edge_dump("< text", buf, flen);

        if (flen > 0)
          {
            char* end = NULL;
            size_t i;

            for (i = 0; i + 3 < flen; i++)
              {
                if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r'
                    && buf[i + 3] == '\n')
                  {
                    end = (char*)buf + i;
                    break;
                  }
              }

            if (end)
              {
                hlen = (size_t)(end - (char*)buf);
                body = end + 4;
              }
          }

        if (edge_header_is((const char*)buf, hlen, "Path", "turn.end"))
          {
            syslog(LOG_INFO, "[%s] turn.end (%zu PCM bytes)\n", TAG,
                   sink->pcm_bytes);
            break;
          }

        if (edge_header_is((const char*)buf, hlen, "Path", "turn.start"))
          {
            continue;
          }

        if (edge_header_is((const char*)buf, hlen, "Path", "audio.metadata"))
          {
            continue; /* boundary events: not used by this backend */
          }

        /* Normally "response"; anything else is worth a look.  The Path is
         * printed because it tells an abort from a normal turn. */
        {
          const char* path = edge_header_value((const char*)buf, hlen, "Path");
          size_t plen = 0;
          size_t body_len = body
            ? (size_t)(flen - (size_t)(body - (char*)buf)) : 0;

          if (path)
            {
              while ((size_t)(path - (const char*)buf) + plen < hlen
                     && path[plen] != '\r' && path[plen] != '\n')
                {
                  plen++;
                }
            }

          syslog(LOG_WARNING, "[%s] service frame: Path=%.*s body=%.*s\n", TAG,
                 (int)plen, path ? path : "", (int)body_len, body ? body : "");
        }
      }
    }

  free(buf);

  if (sink->pcm_bytes == 0 && err == 0)
    {
      return -EPROTO;
    }

  return err;
}

/* ── One synthesis session ───────────────────────────────────── */

static int edge_build_path(char* buf, size_t cap)
{
  char conn_id[33];
  char gec[65];
  unsigned char raw[16];
  int n;

  if (agent_secure_random(raw, sizeof(raw)) != 0)
    {
      return -EIO;
    }

  edge_hex_lower(raw, sizeof(raw), conn_id);
  edge_sec_ms_gec(gec);

  n = snprintf(buf, cap,
               AGENT_EDGE_TTS_PATH
               "?TrustedClientToken=%s"
               "&ConnectionId=%s"
               "&Sec-MS-GEC=%s"
               "&Sec-MS-GEC-Version=%s",
               AGENT_EDGE_TTS_TRUSTED_TOKEN, conn_id, gec,
               AGENT_EDGE_TTS_SEC_MS_GEC_VERSION);

  if (n <= 0 || (size_t)n >= cap)
    {
      return -EOVERFLOW;
    }

  return 0;
}

/* Synthesise one text segment over one WebSocket session, asking for the
 * given output format. */
static int edge_stream_segment(const char* raw_text, size_t raw_len,
                               edge_sink_t* sink, const char* format)
{
  edge_tls_t ctx;
  char path[320];
  char* escaped = NULL;
  int status = 0;
  int attempt;
  int ret = -EIO;

  escaped = malloc(raw_len + 1);
  if (!escaped)
    {
      return -ENOMEM;
    }

  memcpy(escaped, raw_text, raw_len);
  escaped[raw_len] = '\0';
  edge_clean_text(escaped);

  /* DRM: a 403 is retried once with the clock corrected from the Date header
   * of the failed handshake (see edge_note_server_date()). */
  for (attempt = 0; attempt < 2; attempt++)
    {
      char* xml = NULL;

      ret = edge_build_path(path, sizeof(path));
      if (ret != 0)
        {
          break;
        }

      ret = edge_tls_connect(&ctx, AGENT_EDGE_TTS_HOST, AGENT_EDGE_TTS_PORT);
      if (ret != 0)
        {
          break;
        }

      ret = edge_ws_upgrade(&ctx, AGENT_EDGE_TTS_HOST, path, &status);
      if (ret != 0)
        {
          edge_tls_free(&ctx);

          if (ret == -EACCES && attempt == 0)
            {
              syslog(LOG_WARNING,
                     "[%s] HTTP 403, retrying with corrected clock skew\n",
                     TAG);
              continue;
            }

          break;
        }

      ret = edge_send_speech_config(&ctx, format);
      if (ret == 0)
        {
          xml = edge_xml_escape(escaped);
          if (xml)
            {
              ret = edge_send_ssml(&ctx, xml, strlen(xml));
              free(xml);
            }
          else
            {
              ret = -ENOMEM;
            }
        }

      if (ret == 0)
        {
          ret = edge_recv_loop(&ctx, sink);
        }

      edge_tls_free(&ctx);
      break;
    }

  free(escaped);
  return ret;
}

/* ── Backend API ─────────────────────────────────────────────── */

static int edge_tts_init(void)
{
  memset(s_voice, 0, sizeof(s_voice));

  if (claw_config_get(AGENT_CFG_KEY_EDGE_VOICE, s_voice, sizeof(s_voice))
        != OK
      || s_voice[0] == '\0')
    {
      strncpy(s_voice, AGENT_EDGE_DEFAULT_VOICE, sizeof(s_voice) - 1);
    }

  if (!s_debug_known)
    {
      char v[8];

      s_debug_known = 1;

      if (claw_config_get(AGENT_CFG_KEY_EDGE_DEBUG, v, sizeof(v)) == OK)
        {
          s_debug = atoi(v) != 0;
        }

      syslog(LOG_INFO, "[%s] wire debug %s (edge_debug=0 to disable)\n", TAG,
             s_debug ? "on" : "off");
    }

  /* P2d: 命中过的输出格式从 config store 读回，开机第一句不用再重新探测
   * (每次被拒的探测是一次完整 TLS+WS 往返，BT-PAN 上约 2s)。 */
  if (!s_format_loaded)
    {
      edge_format_load();
    }

  syslog(LOG_INFO, "[%s] voice: %s, format: %s (index %d)\n", TAG, s_voice,
         EDGE_FORMATS[s_format_index].format, s_format_index);
  return 0;
}

/* ── One full synthesis pass ─────────────────────────────────── */

/* Segment the text and walk the format ladder until every segment produced
 * audio (or the pass gave up).  sink->pcm_bytes accumulates what the caller
 * callback actually received, which is what the retry logic below judges. */
static int edge_synthesize_pass(const char* text, edge_sink_t* sink)
{
  const char* p = text;
  size_t remaining = strlen(text);
  int ret = 0;

  while (remaining > 0)
    {
      size_t seg = edge_segment_len(p, remaining);
      int k;
      int delivered = 0;

      /* Format ladder: start at the cached rung, then wrap around the table.
       * The endpoint answers 101 and starts a turn even for output formats it
       * refuses (it then closes with 1007), so the only reliable probe is
       * "did any Path:audio frame arrive".  The wrap-around matters now that
       * the cached rung is persisted (P2d): a stale cache must not be able to
       * wedge synthesis forever. */
      for (k = 0; k < EDGE_NFORMATS; k++)
        {
          int fmt = (s_format_index + k) % EDGE_NFORMATS;
          size_t before = sink->pcm_bytes;
          size_t mp3_in = 0;

          sink->riff_checked = 0; /* each session has its own first frame */
          sink->truncated = 0;

          if (EDGE_FORMATS[fmt].is_mp3)
            {
              sink->mp3 = edge_mp3_open(edge_sink_decoded, sink);
              if (!sink->mp3)
                {
                  syslog(LOG_ERR, "[%s] no memory for the MP3 decoder\n", TAG);
                  break;
                }
            }
          else
            {
              sink->mp3 = NULL;
            }

          syslog(LOG_INFO, "[%s] trying output format: %s%s\n", TAG,
                 EDGE_FORMATS[fmt].format,
                 EDGE_FORMATS[fmt].is_mp3 ? " (decoded with minimp3)" : "");

          ret = edge_stream_segment(p, seg, sink, EDGE_FORMATS[fmt].format);

          if (sink->mp3)
            {
              mp3_in = edge_mp3_input_bytes(sink->mp3);

              syslog(LOG_INFO,
                     "[%s] mp3: %zu bytes in -> %zu PCM bytes out (%zu frames, "
                     "%d Hz, %.2f:1)\n",
                     TAG, mp3_in, edge_mp3_pcm_bytes(sink->mp3),
                     edge_mp3_frames(sink->mp3), edge_mp3_sample_rate(sink->mp3),
                     mp3_in ? (double)edge_mp3_pcm_bytes(sink->mp3) / (double)mp3_in
                            : 0.0);

              edge_mp3_close(sink->mp3);
              sink->mp3 = NULL;
            }

          if (sink->pcm_bytes > before)
            {
              if (fmt != s_format_index)
                {
                  syslog(LOG_INFO, "[%s] using output format: %s\n", TAG,
                         EDGE_FORMATS[fmt].format);
                  s_format_index = fmt;
                  edge_format_save(fmt); /* P2d: survive a reboot */
                }

              delivered = 1;
              break;
            }

          syslog(LOG_WARNING, "[%s] format %s produced no audio (rc=%d)\n", TAG,
                 EDGE_FORMATS[fmt].format, ret);
        }

      if (!delivered)
        {
          /* Audio already delivered is not replayed: the fallback chain must
           * not speak the same sentence twice. */
          if (sink->pcm_bytes > 0)
            {
              syslog(LOG_WARNING,
                     "[%s] segment failed (%d) after %zu PCM bytes, keeping "
                     "partial audio\n",
                     TAG, ret, sink->pcm_bytes);
              break;
            }

          syslog(LOG_ERR, "[%s] synthesis failed: %d\n", TAG, ret);
          return ret;
        }

      p += seg;
      remaining -= seg;

      while (remaining > 0
             && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
        {
          p++;
          remaining--;
        }
    }

  return ret;
}

int edge_tts_synthesize_stream(const char* text, voice_tts_chunk_cb cb,
                               void* user_data)
{
  edge_sink_t sink;
  size_t total_bytes;
  double expected;
  double delivered_s;
  int ret;

  if (!text || !cb)
    {
      return -EINVAL;
    }

  edge_tts_init();

  expected = edge_expected_seconds(text);

  memset(&sink, 0, sizeof(sink));
  sink.cb = cb;
  sink.user_data = user_data;

  ret = edge_synthesize_pass(text, &sink);

  total_bytes = sink.pcm_bytes;
  delivered_s = edge_pcm_seconds(total_bytes);

  syslog(LOG_INFO,
         "[%s] pass 1: delivered %.2fs / ~%.2fs expected (%zu PCM bytes, "
         "rc=%d, truncated=%d)\n",
         TAG, delivered_s, expected, total_bytes, ret, sink.truncated);

  /* P2c: 交付 PCM 时长 vs 文本预期时长。BT-PAN 抖动时音频常常只出来一小截
   * 就断了(旧代码把它当"合成成功"返回)，用户听到半句话且永不重试。
   * 判据: 有音频但 <0.3s，或不到预期时长的一半 -> 重试 1 次。
   * 完全不交付(delivered==0)不在这里重试: 那是彻底失败，交给 voice_tts 的
   * 后端回退链，避免在这里白等一个来回。
   * 注意: 重试会把整句重新合成，已经播出去的那一小段会再响一次 —— 这是
   * "重试一次"的代价，日志里显式说明。 */
  if (total_bytes > 0
      && (delivered_s < EDGE_MIN_DELIVERED_SECS
          || (expected > 0.0
              && delivered_s < expected * EDGE_SHORT_DELIVERY_RATIO)))
    {
      edge_sink_t retry_sink;
      int retry_ret;

      syslog(LOG_WARNING,
             "[%s] short delivery: %.2fs of ~%.2fs expected (truncated=%d, "
             "rc=%d) - retrying once into a fresh stream; audio already played "
             "may repeat\n",
             TAG, delivered_s, expected, sink.truncated, ret);

      memset(&retry_sink, 0, sizeof(retry_sink));
      retry_sink.cb = cb;
      retry_sink.user_data = user_data;

      retry_ret = edge_synthesize_pass(text, &retry_sink);
      total_bytes += retry_sink.pcm_bytes;

      syslog(LOG_INFO,
             "[%s] pass 2: delivered %.2fs (%zu PCM bytes, rc=%d, "
             "truncated=%d), total %.2fs / ~%.2fs expected\n",
             TAG, edge_pcm_seconds(retry_sink.pcm_bytes), retry_sink.pcm_bytes,
             retry_ret, retry_sink.truncated, edge_pcm_seconds(total_bytes),
             expected);

      if (retry_sink.pcm_bytes > 0)
        {
          ret = retry_ret;
        }

      if (retry_sink.pcm_bytes == 0 || retry_sink.truncated
          || edge_pcm_seconds(total_bytes) < EDGE_MIN_DELIVERED_SECS
          || (expected > 0.0
              && edge_pcm_seconds(total_bytes)
                   < expected * EDGE_SHORT_DELIVERY_RATIO))
        {
          /* 仍失败: 尽力交付已有音频(不返回错误，否则 voice_tts 会用下一个
           * 后端把同一句再念一遍)。 */
          syslog(LOG_WARNING,
                 "[%s] still short after the retry: %.2fs of ~%.2fs expected - "
                 "delivering what arrived\n",
                 TAG, edge_pcm_seconds(total_bytes), expected);
        }
    }

  if (total_bytes == 0)
    {
      syslog(LOG_ERR, "[%s] no audio received\n", TAG);
      return ret != 0 ? ret : -EPROTO;
    }

  cb(NULL, 0, 1, user_data);

  syslog(LOG_INFO,
         "[%s] delivered %zu PCM bytes (%.1fs @ %d Hz, expected ~%.1fs)\n", TAG,
         total_bytes, edge_pcm_seconds(total_bytes), AGENT_EDGE_TTS_SAMPLE_RATE,
         expected);
  return 0;
}

/* voice_tts_ops_t.synthesize: accumulate the stream into the caller buffer. */
typedef struct
{
  unsigned char* buf;
  size_t cap;
  size_t len;
  int overflow;
} edge_accum_t;

static void edge_accum_cb(const unsigned char* pcm, size_t len, int is_last,
                          void* user_data)
{
  edge_accum_t* acc = (edge_accum_t*)user_data;

  (void)is_last;

  if (!pcm || len == 0)
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

static int edge_tts_synthesize(const char* text, unsigned char* pcm_out,
                               size_t pcm_cap, size_t* pcm_len)
{
  edge_accum_t acc;
  int ret;

  if (!text || !pcm_out || !pcm_len)
    {
      return -EINVAL;
    }

  *pcm_len = 0;

  memset(&acc, 0, sizeof(acc));
  acc.buf = pcm_out;
  acc.cap = pcm_cap;

  ret = edge_tts_synthesize_stream(text, edge_accum_cb, &acc);

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

static const voice_tts_ops_t s_edge_tts_ops =
{
  .name = "edge",
  .init = edge_tts_init,
  .synthesize = edge_tts_synthesize,
  .deinit = NULL,
  .synthesize_stream = edge_tts_synthesize_stream,
  .sample_rate = AGENT_EDGE_TTS_SAMPLE_RATE,
};

int edge_tts_register(void)
{
  return voice_tts_register(&s_edge_tts_ops);
}
