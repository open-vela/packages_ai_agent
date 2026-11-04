/*
 * mimo_asr.c -- Xiaomi MiMo cloud speech recognition backend.
 *
 * Real implementation, no mocks.  Wraps the recorded PCM (16-bit LE, 16 kHz,
 * mono) into a WAV container, base64 encodes it and POSTs it as an
 * input_audio content part to https://api.xiaomimimo.com/v1/chat/completions
 * with model mimo-v2.5-asr.  The transcript comes back in
 * choices[0].message.content.
 */

#include <nuttx/config.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <stdint.h>
#include <stdio.h>

#include "voice/voice_asr.h"
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
#define MIMO_ASR_MODEL   "mimo-v2.5-asr"

#define MIMO_API_KEY \
  "tp-cxxpul014n9wq31xpjt9gxope9d51s6v7wcdbvob9560cxpq"

/* Cap the clip we upload: 8 s of 16 kHz mono 16-bit PCM. */
#define MIMO_ASR_MAX_PCM (16u * 2u * 8000u)

#define MIMO_RESP_CAP    (8u * 1024u)

static const char *TAG = "mimo_asr";

/* ── base64 encoding (self contained) ───────────────────────────────────── */

static const char s_b64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Locate a JSON string value starting just after a member name. */
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

static int b64_encode(const unsigned char *in, size_t in_len,
                      char *out, size_t out_cap, size_t *out_len)
{
  size_t i = 0u;
  size_t o = 0u;

  if (in == NULL || out == NULL || out_len == NULL)
    {
      return -EINVAL;
    }

  while (i < in_len)
    {
      unsigned long acc = 0ul;
      int have = 0;
      int k;

      for (k = 0; k < 3; k++)
        {
          if (i < in_len)
            {
              acc = (acc << 8) | (unsigned long)in[i++];
              have++;
            }
          else
            {
              acc <<= 8;
            }
        }

      if (o + 4u + 1u > out_cap)
        {
          return -ENAMETOOLONG;
        }

      out[o++] = s_b64[(acc >> 18) & 0x3f];
      out[o++] = s_b64[(acc >> 12) & 0x3f];
      out[o++] = (have > 1) ? s_b64[(acc >> 6) & 0x3f] : '=';
      out[o++] = (have > 2) ? s_b64[acc & 0x3f] : '=';
    }

  out[o] = '\0';

  *out_len = o;

  return 0;
}

/* ── transcript decoding ────────────────────────────────────────────────── */

/* Length of the JSON string body starting at d (d points at the first
 * character after the opening quote).  Escapes are skipped so that an
 * embedded \" does not cut the value short.  Returns (size_t)-1 when the
 * string is unterminated. */
static size_t json_str_len(const char *d)
{
  size_t i;

  if (d == NULL)
    {
      return (size_t)-1;
    }

  for (i = 0u; d[i] != '\0'; i++)
    {
      if (d[i] == '\\')
        {
          if (d[i + 1u] == '\0')
            {
              break;
            }

          i++;
          continue;
        }

      if (d[i] == '"')
        {
          return i;
        }
    }

  return (size_t)-1;
}

static unsigned long hex4(const char *s)
{
  unsigned long v = 0ul;
  int k;

  for (k = 0; k < 4; k++)
    {
      char c = s[k];
      int d;

      if (c >= '0' && c <= '9') { d = c - '0'; }
      else if (c >= 'a' && c <= 'f') { d = c - 'a' + 10; }
      else if (c >= 'A' && c <= 'F') { d = c - 'A' + 10; }
      else { return 0xFFFFFFFFul; }

      v = (v << 4) | (unsigned long)d;
    }

  return v;
}

/* Encode one code point as UTF-8.  Returns the number of bytes written. */
static size_t utf8_put(unsigned long cp, char *dst, size_t cap)
{
  if (cp < 0x80ul)
    {
      if (cap < 1u) { return 0u; }
      dst[0] = (char)(cp & 0x7ful);
      return 1u;
    }

  if (cp < 0x800ul)
    {
      if (cap < 2u) { return 0u; }
      dst[0] = (char)(0xC0ul | ((cp >> 6) & 0x1Ful));
      dst[1] = (char)(0x80ul | (cp & 0x3Ful));
      return 2u;
    }

  if (cp < 0x10000ul)
    {
      if (cap < 3u) { return 0u; }
      dst[0] = (char)(0xE0ul | ((cp >> 12) & 0x0Ful));
      dst[1] = (char)(0x80ul | ((cp >> 6) & 0x3Ful));
      dst[2] = (char)(0x80ul | (cp & 0x3Ful));
      return 3u;
    }

  if (cap < 4u) { return 0u; }

  dst[0] = (char)(0xF0ul | ((cp >> 18) & 0x07ul));
  dst[1] = (char)(0x80ul | ((cp >> 12) & 0x3Ful));
  dst[2] = (char)(0x80ul | ((cp >> 6) & 0x3Ful));
  dst[3] = (char)(0x80ul | (cp & 0x3Ful));

  return 4u;
}

/* Copy [s, s+len) into dst resolving JSON escapes.  dst is always
 * NUL-terminated; the value is truncated when cap is too small. */
static void json_unescape(const char *s, size_t len, char *dst, size_t cap)
{
  size_t i;
  size_t o = 0u;

  if (s == NULL || dst == NULL || cap == 0u)
    {
      return;
    }

  for (i = 0u; i < len && o + 1u < cap; i++)
    {
      char c = s[i];

      if (c != '\\')
        {
          dst[o++] = c;
          continue;
        }

      if (i + 1u >= len)
        {
          break;
        }

      c = s[++i];

      if (c == 'u')
        {
          unsigned long cp;

          if (i + 4u >= len)
            {
              break;
            }

          cp = hex4(s + i + 1u);
          i += 4u;

          if (cp == 0xFFFFFFFFul)
            {
              dst[o++] = '?';
              continue;
            }

          /* Surrogate pair (\uD83D\uDE00 style). */
          if (cp >= 0xD800ul && cp <= 0xDBFFul &&
              i + 6u < len && s[i + 1u] == '\\' && s[i + 2u] == 'u')
            {
              unsigned long lo = hex4(s + i + 3u);

              if (lo >= 0xDC00ul && lo <= 0xDFFFul)
                {
                  cp = 0x10000ul + ((cp - 0xD800ul) << 10) + (lo - 0xDC00ul);
                  i += 6u;
                }
            }

          o += utf8_put(cp, dst + o, cap - o - 1u);
          continue;
        }

      switch (c)
        {
          case 'n': c = '\n'; break;
          case 't': c = '\t'; break;
          case 'r': c = '\r'; break;
          case 'b': c = '\b'; break;
          case 'f': c = '\f'; break;
          case '"': c = '"';  break;
          case '/': c = '/';  break;
          case '\\': c = '\\'; break;
          default: break; /* unknown escape: keep the character as-is */
        }

      dst[o++] = c;
    }

  dst[o] = '\0';
}

/* ── WAV container ──────────────────────────────────────────────────────── */

static void put32_le(unsigned char *p, uint32_t v)
{
  p[0] = (unsigned char)(v & 0xffu);
  p[1] = (unsigned char)((v >> 8) & 0xffu);
  p[2] = (unsigned char)((v >> 16) & 0xffu);
  p[3] = (unsigned char)((v >> 24) & 0xffu);
}

static void put16_le(unsigned char *p, uint16_t v)
{
  p[0] = (unsigned char)(v & 0xffu);
  p[1] = (unsigned char)((v >> 8) & 0xffu);
}

/* Builds a 44 byte header followed by the PCM payload. */
static size_t wav_build(const unsigned char *pcm, size_t pcm_len,
                        unsigned char *out, size_t out_cap)
{
  size_t total = pcm_len + 44u;

  if (out_cap < total)
    {
      return 0u;
    }

  memcpy(out, "RIFF", 4);
  put32_le(out + 4u, (uint32_t)(pcm_len + 36u));
  memcpy(out + 8u, "WAVE", 4);
  memcpy(out + 12u, "fmt ", 4);
  put32_le(out + 16u, 16u);
  put16_le(out + 20u, 1u);
  put16_le(out + 22u, 1u);
  put32_le(out + 24u, 16000u);
  put32_le(out + 28u, 32000u);
  put16_le(out + 32u, 2u);
  put16_le(out + 34u, 16u);
  memcpy(out + 36u, "data", 4);
  put32_le(out + 40u, (uint32_t)pcm_len);
  memcpy(out + 44u, pcm, pcm_len);

  return total;
}

/* ==========================================================================
 * [V759] Upload shrinkage: tail-silence trim + IMA-ADPCM
 * ==========================================================================
 *
 * Why this exists -- measured, not guessed.
 *
 * The ASR request used to carry the whole 8 s capture as raw 16 kHz mono
 * 16-bit PCM: 8 * 16000 * 2 = 256,000 B, wrapped in a WAV container and
 * then base64 encoded to ~341 KB -- uploaded over a freshly handshaked TLS
 * connection on every single turn.  xiaozhi (the reference firmware this
 * port is measured against) ships 16~24 KB of OPUS over a persistent
 * WebSocket, which is most of why that one feels instant and this does not.
 * The project's own note (context_builder.c, [V737]) records the cost:
 *   "a 153-byte answer took ~32 s from end of speech to first audio".
 *
 * Two verified facts bounded the design:
 *   1. MiMo's ASR input_audio accepts ONLY mp3 and wav -- no OPUS, no OGG.
 *      So we cannot simply copy xiaozhi's codec.
 *   2. But "wav" is a container, and IMA-ADPCM (wFormatTag 0x0011, 4 bits
 *      per sample) is a standard WAVE payload: fixed 4:1, ~60 lines, no
 *      library, negligible CPU on an S3, and still a .wav file.
 *
 * Full 8 s: 256,000 B -> 65,024 B, so base64 goes 341 KB -> ~87 KB.
 * The VAD pass below usually removes another 30~60% before encoding starts.
 *
 * Block layout (Microsoft IMA-ADPCM, mono):
 *     4 byte preamble: iSamp(2, signed LE) + bPredictor(1) + reserved(1)
 *     then 252 packed bytes carrying 504 nibbles
 *   => samples per block = (blockAlign - 4) * 2 + 1 = 505
 *   Each block restarts prediction from its own first sample, which is what
 *   keeps a long clip from drifting.
 */

#define V759_BLOCK_ALIGN  256u
#define V759_SP_BLOCK     ((V759_BLOCK_ALIGN - 4u) * 2u + 1u)   /* 505 */

static const int s_v759_steps[89] =
{
      7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
     19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
     50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
   2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
   5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
  15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int s_v759_index[16] =
{
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

/* Encode one 16-bit sample into a 4-bit IMA-ADPCM nibble. */
static unsigned char v759_adpcm_nibble(int sample, int *pred, int *idx)
{
  int step  = s_v759_steps[*idx];
  int diff  = sample - *pred;
  int code  = 0;
  int diffq = step >> 3;

  if (diff < 0)
    {
      code = 8;
      diff = -diff;
    }

  if (diff >= step) { code |= 4; diff -= step; diffq += step; }
  step >>= 1;
  if (diff >= step) { code |= 2; diff -= step; diffq += step; }
  step >>= 1;
  if (diff >= step) { code |= 1; diffq += step; }

  if ((code & 8) != 0) { *pred -= diffq; } else { *pred += diffq; }

  if (*pred > 32767)       { *pred = 32767; }
  else if (*pred < -32768) { *pred = -32768; }

  *idx += s_v759_index[code];

  if (*idx < 0)       { *idx = 0; }
  else if (*idx > 88) { *idx = 88; }

  return (unsigned char)code;
}

/* Encode nsamp mono 16-bit samples into IMA-ADPCM.
 * out must hold ceil(nsamp / V759_SP_BLOCK) * V759_BLOCK_ALIGN bytes.
 *
 * pcm_data comes from a malloc()ed capture buffer, so it is at least
 * 4-byte aligned: casting to short * is safe on this little-endian target
 * (the samples are 16-bit LE). */
static size_t v759_adpcm_encode(const short *s, size_t nsamp,
                                unsigned char *out)
{
  size_t o    = 0u;
  size_t i    = 0u;
  int    pred = 0;
  int    idx  = 0;

  while (i < nsamp)
    {
      size_t n = nsamp - i;
      size_t j;
      size_t blk_end;

      if (n > V759_SP_BLOCK) { n = V759_SP_BLOCK; }

      pred = s[i];
      put16_le(out + o, (uint16_t)(short)pred);
      out[o + 2u] = (unsigned char)idx;
      out[o + 3u] = 0u;
      o += 4u;

      /* Remaining n-1 samples, two per byte, low nibble first. */
      for (j = 1u; j < n; j += 2u)
        {
          unsigned char c = v759_adpcm_nibble(s[i + j], &pred, &idx);

          if (j + 1u < n)
            {
              c |= (unsigned char)(v759_adpcm_nibble(s[i + j + 1u],
                                                     &pred, &idx) << 4);
            }

          out[o++] = c;
        }

      /* Pad a short final block out to the block alignment. */
      blk_end = ((o + V759_BLOCK_ALIGN - 1u) / V759_BLOCK_ALIGN)
                * V759_BLOCK_ALIGN;

      while (o < blk_end) { out[o++] = 0u; }

      i += n;
    }

  return o;
}

/* Build a WAVE_FORMAT_IMA_ADPCM (0x0011) file.
 * Layout: RIFF header, a 20-byte fmt chunk (ADPCM needs cbSize and
 * wSamplesPerBlock), a fact chunk, then the data chunk. */
static size_t v759_wav_adpcm_build(const unsigned char *adpcm, size_t alen,
                                   size_t nsamp, unsigned char *out,
                                   size_t out_cap)
{
  size_t   total = 60u + alen;
  uint32_t avg   = 16000u * (uint32_t)V759_BLOCK_ALIGN
                   / (uint32_t)V759_SP_BLOCK;

  if (out_cap < total) { return 0u; }

  memcpy(out, "RIFF", 4);
  put32_le(out + 4u, (uint32_t)(52u + alen));
  memcpy(out + 8u, "WAVE", 4);

  memcpy(out + 12u, "fmt ", 4);
  put32_le(out + 16u, 20u);                  /* fmt chunk size for ADPCM */
  put16_le(out + 20u, 0x0011u);              /* WAVE_FORMAT_IMA_ADPCM */
  put16_le(out + 22u, 1u);                   /* mono */
  put32_le(out + 24u, 16000u);               /* samples per second */
  put32_le(out + 28u, avg);                  /* average bytes per second */
  put16_le(out + 32u, (uint16_t)V759_BLOCK_ALIGN);
  put16_le(out + 34u, 4u);                   /* bits per sample */
  put16_le(out + 36u, 2u);                   /* cbSize */
  put16_le(out + 38u, (uint16_t)V759_SP_BLOCK);

  memcpy(out + 40u, "fact", 4);
  put32_le(out + 44u, 4u);
  put32_le(out + 48u, (uint32_t)nsamp);

  memcpy(out + 52u, "data", 4);
  put32_le(out + 56u, (uint32_t)alen);
  memcpy(out + 60u, adpcm, alen);

  return total;
}

/* Trim the silence that trails the last voiced frame.
 * Returns how many samples to keep.
 *
 * Walks 20 ms frames from the start, remembers the last one whose peak
 * clears the gate, and keeps 160 ms beyond it so a trailing consonant or a
 * softly spoken last word is not clipped.  Deliberately conservative:
 * over-trimming costs a transcript, under-trimming only costs bytes. */
static size_t v759_trim_tail(const short *s, size_t nsamp)
{
  const size_t frame = 320u;   /* 20 ms at 16 kHz, in samples */
  const int    gate  = 400;    /* 16-bit peak; full scale is 32767 */
  const size_t tail  = 8u * frame;
  size_t last_voice  = 0u;
  size_t f;

  for (f = 0u; f + frame <= nsamp; f += frame)
    {
      size_t i;
      int    peak = 0;

      for (i = 0u; i < frame; i++)
        {
          int v = s[f + i];

          if (v < 0) { v = -v; }
          if (v > peak) { peak = v; }
        }

      if (peak > gate) { last_voice = f + frame; }
    }

  /* Nothing cleared the gate: hand the clip over untouched and let the
   * server decide.  An all-silent clip should come back empty, not error. */
  if (last_voice == 0u) { return nsamp; }

  if (last_voice + tail < nsamp) { return last_voice + tail; }

  return nsamp;
}

/* The ADPCM request path.  Mirrors the PCM path's request/parse logic; the
 * only difference is the container built by v759_wav_adpcm_build().
 *
 * The response parsing is duplicated rather than factored out on purpose:
 * the PCM function below is battle-tested and is the fallback, so it stays
 * byte-for-byte as it was. */
static int v759_asr_adpcm_path(const short *pcm, size_t nsamp,
                               char *text_out, size_t text_cap)
{
  unsigned char *raw = NULL;
  unsigned char *wav = NULL;
  char   *b64  = NULL;
  char   *body = NULL;
  char   *resp = NULL;
  size_t  nblk;
  size_t  alen;
  size_t  wavlen = 0u;
  size_t  b64len = 0u;
  size_t  bodycap;
  int     ret;

  nblk = (nsamp + V759_SP_BLOCK - 1u) / V759_SP_BLOCK;
  alen = nblk * V759_BLOCK_ALIGN;

  raw = (unsigned char *)malloc(alen);

  if (raw == NULL) { return -ENOMEM; }

  v759_adpcm_encode(pcm, nsamp, raw);

  wav     = (unsigned char *)malloc(60u + alen);
  bodycap = ((alen + 60u) * 4u) / 3u + 1024u;
  b64     = (char *)malloc(bodycap);
  body    = (char *)malloc(bodycap);
  resp    = (char *)malloc(MIMO_RESP_CAP);

  if (wav == NULL || b64 == NULL || body == NULL || resp == NULL)
    {
      ret = -ENOMEM;

      goto out;
    }

  wavlen = v759_wav_adpcm_build(raw, alen, nsamp, wav, 60u + alen);

  if (wavlen == 0u)
    {
      ret = -EINVAL;

      goto out;
    }

  if (b64_encode(wav, wavlen, b64, bodycap, &b64len) != 0)
    {
      ret = -EINVAL;

      goto out;
    }

  snprintf(body, bodycap,
           "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":"
           "[{\"type\":\"input_audio\",\"input_audio\":{\"data\":"
           "\"data:audio/wav;base64,%s\",\"format\":\"wav\"}}]}],"
           "\"asr_options\":{\"language\":\"zh\"}}",
           MIMO_ASR_MODEL, b64);

  {
    vela_header_t hdrs[2];

    hdrs[0].name = "Authorization";
    hdrs[0].value = "Bearer " MIMO_API_KEY;
    hdrs[1].name = NULL;
    hdrs[1].value = NULL;

    ret = vela_https_post_json(MIMO_HOST, MIMO_PORT, MIMO_PATH,
                               hdrs, body, resp, MIMO_RESP_CAP);
  }

  if (ret < 0)
    {
      syslog(LOG_ERR, "[MIMO-ASR] adpcm transport failed ret=%d\n", ret);

      goto out;
    }

  if (ret >= 400)
    {
      syslog(LOG_ERR, "[MIMO-ASR] adpcm HTTP %d\n", ret);

      ret = -EIO;

      goto out;
    }

  {
    const char *p = strstr(resp, "\"choices\"");

    if (p == NULL) { p = resp; }

    {
      const char *c = strstr(p, "\"content\"");
      const char *v = NULL;
      size_t vl = 0u;

      if (c == NULL)
        {
          syslog(LOG_ERR, "[MIMO-ASR] adpcm: no content in reply\n");

          ret = -ENOENT;

          goto out;
        }

      if (json_str_after(c + 9u, &v, &vl) != 0)
        {
          ret = -EINVAL;

          goto out;
        }

      {
        size_t rl = json_str_len(v);

        if (rl == (size_t)-1)
          {
            syslog(LOG_ERR, "[MIMO-ASR] adpcm: unterminated content\n");

            ret = -EPROTO;

            goto out;
          }

        json_unescape(v, rl, text_out, text_cap);
      }
    }
  }

  syslog(LOG_INFO, "[MIMO-ASR] transcript (adpcm): %s\n", text_out);

  ret = 0;

out:
  if (raw != NULL)  { free(raw); }
  if (wav != NULL)  { free(wav); }
  if (b64 != NULL)  { free(b64); }
  if (body != NULL) { free(body); }
  if (resp != NULL) { free(resp); }

  return ret;
}

/* ── backend ops ────────────────────────────────────────────────────────── */

int __attribute__((used)) mimo_asr_init(void)
{
  syslog(LOG_INFO, "[MIMO-ASR] backend ready host=%s model=%s\n",
         MIMO_HOST, MIMO_ASR_MODEL);

  return 0;
}

void __attribute__((used)) mimo_asr_deinit(void)
{
  syslog(LOG_INFO, "[MIMO-ASR] backend released\n");
}

/* [V759] The original request path, now reachable only as the fallback
 * when the ADPCM container is rejected.  Body deliberately untouched. */
static int v759_asr_pcm_path(const unsigned char *pcm_data,
                              size_t pcm_len,
                              char *text_out,
                              size_t text_cap)
{
  unsigned char *wav = NULL;
  char *b64 = NULL;
  char *body = NULL;
  char *resp = NULL;
  size_t wavlen = 0u;
  size_t b64len = 0u;
  size_t send = pcm_len;
  int ret;

  if (pcm_data == NULL || text_out == NULL || text_cap == 0u)
    {
      return -EINVAL;
    }

  text_out[0] = '\0';

  if (pcm_len < 320u)
    {
      /* Too short to contain speech; report empty rather than error. */
      return -EINVAL;
    }

  if (send > MIMO_ASR_MAX_PCM)
    {
      send = MIMO_ASR_MAX_PCM;
    }

  wav = (unsigned char *)malloc(send + 44u);
  b64 = (char *)malloc(((send + 44u) * 4u) / 3u + 8u);
  body = (char *)malloc(((send + 44u) * 4u) / 3u + 1024u);
  resp = (char *)malloc(MIMO_RESP_CAP);

  if (wav == NULL || b64 == NULL || body == NULL || resp == NULL)
    {
      ret = -ENOMEM;

      goto out;
    }

  wavlen = wav_build(pcm_data, send, wav, send + 44u);

  if (wavlen == 0u)
    {
      ret = -EINVAL;

      goto out;
    }

  if (b64_encode(wav, wavlen, b64, ((send + 44u) * 4u) / 3u + 8u,
                 &b64len) != 0)
    {
      ret = -EINVAL;

      goto out;
    }

  /* NOTE on the closing brackets after %s:
   *   "  -> closes the data string
   *   }} -> closes input_audio{} and the content-part object{}
   *   ]  -> closes "content":[]
   *   }  -> closes the message object{}
   *   ]  -> closes "messages":[]
   * Only after that does "asr_options" sit at the ROOT level, which is what
   * the MiMo endpoint expects. */
  snprintf(body, ((send + 44u) * 4u) / 3u + 1024u,
           "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":"
           "[{\"type\":\"input_audio\",\"input_audio\":{\"data\":"
           "\"data:audio/wav;base64,%s\",\"format\":\"wav\"}}]}],"
           "\"asr_options\":{\"language\":\"zh\"}}",
           MIMO_ASR_MODEL, b64);

  /* Request. */
  {
    vela_header_t hdrs[2];

    hdrs[0].name = "Authorization";
    hdrs[0].value = "Bearer " MIMO_API_KEY;
    hdrs[1].name = NULL;
    hdrs[1].value = NULL;

    ret = vela_https_post_json(MIMO_HOST, MIMO_PORT, MIMO_PATH,
                               hdrs, body, resp, MIMO_RESP_CAP);
  }

  if (ret < 0)
    {
      syslog(LOG_ERR, "[MIMO-ASR] transport failed ret=%d\n", ret);

      goto out;
    }

  if (ret >= 400)
    {
      syslog(LOG_ERR, "[MIMO-ASR] HTTP %d\n", ret);

      ret = -EIO;

      goto out;
    }

  /* Extract choices[0].message.content. */
  {
    const char *p = strstr(resp, "\"choices\"");

    if (p == NULL) { p = resp; }

    {
      const char *c = strstr(p, "\"content\"");
      const char *v = NULL;
      size_t vl = 0u;

      if (c == NULL)
        {
          syslog(LOG_ERR, "[MIMO-ASR] no content in reply\n");

          ret = -ENOENT;

          goto out;
        }

      if (json_str_after(c + 9u, &v, &vl) != 0)
        {
          ret = -EINVAL;

          goto out;
        }

      /* json_str_after() stops at the first quote, which is wrong for a
       * value containing \" ; recompute with escape awareness and then
       * resolve the escapes into the caller's buffer. */
      {
        size_t rl = json_str_len(v);

        if (rl == (size_t)-1)
          {
            syslog(LOG_ERR, "[MIMO-ASR] unterminated content string\n");

            ret = -EPROTO;

            goto out;
          }

        json_unescape(v, rl, text_out, text_cap);
      }
    }
  }

  syslog(LOG_INFO, "[MIMO-ASR] transcript: %s\n", text_out);

  ret = 0;

out:
  if (wav != NULL) { free(wav); }
  if (b64 != NULL) { free(b64); }
  if (body != NULL) { free(body); }
  if (resp != NULL) { free(resp); }

  return ret;
}

int mimo_asr_recognize(const unsigned char *pcm_data,
                              size_t pcm_len,
                              char *text_out,
                              size_t text_cap)
{
  size_t nsamp;
  size_t kept;
  size_t maxsamp;
  int    ret;

  if (pcm_data == NULL || text_out == NULL || text_cap == 0u)
    {
      return -EINVAL;
    }

  text_out[0] = '\0';

  if (pcm_len < 320u)
    {
      /* Too short to contain speech; report empty rather than error. */
      return -EINVAL;
    }

  maxsamp = MIMO_ASR_MAX_PCM / 2u;
  nsamp   = pcm_len / 2u;

  if (nsamp > maxsamp) { nsamp = maxsamp; }

  /* [V844] Drop the saturated transient at the START of the capture.
   *
   * Measured on this board: the first 2046-byte chunk of every single
   * recording reads peak=32768 min=-32768 -- clipped hard against the
   * int16 ceiling -- while chunk#2 onward is normal (peak ~15k).  It is a
   * one-off pop when the stream opens, not a gain problem.
   *
   * Left alone it is spliced onto the front of the utterance that gets
   * uploaded, so the recogniser is handed a full-scale burst where the
   * beginning of the speech should be.
   *
   * Bounded on purpose: stop at the first sample that is not clipped, and
   * never skip more than 2400 samples (150 ms @ 16 kHz), so a genuinely
   * loud voice cannot have its start eaten. */
  {
    const short *s = (const short *)pcm_data;
    size_t       skip = 0u;

    while (skip < nsamp && skip < 2400u &&      /* 150 ms @ 16 kHz */
           (s[skip] >= 32000 || s[skip] <= -32000))
      {
        skip++;
      }

    if (skip > 0u)
      {
        V1024S(LOG_ERR, "[V844]] dropped %u saturated lead sample(s) "
               "(capture-start transient; first chunk was clipped at "
               "full scale)\n", (unsigned)skip);

        pcm_data += (skip * 2u);
        nsamp    -= skip;
      }
  }

  /* [V759] Drop the silence trailing the last voiced frame.  A 3 s
   * utterance followed by 5 s of room noise used to cost exactly as much
   * upload as a full 8 s one. */
  kept = v759_trim_tail((const short *)pcm_data, nsamp);

  V1024S(LOG_INFO, "[V759]] clip %u -> %u samp (%u%% of 8 s): %u B PCM -> %u B ADPCM\n",
         (unsigned)nsamp, (unsigned)kept,
         (unsigned)((kept * 100u) / maxsamp),
         (unsigned)(kept * 2u),
         (unsigned)(((kept + V759_SP_BLOCK - 1u) / V759_SP_BLOCK)
                    * V759_BLOCK_ALIGN));

  /* Small upload first. */
  ret = v759_asr_adpcm_path((const short *)pcm_data, kept,
                            text_out, text_cap);

  if (ret != 0 || text_out[0] == '\0')
    {
      /* [V843] An EMPTY transcript is a failure too.
       *
       * The condition used to be `ret != 0` alone.  The MiMo endpoint
       * answers the IMA-ADPCM WAV with HTTP 200 and a well-formed reply
       * that carries no transcript at all, so the adpcm path returns 0 and
       * this fallback never ran -- eight of nine recognitions in the
       * 2026-10-17 capture came back empty with ret=0 and the working PCM
       * path was never reached.
       *
       * Getting here now means either the request broke or the server
       * accepted the container without understanding the payload.  Resend
       * the same, already-trimmed clip as plain PCM -- the pre-V759
       * behaviour.  Worst case is one extra round trip. */
      if (ret != 0)
        {
          V1024S(LOG_ERR, "[V843]] ADPCM ret=%d, retrying as PCM\n", ret);
        }
      else
        {
          V1024S(LOG_ERR, "[V843]] ADPCM returned OK but EMPTY transcript "
                 "(server took the container, decoded nothing) -> "
                 "retrying as plain PCM\n");
        }

      text_out[0] = '\0';

      ret = v759_asr_pcm_path(pcm_data, kept * 2u, text_out, text_cap);

      if (ret == 0)
        {
          V1024S(LOG_ERR, "[V759]] PCM retry OK: ADPCM unusable here\n");
        }
    }

  return ret;
}

static const voice_asr_ops_t s_mimo_asr_ops __attribute__((used)) =
{
  .name      = "mimo",
  .init      = mimo_asr_init,
  .recognize = mimo_asr_recognize,
  .deinit    = mimo_asr_deinit
};

int __attribute__((noinline, used)) mimo_asr_register(void)
{
  int ret = voice_asr_register(&s_mimo_asr_ops);

  if (ret == 0)
    {
      /* [V501] Cloud MiMo is reachable and verified: use it.  The previous
     * "http" backend pointed at 172.16.88.1:9009, which does not exist,
     * so ASR could never produce a transcript.  "http" stays registered
     * as a fallback. */
      voice_asr_set_backend("mimo");   /* literal: mimo_asr.h is not
                                         included here, and this must
                                         match .name = "mimo" below */
    }

  return ret;
}
