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

/* Register the Volcengine TTS backend with the voice_tts framework. */
int volc_tts_register(void);

/* Re-read speed/volume/pitch from the config store.  volc_tts_register()
 * caches them once; call this after set_volc_voice so the next synthesis
 * request uses the new values without restarting the agent. */
void volc_tts_reload_ratios(void);

/* Legacy direct-call API (kept for backward compatibility). */
int volc_tts_synthesize_compat(const char *text,
                               const char *appid,
                               const char *token,
                               const char *speaker,
                               unsigned char *pcm_out,
                               size_t pcm_cap,
                               size_t *pcm_len);

/* Streaming TTS: deliver decoded PCM chunks via callback. */
typedef void (*volc_tts_chunk_cb)(const unsigned char *pcm_data,
                                  size_t pcm_len,
                                  int is_last,
                                  void *user_data);

int volc_tts_synthesize_stream(const char *text,
                               volc_tts_chunk_cb cb,
                               void *user_data);

/* WebSocket streaming TTS (bidirectional, no buffer size limit). */
int volc_tts_ws_synthesize_stream(const char *text,
                                  volc_tts_chunk_cb cb,
                                  void *user_data);

#ifdef __cplusplus
}
#endif
