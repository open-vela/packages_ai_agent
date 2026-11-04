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

/* audio_native.c - Shared helpers for the native NuttX audio backends.
 *
 * Encapsulates the common session setup/teardown performed against the
 * audio upper-half driver (nuttx/audio/audio.c): device open/close,
 * capability query, format configure, buffer allocation, and the private
 * message queue used to receive AUDIO_MSG_DEQUEUE / AUDIO_MSG_COMPLETE
 * notifications.
 */

#include "audio_native.h"

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <nuttx/audio/audio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>


/* Allocate a unique POSIX message queue for buffer-completion events.
 * Returns 0 on success with *out_mq set and name_buf filled (needed for
 * mq_unlink at teardown); negated errno on failure. */

int native_mq_create(char* name_buf, size_t name_size, mqd_t* out_mq)
{
    static unsigned int seq;
    struct mq_attr attr;

    memset(&attr, 0, sizeof(attr));
    attr.mq_flags = 0;
    attr.mq_maxmsg = NATIVE_MQ_MAXMSG;
    attr.mq_msgsize = sizeof(struct audio_msg_s);
    attr.mq_curmsgs = 0;

    snprintf(name_buf, name_size, "/tmp/agentaud%u", ++seq);

    *out_mq = mq_open(name_buf, O_RDWR | O_CREAT, 0644, &attr);
    if (*out_mq < 0) {
        return -errno;
    }

    return 0;
}

/* Wait for one audio message with a timeout.  Returns 1 when a message
 * was received (copied into *msg), 0 on timeout, -errno on error. */

int native_mq_receive_timed(mqd_t mq, struct audio_msg_s* msg,
    unsigned int timeout_sec)
{
    char rbuf[sizeof(struct audio_msg_s)];
    struct timespec ts;
    unsigned int prio;
    ssize_t n;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)timeout_sec;

    n = mq_timedreceive(mq, rbuf, sizeof(rbuf), &prio, &ts);
    if (n < 0) {
        return (errno == ETIMEDOUT || errno == EAGAIN) ? 0 : -errno;
    }

    if ((size_t)n != sizeof(*msg)) {
        return 0;
    }

    memcpy(msg, rbuf, sizeof(*msg));
    return 1;
}

/* Verify the opened device really is a NuttX audio device supporting the
 * requested direction (AUDIO_TYPE_OUTPUT / AUDIO_TYPE_INPUT).  Returns 0
 * on success, negated errno on failure. */

int native_check_caps(int fd, uint8_t want_type)
{
    struct audio_caps_s caps;
    int ret;

    memset(&caps, 0, sizeof(caps));
    caps.ac_len = sizeof(caps);
    caps.ac_type = AUDIO_TYPE_QUERY;
    caps.ac_subtype = AUDIO_TYPE_QUERY;

    ret = ioctl(fd, AUDIOIOC_GETCAPS, (unsigned long)&caps);
    if (ret != caps.ac_len) {
        return -ENODEV;
    }

    if (!(caps.ac_controls.b[0] & want_type)) {
        return -ENODEV;
    }

    return 0;
}

/* Configure samplerate / channels / bits for the given direction.
 * Field mapping per the es8311 lower half (drivers/audio/es8311.c):
 *   ac_type            = AUDIO_TYPE_OUTPUT or AUDIO_TYPE_INPUT
 *   ac_channels        = channels
 *   ac_controls.hw[0]  = sample rate
 *   ac_controls.b[2]   = bits per sample
 * CONFIGURE takes a struct audio_caps_desc_s wrapper (audio/audio.c). */

int native_configure(int fd, uint8_t dev_type, unsigned int sample_rate,
    unsigned int channels, unsigned int bits_per_sample)
{
    struct audio_caps_desc_s desc;

    memset(&desc, 0, sizeof(desc));
    desc.caps.ac_len = sizeof(struct audio_caps_s);
    desc.caps.ac_type = dev_type;
    desc.caps.ac_channels = (uint8_t)channels;
    desc.caps.ac_controls.hw[0] = (uint16_t)sample_rate;
    desc.caps.ac_controls.b[2] = (uint8_t)bits_per_sample;

    if (ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&desc) < 0) {
        return -errno;
    }

    return 0;
}

/* Allocate an audio pipeline buffer of the given size through the driver.
 * Returns the apb pointer or NULL. */

struct ap_buffer_s* native_alloc_buffer(int fd, size_t size)
{
    struct audio_buf_desc_s desc;
    struct ap_buffer_s* apb = NULL;

    memset(&desc, 0, sizeof(desc));
    desc.numbytes = (apb_samp_t)size;
    desc.u.pbuffer = &apb;

    if (ioctl(fd, AUDIOIOC_ALLOCBUFFER, (unsigned long)&desc) !=
        (int)sizeof(desc)) {
        return NULL;
    }

    return apb;
}

/* Free a pipeline buffer previously obtained via native_alloc_buffer. */

void native_free_buffer(int fd, struct ap_buffer_s* apb)
{
    struct audio_buf_desc_s desc;

    if (!apb) {
        return;
    }

    memset(&desc, 0, sizeof(desc));
    desc.u.buffer = apb;
    ioctl(fd, AUDIOIOC_FREEBUFFER, (unsigned long)&desc);
}

/* Enqueue a filled (playback) or armed-empty (capture) buffer. */

int native_enqueue_buffer(int fd, struct ap_buffer_s* apb)
{
    struct audio_buf_desc_s desc;

    memset(&desc, 0, sizeof(desc));
    desc.u.buffer = apb;

    if (ioctl(fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&desc) != OK) {
        return -errno;
    }

    return 0;
}
