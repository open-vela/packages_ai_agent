/*
 * Copyright (c) 2026 Xiaomi Corporation
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

/* audio_playback.c — PCM playback straight off the NuttX audio device.
 *
 * Mirrors audio_capture.c: the ap_buffer queue of /dev/audio/pcm0p driven
 * directly, rather than through media_player and the ffmpeg graph behind
 * it.  The device runs 2 channels of 16 bit samples - the codec rejects
 * anything else - so a mono stream is duplicated into both halves of every
 * frame here, which is also what keeps the sound on both ears.
 */

#include "voice/audio_playback.h"
#include "agent_config.h"

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <nuttx/audio/audio.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <unistd.h>

static const char* TAG = "audio_pb";

#define PB_DEV_CHANNELS 2
#define PB_DEV_BITS     16
#define PB_DEV_FRAME    (PB_DEV_CHANNELS * PB_DEV_BITS / 8)

#define PB_QUEUE_MAX    16

struct audio_playback {
    int fd;
    mqd_t mq;
    char mqname[32];
    pthread_t thread;
    volatile bool running;
    volatile bool stopped;

    unsigned int in_channels;
    size_t in_frame; /* bytes per frame the caller hands over */

    struct ap_buffer_s** bufs;
    int nbufs;
    int buf_size;

    /* Buffers the device is done with, ready to be filled again. */
    struct ap_buffer_s* freeq[PB_QUEUE_MAX];
    int head;
    int tail;
    pthread_mutex_t lock;
    sem_t avail;

    /* Buffer currently being filled by audio_playback_write(). */
    struct ap_buffer_s* cur;
    size_t cur_off;
};

/* ── Free-buffer ring ───────────────────────────────────────── */

static void ring_push(struct audio_playback* pb, struct ap_buffer_s* apb)
{
    pthread_mutex_lock(&pb->lock);
    if ((pb->head + 1) % PB_QUEUE_MAX != pb->tail) {
        pb->freeq[pb->head] = apb;
        pb->head = (pb->head + 1) % PB_QUEUE_MAX;
        sem_post(&pb->avail);
    }
    pthread_mutex_unlock(&pb->lock);
}

static struct ap_buffer_s* ring_pop(struct audio_playback* pb)
{
    struct ap_buffer_s* apb = NULL;

    pthread_mutex_lock(&pb->lock);
    if (pb->tail != pb->head) {
        apb = pb->freeq[pb->tail];
        pb->tail = (pb->tail + 1) % PB_QUEUE_MAX;
    }
    pthread_mutex_unlock(&pb->lock);

    return apb;
}

/* ── Device thread: recycle drained buffers ─────────────────── */

static void* playback_thread(void* arg)
{
    struct audio_playback* pb = arg;
    struct audio_msg_s msg;
    unsigned int prio;

    while (pb->running) {
        ssize_t n = mq_receive(pb->mq, (char*)&msg, sizeof(msg), &prio);

        if (n != sizeof(msg)) {
            if (!pb->running) {
                break;
            }
            continue;
        }

        switch (msg.msg_id) {
        case AUDIO_MSG_DEQUEUE:
            if (msg.u.ptr) {
                ring_push(pb, (struct ap_buffer_s*)msg.u.ptr);
            }
            break;

        case AUDIO_MSG_STOP:
        case AUDIO_MSG_COMPLETE:
            pb->running = false;
            break;

        default:
            break;
        }
    }

    return NULL;
}

/* ── Frame conversion ───────────────────────────────────────── */

/* A mono caller is expanded into packed stereo frames with the sample in
 * both halves, so both outputs carry the same audio and the result does
 * not depend on which half the codec treats as the left channel.
 *
 * Returns the number of caller bytes consumed - not the number of device
 * bytes produced, which the caller derives from the frame count. */
static size_t frames_to_dev(struct audio_playback* pb,
    const unsigned char* src, size_t src_len,
    unsigned char* dst, size_t dst_cap)
{
    size_t nframes = src_len / pb->in_frame;
    size_t i;

    if (nframes * PB_DEV_FRAME > dst_cap) {
        nframes = dst_cap / PB_DEV_FRAME;
    }

    if (nframes == 0) {
        return 0;
    }

    if (pb->in_channels == PB_DEV_CHANNELS) {
        memcpy(dst, src, nframes * PB_DEV_FRAME);
    } else {
        for (i = 0; i < nframes; i++) {
            unsigned char lo = src[i * 2];
            unsigned char hi = src[i * 2 + 1];

            dst[i * 4] = lo;
            dst[i * 4 + 1] = hi;
            dst[i * 4 + 2] = lo;
            dst[i * 4 + 3] = hi;
        }
    }

    return nframes * pb->in_frame;
}

/* ── Device setup ───────────────────────────────────────────── */

static int playback_alloc_buffers(struct audio_playback* pb)
{
    struct audio_buf_desc_s desc;
    struct ap_buffer_info_s info;
    int i;

    if (ioctl(pb->fd, AUDIOIOC_GETBUFFERINFO, (uintptr_t)&info) != OK) {
        syslog(LOG_ERR, "[%s] GETBUFFERINFO failed: %d\n", TAG, errno);
        return -EIO;
    }

    if (info.nbuffers > PB_QUEUE_MAX) {
        info.nbuffers = PB_QUEUE_MAX;
    }

    pb->nbufs = info.nbuffers;
    pb->buf_size = info.buffer_size;
    pb->bufs = calloc(pb->nbufs, sizeof(struct ap_buffer_s*));

    if (!pb->bufs) {
        return -ENOMEM;
    }

    for (i = 0; i < pb->nbufs; i++) {
        memset(&desc, 0, sizeof(desc));
        desc.numbytes = pb->buf_size;
        desc.u.pbuffer = &pb->bufs[i];

        if (ioctl(pb->fd, AUDIOIOC_ALLOCBUFFER, (uintptr_t)&desc)
                != sizeof(desc)) {
            syslog(LOG_ERR, "[%s] ALLOCBUFFER %d failed\n", TAG, i);
            return -ENOMEM;
        }

        ring_push(pb, pb->bufs[i]);
    }

    return 0;
}

static void playback_free_buffers(struct audio_playback* pb)
{
    struct audio_buf_desc_s desc;
    int i;

    if (!pb->bufs) {
        return;
    }

    for (i = 0; i < pb->nbufs; i++) {
        if (pb->bufs[i]) {
            memset(&desc, 0, sizeof(desc));
            desc.u.buffer = pb->bufs[i];
            ioctl(pb->fd, AUDIOIOC_FREEBUFFER, (uintptr_t)&desc);
        }
    }

    free(pb->bufs);
    pb->bufs = NULL;
}

static int playback_enqueue(struct audio_playback* pb,
    struct ap_buffer_s* apb)
{
    struct audio_buf_desc_s desc;

    apb->curbyte = 0;
    apb->flags = 0;

    memset(&desc, 0, sizeof(desc));
    desc.numbytes = apb->nbytes;
    desc.u.buffer = apb;

    return ioctl(pb->fd, AUDIOIOC_ENQUEUEBUFFER, (uintptr_t)&desc) < 0
        ? -errno
        : 0;
}

audio_playback_t* audio_playback_open(const char* dev_path,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample)
{
    struct audio_caps_desc_s cap_desc;
    struct mq_attr attr;
    audio_playback_t* pb;

    if (!dev_path || !sample_rate) {
        return NULL;
    }

    if (bits_per_sample != 0 && bits_per_sample != PB_DEV_BITS) {
        syslog(LOG_ERR, "[%s] only %d bit samples supported, got %u\n",
            TAG, PB_DEV_BITS, bits_per_sample);
        return NULL;
    }

    if (channels != 1 && channels != 2) {
        syslog(LOG_ERR, "[%s] bad channel count %u\n", TAG, channels);
        return NULL;
    }

    pb = calloc(1, sizeof(*pb));
    if (!pb) {
        return NULL;
    }

    pb->fd = -1;
    pb->mq = (mqd_t)-1;
    pb->in_channels = channels;
    pb->in_frame = channels * (PB_DEV_BITS / 8);
    pthread_mutex_init(&pb->lock, NULL);
    sem_init(&pb->avail, 0, 0);

    pb->fd = open(dev_path, O_RDWR | O_CLOEXEC);
    if (pb->fd < 0) {
        syslog(LOG_ERR, "[%s] open %s failed: %d\n", TAG, dev_path, errno);
        goto err_out;
    }

    memset(&cap_desc, 0, sizeof(cap_desc));
    cap_desc.caps.ac_len = sizeof(struct audio_caps_s);
    cap_desc.caps.ac_type = AUDIO_TYPE_OUTPUT;
    cap_desc.caps.ac_channels = PB_DEV_CHANNELS;
    cap_desc.caps.ac_controls.hw[0] = sample_rate;
    cap_desc.caps.ac_controls.b[3] = sample_rate >> 16;
    cap_desc.caps.ac_controls.b[2] = PB_DEV_BITS;

    if (ioctl(pb->fd, AUDIOIOC_CONFIGURE, (uintptr_t)&cap_desc) < 0) {
        syslog(LOG_ERR, "[%s] CONFIGURE %uHz failed: %d\n",
            TAG, sample_rate, errno);
        goto err_out;
    }

    if (ioctl(pb->fd, AUDIOIOC_RESERVE, 0) < 0) {
        syslog(LOG_ERR, "[%s] RESERVE failed: %d\n", TAG, errno);
        goto err_out;
    }

    if (playback_alloc_buffers(pb) != 0) {
        goto err_out;
    }

    attr.mq_maxmsg = pb->nbufs + 8;
    attr.mq_msgsize = sizeof(struct audio_msg_s);
    attr.mq_curmsgs = 0;
    attr.mq_flags = 0;

    snprintf(pb->mqname, sizeof(pb->mqname), "/tmp/pb%0lx",
        (unsigned long)((uintptr_t)pb));
    mq_unlink(pb->mqname);

    pb->mq = mq_open(pb->mqname, O_RDWR | O_CREAT, 0644, &attr);
    if (pb->mq == (mqd_t)-1) {
        syslog(LOG_ERR, "[%s] mq_open failed: %d\n", TAG, errno);
        goto err_out;
    }

    ioctl(pb->fd, AUDIOIOC_REGISTERMQ, (uintptr_t)pb->mq);

    syslog(LOG_INFO,
        "[%s] open %s: %uHz %uch -> device %dHz %dch, %d x %d byte buffers\n",
        TAG, dev_path, sample_rate, channels,
        sample_rate, PB_DEV_CHANNELS, pb->nbufs, pb->buf_size);

    pb->running = true;

    if (pthread_create(&pb->thread, NULL, playback_thread, pb) != 0) {
        pb->running = false;
        goto err_out;
    }

    /* The device idles until the first buffer is enqueued, so starting it
     * here costs nothing and saves a state transition on the first write. */

    if (ioctl(pb->fd, AUDIOIOC_START, 0) < 0) {
        syslog(LOG_ERR, "[%s] START failed: %d\n", TAG, errno);
        pb->running = false;
        pthread_join(pb->thread, NULL);
        goto err_out;
    }

    return pb;

err_out:
    audio_playback_close(pb);
    return NULL;
}

int audio_playback_write(audio_playback_t* pb, const void* buf, size_t len)
{
    const unsigned char* in = buf;
    size_t done = 0;

    if (!pb || !buf) {
        return -EINVAL;
    }

    if (pb->stopped) {
        return -ECANCELED;
    }

    while (done < len) {
        size_t consumed;

        if (!pb->cur) {
            while (sem_wait(&pb->avail) != 0) {
                if (errno != EINTR) {
                    return done > 0 ? (int)done : -errno;
                }
            }

            if (pb->stopped) {
                return done > 0 ? (int)done : -ECANCELED;
            }

            pb->cur = ring_pop(pb);
            pb->cur_off = 0;

            if (!pb->cur) {
                continue;
            }

            pb->cur->nbytes = 0;
        }

        consumed = frames_to_dev(pb, in + done, len - done,
            &pb->cur->samp[pb->cur_off],
            pb->cur->nmaxbytes - pb->cur_off);

        if (consumed == 0) {
            /* Either the buffer has no room for a whole frame, or the
             * caller's tail is a partial frame.  Flush in the first case,
             * return early in the second; both avoid spinning here. */

            if (pb->cur->nmaxbytes - pb->cur_off < PB_DEV_FRAME) {
                pb->cur->nbytes = pb->cur_off;
                playback_enqueue(pb, pb->cur);
                pb->cur = NULL;
                continue;
            }

            break;
        }

        pb->cur_off += consumed / pb->in_frame * PB_DEV_FRAME;
        done += consumed;

        if (pb->cur_off >= pb->cur->nmaxbytes) {
            pb->cur->nbytes = pb->cur->nmaxbytes;
            playback_enqueue(pb, pb->cur);
            pb->cur = NULL;
        }
    }

    return (int)done;
}

void audio_playback_stop(audio_playback_t* pb)
{
    struct audio_msg_s msg;

    if (!pb) {
        return;
    }

    pb->stopped = true;

    if (pb->fd >= 0) {
        ioctl(pb->fd, AUDIOIOC_STOP, 0);
    }

    if (pb->mq != (mqd_t)-1) {
        memset(&msg, 0, sizeof(msg));
        msg.msg_id = AUDIO_MSG_STOP;
        mq_send(pb->mq, (const char*)&msg, sizeof(msg), 0);
    }

    /* Unblock a writer waiting for a buffer. */
    sem_post(&pb->avail);
}

void audio_playback_close(audio_playback_t* pb)
{
    if (!pb) {
        return;
    }

    /* Flush the tail first: a short utterance leaves a partly filled
     * buffer behind, and dropping it clips the end of the speech. */

    if (pb->cur) {
        if (pb->cur_off > 0 && !pb->stopped) {
            pb->cur->nbytes = pb->cur_off;
            playback_enqueue(pb, pb->cur);
        }
        pb->cur = NULL;
    }

    audio_playback_stop(pb);
    pb->running = false;

    if (pb->thread) {
        pthread_join(pb->thread, NULL);
    }

    playback_free_buffers(pb);

    if (pb->mq != (mqd_t)-1) {
        if (pb->fd >= 0) {
            ioctl(pb->fd, AUDIOIOC_UNREGISTERMQ, (uintptr_t)pb->mq);
        }
        mq_close(pb->mq);
        mq_unlink(pb->mqname);
    }

    if (pb->fd >= 0) {
        ioctl(pb->fd, AUDIOIOC_RELEASE, 0);
        close(pb->fd);
    }

    sem_destroy(&pb->avail);
    pthread_mutex_destroy(&pb->lock);
    free(pb);
}
