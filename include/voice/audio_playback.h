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

/* Audio playback session handle (opaque). */
typedef struct audio_playback audio_playback_t;

/* Open the playback device and configure for PCM output.
 * Returns NULL on failure. */
audio_playback_t* audio_playback_open(const char* dev_path,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample);

/* Write PCM data to the playback device (blocking).
 * Returns bytes written, or negative errno on error. */
int audio_playback_write(audio_playback_t* pb,
    const void* buf, size_t len);

/* Force-stop an active playback from another thread.
 * Stops the underlying media_player and marks the handle so that
 * subsequent write() calls return -ECANCELED immediately.
 * Safe to call from any thread; safe if pb is NULL. */
void audio_playback_stop(audio_playback_t* pb);

/* Stop playback and release all resources. */
void audio_playback_close(audio_playback_t* pb);

/* [V846] True while the speaker is producing sound, and for 400 ms after it
 * stops.  The capture loop must drop those chunks instead of passing them to
 * the VAD: the microphone hears the speaker on this board, so otherwise every
 * TTS reply is transcribed as the next utterance and the device answers
 * itself in a loop. */
int audio_playback_echo_active(void);

#ifdef __cplusplus
}
#endif
