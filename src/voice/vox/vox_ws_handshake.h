/*
 * vox_ws_handshake.h -- the WebSocket opening handshake, with no TLS.
 *
 * WHY THIS IS SEPARATE FROM THE TRANSPORT
 *
 * The transport needs TLS, entropy and a socket.  None of those can be
 * exercised on a build host, and the handshake is exactly where a mistake is
 * silent: a wrong Sec-WebSocket-Accept does not produce an error message, it
 * produces a 400 from the server and a log line that says "upgrade failed".
 *
 * ai_vox's transport (esp_websocket_client) validates the accept value.  The
 * existing WebSocket code in this project (voice/volc_asr.c) does NOT -- it
 * sends the key and then only checks for "101".  That works against a lenient
 * peer and fails against a strict one, and the failure mode is
 * indistinguishable from a network problem.
 *
 * So this module does the whole handshake as pure functions over strings:
 *
 *   vox_ws_build_request()      compose the HTTP upgrade request
 *   vox_ws_parse_response()     read the status line and the headers
 *   vox_ws_expected_accept()    SHA1(key + GUID), base64 -- RFC 6455 5.3
 *   vox_ws_check_accept()       compare the server's value against it
 *
 * SHA-1 and base64 are implemented here rather than pulled from mbedTLS so that
 * the whole thing runs on a host, next to a test with the RFC's own example
 * vector.  That is deliberate: an implementation that can only be exercised
 * against a live server is an implementation whose bugs are found in
 * production.
 *
 * The SHA-1 is only used for the handshake, never for anything security-bearing;
 * RFC 6455 mandates it for the accept value specifically.  TLS provides the
 * actual security.
 */

#ifndef VOX_WS_HANDSHAKE_H
#define VOX_WS_HANDSHAKE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* The magic string from RFC 6455 5.3. */
#define VOX_WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

#define VOX_WS_KEY_RAW_LEN 16
#define VOX_WS_KEY_B64_LEN 24 /* 16 bytes base64-encoded, no padding */
#define VOX_WS_ACCEPT_LEN 28

typedef enum
{
  VOX_WS_HS_OK = 0,
  VOX_WS_HS_BAD_URL,     /* missing scheme, host, or path */
  VOX_WS_HS_BAD_STATUS,  /* not 101 */
  VOX_WS_HS_NO_ACCEPT,   /* server omitted the header */
  VOX_WS_HS_BAD_ACCEPT,  /* header present but wrong */
  VOX_WS_HS_HEADERS_TOO_BIG
} vox_ws_hs_e;

/* Parsed parts of a ws:// or wss:// URL. */
typedef struct
{
  char     host[128];
  char     path[256];
  uint16_t port;
  bool     tls;
} vox_ws_url_t;

/* Splits ws://host:port/path.  The path defaults to "/" and the port to 80/443
 * depending on the scheme, so a caller may pass "ws://host" alone. */
vox_ws_hs_e vox_ws_parse_url(const char *url, vox_ws_url_t *out);

/* base64 of 16 raw bytes into out (24 chars + NUL). */
void vox_ws_encode_key(const uint8_t raw[VOX_WS_KEY_RAW_LEN], char *out,
                       size_t cap);

/* SHA1(key_b64 + GUID) base64-encoded, per RFC 6455 5.3. */
vox_ws_hs_e vox_ws_expected_accept(const char *key_b64, char *out,
                                   size_t cap);

/* Composes the upgrade request.  extra_headers may be NULL; when present each
 * entry must be NUL-terminated and the list is terminated by an empty string.
 * Returns the request length, or 0 if it does not fit. */
size_t vox_ws_build_request(const vox_ws_url_t *u, const char *key_b64,
                            const char *const *extra_headers, char *out,
                            size_t cap);

/* Validates the response.  resp must contain at least the header block; the
 * body is not looked at. */
vox_ws_hs_e vox_ws_check_response(const char *key_b64, const char *resp,
                                  size_t len);

/* For logging: a short name for a status code. */
const char *vox_ws_hs_name(vox_ws_hs_e e);

#ifdef __cplusplus
}
#endif

#endif /* VOX_WS_HANDSHAKE_H */