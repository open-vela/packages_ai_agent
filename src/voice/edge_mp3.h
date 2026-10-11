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

/* Streaming MP3 -> 16-bit PCM decoder for the Edge TTS backend.
 *
 * The Edge read-aloud endpoint refuses raw PCM output (it closes the
 * WebSocket with 1007 "Unsupported Edge output format") and only answers with
 * audio-24khz-48kbitrate-mono-mp3, so the device decodes it here.
 *
 * Decoder: minimp3 (CC0-1.0), vendored in ../thirdparty/minimp3.h.
 * Path:audio payloads are fed in as they arrive; MP3 frames split across
 * WebSocket frames are reassembled, ID3v2 tags are skipped. */

typedef void (*edge_mp3_pcm_cb)(const unsigned char *pcm, size_t len,
                                void *user_data);

typedef struct edge_mp3_s edge_mp3_t;

/* Allocate decoder state on the heap (about 27 KB, see edge_mp3.c).
 * Returns NULL when out of memory. */
edge_mp3_t *edge_mp3_open(edge_mp3_pcm_cb cb, void *user_data);

/* Feed one WebSocket audio payload.  Decoded PCM is passed to the callback
 * as soon as a complete MP3 frame is available.  Returns 0, or a negative
 * errno when the payload cannot be accepted at all. */
int edge_mp3_feed(edge_mp3_t *mp3, const unsigned char *data, size_t len);

/* Release everything. */
void edge_mp3_close(edge_mp3_t *mp3);

/* Statistics (for the log line after a turn). */
size_t edge_mp3_input_bytes(const edge_mp3_t *mp3);
size_t edge_mp3_pcm_bytes(const edge_mp3_t *mp3);
size_t edge_mp3_frames(const edge_mp3_t *mp3);
int edge_mp3_sample_rate(const edge_mp3_t *mp3);
int edge_mp3_channels(const edge_mp3_t *mp3);

#ifdef __cplusplus
}
#endif
