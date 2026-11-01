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
 * Doubao bigmodel streaming ASR via WebSocket (V3 protocol).
 *
 * Protocol: wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async
 * Auth: HTTP headers on the WebSocket upgrade request:
 *   X-Api-Key + X-Api-Resource-Id + X-Api-Request-Id   (new console)
 *   X-Api-App-Key + X-Api-Access-Key                   (legacy console)
 * Flow:
 *   1. TLS connect + HTTP Upgrade to WebSocket
 *   2. Send full_client_request (gzip'd JSON metadata)
 *   3. Send audio_only chunks (gzip'd PCM), last chunk with negated seq
 *   4. Receive full_server_response frames, extract result text
 *
 * Volcengine V3 binary frame (carried inside standard WebSocket frames):
 *   Byte 0: [protocol_version:4][header_size:4]  = 0x11
 *   Byte 1: [message_type:4][message_type_flags:4]
 *   Byte 2: [serialization:4][compression:4]
 *   Byte 3: reserved = 0x00
 *   Bytes 4-7: sequence (big-endian int32; negative on the last chunk)
 *   Bytes 8-11: payload size (big-endian uint32)
 *   Bytes 12+: gzip-compressed payload
 */

#include "voice/volc_asr.h"
#include "infra/config_store.h"
#include "infra/http_proxy.h"
#include "agent_compat.h"
#include "agent_config.h"
#include "voice/voice_asr.h"

#include "cJSON.h"
#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "netutils/base64.h"
#include <zlib.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

static const char* TAG = "volc_asr";

/* Volcengine V3 binary protocol constants */
#define VOLC_PROTO_VER 0x11 /* version=1, header_size=1 (x4=4B) */
#define VOLC_MSG_FULL_REQ 0x1 /* full_client_request */
#define VOLC_MSG_AUDIO 0x2 /* audio_only_request */
#define VOLC_MSG_FULL_RESP 0x9 /* full_server_response */
#define VOLC_MSG_ERROR 0xF /* server error */
#define VOLC_FLAG_POS_SEQ 0x1 /* positive sequence present */
#define VOLC_FLAG_NEG_WITH_SEQ 0x3 /* last packet: negative sequence */
#define VOLC_FLAG_EVENT 0x4 /* event field present */
#define VOLC_FLAG_LAST 0x2 /* is_last_package */
#define VOLC_SER_JSON 0x1 /* JSON serialization */
#define VOLC_COMP_GZIP 0x1 /* gzip compression */
#define VOLC_FRAME_HDR 12 /* 4B header + 4B sequence + 4B payload len */

#define WS_BUF_SIZE 4096
#define WS_MASK_KEY_LEN 4
#define WS_OPCODE_BINARY 0x02
#define WS_OPCODE_CLOSE 0x08
#define WS_OPCODE_PING   0x09
#define WS_OPCODE_PONG   0x0A
#define WS_FIN_BIT 0x80
#define WS_MASK_BIT 0x80

#define ASR_UUID_LEN 37 /* 36 chars + NUL */

/* TLS context for ASR connection (not pooled — single use) */
typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config cfg;
    mbedtls_net_context net;
    mbedtls_ctr_drbg_context ctr_drbg;
} asr_tls_ctx_t;

/* Session auth, filled from the config store by volc_asr_init() */
typedef struct {
    char api_key[256]; /* new console: X-Api-Key */
    char app_key[128]; /* legacy console: X-Api-App-Key */
    char access_key[256]; /* legacy console: X-Api-Access-Key */
    char resource_id[128]; /* X-Api-Resource-Id */
} asr_auth_t;

/* ── gzip (RFC 1952) helpers, used by the V3 framing ────────── */

static int gzip_compress(const unsigned char* in, size_t inlen,
    unsigned char** out, size_t* outlen)
{
    z_stream strm;
    memset(&strm, 0, sizeof(strm));

    /* windowBits 31 = 15 + 16: deflate with a gzip container */
    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
            15 + 16, 8, Z_DEFAULT_STRATEGY)
        != Z_OK) {
        return -EIO;
    }

    /* A few spare bytes on top of the bound: a zero length input still
     * produces a full gzip member, and the bound for that case is tight
     * enough to be worth not relying on. */
    size_t bound = deflateBound(&strm, inlen) + 32;
    unsigned char* buf = malloc(bound);

    if (!buf) {
        deflateEnd(&strm);
        return -ENOMEM;
    }

    strm.next_in = (unsigned char*)in;
    strm.avail_in = (uInt)inlen;
    strm.next_out = buf;
    strm.avail_out = (uInt)bound;

    int zret = deflate(&strm, Z_FINISH);
    size_t produced = (size_t)strm.total_out;

    deflateEnd(&strm);

    if (zret != Z_STREAM_END) {
        free(buf);
        return -EIO;
    }

    *out = buf;
    *outlen = produced;
    return 0;
}

static int gzip_decompress(const unsigned char* in, size_t inlen,
    unsigned char** out, size_t* outlen)
{
    z_stream strm;
    memset(&strm, 0, sizeof(strm));

    if (inflateInit2(&strm, 15 + 16) != Z_OK) {
        return -EIO;
    }

    size_t cap = 8192;
    unsigned char* buf = malloc(cap);

    if (!buf) {
        inflateEnd(&strm);
        return -ENOMEM;
    }

    strm.next_in = (unsigned char*)in;
    strm.avail_in = (uInt)inlen;
    strm.next_out = buf;
    strm.avail_out = (uInt)cap;

    int zret;

    while ((zret = inflate(&strm, Z_NO_FLUSH)) == Z_OK) {
        if (strm.avail_out == 0) {
            /* Grow the output buffer */
            if (cap >= 256 * 1024) {
                free(buf);
                inflateEnd(&strm);
                return -EOVERFLOW;
            }
            size_t ncap = cap * 2;
            unsigned char* nbuf = realloc(buf, ncap);

            if (!nbuf) {
                free(buf);
                inflateEnd(&strm);
                return -ENOMEM;
            }
            buf = nbuf;
            strm.next_out = buf + cap;
            strm.avail_out = (uInt)(ncap - cap);
            cap = ncap;
        }
    }

    size_t produced = (size_t)strm.total_out;

    inflateEnd(&strm);

    if (zret != Z_STREAM_END) {
        free(buf);
        return -EPROTO;
    }

    *out = buf;
    *outlen = produced;
    return 0;
}

/* ── Entropy source ──────────────────────────────────────────── */

static int asr_entropy_func(void* data, unsigned char* output, size_t len)
{
    (void)data;
    if (agent_secure_random(output, len) == 0) {
        return 0;
    }
    syslog(LOG_ERR, "[volc_asr] CRITICAL: No secure entropy source available\n");
    return -1;  /* Generic error - TLS handshake will fail safely */
}

/* ── TLS connect / free ──────────────────────────────────────── */

static int asr_tls_connect(asr_tls_ctx_t* ctx,
    const char* host, const char* port)
{
    int ret;

    mbedtls_ssl_init(&ctx->ssl);
    mbedtls_ssl_config_init(&ctx->cfg);
    mbedtls_net_init(&ctx->net);
    mbedtls_ctr_drbg_init(&ctx->ctr_drbg);

    const char* pers = "volc_asr";

    ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, asr_entropy_func,
        NULL, (const unsigned char*)pers,
        strlen(pers));
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] ctr_drbg_seed: -0x%04x\n", TAG, -ret);
        return -EIO;
    }

    /* If proxy is enabled, open a CONNECT tunnel first */
    if (http_proxy_is_enabled()) {
        int port_num = atoi(port);
        int tunnel_fd = proxy_open_tunnel(host, port_num, 30000);

        if (tunnel_fd < 0) {
            syslog(LOG_ERR, "[%s] proxy tunnel failed\n", TAG);
            return -ECONNREFUSED;
        }

        ctx->net.fd = tunnel_fd;
        syslog(LOG_INFO, "[%s] Using proxy tunnel fd=%d for %s:%s\n",
            TAG, tunnel_fd, host, port);
    } else {
        ret = mbedtls_net_connect(&ctx->net, host, port,
            MBEDTLS_NET_PROTO_TCP);
        if (ret != 0) {
            syslog(LOG_ERR, "[%s] net_connect %s:%s: -0x%04x\n",
                TAG, host, port, -ret);
            return -ECONNREFUSED;
        }
    }

    mbedtls_net_set_block(&ctx->net);
    if (ctx->net.fd >= 0) {
        struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };

        setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO,
            &tv, sizeof(tv));
    }

    ret = mbedtls_ssl_config_defaults(&ctx->cfg,
        MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        return -EIO;
    }

    mbedtls_ssl_conf_min_tls_version(&ctx->cfg,
        MBEDTLS_SSL_VERSION_TLS1_2);
#if defined(MBEDTLS_SSL_PROTO_TLS1_3)
    mbedtls_ssl_conf_max_tls_version(&ctx->cfg,
        MBEDTLS_SSL_VERSION_TLS1_3);
#else
    mbedtls_ssl_conf_max_tls_version(&ctx->cfg,
        MBEDTLS_SSL_VERSION_TLS1_2);
#endif

#if defined(MBEDTLS_SSL_ALPN)
    static const char* alpn[] = { "http/1.1", NULL };

    mbedtls_ssl_conf_alpn_protocols(&ctx->cfg, alpn);
#endif

    mbedtls_ssl_conf_authmode(&ctx->cfg, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_rng(&ctx->cfg, mbedtls_ctr_drbg_random,
        &ctx->ctr_drbg);

    ret = mbedtls_ssl_setup(&ctx->ssl, &ctx->cfg);
    if (ret != 0) {
        return -EIO;
    }

    mbedtls_ssl_set_hostname(&ctx->ssl, host);
    mbedtls_ssl_set_bio(&ctx->ssl, &ctx->net,
        mbedtls_net_send, mbedtls_net_recv, NULL);

    while ((ret = mbedtls_ssl_handshake(&ctx->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ
            && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            syslog(LOG_ERR, "[%s] handshake: -0x%04x\n", TAG, -ret);
            return -EIO;
        }
    }

    syslog(LOG_INFO, "[%s] TLS connected to %s:%s\n", TAG, host, port);
    return 0;
}

static void asr_tls_free(asr_tls_ctx_t* ctx)
{
    mbedtls_ssl_close_notify(&ctx->ssl);
    mbedtls_net_free(&ctx->net);
    mbedtls_ssl_free(&ctx->ssl);
    mbedtls_ssl_config_free(&ctx->cfg);
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
}

/* ── Low-level TLS I/O helpers ───────────────────────────────── */

static int tls_write_all(asr_tls_ctx_t* ctx,
    const unsigned char* buf, size_t len)
{
    size_t written = 0;

    while (written < len) {
        int ret = mbedtls_ssl_write(&ctx->ssl, buf + written,
            len - written);

        if (ret > 0) {
            written += (size_t)ret;
        } else if (ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            syslog(LOG_ERR, "[%s] ssl_write: -0x%04x\n", TAG, -ret);
            return -EIO;
        }
    }
    return 0;
}

static int tls_read_all(asr_tls_ctx_t* ctx,
    unsigned char* buf, size_t len)
{
    size_t got = 0;

    while (got < len) {
        int ret = mbedtls_ssl_read(&ctx->ssl, buf + got, len - got);

        if (ret > 0) {
            got += (size_t)ret;
        } else if (ret == 0
            || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return -ECONNRESET;
        } else if (ret != MBEDTLS_ERR_SSL_WANT_READ) {
            syslog(LOG_ERR, "[%s] ssl_read: -0x%04x\n", TAG, -ret);
            return -EIO;
        }
    }
    return 0;
}

/* ── UUID generation (simple v4-like) ────────────────────────── */

static void generate_uuid(char* out, size_t cap)
{
    unsigned char rnd[16];

    asr_entropy_func(NULL, rnd, sizeof(rnd));
    rnd[6] = (rnd[6] & 0x0F) | 0x40; /* version 4 */
    rnd[8] = (rnd[8] & 0x3F) | 0x80; /* variant 1 */
    snprintf(out, cap,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-"
        "%02x%02x-%02x%02x%02x%02x%02x%02x",
        rnd[0], rnd[1], rnd[2], rnd[3],
        rnd[4], rnd[5], rnd[6], rnd[7],
        rnd[8], rnd[9], rnd[10], rnd[11],
        rnd[12], rnd[13], rnd[14], rnd[15]);
}

/* ── WebSocket upgrade handshake ─────────────────────────────── */

static int ws_upgrade(asr_tls_ctx_t* ctx, const char* host,
    const char* path, const asr_auth_t* auth)
{
    /* Generate random 16-byte key, base64-encode it */
    unsigned char key_raw[16];
    unsigned char key_b64[32];
    size_t key_b64_len;

    asr_entropy_func(NULL, key_raw, sizeof(key_raw));
    mbedtls_base64_encode(key_b64, sizeof(key_b64), &key_b64_len,
        key_raw, sizeof(key_raw));

    char reqid[ASR_UUID_LEN];
    generate_uuid(reqid, sizeof(reqid));

    /* V3 auth: X-Api-Key (new console) or X-Api-App-Key +
     * X-Api-Access-Key (legacy console).  X-Api-Resource-Id selects
     * the model version, X-Api-Request-Id identifies this session. */
    char auth_hdr[768];

    if (auth->api_key[0] != '\0') {
        snprintf(auth_hdr, sizeof(auth_hdr),
            "X-Api-Key: %s\r\n"
            "X-Api-Resource-Id: %s\r\n"
            "X-Api-Request-Id: %s\r\n",
            auth->api_key, auth->resource_id, reqid);
    } else {
        snprintf(auth_hdr, sizeof(auth_hdr),
            "X-Api-App-Key: %s\r\n"
            "X-Api-Access-Key: %s\r\n"
            "X-Api-Resource-Id: %s\r\n"
            "X-Api-Request-Id: %s\r\n",
            auth->app_key, auth->access_key,
            auth->resource_id, reqid);
    }

    char req[1280];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %.*s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "%s"
        "\r\n",
        path, host, (int)key_b64_len, key_b64,
        auth_hdr);

    if (n <= 0 || n >= (int)sizeof(req)) {
        return -EOVERFLOW;
    }

    syslog(LOG_INFO, "[%s] WS upgrade: reqid=%s resource=%s\n",
        TAG, reqid, auth->resource_id);

    int ret = tls_write_all(ctx, (const unsigned char*)req, (size_t)n);

    if (ret != 0) {
        return ret;
    }

    /* Read HTTP 101 response */
    char resp[WS_BUF_SIZE];
    size_t rlen = 0;

    while (rlen < sizeof(resp) - 1) {
        int r = mbedtls_ssl_read(&ctx->ssl,
            (unsigned char*)resp + rlen,
            sizeof(resp) - 1 - rlen);

        if (r > 0) {
            rlen += (size_t)r;
            resp[rlen] = '\0';
            if (strstr(resp, "\r\n\r\n")) {
                break;
            }
        } else if (r == 0
            || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return -ECONNRESET;
        } else if (r != MBEDTLS_ERR_SSL_WANT_READ) {
            return -EIO;
        }
    }

    int status = 0;

    if (sscanf(resp, "HTTP/1.1 %d", &status) != 1 || status != 101) {
        syslog(LOG_ERR, "[%s] WS upgrade failed: HTTP %d\n",
            TAG, status);
        return -EPROTO;
    }

    syslog(LOG_INFO, "[%s] WebSocket upgrade OK\n", TAG);
    return 0;
}

/* ── WebSocket frame send (client must mask) ─────────────────── */

static int ws_send_frame(asr_tls_ctx_t* ctx, unsigned char opcode,
    const unsigned char* payload, size_t plen)
{
    unsigned char hdr[14]; /* max WS header: 2 + 8 + 4 */
    size_t hdr_len = 0;

    hdr[0] = WS_FIN_BIT | opcode;

    if (plen < 126) {
        hdr[1] = WS_MASK_BIT | (unsigned char)plen;
        hdr_len = 2;
    } else if (plen <= 0xFFFF) {
        hdr[1] = WS_MASK_BIT | 126;
        hdr[2] = (unsigned char)(plen >> 8);
        hdr[3] = (unsigned char)(plen & 0xFF);
        hdr_len = 4;
    } else {
        hdr[1] = WS_MASK_BIT | 127;
        memset(hdr + 2, 0, 4); /* high 32 bits = 0 */
        hdr[6] = (unsigned char)((plen >> 24) & 0xFF);
        hdr[7] = (unsigned char)((plen >> 16) & 0xFF);
        hdr[8] = (unsigned char)((plen >> 8) & 0xFF);
        hdr[9] = (unsigned char)(plen & 0xFF);
        hdr_len = 10;
    }

    /* Generate 4-byte mask key */
    unsigned char mask[WS_MASK_KEY_LEN];

    asr_entropy_func(NULL, mask, WS_MASK_KEY_LEN);
    memcpy(hdr + hdr_len, mask, WS_MASK_KEY_LEN);
    hdr_len += WS_MASK_KEY_LEN;

    /* Send header */
    int ret = tls_write_all(ctx, hdr, hdr_len);

    if (ret != 0) {
        return ret;
    }

    /* Mask and send payload in chunks to avoid large stack alloc */
    unsigned char chunk[1024];
    size_t sent = 0;

    while (sent < plen) {
        size_t clen = plen - sent;

        if (clen > sizeof(chunk)) {
            clen = sizeof(chunk);
        }
        for (size_t i = 0; i < clen; i++) {
            chunk[i] = payload[sent + i] ^ mask[(sent + i) % 4];
        }
        ret = tls_write_all(ctx, chunk, clen);
        if (ret != 0) {
            return ret;
        }
        sent += clen;
    }

    return 0;
}

static int ws_send_binary(asr_tls_ctx_t* ctx,
    const unsigned char* payload, size_t plen)
{
    return ws_send_frame(ctx, WS_OPCODE_BINARY, payload, plen);
}

/* ── WebSocket frame recv ────────────────────────────────────── */

static int ws_recv_frame_raw(asr_tls_ctx_t* ctx,
    unsigned char* buf, size_t cap,
    size_t* out_len, int* out_opcode)
{
    unsigned char hdr[2];
    int ret = tls_read_all(ctx, hdr, 2);

    if (ret != 0) {
        return ret;
    }

    *out_opcode = hdr[0] & 0x0F;
    int masked = (hdr[1] & WS_MASK_BIT) != 0;
    size_t plen = hdr[1] & 0x7F;

    if (plen == 126) {
        unsigned char ext[2];

        ret = tls_read_all(ctx, ext, 2);
        if (ret != 0) {
            return ret;
        }
        plen = ((size_t)ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        unsigned char ext[8];

        ret = tls_read_all(ctx, ext, 8);
        if (ret != 0) {
            return ret;
        }
        plen = ((size_t)ext[4] << 24) | ((size_t)ext[5] << 16)
            | ((size_t)ext[6] << 8) | ext[7];
    }

    unsigned char mask[WS_MASK_KEY_LEN];

    if (masked) {
        ret = tls_read_all(ctx, mask, WS_MASK_KEY_LEN);
        if (ret != 0) {
            return ret;
        }
    }

    if (plen > cap) {
        syslog(LOG_ERR, "[%s] WS frame too large: %zu > %zu\n",
            TAG, plen, cap);
        return -EOVERFLOW;
    }

    if (plen > 0) {
        ret = tls_read_all(ctx, buf, plen);
        if (ret != 0) {
            return ret;
        }
        if (masked) {
            for (size_t i = 0; i < plen; i++) {
                buf[i] ^= mask[i % 4];
            }
        }
    }

    *out_len = plen;
    return 0;
}

/* Read the next frame that carries data.  The server sends WebSocket
 * keepalive pings when the stream is quiet for a while, and a ping has no
 * payload - reading it as a data frame turned it into a "frame too
 * short" protocol error and killed a session that was working fine.  A
 * ping is answered with a pong, a pong is ignored, and either way the
 * next frame is read. */
static int ws_recv_frame(asr_tls_ctx_t* ctx,
    unsigned char* buf, size_t cap,
    size_t* out_len, int* out_opcode)
{
    for (;;) {
        int ret = ws_recv_frame_raw(ctx, buf, cap, out_len, out_opcode);

        if (ret != 0) {
            return ret;
        }

        if (*out_opcode == WS_OPCODE_PING) {
            syslog(LOG_INFO, "[%s] WS ping, replying pong\n", TAG);
            ws_send_frame(ctx, WS_OPCODE_PONG, buf, *out_len);
            continue;
        }

        if (*out_opcode == WS_OPCODE_PONG) {
            continue;
        }

        return 0;
    }
}

/* ── Volcengine V3 binary frame builders ─────────────────────── */

/* Build a Volcengine V3 frame: 4B header + 4B sequence + 4B payload
 * size + gzip-compressed payload, wrapped in a WebSocket binary frame.
 *
 * Byte 1 = (message_type << 4) | flags, where flags select the
 * sequence semantics: 0x1 = positive seq, 0x3 = negative seq (last
 * packet of the session). */
static int send_volc_frame(asr_tls_ctx_t* ctx,
    unsigned char msg_type,
    unsigned char flags,
    int32_t seq,
    const unsigned char* payload,
    size_t plen)
{
    unsigned char* gz = NULL;
    size_t gzlen = 0;
    int ret;

    /* Compress unconditionally, even when the payload is empty.  The frame
     * below always declares the gzip flag, and an empty buffer sent under
     * that flag is what made the server answer "unable to ungzip payload:
     * EOF" on the session terminator.  zlib emits a short but complete
     * gzip member for zero input. */

    ret = gzip_compress(payload, plen, &gz, &gzlen);
    if (ret != 0) {
        return ret;
    }

    size_t flen = VOLC_FRAME_HDR + gzlen;
    unsigned char* frame = malloc(flen);

    if (!frame) {
        free(gz);
        return -ENOMEM;
    }

    frame[0] = VOLC_PROTO_VER;
    frame[1] = (unsigned char)((msg_type << 4) | flags);
    frame[2] = (unsigned char)((VOLC_SER_JSON << 4) | VOLC_COMP_GZIP);
    frame[3] = 0x00;

    /* Sequence number, big-endian signed 32-bit */

    frame[4] = (unsigned char)((seq >> 24) & 0xFF);
    frame[5] = (unsigned char)((seq >> 16) & 0xFF);
    frame[6] = (unsigned char)((seq >> 8) & 0xFF);
    frame[7] = (unsigned char)(seq & 0xFF);

    /* Payload size (compressed), big-endian uint32 */

    frame[8] = (unsigned char)((gzlen >> 24) & 0xFF);
    frame[9] = (unsigned char)((gzlen >> 16) & 0xFF);
    frame[10] = (unsigned char)((gzlen >> 8) & 0xFF);
    frame[11] = (unsigned char)(gzlen & 0xFF);

    if (gzlen > 0) {
        memcpy(frame + VOLC_FRAME_HDR, gz, gzlen);
    }

    free(gz);

    ret = ws_send_binary(ctx, frame, flen);

    free(frame);
    return ret;
}

/* Build and send the full_client_request JSON metadata (seq=1).
 * V3 keeps all credentials in the HTTP upgrade headers, so the body
 * only declares the audio format and recognition options. */
static int send_full_client_request(asr_tls_ctx_t* ctx, int32_t seq)
{
    cJSON* root = cJSON_CreateObject();

    if (!root) {
        return -ENOMEM;
    }

    /* user */
    cJSON* user = cJSON_AddObjectToObject(root, "user");

    cJSON_AddStringToObject(user, "uid", "agent");

    /* audio */
    cJSON* audio = cJSON_AddObjectToObject(root, "audio");

    cJSON_AddStringToObject(audio, "format", "pcm");
    cJSON_AddNumberToObject(audio, "rate",
        AGENT_VOICE_SAMPLE_RATE);
    cJSON_AddNumberToObject(audio, "bits", AGENT_VOICE_BITS);
    cJSON_AddNumberToObject(audio, "channel",
        AGENT_VOICE_CHANNELS);

    /* request */
    cJSON* req = cJSON_AddObjectToObject(root, "request");

    cJSON_AddStringToObject(req, "model_name", "bigmodel");
    cJSON_AddBoolToObject(req, "enable_itn", 1);
    cJSON_AddBoolToObject(req, "enable_punc", 1);
    cJSON_AddBoolToObject(req, "enable_ddc", 1);
    /* VAD: avoid cutting short commands on weak microphones */
    cJSON_AddNumberToObject(req, "end_window_size", 800);
    cJSON_AddNumberToObject(req, "force_to_speech_time", 1000);

    char* json_str = cJSON_PrintUnformatted(root);

    cJSON_Delete(root);

    if (!json_str) {
        return -ENOMEM;
    }

    syslog(LOG_INFO, "[%s] full_client_request: seq=%ld\n",
        TAG, (long)seq);

    int ret = send_volc_frame(ctx, VOLC_MSG_FULL_REQ, VOLC_FLAG_POS_SEQ,
        seq, (const unsigned char*)json_str, strlen(json_str));
    free(json_str);
    return ret;
}

/* Send one audio chunk via Volcengine V3 binary protocol.
 * The final chunk of a session carries a negative sequence number
 * (NEG_WITH_SEQ flag) so the server knows the stream is ending. */
static int send_audio_chunk(asr_tls_ctx_t* ctx,
    int32_t* seq,
    const unsigned char* pcm, size_t len,
    int is_last)
{
    unsigned char flags = VOLC_FLAG_POS_SEQ;
    int32_t myseq = *seq;

    if (is_last) {
        flags = VOLC_FLAG_NEG_WITH_SEQ;
        myseq = -myseq;
    }

    int ret = send_volc_frame(ctx, VOLC_MSG_AUDIO, flags,
        myseq, pcm, len);

    if (ret == 0 && !is_last) {
        (*seq)++;
    }

    return ret;
}

/* ── Receive and parse Volcengine server response ────────────── */

/* Read a big-endian uint32 from buf at offset off. */
static uint32_t read_be32(const unsigned char* buf, size_t off)
{
    return ((uint32_t)buf[off] << 24)
        | ((uint32_t)buf[off + 1] << 16)
        | ((uint32_t)buf[off + 2] << 8)
        | (uint32_t)buf[off + 3];
}

/* Receive one WS frame, unwrap Volcengine V3 header, gunzip and
 * parse JSON.  Extracts payload_msg.result.text into text_out.
 * Returns: 1 = got final result, 0 = intermediate, negative = error
 *
 * V3 response layout (after the 4B header):
 *   [4B sequence]     present when flags bit0
 *   [4B event]        present when flags bit2
 *   [4B payload_size] (full_server_response) or [4B code][4B size]
 *                     (error response)
 *   [gzip payload]
 */
static int recv_volc_response(asr_tls_ctx_t* ctx,
    char* text_out, size_t text_cap)
{
    unsigned char* buf = malloc(WS_BUF_SIZE);

    if (!buf) {
        return -ENOMEM;
    }

    size_t flen;
    int opcode;
    int ret = ws_recv_frame(ctx, buf, WS_BUF_SIZE, &flen, &opcode);

    if (ret != 0) {
        free(buf);
        return ret;
    }

    if (opcode == WS_OPCODE_CLOSE) {
        syslog(LOG_WARNING, "[%s] Server sent WS close\n", TAG);
        free(buf);
        return -ECONNRESET;
    }

    /* Need at least Volcengine header (4 bytes minimum) */
    if (flen < 4) {
        syslog(LOG_ERR, "[%s] Frame too short: %zu\n", TAG, flen);
        free(buf);
        return -EPROTO;
    }

    /* Parse Volcengine header */
    int hdr_units = buf[0] & 0x0F;
    size_t volc_hdr_len = (size_t)hdr_units * 4;
    unsigned char msg_type_nibble = (buf[1] >> 4) & 0x0F;
    unsigned char flags = buf[1] & 0x0F;
    unsigned char compression = buf[2] & 0x0F;
    size_t off = volc_hdr_len;

    if (volc_hdr_len < 4 || flen < volc_hdr_len) {
        syslog(LOG_ERR,
            "[%s] Bad volc header: hdr_units=%d flen=%zu\n",
            TAG, hdr_units, flen);
        free(buf);
        return -EPROTO;
    }

    /* Optional sequence number (positive → intermediate) */
    int32_t seq = 0;

    if (flags & 0x01) {
        if (off + 4 > flen) {
            free(buf);
            return -EPROTO;
        }
        seq = (int32_t)read_be32(buf, off);
        off += 4;
    }

    int is_last = (flags & 0x02) ? 1 : 0;

    syslog(LOG_INFO,
        "[%s] resp frame: type=0x%x flags=0x%02x comp=%u seq=%ld "
        "last=%d flen=%zu\n",
        TAG, msg_type_nibble, flags, compression, (long)seq, is_last,
        flen);

    /* Optional event id */
    if (flags & 0x04) {
        if (off + 4 > flen) {
            free(buf);
            return -EPROTO;
        }
        off += 4;
    }

    /* ── Error response: [4B code][4B size][payload] ── */
    if (msg_type_nibble == 0x0F) {
        if (off + 8 > flen) {
            free(buf);
            return -EPROTO;
        }

        uint32_t err_code = read_be32(buf, off);
        uint32_t msg_size = read_be32(buf, off + 4);
        size_t msg_off = off + 8;

        if (msg_off + msg_size > flen) {
            msg_size = (uint32_t)(flen - msg_off);
        }

        /* The error message may be gzipped JSON or plain text */
        char errbuf[256];
        size_t cplen = msg_size;

        if (cplen >= sizeof(errbuf)) {
            cplen = sizeof(errbuf) - 1;
        }
        memcpy(errbuf, buf + msg_off, cplen);
        errbuf[cplen] = '\0';

        syslog(LOG_ERR,
            "[%s] Server error %lu: %s\n",
            TAG, (unsigned long)err_code, errbuf);
        free(buf);
        return -EIO;
    }

    /* ── Normal response: [4B payload_size][payload] ── */
    if (msg_type_nibble != 0x09) {
        free(buf);
        return 0; /* skip unknown message types */
    }

    if (off + 4 > flen) {
        syslog(LOG_ERR,
            "[%s] Frame too short for payload size: %zu\n",
            TAG, flen);
        free(buf);
        return -EPROTO;
    }

    uint32_t payload_len = read_be32(buf, off);

    off += 4;

    if (off + payload_len > flen) {
        syslog(LOG_ERR,
            "[%s] Payload overrun: off=%zu + plen=%lu "
            "> flen=%zu\n",
            TAG, off, (unsigned long)payload_len, flen);
        free(buf);
        return -EPROTO;
    }

    /* Event frames carry no payload; skip them */
    if (payload_len == 0) {
        syslog(LOG_INFO, "[%s] event frame: flags=0x%02x\n",
            TAG, flags);
        free(buf);
        return 0;
    }

    /* Payload is gzip-compressed on some paths and plain JSON on others.
     * The compression field sits in a different nibble depending on the
     * protocol revision, so trust the magic bytes over the flag. */
    unsigned char* json_buf;
    size_t json_len;
    int is_gzip = (compression == VOLC_COMP_GZIP) ||
        (payload_len >= 2 && buf[off] == 0x1f && buf[off + 1] == 0x8b);

    if (is_gzip) {
        ret = gzip_decompress(buf + off, payload_len,
            &json_buf, &json_len);
        if (ret != 0) {
            syslog(LOG_ERR, "[%s] gunzip payload failed: %d\n",
                TAG, ret);
            free(buf);
            return -EPROTO;
        }
    } else {
        json_buf = malloc(payload_len + 1);

        if (!json_buf) {
            free(buf);
            return -ENOMEM;
        }
        memcpy(json_buf, buf + off, payload_len);
        json_buf[payload_len] = '\0';
        json_len = payload_len;
    }

    free(buf);
    buf = NULL;

    cJSON* root = cJSON_ParseWithLength((const char*)json_buf, json_len);

    free(json_buf);

    if (!root) {
        syslog(LOG_ERR, "[%s] Bad JSON in response\n", TAG);
        return -EPROTO;
    }

    cJSON* code_j = cJSON_GetObjectItem(root, "code");
    int code = code_j ? (int)code_j->valuedouble : 0;

    if (code != 0) {
        cJSON* msg_j = cJSON_GetObjectItem(root, "message");

        syslog(LOG_ERR, "[%s] ASR error %d: %s\n", TAG, code,
            (msg_j && msg_j->valuestring)
                ? msg_j->valuestring
                : "unknown");
        cJSON_Delete(root);
        return -EIO;
    }

    /* The result text is updated as more audio arrives, so each response
     * simply replaces text_out.  It lives at result.text on this endpoint;
     * the nested payload_msg.result.text shape is kept as a fallback in
     * case a different model version moves it back. */
    cJSON* result = cJSON_GetObjectItem(root, "result");

    if (result) {
        cJSON* txt = cJSON_GetObjectItem(result, "text");

        if (txt && cJSON_IsString(txt) && txt->valuestring
            && txt->valuestring[0] != '\0') {
            strncpy(text_out, txt->valuestring, text_cap - 1);
            text_out[text_cap - 1] = '\0';
        }
    }

    if (text_out[0] == '\0') {
        cJSON* pmsg = cJSON_GetObjectItem(root, "payload_msg");

        if (pmsg) {
            cJSON* presult = cJSON_GetObjectItem(pmsg, "result");

            if (presult) {
                cJSON* txt = cJSON_GetObjectItem(presult, "text");

                if (txt && cJSON_IsString(txt) && txt->valuestring
                    && txt->valuestring[0] != '\0') {
                    strncpy(text_out, txt->valuestring, text_cap - 1);
                    text_out[text_cap - 1] = '\0';
                }
            }
        }
    }

    if (text_out[0] == '\0' && is_last) {
        char* dump = cJSON_PrintUnformatted(root);

        syslog(LOG_WARNING,
            "[%s] no text in final response: %s\n",
            TAG, dump ? dump : "(unprintable)");
        free(dump);
    }

    cJSON_Delete(root);

    return is_last ? 1 : 0;
}

/* ── Credentials (self-loaded from config store) ─────────────── */

static asr_auth_t s_auth;

static int volc_asr_init(void)
{
    memset(&s_auth, 0, sizeof(s_auth));

    /* New console: a single API key */
    claw_config_get(AGENT_CFG_KEY_VOLC_API_KEY,
        s_auth.api_key, sizeof(s_auth.api_key));

    /* Legacy console: app key + access key */
    claw_config_get(AGENT_CFG_KEY_VOLC_APPKEY,
        s_auth.app_key, sizeof(s_auth.app_key));
    claw_config_get(AGENT_CFG_KEY_VOLC_TOKEN,
        s_auth.access_key, sizeof(s_auth.access_key));

    /* Resource id selects the model version */
    if (claw_config_get(AGENT_CFG_KEY_VOLC_ASR_RESOURCE,
            s_auth.resource_id, sizeof(s_auth.resource_id))
            != OK
        || s_auth.resource_id[0] == '\0') {
        strncpy(s_auth.resource_id, AGENT_DOUBAO_ASR_RESOURCE,
            sizeof(s_auth.resource_id) - 1);
    }

    return 0;
}

static int volc_asr_auth_ready(void)
{
    return (s_auth.api_key[0] != '\0')
        || (s_auth.app_key[0] != '\0' && s_auth.access_key[0] != '\0');
}

/* ── Public API (direct call, kept for backward compat) ──────── */

int volc_asr_recognize(const unsigned char* pcm_data,
    size_t pcm_len,
    char* text_out,
    size_t text_cap)
{
    if (!pcm_data || pcm_len == 0 || !text_out || text_cap == 0) {
        return -EINVAL;
    }

    text_out[0] = '\0';

    volc_asr_init();

    if (!volc_asr_auth_ready()) {
        syslog(LOG_ERR, "[%s] ASR credentials not configured\n", TAG);
        return -ENOENT;
    }

    struct timespec e2e_t0;
    clock_gettime(CLOCK_MONOTONIC, &e2e_t0);

    asr_tls_ctx_t ctx;
    int ret;

    /* 1. TLS connect */
    ret = asr_tls_connect(&ctx, AGENT_DOUBAO_ASR_HOST,
        AGENT_DOUBAO_ASR_PORT);
    if (ret != 0) {
        asr_tls_free(&ctx);
        return ret;
    }

    /* 2. WebSocket upgrade (auth headers carry the API key) */
    ret = ws_upgrade(&ctx, AGENT_DOUBAO_ASR_HOST,
        AGENT_DOUBAO_ASR_WS_PATH, &s_auth);
    if (ret != 0) {
        asr_tls_free(&ctx);
        return ret;
    }

    /* 3. Send full_client_request (seq starts at 1) */
    int32_t seq = 1;

    ret = send_full_client_request(&ctx, seq);
    if (ret != 0) {
        asr_tls_free(&ctx);
        return ret;
    }

    seq = 2;

    /* 4. Stream audio chunks; the last one carries a negative seq */
    size_t offset = 0;
    size_t chunk_size = AGENT_ASR_CHUNK_SIZE;

    while (offset < pcm_len) {
        size_t remain = pcm_len - offset;
        size_t clen = (remain < chunk_size) ? remain : chunk_size;
        int is_last = (offset + clen >= pcm_len) ? 1 : 0;

        ret = send_audio_chunk(&ctx, &seq, pcm_data + offset, clen,
            is_last);
        if (ret != 0) {
            syslog(LOG_ERR, "[%s] send_audio_chunk failed: %d\n",
                TAG, ret);
            asr_tls_free(&ctx);
            return ret;
        }
        offset += clen;
    }

    syslog(LOG_INFO, "[%s] Sent %zu bytes in %zu-byte chunks\n",
        TAG, pcm_len, chunk_size);

    /* 5. Receive responses until final (sequence < 0) */
    struct timespec asr_t0;
    clock_gettime(CLOCK_MONOTONIC, &asr_t0);

    int attempts = 0;
    int max_attempts = 100; /* safety limit */

    while (attempts < max_attempts) {
        ret = recv_volc_response(&ctx, text_out, text_cap);
        if (ret < 0) {
            syslog(LOG_ERR, "[%s] recv error: %d\n", TAG, ret);
            asr_tls_free(&ctx);
            return ret;
        }
        if (ret == 1) {
            syslog(LOG_INFO, "[%s] final response after %d frames\n",
                TAG, attempts + 1);
            break; /* final response received */
        }
        attempts++;
    }

    if (attempts >= max_attempts) {
        syslog(LOG_WARNING,
            "[%s] gave up after %d non-final frames\n", TAG, attempts);
    }

    struct timespec asr_t1;
    clock_gettime(CLOCK_MONOTONIC, &asr_t1);
    long asr_ms = (asr_t1.tv_sec - asr_t0.tv_sec) * 1000
        + (asr_t1.tv_nsec - asr_t0.tv_nsec) / 1000000;

    asr_tls_free(&ctx);

    if (text_out[0] == '\0') {
        syslog(LOG_WARNING, "[%s] No text recognized (%ldms)\n",
            TAG, asr_ms);
        return -ENODATA;
    }

    struct timespec e2e_t1;
    clock_gettime(CLOCK_MONOTONIC, &e2e_t1);
    long e2e_ms = (e2e_t1.tv_sec - e2e_t0.tv_sec) * 1000
        + (e2e_t1.tv_nsec - e2e_t0.tv_nsec) / 1000000;

    syslog(LOG_INFO,
        "[%s] Recognized: %.80s (infer=%ldms e2e=%ldms)\n",
        TAG, text_out, asr_ms, e2e_ms);
    return 0;
}

/* ── Streaming ASR API ────────────────────────────────────────── */

struct volc_asr_stream {
    asr_tls_ctx_t tls;
    int32_t seq; /* frame sequence counter (full request = 1) */
    int ready; /* 1 after successful open */
};

volc_asr_stream_t* volc_asr_stream_open(void)
{
    volc_asr_init();

    if (!volc_asr_auth_ready()) {
        syslog(LOG_ERR, "[%s] stream: credentials not configured\n",
            TAG);
        return NULL;
    }

    volc_asr_stream_t* s = calloc(1, sizeof(*s));

    if (!s) {
        return NULL;
    }

    int ret = asr_tls_connect(&s->tls, AGENT_DOUBAO_ASR_HOST,
        AGENT_DOUBAO_ASR_PORT);
    if (ret != 0) {
        asr_tls_free(&s->tls);
        free(s);
        return NULL;
    }

    ret = ws_upgrade(&s->tls, AGENT_DOUBAO_ASR_HOST,
        AGENT_DOUBAO_ASR_WS_PATH, &s_auth);
    if (ret != 0) {
        asr_tls_free(&s->tls);
        free(s);
        return NULL;
    }

    s->seq = 1;
    ret = send_full_client_request(&s->tls, s->seq);
    if (ret != 0) {
        asr_tls_free(&s->tls);
        free(s);
        return NULL;
    }

    s->seq = 2;
    s->ready = 1;
    syslog(LOG_INFO, "[%s] stream: session opened\n", TAG);
    return s;
}

int volc_asr_stream_send(volc_asr_stream_t* s,
    const unsigned char* pcm, size_t len)
{
    if (!s || !s->ready || !pcm || len == 0) {
        return -EINVAL;
    }

    return send_audio_chunk(&s->tls, &s->seq, pcm, len, 0);
}

int volc_asr_stream_finish(volc_asr_stream_t* s,
    char* text_out, size_t text_cap)
{
    if (!s || !text_out || text_cap == 0) {
        free(s);
        return -EINVAL;
    }

    text_out[0] = '\0';

    if (!s->ready) {
        free(s);
        return -EINVAL;
    }

    /* Send empty last-chunk (negative seq) to signal end of audio */
    int ret = send_audio_chunk(&s->tls, &s->seq,
        (const unsigned char*)"", 0, 1);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] stream: send last chunk failed: %d\n",
            TAG, ret);
        asr_tls_free(&s->tls);
        free(s);
        return ret;
    }

    /* Receive responses until final */
    int attempts = 0;

    while (attempts < 100) {
        ret = recv_volc_response(&s->tls, text_out, text_cap);
        if (ret < 0) {
            syslog(LOG_ERR, "[%s] stream: recv error: %d\n",
                TAG, ret);
            asr_tls_free(&s->tls);
            free(s);
            return ret;
        }
        if (ret == 1) {
            break;
        }
        attempts++;
    }

    asr_tls_free(&s->tls);
    free(s);

    if (text_out[0] == '\0') {
        syslog(LOG_WARNING, "[%s] stream: no text recognized\n",
            TAG);
        return -ENODATA;
    }

    syslog(LOG_INFO, "[%s] stream: recognized: %.80s\n",
        TAG, text_out);
    return 0;
}

void volc_asr_stream_abort(volc_asr_stream_t* s)
{
    if (!s) {
        return;
    }

    if (s->ready) {
        asr_tls_free(&s->tls);
    }

    free(s);
    syslog(LOG_INFO, "[%s] stream: aborted\n", TAG);
}

/* ── Ops-based wrapper (simplified signature for framework) ──── */

static int volc_asr_recognize_ops(const unsigned char* pcm_data,
    size_t pcm_len,
    char* text_out,
    size_t text_cap)
{
    /* volc_asr_recognize() reloads credentials internally */
    return volc_asr_recognize(pcm_data, pcm_len,
        text_out, text_cap);
}

/* Backend ops registration */
static const voice_asr_ops_t s_volc_asr_ops = {
    .name = "volcengine",
    .init = volc_asr_init,
    .recognize = volc_asr_recognize_ops,
    .deinit = NULL,
};

int volc_asr_register(void)
{
    return voice_asr_register(&s_volc_asr_ops);
}
