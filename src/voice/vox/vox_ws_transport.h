/*
 * vox_ws_transport.h -- WebSocket transport: TLS, handshake, read loop.
 *
 * WHAT THIS ADDS
 *
 * The existing WebSocket code in this project can open a socket and send
 * frames, and nothing else:
 *
 *   voice/volc_asr.c      handshake + send, no read loop, no thread
 *   voice/volc_tts_ws.c   the same
 *   neither of them        defines WS_OPCODE_TEXT, which is the opcode every
 *                          JSON message in the ai_vox protocol arrives with
 *
 * So this file supplies the three missing pieces and wires the two pieces that
 * already exist and are tested:
 *
 *   TLS + upgrade   ->  vox_ws_handshake.h  (pure string functions, tested
 *                       against the RFC 6455 accept vector)
 *   framing         ->  vox_ws_frame.h       (decoder/encoder, 105 tests under
 *                       ASan)
 *   protocol        ->  vox_ws_protocol.h    (state machine and message
 *                       parsing, 54 tests)
 *
 * The design keeps those three free of platform code so all 234 of their tests
 * run on a host.  Only this file knows about sockets, threads and mbedTLS.
 *
 * THREADING
 *
 * One reader thread per transport.  It blocks in the TLS read, feeds bytes to
 * the frame decoder, and hands decoded messages to the protocol session, which
 * invokes the observer.  Writes happen on the caller's thread, guarded by a
 * mutex, because two threads writing to one TLS session interleaves mid-frame.
 *
 * The observer callbacks therefore run ON THE READER THREAD, not the caller's.
 * That is stated here because it is the kind of detail that silently becomes a
 * race: an observer that touches UI state must hand off rather than block.
 *
 * A pong is answered automatically.  Not doing so is what makes some proxies
 * drop an idle "listening" connection after a minute, which presents as the
 * device mysteriously going quiet between turns.
 */

#ifndef VOX_WS_TRANSPORT_H
#define VOX_WS_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vox_ws_frame.h"
#include "vox_ws_protocol.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum
{
  VOX_WS_T_OK = 0,
  VOX_WS_T_ERR_URL,
  VOX_WS_T_ERR_ENTROPY,
  VOX_WS_T_ERR_CONNECT,
  VOX_WS_T_ERR_TLS,
  VOX_WS_T_ERR_HANDSHAKE,
  VOX_WS_T_ERR_THREAD,
  VOX_WS_T_ERR_CLOSED,
  VOX_WS_T_ERR_PROTO
} vox_ws_t_e;

typedef struct vox_ws_transport vox_ws_transport_t;

/* Called by the reader thread for each decoded message.  text is NUL-terminated
 * for text frames; for binary frames len bytes at data are the payload. */
typedef void (*vox_ws_on_message_fn)(void *ctx, uint8_t opcode,
                                     const uint8_t *data, size_t len);

/* Called by the reader thread when the connection ends, for any reason,
 * including a protocol error.  Always the last callback for a transport. */
typedef void (*vox_ws_on_closed_fn)(void *ctx, vox_ws_t_e reason);

typedef struct
{
  void                  *ctx;
  vox_ws_on_message_fn   on_message;
  vox_ws_on_closed_fn    on_closed;
  /* Extra request headers, e.g. Authorization.  Each is a full "Name: value"
   * line; the list is terminated by a NULL entry. */
  const char *const    *extra_headers;
  /* Read timeout in seconds for the reader loop.  0 disables the timeout, which
   * is the right setting for a long-lived listening session. */
  int                    recv_timeout_s;
  /* Stack size for the reader thread, in bytes.  0 selects a default. */
  size_t                 reader_stack;
} vox_ws_transport_cfg_t;

/* Creates a transport and starts its reader thread.  On success *out receives a
 * handle that must be released with vox_ws_transport_close().  Returns OK once
 * the handshake has completed and the thread is running. */
vox_ws_t_e vox_ws_transport_open(const char *url,
                                 const vox_ws_transport_cfg_t *cfg,
                                 vox_ws_transport_t **out);

/* Sends a text frame.  Safe to call from any thread. */
vox_ws_t_e vox_ws_transport_send_text(vox_ws_transport_t *t, const char *text);

/* Sends a binary frame.  Safe to call from any thread. */
vox_ws_t_e vox_ws_transport_send_binary(vox_ws_transport_t *t,
                                        const uint8_t *data, size_t len);

/* Closes the connection and stops the reader thread.  Blocks briefly for the
 * thread to exit.  The on_closed callback fires unless the transport was never
 * connected.  The handle is freed. */
void vox_ws_transport_close(vox_ws_transport_t *t);

/* Diagnostics: frames in/out, decode errors, bytes received. */
typedef struct
{
  uint32_t rx_frames;
  uint32_t tx_frames;
  uint32_t rx_bytes;
  uint32_t tx_bytes;
  uint32_t decode_errors;
  uint32_t pongs_sent;
  uint32_t reconnects;
  int64_t  first_msg_ms;  /* -1 until the first message arrives */
} vox_ws_transport_stats_t;

void vox_ws_transport_get_stats(vox_ws_transport_t *t,
                                vox_ws_transport_stats_t *out);

/* A short name for a status, for logs. */
const char *vox_ws_transport_name(vox_ws_t_e e);

#ifdef __cplusplus
}
#endif

#endif /* VOX_WS_TRANSPORT_H */