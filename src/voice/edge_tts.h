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

#include "voice/voice_tts.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register the Microsoft Edge "read aloud" TTS backend (name "edge").
 * Free, credential-less: the only request parameter that carries any
 * "authorisation" is the Sec-MS-GEC token derived from the wall clock. */
int edge_tts_register(void);

/* Streaming synthesis: raw 24 kHz/16-bit/mono PCM chunks are delivered
 * through cb as they arrive; cb is finally called with is_last == 1.
 * Returns 0 as soon as at least one chunk was delivered (partial audio is
 * better than replaying the same sentence through the next fallback),
 * negative errno when nothing could be synthesised at all. */
int edge_tts_synthesize_stream(const char *text,
                               voice_tts_chunk_cb cb,
                               void *user_data);

#ifdef __cplusplus
}
#endif
