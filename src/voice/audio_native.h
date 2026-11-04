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

/* audio_native.h - Internal declarations for the native NuttX audio
 * character-device backends.  Not installed; included only by the voice/
 * backend sources (audio_native.c, audio_playback_native.c,
 * audio_capture_native.c) and dispatched to from audio_playback.c /
 * audio_capture.c when CONFIG_AI_AGENT_AUDIO_NATIVE is enabled.
 *
 * The native backends talk directly to the raw audio devices registered by
 * board bring-up (e.g. /dev/audio/pcm0p / pcm0c on esp32s3-eye + ES8311)
 * using the standard NuttX audio API:
 *
 *   open(O_RDWR) -> AUDIOIOC_GETCAPS -> AUDIOIOC_RESERVE
 *   -> mq_open + AUDIOIOC_REGISTERMQ -> AUDIOIOC_CONFIGURE
 *   -> AUDIOIOC_GETBUFFERINFO + AUDIOIOC_ALLOCBUFFER
 *   -> AUDIOIOC_ENQUEUEBUFFER -> AUDIOIOC_START
 *   -> mq_receive(AUDIO_MSG_DEQUEUE / _COMPLETE) ... -> AUDIOIOC_STOP
 *   -> AUDIOIOC_RELEASE -> AUDIOIOC_FREEBUFFER -> close.
 */

#ifndef __AUDIO_NATIVE_H
#define __AUDIO_NATIVE_H

#include <stddef.h>
#include <stdint.h>

/* Message-queue parameters shared by the native backends. */
#define NATIVE_MQ_MAXMSG     16
#define NATIVE_MQ_NAME_LEN   24

#include <mqueue.h>
#include <nuttx/audio/audio.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct native_pb_s  native_pb_t;   /* native playback stream  */
typedef struct native_cap_s native_cap_t;  /* native capture stream   */

/* ── Shared helpers (audio_native.c) ────────────────────────────────── */

/* Allocate a unique POSIX message queue for buffer-completion events.
 * name_buf receives the queue name (needed for mq_unlink at teardown).
 * Returns 0 on success; negated errno on failure. */

int native_mq_create(char* name_buf, size_t name_size, mqd_t* out_mq);

/* Wait for one audio message with a timeout.  Returns 1 when a message
 * was received (copied into *msg), 0 on timeout, -errno on error. */

int native_mq_receive_timed(mqd_t mq, struct audio_msg_s* msg,
    unsigned int timeout_sec);

/* Verify the opened device is a NuttX audio device supporting want_type
 * (AUDIO_TYPE_OUTPUT / AUDIO_TYPE_INPUT).  0 on success. */

int native_check_caps(int fd, uint8_t want_type);

/* Configure samplerate / channels / bits for the given direction. */

int native_configure(int fd, uint8_t dev_type, unsigned int sample_rate,
    unsigned int channels, unsigned int bits_per_sample);

/* Allocate/free an audio pipeline buffer through the driver. */

struct ap_buffer_s* native_alloc_buffer(int fd, size_t size);
void native_free_buffer(int fd, struct ap_buffer_s* apb);

/* Enqueue a filled (playback) or armed-empty (capture) buffer. */

int native_enqueue_buffer(int fd, struct ap_buffer_s* apb);

/* ── Playback: raw PCM into /dev/audio/pcmXp ──────────────────────────
 * write() blocks while the pipeline is full, mirroring a pipe: buffer
 * completions are reclaimed from the registered message queue.  Returns
 * the number of bytes accepted, or a negated errno on failure
 * (-ECANCELED after stop(), -EPIPE if the driver completed the stream).
 */

int native_playback_open(native_pb_t** out_pb, const char* dev_path,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample);
int native_playback_write(native_pb_t* pb, const void* buf, size_t len);
void native_playback_stop(native_pb_t* pb);
void native_playback_close(native_pb_t* pb);

/* ── Capture: raw PCM out of /dev/audio/pcmXc ─────────────────────────
 * start() primes the pipeline with empty buffers and starts the driver.
 * read() blocks until a filled buffer is returned by the driver; data is
 * copied out and the buffer is re-armed so the pipeline stays full.
 * Returns the number of bytes produced, 0 on timeout-less drain end, or
 * a negated errno (-ECANCELED after abort()).
 */

int native_capture_open(native_cap_t** out_cap, const char* dev_path,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample);
int native_capture_start(native_cap_t* cap);
int native_capture_read(native_cap_t* cap, void* buf, size_t len);
int native_capture_abort(native_cap_t* cap);
void native_capture_close(native_cap_t* cap);

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_NATIVE_H */
