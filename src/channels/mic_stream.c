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

/* Microphone uplink: AUDCODEC capture -> binary WebSocket frames.
 *
 * One thread per stream. It owns the codec for as long as it runs, so the
 * bring-up commands (mic_test and friends) must not be used concurrently.
 */

#include "channels/mic_stream.h"
#include "channels/ws_server.h"
#include "agent_compat.h"

#include "sf32lb_audcodec.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef CONFIG_SF32LB52_AUDCODEC

/* 5801 Hz, G.711 mu-law, one byte per sample.
 *
 * The rate is set by the link, not by the recogniser. The PPP connection
 * under this board delivers about 6 KB/s of payload; 8 kHz of mu-law needs
 * 8 KB/s and does not fit, and overrunning the link does not degrade
 * gracefully -- the reader falls behind, the DMA ring laps, and the missing
 * samples are replaced by whatever came a second later. 5801 Hz of mu-law is
 * 5.8 KB/s and arrives contiguous, and 2.9 kHz of audio band is close to what
 * the telephone network gives a recogniser.
 *
 * 5801 is not a design choice, it is what the ADC actually produces at
 * clk_div 7 / osr_sel 3, measured with mic_rate. The value matters because it
 * is what the far end resamples from: announcing anything else shifts the
 * whole spectrum, which a recogniser notices. Re-measure after touching
 * either divider.
 *
 * The delivered byte rate equals MIC_STREAM_RATE exactly while nothing is
 * dropped, so the receiver's byte counter is the check to make.
 */

#define MIC_STREAM_RATE    5801

static pthread_t s_thread;
static volatile bool s_running;
static bool s_started;
static char s_chat_id[64];
static int  s_samples = MIC_STREAM_DEF_SAMPLES;

/* Static rather than stack: the streaming thread only needs a small stack,
 * and this keeps it that way.
 */

static int16_t s_block[MIC_STREAM_MAX_SAMPLES];
static uint8_t s_ulaw[MIC_STREAM_MAX_SAMPLES];

/****************************************************************************
 * Name: mic_ulaw_encode
 *
 * Description:
 *   ITU-T G.711 mu-law companding. Quantisation is logarithmic because
 *   speech spends most of its time at low amplitude, and a linear 8-bit
 *   representation throws away the very part that carries the words.
 *
 ****************************************************************************/

static uint8_t mic_ulaw_encode(int16_t pcm)
{
    const int bias = 0x84;
    const int clip = 32635;
    int sample = pcm;          /* int: -(-32768) has to fit */
    int sign = 0;
    int exponent = 7;
    int mantissa;
    int mask;

    if (sample < 0) {
        sample = -sample;
        sign = 0x80;
    }

    if (sample > clip) {
        sample = clip;
    }

    sample += bias;

    /* Segment count: how many bits below bit 14 are needed to reach the
     * leading one. */
    for (mask = 0x4000; exponent > 0 && (sample & mask) == 0; mask >>= 1) {
        exponent--;
    }

    mantissa = (sample >> (exponent + 3)) & 0x0f;

    return (uint8_t)(~(sign | (exponent << 4) | mantissa));
}

static void* mic_stream_thread(void* arg)
{
    (void)arg;

    syslog(LOG_INFO, "[mic_stream] capture starting for %s\n", s_chat_id);

    /* Tell the far end what the bytes mean before sending any. A recogniser
     * fed the wrong rate or the wrong codec does not fail loudly, it just
     * produces confident nonsense, so the format travels with the stream. */
    {
        char fmt[96];
        snprintf(fmt, sizeof(fmt),
            "{\"type\":\"audio_format\",\"codec\":\"ulaw\",\"rate\":%d,"
            "\"channels\":1}", MIC_STREAM_RATE);
        ws_server_send_json(s_chat_id, fmt);
    }

    if (!sf32lb_audcodec_is_open()) {
        int ret = sf32lb_audcodec_open(MIC_STREAM_RATE);
        if (ret < 0) {
            syslog(LOG_ERR, "[mic_stream] open failed (%d)\n", ret);
            s_running = false;
            return NULL;
        }
    }

    while (s_running) {
        ssize_t n = sf32lb_audcodec_read(s_block, (size_t)s_samples);
        ssize_t i;

        if (n < 0) {
            syslog(LOG_ERR, "[mic_stream] capture read failed (%d)\n", (int)n);
            break;
        }

        for (i = 0; i < n; i++) {
            s_ulaw[i] = mic_ulaw_encode(s_block[i]);
        }

        /* A failed send means the client went away; stop rather than spin. */
        if (ws_server_send_binary(s_chat_id, s_ulaw, (size_t)n) != OK) {
            syslog(LOG_WARNING, "[mic_stream] send failed, stopping\n");
            break;
        }

    }

    sf32lb_audcodec_close();
    s_running = false;

    syslog(LOG_INFO, "[mic_stream] capture stopped\n");
    return NULL;
}

int mic_stream_start(const char* chat_id, int samples)
{
    if (s_running) {
        return OK; /* already streaming */
    }

    if (chat_id == NULL || chat_id[0] == '\0') {
        return ERROR;
    }

    if (samples <= 0) {
        samples = MIC_STREAM_DEF_SAMPLES;
    } else if (samples > MIC_STREAM_MAX_SAMPLES) {
        samples = MIC_STREAM_MAX_SAMPLES;
    }

    s_samples = samples;

    strncpy(s_chat_id, chat_id, sizeof(s_chat_id) - 1);
    s_chat_id[sizeof(s_chat_id) - 1] = '\0';

    syslog(LOG_INFO, "[mic_stream] chunk = %d samples (%d bytes)\n",
        s_samples, s_samples * 2);

    s_running = true;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4096);

    int ret = pthread_create(&s_thread, &attr, mic_stream_thread, NULL);
    pthread_attr_destroy(&attr);

    if (ret != 0) {
        s_running = false;
        syslog(LOG_ERR, "[mic_stream] pthread_create failed (%d)\n", ret);
        return ERROR;
    }

    s_started = true;
    return OK;
}

int mic_stream_stop(void)
{
    if (!s_running && !s_started) {
        return OK;
    }

    s_running = false;

    /* The thread notices s_running within one 100 ms block. */
    pthread_join(s_thread, NULL);
    s_started = false;

    return OK;
}

bool mic_stream_is_running(void)
{
    return s_running;
}

#else /* !CONFIG_SF32LB52_AUDCODEC */

int mic_stream_start(const char* chat_id, int samples)
{
    (void)chat_id;
    (void)samples;
    return ERROR;
}

int mic_stream_stop(void)
{
    return OK;
}

bool mic_stream_is_running(void)
{
    return false;
}

#endif /* CONFIG_SF32LB52_AUDCODEC */
