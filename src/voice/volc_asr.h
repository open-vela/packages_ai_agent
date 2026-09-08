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

/* Register the Volcengine ASR backend with the voice_asr framework. */
int volc_asr_register(void);

/* Legacy direct-call API (kept for backward compatibility). */
int volc_asr_recognize(const unsigned char *pcm_data,
                       size_t pcm_len,
                       const char *app_id,
                       const char *token,
                       const char *resource_id_or_cluster,
                       char *text_out,
                       size_t text_cap);

/* ── Streaming ASR API ───────────────────────────────────────── */

/* Opaque handle for a streaming ASR session. */
typedef struct volc_asr_stream volc_asr_stream_t;

/* Open a streaming ASR session (TLS + WS upgrade + metadata).
 * Returns NULL on failure. Caller must call volc_asr_stream_finish()
 * or volc_asr_stream_abort() to release resources. */
volc_asr_stream_t *volc_asr_stream_open(void);

/* Send one PCM chunk. Can be called repeatedly from recording thread.
 * Returns 0 on success, negative errno on error. */
int volc_asr_stream_send(volc_asr_stream_t *s,
                         const unsigned char *pcm, size_t len);

/* Mark end of audio, receive final ASR result, and close session.
 * text_out receives the recognized text (NUL-terminated).
 * Returns 0 on success, negative errno on error. Always frees s. */
int volc_asr_stream_finish(volc_asr_stream_t *s,
                           char *text_out, size_t text_cap);

/* Abort a streaming session without waiting for result. Frees s. */
void volc_asr_stream_abort(volc_asr_stream_t *s);

#ifdef __cplusplus
}
#endif
