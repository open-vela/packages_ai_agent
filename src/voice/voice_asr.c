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
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#define VOICE_ASR_MAX_BACKENDS 4

static const char* TAG = "voice_asr";

static const voice_asr_ops_t* s_backends[VOICE_ASR_MAX_BACKENDS];
static int s_backend_count;
static const voice_asr_ops_t* s_active;

/* The recognition backend opens one WebSocket/TLS session per call, and
 * the Volcengine endpoint only serves one session per API key at a time.
 * volc_asr also reloads its credentials into a shared static on entry.
 * A second PTT released while the previous recognition is still
 * streaming therefore collides with the first: the new session is
 * refused, stalls, or comes back with an empty result.  Serialize the
 * calls; a pending one simply waits its turn. */

static pthread_mutex_t s_recognize_lock = PTHREAD_MUTEX_INITIALIZER;

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

    if (pthread_mutex_trylock(&s_recognize_lock) != 0)
    {
        syslog(LOG_INFO,
            "[%s] recognize: previous session still running, waiting\n",
            TAG);
        pthread_mutex_lock(&s_recognize_lock);
    }

    int ret = s_active->recognize(pcm_data, pcm_len, text_out, text_cap);
    pthread_mutex_unlock(&s_recognize_lock);
    return ret;
}

/* ── Streaming ASR (delegates to volc_asr streaming API) ─────── */

struct voice_asr_stream {
    volc_asr_stream_t* inner;
};

voice_asr_stream_t* voice_asr_stream_open(void)
{
    if (!s_active) {
        syslog(LOG_ERR, "[%s] No ASR backend for streaming\n", TAG);
        return NULL;
    }

    volc_asr_stream_t* inner = volc_asr_stream_open();

    if (!inner) {
        return NULL;
    }

    voice_asr_stream_t* s = calloc(1, sizeof(*s));

    if (!s) {
        volc_asr_stream_abort(inner);
        return NULL;
    }

    s->inner = inner;
    return s;
}

int voice_asr_stream_send(voice_asr_stream_t* s,
    const unsigned char* pcm, size_t len)
{
    if (!s || !s->inner) {
        return -EINVAL;
    }

    return volc_asr_stream_send(s->inner, pcm, len);
}

int voice_asr_stream_finish(voice_asr_stream_t* s,
    char* text_out, size_t text_cap)
{
    if (!s) {
        return -EINVAL;
    }

    int ret = volc_asr_stream_finish(s->inner, text_out, text_cap);

    /* s->inner is freed by volc_asr_stream_finish */
    free(s);
    return ret;
}

void voice_asr_stream_abort(voice_asr_stream_t* s)
{
    if (!s) {
        return;
    }

    volc_asr_stream_abort(s->inner);
    free(s);
}
