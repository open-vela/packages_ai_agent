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
#include "voice/minimax_tts.h"
#include "voice/volc_tts.h"
#include "agent_config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

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
 * A fallback backend may disagree: the Volcengine WebSocket stream and the
 * Edge stream are 24kHz, the Volcengine HTTP one-shot is 16kHz.  Feeding
 * 16kHz samples to a device opened for 24kHz plays them 1.5x too slow, so
 * chunks crossing that boundary are resampled here (linear interpolation).
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

/* v125 P0-B（2026-11-03）：把播放回调包一层计数，用于"可播即播"判定。
 * 只要某个后端已经把 PCM 交给播放器，整条链结束时就不能再判失败：
 * 旧实现里 MiMo 已解出 164 736 B PCM，却因一次性缓冲 -28 被整句丢弃，
 * 最终 0 字节播出（serial_perf_session.log:1938-1966）。 */
typedef struct {
    voice_tts_chunk_cb cb;
    void* user_data;
    size_t bytes;
    int chunks;
} tts_deliver_t;

static void tts_deliver_cb(const unsigned char* pcm, size_t len,
    int is_last, void* user_data)
{
    tts_deliver_t* d = (tts_deliver_t*)user_data;

    if (pcm != NULL && len > 0) {
        d->bytes += len;
        d->chunks++;
    }

    if (d->cb != NULL) {
        d->cb(pcm, len, is_last, d->user_data);
    }
}

/* 单轮 TTS 计时：相对本次 speak 起点的毫秒差，不累计历史。 */
static long tts_elapsed_ms(const struct timespec* t0)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - t0->tv_sec) * 1000
        + (now.tv_nsec - t0->tv_nsec) / 1000000;
}

int voice_tts_speak_stream(const char* text,
    voice_tts_chunk_cb cb,
    void* user_data)
{
    int ret = -EIO;
    unsigned int play_rate;
    tts_deliver_t deliver;
    struct timespec step_t0;

    if (!s_active) {
        syslog(LOG_ERR, "[%s] No TTS backend for streaming\n", TAG);
        return -ENODEV;
    }

    if (!text || !cb) {
        return -EINVAL;
    }

    deliver.cb = cb;
    deliver.user_data = user_data;
    deliver.bytes = 0;
    deliver.chunks = 0;

    /* Rate the caller's playback device was opened with. */
    play_rate = voice_tts_get_sample_rate();

    /* Fallback order: the active backend, then MiniMax (paid, the only
     * provider whose key works on this board) -> Edge (free but answering
     * 403 on every call, which used to cost a ~43 s stall per sentence
     * before a usable backend was reached) -> Volcengine (appid+token never
     * granted; the V3 HTTP key is rejected with 45000030).  Each step is
     * skipped when the active backend is already that backend, so nothing is
     * tried twice in one call, and a backend that already produced PCM stops
     * the chain. */

    /* 1) Whatever backend is active, if it can stream natively. */
    if (s_active->synthesize_stream) {
        clock_gettime(CLOCK_MONOTONIC, &step_t0);
        ret = tts_run_stream(s_active->synthesize_stream, text,
            tts_deliver_cb, &deliver, tts_rate_of(s_active), play_rate);

        if (ret == 0) {
            syslog(LOG_INFO, "[%s] step1 '%s' stream ok (%ldms, %zu bytes)\n",
                TAG, s_active->name, tts_elapsed_ms(&step_t0), deliver.bytes);
            return 0;
        }

        /* 已播出过 PCM：不再判失败，也不让别的后端把同一句重念一遍。 */
        if (deliver.bytes > 0) {
            syslog(LOG_WARNING,
                "[%s] step1 '%s' failed (%d) after %zu bytes; keeping played audio\n",
                TAG, s_active->name, ret, deliver.bytes);
            return 0;
        }

        syslog(LOG_WARNING, "[%s] '%s' stream failed (%d), trying fallbacks\n",
            TAG, s_active->name, ret);
    }

    /* 2) MiniMax T2A v2 (HTTPS, Bearer key from tts_api_key/api_key/
     *    llm_backend_0).  Skipped when step 1 already ran it, i.e. when it
     *    is the active backend. */
    if (s_active->synthesize_stream != minimax_tts_synthesize_stream) {
        clock_gettime(CLOCK_MONOTONIC, &step_t0);
        ret = tts_run_stream(minimax_tts_synthesize_stream, text,
            tts_deliver_cb, &deliver,
            AGENT_MINIMAX_TTS_SAMPLE_RATE, play_rate);

        if (ret == 0) {
            syslog(LOG_INFO, "[%s] minimax stream ok (%ldms, %zu bytes)\n",
                TAG, tts_elapsed_ms(&step_t0), deliver.bytes);
            return 0;
        }

        if (deliver.bytes > 0) {
            syslog(LOG_WARNING,
                "[%s] minimax failed (%d) after %zu bytes; keeping played audio\n",
                TAG, ret, deliver.bytes);
            return 0;
        }

        syslog(LOG_WARNING,
            "[%s] minimax stream failed (%d, %ldms), trying Edge TTS\n",
            TAG, ret, tts_elapsed_ms(&step_t0));
    }

    /* 3) Microsoft Edge online TTS - free, no credentials (currently 403 on
     *    every call). */
    if (s_active->synthesize_stream != edge_tts_synthesize_stream) {
        clock_gettime(CLOCK_MONOTONIC, &step_t0);
        ret = tts_run_stream(edge_tts_synthesize_stream, text,
            tts_deliver_cb, &deliver, AGENT_EDGE_TTS_SAMPLE_RATE, play_rate);

        if (ret == 0) {
            syslog(LOG_INFO, "[%s] edge stream ok (%ldms, %zu bytes)\n",
                TAG, tts_elapsed_ms(&step_t0), deliver.bytes);
            return 0;
        }

        if (deliver.bytes > 0) {
            syslog(LOG_WARNING,
                "[%s] edge failed (%d) after %zu bytes; keeping played audio\n",
                TAG, ret, deliver.bytes);
            return 0;
        }

        syslog(LOG_WARNING,
            "[%s] edge stream failed (%d, %ldms), trying Volcengine TTS\n",
            TAG, ret, tts_elapsed_ms(&step_t0));
    }

    /* 4) Volcengine WebSocket streaming (classic appid+token interface). */
    if (s_active->synthesize_stream != volc_tts_ws_synthesize_stream) {
        clock_gettime(CLOCK_MONOTONIC, &step_t0);
        ret = tts_run_stream(volc_tts_ws_synthesize_stream, text,
            tts_deliver_cb, &deliver, AGENT_TTS_WS_SAMPLE_RATE, play_rate);

        if (ret == 0) {
            syslog(LOG_INFO, "[%s] volc ws stream ok (%ldms, %zu bytes)\n",
                TAG, tts_elapsed_ms(&step_t0), deliver.bytes);
            return 0;
        }

        if (deliver.bytes > 0) {
            syslog(LOG_WARNING,
                "[%s] volc ws failed (%d) after %zu bytes; keeping played audio\n",
                TAG, ret, deliver.bytes);
            return 0;
        }

        syslog(LOG_WARNING,
            "[%s] volc ws stream failed (%d, %ldms), trying HTTP TTS\n",
            TAG, ret, tts_elapsed_ms(&step_t0));
    }

    /* 5) 最后一搏 A（v125 P0-B）：把 active backend 的**流式**路径再放一次。
     * 旧代码这里调 s_active->synthesize()（one-shot）：MiMo 的实现会先把整句
     * PCM 收进 128 KB 定长缓冲再一次性发出，长回答必然 -ENOSPC 并被整句丢弃
     * ——真机 164 736 B PCM → 0 字节播出（log:1938-1966）。
     * 流式重放没有整句缓冲上限，解码一块播一块（"可播即播"），
     * 也不需要按句子切文本、不需要承担多段请求的失败/拼接复杂度。
     * Edge/MiniMax 的流式同传输已在第 2/3 步各试过一次，这里不再重复。 */
    if (s_active->synthesize_stream
        && s_active->synthesize_stream != edge_tts_synthesize_stream
        && s_active->synthesize_stream != minimax_tts_synthesize_stream) {
        clock_gettime(CLOCK_MONOTONIC, &step_t0);
        ret = tts_run_stream(s_active->synthesize_stream, text,
            tts_deliver_cb, &deliver, tts_rate_of(s_active), play_rate);

        if (ret == 0) {
            syslog(LOG_INFO,
                "[%s] retry '%s' stream ok (%ldms, %zu bytes)\n",
                TAG, s_active->name, tts_elapsed_ms(&step_t0), deliver.bytes);
            return 0;
        }

        if (deliver.bytes > 0) {
            syslog(LOG_WARNING,
                "[%s] retry '%s' failed (%d) after %zu bytes; keeping played audio\n",
                TAG, s_active->name, ret, deliver.bytes);
            return 0;
        }

        syslog(LOG_WARNING,
            "[%s] retry '%s' stream failed (%d, %ldms), trying one-shot TTS\n",
            TAG, s_active->name, ret, tts_elapsed_ms(&step_t0));
    }

    /* 6) 最后一搏 B：只有 one-shot 实现的后端（当前只有 Volcengine V3 HTTP，
     *    它的 ops 没有 synthesize_stream）。采样率用后端声明值；未声明(0)
     *    时退回 AGENT_VOICE_SAMPLE_RATE —— 那正是 volc HTTP 请求里的
     *    sample_rate（16 kHz）。不能在这里硬编码 16 kHz：MiMo/Edge/MiniMax
     *    是 24 kHz，按 16k 重采样会把整句放慢/降调 1.5 倍。 */
    if (s_active->synthesize && s_active->synthesize_stream == NULL) {
        unsigned char* buf = malloc(AGENT_VOICE_PCM_BUF_SIZE);
        unsigned int one_shot_rate;
        size_t len = 0;

        if (!buf) {
            return -ENOMEM;
        }

        clock_gettime(CLOCK_MONOTONIC, &step_t0);
        ret = s_active->synthesize(text, buf, AGENT_VOICE_PCM_BUF_SIZE, &len);

        one_shot_rate = (s_active->sample_rate > 0)
            ? s_active->sample_rate : AGENT_VOICE_SAMPLE_RATE;

        if (ret == 0 && len > 0) {
            tts_emit_buffer(tts_deliver_cb, &deliver, buf, len,
                one_shot_rate, play_rate);
            free(buf);
            syslog(LOG_INFO, "[%s] one-shot '%s' ok (%ldms, %zu bytes)\n",
                TAG, s_active->name, tts_elapsed_ms(&step_t0), len);
            return deliver.bytes > 0 ? 0 : -EIO;
        }

        free(buf);

        if (deliver.bytes > 0) {
            syslog(LOG_WARNING,
                "[%s] one-shot failed (%d) after %zu bytes; keeping played audio\n",
                TAG, ret, deliver.bytes);
            return 0;
        }

        syslog(LOG_WARNING, "[%s] one-shot '%s' failed (%d, %ldms)\n",
            TAG, s_active->name, ret, tts_elapsed_ms(&step_t0));
    }

    /* 兜底：只要播放器收到过 PCM，就不是"全失败"（否则 voice_channel 会
     * 把它当没出声退款，真机表现就是"有声音却报 -28"）。 */
    if (deliver.bytes > 0) {
        syslog(LOG_WARNING,
            "[%s] backends returned %d but %zu PCM bytes were already played\n",
            TAG, ret, deliver.bytes);
        return 0;
    }

    syslog(LOG_ERR, "[%s] all TTS backends failed (%d)\n", TAG, ret);
    return ret != 0 ? ret : -EIO;
}
