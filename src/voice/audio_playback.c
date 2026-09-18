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

/* audio_playback.c - Streaming audio playback.
 *
 * Two backends:
 *  - NUTTX direct (/dev/audio/pcm0p): gated by CONFIG_AI_AGENT_AUDIO_NUTTX_DIRECT.
 *    Drives the standard NuttX audio framework device via open/ioctl/enqueue,
 *    the only path the BK7258 pcm0p lower-half implements (its ->write method
 *    is NULL).  Buffers are allocated with AUDIOIOC_ALLOCBUFFER, filled, and
 *    pushed with AUDIOIOC_ENQUEUEBUFFER; a message queue delivers
 *    AUDIO_MSG_DEQUEUE when the DMA has drained a buffer so it can be reused.
 *  - media_player: portable fallback, used when the NuttX device open fails
 *    or when CONFIG_AI_AGENT_AUDIO_NUTTX_DIRECT is off (e.g. qemu goldfish).
 *
 * write() enqueues non-blocking and only blocks for back-pressure when all
 * buffers are in flight.  close() drains the enqueued audio before stopping
 * so a short notification (beep / TTS clip) is fully audible.
 */

#include "voice/audio_playback.h"
#include "agent_config.h"

#ifdef CONFIG_AI_AGENT_AUDIO_NUTTX_DIRECT
#include <fcntl.h>
#include <mqueue.h>
#include <sys/ioctl.h>
#include <nuttx/audio/audio.h>
#endif
#include <errno.h>
#include <media_player.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

static const char* TAG = "audio_pb";

#define PB_OPTIONS_LEN 128

#define PB_MAX_BUFS     8
#define PB_MQ_NAME_LEN  32

enum audio_playback_backend {
    AUDIO_PB_BACKEND_NONE = 0,
    AUDIO_PB_BACKEND_NUTTX,
    AUDIO_PB_BACKEND_MEDIA_PLAYER,
};

struct audio_playback {
    enum audio_playback_backend backend;

    /* media_player backend */
    void* player;

    /* NUTTX backend */
#ifdef CONFIG_AI_AGENT_AUDIO_NUTTX_DIRECT
    int fd;
    mqd_t mq;
    char mq_name[PB_MQ_NAME_LEN];
    struct ap_buffer_s* apbs[PB_MAX_BUFS];   /* allocated pool (owned) */
    struct ap_buffer_s* freeq[PB_MAX_BUFS];  /* dequeued, reusable stack */
    unsigned int nbuffers;
    unsigned int free_count;
    unsigned int buf_bytes;
    int started;
#endif

    size_t total_written;
    int bytes_per_frame;
    volatile int stopped;
};

static void* s_active_player;

/* ── media_player backend ───────────────────────────────── */

static int open_media_player(audio_playback_t* pb,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample)
{
    if (s_active_player) {
        syslog(LOG_WARNING, "[%s] force closing stale player\n", TAG);
        media_player_stop(s_active_player);
        media_player_close(s_active_player, 0);
        s_active_player = NULL;
        usleep(100000);
    }

    void* player = media_player_open(MEDIA_STREAM_MUSIC);
    if (!player) {
        syslog(LOG_ERR, "[%s] media_player_open failed\n", TAG);
        return -EIO;
    }

    char opts[PB_OPTIONS_LEN];
    snprintf(opts, sizeof(opts),
        "format=s%ule:sample_rate=%u:ch_layout=%s",
        bits_per_sample, sample_rate,
        (channels == 1) ? "mono" : "stereo");

    int ret = media_player_prepare(player, NULL, opts);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] prepare failed: %d\n", TAG, ret);
        media_player_close(player, 0);
        return ret;
    }

    ret = media_player_start(player);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] start failed: %d\n", TAG, ret);
        media_player_close(player, 0);
        return ret;
    }

    pb->player = player;
    pb->bytes_per_frame = (int)((bits_per_sample / 8) * channels);
    s_active_player = player;

    syslog(LOG_INFO, "[%s] opened (%uHz %uch %ubit) via media_player\n",
        TAG, sample_rate, channels, bits_per_sample);
    return 0;
}

/* ── NUTTX direct backend ─────────────────────────────────── */

#ifdef CONFIG_AI_AGENT_AUDIO_NUTTX_DIRECT

static int open_nuttx_playback(audio_playback_t* pb, const char* dev_path,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample)
{
    struct audio_caps_desc_s caps;
    struct ap_buffer_info_s info;
    struct audio_buf_desc_s buf_desc;
    struct mq_attr attr;
    int fd;
    int ret;
    unsigned int i;

    /* The BK7258 pcm0p lower-half plays 16-bit mono S16_LE. */
    if (channels != 1 || bits_per_sample != 16) {
        syslog(LOG_WARNING,
            "[%s] NuttX audio playback requires 16-bit mono\n", TAG);
        return -ENOTSUP;
    }

    fd = open(dev_path, O_WRONLY);
    if (fd < 0) {
        syslog(LOG_ERR, "[%s] open(%s) failed, errno=%d\n",
            TAG, dev_path, errno);
        return -errno;
    }

    ret = ioctl(fd, AUDIOIOC_RESERVE, 0);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] AUDIOIOC_RESERVE failed: %d\n", TAG, ret);
        close(fd);
        return ret;
    }

    memset(&caps, 0, sizeof(caps));
    caps.caps.ac_len            = sizeof(caps.caps);
    caps.caps.ac_type           = AUDIO_TYPE_OUTPUT;
    caps.caps.ac_subtype        = AUDIO_FMT_PCM;
    caps.caps.ac_channels       = (uint8_t)channels;
    caps.caps.ac_controls.hw[0] = (uint16_t)(sample_rate & 0xFFFFu);
    caps.caps.ac_controls.b[3]  = (uint8_t)(sample_rate >> 16);
    caps.caps.ac_controls.b[2]  = (uint8_t)bits_per_sample;

    ret = ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&caps);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] AUDIOIOC_CONFIGURE failed: %d\n", TAG, ret);
        close(fd);
        return ret;
    }

    /* Message queue: the driver posts AUDIO_MSG_DEQUEUE when a buffer has
     * been drained by the DMA so we can reuse it. */
    snprintf(pb->mq_name, sizeof(pb->mq_name),
        "/audio_pb_mq_%d", (int)getpid());

    attr.mq_maxmsg  = 16;
    attr.mq_msgsize = sizeof(struct audio_msg_s);
    attr.mq_flags   = 0;
    attr.mq_curmsgs = 0;

    mq_unlink(pb->mq_name);
    pb->mq = mq_open(pb->mq_name, O_RDWR | O_CREAT, 0644, &attr);
    if (pb->mq == (mqd_t)-1) {
        syslog(LOG_ERR, "[%s] mq_open failed: %d\n", TAG, errno);
        close(fd);
        return -errno;
    }

    ret = ioctl(fd, AUDIOIOC_REGISTERMQ, (unsigned long)pb->mq);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] AUDIOIOC_REGISTERMQ failed: %d\n", TAG, ret);
        mq_close(pb->mq);
        mq_unlink(pb->mq_name);
        close(fd);
        return ret;
    }

    ret = ioctl(fd, AUDIOIOC_GETBUFFERINFO, (unsigned long)&info);
    if (ret < 0) {
        info.nbuffers    = 4;
        info.buffer_size = 4096;
    }

    if (info.nbuffers > PB_MAX_BUFS) {
        info.nbuffers = PB_MAX_BUFS;
    }
    if (info.nbuffers == 0) {
        info.nbuffers = 4;
    }
    if (info.buffer_size == 0) {
        info.buffer_size = 4096;
    }

    for (i = 0; i < (unsigned int)info.nbuffers; i++) {
        struct ap_buffer_s* apb = NULL;

        memset(&buf_desc, 0, sizeof(buf_desc));
        buf_desc.numbytes  = info.buffer_size;
        buf_desc.u.pbuffer = &apb;
        ret = ioctl(fd, AUDIOIOC_ALLOCBUFFER, (unsigned long)&buf_desc);
        if (ret < 0 || apb == NULL) {
            syslog(LOG_ERR, "[%s] AUDIOIOC_ALLOCBUFFER failed: %d\n",
                TAG, ret);
            while (i > 0) {
                i--;
                memset(&buf_desc, 0, sizeof(buf_desc));
                buf_desc.u.buffer = pb->apbs[i];
                ioctl(fd, AUDIOIOC_FREEBUFFER, (unsigned long)&buf_desc);
            }
            ioctl(fd, AUDIOIOC_UNREGISTERMQ, (unsigned long)pb->mq);
            mq_close(pb->mq);
            mq_unlink(pb->mq_name);
            close(fd);
            return (ret < 0) ? ret : -ENOMEM;
        }
        pb->apbs[i]  = apb;
        pb->freeq[i] = apb;
    }

    pb->fd             = fd;
    pb->nbuffers       = (unsigned int)info.nbuffers;
    pb->free_count     = pb->nbuffers;
    pb->buf_bytes      = (unsigned int)info.buffer_size;
    pb->started        = 0;
    pb->bytes_per_frame = (int)((bits_per_sample / 8) * channels);

    syslog(LOG_INFO,
        "[%s] opened NuttX playback (%s, %uHz, %uch, %ubit, %u x %uB)\n",
        TAG, dev_path, sample_rate, channels, bits_per_sample,
        pb->nbuffers, pb->buf_bytes);
    return 0;
}

/* Receive one message, folding any DEQUEUE'd buffer back into the free list.
 * Returns 0 (processed), -ETIMEDOUT, or -EIO. */
static int nuttx_recv_one(audio_playback_t* pb, int timeout_ms)
{
    struct audio_msg_s msg;
    struct timespec to;
    unsigned int prio;
    ssize_t n;

    clock_gettime(CLOCK_REALTIME, &to);
    to.tv_sec  += (time_t)(timeout_ms / 1000);
    to.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (to.tv_nsec >= 1000000000L) {
        to.tv_sec++;
        to.tv_nsec -= 1000000000L;
    }

    n = mq_timedreceive(pb->mq, (char*)&msg, sizeof(msg), &prio, &to);
    if (n < 0) {
        return (errno == ETIMEDOUT) ? -ETIMEDOUT : -errno;
    }

    if (msg.msg_id == AUDIO_MSG_DEQUEUE && msg.u.ptr) {
        if (pb->free_count < pb->nbuffers) {
            pb->freeq[pb->free_count++] = (struct ap_buffer_s*)msg.u.ptr;
        }
        return 0;
    }

    if (msg.msg_id == AUDIO_MSG_IOERR) {
        return -EIO;
    }

    /* COMPLETE / UNDERRUN / others: not a buffer, keep polling. */
    return 0;
}

static struct ap_buffer_s* nuttx_get_free_buffer(audio_playback_t* pb)
{
    for (;;) {
        if (pb->stopped) {
            return NULL;
        }
        if (pb->free_count > 0) {
            return pb->freeq[--pb->free_count];
        }

        int r = nuttx_recv_one(pb, 500);
        if (r == -EIO) {
            return NULL;
        }
        /* -ETIMEDOUT or 0: loop and re-check stopped / free_count. */
    }
}

static int nuttx_write(audio_playback_t* pb, const void* buf, size_t len)
{
    const uint8_t* src = (const uint8_t*)buf;
    size_t remaining = len;

    while (remaining > 0) {
        struct ap_buffer_s* apb = nuttx_get_free_buffer(pb);
        if (!apb) {
            return pb->stopped ? -ECANCELED : -EIO;
        }

        size_t chunk = remaining;
        if (chunk > pb->buf_bytes) {
            chunk = pb->buf_bytes;
        }

        memcpy(apb->samp, src, chunk);
        apb->nbytes  = (apb_samp_t)chunk;
        apb->curbyte = 0;

        struct audio_buf_desc_s desc;
        memset(&desc, 0, sizeof(desc));
        desc.numbytes = chunk;
        desc.u.buffer = apb;

        int ret = ioctl(pb->fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&desc);
        if (ret < 0) {
            syslog(LOG_ERR, "[%s] AUDIOIOC_ENQUEUEBUFFER failed: %d\n",
                TAG, ret);
            return ret;
        }

        if (!pb->started) {
            ret = ioctl(pb->fd, AUDIOIOC_START, 0);
            if (ret < 0) {
                syslog(LOG_ERR, "[%s] AUDIOIOC_START failed: %d\n", TAG, ret);
                return ret;
            }
            pb->started = 1;
        }

        src += chunk;
        remaining -= chunk;
        pb->total_written += chunk;
    }

    return (int)len;
}

/* Wait (bounded) until all enqueued buffers have been drained back. */
static void nuttx_drain(audio_playback_t* pb, int max_ms)
{
    int waited_ms = 0;

    while (pb->free_count < pb->nbuffers) {
        if (pb->stopped) {
            break;
        }
        if (waited_ms >= max_ms) {
            syslog(LOG_WARNING,
                "[%s] drain timeout (%dms), %u/%u buffers back\n",
                TAG, max_ms, pb->free_count, pb->nbuffers);
            break;
        }

        int r = nuttx_recv_one(pb, 200);
        waited_ms += 200;
        if (r == -EIO) {
            break;
        }
        /* -ETIMEDOUT or 0: loop with stopped / max_ms guard. */
    }
}

static void nuttx_close(audio_playback_t* pb)
{
    unsigned int i;

    /* Let enqueued audio finish playing before stopping (unless aborted). */
    nuttx_drain(pb, 5000);

    ioctl(pb->fd, AUDIOIOC_STOP, 0);

    /* The driver returns any still-in-flight buffers as DEQUEUE messages on
     * stop; collect them so we don't free a buffer it still owns. */
    nuttx_drain(pb, 1000);

    if (pb->free_count == pb->nbuffers) {
        for (i = 0; i < pb->nbuffers; i++) {
            struct audio_buf_desc_s desc;
            memset(&desc, 0, sizeof(desc));
            desc.u.buffer = pb->apbs[i];
            ioctl(pb->fd, AUDIOIOC_FREEBUFFER, (unsigned long)&desc);
            pb->apbs[i] = NULL;
        }
    } else {
        /* Error path: only free the buffers the driver gave back; leak the
         * rest rather than risk a use-after-free. */
        syslog(LOG_WARNING, "[%s] %u/%u buffers not returned; leaking\n",
            TAG, pb->nbuffers - pb->free_count, pb->nbuffers);
        while (pb->free_count > 0) {
            struct ap_buffer_s* apb = pb->freeq[--pb->free_count];
            struct audio_buf_desc_s desc;
            memset(&desc, 0, sizeof(desc));
            desc.u.buffer = apb;
            ioctl(pb->fd, AUDIOIOC_FREEBUFFER, (unsigned long)&desc);
        }
    }

    ioctl(pb->fd, AUDIOIOC_UNREGISTERMQ, (unsigned long)pb->mq);
    ioctl(pb->fd, AUDIOIOC_RELEASE, 0);
    mq_close(pb->mq);
    mq_unlink(pb->mq_name);
    close(pb->fd);
    pb->fd = -1;
}

#endif /* CONFIG_AI_AGENT_AUDIO_NUTTX_DIRECT */

/* ── Public API ───────────────────────────────────────────── */

audio_playback_t* audio_playback_open(const char* dev_path,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample)
{
    audio_playback_t* pb = calloc(1, sizeof(*pb));
    if (!pb) {
        return NULL;
    }

#ifdef CONFIG_AI_AGENT_AUDIO_NUTTX_DIRECT
    /* Prefer the NuttX audio framework device when present (e.g. BK7258
     * /dev/audio/pcm0p). */
    if (open_nuttx_playback(pb, dev_path, sample_rate, channels,
            bits_per_sample) == 0) {
        pb->backend = AUDIO_PB_BACKEND_NUTTX;
        return pb;
    }
#endif

    /* Fall back to the portable media_player backend. */
    if (open_media_player(pb, sample_rate, channels, bits_per_sample) == 0) {
        pb->backend = AUDIO_PB_BACKEND_MEDIA_PLAYER;
        return pb;
    }

    free(pb);
    return NULL;
}

int audio_playback_write(audio_playback_t* pb, const void* buf, size_t len)
{
    if (!pb || !buf || len == 0) {
        return -EINVAL;
    }

    if (pb->stopped) {
        return -ECANCELED;
    }

    switch (pb->backend) {
#ifdef CONFIG_AI_AGENT_AUDIO_NUTTX_DIRECT
    case AUDIO_PB_BACKEND_NUTTX:
        return nuttx_write(pb, buf, len);
#endif

    case AUDIO_PB_BACKEND_MEDIA_PLAYER: {
        ssize_t n = media_player_write_data(pb->player, buf, len);
        if (n > 0) {
            pb->total_written += (size_t)n;
        }
        return (int)n;
    }

    default:
        return -EINVAL;
    }
}

void audio_playback_stop(audio_playback_t* pb)
{
    if (!pb) {
        return;
    }

    pb->stopped = 1;

    switch (pb->backend) {
#ifdef CONFIG_AI_AGENT_AUDIO_NUTTX_DIRECT
    case AUDIO_PB_BACKEND_NUTTX:
        if (pb->fd >= 0) {
            ioctl(pb->fd, AUDIOIOC_STOP, 0);
        }
        break;
#endif

    case AUDIO_PB_BACKEND_MEDIA_PLAYER:
        if (pb->player) {
            media_player_stop(pb->player);
        }
        break;

    default:
        break;
    }
}

void audio_playback_close(audio_playback_t* pb)
{
    if (!pb) {
        return;
    }

    switch (pb->backend) {
#ifdef CONFIG_AI_AGENT_AUDIO_NUTTX_DIRECT
    case AUDIO_PB_BACKEND_NUTTX:
        nuttx_close(pb);
        break;
#endif

    case AUDIO_PB_BACKEND_MEDIA_PLAYER:
        if (pb->player) {
            syslog(LOG_INFO, "[%s] closing (%zu bytes written)\n",
                TAG, pb->total_written);
            media_player_stop(pb->player);
            usleep(50 * 1000);
            media_player_close(pb->player, 0);
            s_active_player = NULL;
        }
        break;

    default:
        break;
    }

    free(pb);
}
