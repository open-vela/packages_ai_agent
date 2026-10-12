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

/* Register the MiniMax T2A v2 TTS backend (name "minimax").
 *
 * HTTPS POST https://api.minimaxi.com/v1/t2a_v2 with
 * "Authorization: Bearer <key>"; the key comes from the "tts_api_key" config
 * key, or from "api_key" when that is absent (the MiniMax key the LLM uses).
 * Docs: https://platform.minimax.cn/docs/api-reference/speech-t2a-http
 *
 * Unlike the credential-less Edge backend this one costs money, so it is
 * registered after it and never becomes the default backend. */
int minimax_tts_register(void);

/* Streaming synthesis: the response audio (hex or base64 encoded MP3) is
 * decoded as it arrives and handed to the minimp3 wrapper chunk by chunk;
 * cb is finally called with is_last == 1.  Raw 24 kHz/16-bit/mono PCM, the
 * same contract as edge_tts_synthesize_stream().
 *
 * Returns 0 as soon as at least one PCM chunk was delivered (partial audio is
 * better than re-speaking the sentence through the next fallback), negative
 * errno when nothing could be synthesised at all:
 *   -EACCES  no API key configured, or the service rejected it
 *   -ENOMEM  decoder/request allocation failed
 *   -ETIMEDOUT / -EIO / -EPROTO  transport or protocol failure
 * Never asserts, never retries in a loop. */
int minimax_tts_synthesize_stream(const char *text,
                                  voice_tts_chunk_cb cb,
                                  void *user_data);

#ifdef __cplusplus
}
#endif
