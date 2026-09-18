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
 * Doubao streaming ASR via WebSocket (V3 bigmodel binary protocol).
 *
 * Protocol: wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async
 * Auth: X-Api-Key / X-Api-Resource-Id / X-Api-Connect-Id headers (no Bearer).
 * Flow:
 *   1. TLS connect + HTTP Upgrade to WebSocket
 *   2. Send full_client_request (JSON metadata) in Volcengine frame
 *   3. Send audio_only chunks (PCM), last chunk flagged
 *   4. Receive full_server_response, extract result.text
 *
 * Volcengine binary frame (carried inside standard WebSocket frames):
 *   Byte 0: [protocol_version:4][header_size:4]  = 0x11
 *   Byte 1: [message_type:4][message_type_flags:4]
 *   Byte 2: [serialization:4][compression:4]
 *   Byte 3: reserved = 0x00
 *   Bytes 4-7: payload size (big-endian uint32)
 *   Bytes 8+: payload
 *
 * V3 server response layout (msg_type 0x9 = full_server_response):
 *   flags (byte1 low nibble) bit0=1 → a 4-byte sequence follows the header;
 *   bit1=1 → last packet (final result). So:
 *     [4B hdr][(4B seq)?][4B payload_size][payload JSON]
 *   The JSON carries result.text (full accumulated string), not result[0].text.
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

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

static const char* TAG = "volc_asr";

/* Volcengine binary protocol constants */
#define VOLC_PROTO_VER 0x11 /* version=1, header_size=1 (x4=4B) */
#define VOLC_MSG_FULL_REQ 0x10 /* full_client_request */
#define VOLC_MSG_AUDIO 0x20 /* audio_only, not last */
#define VOLC_MSG_AUDIO_LAST 0x22 /* audio_only, last chunk */
#define VOLC_MSG_FULL_RESP 0x90 /* full_server_response */
#define VOLC_MSG_ERROR 0xF0 /* server error */
#define VOLC_SER_JSON 0x10 /* JSON, no compression */
#define VOLC_SER_RAW 0x00 /* raw audio, no compression */
#define VOLC_HDR_SIZE 8 /* 4-byte header + 4-byte payload len */

#define WS_BUF_SIZE 4096
#define WS_MASK_KEY_LEN 4
#define WS_OPCODE_BINARY 0x02
#define WS_OPCODE_CLOSE 0x08
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
    const char* path, const char* api_key, const char* resource)
{
    /* Generate random 16-byte key, base64-encode it */
    unsigned char key_raw[16];
    unsigned char key_b64[32];
    size_t key_b64_len;

    asr_entropy_func(NULL, key_raw, sizeof(key_raw));
    mbedtls_base64_encode(key_b64, sizeof(key_b64), &key_b64_len,
        key_raw, sizeof(key_raw));

    char connect_id[ASR_UUID_LEN];

    generate_uuid(connect_id, sizeof(connect_id));

    char req[1024];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %.*s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "X-Api-Key: %s\r\n"
        "X-Api-Resource-Id: %s\r\n"
        "X-Api-Connect-Id: %s\r\n"
        "\r\n",
        path, host, (int)key_b64_len, key_b64,
        api_key, resource, connect_id);

    if (n <= 0 || n >= (int)sizeof(req)) {
        return -EOVERFLOW;
    }

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

static int ws_send_binary(asr_tls_ctx_t* ctx,
    const unsigned char* payload, size_t plen)
{
    unsigned char hdr[14]; /* max WS header: 2 + 8 + 4 */
    size_t hdr_len = 0;

    hdr[0] = WS_FIN_BIT | WS_OPCODE_BINARY;

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

/* ── WebSocket frame recv ────────────────────────────────────── */

static int ws_recv_frame(asr_tls_ctx_t* ctx,
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

/* ── Volcengine binary frame builders ────────────────────────── */

/* Build a Volcengine header (8 bytes) and wrap payload into a
 * standard WebSocket binary frame for sending. */
static int send_volc_frame(asr_tls_ctx_t* ctx,
    unsigned char msg_type,
    unsigned char serialization,
    const unsigned char* payload,
    size_t plen)
{
    unsigned char* frame = malloc(VOLC_HDR_SIZE + plen);

    if (!frame) {
        return -ENOMEM;
    }

    frame[0] = VOLC_PROTO_VER;
    frame[1] = msg_type;
    frame[2] = serialization;
    frame[3] = 0x00;
    frame[4] = (unsigned char)((plen >> 24) & 0xFF);
    frame[5] = (unsigned char)((plen >> 16) & 0xFF);
    frame[6] = (unsigned char)((plen >> 8) & 0xFF);
    frame[7] = (unsigned char)(plen & 0xFF);

    if (plen > 0) {
        memcpy(frame + VOLC_HDR_SIZE, payload, plen);
    }

    int ret = ws_send_binary(ctx, frame, VOLC_HDR_SIZE + plen);

    free(frame);
    return ret;
}

/* Build and send the full_client_request JSON metadata (V3 bigmodel).
 * Auth lives in the WS upgrade headers (X-Api-Key etc.), so there is no
 * "app" block here. */
static int send_full_client_request(asr_tls_ctx_t* ctx)
{
    cJSON* root = cJSON_CreateObject();

    if (!root) {
        return -ENOMEM;
    }

    /* user */
    cJSON* user = cJSON_AddObjectToObject(root, "user");

    cJSON_AddStringToObject(user, "uid", "agent");

    /* audio: raw PCM (format=pcm, codec=raw) */
    cJSON* audio = cJSON_AddObjectToObject(root, "audio");

    cJSON_AddStringToObject(audio, "format", "pcm");
    cJSON_AddStringToObject(audio, "codec", "raw");
    cJSON_AddNumberToObject(audio, "rate",
        AGENT_VOICE_SAMPLE_RATE);
    cJSON_AddNumberToObject(audio, "bits", AGENT_VOICE_BITS);
    cJSON_AddNumberToObject(audio, "channel",
        AGENT_VOICE_CHANNELS);

    /* request: bigmodel recognition */
    cJSON* req = cJSON_AddObjectToObject(root, "request");

    cJSON_AddStringToObject(req, "model_name", "bigmodel");
    cJSON_AddBoolToObject(req, "enable_itn", 0);
    cJSON_AddBoolToObject(req, "enable_punc", 0);
    cJSON_AddBoolToObject(req, "show_utterances", 1);
    cJSON_AddNumberToObject(req, "end_window_size", 800);

    char* json_str = cJSON_PrintUnformatted(root);

    cJSON_Delete(root);

    if (!json_str) {
        return -ENOMEM;
    }

    syslog(LOG_INFO, "[%s] full_client_request (v3 bigmodel)\n", TAG);

    int ret = send_volc_frame(ctx, VOLC_MSG_FULL_REQ, VOLC_SER_JSON,
        (const unsigned char*)json_str,
        strlen(json_str));
    free(json_str);
    return ret;
}

/* Send one audio chunk via Volcengine binary protocol. */
static int send_audio_chunk(asr_tls_ctx_t* ctx,
    const unsigned char* pcm, size_t len,
    int is_last)
{
    unsigned char msg_type = is_last ? VOLC_MSG_AUDIO_LAST
                                     : VOLC_MSG_AUDIO;

    return send_volc_frame(ctx, msg_type, VOLC_SER_RAW, pcm, len);
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

/* Receive one WS frame, unwrap Volcengine header, parse JSON.
 * Extracts result[0].text into text_out.
 * Returns: 1 = got final result, 0 = intermediate, negative = error
 *
 * Volcengine binary protocol layout varies by message type:
 *
 *   Normal response (msg_type 0x9):
 *     [4B header] [4B payload_size] [payload...]
 *
 *   Error response (msg_type 0xF):
 *     [4B header] [4B error_code] [4B error_msg_size] [error_msg...]
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
    unsigned char msg_type_byte = buf[1];
    unsigned char msg_type_nibble = (msg_type_byte >> 4) & 0x0F;
    unsigned char msg_flags = msg_type_byte & 0x0F;

    if (volc_hdr_len < 4 || flen < volc_hdr_len) {
        syslog(LOG_ERR,
            "[%s] Bad volc header: hdr_units=%d flen=%zu\n",
            TAG, hdr_units, flen);
        free(buf);
        return -EPROTO;
    }

    /* ── Error response: special layout with error_code field ── */
    if (msg_type_nibble == 0x0F) {
        /* [4B hdr][4B error_code][4B msg_size][msg...] */
        if (flen < volc_hdr_len + 8) {
            syslog(LOG_ERR,
                "[%s] Error frame too short: %zu\n",
                TAG, flen);
            free(buf);
            return -EPROTO;
        }

        uint32_t err_code = read_be32(buf, volc_hdr_len);
        uint32_t msg_size = read_be32(buf, volc_hdr_len + 4);
        size_t msg_off = volc_hdr_len + 8;

        if (msg_off + msg_size > flen) {
            msg_size = (uint32_t)(flen - msg_off);
        }

        /* NUL-terminate and log the error message */
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

    /* ── Normal response (msg_type 0x9):
     *     [4B hdr][(4B seq if flags bit0)][4B payload_size][payload] ── */
    size_t off = volc_hdr_len;

    if (msg_flags & 0x01) {
        off += 4; /* sequence field present */
    }

    if (flen < off + 4) {
        syslog(LOG_ERR,
            "[%s] Frame too short for payload size: %zu\n",
            TAG, flen);
        free(buf);
        return -EPROTO;
    }

    uint32_t payload_len = read_be32(buf, off);
    size_t data_off = off + 4;

    if (data_off + payload_len > flen) {
        syslog(LOG_ERR,
            "[%s] Payload overrun: off=%zu + plen=%lu "
            "> flen=%zu\n",
            TAG, data_off, (unsigned long)payload_len, flen);
        free(buf);
        return -EPROTO;
    }

    /* We only care about full_server_response (0x9) */
    if (msg_type_nibble != 0x09) {
        free(buf);
        return 0; /* skip unknown message types */
    }

    /* Parse JSON payload */
    buf[data_off + payload_len] = '\0';
    cJSON* root = cJSON_Parse((char*)(buf + data_off));

    free(buf);
    buf = NULL;

    if (!root) {
        syslog(LOG_ERR, "[%s] Bad JSON in response\n", TAG);
        return -EPROTO;
    }

    /* V3: result.text is the full accumulated string (no code/message
     * wrapper).  result is an object; keep array handling for safety. */
    cJSON* result = cJSON_GetObjectItem(root, "result");
    const char* txt = NULL;

    if (cJSON_IsObject(result)) {
        cJSON* t = cJSON_GetObjectItem(result, "text");

        if (t && cJSON_IsString(t)) {
            txt = t->valuestring;
        }
    } else if (cJSON_IsArray(result)
        && cJSON_GetArraySize(result) > 0) {
        cJSON* first = cJSON_GetArrayItem(result, 0);
        cJSON* t = cJSON_GetObjectItem(first, "text");

        if (t && cJSON_IsString(t)) {
            txt = t->valuestring;
        }
    }

    if (txt && txt[0] != '\0') {
        strncpy(text_out, txt, text_cap - 1);
        text_out[text_cap - 1] = '\0';
    }

    cJSON_Delete(root);

    /* Final when flags bit1 (0x2) set: last packet */
    return (msg_flags & 0x02) ? 1 : 0;
}

/* ── Credentials (self-loaded from config store) ─────────────── */

static char s_api_key[256];
static char s_resource[128];

static int volc_asr_init(void)
{
    memset(s_api_key, 0, sizeof(s_api_key));
    memset(s_resource, 0, sizeof(s_resource));

    claw_config_get(AGENT_CFG_KEY_VOLC_API_KEY,
        s_api_key, sizeof(s_api_key));

    strncpy(s_resource, AGENT_DOUBAO_ASR_RESOURCE,
        sizeof(s_resource) - 1);

    return 0;
}

/* ── Public API (direct call, kept for backward compat) ──────── */

int volc_asr_recognize(const unsigned char* pcm_data,
    size_t pcm_len,
    const char* api_key,
    const char* resource,
    char* text_out,
    size_t text_cap)
{
    if (!pcm_data || pcm_len == 0 || !api_key
        || !resource || !text_out || text_cap == 0) {
        return -EINVAL;
    }

    text_out[0] = '\0';

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

    /* 2. WebSocket upgrade */
    ret = ws_upgrade(&ctx, AGENT_DOUBAO_ASR_HOST,
        AGENT_DOUBAO_ASR_WS_PATH, api_key, resource);
    if (ret != 0) {
        asr_tls_free(&ctx);
        return ret;
    }

    /* 3. Send full_client_request (JSON metadata) */
    ret = send_full_client_request(&ctx);
    if (ret != 0) {
        asr_tls_free(&ctx);
        return ret;
    }

    /* 4. Stream audio chunks */
    size_t offset = 0;
    size_t chunk_size = AGENT_ASR_CHUNK_SIZE;

    while (offset < pcm_len) {
        size_t remain = pcm_len - offset;
        size_t clen = (remain < chunk_size) ? remain : chunk_size;
        int is_last = (offset + clen >= pcm_len) ? 1 : 0;

        ret = send_audio_chunk(&ctx, pcm_data + offset, clen,
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
            break; /* final response received */
        }
        attempts++;
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
    int ready; /* 1 after successful open */
    unsigned char last_chunk[AGENT_ASR_CHUNK_SIZE]; /* tail to re-send as
                                                      * the "last" chunk */
    size_t last_len;
};

volc_asr_stream_t* volc_asr_stream_open(void)
{
    volc_asr_init();

    if (s_api_key[0] == '\0') {
        syslog(LOG_ERR, "[%s] stream: API key not configured\n",
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
        AGENT_DOUBAO_ASR_WS_PATH, s_api_key, s_resource);
    if (ret != 0) {
        asr_tls_free(&s->tls);
        free(s);
        return NULL;
    }

    ret = send_full_client_request(&s->tls);
    if (ret != 0) {
        asr_tls_free(&s->tls);
        free(s);
        return NULL;
    }

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

    int ret = send_audio_chunk(&s->tls, pcm, len, 0);

    if (ret == 0) {
        /* Remember the tail so finish() can signal end-of-audio with a
         * NON-EMPTY last chunk.  The v3 bigmodel_async server treats an
         * empty AUDIO_LAST as an empty utterance; the working offline
         * path instead marks the last data chunk is_last=1, so mirror
         * that here. */
        size_t keep = (len < sizeof(s->last_chunk)) ? len
                                                    : sizeof(s->last_chunk);

        memcpy(s->last_chunk, pcm + (len - keep), keep);
        s->last_len = keep;
    }

    return ret;
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

    /* Signal end of audio with a NON-EMPTY last chunk: re-send the
     * remembered tail of the final audio chunk.  An empty AUDIO_LAST
     * makes the v3 bigmodel_async server finalize with no text. */
    static const unsigned char last_silence[2] = { 0, 0 };
    const unsigned char* tail = (s->last_len > 0)
        ? s->last_chunk : last_silence;
    size_t tail_len = (s->last_len > 0) ? s->last_len : 2;
    int ret = send_audio_chunk(&s->tls, tail, tail_len, 1);

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
            /* The server can signal end-of-stream by closing or sending
             * a trailing frame we don't fully parse, WITHOUT setting the
             * "final" flag on the last result frame.  result.text is the
             * full accumulated string, so if we already captured text,
             * accept it as definitive instead of failing the stream. */
            if (text_out[0] != '\0') {
                syslog(LOG_WARNING,
                    "[%s] stream: recv err %d after text, "
                    "using recognized text\n", TAG, ret);
                break;
            }
            syslog(LOG_ERR, "[%s] stream: recv error: %d\n",
                TAG, ret);
            asr_tls_free(&s->tls);
            free(s);
            return ret;
        }
        if (ret == 1) {
            /* Final frame: the server sends exactly one final (which
             * may be empty when no speech was recognized) and then
             * closes.  Accept it as definitive — a non-empty final
             * breaks out with the recognized text, an empty final
             * falls through to the -ENODATA return below. */
            syslog(LOG_INFO, "[%s] stream: final#%d text='%.120s'\n",
                TAG, attempts, text_out);
            break;
        }
        attempts++;
    }
    syslog(LOG_INFO, "[%s] stream: recv done, %d attempts, text='%.120s'\n",
        TAG, attempts, text_out);

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
    /* Reload credentials in case they changed at runtime */
    volc_asr_init();

    if (s_api_key[0] == '\0') {
        syslog(LOG_ERR, "[%s] ASR API key not configured\n", TAG);
        return -ENOENT;
    }

    return volc_asr_recognize(pcm_data, pcm_len,
        s_api_key, s_resource,
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
