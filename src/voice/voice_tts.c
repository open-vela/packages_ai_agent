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
#include "infra/network_manager.h"   /* v142 Fix3：TTS 前网络/IOB 压力门禁 */

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#define VOICE_TTS_MAX_BACKENDS 4

static const char* TAG = "voice_tts";

static const voice_tts_ops_t* s_backends[VOICE_TTS_MAX_BACKENDS];
static int s_backend_count;
static const voice_tts_ops_t* s_active;

/* v142 Fix3：本 boot 内已确认"必死"的后端（认证/握手类失败且一个字节都没
 * 产出），后续轮直接跳过；voice_tts_set_backend() 切换后端时清空。
 * 声明放这里是因为 set_backend() 在文件前部也要用到。 */
static bool s_minimax_dead;
static bool s_edge_dead;
static bool s_volc_dead;

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

            /* v142 Fix3：切换后端时清空"本 boot 死后端"缓存 —— 用户可能刚
             * 通过 set_voice_tts/set_minimax_key 修好了 key，不能一直跳过。 */
            s_minimax_dead = false;
            s_edge_dead = false;
            s_volc_dead = false;

            syslog(LOG_INFO, "[%s] Backend set to: %s (dead-backend cache cleared)\n",
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

/* ═══ v142 Fix3：回退链总预算 / 本 boot 死后端 / Edge 启动门禁 ═══════
 *
 * 现场（.v3_work/plan/reset_rca.md）：MiMo TTS 截断后，回退链在**没有总
 * 预算**的情况下依次去试 minimax(1004 auth)/Edge(TLS -0x4C，每次 ~30s，还会
 * 换第 2 种 format)/volc(无凭证)，每步新建 TLS 连接，把已经 ACL 耗尽的
 * BT PAN 继续压 ~70s，最终整机冻结 → 硬件看门狗复位（WSR=0x20）。
 *
 * 四道闸，全部有编译开关（设为 0 即恢复旧行为）：
 *   TTS_CHAIN_BUDGET_MS     回退链总预算（默认 15000ms）
 *   TTS_SKIP_DEAD_BACKENDS  本 boot 死后端跳过（默认 1）
 *   TTS_EDGE_START_MAX_MS   Edge 启动门禁（默认 3000ms，Edge 内部会轮询
 *                           多种 format，本板每次 ~30s；voice_tts 无法改它的
 *                           内部循环，只能控制"要不要开始"）
 *   TTS_NET_GATE            网络/IOB 压力门禁（默认 1）
 *
 * 注意：预算只能在**后端之间**生效，不能打断正在阻塞的单个后端（其 socket
 * 超时由各后端自己控制：vela_tls 60s / minimax 20s）。验收时看
 * [voice_tts] chain budget exceeded / backend marked dead 日志。 */
#ifndef TTS_CHAIN_BUDGET_MS
#define TTS_CHAIN_BUDGET_MS 15000
#endif
#ifndef TTS_SKIP_DEAD_BACKENDS
#define TTS_SKIP_DEAD_BACKENDS 1
#endif
#ifndef TTS_EDGE_START_MAX_MS
#define TTS_EDGE_START_MAX_MS 3000
#endif
#ifndef TTS_NET_GATE
#define TTS_NET_GATE 1
#endif

/* 预算检查：超时返回 false 并打一行日志（调用方直接跳过剩余后端）。 */
static bool tts_chain_budget_ok(const struct timespec* t0, const char* where)
{
    long ms;

    if (TTS_CHAIN_BUDGET_MS <= 0) {
        return true;            /* 编译开关 = 0：关闭总预算 */
    }

    ms = tts_elapsed_ms(t0);
    if (ms < TTS_CHAIN_BUDGET_MS) {
        return true;
    }

    syslog(LOG_WARNING,
        "[%s] chain budget exceeded at %s (%ldms >= %dms), giving up rest of chain\n",
        TAG, where, ms, TTS_CHAIN_BUDGET_MS);
    return false;
}

/* 网络/IOB 压力门禁：链路断或 zblue/IOB 已经高水位时，不再叠加新的 TLS 连接。 */
static bool tts_network_ok(void)
{
#if !TTS_NET_GATE
    return true;
#else
#ifdef CONFIG_AI_AGENT_NET_RPMSG
    int iob;

    if (network_get_state() != NET_STATE_CONNECTED) {
        syslog(LOG_WARNING, "[%s] network down, skip TTS fallback\n", TAG);
        return false;
    }

    iob = network_get_iob_usage();
    if (iob >= NET_IOB_HIGH_WATERMARK) {
        syslog(LOG_WARNING,
            "[%s] IOB usage %d%% >= %d%%, skip TTS fallback (avoid PAN overload)\n",
            TAG, iob, NET_IOB_HIGH_WATERMARK);
        return false;
    }
#else
    if (!network_is_connected()) {
        syslog(LOG_WARNING, "[%s] network down, skip TTS fallback\n", TAG);
        return false;
    }
#endif
    return true;
#endif
}

/* 后端编号：1=minimax 2=edge 3=volc（0 保留给 active backend，永不判死）。 */
static bool tts_backend_dead(int which)
{
#if !TTS_SKIP_DEAD_BACKENDS
    (void)which;
    return false;
#else
    switch (which) {
    case 1: return s_minimax_dead;
    case 2: return s_edge_dead;
    case 3: return s_volc_dead;
    default: return false;
    }
#endif
}

static void tts_backend_mark_dead(int which, const char* name, int err)
{
#if TTS_SKIP_DEAD_BACKENDS
    switch (which) {
    case 1: s_minimax_dead = true; break;
    case 2: s_edge_dead = true; break;
    case 3: s_volc_dead = true; break;
    default: return;
    }

    syslog(LOG_WARNING,
        "[%s] backend '%s' marked dead for this boot (err=%d); later turns skip it\n",
        TAG, name, err);
#else
    (void)which;
    (void)name;
    (void)err;
#endif
}

int voice_tts_speak_stream(const char* text,
    voice_tts_chunk_cb cb,
    void* user_data)
{
    int ret = -EIO;
    unsigned int play_rate;
    tts_deliver_t deliver;
    struct timespec step_t0;
    struct timespec chain_t0;

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

    /* v142 Fix3：整条回退链的总计时钟 + 网络/IOB 压力门禁。
     * 链路断或 PAN 已高水位时，直接快速失败，不叠加新的 TLS 连接
     *（现场就是回退链在 PAN 饱和时继续握手，把整机推入看门狗复位）。 */
    clock_gettime(CLOCK_MONOTONIC, &chain_t0);
    if (!tts_network_ok()) {
        return -ENETDOWN;
    }

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
     *    is the active backend.
     *    v142 Fix3：本 boot 已判死/预算耗尽/网络高压力时跳过。 */
    if (s_active->synthesize_stream != minimax_tts_synthesize_stream) {
        if (tts_backend_dead(1)) {
            syslog(LOG_INFO,
                "[%s] skip minimax: marked dead earlier this boot\n", TAG);
        } else if (!tts_chain_budget_ok(&chain_t0, "minimax")
                   || !tts_network_ok()) {
            /* 预算/门禁 helper 已打日志 */
        } else {
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

        /* v142 Fix3：认证类失败（MiMo/MiniMax 1004 -> -EACCES）说明 key 无效，
         * 本 boot 不必再对同一后端重试。 */
        if (ret == -EACCES || ret == -EPERM) {
            tts_backend_mark_dead(1, "minimax", ret);
        }
        }
    }

    /* 3) Microsoft Edge online TTS - free, no credentials (currently 403 on
     *    every call).
     *    v142 Fix3：Edge 一次调用内部会轮询多种 format（edge_tts.c:1869），
     *    本板每次 TLS 握手就要 ~30s；voice_tts 改不了它的内部循环，所以只在
     *    "前面后端失败得非常快"（<= TTS_EDGE_START_MAX_MS）时才允许开始，
     *    失败一次即本 boot 判死，后续轮直接跳过。 */
    if (s_active->synthesize_stream != edge_tts_synthesize_stream) {
        if (tts_backend_dead(2)) {
            syslog(LOG_INFO,
                "[%s] skip edge: marked dead earlier this boot\n", TAG);
        } else if (tts_elapsed_ms(&chain_t0) > TTS_EDGE_START_MAX_MS
                   || !tts_chain_budget_ok(&chain_t0, "edge")
                   || !tts_network_ok()) {
            syslog(LOG_WARNING,
                "[%s] skip edge: chain already %ldms (> %dms) or budget/net gate\n",
                TAG, tts_elapsed_ms(&chain_t0), TTS_EDGE_START_MAX_MS);
        } else {
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

        /* v142 Fix3：本板 Edge 失败基本是 TLS/403 类（-0x4C/-EIO），单次代价
         * ~30s —— 本 boot 不再重复尝试（否则每轮都白付一次握手）。 */
        tts_backend_mark_dead(2, "edge", ret);
        }
    }

    /* 4) Volcengine WebSocket streaming (classic appid+token interface).
     *    v142 Fix3：本板没有 appid/token，失败即本 boot 判死，后续轮跳过。 */
    if (s_active->synthesize_stream != volc_tts_ws_synthesize_stream) {
        if (tts_backend_dead(3)) {
            syslog(LOG_INFO,
                "[%s] skip volc ws: marked dead earlier this boot\n", TAG);
        } else if (!tts_chain_budget_ok(&chain_t0, "volc_ws")
                   || !tts_network_ok()) {
            /* 预算/门禁 helper 已打日志 */
        } else {
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

        /* v142 Fix3：凭证从未配过，本 boot 不再重试。 */
        tts_backend_mark_dead(3, "volc_ws", ret);
        }
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
        /* v142 Fix2 已在 MiMo 的 HTTP 层加了"截断重试一次"
         * (MIMO_TTS_HTTP_ATTEMPTS=2)，这里不再重复整句重放，
         * 否则一条回答最多会发 4 个 TTS 请求。其它 active 后端保持旧兜底。 */
        if (strcmp(s_active->name, "mimo") == 0) {
            syslog(LOG_INFO,
                "[%s] skip retry_active: '%s' already retried at HTTP layer\n",
                TAG, s_active->name);
        } else if (!tts_chain_budget_ok(&chain_t0, "retry_active")
                   || !tts_network_ok()) {
            /* 预算/门禁 helper 已打日志 */
        } else {
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
    }

    /* 6) 最后一搏 B：只有 one-shot 实现的后端（当前只有 Volcengine V3 HTTP，
     *    它的 ops 没有 synthesize_stream）。采样率用后端声明值；未声明(0)
     *    时退回 AGENT_VOICE_SAMPLE_RATE —— 那正是 volc HTTP 请求里的
     *    sample_rate（16 kHz）。不能在这里硬编码 16 kHz：MiMo/Edge/MiniMax
     *    是 24 kHz，按 16k 重采样会把整句放慢/降调 1.5 倍。 */
    if (s_active->synthesize && s_active->synthesize_stream == NULL) {
        if (!tts_chain_budget_ok(&chain_t0, "one_shot")
            || !tts_network_ok()) {
            /* 预算/门禁 helper 已打日志 */
        } else {
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
