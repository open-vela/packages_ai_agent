/*
 * Copyright (C) 2025 Xiaomi Corporation
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

/**
 * xiaozhi_udp.c — UDP audio transport for XiaoZhi protocol.
 *
 * Packet format (aligned with xiaozhi-esp32 mqtt_protocol.cc):
 *   The 16-byte AES nonce IS the packet header. Fields are written
 *   into the nonce before encryption, then sent as-is:
 *
 *   nonce[0]     = type (0x01 = audio)
 *   nonce[1]     = flags (0x00)
 *   nonce[2..3]  = payload_len (big-endian)
 *   nonce[4..7]  = ssrc (from original nonce, unchanged)
 *   nonce[8..11] = timestamp (big-endian)
 *   nonce[12..15]= sequence (big-endian)
 *
 *   Wire format: [nonce 16B] [encrypted_opus_payload]
 *
 *   The receiver uses the first 16 bytes as the AES-CTR nonce
 *   to decrypt the remaining payload.
 */

#include "channels/xiaozhi_udp.h"
#include "agent_config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>

static const char* TAG = "xz_udp";

#define XZ_PKT_TYPE_AUDIO 0x01
#define XZ_NONCE_SIZE     16

/* ── UDP socket ───────────────────────────────────────────── */

int xiaozhi_udp_open(const char* host, int port, int timeout_sec)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        syslog(LOG_ERR, "[%s] DNS resolve failed: %s\n", TAG, host);
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        syslog(LOG_ERR, "[%s] UDP connect failed: %s:%d (%s)\n",
            TAG, host, port, strerror(errno));
        close(fd); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);

    struct timeval tv = { .tv_sec = timeout_sec, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    syslog(LOG_INFO, "[%s] UDP connected to %s:%d (fd=%d)\n",
        TAG, host, port, fd);
    return fd;
}

void xiaozhi_udp_close(int sockfd)
{
    if (sockfd >= 0) close(sockfd);
}

/* ── AES-CTR (per-direction context) ──────────────────────── */

int xiaozhi_aes_ctx_init(xiaozhi_aes_ctx_t* ctx,
    const uint8_t* key, size_t key_len,
    const uint8_t* nonce, size_t nonce_len)
{
    if (!ctx || key_len != 16 || nonce_len != 16)
        return -EINVAL;

    mbedtls_aes_init(&ctx->aes);
    int ret = mbedtls_aes_setkey_enc(&ctx->aes, key, 128);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] aes_setkey_enc failed: -0x%04x\n",
            TAG, (unsigned)-ret);
        return -EIO;
    }

    memcpy(ctx->nonce, nonce, 16);
    ctx->ready = true;
    return 0;
}

void xiaozhi_aes_ctx_free(xiaozhi_aes_ctx_t* ctx)
{
    if (ctx && ctx->ready) {
        mbedtls_aes_free(&ctx->aes);
        ctx->ready = false;
    }
}

/**
 * Build the per-packet nonce (= packet header) by writing fields
 * into a copy of the base nonce, matching ESP32 SendAudio():
 *
 *   nonce[0]      = 0x01 (type)
 *   nonce[1]      = 0x00 (flags)
 *   nonce[2..3]   = payload_len (big-endian)
 *   nonce[4..7]   = unchanged (ssrc from server nonce)
 *   nonce[8..11]  = timestamp (big-endian)
 *   nonce[12..15] = sequence (big-endian)
 */
static void build_packet_nonce(uint8_t* pkt_nonce,
    const uint8_t* base_nonce,
    uint16_t payload_len,
    uint32_t timestamp,
    uint32_t sequence)
{
    memcpy(pkt_nonce, base_nonce, XZ_NONCE_SIZE);
    pkt_nonce[0] = XZ_PKT_TYPE_AUDIO;
    pkt_nonce[1] = 0x00;
    /* Use memcpy to avoid unaligned access on ARM */
    uint16_t tmp16 = htons(payload_len);
    memcpy(&pkt_nonce[2], &tmp16, sizeof(tmp16));
    /* nonce[4..7] = ssrc, keep from base nonce */
    uint32_t tmp32 = htonl(timestamp);
    memcpy(&pkt_nonce[8], &tmp32, sizeof(tmp32));
    tmp32 = htonl(sequence);
    memcpy(&pkt_nonce[12], &tmp32, sizeof(tmp32));
}

/* ── Send audio packet ────────────────────────────────────── */

int xiaozhi_udp_send_audio(int sockfd, xiaozhi_aes_ctx_t* aes,
    const uint8_t* opus_data, size_t opus_len,
    uint32_t sequence, uint32_t ssrc, uint32_t timestamp)
{
    (void)ssrc; /* ssrc comes from base nonce[4..7] */

    if (sockfd < 0 || !aes || !aes->ready || !opus_data || opus_len == 0)
        return -EINVAL;
    if (opus_len > XIAOZHI_UDP_MAX_PKT - XZ_NONCE_SIZE)
        return -EMSGSIZE;

    /* Build packet: [nonce/header 16B] [encrypted payload] */
    uint8_t pkt[XIAOZHI_UDP_MAX_PKT];
    build_packet_nonce(pkt, aes->nonce,
        (uint16_t)opus_len, timestamp, sequence);

    /* Encrypt opus payload using the packet nonce as CTR counter */
    size_t nc_off = 0;
    uint8_t stream_block[16] = { 0 };
    int ret = mbedtls_aes_crypt_ctr(&aes->aes, opus_len,
        &nc_off, pkt, /* nonce = first 16 bytes of pkt */
        stream_block,
        opus_data, pkt + XZ_NONCE_SIZE);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] AES encrypt failed: %d\n", TAG, ret);
        return -EIO;
    }

    ssize_t sent = send(sockfd, pkt, XZ_NONCE_SIZE + opus_len, 0);
    if (sent < 0)
        return -errno;
    return (int)opus_len;
}

/* ── Receive audio packet ─────────────────────────────────── */

int xiaozhi_udp_recv_audio(int sockfd, xiaozhi_aes_ctx_t* aes,
    uint8_t* opus_out, size_t opus_cap,
    uint32_t* out_sequence)
{
    if (sockfd < 0 || !aes || !opus_out)
        return -EINVAL;

    uint8_t pkt[XIAOZHI_UDP_MAX_PKT];
    ssize_t n = recv(sockfd, pkt, sizeof(pkt), 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0; /* timeout */
        return -errno;
    }
    if (n < XZ_NONCE_SIZE) {
        syslog(LOG_WARNING, "[%s] pkt too short: %zd\n", TAG, n);
        return -EPROTO;
    }

    /* Validate packet type */
    if (pkt[0] != XZ_PKT_TYPE_AUDIO) {
        syslog(LOG_WARNING, "[%s] unknown pkt type: 0x%02x\n", TAG, pkt[0]);
        return -EPROTO;
    }

    /* Extract fields from nonce/header (use memcpy for alignment safety) */
    uint32_t sequence;
    memcpy(&sequence, &pkt[12], sizeof(sequence));
    sequence = ntohl(sequence);

    size_t payload_len = (size_t)(n - XZ_NONCE_SIZE);
    if (payload_len == 0)
        return 0;
    if (payload_len > opus_cap)
        return -ENOBUFS;

    /* Decrypt: use first 16 bytes (nonce) as CTR counter,
     * exactly as ESP32 does in OnMessage callback */
    size_t nc_off = 0;
    uint8_t stream_block[16] = { 0 };
    uint8_t nonce_copy[XZ_NONCE_SIZE];
    memcpy(nonce_copy, pkt, XZ_NONCE_SIZE);

    int ret = mbedtls_aes_crypt_ctr(&aes->aes, payload_len,
        &nc_off, nonce_copy, stream_block,
        pkt + XZ_NONCE_SIZE, opus_out);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] AES decrypt failed: %d\n", TAG, ret);
        return -EIO;
    }

    if (out_sequence)
        *out_sequence = sequence;
    return (int)payload_len;
}
