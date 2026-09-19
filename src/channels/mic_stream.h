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

/* Stream the on-board microphone to a WebSocket client.
 *
 * The device captures 8 kHz / 16-bit / mono PCM, compresses it to G.711
 * mu-law and pushes it as binary WebSocket frames; the far end is expected to
 * do the recognition. Keeping ASR off the device is deliberate -- the same
 * audio then feeds whatever recogniser the host has, including ones that
 * would never fit in the firmware, and the device stays a capture endpoint.
 */

/* Largest chunk the stream will send in one frame, in samples. Kept under
 * the encoder's 65535 byte frame limit with room to spare.
 */

#define MIC_STREAM_MAX_SAMPLES 8192

/* Upper bound on one frame, in samples; the capture driver decides how much
 * of it to use. It must stay strictly below the DMA ring --
 * SF32LB_AUDIO_DMA_WORDS, currently 8192 -- because the reader tracks its
 * position modulo the ring, so an advance of exactly one ring reads back as
 * no advance at all and capture stalls with no error anywhere.
 *
 * The ceiling only bites when a backlog has built up -- the driver returns as
 * soon as it has a useful amount, not when it has filled the request -- and
 * that is exactly when frames come out largest, so it is the backlog case
 * that needs bounding.
 *
 * 1024 rather than the largest that fits: every writer on a client's socket
 * (this stream, agent replies, keepalive pongs) serialises on one mutex, and
 * a send is a blocked send for as long as a frame takes to shift over a
 * ~6 KB/s link -- about 170 ms here against ~700 ms at 4096. That bounds how
 * long one slow writer can hold the socket against the others.
 *
 * Measured caveat: shortening this did NOT stop the far end missing
 * keepalives under a heavy agent load. The dominant cause there turned out to
 * be a *different* WS client that stops reading, which parks the shared send
 * mutex inside send() and starves every other writer regardless of frame
 * size. Bounding the frame is still worth doing, it just is not the fix for
 * that.
 */

#define MIC_STREAM_DEF_SAMPLES 1024

int mic_stream_start(const char* chat_id, int samples);
int mic_stream_stop(void);
bool mic_stream_is_running(void);
