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
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Audio capture session handle (opaque). */
typedef struct audio_capture audio_capture_t;

/* Open the capture device and configure for PCM recording.
 * Returns NULL on failure. */
audio_capture_t *audio_capture_open(const char *dev_path,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample);

/* Start the audio hardware. Must be called after open. */
int audio_capture_start(audio_capture_t *cap);

/* Read PCM data from the capture device (blocking).
 * Returns bytes read, or negative errno on error. */
int audio_capture_read(audio_capture_t *cap,
    void *buf, size_t len);

/* Interrupt any in-flight blocking read without freeing the handle. */
int audio_capture_abort(audio_capture_t* cap);

/* Stop recording and release all resources. */
void audio_capture_close(audio_capture_t *cap);

#ifdef __cplusplus
}
#endif
