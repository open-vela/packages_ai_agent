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

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Generic ASR backend interface. Each provider implements this struct
 * and registers via voice_asr_register(). */

typedef struct voice_asr_ops {
    const char *name;

    /* Load credentials from config store. */
    int (*init)(void);

    /* Recognize PCM audio (16-bit LE, 16kHz, mono) into text. */
    int (*recognize)(const unsigned char *pcm_data,
                     size_t pcm_len,
                     char *text_out,
                     size_t text_cap);

    /* Release resources held by the backend. */
    void (*deinit)(void);

    /* ── 可选的流式接口 ────────────────────────────────────────────────
     * 后端若支持"边采集边处理", 就由采集线程逐块把 PCM 送进来(stream_send),
     * 而不是等录完再一次性交给 recognize()。MiniMax 用它做**边录边编码**
     * (见 minimax_asr.c): complexity=0 实测 2.05x 实时, 跟得上采集,
     * 于是录完时只剩上传, 省掉整段 2 秒的编码等待。
     * 四个回调要么都给、要么都不给。stream_open 返回 NULL 表示本次不可用,
     * 调用方会回退到 recognize() 的批量路径。 */
    void *(*stream_open)(void);
    int   (*stream_send)(void *s, const unsigned char *pcm, size_t len);
    int   (*stream_finish)(void *s, char *text_out, size_t text_cap);
    void  (*stream_abort)(void *s);
} voice_asr_ops_t;

/* Register an ASR backend. Multiple backends can be registered. */
int voice_asr_register(const voice_asr_ops_t *ops);

/* Select a backend by name. Returns 0 or -ENOENT. */
int voice_asr_set_backend(const char *name);

/* Get the name of the current active backend, or NULL. */
const char *voice_asr_get_backend(void);

/* Recognize speech using the active backend. */
int voice_asr_recognize(const unsigned char *pcm_data,
                        size_t pcm_len,
                        char *text_out,
                        size_t text_cap);

/* ── Streaming ASR interface ─────────────────────────────────── */

/* Opaque handle for a streaming ASR session. */
typedef struct voice_asr_stream voice_asr_stream_t;

/* Open a streaming ASR session using the active backend.
 * Returns NULL if the backend does not support streaming. */
voice_asr_stream_t *voice_asr_stream_open(void);

/* Send one PCM chunk to the streaming session. */
int voice_asr_stream_send(voice_asr_stream_t *s,
                          const unsigned char *pcm, size_t len);

/* Finish streaming, get final text, and free the session. */
int voice_asr_stream_finish(voice_asr_stream_t *s,
                            char *text_out, size_t text_cap);

/* Abort streaming session without waiting for result. */
void voice_asr_stream_abort(voice_asr_stream_t *s);

#ifdef __cplusplus
}
#endif
