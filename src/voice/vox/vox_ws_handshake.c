/*
 * vox_ws_handshake.c -- RFC 6455 opening handshake as pure string functions.
 *
 * SHA-1 and base64 are implemented here rather than taken from mbedTLS so that
 * the whole handshake can be tested on a host against the RFC's own example
 * vector.  The SHA-1 below exists only to compute Sec-WebSocket-Accept, which
 * RFC 6455 5.3 specifies as SHA-1 by definition; it carries no security weight
 * of its own, the TLS session does.
 */

#include "vox_ws_handshake.h"

#include <stdio.h>
#include <string.h>

/* Case-insensitive compare, local so this file needs nothing beyond
 * <string.h>.  <strings.h> exists on NuttX (strncasecmp is declared in
 * include/strings.h there) but is not standard C, and pulling it in just for
 * two calls would make the host test and the firmware build diverge. */
static int ci_eq_n(const char *a, const char *b, size_t n)
{
  size_t i;

  for (i = 0; i < n; i++)
    {
      char ca = a[i];
      char cb = b[i];

      if (ca >= 'A' && ca <= 'Z')
        {
          ca = (char)(ca - 'A' + 'a');
        }

      if (cb >= 'A' && cb <= 'Z')
        {
          cb = (char)(cb - 'A' + 'a');
        }

      if (ca != cb)
        {
          return 0;
        }
    }

  return 1;
}

/* ------------------------------------------------------------------ SHA-1 */

/* FIPS 180-1 SHA-1.  Self-contained; the struct form avoids any assumption
 * about the caller's stack alignment beyond uint32_t. */
typedef struct
{
  uint32_t h[5];
  uint64_t total;
  uint8_t  blk[64];
  size_t   blk_len;
} sha1_t;

static uint32_t rol(uint32_t v, unsigned n)
{
  return (v << n) | (v >> (32u - n));
}

static void sha1_compress(sha1_t *s, const uint8_t *p)
{
  uint32_t w[80];
  uint32_t a, b, c, d, e;
  int      i;

  for (i = 0; i < 16; i++)
    {
      w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
             ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }

  for (i = 16; i < 80; i++)
    {
      w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

  a = s->h[0];
  b = s->h[1];
  c = s->h[2];
  d = s->h[3];
  e = s->h[4];

  for (i = 0; i < 80; i++)
    {
      uint32_t f;
      uint32_t k;
      uint32_t t;

      if (i < 20)
        {
          f = (b & c) | ((~b) & d);
          k = 0x5A827999u;
        }
      else if (i < 40)
        {
          f = b ^ c ^ d;
          k = 0x6ED9EBA1u;
        }
      else if (i < 60)
        {
          f = (b & c) | (b & d) | (c & d);
          k = 0x8F1BBCDCu;
        }
      else
        {
          f = b ^ c ^ d;
          k = 0xCA62C1D6u;
        }

      t = rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rol(b, 30);
      b = a;
      a = t;
    }

  s->h[0] += a;
  s->h[1] += b;
  s->h[2] += c;
  s->h[3] += d;
  s->h[4] += e;
}

static void sha1_init(sha1_t *s)
{
  s->h[0] = 0x67452301u;
  s->h[1] = 0xEFCDAB89u;
  s->h[2] = 0x98BADCFEu;
  s->h[3] = 0x10325476u;
  s->h[4] = 0xC3D2E1F0u;
  s->total = 0;
  s->blk_len = 0;
}

static void sha1_update(sha1_t *s, const void *data, size_t len)
{
  const uint8_t *p = (const uint8_t *)data;

  s->total += (uint64_t)len;

  while (len > 0)
    {
      size_t take = 64 - s->blk_len;

      if (take > len)
        {
          take = len;
        }

      memcpy(s->blk + s->blk_len, p, take);
      s->blk_len += take;
      p += take;
      len -= take;

      if (s->blk_len == 64)
        {
          sha1_compress(s, s->blk);
          s->blk_len = 0;
        }
    }
}

static void sha1_final(sha1_t *s, uint8_t out[20])
{
  uint64_t bits = s->total * 8u;
  uint8_t  pad  = 0x80;
  uint8_t  zero = 0x00;
  uint8_t  lenb[8];
  int      i;

  sha1_update(s, &pad, 1);

  while (s->blk_len != 56)
    {
      sha1_update(s, &zero, 1);
    }

  for (i = 0; i < 8; i++)
    {
      lenb[i] = (uint8_t)((bits >> (56 - 8 * i)) & 0xFF);
    }

  /* Append the length directly: sha1_update would add it to the count. */
  memcpy(s->blk + 56, lenb, 8);
  sha1_compress(s, s->blk);

  for (i = 0; i < 20; i++)
    {
      out[i] = (uint8_t)((s->h[i / 4] >> (24 - 8 * (i % 4))) & 0xFF);
    }
}

/* ---------------------------------------------------------------- base64 */

static const char B64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_encode(const uint8_t *in, size_t inlen, char *out,
                         size_t cap)
{
  size_t o = 0;
  size_t i;

  for (i = 0; i + 2 < inlen; i += 3)
    {
      uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) |
                   (uint32_t)in[i + 2];

      if (o + 4 >= cap)
        {
          return 0; /* 4 chars plus a NUL must fit */
        }

      out[o++] = B64[(v >> 18) & 0x3F];
      out[o++] = B64[(v >> 12) & 0x3F];
      out[o++] = B64[(v >> 6) & 0x3F];
      out[o++] = B64[v & 0x3F];
    }

  if (i < inlen)
    {
      uint32_t v = (uint32_t)in[i] << 16;
      bool     two = (i + 1 < inlen);

      if (two)
        {
          v |= (uint32_t)in[i + 1] << 8;
        }

      if (o + 4 >= cap)
        {
          return 0; /* 4 chars plus a NUL must fit */
        }

      out[o++] = B64[(v >> 18) & 0x3F];
      out[o++] = B64[(v >> 12) & 0x3F];
      out[o++] = two ? B64[(v >> 6) & 0x3F] : '=';
      out[o++] = '=';
    }

  if (o < cap)
    {
      out[o] = '\0';
    }

  return o;
}

/* -------------------------------------------------------------- URL parse */

vox_ws_hs_e vox_ws_parse_url(const char *url, vox_ws_url_t *out)
{
  const char *p;
  const char *host_begin;
  const char *host_end;
  const char *path;
  size_t      host_len;

  if (url == NULL || out == NULL)
    {
      return VOX_WS_HS_BAD_URL;
    }

  memset(out, 0, sizeof(*out));

  if (strncmp(url, "wss://", 6) == 0)
    {
      out->tls  = true;
      out->port = 443;
      p         = url + 6;
    }
  else if (strncmp(url, "ws://", 5) == 0)
    {
      out->tls  = false;
      out->port = 80;
      p         = url + 5;
    }
  else
    {
      return VOX_WS_HS_BAD_URL;
    }

  /* Credentials in the URL are not supported; rejecting loudly beats sending
   * them to a host that may not be the one the user intended. */
  host_begin = p;
  host_end   = host_begin;

  while (*host_end != '\0' && *host_end != '/' && *host_end != ':')
    {
      host_end++;
    }

  host_len = (size_t)(host_end - host_begin);

  if (host_len == 0 || host_len >= sizeof(out->host))
    {
      return VOX_WS_HS_BAD_URL;
    }

  memcpy(out->host, host_begin, host_len);
  out->host[host_len] = '\0';

  p = host_end;

  if (*p == ':')
    {
      int port = 0;

      p++;

      if (*p < '0' || *p > '9')
        {
          return VOX_WS_HS_BAD_URL;
        }

      while (*p >= '0' && *p <= '9')
        {
          port = port * 10 + (*p - '0');
          p++;

          if (port > 65535)
            {
              return VOX_WS_HS_BAD_URL;
            }
        }

      if (port <= 0)
        {
          return VOX_WS_HS_BAD_URL;
        }

      out->port = (uint16_t)port;
    }

  path = (*p == '/') ? p : "/";
  host_len = strlen(path);

  if (host_len >= sizeof(out->path))
    {
      return VOX_WS_HS_BAD_URL;
    }

  snprintf(out->path, sizeof(out->path), "%s", path);
  return VOX_WS_HS_OK;
}

/* ---------------------------------------------------------------- accept */

void vox_ws_encode_key(const uint8_t raw[VOX_WS_KEY_RAW_LEN], char *out,
                       size_t cap)
{
  size_t n;

  if (out == NULL || cap == 0)
    {
      return;
    }

  out[0] = '\0';
  n = b64_encode(raw, VOX_WS_KEY_RAW_LEN, out, cap);

  /* 16 bytes is 1 mod 3, so a textbook encoder appends two '='.  RFC 6455 5.3
   * asks for "a base64-encoded 16-byte value" without padding, and servers
   * compare the digest of whatever they receive, so the padded form also
   * works.  It is stripped here because that is the conventional form and it
   * keeps the value at exactly 22 characters. */
  while (n > 0 && out[n - 1] == '=')
    {
      n--;
    }

  out[n] = '\0';
}

vox_ws_hs_e vox_ws_expected_accept(const char *key_b64, char *out, size_t cap)
{
  sha1_t   s;
  uint8_t  digest[20];
  char     joined[128];
  int      n;

  if (key_b64 == NULL || out == NULL)
    {
      return VOX_WS_HS_BAD_ACCEPT;
    }

  n = snprintf(joined, sizeof(joined), "%s%s", key_b64, VOX_WS_GUID);

  if (n <= 0 || (size_t)n >= sizeof(joined))
    {
      return VOX_WS_HS_BAD_ACCEPT;
    }

  sha1_init(&s);
  sha1_update(&s, joined, (size_t)n);
  sha1_final(&s, digest);

  if (b64_encode(digest, sizeof(digest), out, cap) == 0)
    {
      return VOX_WS_HS_BAD_ACCEPT;
    }

  return VOX_WS_HS_OK;
}

/* ---------------------------------------------------------------- request */

size_t vox_ws_build_request(const vox_ws_url_t *u, const char *key_b64,
                            const char *const *extra_headers, char *out,
                            size_t cap)
{
  int    n;
  size_t total;

  if (u == NULL || key_b64 == NULL || out == NULL || cap == 0)
    {
      return 0;
    }

  n = snprintf(out, cap,
               "GET %s HTTP/1.1\r\n"
               "Host: %s:%u\r\n"
               "Upgrade: websocket\r\n"
               "Connection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\n"
               "Sec-WebSocket-Version: 13\r\n",
               u->path, u->host, (unsigned)u->port, key_b64);

  if (n <= 0 || (size_t)n >= cap)
    {
      return 0;
    }

  total = (size_t)n;

  if (extra_headers != NULL)
    {
      size_t i;

      for (i = 0; extra_headers[i] != NULL && extra_headers[i][0] != '\0'; i++)
        {
          n = snprintf(out + total, cap - total, "%s\r\n",
                       extra_headers[i]);

          if (n <= 0 || (size_t)n >= cap - total)
            {
              return 0;
            }

          total += (size_t)n;
        }
    }

  if (total + 2 > cap)
    {
      return 0;
    }

  out[total++] = '\r';
  out[total++] = '\n';
  out[total]   = '\0';

  return total;
}

/* --------------------------------------------------------------- response */

/* Case-insensitive search for a header, returning a pointer to its value.
 * Only scans the header block: the caller passes everything up to CRLFCRLF. */
static const char *find_header(const char *resp, size_t len, const char *name,
                               size_t *val_len)
{
  size_t name_len = strlen(name);
  size_t i;
  size_t limit;

  /* Skip the status line. */
  limit = 0;

  while (limit < len && resp[limit] != '\n')
    {
      limit++;
    }

  if (limit >= len)
    {
      return NULL;
    }

  i = limit;

  while (i < len)
    {
      size_t line_end = i;

      while (line_end < len && resp[line_end] != '\n')
        {
          line_end++;
        }

      if ((line_end - i) > name_len &&
          ci_eq_n(resp + i, name, name_len) &&
          resp[i + name_len] == ':')
        {
          size_t v = i + name_len + 1;

          while (v < line_end && (resp[v] == ' ' || resp[v] == '\t'))
            {
              v++;
            }

          /* Trim the CR and any trailing spaces. */
          while (line_end > v &&
                 (resp[line_end - 1] == '\r' || resp[line_end - 1] == ' ' ||
                  resp[line_end - 1] == '\t'))
            {
              line_end--;
            }

          *val_len = line_end - v;
          return resp + v;
        }

      i = (line_end < len) ? line_end + 1 : len;
    }

  return NULL;
}

vox_ws_hs_e vox_ws_check_response(const char *key_b64, const char *resp,
                                  size_t len)
{
  char         expect[VOX_WS_ACCEPT_LEN + 1];
  const char  *accept;
  size_t       accept_len;
  int          status = 0;

  if (key_b64 == NULL || resp == NULL || len < 12)
    {
      return VOX_WS_HS_BAD_STATUS;
    }

  if (strncmp(resp, "HTTP/1.", 7) != 0)
    {
      return VOX_WS_HS_BAD_STATUS;
    }

  /* "HTTP/1.x NNN" -- the reason phrase is irrelevant. */
  if (sscanf(resp, "HTTP/1.%*d %d", &status) != 1)
    {
      return VOX_WS_HS_BAD_STATUS;
    }

  if (status != 101)
    {
      return VOX_WS_HS_BAD_STATUS;
    }

  accept = find_header(resp, len, "Sec-WebSocket-Accept", &accept_len);

  if (accept == NULL)
    {
      /* Strictly a protocol violation, but a peer that omits it is more likely
       * to be lenient than broken; report it distinctly so the log can say so
       * instead of "upgrade failed". */
      return VOX_WS_HS_NO_ACCEPT;
    }

  if (accept_len != (size_t)VOX_WS_ACCEPT_LEN)
    {
      return VOX_WS_HS_BAD_ACCEPT;
    }

  if (vox_ws_expected_accept(key_b64, expect, sizeof(expect)) != VOX_WS_HS_OK)
    {
      return VOX_WS_HS_BAD_ACCEPT;
    }

  if (!ci_eq_n(accept, expect, VOX_WS_ACCEPT_LEN))
    {
      return VOX_WS_HS_BAD_ACCEPT;
    }

  return VOX_WS_HS_OK;
}

const char *vox_ws_hs_name(vox_ws_hs_e e)
{
  switch (e)
    {
      case VOX_WS_HS_OK:             return "ok";
      case VOX_WS_HS_BAD_URL:        return "bad_url";
      case VOX_WS_HS_BAD_STATUS:     return "bad_status";
      case VOX_WS_HS_NO_ACCEPT:      return "no_accept_header";
      case VOX_WS_HS_BAD_ACCEPT:     return "accept_mismatch";
      case VOX_WS_HS_HEADERS_TOO_BIG: return "headers_too_big";
      default:                       return "?";
    }
}