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
#include "voice/mimo_asr.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

/* Backend entry points used by the by-name dispatch in voice_asr_recognize().
 * They are declared here rather than in a shared header: http_asr_recognize()
 * lives in http_asr.c and volc_asr_recognize_ops() is the ops-shaped wrapper in
 * volc_asr.c (volc_asr_recognize() itself has a legacy app_id/token/cluster
 * signature and is NOT used by this dispatch path). */
int http_asr_recognize(const unsigned char* pcm_data, size_t pcm_len,
    char* text_out, size_t text_cap);
int volc_asr_recognize_ops(const unsigned char* pcm_data, size_t pcm_len,
    char* text_out, size_t text_cap);

#define VOICE_ASR_MAX_BACKENDS 4

/* Registered backend names (see the .name field of each ops table). */
#define VOICE_ASR_VOLC_NAME "volcengine"
#define VOICE_ASR_HTTP_NAME "http"

static const char* TAG = "voice_asr";

static const voice_asr_ops_t* s_backends[VOICE_ASR_MAX_BACKENDS];
static int s_backend_count;
static const voice_asr_ops_t* s_active;

/* Secondary link anchor for the MiMo cloud ASR backend (see
 * voice_asr_recognize()).  The primary anchor is the by-name direct call to
 * mimo_asr_recognize(); this store keeps the symbol referenced even if the
 * branch is ever optimised differently. */
void* volatile g_voice_asr_mimo_keep[2];

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

/* [V693] Is a backend with this name registered?  voice_asr_recognize()
 * calls http_asr_recognize() directly when the cloud backend fails, and a
 * direct symbol call runs whether or not the backend was ever registered:
 * with the self-hosted "http" backend gone that fallback would block for
 * HTTP_ASR_CONN_MS (5 s) on a closed port every time MiMo hiccups. */
static int voice_asr_registered(const char* name)
{
    if (name == NULL) {
        return 0;
    }

    for (int i = 0; i < s_backend_count; i++) {
        if (s_backends[i]->name &&
            strcmp(s_backends[i]->name, name) == 0) {
            return 1;
        }
    }

    return 0;
}

int voice_asr_recognize(const unsigned char* pcm_data,
    size_t pcm_len,
    char* text_out,
    size_t text_cap)
{
    int ret = -ENOSYS;

    if (!s_active) {
        syslog(LOG_ERR, "[%s] No ASR backend registered\n", TAG);
        return -ENODEV;
    }

    /* Secondary anchor: pin the MiMo cloud backend into the image. */
    g_voice_asr_mimo_keep[0] = (void*)mimo_asr_recognize;
    g_voice_asr_mimo_keep[1] = (void*)mimo_asr_register;

    /* Dispatch by NAME, never through s_active->recognize.  A function-pointer
     * call on s_active lets the link-time optimizer (LTO) devirtualize it,
     * prove the strcmp(name, "mimo") branch unreachable, and --gc-sections then
     * drops the whole mimo_asr translation unit -- every [MIMO-ASR] string
     * included -- so the cloud backend silently vanished from the firmware.
     * voice_tts_speak_stream() dodges the identical trap by calling its
     * http/volc TTS backends by name; that is why [MIMO-TTS] links while
     * [MIMO-ASR] did not.  The direct, name-based call to mimo_asr_recognize()
     * below is the anchor that links the MiMo ASR backend. */
    if (s_active->name) {
        if (strcmp(s_active->name, MIMO_ASR_BACKEND_NAME) == 0) {
            ret = mimo_asr_recognize(pcm_data, pcm_len, text_out, text_cap);
        } else if (strcmp(s_active->name, VOICE_ASR_VOLC_NAME) == 0) {
            ret = volc_asr_recognize_ops(pcm_data, pcm_len, text_out, text_cap);
        } else if (strcmp(s_active->name, VOICE_ASR_HTTP_NAME) == 0) {
            ret = http_asr_recognize(pcm_data, pcm_len, text_out, text_cap);
        }
    }

    if (ret != 0 &&
        (!s_active->name || strcmp(s_active->name, VOICE_ASR_HTTP_NAME) != 0) &&
        voice_asr_registered(VOICE_ASR_HTTP_NAME)) {
        /* A cloud backend (e.g. "mimo") may be unreachable; retry once with
         * the self-hosted HTTP backend when it is registered and different.
         *
         * [V693] The registration test matters: the call below is a direct
         * symbol reference, so without it this branch also fires when no
         * "http" backend exists and waits out the full 5 s connect
         * timeout against 42.48.185.89:19009. */
        ret = http_asr_recognize(pcm_data, pcm_len, text_out, text_cap);
    }

    /* [V360] ASR 环的“识别结果”留痕：有没有文本、多少字节、前 80 字节。 */

    

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

    /* [V693] Only the volcengine backend has a streaming (WebSocket) API;
     * every other backend is request/response.  Asking for a stream can
     * therefore only fail, and it used to fail loudly on every press:
     *
     *   [volc_asr] stream: credentials not configured
     *   [voice] pre-open ASR failed, will retry in thread
     *   [voice] streaming ASR unavailable, batch fallback
     *
     * Return NULL quietly instead; the caller already reads NULL as
     * "accumulate the PCM and recognise it in one shot on release". */
    if (s_active->name == NULL ||
        strcmp(s_active->name, VOICE_ASR_VOLC_NAME) != 0) {
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
