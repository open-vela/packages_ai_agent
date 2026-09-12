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

/* Generic TTS backend interface. Each provider implements this struct
 * and registers via voice_tts_register(). */

/* Callback invoked for each PCM chunk received from TTS.
 * pcm_data/pcm_len: decoded PCM audio (16-bit LE, mono).
 * is_last: 1 if this is the final chunk.
 * user_data: opaque pointer passed to voice_tts_speak_stream(). */
typedef void (*voice_tts_chunk_cb)(const unsigned char *pcm_data,
                                   size_t pcm_len,
                                   int is_last,
                                   void *user_data);

typedef struct voice_tts_ops {
    const char *name;

    /* Load credentials from config store. */
    int (*init)(void);

    /* Synthesize UTF-8 text into raw PCM (16-bit LE, mono). */
    int (*synthesize)(const char *text,
                      unsigned char *pcm_out,
                      size_t pcm_cap,
                      size_t *pcm_len);

    /* Optional: synthesize text and stream decoded PCM chunks via cb.
     * If NULL, voice_tts_speak_stream() falls back to synthesize() and
     * delivers the full result as a single chunk. */
    int (*synthesize_stream)(const char *text,
                             voice_tts_chunk_cb cb,
                             void *user_data);

    /* Release resources held by the backend. */
    void (*deinit)(void);
} voice_tts_ops_t;

/* Register a TTS backend. Multiple backends can be registered. */
int voice_tts_register(const voice_tts_ops_t *ops);

/* Select a backend by name. Returns 0 or -ENOENT. */
int voice_tts_set_backend(const char *name);

/* Get the name of the current active backend, or NULL. */
const char *voice_tts_get_backend(void);

/* Synthesize text using the active backend. */
int voice_tts_speak(const char *text,
                    unsigned char *pcm_out,
                    size_t pcm_cap,
                    size_t *pcm_len);

/* ── Streaming TTS interface ─────────────────────────────────── */

/* Synthesize text with streaming callback. Each decoded PCM chunk
 * is delivered via cb as it arrives from the TTS server.
 * Returns 0 on success, negative errno on error. */
int voice_tts_speak_stream(const char *text,
                           voice_tts_chunk_cb cb,
                           void *user_data);

#ifdef __cplusplus
}
#endif
