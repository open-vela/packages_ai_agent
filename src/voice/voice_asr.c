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

#include "voice/voice_asr.h"
#include "voice/volc_asr.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#define VOICE_ASR_MAX_BACKENDS 4

static const char* TAG = "voice_asr";

static const voice_asr_ops_t* s_backends[VOICE_ASR_MAX_BACKENDS];
static int s_backend_count;
static const voice_asr_ops_t* s_active;

/* ── VOICE-F3（task-61, audio_rca2 §2.3/F3）：连续 -ENODATA(-61) 限流 ──
 * 短按 PTT 时 VAD 可能整段裁掉 -> 后端返回 -ENODATA；旧代码每次失败都打
 * 一条 "no speech detected"，连续 5 次也没有任何操作引导（RCA2 现场）。
 * 这里在 ASR 公共分发层统计连续次数：
 *   - 日志限流：第 1/2 次 + 之后每 4 次一条；
 *   - 第 3 次给一次"按住说话、说完再松手"的引导；
 *   - 成功识别一次即清零（并打一条 streak cleared 供回归对比）。
 * 不在失败后自动重试 —— 每次重试都会重新采集+走一次 ASR（RCA2 的 -61
 * 连击正是这样把体验拖到 20s 的）；voice_asr_consecutive_nospeech() 给
 * UI/VOICE-B 自行决定提示或限流。
 * 回滚：编译加 -DVOICE_ASR_NOSPEECH_RL=0 恢复"每次失败都打印"。 */
#ifndef VOICE_ASR_NOSPEECH_RL
#  define VOICE_ASR_NOSPEECH_RL 1
#endif

static unsigned s_nospeech_run;      /* 连续 -ENODATA 次数 */
static unsigned s_nospeech_total;    /* 本 boot 累计次数 */
static int      s_nospeech_hinted;   /* 是否已经给过"按住说"引导 */

static void voice_asr_note_result(int ret)
{
    if (ret == -ENODATA) {
        if (s_nospeech_run < 255) {
            s_nospeech_run++;
        }
        s_nospeech_total++;
#if VOICE_ASR_NOSPEECH_RL
        if (s_nospeech_run <= 2 || (s_nospeech_run % 4) == 0) {
            syslog(LOG_WARNING,
                "[%s] no speech detected x%u (total %u, -ENODATA)\n",
                TAG, s_nospeech_run, s_nospeech_total);
        }
        if (s_nospeech_run >= 3 && !s_nospeech_hinted) {
            s_nospeech_hinted = 1;
            syslog(LOG_WARNING,
                "[%s] consecutive no-speech x%u: hint: press-and-hold, "
                "speak after the start tone, release after finishing\n",
                TAG, s_nospeech_run);
        }
#else
        syslog(LOG_WARNING,
            "[%s] no speech detected x%u (total %u, -ENODATA)\n",
            TAG, s_nospeech_run, s_nospeech_total);
#endif
    } else if (ret == 0) {
        if (s_nospeech_run != 0) {
            syslog(LOG_INFO,
                "[%s] no-speech streak cleared after %u (total %u)\n",
                TAG, s_nospeech_run, s_nospeech_total);
        }
        s_nospeech_run = 0;
        s_nospeech_hinted = 0;
    }
}

unsigned voice_asr_consecutive_nospeech(void)
{
    return s_nospeech_run;
}

void voice_asr_reset_nospeech(void)
{
    s_nospeech_run = 0;
    s_nospeech_hinted = 0;
}

int voice_asr_register(const voice_asr_ops_t* ops)
{
    if (!ops || !ops->name || !ops->recognize) {
        return -EINVAL;
    }

    if (s_backend_count >= VOICE_ASR_MAX_BACKENDS) {
        syslog(LOG_ERR, "[%s] Too many ASR backends\n", TAG);
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

int voice_asr_set_backend(const char* name)
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

            /* VOICE-F3：换后端后旧的 no-speech 连击计数不再有参考意义 */
            voice_asr_reset_nospeech();

            syslog(LOG_INFO, "[%s] Backend set to: %s\n",
                TAG, name);
            return 0;
        }
    }

    syslog(LOG_ERR, "[%s] Backend not found: %s\n", TAG, name);
    return -ENOENT;
}

const char* voice_asr_get_backend(void)
{
    return s_active ? s_active->name : NULL;
}

int voice_asr_recognize(const unsigned char* pcm_data,
    size_t pcm_len,
    char* text_out,
    size_t text_cap)
{
    if (!s_active) {
        syslog(LOG_ERR, "[%s] No ASR backend registered\n", TAG);
        return -ENODEV;
    }

    int ret = s_active->recognize(pcm_data, pcm_len, text_out, text_cap);

    /* VOICE-F3：-ENODATA 连击限流/引导计数 */
    voice_asr_note_result(ret);
    return ret;
}

/* ── Streaming ASR ───────────────────────────────────────────────
 * 优先用后端自己的流式回调(voice_asr_ops_t 的 stream_*)。火山用的是另一套
 * WebSocket 流式接口(volc_asr_stream_*), 作为兼容路径保留。 */

struct voice_asr_stream {
    void* inner;        /* 后端自己的会话对象 */
    int is_volc;        /* 1 = inner 是 volc_asr_stream_t* */
};

voice_asr_stream_t* voice_asr_stream_open(void)
{
    voice_asr_stream_t* s;
    void* inner;

    if (!s_active) {
        syslog(LOG_ERR, "[%s] No ASR backend for streaming\n", TAG);
        return NULL;
    }

    if (s_active->stream_open != NULL) {
        inner = s_active->stream_open();

        if (!inner) {
            return NULL;
        }

        s = calloc(1, sizeof(*s));
        if (!s) {
            if (s_active->stream_abort) {
                s_active->stream_abort(inner);
            }
            return NULL;
        }

        s->inner = inner;
        s->is_volc = 0;
        return s;
    }

    /* 兼容路径: 火山后端的流式实现是独立的一套 API */
    if (strcmp(s_active->name, "volcengine") == 0) {
        volc_asr_stream_t* v = volc_asr_stream_open();

        if (!v) {
            return NULL;
        }

        s = calloc(1, sizeof(*s));
        if (!s) {
            volc_asr_stream_abort(v);
            return NULL;
        }

        s->inner = v;
        s->is_volc = 1;
        return s;
    }

    syslog(LOG_WARNING, "[%s] backend '%s' has no streaming interface\n",
        TAG, s_active->name);
    return NULL;
}

int voice_asr_stream_send(voice_asr_stream_t* s,
    const unsigned char* pcm, size_t len)
{
    if (!s || !s->inner) {
        return -EINVAL;
    }

    if (s->is_volc) {
        return volc_asr_stream_send((volc_asr_stream_t*)s->inner, pcm, len);
    }

    if (s_active == NULL || s_active->stream_send == NULL) {
        return -ENOSYS;
    }

    return s_active->stream_send(s->inner, pcm, len);
}

int voice_asr_stream_utterance_done(voice_asr_stream_t* s)
{
    if (!s || !s->inner || s->is_volc) {
        return 0;
    }

    if (s_active == NULL || s_active->stream_utterance_done == NULL) {
        return 0;
    }

    return s_active->stream_utterance_done(s->inner);
}

int voice_asr_stream_end_input(voice_asr_stream_t* s)
{
    if (!s || !s->inner || s->is_volc) {
        /* 火山那套是 WebSocket 流式, 收尾语义不同, 不需要这一步 */
        return 0;
    }

    if (s_active == NULL || s_active->stream_end_input == NULL) {
        return 0;
    }

    return s_active->stream_end_input(s->inner);
}

int voice_asr_stream_finish(voice_asr_stream_t* s,
    char* text_out, size_t text_cap)
{
    int ret;

    if (!s) {
        return -EINVAL;
    }

    if (s->is_volc) {
        ret = volc_asr_stream_finish((volc_asr_stream_t*)s->inner,
            text_out, text_cap);
        /* inner 已被 volc_asr_stream_finish 释放 */
    } else if (s_active != NULL && s_active->stream_finish != NULL) {
        ret = s_active->stream_finish(s->inner, text_out, text_cap);
    } else {
        ret = -ENOSYS;
    }

    /* VOICE-F3：流式路径也一样统计 -ENODATA 连击（真机走的就是这条） */
    voice_asr_note_result(ret);

    free(s);
    return ret;
}

void voice_asr_stream_abort(voice_asr_stream_t* s)
{
    if (!s) {
        return;
    }

    if (!s->inner) {
        free(s);
        return;
    }

    if (s->is_volc) {
        volc_asr_stream_abort((volc_asr_stream_t*)s->inner);
    } else if (s_active != NULL && s_active->stream_abort != NULL) {
        s_active->stream_abort(s->inner);
    }

    free(s);
}
