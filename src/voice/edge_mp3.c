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
 * edge_mp3.c - streaming MP3 -> PCM for the Edge TTS backend.
 *
 * Decoder: minimp3, https://github.com/lieff/minimp3 (CC0-1.0, public
 * domain), vendored verbatim in thirdparty/minimp3.h and fetched from
 * https://cdn.jsdelivr.net/gh/lieff/minimp3@master/minimp3.h
 * (md5 c3eb1a828607e8f9f0b6f3ae35f4100b).  See thirdparty/README.md.
 *
 * Two properties of minimp3 shape this file:
 *
 *  1. mp3dec_decode_frame() keeps a mp3dec_scratch_t - 16,256 bytes with this
 *     toolchain - on the stack.  The voice path runs on 16 KB threads (the
 *     outbound channel, the LVGL UI channel, and the NSH console that
 *     voice_test_tts runs on), so calling it as it comes would overflow the
 *     stack.  edge_mp3_decode_frame() below is upstream's function body
 *     (MINIMP3_ONLY_MP3 path) with that scratch moved into the heap state
 *     allocated by edge_mp3_open().  The two were verified sample-exact
 *     against each other on the host (see the test in the report).
 *  2. It does not skip ID3v2 tags; edge_mp3_id3_size() does that here.
 *
 * Frames are decoded whole and aligned at the buffer start.  That keeps
 * minimp3's fast path (previous header still matching at offset 0) and its
 * bit reservoir alive; handing it a whole WebSocket payload instead would
 * reset the decoder once per payload and click.
 */

#include "voice/edge_mp3.h"

#include "agent_compat.h"

#include <stdlib.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3 /* the endpoint only sends Layer III */
#define MINIMP3_NO_SIMD  /* Cortex-M33: use the portable C path */
#include "thirdparty/minimp3.h"

static const char* TAG = "edge_mp3";

/* One 24 ms frame at 48 kbps is ~144 bytes; 4 KB holds more than 20 of them
 * plus the partial one that a WebSocket boundary can leave behind. */
#define EDGE_MP3_BUF_SIZE (4 * 1024)

struct edge_mp3_s
{
  mp3dec_t dec;
  mp3dec_scratch_t* scratch; /* 16 KB: heap, not stack - see file comment */
  mp3dec_frame_info_t info;
  mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
  unsigned char buf[EDGE_MP3_BUF_SIZE];
  size_t len;
  size_t in_bytes;
  size_t pcm_bytes;
  size_t frames;
  int sample_rate;
  int channels;
  int id3_done;
  edge_mp3_pcm_cb cb;
  void* user_data;
};

/* ── Decoder entry ───────────────────────────────────────────────────────
 *
 * Copied from minimp3.h:mp3dec_decode_frame() (upstream, MINIMP3_ONLY_MP3
 * path; the layer I/II branch is a "return 0" there as well).  The single
 * change is that the scratch buffer is a pointer into the caller's heap state
 * instead of a 16 KB stack local.  Keep in sync if the vendored header is
 * updated. */

static int edge_mp3_decode_frame(mp3dec_t* dec, mp3dec_scratch_t* scratch,
                                 const uint8_t* mp3, int mp3_bytes,
                                 mp3d_sample_t* pcm,
                                 mp3dec_frame_info_t* info)
{
  int i = 0, igr, frame_size = 0, success = 1;
  const uint8_t* hdr;
  bs_t bs_frame[1];

  if (mp3_bytes > 4 && dec->header[0] == 0xff && hdr_compare(dec->header, mp3))
    {
      frame_size = hdr_frame_bytes(mp3, dec->free_format_bytes)
                   + hdr_padding(mp3);
      if (frame_size != mp3_bytes
          && (frame_size + HDR_SIZE > mp3_bytes
              || !hdr_compare(mp3, mp3 + frame_size)))
        {
          frame_size = 0;
        }
    }

  if (!frame_size)
    {
      memset(dec, 0, sizeof(mp3dec_t));
      i = mp3d_find_frame(mp3, mp3_bytes, &dec->free_format_bytes, &frame_size);
      if (!frame_size || i + frame_size > mp3_bytes)
        {
          info->frame_bytes = i;
          return 0;
        }
    }

  hdr = mp3 + i;
  memcpy(dec->header, hdr, HDR_SIZE);
  info->frame_bytes = i + frame_size;
  info->frame_offset = i;
  info->channels = HDR_IS_MONO(hdr) ? 1 : 2;
  info->hz = hdr_sample_rate_hz(hdr);
  info->layer = 4 - HDR_GET_LAYER(hdr);
  info->bitrate_kbps = hdr_bitrate_kbps(hdr);

  if (!pcm)
    {
      return hdr_frame_samples(hdr);
    }

  bs_init(bs_frame, hdr + HDR_SIZE, frame_size - HDR_SIZE);
  if (HDR_IS_CRC(hdr))
    {
      get_bits(bs_frame, 16);
    }

  if (info->layer == 3)
    {
      int main_data_begin = L3_read_side_info(bs_frame, scratch->gr_info, hdr);

      if (main_data_begin < 0 || bs_frame->pos > bs_frame->limit)
        {
          mp3dec_init(dec);
          return 0;
        }

      success = L3_restore_reservoir(dec, bs_frame, scratch, main_data_begin);
      if (success)
        {
          for (igr = 0; igr < (HDR_TEST_MPEG1(hdr) ? 2 : 1);
               igr++, pcm += 576 * info->channels)
            {
              memset(scratch->grbuf[0], 0, 576 * 2 * sizeof(float));
              L3_decode(dec, scratch,
                        scratch->gr_info + igr * info->channels,
                        info->channels);
              mp3d_synth_granule(dec->qmf_state, scratch->grbuf[0], 18,
                                 info->channels, pcm, scratch->syn[0]);
            }
        }

      L3_save_reservoir(dec, scratch);
    }
  else
    {
      return 0; /* MINIMP3_ONLY_MP3 */
    }

  return success * hdr_frame_samples(dec->header);
}

/* ── Stream handling ─────────────────────────────────────────────────── */

/* ID3v2 tag size (header + body + optional footer), 0 when there is none. */
static size_t edge_mp3_id3_size(const unsigned char* d, size_t len)
{
  size_t size, total;

  if (len < 10 || memcmp(d, "ID3", 3) != 0)
    {
      return 0;
    }

  /* syncsafe integer: 7 bits per byte */
  size = ((size_t)(d[6] & 0x7f) << 21) | ((size_t)(d[7] & 0x7f) << 14)
         | ((size_t)(d[8] & 0x7f) << 7) | (size_t)(d[9] & 0x7f);

  total = 10 + size;

  if (d[5] & 0x10) /* footer present */
    {
      total += 10;
    }

  return total;
}

/* Byte size of the Layer III frame at the head of the buffer, or 0 when it
 * cannot be told (free format, junk) - then minimp3's own scanner runs. */
static int edge_mp3_frame_size(const unsigned char* h, size_t len)
{
  if (len < HDR_SIZE || !hdr_valid(h) || HDR_IS_FREE_FORMAT(h))
    {
      return 0;
    }

  return hdr_frame_bytes(h, 0) + hdr_padding(h);
}

static void edge_mp3_emit(edge_mp3_t* m, int samples)
{
  size_t bytes = (size_t)samples * (size_t)m->info.channels
                 * sizeof(mp3d_sample_t);

  if (bytes == 0)
    {
      return;
    }

  if (m->frames == 0)
    {
      syslog(LOG_INFO, "[%s] first frame: %d Hz, %s, %d kbps\n", TAG,
             m->info.hz, m->info.channels == 1 ? "mono" : "stereo",
             m->info.bitrate_kbps);
    }

  m->sample_rate = m->info.hz;
  m->channels = m->info.channels;
  m->frames++;
  m->pcm_bytes += bytes;
  m->cb((const unsigned char*)m->pcm, bytes, m->user_data);
}

/* Decode everything that is complete in the buffer. */
static void edge_mp3_drain(edge_mp3_t* m)
{
  while (m->len > 0)
    {
      int frame_size;
      int samples;
      size_t consumed;

      if (!m->id3_done)
        {
          size_t skip;

          /* Wait for the full 10 byte tag header (or the first frame): a
           * short chunk must not be mistaken for "no ID3 here", or the tag
           * body ends up in minimp3's resync scanner and can eat a frame. */
          if (m->len < 10)
            {
              return;
            }

          skip = edge_mp3_id3_size(m->buf, m->len);

          if (skip > 0)
            {
              if (skip > m->len)
                {
                  if (skip <= EDGE_MP3_BUF_SIZE)
                    {
                      return; /* tag not fully buffered yet */
                    }

                  syslog(LOG_INFO,
                         "[%s] ID3v2 tag (%zu bytes) too large, dropped\n",
                         TAG, skip);
                  m->len = 0;
                  return;
                }

              memmove(m->buf, m->buf + skip, m->len - skip);
              m->len -= skip;
              syslog(LOG_INFO, "[%s] skipped ID3v2 tag: %zu bytes\n", TAG, skip);
            }

          m->id3_done = 1;
        }

      frame_size = edge_mp3_frame_size(m->buf, m->len);

      if (frame_size > 0 && (size_t)frame_size > m->len)
        {
          return; /* the rest of this frame is still in flight */
        }

#ifdef EDGE_MP3_USE_UPSTREAM_DECODE
      /* Escape hatch for a platform whose TTS thread has more than ~24 KB of
       * stack: call upstream minimp3 verbatim (16,256-byte stack frame)
       * instead of the heap-scratch copy above. */
      samples = mp3dec_decode_frame(&m->dec, m->buf,
                                    frame_size > 0 ? frame_size : (int)m->len,
                                    m->pcm, &m->info);
#else
      samples = edge_mp3_decode_frame(&m->dec, m->scratch, m->buf,
                                      frame_size > 0 ? frame_size : (int)m->len,
                                      m->pcm, &m->info);
#endif

      consumed = m->info.frame_bytes > 0 ? (size_t)m->info.frame_bytes : 0;

      if (samples > 0)
        {
          edge_mp3_emit(m, samples);
        }

      if (consumed == 0)
        {
          return; /* need more bytes before anything can be decoded */
        }

      if (consumed > m->len)
        {
          consumed = m->len;
        }

      if (consumed < m->len)
        {
          memmove(m->buf, m->buf + consumed, m->len - consumed);
        }

      m->len -= consumed;
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

edge_mp3_t* edge_mp3_open(edge_mp3_pcm_cb cb, void* user_data)
{
  edge_mp3_t* m = calloc(1, sizeof(edge_mp3_t));

  if (!m)
    {
      syslog(LOG_ERR, "[%s] out of memory (%zu bytes)\n", TAG,
             sizeof(edge_mp3_t));
      return NULL;
    }

  m->scratch = malloc(sizeof(mp3dec_scratch_t));
  if (!m->scratch)
    {
      syslog(LOG_ERR, "[%s] out of memory (scratch %zu bytes)\n", TAG,
             sizeof(mp3dec_scratch_t));
      free(m);
      return NULL;
    }

  mp3dec_init(&m->dec);
  m->cb = cb;
  m->user_data = user_data;
  return m;
}

int edge_mp3_feed(edge_mp3_t* m, const unsigned char* data, size_t len)
{
  if (!m || (!data && len > 0))
    {
      return -EINVAL;
    }

  m->in_bytes += len;

  while (len > 0)
    {
      size_t space = sizeof(m->buf) - m->len;
      size_t n = len < space ? len : space;

      memcpy(m->buf + m->len, data, n);
      m->len += n;
      data += n;
      len -= n;

      edge_mp3_drain(m);

      if (len > 0 && m->len == sizeof(m->buf))
        {
          /* Nothing in here decodes: keep the tail (a frame may span the
           * boundary) and throw the rest away instead of wedging. */
          size_t drop = sizeof(m->buf) / 2;

          syslog(LOG_WARNING, "[%s] no decodable frame, dropping %zu bytes\n",
                 TAG, drop);
          memmove(m->buf, m->buf + drop, m->len - drop);
          m->len -= drop;
        }
    }

  return 0;
}

void edge_mp3_close(edge_mp3_t* m)
{
  if (!m)
    {
      return;
    }

  free(m->scratch);
  free(m);
}

size_t edge_mp3_input_bytes(const edge_mp3_t* m)
{
  return m ? m->in_bytes : 0;
}

size_t edge_mp3_pcm_bytes(const edge_mp3_t* m)
{
  return m ? m->pcm_bytes : 0;
}

size_t edge_mp3_frames(const edge_mp3_t* m)
{
  return m ? m->frames : 0;
}

int edge_mp3_sample_rate(const edge_mp3_t* m)
{
  return m ? m->sample_rate : 0;
}

int edge_mp3_channels(const edge_mp3_t* m)
{
  return m ? m->channels : 0;
}
