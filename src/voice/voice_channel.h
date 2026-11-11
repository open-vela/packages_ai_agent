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

/* Initialize voice channel (load config keys). */
int voice_channel_init(void);

/* Where a recognized utterance goes.
 *
 * The built-in consumer is the chat bus (the on-device agent).  A product
 * that wants the text somewhere else -- a remote-control channel, a home
 * automation bridge -- registers a sink and becomes that destination.
 * Registration is exclusive: a sink replaces the chat bus rather than
 * adding to it, so one utterance has exactly one owner.  voice_channel
 * never learns who is listening; the dependency points at it, never from
 * it.  With no sink registered behaviour is unchanged, so boards that do
 * not use the consumer keep working.
 */
typedef void (*voice_text_sink_t)(const char *text, void *ctx);
int voice_channel_register_sink(voice_text_sink_t fn, void *ctx);

/* Hand one recognized utterance to the current destination (the sink if one
 * is registered, the chat bus otherwise).  Callers that already hold the ASR
 * text -- a PTT button handler, a console command -- route through here so
 * the destination stays in one place.
 */
void voice_channel_dispatch(const char *text);

/* Start voice recording, ASR, then push text to agent via message_bus. */
int voice_channel_start(void);

/* Stop an active voice session. */
int voice_channel_stop(void);

/* Stop recording and return ASR text to caller (does NOT push inbound).
 * text_out: buffer to receive ASR text, text_cap: buffer capacity.
 * Returns 0 on success, negative errno on failure.
 * If ASR returns empty text, text_out[0] is set to '\0' (not an error). */
int voice_channel_stop_with_text(char *text_out, size_t text_cap);

/* Synthesize text and play back (called from outbound dispatcher). */
int voice_channel_speak(const char *text);

/* Play a short sine-wave beep (attention cue). freq_hz 0 → default 1000Hz,
 * duration_ms 0 → default 200ms. Returns 0 on success, negative errno. */
int voice_channel_beep(unsigned int freq_hz, unsigned int duration_ms);

/* Test TTS: synthesize text, save PCM to file. */
int voice_channel_test_tts(const char *text, const char *out_path);

/* Test ASR: read PCM file, recognize, print result. */
int voice_channel_test_asr(const char *pcm_path);

#ifdef __cplusplus
}
#endif
