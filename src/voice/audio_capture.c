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

/* audio_capture.c — PCM capture straight off the NuttX audio device.
 *
 * This drives the ap_buffer queue of /dev/audio/pcm0c directly.  The
 * media_recorder it replaces is built on audio_graph, which pulls in the
 * whole of ffmpeg; the ES8388 lower half only implements the buffer queue
 * anyway (audio_read is a stub), so this is the shortest path to working
 * capture with no extra dependencies.
 *
 * The device always runs 2 channels of 16 bit samples - the codec rejects
 * anything else - and the caller's channel count is produced by folding or
 * copying frames here.  The sample rate is handed to the device unchanged;
 * the I2S derives MCLK as 256*fs so any rate is programmable.
 */

#include "voice/audio_capture.h"
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

static const char* TAG = "audio_cap";

#define CAP_DEV_RATE     48000
#define CAP_DEV_CHANNELS 2
#define CAP_DEV_BITS     16
#define CAP_DEV_FRAME    (CAP_DEV_CHANNELS * CAP_DEV_BITS / 8)

/* Where a filled buffer waits for the reader.  One slot per device buffer
 * is enough: a buffer only becomes filled after it was enqueued. */
#define CAP_QUEUE_MAX    16

struct audio_capture {
    int fd;
    mqd_t mq;
    char mqname[32];
    pthread_t thread;
    volatile bool running;

    unsigned int out_channels;
    unsigned int out_rate;
    unsigned int decim; /* device frames per output frame */
    size_t out_frame;   /* bytes per frame the caller wants */

    struct ap_buffer_s** bufs;
    int nbufs;
    int buf_size;

    /* Filled buffers handed from the device thread to the reader. */
    struct ap_buffer_s* filled[CAP_QUEUE_MAX];
    int head;
    int tail;
    pthread_mutex_t lock;
    sem_t avail;

    /* Buffer currently being drained by audio_capture_read(). */
    struct ap_buffer_s* cur;
    size_t cur_off;

    /* When set, the device thread writes each buffer here directly and
     * recycles it, exactly like nxrecorder's record thread. */
    int sink_fd;
    unsigned sink_buffers;
    unsigned sink_bytes;
};

static int capture_alloc_buffers(struct audio_capture* cap);
static int capture_enqueue(struct audio_capture* cap,
    struct ap_buffer_s* apb);

/* ── Filled-buffer ring ─────────────────────────────────────── */

static void ring_push(struct audio_capture* cap, struct ap_buffer_s* apb)
{
    pthread_mutex_lock(&cap->lock);
    if ((cap->head + 1) % CAP_QUEUE_MAX != cap->tail) {
        cap->filled[cap->head] = apb;
        cap->head = (cap->head + 1) % CAP_QUEUE_MAX;
        sem_post(&cap->avail);
    }
    pthread_mutex_unlock(&cap->lock);
}

static struct ap_buffer_s* ring_pop(struct audio_capture* cap)
{
    struct ap_buffer_s* apb = NULL;

    pthread_mutex_lock(&cap->lock);
    if (cap->tail != cap->head) {
        apb = cap->filled[cap->tail];
        cap->tail = (cap->tail + 1) % CAP_QUEUE_MAX;
    }
    pthread_mutex_unlock(&cap->lock);

    return apb;
}

/* ── Device thread: collect filled buffers ──────────────────── */

static void* capture_thread(void* arg)
{
    struct audio_capture* cap = arg;
    struct audio_msg_s msg;
    unsigned int prio;
    int i;

    /* nxrecorder's record thread owns the whole pipeline setup: query the
     * buffer geometry, allocate, prime the queue and only then start the
     * device.  Doing any of it from the caller's context is a difference
     * worth not having. */

    if (capture_alloc_buffers(cap) != 0) {
        syslog(LOG_ERR, "[%s] buffer allocation failed\n", TAG);
        return NULL;
    }

    for (i = 0; i < cap->nbufs; i++) {
        if (capture_enqueue(cap, cap->bufs[i]) != 0) {
            syslog(LOG_ERR, "[%s] enqueue %d failed: %d\n",
                TAG, i, errno);
            return NULL;
        }
    }

    if (ioctl(cap->fd, AUDIOIOC_START, 0) < 0) {
        syslog(LOG_ERR, "[%s] START failed: %d\n", TAG, errno);
        return NULL;
    }

    while (cap->running) {
        struct timespec ts;
        ssize_t n;

        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        n = mq_timedreceive(cap->mq, (char*)&msg, sizeof(msg), &prio, &ts);

        if (n < 0 && (errno == ETIMEDOUT || errno == EAGAIN)) {
            continue;
        }

        if (n != sizeof(msg)) {
            if (!cap->running) {
                break;
            }
            continue;
        }

        switch (msg.msg_id) {
        case AUDIO_MSG_DEQUEUE:
            if (!msg.u.ptr) {
                break;
            }

            if (cap->sink_fd >= 0) {
                struct ap_buffer_s* apb = (struct ap_buffer_s*)msg.u.ptr;

                if (write(cap->sink_fd, apb->samp, apb->nbytes)
                        != (ssize_t)apb->nbytes) {
                    syslog(LOG_ERR, "[%s] sink write failed: %d\n",
                        TAG, errno);
                }

                if (cap->sink_buffers < 4) {
                    syslog(LOG_INFO,
                        "[%s] sink buf#%u: %u bytes from %p\n",
                        TAG, cap->sink_buffers, apb->nbytes,
                        (void*)apb->samp);
                }

                /* Dump words straight out of the buffer, before they go
                 * to the file: if these are clean and the file is not, the
                 * damage happens on the way to the card.  One buffer from
                 * the start and one from the middle - the middle is where
                 * the audio actually is, the first buffer is still the
                 * codec's all-ones idle pattern. */
                if (cap->sink_buffers == 0 || cap->sink_buffers == 6) {
                    unsigned char* w = apb->samp;
                    unsigned k;
                    size_t base = (cap->sink_buffers == 6) ? 16384 : 0;

                    syslog(LOG_INFO, "[%s] apb buf#%u words at %zu:",
                        TAG, cap->sink_buffers, base);
                    for (k = 0; k < 16 &&
                         base + (k + 1) * 4 <= apb->nbytes; k++) {
                        syslog(LOG_INFO, " %02x%02x%02x%02x",
                            w[base + k * 4 + 3], w[base + k * 4 + 2],
                            w[base + k * 4 + 1], w[base + k * 4]);
                    }
                }

                cap->sink_buffers++;
                cap->sink_bytes += apb->nbytes;
                capture_enqueue(cap, apb);
            } else {
                ring_push(cap, (struct ap_buffer_s*)msg.u.ptr);
            }
            break;

        case AUDIO_MSG_STOP:
            cap->running = false;
            break;

        case AUDIO_MSG_COMPLETE:
            /* es8388 emits COMPLETE after every buffer, not just at the
             * end of the stream - it is not a terminator here. */
            break;

        default:
            break;
        }
    }

    return NULL;
}

/* ── Frame conversion ───────────────────────────────────────── */

/* The device always runs at 48kHz stereo - the parameters the nxrecorder
 * "record" command uses, which is the one combination known to produce
 * clean audio on this board.  Capturing at 16kHz instead comes out as
 * loud noise, so a slower caller is box-filtered down here instead.  The
 * channel count is folded at the same time, and a mono caller gets the
 * left slot: that is where the microphone ends up, and averaging the
 * grounded right slot in would only add its noise. */
static size_t frames_to_out(struct audio_capture* cap,
    const unsigned char* src, size_t src_len,
    unsigned char* dst, size_t dst_cap)
{
    size_t n_in = src_len / CAP_DEV_FRAME;
    size_t n_out = n_in / cap->decim;
    size_t i;

    if (n_out * cap->out_frame > dst_cap) {
        n_out = dst_cap / cap->out_frame;
    }

    if (n_out == 0) {
        return 0;
    }

    if (cap->decim == 1 && cap->out_channels == CAP_DEV_CHANNELS) {
        memcpy(dst, src, n_out * CAP_DEV_FRAME);
        return n_out * CAP_DEV_FRAME;
    }

    for (i = 0; i < n_out; i++) {
        size_t f0 = i * cap->decim;
        int32_t left = 0;
        int32_t right = 0;
        size_t f;

        for (f = 0; f < cap->decim; f++) {
            const unsigned char* p = src + (f0 + f) * CAP_DEV_FRAME;

            left += (int16_t)(p[0] | (p[1] << 8));
            right += (int16_t)(p[2] | (p[3] << 8));
        }

        left /= (int32_t)cap->decim;
        right /= (int32_t)cap->decim;

        if (cap->out_channels == 2) {
            dst[i * 4] = (unsigned char)(left & 0xff);
            dst[i * 4 + 1] = (unsigned char)((left >> 8) & 0xff);
            dst[i * 4 + 2] = (unsigned char)(right & 0xff);
            dst[i * 4 + 3] = (unsigned char)((right >> 8) & 0xff);
        } else {
            dst[i * 2] = (unsigned char)(left & 0xff);
            dst[i * 2 + 1] = (unsigned char)((left >> 8) & 0xff);
        }
    }

    return n_out * cap->out_frame;
}

/* ── Device setup ───────────────────────────────────────────── */

static int capture_alloc_buffers(struct audio_capture* cap)
{
    struct audio_buf_desc_s desc;
    struct ap_buffer_info_s info;
    int i;

    if (ioctl(cap->fd, AUDIOIOC_GETBUFFERINFO, (uintptr_t)&info) != OK) {
        syslog(LOG_ERR, "[%s] GETBUFFERINFO failed: %d\n", TAG, errno);
        return -EIO;
    }

    if (info.nbuffers > CAP_QUEUE_MAX) {
        info.nbuffers = CAP_QUEUE_MAX;
    }

    if (info.nbuffers > 0 && info.buffer_size > 0) {
        cap->nbufs = info.nbuffers;
        cap->buf_size = info.buffer_size;
    }

    syslog(LOG_INFO, "[%s] allocating %d x %d byte buffers\n",
        TAG, cap->nbufs, cap->buf_size);
    cap->bufs = calloc(cap->nbufs, sizeof(struct ap_buffer_s*));

    if (!cap->bufs) {
        return -ENOMEM;
    }

    for (i = 0; i < cap->nbufs; i++) {
        memset(&desc, 0, sizeof(desc));
        desc.numbytes = cap->buf_size;
        desc.u.pbuffer = &cap->bufs[i];

        if (ioctl(cap->fd, AUDIOIOC_ALLOCBUFFER, (uintptr_t)&desc)
                != sizeof(desc)) {
            syslog(LOG_ERR, "[%s] ALLOCBUFFER %d failed\n", TAG, i);
            return -ENOMEM;
        }
    }

    return 0;
}

static void capture_free_buffers(struct audio_capture* cap)
{
    struct audio_buf_desc_s desc;
    int i;

    if (!cap->bufs) {
        return;
    }

    for (i = 0; i < cap->nbufs; i++) {
        if (cap->bufs[i]) {
            memset(&desc, 0, sizeof(desc));
            desc.u.buffer = cap->bufs[i];
            ioctl(cap->fd, AUDIOIOC_FREEBUFFER, (uintptr_t)&desc);
        }
    }

    free(cap->bufs);
    cap->bufs = NULL;
}

static int capture_enqueue(struct audio_capture* cap,
    struct ap_buffer_s* apb)
{
    struct audio_buf_desc_s desc;

    apb->nbytes = apb->nmaxbytes;
    apb->curbyte = 0;
    apb->flags = 0;

    memset(&desc, 0, sizeof(desc));
    desc.numbytes = apb->nbytes;
    desc.u.buffer = apb;

    return ioctl(cap->fd, AUDIOIOC_ENQUEUEBUFFER, (uintptr_t)&desc) < 0
        ? -errno
        : 0;
}

audio_capture_t* audio_capture_open(const char* dev_path,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample)
{
    struct audio_caps_desc_s cap_desc;
    struct mq_attr attr;
    audio_capture_t* cap;

    if (!dev_path || !sample_rate) {
        return NULL;
    }

    if (CAP_DEV_RATE % sample_rate != 0) {
        syslog(LOG_ERR,
            "[%s] sample rate %u must divide the device rate %d\n",
            TAG, sample_rate, CAP_DEV_RATE);
        return NULL;
    }

    if (bits_per_sample != 0 && bits_per_sample != CAP_DEV_BITS) {
        syslog(LOG_ERR, "[%s] only %d bit samples supported, got %u\n",
            TAG, CAP_DEV_BITS, bits_per_sample);
        return NULL;
    }

    if (channels != 1 && channels != 2) {
        syslog(LOG_ERR, "[%s] bad channel count %u\n", TAG, channels);
        return NULL;
    }

    cap = calloc(1, sizeof(*cap));
    if (!cap) {
        return NULL;
    }

    cap->fd = -1;
    cap->mq = (mqd_t)-1;
    cap->sink_fd = -1;
    cap->out_channels = channels;
    cap->out_rate = sample_rate;
    cap->decim = CAP_DEV_RATE / sample_rate;
    cap->out_frame = channels * (CAP_DEV_BITS / 8);
    pthread_mutex_init(&cap->lock, NULL);
    sem_init(&cap->avail, 0, 0);

    cap->fd = open(dev_path, O_RDWR | O_CLOEXEC);
    if (cap->fd < 0) {
        syslog(LOG_ERR, "[%s] open %s failed: %d\n", TAG, dev_path, errno);
        goto err_out;
    }

    /* The codec only takes stereo, so the device is configured for two
     * channels whatever the caller asked for. */

    /* The order below is nxrecorder's, byte for byte: RESERVE before
     * CONFIGURE, and the buffers are not allocated until start().  Doing
     * CONFIGURE first produced a stream whose upper half words were
     * corrupted every fourth frame, for reasons that only the driver's
     * reserve path explains. */

    if (ioctl(cap->fd, AUDIOIOC_RESERVE, 0) < 0) {
        syslog(LOG_ERR, "[%s] RESERVE failed: %d\n", TAG, errno);
        goto err_out;
    }

    {
        struct audio_caps_s q;

        memset(&q, 0, sizeof(q));
        q.ac_len = sizeof(q);
        q.ac_type = AUDIO_TYPE_INPUT;
        q.ac_subtype = AUDIO_TYPE_QUERY;
        ioctl(cap->fd, AUDIOIOC_GETCAPS, (uintptr_t)&q);
    }

    memset(&cap_desc, 0, sizeof(cap_desc));
    cap_desc.caps.ac_len = sizeof(struct audio_caps_s);
    cap_desc.caps.ac_type = AUDIO_TYPE_INPUT;
    cap_desc.caps.ac_channels = CAP_DEV_CHANNELS;
    cap_desc.caps.ac_chmap = 0;
    cap_desc.caps.ac_controls.hw[0] = CAP_DEV_RATE;
    cap_desc.caps.ac_controls.b[3] = CAP_DEV_RATE >> 16;
    cap_desc.caps.ac_controls.b[2] = CAP_DEV_BITS;
    cap_desc.caps.ac_subtype = AUDIO_FMT_UNDEF;

    if (ioctl(cap->fd, AUDIOIOC_CONFIGURE, (uintptr_t)&cap_desc) < 0) {
        syslog(LOG_ERR, "[%s] CONFIGURE %dHz failed: %d\n",
            TAG, CAP_DEV_RATE, errno);
        goto err_out;
    }

    /* The queue has to be big enough for every buffer the driver can hand
     * back while it is shutting down, so the buffer geometry is queried
     * here - before the queue exists - and not only in the device thread. */
    {
        struct ap_buffer_info_s info;

        if (ioctl(cap->fd, AUDIOIOC_GETBUFFERINFO, (uintptr_t)&info) == OK) {
            cap->nbufs = info.nbuffers;
            cap->buf_size = info.buffer_size;
        } else {
            cap->nbufs = 8;
            cap->buf_size = 32768;
        }
    }

    attr.mq_maxmsg = cap->nbufs + 8;
    attr.mq_msgsize = sizeof(struct audio_msg_s);
    attr.mq_curmsgs = 0;
    attr.mq_flags = 0;

    snprintf(cap->mqname, sizeof(cap->mqname), "/tmp/cap%0lx",
        (unsigned long)((uintptr_t)cap));
    mq_unlink(cap->mqname);

    cap->mq = mq_open(cap->mqname, O_RDWR | O_CREAT, 0644, &attr);
    if (cap->mq == (mqd_t)-1) {
        syslog(LOG_ERR, "[%s] mq_open failed: %d\n", TAG, errno);
        goto err_out;
    }

    ioctl(cap->fd, AUDIOIOC_REGISTERMQ, (uintptr_t)cap->mq);

    syslog(LOG_INFO,
        "[%s] open %s: %uHz %uch <- device %dHz %dch, %d x %d byte buffers\n",
        TAG, dev_path, sample_rate, channels,
        CAP_DEV_RATE, CAP_DEV_CHANNELS, cap->nbufs, cap->buf_size);

    return cap;

err_out:
    audio_capture_close(cap);
    return NULL;
}

int audio_capture_start(audio_capture_t* cap)
{
    pthread_attr_t attr;
    int ret;

    if (!cap) {
        return -EINVAL;
    }

    cap->running = true;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8192);
    ret = pthread_create(&cap->thread, &attr, capture_thread, cap);
    pthread_attr_destroy(&attr);

    if (ret != 0) {
        cap->running = false;
        return -EIO;
    }

    return 0;
}

/* Consume one device buffer exactly the way nxrecorder does: hand the raw
 * buffer to the caller to write, then give it back.  Used by the record
 * to file path, where the buffered reader's bookkeeping is pure overhead
 * and a source of bugs - nxrecorder writes apb->samp/apb->nbytes straight
 * out and that path is known good. */
unsigned audio_capture_sink_bytes(audio_capture_t* cap)
{
    return cap ? cap->sink_bytes : 0;
}

void audio_capture_set_sink(audio_capture_t* cap, int fd)
{
    if (cap) {
        cap->sink_fd = fd;
    }
}

int audio_capture_take_raw(audio_capture_t* cap,
    const unsigned char** data, size_t* len)
{
    if (!cap || !data || !len) {
        return -EINVAL;
    }

    while (sem_wait(&cap->avail) != 0) {
        if (errno != EINTR) {
            return -errno;
        }
    }

    cap->cur = ring_pop(cap);
    if (!cap->cur) {
        return -EIO;
    }

    *data = cap->cur->samp;
    *len = cap->cur->nbytes;
    return 0;
}

void audio_capture_release_raw(audio_capture_t* cap)
{
    if (cap && cap->cur) {
        capture_enqueue(cap, cap->cur);
        cap->cur = NULL;
    }
}

int audio_capture_read(audio_capture_t* cap, void* buf, size_t len)
{
    unsigned char* out = buf;
    size_t done = 0;

    if (!cap || !buf || len == 0) {
        return -EINVAL;
    }

    while (done < len) {
        size_t produced;

        if (!cap->cur) {
            while (sem_wait(&cap->avail) != 0) {
                if (errno != EINTR) {
                    return done > 0 ? (int)done : -errno;
                }
            }

            cap->cur = ring_pop(cap);
            cap->cur_off = 0;

            if (!cap->cur) {
                continue;
            }
        }

        produced = frames_to_out(cap,
            &cap->cur->samp[cap->cur_off],
            cap->cur->nbytes - cap->cur_off,
            out + done, len - done);

        if (produced == 0) {
            /* Either the tail of the buffer holds no whole frame, or the
             * caller's tail is a partial frame.  Recycle in the first
             * case, return early in the second; both avoid spinning. */

            if (cap->cur->nbytes - cap->cur_off
                    < cap->decim * CAP_DEV_FRAME) {
                capture_enqueue(cap, cap->cur);
                cap->cur = NULL;
                continue;
            }

            break;
        }

        cap->cur_off += produced / cap->out_frame
            * cap->decim * CAP_DEV_FRAME;
        done += produced;

        if (cap->cur_off >= cap->cur->nbytes) {
            capture_enqueue(cap, cap->cur);
            cap->cur = NULL;
        }
    }

    return (int)done;
}

void audio_capture_close(audio_capture_t* cap)
{
    struct audio_msg_s msg;

    if (!cap) {
        return;
    }

    /* Stop the device BEFORE this thread stops reading.  The stop ioctl
     * makes the driver hand back the buffers it still holds and the
     * driver's own worker posts one message per buffer; if this thread
     * had already stopped consuming, that queue would fill, the driver
     * worker would block in mq_send, and the stop ioctl - which waits for
     * that worker - would never return.  nxrecorder's record thread drains
     * until COMPLETE for the same reason. */
    syslog(LOG_INFO, "[%s] close: stopping device\n", TAG);

    if (cap->fd >= 0) {
        ioctl(cap->fd, AUDIOIOC_STOP, 0);
    }

    cap->running = false;

    if (cap->mq != (mqd_t)-1) {
        memset(&msg, 0, sizeof(msg));
        msg.msg_id = AUDIO_MSG_STOP;
        if (mq_send(cap->mq, (const char*)&msg, sizeof(msg), 0) < 0) {
            syslog(LOG_ERR, "[%s] close: mq_send failed: %d\n",
                TAG, errno);
        }
    }

    syslog(LOG_INFO, "[%s] close: joining device thread\n", TAG);

    if (cap->thread) {
        pthread_join(cap->thread, NULL);
    }

    syslog(LOG_INFO, "[%s] close: device thread joined\n", TAG);

    /* The half-consumed buffer is dropped rather than re-enqueued: the
     * device is already stopped and its worker is gone, so handing it
     * back would leave the driver holding a pointer to memory that
     * capture_free_buffers() frees right below. */

    cap->cur = NULL;

    capture_free_buffers(cap);

    if (cap->mq != (mqd_t)-1) {
        if (cap->fd >= 0) {
            ioctl(cap->fd, AUDIOIOC_UNREGISTERMQ, (uintptr_t)cap->mq);
        }
        mq_close(cap->mq);
        mq_unlink(cap->mqname);
    }

    if (cap->fd >= 0) {
        ioctl(cap->fd, AUDIOIOC_RELEASE, 0);
        close(cap->fd);
    }

    sem_destroy(&cap->avail);
    pthread_mutex_destroy(&cap->lock);
    free(cap);
}
