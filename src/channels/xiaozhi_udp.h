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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <mbedtls/aes.h>

/**
 * xiaozhi_udp.h — UDP audio transport for XiaoZhi protocol.
 *
 * Packet format: [nonce 16B] [encrypted_opus_payload]
 * The 16-byte nonce IS the packet header (type/flags/len/ssrc/ts/seq).
 * AES-CTR uses the nonce directly as the counter block.
 */

#define XIAOZHI_UDP_MAX_PKT 1400

/* Per-direction AES-CTR context */
typedef struct {
    mbedtls_aes_context aes;
    uint8_t nonce[16]; /* base nonce from server hello */
    bool ready;
} xiaozhi_aes_ctx_t;

int xiaozhi_aes_ctx_init(xiaozhi_aes_ctx_t* ctx,
    const uint8_t* key, size_t key_len,
    const uint8_t* nonce, size_t nonce_len);
void xiaozhi_aes_ctx_free(xiaozhi_aes_ctx_t* ctx);

int xiaozhi_udp_open(const char* host, int port, int timeout_sec);
void xiaozhi_udp_close(int sockfd);

int xiaozhi_udp_send_audio(int sockfd, xiaozhi_aes_ctx_t* aes,
    const uint8_t* opus_data, size_t opus_len,
    uint32_t sequence, uint32_t ssrc, uint32_t timestamp);

int xiaozhi_udp_recv_audio(int sockfd, xiaozhi_aes_ctx_t* aes,
    uint8_t* opus_out, size_t opus_cap,
    uint32_t* out_sequence);
