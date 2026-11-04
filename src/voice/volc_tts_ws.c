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
 * Doubao TTS via WebSocket (V1 binary protocol).
 *
 * Protocol: wss://openspeech.bytedance.com/api/v1/tts/ws_binary
 * Flow:
 *   1. TLS connect + HTTP Upgrade to WebSocket
 *   2. Send full_client_request (JSON: app/user/audio/request)
 *   3. Receive audio_only_server_response frames (raw PCM)
 *   4. Last frame has sequence < 0
 *
 * Binary frame format: see volc_asr.c header comment.
 */

#include "infra/config_store.h"
#include "infra/http_proxy.h"
#include "agent_compat.h"
#include "agent_config.h"
#include "voice/volc_tts.h"

#include "cJSON.h"
#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

static const char* TAG = "volc_tts_ws";

#define TTS_WS_PATH "/api/v1/tts/ws_binary"

/* Volcengine binary protocol constants */
#define VOLC_PROTO_VER 0x11
#define VOLC_HDR_SIZE 8
#define VOLC_MSG_FULL_REQ 0x10
#define VOLC_MSG_AUDIO_RESP 0xB0 /* audio_only server response */
#define VOLC_MSG_FRONTEND 0xC0 /* frontend server response */
#define VOLC_MSG_ERROR 0xF0
#define VOLC_SER_JSON 0x10
#define VOLC_SER_JSON_GZ 0x11 /* JSON + gzip */
#define VOLC_SER_RAW 0x00

/* WebSocket constants */
#define WS_BUF_SIZE (32 * 1024)
#define WS_MASK_KEY_LEN 4
#define WS_OPCODE_BINARY 0x02
#define WS_OPCODE_CLOSE 0x08
#define WS_FIN_BIT 0x80
#define WS_MASK_BIT 0x80

/* Credentials */
static char s_appid[64];
static char s_token[128];
static char s_cluster[64];
static char s_speaker[64];

typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config cfg;
    mbedtls_net_context net;
    mbedtls_ctr_drbg_context ctr_drbg;
} tts_tls_ctx_t;

/* ── Entropy ─────────────────────────────────────────────────── */

static int tts_entropy_func(void* data, unsigned char* output, size_t len)
{
    (void)data;
    if (agent_secure_random(output, len) == 0) {
        return 0;
    }
    syslog(LOG_ERR, "[volc_tts] CRITICAL: No secure entropy source available\n");
    return -1;  /* Generic error - TLS handshake will fail safely */
}

/* ── TLS connect / free ──────────────────────────────────────── */

static void tts_tls_free(tts_tls_ctx_t* ctx);

static int tts_tls_connect(tts_tls_ctx_t* ctx, const char* host,
    const char* port)
{
    int ret;

    mbedtls_ssl_init(&ctx->ssl);
    mbedtls_ssl_config_init(&ctx->cfg);
    mbedtls_net_init(&ctx->net);
    mbedtls_ctr_drbg_init(&ctx->ctr_drbg);

    ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, tts_entropy_func, NULL,
        (const unsigned char*)"volc_tts_ws", 11);
    if (ret != 0) {
        goto fail;
    }

    if (http_proxy_is_enabled()) {
        int tunnel_fd = proxy_open_tunnel(host, atoi(port), 30000);

        if (tunnel_fd < 0) {
            ret = -ECONNREFUSED;
            goto fail;
        }

        ctx->net.fd = tunnel_fd;
    } else {
        ret = mbedtls_net_connect(&ctx->net, host, port, MBEDTLS_NET_PROTO_TCP);
        if (ret != 0) {
            ret = -ECONNREFUSED;
            goto fail;
        }
    }

    mbedtls_net_set_block(&ctx->net);

    if (ctx->net.fd >= 0) {
        struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };

        setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    ret = mbedtls_ssl_config_defaults(&ctx->cfg, MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        ret = -EIO;
        goto fail;
    }

    mbedtls_ssl_conf_min_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_2);
#if defined(MBEDTLS_SSL_PROTO_TLS1_3)
    mbedtls_ssl_conf_max_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_3);
#else
    mbedtls_ssl_conf_max_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_2);
#endif

    mbedtls_ssl_conf_authmode(&ctx->cfg, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_rng(&ctx->cfg, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);

    ret = mbedtls_ssl_setup(&ctx->ssl, &ctx->cfg);
    if (ret != 0) {
        ret = -EIO;
        goto fail;
    }

    mbedtls_ssl_set_hostname(&ctx->ssl, host);
    mbedtls_ssl_set_bio(&ctx->ssl, &ctx->net, mbedtls_net_send, mbedtls_net_recv,
        NULL);

    syslog(LOG_INFO, "[%s] Handshake start: %s:%s\n", TAG, host, port);

    while ((ret = mbedtls_ssl_handshake(&ctx->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            syslog(LOG_ERR, "[%s] handshake: -0x%04x\n", TAG, -ret);
            ret = -EIO;
            goto fail;
        }
    }

    syslog(LOG_INFO, "[%s] TLS connected\n", TAG);
    return 0;

fail:
    tts_tls_free(ctx);
    return ret;
}

static void tts_tls_free(tts_tls_ctx_t* ctx)
{
    mbedtls_ssl_close_notify(&ctx->ssl);
    mbedtls_net_free(&ctx->net);
    mbedtls_ssl_free(&ctx->ssl);
    mbedtls_ssl_config_free(&ctx->cfg);
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
}

/* ── TLS I/O helpers ─────────────────────────────────────────── */

static int tls_write_all(tts_tls_ctx_t* ctx, const unsigned char* buf,
    size_t len)
{
    size_t written = 0;

    while (written < len) {
        int ret = mbedtls_ssl_write(&ctx->ssl, buf + written, len - written);

        if (ret > 0) {
            written += (size_t)ret;
        } else if (ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            return -EIO;
        }
    }

    return 0;
}

static int tls_read_all(tts_tls_ctx_t* ctx, unsigned char* buf, size_t len)
{
    size_t got = 0;

    while (got < len) {
        int ret = mbedtls_ssl_read(&ctx->ssl, buf + got, len - got);

        if (ret > 0) {
            got += (size_t)ret;
        } else if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return -ECONNRESET;
        } else if (ret != MBEDTLS_ERR_SSL_WANT_READ) {
            /* Distinguish SO_RCVTIMEO timeout from real I/O errors.
             * When the socket recv timeout fires, recv() returns -1
             * with errno EAGAIN/EWOULDBLOCK, and mbedtls maps that
             * to MBEDTLS_ERR_NET_RECV_FAILED (-0x004C).  Return a
             * distinct code so callers can treat timeout as normal
             * end-of-stream without masking real connection failures. */
            if (ret == -0x004C && (errno == EAGAIN || errno == EWOULDBLOCK
                                   || errno == ETIMEDOUT)) {
                return -ETIMEDOUT;
            }
            return -EIO;
        }
    }

    return 0;
}

/* ── WebSocket upgrade ───────────────────────────────────────── */

static int ws_upgrade(tts_tls_ctx_t* ctx, const char* host, const char* path,
    const char* token)
{
    unsigned char key_raw[16];
    unsigned char key_b64[32];
    size_t key_b64_len;

    tts_entropy_func(NULL, key_raw, sizeof(key_raw));
    mbedtls_base64_encode(key_b64, sizeof(key_b64), &key_b64_len, key_raw,
        sizeof(key_raw));

    char req[768];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %.*s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Authorization: Bearer;%s\r\n"
        "\r\n",
        path, host, (int)key_b64_len, key_b64, token);

    if (n <= 0 || n >= (int)sizeof(req)) {
        return -EOVERFLOW;
    }

    int ret = tls_write_all(ctx, (const unsigned char*)req, (size_t)n);

    if (ret != 0) {
        return ret;
    }

    char resp[1024];
    size_t rlen = 0;

    while (rlen < sizeof(resp) - 1) {
        int r = mbedtls_ssl_read(&ctx->ssl, (unsigned char*)resp + rlen,
            sizeof(resp) - 1 - rlen);

        if (r > 0) {
            rlen += (size_t)r;
            resp[rlen] = '\0';
            if (strstr(resp, "\r\n\r\n")) {
                break;
            }
        } else if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return -ECONNRESET;
        } else if (r != MBEDTLS_ERR_SSL_WANT_READ) {
            return -EIO;
        }
    }

    int status = 0;

    if (sscanf(resp, "HTTP/1.1 %d", &status) != 1 || status != 101) {
        syslog(LOG_ERR, "[%s] WS upgrade failed: HTTP %d\n", TAG, status);
        return -EPROTO;
    }

    syslog(LOG_INFO, "[%s] WebSocket upgrade OK\n", TAG);
    return 0;
}

/* ── WebSocket frame send (client must mask) ─────────────────── */

static int ws_send_binary(tts_tls_ctx_t* ctx, const unsigned char* payload,
    size_t plen)
{
    unsigned char hdr[14];
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
        memset(hdr + 2, 0, 4);
        hdr[6] = (unsigned char)((plen >> 24) & 0xFF);
        hdr[7] = (unsigned char)((plen >> 16) & 0xFF);
        hdr[8] = (unsigned char)((plen >> 8) & 0xFF);
        hdr[9] = (unsigned char)(plen & 0xFF);
        hdr_len = 10;
    }

    unsigned char mask[WS_MASK_KEY_LEN];

    tts_entropy_func(NULL, mask, WS_MASK_KEY_LEN);
    memcpy(hdr + hdr_len, mask, WS_MASK_KEY_LEN);
    hdr_len += WS_MASK_KEY_LEN;

    int ret = tls_write_all(ctx, hdr, hdr_len);

    if (ret != 0) {
        return ret;
    }

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

static int ws_recv_frame(tts_tls_ctx_t* ctx, unsigned char* buf, size_t cap,
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

        plen = ((size_t)ext[4] << 24) | ((size_t)ext[5] << 16) | ((size_t)ext[6] << 8) | ext[7];
    }

    unsigned char mask_key[WS_MASK_KEY_LEN];

    if (masked) {
        ret = tls_read_all(ctx, mask_key, WS_MASK_KEY_LEN);
        if (ret != 0) {
            return ret;
        }
    }

    if (plen > cap) {
        syslog(LOG_ERR, "[%s] WS frame too large: %zu\n", TAG, plen);
        return -EOVERFLOW;
    }

    if (plen > 0) {
        ret = tls_read_all(ctx, buf, plen);
        if (ret != 0) {
            return ret;
        }

        if (masked) {
            for (size_t i = 0; i < plen; i++) {
                buf[i] ^= mask_key[i % 4];
            }
        }
    }

    *out_len = plen;
    return 0;
}

/* ── Volcengine frame send ───────────────────────────────────── */

static int send_volc_frame(tts_tls_ctx_t* ctx, unsigned char msg_type,
    unsigned char serialization,
    const unsigned char* payload, size_t plen)
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

/* ── Build and send TTS full_client_request ──────────────────── */

static int send_tts_request(tts_tls_ctx_t* ctx, const char* text)
{
    cJSON* root = cJSON_CreateObject();

    if (!root) {
        return -ENOMEM;
    }

    /* app */
    cJSON* app = cJSON_AddObjectToObject(root, "app");

    cJSON_AddStringToObject(app, "appid", s_appid);
    cJSON_AddStringToObject(app, "token", s_token);
    cJSON_AddStringToObject(app, "cluster", s_cluster);

    /* user */
    cJSON* user = cJSON_AddObjectToObject(root, "user");

    cJSON_AddStringToObject(user, "uid", "agent");

    /* audio */
    cJSON* audio = cJSON_AddObjectToObject(root, "audio");

    cJSON_AddStringToObject(audio, "voice_type", s_speaker);
    cJSON_AddStringToObject(audio, "encoding", "pcm");
    cJSON_AddNumberToObject(audio, "sample_rate", AGENT_TTS_WS_SAMPLE_RATE);
    /* [CALM-VOICE 2026-10-27] 1.0 -> 0.9：语速放慢，语气更平静。 */
    cJSON_AddNumberToObject(audio, "speed_ratio", 0.9);

    /* request */
    cJSON* req = cJSON_AddObjectToObject(root, "request");

    cJSON_AddStringToObject(req, "reqid", "agent-tts");
    cJSON_AddStringToObject(req, "text", text);
    cJSON_AddStringToObject(req, "text_type", "plain");
    cJSON_AddStringToObject(req, "operation", "submit");

    char* json_str = cJSON_PrintUnformatted(root);

    cJSON_Delete(root);

    if (!json_str) {
        return -ENOMEM;
    }

    syslog(LOG_INFO, "[%s] TTS request: text=%zu bytes\n", TAG, strlen(text));

    int ret = send_volc_frame(ctx, VOLC_MSG_FULL_REQ, VOLC_SER_JSON,
        (const unsigned char*)json_str, strlen(json_str));
    free(json_str);
    return ret;
}

/* ── Receive audio responses ─────────────────────────────────── */

static int recv_tts_audio(tts_tls_ctx_t* ctx, volc_tts_chunk_cb cb,
    void* user_data)
{
    unsigned char* buf = malloc(WS_BUF_SIZE);

    if (!buf) {
        return -ENOMEM;
    }

    /* Recv timeout strategy:
     * - First chunk: keep the handshake timeout (10s) since TTS synthesis
     *   latency varies with text length and server load (200ms–2s typical).
     * - After first chunk: tighten to 500ms per recv.  To tolerate
     *   transient network stalls without silently truncating audio,
     *   allow up to 3 consecutive timeouts (~1.5s total) before
     *   declaring end-of-stream.  A single stall just retries.
     * The initial 10s timeout was set by tts_tls_connect(), so we only
     * need to tighten it after the first audio chunk arrives. */

#define TTS_MAX_CONSECUTIVE_TIMEOUTS 3

    int chunks = 0;
    int consecutive_timeouts = 0;
    int err = 0;

    while (1) {
        size_t flen;
        int opcode;
        int ret = ws_recv_frame(ctx, buf, WS_BUF_SIZE, &flen, &opcode);

        if (ret != 0) {
            /* Timeout after audio started: retry up to N times to
             * tolerate transient stalls.  Only declare EOF after
             * consecutive timeouts exceed the threshold (~1.5s). */
            if (chunks > 0 && ret == -ETIMEDOUT) {
                consecutive_timeouts++;
                if (consecutive_timeouts < TTS_MAX_CONSECUTIVE_TIMEOUTS) {
                    syslog(LOG_DEBUG,
                        "[%s] recv timeout %d/%d, retrying\n",
                        TAG, consecutive_timeouts,
                        TTS_MAX_CONSECUTIVE_TIMEOUTS);
                    continue;
                }
                syslog(LOG_INFO,
                    "[%s] recv ended after %d chunks "
                    "(%d consecutive timeouts)\n",
                    TAG, chunks, consecutive_timeouts);
                break;
            }

            /* Peer close after audio started is normal EOF. */
            if (chunks > 0 && ret == -ECONNRESET) {
                syslog(LOG_INFO, "[%s] recv ended after %d chunks (rc=%d)\n",
                    TAG, chunks, ret);
                break;
            }

            syslog(LOG_ERR, "[%s] recv error before any audio: %d\n", TAG, ret);
            err = ret;
            break;
        }

        /* Any successful frame resets the timeout counter. */
        consecutive_timeouts = 0;

        if (opcode == WS_OPCODE_CLOSE) {
            syslog(LOG_INFO, "[%s] server closed WS\n", TAG);
            break;
        }

        if (flen < 4) {
            continue;
        }

        unsigned char msg_type = buf[1] & 0xF0;
        unsigned char msg_flags = buf[1] & 0x0F;
        size_t volc_hdr_len = (size_t)(buf[0] & 0x0F) * 4;


        if (volc_hdr_len < 4 || flen < volc_hdr_len) {
            continue;
        }

        /* Error response */
        if (msg_type == VOLC_MSG_ERROR) {
            uint32_t code = 0;

            if (flen >= volc_hdr_len + 4) {
                code = ((uint32_t)buf[volc_hdr_len] << 24) | ((uint32_t)buf[volc_hdr_len + 1] << 16) | ((uint32_t)buf[volc_hdr_len + 2] << 8) | (uint32_t)buf[volc_hdr_len + 3];
            }

            syslog(LOG_ERR, "[%s] server error: %lu\n", TAG, (unsigned long)code);
            err = -EIO;
            break;
        }

        /* Frontend response (e.g. duration info) — signals end of audio
         * when it arrives after audio chunks have been received. */
        if (msg_type == VOLC_MSG_FRONTEND) {
            if (chunks > 0) {
                break; /* All audio delivered, frontend is the epilogue */
            }
            continue;
        }

        /* Audio-only response (0xB) */
        if (msg_type == VOLC_MSG_AUDIO_RESP) {
            /* flags: 0=ack(no audio), 1+=has audio data */
            if (msg_flags == 0) {
                continue; /* ACK, no audio data */
            }

            /* After volc header: 4-byte sequence (signed) + 4-byte payload_size */
            size_t audio_off = volc_hdr_len + 8;

            if (flen <= audio_off) {
                continue;
            }

            /* Extract sequence as signed 32-bit (big-endian).
             * Per Volcengine binary protocol: sequence < 0 means last frame. */
            int32_t seq = (int32_t)(
                ((uint32_t)buf[volc_hdr_len] << 24) |
                ((uint32_t)buf[volc_hdr_len + 1] << 16) |
                ((uint32_t)buf[volc_hdr_len + 2] << 8) |
                (uint32_t)buf[volc_hdr_len + 3]);

            unsigned char* pcm = buf + audio_off;
            size_t pcm_len = flen - audio_off;

            cb(pcm, pcm_len, 0, user_data);
            chunks++;

            /* After first chunk, tighten recv timeout so we detect
             * end-of-stream quickly (server may not send close frame). */
            if (chunks == 1 && ctx->net.fd >= 0) {
                struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
                if (setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO,
                        &tv, sizeof(tv)) < 0) {
                    syslog(LOG_WARNING,
                        "[%s] setsockopt SO_RCVTIMEO failed: %d\n",
                        TAG, errno);
                }
            }

            /* sequence < 0 = last audio frame */
            if (seq < 0) {
                break;
            }
        }
    }

    free(buf);

    if (chunks > 0 && err == 0) {
        cb(NULL, 0, 1, user_data);
    }

    syslog(LOG_INFO, "[%s] %d audio chunks delivered\n", TAG, chunks);

    if (chunks == 0 && err == 0) {
        return -EPROTO;
    }

    return err;
}

/* ── Init credentials ────────────────────────────────────────── */

static void tts_ws_init(void)
{
    memset(s_appid, 0, sizeof(s_appid));
    memset(s_token, 0, sizeof(s_token));
    memset(s_cluster, 0, sizeof(s_cluster));
    memset(s_speaker, 0, sizeof(s_speaker));

    claw_config_get(AGENT_CFG_KEY_VOLC_APPKEY, s_appid, sizeof(s_appid));
    claw_config_get(AGENT_CFG_KEY_VOLC_TOKEN, s_token, sizeof(s_token));

    if (claw_config_get(AGENT_CFG_KEY_VOLC_CLUSTER, s_cluster,
            sizeof(s_cluster))
            != OK
        || s_cluster[0] == '\0') {
        strncpy(s_cluster, AGENT_VOICE_DEFAULT_CLUSTER, sizeof(s_cluster) - 1);
    }

    if (claw_config_get(AGENT_CFG_KEY_VOLC_SPEAKER, s_speaker,
            sizeof(s_speaker))
            != OK
        || s_speaker[0] == '\0') {
        strncpy(s_speaker, AGENT_VOICE_DEFAULT_SPEAKER, sizeof(s_speaker) - 1);
    }
}

/* ── Public API ──────────────────────────────────────────────── */

int volc_tts_ws_synthesize_stream(const char* text, volc_tts_chunk_cb cb,
    void* user_data)
{
    if (!text || !cb) {
        return -EINVAL;
    }

    tts_ws_init();

    if (s_appid[0] == '\0' || s_token[0] == '\0') {
        syslog(LOG_ERR, "[%s] credentials not configured\n", TAG);
        return -ENOENT;
    }

    tts_tls_ctx_t ctx;
    int ret = tts_tls_connect(&ctx, AGENT_DOUBAO_TTS_HOST, AGENT_DOUBAO_TTS_PORT);

    if (ret != 0) {
        tts_tls_free(&ctx);
        return ret;
    }

    ret = ws_upgrade(&ctx, AGENT_DOUBAO_TTS_HOST, TTS_WS_PATH, s_token);
    if (ret != 0) {
        tts_tls_free(&ctx);
        return ret;
    }

    ret = send_tts_request(&ctx, text);
    if (ret != 0) {
        tts_tls_free(&ctx);
        return ret;
    }

    ret = recv_tts_audio(&ctx, cb, user_data);

    tts_tls_free(&ctx);
    return ret;
}
