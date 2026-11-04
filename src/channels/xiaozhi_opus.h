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

#include <stddef.h>
#include <stdint.h>

/**
 * xiaozhi_opus.h — Opus codec wrapper for XiaoZhi channel.
 *
 * Encoding: 16kHz mono, 60ms frames (960 samples)
 * Decoding: 24kHz mono, 60ms frames (1440 samples)
 */

#define XIAOZHI_OPUS_ENC_FRAME_SAMPLES 960  /* 60ms @ 16kHz */
#define XIAOZHI_OPUS_DEC_FRAME_SAMPLES 1440 /* 60ms @ 24kHz */
#define XIAOZHI_OPUS_ENC_FRAME_BYTES (XIAOZHI_OPUS_ENC_FRAME_SAMPLES * 2)
#define XIAOZHI_OPUS_DEC_FRAME_BYTES (XIAOZHI_OPUS_DEC_FRAME_SAMPLES * 2)
#define XIAOZHI_OPUS_MAX_PACKET 512

typedef struct xiaozhi_opus_encoder xiaozhi_opus_encoder_t;
typedef struct xiaozhi_opus_decoder xiaozhi_opus_decoder_t;

xiaozhi_opus_encoder_t* xiaozhi_opus_encoder_create(void);
void xiaozhi_opus_encoder_destroy(xiaozhi_opus_encoder_t* enc);
int xiaozhi_opus_encode(xiaozhi_opus_encoder_t* enc,
    const int16_t* pcm, int frame_size,
    uint8_t* out, int max_out);

xiaozhi_opus_decoder_t* xiaozhi_opus_decoder_create(void);
void xiaozhi_opus_decoder_destroy(xiaozhi_opus_decoder_t* dec);
int xiaozhi_opus_decode(xiaozhi_opus_decoder_t* dec,
    const uint8_t* data, int len,
    int16_t* pcm, int frame_size);
