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
 * xiaozhi_opus.c — Opus codec wrapper for XiaoZhi channel.
 *
 * When CONFIG_LIB_OPUS is enabled, uses real libopus.
 * Otherwise, provides stubs that return -ENOSYS.
 */

#include "channels/xiaozhi_opus.h"

#include <errno.h>
#include <stdlib.h>
#include <syslog.h>

static const char* TAG = "xz_opus";

#ifdef CONFIG_LIB_OPUS
/* ── Real Opus implementation ─────────────────────────────── */

#include <opus/opus.h>

struct xiaozhi_opus_encoder {
    OpusEncoder* enc;
};

struct xiaozhi_opus_decoder {
    OpusDecoder* dec;
};

xiaozhi_opus_encoder_t* xiaozhi_opus_encoder_create(void)
{
    int err;
    OpusEncoder* enc = opus_encoder_create(16000, 1,
        OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !enc) {
        syslog(LOG_ERR, "[%s] opus_encoder_create failed: %d\n", TAG, err);
        return NULL;
    }

    xiaozhi_opus_encoder_t* e = calloc(1, sizeof(*e));
    if (!e) {
        opus_encoder_destroy(enc);
        return NULL;
    }

    e->enc = enc;
    syslog(LOG_INFO, "[%s] Opus encoder created (16kHz mono VOIP)\n", TAG);
    return e;
}

void xiaozhi_opus_encoder_destroy(xiaozhi_opus_encoder_t* e)
{
    if (e) {
        if (e->enc)
            opus_encoder_destroy(e->enc);
        free(e);
    }
}

int xiaozhi_opus_encode(xiaozhi_opus_encoder_t* e,
    const int16_t* pcm, int frame_size,
    uint8_t* out, int max_out)
{
    if (!e || !e->enc || !pcm || !out)
        return -EINVAL;

    int ret = opus_encode(e->enc, pcm, frame_size, out, max_out);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] opus_encode failed: %d\n", TAG, ret);
        return -EIO;
    }

    return ret; /* bytes written */
}

xiaozhi_opus_decoder_t* xiaozhi_opus_decoder_create(void)
{
    int err;
    OpusDecoder* dec = opus_decoder_create(24000, 1, &err);
    if (err != OPUS_OK || !dec) {
        syslog(LOG_ERR, "[%s] opus_decoder_create failed: %d\n", TAG, err);
        return NULL;
    }

    xiaozhi_opus_decoder_t* d = calloc(1, sizeof(*d));
    if (!d) {
        opus_decoder_destroy(dec);
        return NULL;
    }

    d->dec = dec;
    syslog(LOG_INFO, "[%s] Opus decoder created (24kHz mono)\n", TAG);
    return d;
}

void xiaozhi_opus_decoder_destroy(xiaozhi_opus_decoder_t* d)
{
    if (d) {
        if (d->dec)
            opus_decoder_destroy(d->dec);
        free(d);
    }
}

int xiaozhi_opus_decode(xiaozhi_opus_decoder_t* d,
    const uint8_t* data, int len,
    int16_t* pcm, int frame_size)
{
    if (!d || !d->dec || !pcm)
        return -EINVAL;

    int ret = opus_decode(d->dec, data, len, pcm, frame_size, 0);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] opus_decode failed: %d\n", TAG, ret);
        return -EIO;
    }

    return ret; /* samples decoded */
}

#else
/* ── Stub implementation (no libopus) ─────────────────────── */

struct xiaozhi_opus_encoder {
    int dummy;
};

struct xiaozhi_opus_decoder {
    int dummy;
};

xiaozhi_opus_encoder_t* xiaozhi_opus_encoder_create(void)
{
    syslog(LOG_WARNING, "[%s] Opus encoder stub (no CONFIG_LIB_OPUS)\n", TAG);
    return calloc(1, sizeof(xiaozhi_opus_encoder_t));
}

void xiaozhi_opus_encoder_destroy(xiaozhi_opus_encoder_t* e)
{
    free(e);
}

int xiaozhi_opus_encode(xiaozhi_opus_encoder_t* e,
    const int16_t* pcm, int frame_size,
    uint8_t* out, int max_out)
{
    (void)e;
    (void)pcm;
    (void)frame_size;
    (void)out;
    (void)max_out;
    return -ENOSYS;
}

xiaozhi_opus_decoder_t* xiaozhi_opus_decoder_create(void)
{
    syslog(LOG_WARNING, "[%s] Opus decoder stub (no CONFIG_LIB_OPUS)\n", TAG);
    return calloc(1, sizeof(xiaozhi_opus_decoder_t));
}

void xiaozhi_opus_decoder_destroy(xiaozhi_opus_decoder_t* d)
{
    free(d);
}

int xiaozhi_opus_decode(xiaozhi_opus_decoder_t* d,
    const uint8_t* data, int len,
    int16_t* pcm, int frame_size)
{
    (void)d;
    (void)data;
    (void)len;
    (void)pcm;
    (void)frame_size;
    return -ENOSYS;
}

#endif /* CONFIG_LIB_OPUS */
