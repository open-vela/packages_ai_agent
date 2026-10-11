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

#include "voice/voice_tts.h"
#include "voice/edge_tts.h"
#include "voice/volc_tts.h"
#include "agent_config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#define VOICE_TTS_MAX_BACKENDS 4

static const char* TAG = "voice_tts";

static const voice_tts_ops_t* s_backends[VOICE_TTS_MAX_BACKENDS];
static int s_backend_count;
static const voice_tts_ops_t* s_active;

/* PCM rate a backend produces; 0 means "the default streaming rate". */
static unsigned int tts_rate_of(const voice_tts_ops_t* ops)
{
    if (ops && ops->sample_rate > 0) {
        return ops->sample_rate;
    }

    return AGENT_TTS_WS_SAMPLE_RATE;
}

int voice_tts_register(const voice_tts_ops_t* ops)
{
    if (!ops || !ops->name || !ops->synthesize) {
        return -EINVAL;
    }

    if (s_backend_count >= VOICE_TTS_MAX_BACKENDS) {
        syslog(LOG_ERR, "[%s] Too many TTS backends\n", TAG);
        return -ENOMEM;
    }

    s_backends[s_backend_count++] = ops;
    syslog(LOG_INFO, "[%s] Registered backend: %s\n", TAG, ops->name);

    /* First registered backend becomes the default */
    if (!s_active) {
        s_active = ops;
        if (ops->init) {
            ops->init();
        }
    }

    return 0;
}

int voice_tts_set_backend(const char* name)
{
    if (!name) {
        return -EINVAL;
    }

    for (int i = 0; i < s_backend_count; i++) {
        if (strcmp(s_backends[i]->name, name) == 0) {
            if (s_active && s_active->deinit) {
                s_active->deinit();
            }

            s_active = s_backends[i];
            if (s_active->init) {
                s_active->init();
            }

            syslog(LOG_INFO, "[%s] Backend set to: %s\n",
                TAG, name);
            return 0;
        }
    }

    syslog(LOG_ERR, "[%s] Backend not found: %s\n", TAG, name);
    return -ENOENT;
}

const char* voice_tts_get_backend(void)
{
    return s_active ? s_active->name : NULL;
}

unsigned int voice_tts_get_sample_rate(void)
{
    return tts_rate_of(s_active);
}

int voice_tts_speak(const char* text,
    unsigned char* pcm_out,
    size_t pcm_cap,
    size_t* pcm_len)
{
    if (!s_active) {
        syslog(LOG_ERR, "[%s] No TTS backend registered\n", TAG);
        return -ENODEV;
    }

    return s_active->synthesize(text, pcm_out, pcm_cap, pcm_len);
}

/* ── Sample-rate adaptation ──────────────────────────────────────
 *
 * The playback device is opened once, before the chain runs, with the rate
 * reported by voice_tts_get_sample_rate() (the *active* backend's rate).
 * A fallback backend may disagree: the Volcengine WebSocket stream is 24kHz,
 * the Volcengine HTTP and the Edge streams are 16kHz.  Feeding 16kHz samples
 * to a device opened for 24kHz plays them 1.5x too slow, so chunks crossing
 * that boundary are resampled here (streaming linear interpolation).
 *
 * When both rates match - the normal case, and the whole Volcengine path -
 * no wrapper is installed at all and the callback is called directly. */

typedef struct {
    voice_tts_chunk_cb cb;
    void* user_data;
    unsigned int in_rate;
    unsigned int out_rate;
    int16_t last; /* last input sample of the previous chunk */
    double pos; /* next output position, in input samples */
} tts_rate_shim_t;

static int tts_floor_int(double v)
{
    int i = (int)v;

    return (v < 0.0 && (double)i != v) ? i - 1 : i;
}

static int16_t tts_read_s16(const unsigned char* p)
{
    int16_t v;

    memcpy(&v, p, 2); /* buffers may be unaligned */
    return v;
}

static size_t tts_pcm_resample(tts_rate_shim_t* sh,
    const unsigned char* in, size_t in_n,
    int16_t* out, size_t out_cap)
{
    const double step = (double)sh->in_rate / (double)sh->out_rate;
    double pos = sh->pos;
    size_t k = 0;

    while (k < out_cap) {
        int j = tts_floor_int(pos);
        double a;
        double b;
        double t;
        int32_t v;

        if (j >= (int)in_n) {
            break;
        }

        /* Positions past the last sample need the next chunk's first sample:
         * defer them instead of holding, so a streamed signal resamples
         * exactly like a whole buffer would. */
        if (j == (int)in_n - 1 && pos > (double)j) {
            break;
        }

        t = pos - (double)j;

        if (j < 0) {
            /* Position falls between the previous chunk's last sample and
             * this chunk's first one. */
            a = (double)sh->last;
            b = (double)tts_read_s16(in);
        } else if (j + 1 < (int)in_n) {
            a = (double)tts_read_s16(in + (size_t)j * 2);
            b = (double)tts_read_s16(in + (size_t)j * 2 + 2);
        } else {
            /* Exactly on the last sample: no interpolation needed. */
            a = (double)tts_read_s16(in + (size_t)j * 2);
            b = a;
            t = 0.0;
        }

        v = (int32_t)(a + (b - a) * t + (a <= b ? 0.5 : -0.5));

        if (v > 32767) {
            v = 32767;
        } else if (v < -32768) {
            v = -32768;
        }

        out[k++] = (int16_t)v;
        pos += step;
    }

    sh->last = tts_read_s16(in + (in_n - 1) * 2);
    sh->pos = pos - (double)in_n;
    return k;
}

static void tts_rate_shim_cb(const unsigned char* pcm_data, size_t pcm_len,
    int is_last, void* user_data)
{
    tts_rate_shim_t* sh = (tts_rate_shim_t*)user_data;
    size_t in_n;
    size_t cap;
    int16_t* out;

    if (!pcm_data || pcm_len < 2) {
        sh->cb(pcm_data, pcm_len, is_last, sh->user_data);
        return;
    }

    in_n = pcm_len / 2;
    cap = (size_t)((double)in_n * (double)sh->out_rate
            / (double)sh->in_rate) + 8;

    out = malloc(cap * sizeof(int16_t));

    if (!out) {
        /* Never drop audio just because the scratch buffer failed. */
        sh->cb(pcm_data, pcm_len, is_last, sh->user_data);
        return;
    }

    size_t out_n = tts_pcm_resample(sh, pcm_data, in_n, out, cap);

    if (out_n > 0) {
        sh->cb((const unsigned char*)out, out_n * 2, is_last, sh->user_data);
    } else if (is_last) {
        sh->cb(NULL, 0, 1, sh->user_data);
    }

    free(out);
}

/* Deliver a whole PCM buffer (one-shot backends) through the same rate
 * adaptation, marking it as the final chunk. */
static void tts_emit_buffer(voice_tts_chunk_cb cb, void* user_data,
    const unsigned char* buf, size_t len, unsigned int src_rate,
    unsigned int dst_rate)
{
    tts_rate_shim_t sh;

    if (src_rate == 0 || dst_rate == 0 || src_rate == dst_rate) {
        cb(buf, len, 1, user_data);
        return;
    }

    memset(&sh, 0, sizeof(sh));
    sh.cb = cb;
    sh.user_data = user_data;
    sh.in_rate = src_rate;
    sh.out_rate = dst_rate;
    tts_rate_shim_cb(buf, len, 1, &sh);
}

/* Run one streaming provider, resampling when its rate differs from the rate
 * the playback device was opened with. */
static int tts_run_stream(int (*fn)(const char*, voice_tts_chunk_cb, void*),
    const char* text, voice_tts_chunk_cb cb, void* user_data,
    unsigned int src_rate, unsigned int dst_rate)
{
    tts_rate_shim_t sh;

    if (!fn) {
        return -ENOSYS;
    }

    if (src_rate == 0 || dst_rate == 0 || src_rate == dst_rate) {
        return fn(text, cb, user_data);
    }

    memset(&sh, 0, sizeof(sh));
    sh.cb = cb;
    sh.user_data = user_data;
    sh.in_rate = src_rate;
    sh.out_rate = dst_rate;

    syslog(LOG_INFO, "[%s] resampling %u Hz -> %u Hz for playback\n",
        TAG, src_rate, dst_rate);
    return fn(text, tts_rate_shim_cb, &sh);
}

/* ── Streaming TTS with fallback chain ───────────────────────── */

int voice_tts_speak_stream(const char* text,
    voice_tts_chunk_cb cb,
    void* user_data)
{
    int ret;
    unsigned int play_rate;

    if (!s_active) {
        syslog(LOG_ERR, "[%s] No TTS backend for streaming\n", TAG);
        return -ENODEV;
    }

    if (!text || !cb) {
        return -EINVAL;
    }

    /* Rate the caller's playback device was opened with. */
    play_rate = voice_tts_get_sample_rate();

    /* 1) Whatever backend is active, if it can stream natively. */
    if (s_active->synthesize_stream) {
        ret = tts_run_stream(s_active->synthesize_stream, text, cb, user_data,
            tts_rate_of(s_active), play_rate);

        if (ret == 0) {
            return 0;
        }

        syslog(LOG_WARNING, "[%s] '%s' stream failed (%d), trying fallbacks\n",
            TAG, s_active->name, ret);
    }

    /* 2) Volcengine WebSocket streaming (classic appid+token interface).
     *    Skipped when step 1 already ran it. */
    if (s_active->synthesize_stream != volc_tts_ws_synthesize_stream) {
        ret = tts_run_stream(volc_tts_ws_synthesize_stream, text, cb,
            user_data, AGENT_TTS_WS_SAMPLE_RATE, play_rate);

        if (ret == 0) {
            return 0;
        }

        syslog(LOG_WARNING, "[%s] volc ws stream failed (%d), trying HTTP TTS\n",
            TAG, ret);
    }

    /* 3) Active backend's one-shot synthesize (Volcengine V3 HTTP, needs only
     *    an API key).  Backends whose one-shot path is the same transport as
     *    their streaming path (Edge) are skipped here - step 4 covers them. */
    if (s_active->synthesize
        && s_active->synthesize_stream != edge_tts_synthesize_stream) {
        unsigned char* buf = malloc(AGENT_VOICE_PCM_BUF_SIZE);

        if (!buf) {
            return -ENOMEM;
        }

        size_t len = 0;

        ret = s_active->synthesize(text, buf, AGENT_VOICE_PCM_BUF_SIZE, &len);

        if (ret == 0 && len > 0) {
            tts_emit_buffer(cb, user_data, buf, len, AGENT_VOICE_SAMPLE_RATE,
                play_rate);
            free(buf);
            return 0;
        }

        free(buf);
        syslog(LOG_WARNING, "[%s] HTTP TTS failed (%d), trying Edge TTS\n",
            TAG, ret);
    }

    /* 4) Microsoft Edge online TTS - free, no credentials. */
    if (s_active->synthesize_stream != edge_tts_synthesize_stream) {
        ret = tts_run_stream(edge_tts_synthesize_stream, text, cb, user_data,
            AGENT_EDGE_TTS_SAMPLE_RATE, play_rate);

        if (ret == 0) {
            return 0;
        }

        syslog(LOG_ERR, "[%s] all TTS backends failed (edge: %d)\n", TAG, ret);
        return ret;
    }

    syslog(LOG_ERR, "[%s] all TTS backends failed\n", TAG);
    return -EIO;
}
