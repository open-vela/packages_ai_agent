/*
 * vox_ws_frame.c -- WebSocket frame codec.  See vox_ws_frame.h for the design
 * rationale; the short version is that this implementation keeps exactly one
 * cursor (the front of the staging buffer) and copies instead of tracking
 * offsets, because offsets in an in-place parser drift out of step the moment
 * two frames land in one read.
 */

#include "vox_ws_frame.h"

#include <string.h>

void vox_ws_dec_init(vox_ws_decoder_t *d)
{
  if (d != NULL)
    {
      memset(d, 0, sizeof(*d));
    }
}

size_t vox_ws_dec_pending(const vox_ws_decoder_t *d)
{
  return (d == NULL) ? 0u : d->in_len;
}

void vox_ws_dec_feed(vox_ws_decoder_t *d, const uint8_t *data, size_t len)
{
  size_t room;

  if (d == NULL || data == NULL || len == 0)
    {
      return;
    }

  room = sizeof(d->in) - d->in_len;

  if (len > room)
    {
      /* Drop the excess rather than overflow.  Overflowing would corrupt
       * whatever sits next to this struct in .bss; a truncated stream surfaces
       * as a decode failure, which the caller can report and reconnect from. */
      len = room;
    }

  memcpy(d->in + d->in_len, data, len);
  d->in_len += len;
}

static uint16_t rd16(const uint8_t *p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint64_t rd64(const uint8_t *p)
{
  uint64_t v = 0;
  int      i;

  for (i = 0; i < 8; i++)
    {
      v = (v << 8) | (uint64_t)p[i];
    }

  return v;
}

/* Drops the first n bytes of the staging buffer.  This is the only place the
 * cursor moves, which is the whole point: there is no second offset to keep in
 * step with in_len. */
static void consume(vox_ws_decoder_t *d, size_t n)
{
  if (n >= d->in_len)
    {
      d->in_len = 0;
      return;
    }

  memmove(d->in, d->in + n, d->in_len - n);
  d->in_len -= n;
}

vox_ws_dec_e vox_ws_dec_next(vox_ws_decoder_t *d, vox_ws_msg_t *msg)
{
  const uint8_t *p;
  uint8_t        b0;
  uint8_t        b1;
  bool           fin;
  bool           masked;
  uint8_t        opcode;
  uint64_t       plen;
  size_t         hdr;
  size_t         need;
  uint8_t        mask[4] = { 0, 0, 0, 0 };
  uint8_t       *dst;
  size_t         i;

  if (d == NULL || msg == NULL)
    {
      return VOX_WS_DEC_ERROR;
    }

  memset(msg, 0, sizeof(*msg));

  if (d->in_len < 2)
    {
      return VOX_WS_DEC_NEED_MORE;
    }

  p      = d->in;
  b0     = p[0];
  b1     = p[1];
  fin    = (b0 & VOX_WS_FIN) != 0;
  masked = (b1 & VOX_WS_MASK) != 0;
  opcode = (uint8_t)(b0 & 0x0F);
  plen   = (uint64_t)(b1 & 0x7F);
  hdr    = 2;

  if (plen == 126)
    {
      if (d->in_len < 4)
        {
          return VOX_WS_DEC_NEED_MORE;
        }

      plen = rd16(p + 2);
      hdr += 2;
    }
  else if (plen == 127)
    {
      if (d->in_len < 10)
        {
          return VOX_WS_DEC_NEED_MORE;
        }

      plen = rd64(p + 2);
      hdr += 8;
    }

  if (masked)
    {
      hdr += 4;
    }

  /* RFC 6455 5.5: control frames are <=125 bytes and never fragmented. */
  if ((opcode & 0x08) != 0 && (plen > 125 || !fin))
    {
      return VOX_WS_DEC_ERROR;
    }

  /* Refuse an implausible length instead of waiting for bytes that will never
   * arrive.  Both buffers are static and bounded, so a large length is a
   * protocol error, not something to make room for. */
  if (plen > (uint64_t)VOX_WS_MAX_MESSAGE)
    {
      return VOX_WS_DEC_ERROR;
    }

  /* Does the whole frame fit in what has arrived? */
  need = hdr + (size_t)plen;

  if (d->in_len < need)
    {
      return VOX_WS_DEC_NEED_MORE;
    }

  /* A data frame must also fit in the output area. */
  if ((opcode & 0x08) == 0 && !d->assembling &&
      (size_t)plen > sizeof(d->out))
    {
      return VOX_WS_DEC_ERROR;
    }

  if (masked)
    {
      memcpy(mask, p + hdr - 4, 4);
    }

  /* ---- control frame: hand the payload over, leave the message alone ---- */
  if ((opcode & 0x08) != 0)
    {
      size_t n = (size_t)plen;

      for (i = 0; i < n; i++)
        {
          d->ctrl[i] = masked ? (uint8_t)(p[hdr + i] ^ mask[i & 3])
                              : p[hdr + i];
        }

      d->ctrl_len    = n;
      d->ctrl_opcode = opcode;

      consume(d, need);

      msg->opcode = opcode;
      msg->data   = d->ctrl;
      msg->len    = n;
      return VOX_WS_DEC_CONTROL;
    }

  /* ---- data frame ---- */
  if (opcode != VOX_WS_OP_CONT)
    {
      if (d->assembling)
        {
          /* A new message while one is in progress violates the spec. */
          return VOX_WS_DEC_ERROR;
        }

      d->out_opcode = opcode;
      d->out_len    = 0;
      d->assembling = true;
    }
  else if (!d->assembling)
    {
      /* A continuation with nothing started. */
      return VOX_WS_DEC_ERROR;
    }

  if (d->out_len + (size_t)plen > sizeof(d->out))
    {
      return VOX_WS_DEC_ERROR;
    }

  dst = d->out + d->out_len;

  for (i = 0; i < (size_t)plen; i++)
    {
      dst[i] = masked ? (uint8_t)(p[hdr + i] ^ mask[i & 3]) : p[hdr + i];
    }

  d->out_len += (size_t)plen;
  consume(d, need);

  if (!fin)
    {
      return VOX_WS_DEC_NEED_MORE;
    }

  msg->opcode = d->out_opcode;
  msg->data   = d->out;
  msg->len    = d->out_len;

  d->assembling = false;
  d->out_len    = 0;

  return VOX_WS_DEC_MESSAGE;
}

/* ---- encoding ---- */

size_t vox_ws_header_len(size_t plen)
{
  if (plen < 126)
    {
      return 2 + 4;
    }

  if (plen <= 0xFFFF)
    {
      return 2 + 2 + 4;
    }

  return 2 + 8 + 4;
}

size_t vox_ws_encode(uint8_t *out, size_t out_cap, uint8_t opcode, bool fin,
                     const uint8_t *payload, size_t plen, uint32_t mask_key)
{
  uint8_t  mask[4];
  size_t   hlen;
  uint8_t *dst;
  size_t   i;

  if (out == NULL || plen > (size_t)VOX_WS_MAX_MESSAGE)
    {
      return 0;
    }

  hlen = vox_ws_header_len(plen);

  if (hlen + plen > out_cap)
    {
      return 0;
    }

  if (payload == NULL && plen != 0)
    {
      return 0;
    }

  mask[0] = (uint8_t)(mask_key & 0xFF);
  mask[1] = (uint8_t)((mask_key >> 8) & 0xFF);
  mask[2] = (uint8_t)((mask_key >> 16) & 0xFF);
  mask[3] = (uint8_t)((mask_key >> 24) & 0xFF);

  out[0] = (uint8_t)((fin ? VOX_WS_FIN : 0) | (opcode & 0x0F));

  if (plen < 126)
    {
      out[1] = (uint8_t)(VOX_WS_MASK | (uint8_t)plen);
      memcpy(out + 2, mask, 4);
      dst = out + 6;
    }
  else if (plen <= 0xFFFF)
    {
      out[1] = (uint8_t)(VOX_WS_MASK | 126);
      out[2] = (uint8_t)((plen >> 8) & 0xFF);
      out[3] = (uint8_t)(plen & 0xFF);
      memcpy(out + 4, mask, 4);
      dst = out + 8;
    }
  else
    {
      int k;

      out[1] = (uint8_t)(VOX_WS_MASK | 127);

      for (k = 0; k < 8; k++)
        {
          out[2 + k] = (uint8_t)((plen >> (56 - 8 * k)) & 0xFF);
        }

      memcpy(out + 10, mask, 4);
      dst = out + 14;
    }

  /* RFC 6455 5.3: client frames MUST be masked. */
  for (i = 0; i < plen; i++)
    {
      dst[i] = (uint8_t)(payload[i] ^ mask[i & 3]);
    }

  return hlen + plen;
}