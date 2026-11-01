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

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize voice channel (load config keys). */
int voice_channel_init(void);

/* Start voice recording, ASR, then push text to agent via message_bus. */
int voice_channel_start(void);

/* Stop an active voice session. */
int voice_channel_stop(void);

/* Stop recording and return ASR text to caller (does NOT push inbound).
 * text_out: buffer to receive ASR text, text_cap: buffer capacity.
 * Returns 0 on success, negative errno on failure.
 * If ASR returns empty text, text_out[0] is set to '\0' (not an error). */
int voice_channel_stop_with_text(char *text_out, size_t text_cap);

/* Push-to-talk edge from the panel (examples/ls_lvgl): `down` is true on
 * the long-press and false on the release.  Non-blocking - the edge is
 * queued to a worker that runs the session and posts progress back
 * through ls_lvgl_set_ai_text().  ls_lvgl.c carries a weak no-op of this
 * so the panel links without an agent; this is the strong definition. */
void ai_agent_voice_ptt(bool down);

/* Print peak / step / zero-crossing statistics per channel for a WAV on
 * the card.  PTT_CAPTURE_RATE and PTT_CAPTURE_CHANNELS are assumed. */
int voice_channel_analyze_file(const char *path);

/* Synthesize text and play back (called from outbound dispatcher). */
int voice_channel_speak(const char *text);

/* Test TTS: synthesize text, save PCM to file. */
int voice_channel_test_tts(const char *text, const char *out_path);

/* Test ASR: read an audio file, recognize, print result.  A WAV header in
 * the file supplies the format; channels/rate override it and cover
 * headerless captures, 0 meaning "not stated".  Either way the samples are
 * folded to the 16kHz mono that voice_asr_recognize() declares upstream. */
int voice_channel_test_asr(const char *pcm_path,
                           unsigned int channels, unsigned int rate);

#ifdef __cplusplus
}
#endif
