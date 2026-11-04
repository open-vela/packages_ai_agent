/*
 * mimo_tts.c -- Xiaomi MiMo cloud text-to-speech backend.
 *
 * Real implementation, no mocks.  POSTs a chat-completions request to
 * https://api.xiaomimimo.com/v1/chat/completions with model mimo-v2.5-tts and
 * audio:{format:"wav"}.  The reply carries the synthesised clip as base64 WAV
 * in choices[0].message.audio.data.  We decode it, locate the WAV "data"
 * chunk, read the real sample rate (MiMo returns 24000 Hz) and resample to the
 * 16 kHz the board's I2S runs at, then hand the PCM back through the generic
 * voice_tts_ops_t synthesize() contract (16-bit LE, 16 kHz, mono).
 */

#include <nuttx/config.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <pthread.h>   /* [V851] background TTS prefetch */
#include <unistd.h>    /* [V851] usleep */
#include <stdint.h>
#include <stdio.h>
#include <time.h>      /* [V761] clock_gettime() for the per-chunk timing */

#include "voice/voice_tts.h"
#include "voice/mimo_tts.h"
#include "infra/vela_tls.h"

/* [V1024] Probe gate -- see fortify_stubs.c. */
#ifdef __cplusplus
extern "C" int g_v1024_quiet;
#else
extern int g_v1024_quiet;
#endif

#ifndef V1024_QUIET
#define V1024_QUIET 1
#endif
#define V1024P(...) do { if (V1024_QUIET == 0) printf(__VA_ARGS__); } while (0)
#define V1024S(lvl, ...) do { if (V1024_QUIET == 0) syslog(lvl, __VA_ARGS__); } while (0)


#define MIMO_HOST        "token-plan-cn.xiaomimimo.com"
#define MIMO_PORT        "443"
#define MIMO_PATH        "/v1/chat/completions"
#define MIMO_TTS_MODEL   "mimo-v2.5-tts"

/* API key provisioned for this device. */
#define MIMO_API_KEY \
  "tp-cxxpul014n9wq31xpjt9gxope9d51s6v7wcdbvob9560cxpq"

/* [V706] Was 384 KB.  The reply carries the clip as base64 WAV and grows
 * ~5.3 KB per input byte, so 384 KB capped one request at ~72 text bytes
 * and silently forced even a one-sentence answer (48 B) into the slow
 * sentence-by-sentence path.  The buffer is a plain malloc() on the PSRAM
 * app heap, which the v705 capture measures at "arena=8388608
 * free=7439272" -- 1 MB is free, so let one request render ~193 bytes. */
#define MIMO_RESP_CAP    (1024u * 1024u)
#define MIMO_TEXT_MAX    1024u
#define MIMO_PCM_RATE    16000u

static const char *TAG = "mimo_tts";

/* ── base64 decoding (self contained, no external dependency) ───────────── */

static int b64_val(int c)
{
  if (c >= 'A' && c <= 'Z') { return c - 'A'; }
  if (c >= 'a' && c <= 'z') { return c - 'a' + 26; }
  if (c >= '0' && c <= '9') { return c - '0' + 52; }
  if (c == '+') { return 62; }
  if (c == '/') { return 63; }
  return -1;
}

/* Returns 0 on success, negative errno on malformed input. */
static int b64_decode(const char *in, size_t in_len,
                      unsigned char *out, size_t *out_len)
{
  size_t i;
  size_t o = 0;
  int acc = 0;
  int bits = 0;

  if (in == NULL || out == NULL || out_len == NULL)
    {
      return -EINVAL;
    }

  for (i = 0; i < in_len; i++)
    {
      int c = in[i];

      if (c == '=')
        {
          break;
        }

      if (c == '\r' || c == '\n' || c == ' ' || c == '\t')
        {
          continue;
        }

      {
        int v = b64_val(c);

        if (v < 0)
          {
            return -EINVAL;
          }

        acc = (acc << 6) | v;
        bits += 6;

        if (bits >= 8)
          {
            bits -= 8;

            out[o++] = (unsigned char)((acc >> bits) & 0xff);

            if (o + 1u > (size_t)0x7fffffffu)
              {
                return -EINVAL;
              }
          }
      }
    }

  *out_len = o;

  return 0;
}

/* ── JSON helpers ───────────────────────────────────────────────────────── */

/* Escape src into dst as a JSON string body.  dst_cap includes the NUL. */
static int json_escape(const char *src, char *dst, size_t dst_cap)
{
  size_t o = 0;
  size_t i;

  if (src == NULL || dst == NULL || dst_cap == 0u)
    {
      return -EINVAL;
    }

  for (i = 0; src[i] != '\0'; i++)
    {
      unsigned char c = (unsigned char)src[i];
      const char *repl = NULL;

      if (c == '"') { repl = "\\\""; }
      else if (c == '\\') { repl = "\\\\"; }
      else if (c == '\n') { repl = "\\n"; }
      else if (c == '\r') { repl = "\\r"; }
      else if (c == '\t') { repl = "\\t"; }
      else if (c < 0x20)
        {
          /* Control characters must be escaped; map them to a space. */
          repl = " ";
        }

      if (repl != NULL)
        {
          size_t rl = strlen(repl);

          if (o + rl + 1u > dst_cap)
            {
              return -ENAMETOOLONG;
            }

          memcpy(&dst[o], repl, rl);
          o += rl;
        }
      else
        {
          if (o + 2u > dst_cap)
            {
              return -ENAMETOOLONG;
            }

          dst[o++] = (char)c;
        }
    }

  dst[o] = '\0';

  return 0;
}

/* Locate the string value of a JSON member starting at p (p points just after
 * the member name).  Returns 0 and sets *vstart / *vlen on success. */
static int json_str_after(const char *p, const char **vstart, size_t *vlen)
{
  const char *d = p;
  const char *e;

  if (p == NULL || vstart == NULL || vlen == NULL)
    {
      return -EINVAL;
    }

  while (*d != '\0' && *d != ':') { d++; }

  if (*d != ':') { return -EINVAL; }

  d++;

  while (*d == ' ' || *d == '\t') { d++; }

  if (*d != '"') { return -EINVAL; }

  d++;
  e = strchr(d, '"');

  if (e == NULL) { return -EINVAL; }

  *vstart = d;
  *vlen = (size_t)(e - d);

  return 0;
}

/* ── WAV parsing ────────────────────────────────────────────────────────── */

static uint32_t rd32_le(const unsigned char *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16_le(const unsigned char *p)
{
  return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

/* Walk the RIFF chunks and describe the PCM payload. */
static int wav_parse(const unsigned char *w, size_t len,
                     uint32_t *rate, uint16_t *bits, uint16_t *channels,
                     const unsigned char **data, size_t *dlen)
{
  size_t off = 12u;

  if (w == NULL || len < 44u || rate == NULL || bits == NULL ||
      channels == NULL || data == NULL || dlen == NULL)
    {
      return -EINVAL;
    }

  if (memcmp(w, "RIFF", 4) != 0 || memcmp(w + 8u, "WAVE", 4) != 0)
    {
      return -EINVAL;
    }

  while (off + 8u <= len)
    {
      uint32_t csz = rd32_le(w + off + 4u);
      size_t body = off + 8u;

      if (memcmp(w + off, "fmt ", 4) == 0)
        {
          if (body + 16u <= len)
            {
              *channels = rd16_le(w + body + 2u);
              *rate = rd32_le(w + body + 4u);
              *bits = rd16_le(w + body + 14u);
            }
        }
      else if (memcmp(w + off, "data", 4) == 0)
        {
          size_t avail = len - body;

          if (csz > avail) { csz = (uint32_t)avail; }

          *data = w + body;
            *dlen = (size_t)csz;

            if (*rate == 0u) { *rate = 24000u; }
            if (*bits == 0u) { *bits = 16u; }
            if (*channels == 0u) { *channels = 1u; }

            return 0;
        }

      if (csz == 0u) { break; }

      off = body + (size_t)csz;

      /* Chunk sizes are word aligned. */
      if ((csz & 1u) != 0u) { off++; }
    }

  return -ENOENT;
}

/* ── resampling 24000 -> 16000 (2-phase polyphase FIR) ─────────────────── */

/* n_in input samples at 24 kHz produce floor(n_in * 2 / 3) samples at 16 kHz.
 * Writes at most cap_out samples.  Returns the number of samples written.
 *
 * [V659] The output rate is exactly 2/3 of the input rate, so output sample i
 * sits at input position 1.5*i -- an integer for even i and a half sample for
 * odd i.  Two precomputed Q14 branches cover exactly those two phases:
 *
 *   s_rs_h0[49]   integer phase: Hamming-windowed sinc, fc = 7.4 kHz @ 24 kHz
 *   s_rs_h1[52]   half-sample phase: the Catmull-Rom half-sample interpolator
 *                 is folded into the coefficients, so the inner loop stays a
 *                 plain MAC instead of needing four loads per tap.
 *
 * What this replaces, and why: the old code low-passed with two cascaded
 * [1 2 1]/4 passes and then interpolated linearly.  Measured end-to-end that
 * is -1.3 dB at 2 kHz, -5.0 at 4 kHz, -8.9 at 5 kHz and -13.3 at 6 kHz -- the
 * whole speech-clarity band tilted down by 1.4..13 dB, which is the muffled /
 * "wrong" timbre heard on the greeting -- while 8..12 kHz only reached
 * -24 dB and folded a metallic edge back into 0..4 kHz.  This FIR measures
 * -0.01 dB at 2 kHz, -0.22 at 4 kHz, -0.50 at 6 kHz, -2.10 at 7 kHz and
 * -18.8 dB at 7.8 kHz.
 *
 * Both branches sum to 16384 (unity gain).  sum|h| * 32768 stays below
 * INT32_MAX, so a 32-bit accumulator cannot overflow for any input.  Phase
 * was verified independently: on a ramp x[n]=n the integer branch returns
 * exactly n0 and the half branch exactly n0+0.5.
 *
 * Cost: 49 (or 52) MACs per output sample, ~0.8 M MAC/s at 16 kHz. */

#define RS_H0_TAPS   49
#define RS_H1_TAPS   52
#define RS_HALF      ((RS_H0_TAPS - 1) / 2)   /* 24 = group delay @ 24 kHz */

static const int16_t s_rs_h0[RS_H0_TAPS] =
{
     10,    10,   -22,     4,    32,   -37,   -19,    80,
    -41,   -89,   143,    10,  -223,   179,   172,  -409,
    104,   512,  -608,  -244,  1215,  -761, -1715,  4844,
  10092,  4844, -1715,  -761,  1215,  -244,  -608,   512,
    104,  -409,   172,   179,  -223,    10,   143,   -89,
    -41,    80,   -19,   -37,    32,     4,   -22,    10,
     10
};

static const int16_t s_rs_h1[RS_H1_TAPS] =
{
     -1,     5,    13,    -8,   -13,    24,    -2,   -39,
     39,    28,   -87,    32,   106,  -140,   -36,   237,
   -151,  -214,   410,   -45,  -587,   632,   378, -1772,
   1177,  8206,  8206,  1177, -1772,   378,   632,  -587,
    -45,   410,  -214,  -151,   237,   -36,  -140,   106,
     32,   -87,    28,    39,   -39,    -2,    24,   -13,
     -8,    13,     5,    -1
};

static size_t resample_24k_to_16k(int16_t *in, size_t n_in,
                                  int16_t *out, size_t cap_out)
{
  size_t n_out = (n_in * 2u) / 3u;
  size_t i;

  if (in == NULL || out == NULL || n_in < 2u)
    {
      return 0u;
    }

  if (n_out > cap_out) { n_out = cap_out; }

  for (i = 0u; i < n_out; i++)
    {
      unsigned long q   = 3ul * (unsigned long)i;
      long          n0  = (long)(q >> 1);
      long          acc = 0;
      int           k;

      if ((q & 1ul) == 0ul)
        {
          /* Integer phase: the output sits exactly on input sample n0. */

          for (k = 0; k < RS_H0_TAPS; k++)
            {
              long m = n0 - RS_HALF + k;

              if (m >= 0 && (size_t)m < n_in)
                {
                  acc += (long)s_rs_h0[k] * (long)in[m];
                }
            }
        }
      else
        {
          /* Half-sample phase.  The interpolator is already folded into the
           * coefficients, so the argument window simply shifts by one. */

          for (k = 0; k < RS_H1_TAPS; k++)
            {
              long m = n0 - RS_HALF - 1 + k;

              if (m >= 0 && (size_t)m < n_in)
                {
                  acc += (long)s_rs_h1[k] * (long)in[m];
                }
            }
        }

      acc = (acc + 8192l) >> 14;            /* Q14 -> PCM */

      if (acc > 32767l)  { acc = 32767l;  }
      if (acc < -32768l) { acc = -32768l; }

      out[i] = (int16_t)acc;
    }

  return n_out;
}

/* ── backend ops ───────────────────────────────────────────────────────── */

static __attribute__((used)) int mimo_tts_init(void)
{
  syslog(LOG_INFO, "[MIMO-TTS] backend ready host=%s model=%s\n",
         MIMO_HOST, MIMO_TTS_MODEL);

  return 0;
}

static __attribute__((used)) void mimo_tts_deinit(void)
{
  syslog(LOG_INFO, "[MIMO-TTS] backend released\n");
}

/* [V787] Per-stage timing for the TTS round trip.
 *
 * There was no timing anywhere in this file, so "the reply is slow" had
 * no measurable cause.  These counters cost one clock read per stage and
 * are printed once per synthesise() call.  t_post is the one to watch: it
 * is the only stage that streaming can shorten.
 */
static unsigned long v787_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
      return 0ul;
    }

  return (unsigned long)ts.tv_sec * 1000ul +
         (unsigned long)(ts.tv_nsec / 1000000l);
}


static __attribute__((used)) int mimo_tts_synthesize(const char *text,
                               unsigned char *pcm_out,
                               size_t pcm_cap,
                               size_t *pcm_len)
{
  char *body = NULL;
  char esc[MIMO_TEXT_MAX];
  char *resp = NULL;
  unsigned char *wav = NULL;
  const char *b64 = NULL;
  size_t b64len = 0u;
  const unsigned char *pcm24 = NULL;
  size_t pcm24_len = 0u;
  uint32_t rate = 0u;
  uint16_t bits = 0u;
  uint16_t ch = 0u;
  size_t wavlen = 0u;
  size_t frames;
  /* [V787] stage stamps, milliseconds */
  unsigned long v787_t0 = v787_ms();
  unsigned long v787_t_esc;
  unsigned long v787_t_alloc;
  unsigned long v787_t_post;
  unsigned long v787_t_b64;
  unsigned long v787_t_parse;
  unsigned long v787_t_resamp;
  unsigned long v787_t_end;
  unsigned long v798_t0 = v787_ms();   /* [V798] absolute stamp */
  size_t v787_textlen = strlen(text);
  size_t wrote = 0u;
  int ret;

  if (text == NULL || pcm_out == NULL || pcm_len == NULL)
    {
      return -EINVAL;
    }

  *pcm_len = 0u;

  if (text[0] == '\0')
    {
      /* Nothing to say is not an error, but there is nothing to play. */
      return -EINVAL;
    }

  if (pcm_cap < 2u)
    {
      return -EINVAL;
    }

  /* [V787] escape */
  v787_t_end = v787_ms();
  v787_t_esc = v787_t_end - v787_t0;
  if (json_escape(text, esc, sizeof(esc)) != 0)
    {
      syslog(LOG_ERR, "[MIMO-TTS] text too long or unescapable\n");

      return -ENAMETOOLONG;
    }

  body = (char *)malloc(MIMO_TEXT_MAX + 256u);
  resp = (char *)malloc(MIMO_RESP_CAP);

  /* [V787] alloc */
  v787_t_end = v787_ms();
  v787_t_alloc = v787_t_end - (v787_t0 + v787_t_esc);
  if (body == NULL || resp == NULL)
    {
      ret = -ENOMEM;

      goto out;
    }

  snprintf(body, MIMO_TEXT_MAX + 256u,
           "{\"model\":\"%s\",\"messages\":[{\"role\":\"assistant\","
           "\"content\":\"%s\"}],\"audio\":{\"format\":\"wav\"}}",
           MIMO_TTS_MODEL, esc);

  /* Request. */
  {
    vela_header_t hdrs[2];

    hdrs[0].name = "Authorization";
    hdrs[0].value = "Bearer " MIMO_API_KEY;
    hdrs[1].name = NULL;
    hdrs[1].value = NULL;

    /* [V787] https -- dominant stage, and the only one that
     * streaming can shorten.  Ret is logged by the caller too. */
    /* [V798] Directly measured HTTPS time.  The [V787] "post"
     * figure is derived by subtracting cumulative stamps from each
     * other, so an error in any stage propagates into the rest; these
     * two stamps are independent of that chain. */
    ret = vela_https_post_json(MIMO_HOST, MIMO_PORT, MIMO_PATH,
                               hdrs, body, resp, MIMO_RESP_CAP);
                               v787_t_end = v787_ms();
                               v787_t_post = v787_t_end -
                                 (v787_t0 + v787_t_esc + v787_t_alloc);
  }

  if (ret < 0)
    {
      syslog(LOG_ERR, "[MIMO-TTS] transport failed ret=%d\n", ret);

      goto out;
    }

  /* [V798] Directly measured HTTPS time, independent of the
   * cumulative-stamp arithmetic in [V787].  Placed after the
   * ret<0 guard so resp is known to be allocated here. */
  syslog(LOG_INFO,
         "[MIMO-TTS] [V798] https=%lu ms reply=%u B derived_post=%lu\n",
         v787_ms() - v798_t0, (unsigned)strlen(resp),
         v787_t_post);

  if (ret >= 400)
    {
      syslog(LOG_ERR, "[MIMO-TTS] HTTP %d\n", ret);

      ret = -EIO;

      goto out;
    }

  /* Locate choices[0].message.audio.data (base64 WAV). */
  {
    const char *p = strstr(resp, "\"audio\"");
    const char *d;

    if (p == NULL) { p = resp; }

    d = strstr(p, "\"data\"");

    if (d == NULL || json_str_after(d + 6u, &b64, &b64len) != 0)
      {
        syslog(LOG_ERR, "[MIMO-TTS] no audio.data in reply\n");
        ret = -ENOENT;

        goto out;
      }
  }

  wavlen = (b64len * 3u) / 4u + 8u;
  wav = (unsigned char *)malloc(wavlen);

  if (wav == NULL)
    {
      ret = -ENOMEM;

      goto out;
    }

  /* [V787] base64 decode: the reply is one big base64 string, so
   * this scales with how much the model said. */
  v787_t_end = v787_ms();
  v787_t_b64 = v787_t_end -
    (v787_t0 + v787_t_esc + v787_t_alloc + v787_t_post);
  if (b64_decode(b64, b64len, wav, &wavlen) != 0)
    {
      syslog(LOG_ERR, "[MIMO-TTS] base64 decode failed\n");

      ret = -EINVAL;

      goto out;
    }

  /* [V787] wav header parse */
  v787_t_end = v787_ms();
  v787_t_parse = v787_t_end -
    (v787_t0 + v787_t_esc + v787_t_alloc + v787_t_post +
     v787_t_b64);
  if (wav_parse(wav, wavlen, &rate, &bits, &ch, &pcm24, &pcm24_len) != 0)
    {
      syslog(LOG_ERR, "[MIMO-TTS] bad WAV\n");

      ret = -EINVAL;

      goto out;
    }

  if (bits != 16u || ch != 1u)
    {
      syslog(LOG_ERR, "[MIMO-TTS] unsupported format bits=%u ch=%u\n",
             (unsigned)bits, (unsigned)ch);
      ret = -EINVAL;

      goto out;
    }

  frames = pcm24_len / 2u;

  if (rate == MIMO_PCM_RATE)
    {
      wrote = frames * 2u;

      if (wrote > pcm_cap) { wrote = pcm_cap - (pcm_cap % 2u); }

      memcpy(pcm_out, pcm24, wrote);
    }
  else
    {
      size_t n_out;
      size_t cap_frames = pcm_cap / 2u;
      size_t o_frames;

      wrote = 0u;

      n_out = (frames * (size_t)MIMO_PCM_RATE) / (size_t)rate;

      if (n_out > cap_frames) { n_out = cap_frames; }

      o_frames = resample_24k_to_16k((int16_t *)pcm24, frames,
                                     (int16_t *)pcm_out, n_out);

      wrote = o_frames * 2u;
    }

  *pcm_len = wrote;

  syslog(LOG_INFO,
         "[MIMO-TTS] synthesized %u bytes in_rate=%u out_rate=%u\n",
         (unsigned)wrote, (unsigned)rate, (unsigned)MIMO_PCM_RATE);

         /* [V787] Stage breakdown.  If t_post dominates, the latency is
          * the server round trip and only streaming helps.  If t_b64 or
          * t_resamp are large, the fix is local and cheap.  t_end is
          * stamped right after resample, so t_resamp excludes the copy
          * above it. */
         v787_t_end = v787_ms();
         v787_t_resamp = v787_t_end -
           (v787_t0 + v787_t_esc + v787_t_alloc + v787_t_post +
            v787_t_b64 + v787_t_parse);
         
         syslog(LOG_INFO,
                "[MIMO-TTS] [V787] %u B text | esc %lu alloc %lu post %lu"
                " b64 %lu parse %lu resamp %lu total %lu ms\n",
                (unsigned)v787_textlen, v787_t_esc, v787_t_alloc,
                v787_t_post, v787_t_b64, v787_t_parse, v787_t_resamp,
                v787_t_end - v787_t0);

  ret = 0;

out:
  if (body != NULL) { free(body); }
  if (resp != NULL) { free(resp); }
  if (wav != NULL) { free(wav); }

  return ret;
}

/* ── streaming entry point ───────────────────────────────────────────────
 *
 * voice_channel speaks through the streaming API, so the one-shot
 * synthesize() above is not enough on its own: this wrapper renders the whole
 * clip into a heap buffer and pushes it to the callback in playback-sized
 * chunks, terminated by the is_last marker.
 */

/* [V706] Was 192 KB (6 s).  With MIMO_WHOLE_TEXT_BYTES now 128 the clip
 * can run ~12 s, so give the PCM buffer 512 KB (16 s @ 16 kHz s16le). */
#define MIMO_TTS_STREAM_PCM_CAP (512u * 1024u)
/* [V710] Was 2048 (64 ms of audio per transfer).  audio_playback_write()
 * submits one DMA transfer per call, so the smaller this is the more often
 * the transmitter has to be re-armed -- and every re-arm is a hole in the
 * audio.  4064 is AP_WRITE_CHUNK_MAX in audio_playback.c and sits just
 * inside I2S_DMABUF_SLOT_SZ (4096), so it is the largest single transfer the
 * driver will take: 127 ms of audio, half the turnarounds. */
#define MIMO_TTS_STREAM_CHUNK   4064u

/* [V650] Chunked synthesis limits.
 *
 * The MiMo reply carries the whole utterance as base64 WAV and its size grows
 * with the input text (~5.7 KB of response per input byte, measured).  Both
 * buffers here are fixed size, so a long assistant reply cannot be rendered in
 * one request:
 *
 *   MIMO_RESP_CAP           =  256 KB  -> ~49 text bytes before the base64
 *                                        payload loses its closing quote
 *   MIMO_TTS_STREAM_PCM_CAP =  192 KB  -> ~153 text bytes (6 s) before PCM is
 *                                        silently truncated
 *
 * Measured on the board: the boot greeting (27 B) worked, a 132-byte reply
 * failed with "no audio.data in reply".  36 bytes of text is ~216 KB of
 * response, comfortably inside the 256 KB response buffer.
 */
/* [V655] Longest text we ever try to render in one request. */
/* [V737] Was 128u.  The stale comment below still budgets the response
 * against a 256 KB buffer, but MIMO_RESP_CAP has been 1 MB for a long time.
 * Measured on the board: 153 bytes of text -> 271360 bytes of PCM, i.e.
 * 1.77 KB of PCM per input byte.  224 bytes -> 397 KB PCM (inside the 512 KB
 * MIMO_TTS_STREAM_PCM_CAP) and ~530 KB base64 (inside the 1 MB response
 * buffer).  Raising this is what turns a 3-4 request synthesis into one. */
#define MIMO_CHUNK_TEXT_BYTES   224u
/* [V655] Longest text we try whole before falling back to
 * pieces: the response body grows ~5.3 KB per input byte, so
 * past ~49 bytes the single request cannot fit MIMO_RESP_CAP
 * and would only burn a TLS round trip.  Keep a margin. */
#define MIMO_WHOLE_TEXT_BYTES   224u
/* [V761] Budget for the FIRST chunk only.
 *
 * Measured on the board: 1.77 KB of PCM per input byte, so 64 bytes of
 * text is about 113 KB of PCM, which at 16 kHz mono 16 bit is ~3.5 s of
 * speech synthesized in roughly 1.2 s.  Synthesis is ~3x faster than
 * playback, so the later chunks are always ready before the speaker
 * reaches them -- the queue never starves.
 *
 * Why small: first-audio latency is the ONLY number a voice UI user feels.
 * The 224 B whole-utterance path made the speaker stay silent until the
 * entire reply had been rendered, which is where the 1.7~6.3 s spread came
 * from.  A boot greeting is well under 64 B, so short replies still go out
 * as a single request and behave exactly as before.
 */
#define MIMO_FIRST_TEXT_BYTES  64u
/* Length of the UTF-8 character that starts at s[0] (1..4). */
static size_t mimo_utf8_len(unsigned char c)
{
  if ((c & 0xE0u) == 0xC0u) { return 2u; }
  if ((c & 0xF0u) == 0xE0u) { return 3u; }
  if ((c & 0xF8u) == 0xF0u) { return 4u; }

  return 1u;
}

/* True when the character ending at s[end - 1] is a good place to break:
 * ASCII space / newline / . ! ? , ; : or a CJK full-width punctuation mark.
 */
static int mimo_is_break(const unsigned char *s, size_t end)
{
  unsigned char c;

  if (end == 0u) { return 0; }

  c = s[end - 1u];

  if (c == ' ' || c == '\n' || c == '\r' || c == '\t' ||
      c == '.' || c == '!' || c == '?' || c == ',' ||
      c == ';' || c == ':' || c == '-')
    {
      return 1;
    }

  if (end >= 3u && (c & 0xF0u) == 0xE0u)
    {
      unsigned int cp = ((unsigned int)(s[end - 3u] & 0x0Fu) << 12) |
                        ((unsigned int)(s[end - 2u] & 0x3Fu) << 6) |
                         (unsigned int)(s[end - 1u] & 0x3Fu);

      switch (cp)
        {
          case 0x3002u:          /* 。 */
          case 0xFF01u:          /* ！ */
          case 0xFF1Fu:          /* ？ */
          case 0xFF1Bu:          /* ； */
          case 0xFF0Cu:          /* ， */
          case 0x3001u:          /* 、 */
          case 0xFF1Au:          /* ： */
          case 0x2014u:          /* — */
          case 0x2026u:          /* … */
            return 1;
          default:
            break;
        }
    }

  return 0;
}

/* How many bytes of s to take for the next chunk: at most max_bytes, never
 * splitting a UTF-8 character, and preferring to stop right after punctuation
 * so the per-chunk prosody stays natural.  Returns 0 when s is empty.
 *
 * At least 8 bytes are kept per chunk if a punctuation cut is available, so a
 * run like "a.a.a..." cannot produce a swarm of tiny requests.
 */
static size_t mimo_chunk_len(const char *s, size_t max_bytes)
{
  size_t n = 0u;
  size_t best = 0u;

  /* [V655] An empty string must yield 0, not the length of its NUL byte.
   *
   * Without this guard the function fell through to the "single character
   * longer than max_bytes" fallback below and returned mimo_utf8_len('\0'),
   * which is 1.  The chunk loop therefore never saw cl == 0 at the end of the
   * text: it took one extra 1-byte piece whose payload was just the
   * terminator, and mimo_tts_synthesize() rejected that empty string with a
   * silent -EINVAL -- the "[MIMO-TTS] chunk N failed (-22), stopping" line
   * that appeared with no accompanying diagnostic.
   */

  if (s[0] == '\0')
    {
      return 0u;
    }

  while (s[n] != '\0')
    {
      size_t clen = mimo_utf8_len((unsigned char)s[n]);

      if (n + clen > max_bytes) { break; }

      n += clen;

      if (mimo_is_break((const unsigned char *)s, n)) { best = n; }
    }

  if (n == 0u)
    {
      /* Single character longer than max_bytes: emit it anyway. */
      return mimo_utf8_len((unsigned char)s[0]);
    }

  if (best >= 8u && best < n) { return best; }

  return n;
}

/* -- [V851] background prefetch of the next synthesis chunk -----------------
 *
 * The chunk loop used to be strictly serial:
 *
 *     synth(chunk N) -> push(chunk N) to the speaker -> synth(chunk N+1) ...
 *
 * mimo_push_pcm() does not return until the DMA has consumed the clip, so
 * the HTTP round trip of chunk N+1 happened with an empty playback queue --
 * it was played as silence in the middle of the sentence.  Measured on the
 * board: chunk 0 = 20 B text -> 46080 B pcm (1.44 s of audio) in 1180 ms,
 * then chunk 1 = 13 B -> 61440 B in 2480 ms, i.e. a 2.5 s hole right after
 * 1.44 s of speech.  That is the reported stutter.
 *
 * Fix: start synthesizing chunk N+1 on a background thread *before* the
 * speaker starts on chunk N, so the round trip is hidden behind audio that
 * is already playing.  Only one prefetch is ever in flight and it owns
 * g_pf_buf until it reports done; the wait is bounded so a wedged request
 * cannot wedge the speaker.
 */

struct mimo_pf_s
{
  pthread_t       tid;
  char            piece[MIMO_CHUNK_TEXT_BYTES + 1u];
  unsigned char  *buf;
  size_t          cap;
  size_t          len;
  int             rc;
  long            ms;
  volatile int    done;
  volatile int    abandoned;  /* [V868] parent gave up; self-free */
};

static unsigned char *g_pf_buf;    /* allocated once, never freed */
static volatile int   g_pf_busy;   /* a prefetch thread owns g_pf_buf */

static void *mimo_pf_thread(void *arg)
{
  struct mimo_pf_s *p = (struct mimo_pf_s *)arg;
  struct timespec   c0;
  struct timespec   c1;
  size_t            n = 0u;

  clock_gettime(CLOCK_MONOTONIC, &c0);
  p->rc = mimo_tts_synthesize(p->piece, p->buf, p->cap, &n);
  clock_gettime(CLOCK_MONOTONIC, &c1);

  p->len = n;
  p->ms  = (c1.tv_sec - c0.tv_sec) * 1000
         + (c1.tv_nsec - c0.tv_nsec) / 1000000;
  p->done = 1;


  /* [V868] The parent timed out on us and has already stopped
   * looking at this struct.  It cannot free it -- we are still
   * writing to it -- so we own it now: hand g_pf_buf back and
   * release ourselves.  Without this the parent freed the struct
   * out from under these three stores. */
  if (p->abandoned)
    {
      g_pf_busy = 0;
      free(p);
    }
  return NULL;
}

/* Push one synthesized clip to the playback callback in 2 KB pieces. */
static void mimo_push_pcm(const unsigned char *pcm, size_t pcm_len,
                          voice_tts_chunk_cb cb, void *user_data)
{
  size_t off = 0u;

  while (off < pcm_len)
    {
      size_t n = pcm_len - off;

      if (n > MIMO_TTS_STREAM_CHUNK) { n = MIMO_TTS_STREAM_CHUNK; }

      cb(pcm + off, n, 0, user_data);
      off += n;
    }
}

int __attribute__((used)) mimo_tts_speak_stream(const char *text,
                          voice_tts_chunk_cb cb,
                          void *user_data)
{
  unsigned char *pcm = NULL;
  size_t pcm_len = 0u;
  size_t total = 0u;
  int nchunks = 0;
  int ret;

  if (text == NULL || cb == NULL)
    {
      return -EINVAL;
    }

  pcm = (unsigned char *)malloc(MIMO_TTS_STREAM_PCM_CAP);

  if (pcm == NULL)
    {
      return -ENOMEM;
    }

  /* [V655] SUPERSEDED BY V761 -- kept only for the measurements below.
   *
   * The two-phase design this comment describes no longer exists: V761
   * removed Phase 1 and always renders sentence by sentence.  Do not read
   * "Phase 1 asks for the whole utterance in one request" as current
   * behaviour -- that single-request path is precisely what was removed to
   * cut first-audio latency.  The measured numbers below still stand and
   * are still the reason MIMO_CHUNK_TEXT_BYTES is 224.
   *
   * It asked for the whole utterance in one request, which keeps the
   *
   * Phase 1 asks for the whole utterance in one request, which keeps the
   * prosody intact and is what short texts (the boot greeting) already did
   * successfully.  It can only work while the reply fits MIMO_RESP_CAP: the
   * response carries the audio as base64 WAV and grows ~5.3 KB per input
   * byte (measured), so past ~49 bytes the payload loses its closing quote
   * and the reply looks like "no audio.data in reply".  MIMO_WHOLE_TEXT_BYTES
   * keeps a safety margin below that; longer text skips phase 1 outright
   * rather than burning a TLS round trip on a request that cannot succeed.
   *
   * Phase 2 renders the same text sentence by sentence (MIMO_CHUNK_TEXT_BYTES
   * per request).  mimo_chunk_len() never splits a UTF-8 character and
   * prefers to cut just after punctuation so each piece keeps natural
   * prosody.  Only one is_last marker is emitted, at the very end, so
   * playback still sees a single continuous utterance.
   */

  total = 0u;
  nchunks = 0;
  ret = -ENOENT;

  /* [V761] First-chunk-priority synthesis.
   *
   * The old code had two phases.  Phase 1 asked for the WHOLE utterance in
   * one request whenever it fit in MIMO_WHOLE_TEXT_BYTES (224 B ~ 74 Han
   * characters), which is nearly every real answer, and only pushed PCM to
   * playback after all of it had been rendered.  The speaker therefore
   * stayed silent for the full synthesis of the whole reply -- 1.7 s for a
   * short one, 6.3 s for a long one, which is exactly the spread measured
   * on the board.
   *
   * Phase 2 already did the right thing (render sentence by sentence and
   * push each one as soon as it arrived) but was only reached for texts
   * over the limit or after a Phase 1 failure.
   *
   * Keep Phase 2, drop Phase 1, and give the first chunk a smaller budget
   * than the rest.  Short texts are unaffected: they fit inside
   * MIMO_FIRST_TEXT_BYTES, so the first chunk is the whole reply and there
   * is exactly one request, same as before.
   *
   * Failure handling is unchanged: a first chunk that fails still returns
   * the error to the caller, a later chunk that fails stops the loop but
   * keeps whatever already went to the speaker.
   */
  {
    const char *t = text;
    int first = 1;
    struct mimo_pf_s *pf = NULL;   /* [V851] synthesis in flight */

    for (;;)
      {
        char piece[MIMO_CHUNK_TEXT_BYTES + 1u];
        size_t budget = first ? (size_t)MIMO_FIRST_TEXT_BYTES
                              : (size_t)MIMO_CHUNK_TEXT_BYTES;
        size_t cl = mimo_chunk_len(t, budget);
        struct timespec c0;
        struct timespec c1;
        long ms;
        int  swapped = 0;   /* [V851] pcm came from the prefetch thread */

        if (cl == 0u) { break; }

        memcpy(piece, t, cl);
        piece[cl] = '\0';

        /* [V851] If a background synthesis for this chunk has been running
         * while the previous one played, adopt its result instead of
         * issuing the request now.  The wait is bounded: a wedged HTTP
         * request must not wedge the speaker.
         */
        if (pf != NULL)
          {
            int waited = 0;

            while (!pf->done && waited < 30000)
              {
                usleep(20000);
                waited += 20;
              }

            if (pf->done)
              {
                unsigned char *tmp;

                pthread_join(pf->tid, NULL);
                tmp       = pcm;
                pcm       = pf->buf;
                g_pf_buf  = tmp;
                pcm_len   = pf->len;
                ret       = pf->rc;
                ms        = pf->ms;
                swapped   = 1;
                g_pf_busy = 0;
                free(pf);
                pf = NULL;
              }
            else
              {
                V1024S(LOG_ERR, "[V851]] prefetch of chunk %d did not finish "
                       "in %dms; continuing without prefetch\n",
                       nchunks, waited);
                /* [V868] Do NOT free: the thread is still
                 * running and still writes p->len / p->done.
                 * Hand the struct over to it. */
                pf->abandoned = 1;
                pthread_detach(pf->tid);
                pf = NULL;
              }
          }

        if (!swapped)
          {
            clock_gettime(CLOCK_MONOTONIC, &c0);
            ret = mimo_tts_synthesize(piece, pcm, MIMO_TTS_STREAM_PCM_CAP,
                                      &pcm_len);
            clock_gettime(CLOCK_MONOTONIC, &c1);
            ms = (c1.tv_sec - c0.tv_sec) * 1000
               + (c1.tv_nsec - c0.tv_nsec) / 1000000;
          }

        if (ret != 0)
          {
            V1024S(LOG_ERR, "[V761]] chunk %d failed (%d), stopping\n",
                   nchunks, ret);

            if (nchunks == 0)
              {
                free(pcm);
                return ret;
              }

            /* [V851] Drop the in-flight prefetch before leaving the loop so
             * its buffer is not written after we are done with it.
             */
            if (pf != NULL)
              {
                int w = 0;
                int fin;

                while (!pf->done && w < 30000)
                  {
                    usleep(20000);
                    w += 20;
                  }

                fin = pf->done;

                if (fin)
                  {
                    pthread_join(pf->tid, NULL);
                    free(pf);
                    pf = NULL;
                    g_pf_busy = 0;
                  }
                else
                  {
                    /* [V868] The thread is still writing to
                     * this struct; it frees itself when it
                     * is done and releases g_pf_buf then. */
                    pf->abandoned = 1;
                    pthread_detach(pf->tid);
                    pf = NULL;
                  }
              }

            break;
          }

        /* [V851] Kick off the NEXT chunk before the speaker starts on this
         * one, so its HTTP round trip overlaps audio that is already
         * playing instead of becoming silence at the chunk boundary.
         */
        {
          size_t ncl = mimo_chunk_len(t + cl, (size_t)MIMO_CHUNK_TEXT_BYTES);

          if (ncl != 0u && !g_pf_busy)
            {
              if (g_pf_buf == NULL)
                {
                  g_pf_buf = (unsigned char *)malloc(MIMO_TTS_STREAM_PCM_CAP);
                }

              if (g_pf_buf != NULL)
                {
                  struct mimo_pf_s *np =
                    (struct mimo_pf_s *)malloc(sizeof(struct mimo_pf_s));

                  if (np != NULL)
                    {
                      pthread_attr_t at;

                      memset(np, 0, sizeof(*np));
                      memcpy(np->piece, t + cl, ncl);
                      np->piece[ncl] = 0;
                      np->buf = g_pf_buf;
                      np->cap = MIMO_TTS_STREAM_PCM_CAP;

                      pthread_attr_init(&at);
                      pthread_attr_setstacksize(&at, 32768);

                      if (pthread_create(&np->tid, &at, mimo_pf_thread, np) != 0)
                        {
                          pthread_attr_destroy(&at);
                          free(np);
                          V1024S(LOG_ERR, "[V851]] prefetch thread failed\n");
                        }
                      else
                        {
                          pthread_attr_destroy(&at);
                          pf = np;
                          g_pf_busy = 1;
                        }
                    }
                }
            }
        }

        mimo_push_pcm(pcm, pcm_len, cb, user_data);

        V1024S(LOG_INFO, "[V761]] chunk %d: %u B text -> %u B pcm in %ldms%s\n",
               nchunks, (unsigned)cl, (unsigned)pcm_len, ms,
               first ? " (first audio here)" : "");

        total += pcm_len;
        t += cl;
        ++nchunks;
        first = 0;
      }
  }

  if (nchunks == 0)
    {
      free(pcm);
      return -ENOENT;
    }

  cb(NULL, 0u, 1, user_data);

  syslog(LOG_INFO,
         "[MIMO-TTS] streamed %u bytes to playback in %d chunk(s)\n",
         (unsigned)total, nchunks);

  free(pcm);

  return 0;
}

static const voice_tts_ops_t s_mimo_tts_ops __attribute__((used)) =
{
  .name       = MIMO_TTS_BACKEND_NAME,
  .init       = mimo_tts_init,
  .synthesize = mimo_tts_synthesize,
  .deinit     = mimo_tts_deinit
};

int __attribute__((noinline, used)) mimo_tts_register(void)
{
  int ret = voice_tts_register(&s_mimo_tts_ops);

  if (ret == 0)
    {
      voice_tts_set_backend("mimo");
    }

  return ret;
}
