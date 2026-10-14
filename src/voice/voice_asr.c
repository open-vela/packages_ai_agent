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

    return s_active->recognize(pcm_data, pcm_len, text_out, text_cap);
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
