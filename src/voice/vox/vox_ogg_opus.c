/*
 * vox_ogg_opus.c -- Ogg page framing stripped to Opus packets.
 *
 * DESIGN NOTE (read before changing anything here)
 *
 * The first version of this parser assembled a packet inside feed() and
 * returned as soon as it was complete.  That loses it: the caller can only
 * observe state through vox_ogg_next_packet() after feed() has returned, and
 * by then feed() has already consumed the rest of the buffer and overwritten
 * the packet area.  Two packets in one page both vanished, and so did anything
 * that followed a multi-segment packet.  The tests caught it; patching it would
 * have meant tracking "one packet pending" plus "how much of it is left" plus
 * "is the caller mid-drain", which is three more quantities that can disagree.
 *
 * So the parser is a pull parser instead:
 *
 *   feed()          only moves bytes into a buffer
 *   next_packet()   parses as far as it can, returns ONE packet, and remembers
 *                   exactly where it stopped
 *
 * One cursor, one place that advances.  A packet survives until the next call
 * returns a different one, which is what a caller expects from a decode loop.
 *
 * The byte layout follows main/audio/demuxer/ogg_demuxer.cc in
 * 78/xiaozhi-esp32: "OggS" capture, version 0 at byte 4, segment count at byte
 * 26, segment lengths below 255 terminating a packet.  Two behaviours from that
 * file are reproduced deliberately:
 *
 *   - a packet seen before OpusHead is dropped, not decoded (its warning at
 *     ogg_demuxer.cc:362), because handing it to the decoder produces noise;
 *   - Opus is decoded at 48 kHz regardless of the rate in OpusHead, which that
 *     file notes is informational (ogg_demuxer.cc:315-317).
 */

#include "vox_ogg_opus.h"

#include <string.h>

#define OGG_HDR_LEN   27
#define OGG_OFF_VER   4
#define OGG_OFF_SEGS  26

/* Bytes staged but not yet parsed. */
typedef struct
{
  uint8_t buf[VOX_OGG_BUF];
  size_t  len;
  size_t  pos; /* how far next_packet() has consumed */

  /* State carried between calls so parsing can resume. */
  int  in_page;
  int  hdr_got;
  int  seg_count;
  int  seg_got;
  int  seg_index;
  int  want_header_bytes;
  int  header_filled;
  int  pkt_len;

  uint32_t packets;
  uint32_t pages;
  uint32_t dropped;
} parser_t;

static parser_t P;

static uint32_t rd32le(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

void vox_ogg_init(vox_ogg_t *d)
{
  if (d != NULL)
    {
      memset(d, 0, sizeof(*d));
      d->channels   = VOX_OPUS_CHANNELS;
      d->input_rate = VOX_OPUS_SAMPLE_RATE;
    }

  memset(&P, 0, sizeof(P));
}

void vox_ogg_feed(vox_ogg_t *d, const uint8_t *data, size_t len)
{
  size_t room;

  /* The parser state lives in its own storage, so this only needs the argument
   * to satisfy the published signature. */
  (void)d;

  if (data == NULL || len == 0)
    {
      return;
    }

  room = sizeof(P.buf) - P.len;

  if (len > room)
    {
      len = room;
    }

  memcpy(P.buf + P.len, data, len);
  P.len += len;
}


bool vox_ogg_next_packet(vox_ogg_t *d, const uint8_t **pkt, size_t *pkt_len)
{
  if (d == NULL || pkt == NULL || pkt_len == NULL)
    {
      return false;
    }

  for (;;)
    {
      /* Drop fully consumed bytes so the buffer does not creep forward over a
       * long stream. */
      if (P.pos > 0 && P.pos == P.len)
        {
          P.len = 0;
          P.pos = 0;
        }

      if (!P.in_page)
        {
          /* Scan forward for the capture pattern. */
          size_t i = P.pos;

          while (i + 4 <= P.len)
            {
              if (memcmp(P.buf + i, "OggS", 4) == 0)
                {
                  break;
                }

              i++;
            }

          P.pos = i;

          if (P.pos + 4 > P.len)
            {
              return false; /* need more to decide */
            }

          P.in_page     = 1;
          P.hdr_got     = 0;
          P.header_filled = 0;
          P.seg_got     = 0;
          P.seg_index   = 0;
          P.pkt_len     = 0;
          continue;
        }

      /* ---- page header ---- */
      if (P.header_filled < OGG_HDR_LEN)
        {
          int take = OGG_HDR_LEN - P.header_filled;

          if (P.len - P.pos < (size_t)take)
            {
              return false;
            }

          memcpy(d->header + P.header_filled, P.buf + P.pos, (size_t)take);
          P.pos          += (size_t)take;
          P.header_filled += take;

          if (P.header_filled < OGG_HDR_LEN)
            {
              continue;
            }
        }

      /* ogg_demuxer.cc:193 rejects any version byte other than 0. */
      if (d->header[OGG_OFF_VER] != 0)
        {
          d->error = true;
          P.in_page = 0;
          continue;
        }

      P.seg_count = d->header[OGG_OFF_SEGS];

      if (P.seg_count == 0)
        {
          /* A page with no segments carries nothing. */
          P.in_page = 0;
          P.pages++;
          continue;
        }

      /* ---- segment table ---- */
      if (P.seg_got < P.seg_count)
        {
          int take = P.seg_count - P.seg_got;

          if (P.len - P.pos < (size_t)take)
            {
              return false;
            }

          memcpy(d->seg_table + P.seg_got, P.buf + P.pos, (size_t)take);
          P.pos    += (size_t)take;
          P.seg_got += take;
        }

      /* ---- segment payloads ---- */
      while (P.seg_index < P.seg_count)
        {
          int seg_len = d->seg_table[P.seg_index];

          if (seg_len > 0)
            {
              if (P.pkt_len + seg_len > VOX_OPUS_MAX_PACKET)
                {
                  /* A length table that overruns the packet buffer is corrupt.
                   * Skip the rest of this page rather than stalling on it. */
                  P.pkt_len = 0;
                  P.dropped++;
                  P.seg_index = P.seg_count;
                  break;
                }

              if (P.len - P.pos < (size_t)seg_len)
                {
                  return false; /* wait for the rest of this segment */
                }

              memcpy(d->packet + P.pkt_len, P.buf + P.pos, (size_t)seg_len);
              P.pos     += (size_t)seg_len;
              P.pkt_len += seg_len;
            }

          P.seg_index++;

          if (seg_len < 255)
            {
              /* End of a packet. */
              int complete = (P.pkt_len > 0);
              int is_head  = 0;

              if (complete)
                {
                  /* OpusHead is the identification header; OpusTags precedes the
                   * audio.  Everything before them is dropped, matching the
                   * reference's "dropping Ogg packet before OpusHead". */
                  if (P.pkt_len >= 8 &&
                      memcmp(d->packet, "OpusHead", 8) == 0)
                    {
                      is_head = 1;

                      if (P.pkt_len >= 12)
                        {
                          d->channels   = d->packet[9];
                          /* OpusHead layout: "OpusHead"(8) version(1) channels(1)
                           pre-skip(2) input sample rate(4) */
                          d->input_rate = rd32le(d->packet + 12);

                          /* Opus always runs internally at 48 kHz; the rate in
                           * the header is informational (ogg_demuxer.cc:315). */
                          if (d->channels == 0 || d->channels > 2)
                            {
                              d->error = true;
                            }
                          else
                            {
                              d->have_head = true;
                            }
                        }

                      P.pkt_len = 0;
                      P.dropped++;
                    }
                  else if (!d->have_head)
                    {
                      P.pkt_len = 0;
                      P.dropped++;
                    }
                  else if (P.pkt_len >= 8 &&
                           memcmp(d->packet, "OpusTags", 8) == 0)
                    {
                      d->saw_tags = true;
                      P.pkt_len   = 0;
                      P.dropped++;
                    }
                }

              if (complete && !is_head && d->have_head)
                {
                  /* Hand the packet over and stop, so the caller can consume it
                   * before anything overwrites the buffer. */
                  *pkt     = d->packet;
                  *pkt_len = (size_t)P.pkt_len;
                  P.packets++;
                  P.pkt_len = 0;
                  return true;
                }

              P.pkt_len = 0;
              continue;
            }

          /* seg_len == 255: the packet continues into the next segment. */
        }

      /* Page finished. */
      P.pages++;
      P.in_page = 0;
    }
}

bool vox_ogg_ready(const vox_ogg_t *d)
{
  return d != NULL && d->have_head && !d->error;
}

uint32_t vox_ogg_packets(const vox_ogg_t *d)
{
  (void)d;
  return P.packets;
}

uint32_t vox_ogg_pages(const vox_ogg_t *d)
{
  (void)d;
  return P.pages;
}

uint32_t vox_ogg_dropped(const vox_ogg_t *d)
{
  (void)d;
  return P.dropped;
}