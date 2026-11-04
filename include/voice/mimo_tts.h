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

#ifdef __cplusplus
extern "C" {
#endif

/* Xiaomi MiMo Token Plan TTS backend (mimo-v2.5-tts).
 * OpenAI-compatible chat/completions with audio output; MP3 is decoded by
 * the shared minimp3 wrapper (edge_mp3.c).  Registered first so it becomes
 * the default backend. */
int mimo_tts_register(void);

/* Exposed for unit tests / chain use. */
int mimo_tts_synthesize_stream(const char* text,
                               void (*cb)(const unsigned char*, size_t, int, void*),
                               void* user_data);

#ifdef __cplusplus
}
#endif
