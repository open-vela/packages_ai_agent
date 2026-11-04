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

/* ── Streaming TTS callback ──────────────────────────────────── */

/* Callback invoked for each PCM chunk received from TTS.
 * pcm_data/pcm_len: decoded PCM audio (16-bit LE, 16kHz, mono).
 * is_last: 1 if this is the final chunk.
 * user_data: opaque pointer passed to voice_tts_speak_stream(). */
typedef void (*voice_tts_chunk_cb)(const unsigned char *pcm_data,
                                   size_t pcm_len,
                                   int is_last,
                                   void *user_data);

/* Generic TTS backend interface. Each provider implements this struct
 * and registers via voice_tts_register(). */

typedef struct voice_tts_ops {
    const char *name;

    /* Load credentials from config store. */
    int (*init)(void);

    /* Synthesize UTF-8 text into raw PCM (16-bit LE, 16kHz, mono). */
    int (*synthesize)(const char *text,
                      unsigned char *pcm_out,
                      size_t pcm_cap,
                      size_t *pcm_len);

    /* Release resources held by the backend. */
    void (*deinit)(void);

    /* Optional native streaming implementation.  NULL when the backend only
     * supports one-shot synthesis; voice_tts_speak_stream() then falls back
     * to synthesize() plus the built-in Volcengine/Edge chain. */
    int (*synthesize_stream)(const char *text,
                             voice_tts_chunk_cb cb,
                             void *user_data);

    /* Sample rate (Hz) of the PCM this backend emits.  0 means "same as the
     * default streaming rate" (AGENT_TTS_WS_SAMPLE_RATE, 24kHz).  The voice
     * channel opens the playback device with voice_tts_get_sample_rate();
     * voice_tts_speak_stream() resamples when a fallback backend disagrees. */
    unsigned int sample_rate;
} voice_tts_ops_t;

/* Register a TTS backend. Multiple backends can be registered. */
int voice_tts_register(const voice_tts_ops_t *ops);

/* Select a backend by name. Returns 0 or -ENOENT. */
int voice_tts_set_backend(const char *name);

/* Get the name of the current active backend, or NULL. */
const char *voice_tts_get_backend(void);

/* Sample rate of the PCM produced by the active backend (never 0). */
unsigned int voice_tts_get_sample_rate(void);

/* Synthesize text using the active backend. */
int voice_tts_speak(const char *text,
                    unsigned char *pcm_out,
                    size_t pcm_cap,
                    size_t *pcm_len);

/* ── Streaming TTS interface ─────────────────────────────────── */

/* Synthesize text with streaming callback. Each decoded PCM chunk
 * is delivered via cb as it arrives from the TTS server.  Fallback chain:
 * active backend -> MiniMax T2A v2 -> Edge -> Volcengine WebSocket ->
 * Volcengine HTTP.
 * Returns 0 on success, negative errno when every backend failed. */
int voice_tts_speak_stream(const char *text,
                           voice_tts_chunk_cb cb,
                           void *user_data);

#ifdef __cplusplus
}
#endif
