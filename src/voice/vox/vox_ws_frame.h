/*
 * vox_ws_frame.h -- WebSocket frame codec with no I/O.
 *
 * WHY THIS FILE EXISTS
 *
 * ai_vox carries its protocol over one WebSocket; OpenVela's existing
 * WebSocket code (voice/volc_asr.c, voice/volc_tts_ws.c) can send frames but
 * has no frame parser at all -- neither even defines WS_OPCODE_TEXT, which is
 * the opcode a server uses for every JSON message.  Those paths work only
 * because their peers are lenient.  A parser therefore has to be written, and a
 * parser is exactly the kind of code that fails in ways a live conversation
 * will not reveal: a dropped continuation frame, a length field read from
 * payload bytes, a ping that strands the message it landed inside.
 *
 * Keeping the codec free of sockets and TLS makes all of that testable offline.
 *
 * DESIGN -- deliberately not incremental
 *
 * The first version of this decoder kept frames in place and tracked "how far
 * have I consumed" as an offset.  That is the compact approach and it is wrong
 * in a way that only shows up once frames arrive together: the consumed bytes
 * stay in the buffer, so the offset and the buffer length describe different
 * things, and after four rounds of patching the decoder still reported
 * NEED_MORE for a frame that had arrived whole.
 *
 * This version instead copies: bytes arrive into one staging buffer, and each
 * frame is parsed in place from the front of that buffer.  Once a frame is
 * consumed, the remainder is moved to the front and the length updated.  There
 * is exactly one cursor -- the front of the buffer -- so there is no second
 * quantity that can drift out of step.
 *
 * A message being reassembled is kept in a separate output area, not in the
 * staging buffer.  That separation is what makes "a ping arrived in the middle
 * of a fragmented message" fall out for free instead of needing a special case.
 *
 * Buffer ownership: a decoded message points into the decoder's own storage and
 * stays valid until the next vox_ws_dec_feed() or vox_ws_dec_next().  Callers
 * copy what they intend to keep.
 */

#ifndef VOX_WS_FRAME_H
#define VOX_WS_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define VOX_WS_OP_CONT   0x00
#define VOX_WS_OP_TEXT   0x01
#define VOX_WS_OP_BINARY 0x02
#define VOX_WS_OP_CLOSE  0x08
#define VOX_WS_OP_PING   0x09
#define VOX_WS_OP_PONG   0x0A

#define VOX_WS_FIN  0x80
#define VOX_WS_MASK 0x80

/* The largest message this project exchanges is a hello plus an MCP payload.
 * 8 KiB is generous and, more importantly, static: this board's kernel heap has
 * been measured with as little as 119 KiB free, so the decoder must not depend
 * on being able to grow at runtime. */
#ifndef VOX_WS_MAX_MESSAGE
#define VOX_WS_MAX_MESSAGE 8192
#endif

typedef enum
{
  VOX_WS_DEC_NEED_MORE = 0, /* incomplete; feed more bytes and call again */
  VOX_WS_DEC_MESSAGE,       /* a complete text or binary message is ready */
  VOX_WS_DEC_CONTROL,       /* ping/pong/close: reply, then call again */
  VOX_WS_DEC_ERROR          /* protocol violation; the caller must close */
} vox_ws_dec_e;

typedef struct
{
  uint8_t  opcode;    /* TEXT or BINARY */
  uint8_t *data;      /* into the decoder's output area */
  size_t   len;
} vox_ws_msg_t;

typedef struct
{
  /* Staging: bytes received but not yet consumed as whole frames.  The next
   * frame always starts at in[0]. */
  uint8_t  in[VOX_WS_MAX_MESSAGE];
  size_t   in_len;

  /* Output: a message being reassembled across fragments. */
  uint8_t  out[VOX_WS_MAX_MESSAGE];
  size_t   out_len;
  uint8_t  out_opcode;
  bool     assembling;

  /* A control frame's payload, kept until the caller has echoed it. */
  uint8_t  ctrl[128];
  size_t   ctrl_len;
  uint8_t  ctrl_opcode;
} vox_ws_decoder_t;

void vox_ws_dec_init(vox_ws_decoder_t *d);

/* Append received bytes.  Any chunk size works, down to one byte. */
void vox_ws_dec_feed(vox_ws_decoder_t *d, const uint8_t *data, size_t len);

/* Pull the next message or control frame.  See vox_ws_dec_e. */
vox_ws_dec_e vox_ws_dec_next(vox_ws_decoder_t *d, vox_ws_msg_t *msg);

/* Bytes staged but not yet consumed as whole frames, for diagnostics. */
size_t vox_ws_dec_pending(const vox_ws_decoder_t *d);

/* ---- encoding ---- */

/* Bytes an encoded frame of plen will occupy, header included. */
size_t vox_ws_header_len(size_t plen);

/* Write a masked client frame (RFC 6455 5.3 requires clients to mask).
 * mask_key must differ between frames; callers advance it.
 * Returns the frame length, or 0 if it does not fit in out. */
size_t vox_ws_encode(uint8_t *out, size_t out_cap, uint8_t opcode, bool fin,
                     const uint8_t *payload, size_t plen, uint32_t mask_key);

#ifdef __cplusplus
}
#endif

#endif /* VOX_WS_FRAME_H */