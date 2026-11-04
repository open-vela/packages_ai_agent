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

/* audio_playback_native.c - Streaming playback through the standard NuttX
 * audio character device, bypassing the Vela media framework.
 *
 * Data flow (TTS voice path):
 *   volc TTS WS chunks (base64 PCM, "format":"pcm") -> decoded s16le PCM
 *   -> voice_channel tts_stream_cb -> audio_playback_write()
 *   -> native_playback_write(): copy into apb, AUDIOIOC_ENQUEUEBUFFER
 *   -> es8311 lower half worker -> I2S0 TX -> ES8311 DAC -> speaker.
 *
 * Buffer lifecycle: nbuffers apbs are allocated via AUDIOIOC_ALLOCBUFFER.
 * write() fills free apbs and enqueues them; played-out buffers come back
 * as AUDIO_MSG_DEQUEUE messages on the private mq and return to the free
 * pool, so write() blocks like a pipe when the pipeline is full.
 */

#include "audio_native.h"

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <nuttx/audio/audio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

/* [V1024] Probe gate.
 *
 * This build carries ~60 probe lines/s during a reply, and a few
 * of them ([V810], [V1012]) dump raw PCM straight at the console.
 * Console throughput here is ~4300 chars/s with an 8 KB ring, so a
 * printf blocks hard once it fills.  At 60 lines/s the audio thread
 * spends most of its budget inside printf, which is itself a source
 * of the stutter we are trying to measure.
 *
 * g_v1024_quiet == 1 suppresses every [Vnnn] printf except the
 * handful of load-bearing ones (see KEEP in the patch script).
 * Flip it to 0 at runtime from NSH to bring them all back.
 */
#ifdef __cplusplus
extern "C" int g_v1024_quiet;
#else
extern int g_v1024_quiet;
#endif

#ifndef V1024_QUIET
#define V1024_QUIET 1
#endif
#define V1024P(...) do { if (V1024_QUIET == 0) printf(__VA_ARGS__); } while (0)
#define V1024S(lvl, ...) do { if (V1024_QUIET == 0) syslog(lvl, __VA_ARGS__); } while (0)


static const char* TAG = "audio_pb_nat";

/* [V924] DIAGNOSTIC ONLY -- playback-session census.
 *
 * /dev/audio/pcm0p is opened by more than one place, and the log shows
 * '[R36] configure OUT' + 'native playback ready' landing in the middle
 * of a stream that is already playing.  Each of those re-programs the
 * ES8311 and the I2S TX channel, and each matching close() issues
 * AUDIOIOC_STOP.  Both punch a hole into whatever is playing.
 *
 * Before changing any of that, count the sessions: this block tags every
 * open/close/write with a session id, the caller return address (resolve
 * with addr2line) and the number of sessions currently live.  Nothing
 * here gates or skips anything, so the audio path is bit-for-bit the one
 * V923 shipped. */

static pthread_mutex_t g_v924_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_v924_live;
static unsigned int    g_v924_seq;

static int v924_live(void)
{
    int n;

    pthread_mutex_lock(&g_v924_lock);
    n = g_v924_live;
    pthread_mutex_unlock(&g_v924_lock);
    return n;
}

/* [V925] DIAGNOSTIC ONLY -- transmitter census and feed-hole detector.
 *
 * deq  = every AUDIO_MSG_DEQUEUE the driver hands back.  One apb here is
 *        4096 B = 1024 stereo frames at 16 bit, so deq/s * 1024 is the
 *        frame rate the I2S transmitter is really running at.  If that
 *        number is not ~16000 the DAC clock is wrong, not the software.
 * gap  = distance between two consecutive write() calls.  A 20 ms packet
 *        cadence is normal; anything past 60 ms means a buffer ran dry
 *        and the speaker had nothing to shift out. */

static uint64_t g_v925_t0_us;
static uint64_t g_v925_last_us;
static unsigned g_v925_deq;
static unsigned g_v925_snap;

/* Pipeline geometry when the driver does not report its own
 * (GETBUFFERINFO fails on the es8311 lower half in this build).
 * 4096 B @ 24 kHz mono s16le = ~85 ms per buffer, 4 buffers deep. */

/* [V1007] 4 -> 8 buffers.
 *
 * THIS is the pool that actually plays.  Everything patched in
 * ai_vox/.../audio_output_engine.cpp sits upstream of it (jitter buffer,
 * pacing), and the nbuffers=8 raised in nuttx/audio/audio.c (v1001) only
 * feeds upper-half bookkeeping -- this driver keeps its own geometry and
 * kept reporting "4 x 4096 B".
 *
 * 4 x 4096 B = 16 KB = 512 ms at 16 kHz/16-bit/mono, in 128 ms buffers.
 * Measured arrival holes reach 220 ms and the downlink runs slightly
 * ABOVE realtime ([V1002] fill 104-110%), so bursts overflow a pool this
 * shallow and the transmitter runs dry.
 *
 * 8 x 4096 B = 32 KB = 1.0 s.  PB_MAX_NBUFFERS is already 8, so this is
 * the sanctioned ceiling; the 4096 B size stays well inside the DMA
 * ceiling of 4095 * 2 = 8190 B. */
#define PB_FALLBACK_NBUFFERS  8
#define PB_FALLBACK_BUFSIZE   4096

#define PB_MAX_NBUFFERS       8
#define PB_MAX_BUFSIZE        16384

/* How long write() waits for a played-out buffer before giving up, and
 * how long close() waits for in-flight buffers during the drain. */

#define PB_MSG_TIMEOUT_SEC    10
#define PB_DRAIN_TIMEOUT_SEC  5

struct native_pb_s {
    int fd;
    mqd_t mq;
    char mqname[NATIVE_MQ_NAME_LEN];

    struct ap_buffer_s** apbs;    /* all allocated apbs */
    int nbuffers;

    struct ap_buffer_s** freeq;   /* apbs ready to be filled */
    int nfree;

    struct ap_buffer_s* cur;      /* partially filled, not yet enqueued */
    int outstanding;              /* enqueued, not yet returned */

    volatile int stopped;
    int complete;                 /* driver sent AUDIO_MSG_COMPLETE */
    size_t total_written;

    /* [V924] census, diagnostics only */
    unsigned int v924_id;
    unsigned int v924_writes;
    unsigned int v924_deq;
    int          v924_counted;
};

/* Return the next free apb, reclaiming played-out buffers from the mq.
 * Returns NULL when the stream was stopped/completed or on timeout. */

static struct ap_buffer_s* pb_take_free(struct native_pb_s* pb)
{
    struct audio_msg_s msg;
    int ret;

    while (1) {
        if (pb->stopped) {
            return NULL;
        }

        if (pb->nfree > 0) {
            return pb->freeq[--pb->nfree];
        }

        ret = native_mq_receive_timed(pb->mq, &msg, PB_MSG_TIMEOUT_SEC);
        if (ret <= 0) {
            syslog(LOG_ERR, "[%s] no played-out buffer (ret=%d)\n",
                TAG, ret);
            return NULL;
        }

        switch (msg.msg_id) {
        case AUDIO_MSG_DEQUEUE:
            g_v925_deq++;            /* [V925] transmitter census */

            /* [V969] *** How long does one buffer take to play out? ***
             *
             * See the header comment: this interval is the one quantity on
             * the playback path that does not depend on whether we or the
             * downlink is the bottleneck.  Reported as a running average so
             * the 10 ms clock tick averages out and so a replayed log
             * segment cannot pass for a second measurement. */
            {
              struct timespec v969_ts;
              int64_t         v969_now;
              static int64_t  s_v969_last = -1;
              static uint64_t s_v969_sum  = 0;
              static uint32_t s_v969_n    = 0;

              clock_gettime(CLOCK_MONOTONIC, &v969_ts);
              v969_now = (int64_t)v969_ts.tv_sec * 1000000LL
                + (int64_t)v969_ts.tv_nsec / 1000LL;

              if (s_v969_last >= 0 && v969_now > s_v969_last) {
                s_v969_sum += (uint64_t)(v969_now - s_v969_last);
              }

              s_v969_last = v969_now;
              ++s_v969_n;

              if ((s_v969_n % 8U) == 0U) {
                uint32_t v969_avg = (uint32_t)(s_v969_sum / s_v969_n);
                unsigned v969_sz  = (unsigned)(pb->apbs && pb->apbs[0]
                                               ? pb->apbs[0]->nmaxbytes : 0u);

                V1024P("[V969]] DEQ n=%u int_us=%u bufsz=%u "
                       "hz4=%u\n",
                       s_v969_n, v969_avg, v969_sz,
                       v969_avg
                         ? (unsigned)((uint64_t)v969_sz * 1000000ULL
                                      / (4ULL * (uint64_t)v969_avg))
                         : 0u);
              }
            }

            if (msg.u.ptr && pb->outstanding > 0) {
                pb->outstanding--;
                pb->freeq[pb->nfree++] = (struct ap_buffer_s*)msg.u.ptr;
            }
            break;

        case AUDIO_MSG_COMPLETE:
            pb->complete = 1;
            syslog(LOG_INFO, "[%s] driver completed stream\n", TAG);
            return NULL;

        case AUDIO_MSG_STOP:
            pb->stopped = 1;
            return NULL;

        default:
            break;
        }
    }
}

/* Enqueue the partially-filled current buffer.  Returns 0 or -errno. */

static int pb_flush_cur(struct native_pb_s* pb)
{
    int ret;

    if (!pb->cur || pb->cur->nbytes == 0) {
        if (pb->cur) {
            pb->freeq[pb->nfree++] = pb->cur;
            pb->cur = NULL;
        }
        return 0;
    }

    ret = native_enqueue_buffer(pb->fd, pb->cur);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] enqueue failed: %d\n", TAG, ret);
        pb->freeq[pb->nfree++] = pb->cur;
        pb->cur = NULL;
        return ret;
    }

    pb->outstanding++;
    pb->cur = NULL;
    return 0;
}

int native_playback_open(native_pb_t** out_pb, const char* dev_path,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample)
{
    struct native_pb_s* pb;
    struct ap_buffer_info_s bufinfo;
    size_t buf_size;
    int ret;
    int i;

    if (!out_pb || !dev_path || channels == 0 || channels > 2) {
        return -EINVAL;
    }

    *out_pb = NULL;

    V1024S(LOG_ERR, "[V924]] OPEN ra=%p live=%d fmt=%u/%u/%u\n",
        __builtin_return_address(0), v924_live(), sample_rate, channels,
        bits_per_sample);

    pb = calloc(1, sizeof(*pb));
    if (!pb) {
        return -ENOMEM;
    }

    /* Sentinels so the shared error/cleanup path is safe. */

    pb->fd = -1;
    pb->mq = -1;

    /* Open the raw audio device (e.g. /dev/audio/pcm0p). */

    pb->fd = open(dev_path, O_RDWR);
    if (pb->fd < 0) {
        ret = -errno;
        syslog(LOG_ERR, "[%s] open(%s) failed: %d\n", TAG, dev_path, ret);
        goto err_out;
    }

    /* Reserve an exclusive session with the driver. */

    if (ioctl(pb->fd, AUDIOIOC_RESERVE, 0) != OK) {
        ret = -errno;
        syslog(LOG_ERR, "[%s] AUDIOIOC_RESERVE failed: %d\n", TAG, ret);
        goto err_out;
    }

    /* Private mq for buffer-completion notifications. */

    ret = native_mq_create(pb->mqname, sizeof(pb->mqname), &pb->mq);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] mq_create failed: %d\n", TAG, ret);
        goto err_out;
    }

    if (ioctl(pb->fd, AUDIOIOC_REGISTERMQ, (unsigned long)pb->mq) != OK) {
        ret = -errno;
        syslog(LOG_ERR, "[%s] REGISTERMQ failed: %d\n", TAG, ret);
        goto err_out;
    }

    /* Confirm this is an output-capable audio device. */

    ret = native_check_caps(pb->fd, AUDIO_TYPE_OUTPUT);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] %s is not an output audio device\n",
            TAG, dev_path);
        goto err_out;
    }

    /* Configure the stream format (es8311 reprograms the I2S clocks). */

    ret = native_configure(pb->fd, AUDIO_TYPE_OUTPUT, sample_rate,
        channels, bits_per_sample);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] configure %uHz %uch %ubit failed: %d\n",
            TAG, sample_rate, channels, bits_per_sample, ret);
        goto err_out;
    }

    /* Pipeline geometry: prefer the driver's own numbers, clamp to sane
     * streaming limits, otherwise use the fallback (4 x 4096 B). */

    pb->nbuffers = PB_FALLBACK_NBUFFERS;
    buf_size = PB_FALLBACK_BUFSIZE;
    if (ioctl(pb->fd, AUDIOIOC_GETBUFFERINFO,
              (unsigned long)&bufinfo) == OK &&
        bufinfo.nbuffers > 0 && bufinfo.buffer_size > 0) {
        pb->nbuffers = (bufinfo.nbuffers > PB_MAX_NBUFFERS)
            ? PB_MAX_NBUFFERS : (int)bufinfo.nbuffers;
        buf_size = (bufinfo.buffer_size > PB_MAX_BUFSIZE)
            ? PB_MAX_BUFSIZE : (size_t)bufinfo.buffer_size;
    }

    pb->apbs = calloc(pb->nbuffers, sizeof(*pb->apbs));
    pb->freeq = calloc(pb->nbuffers, sizeof(*pb->freeq));
    if (!pb->apbs || !pb->freeq) {
        ret = -ENOMEM;
        goto err_out;
    }

    for (i = 0; i < pb->nbuffers; i++) {
        pb->apbs[i] = native_alloc_buffer(pb->fd, buf_size);
        if (!pb->apbs[i]) {
            ret = -ENOMEM;
            syslog(LOG_ERR, "[%s] alloc apb %d failed\n", TAG, i);
            goto err_out;
        }

        pb->freeq[pb->nfree++] = pb->apbs[i];
    }

    /* Start the stream; data flows as write() enqueues buffers. */

    if (ioctl(pb->fd, AUDIOIOC_START, 0) != OK) {
        ret = -errno;
        syslog(LOG_ERR, "[%s] AUDIOIOC_START failed: %d\n", TAG, ret);
        goto err_out;
    }

    {
        int v924_n;

        pthread_mutex_lock(&g_v924_lock);
        pb->v924_id = ++g_v924_seq;
        pb->v924_counted = 1;
        v924_n = ++g_v924_live;
        pthread_mutex_unlock(&g_v924_lock);

        V1024S(LOG_ERR, "[V924]] OPENED id=%u live=%d\n", pb->v924_id,
            v924_n);
    }

    *out_pb = pb;
    syslog(LOG_INFO,
        "[%s] native playback ready (%s, %uHz %uch %ubit, %d x %u B)\n",
        TAG, dev_path, sample_rate, channels, bits_per_sample,
        pb->nbuffers,
        (unsigned)(pb->apbs[0] ? pb->apbs[0]->nmaxbytes : 0));
    return 0;

err_out:
    native_playback_close(pb);
    return ret;
}

int native_playback_write(native_pb_t* pb, const void* buf, size_t len)
{
    const unsigned char* src = buf;
    size_t remaining = len;

    if (!pb || !buf || len == 0) {
        return -EINVAL;
    }

    /* [BOOT-HAL-MUTE-2 2026-10-27] 本地链路也执行同一道开机硬闸。
     *
     * aivox 走 AudioOutputDevice::Write()，本地固定回复（台灯/传感器）走这里，
     * 两条路分开，只堵一条等于漏一条。开机窗口内非全零缓冲直接丢；
     * 全零缓冲照常放行——采集期的 clock-keeper 靠它驱动 I2S TX 时钟，
     * 且零样本本来就不会出声。 */
    {
        extern int64_t g_vox_audio_mute_until_ms;   /* 定义在 ai_vox_main.cpp（extern "C"） */
        if (g_vox_audio_mute_until_ms != 0) {
            struct timespec bts;
            int64_t now_ms;
            clock_gettime(CLOCK_MONOTONIC, &bts);
            now_ms = (int64_t)bts.tv_sec * 1000LL + (int64_t)(bts.tv_nsec / 1000000L);
            if (now_ms < g_vox_audio_mute_until_ms) {
                const int16_t* s = (const int16_t*)buf;
                size_t n = len / sizeof(int16_t);
                size_t k;
                int nonzero = 0;
                for (k = 0; k < n; k++) {
                    if (s[k] != 0) { nonzero = 1; break; }
                }
                if (nonzero) {
                    static int bhm2_once = 0;
                    if (bhm2_once++ < 4) {
                        syslog(LOG_WARNING,
                            "[BOOT-HAL-MUTE] drop local %u bytes (boot window)\n",
                            (unsigned)len);
                    }
                    return (int)len;    /* 已消化，不让上层重试 */
                }
            }
        }
    }

    if (pb->stopped) {
        return -ECANCELED;
    }

    {
        struct timespec v925_ts;
        uint64_t v925_us;

        clock_gettime(CLOCK_MONOTONIC, &v925_ts);
        v925_us = (uint64_t)v925_ts.tv_sec * 1000000ULL
            + (uint64_t)v925_ts.tv_nsec / 1000ULL;

        if (g_v925_last_us != 0 && v925_us > g_v925_last_us
            && (v925_us - g_v925_last_us) > 60000ULL) {
            V1024S(LOG_ERR, "[V925]] GAP %u ms id=%u out=%d\n",
                (unsigned)((v925_us - g_v925_last_us) / 1000ULL),
                pb->v924_id, pb->outstanding);
        }

        g_v925_last_us = v925_us;

        if (g_v925_t0_us == 0) {
            g_v925_t0_us = v925_us;
        } else if ((v925_us - g_v925_t0_us) >= 1000000ULL) {
            unsigned v925_n = g_v925_deq - g_v925_snap;

            V1024S(LOG_ERR, "[V925]] tx deq/s=%u est=%u Hz out=%d\n",
                v925_n, v925_n * 1024u, pb->outstanding);

            g_v925_snap = g_v925_deq;
            g_v925_t0_us = v925_us;
        }
    }

    while (remaining > 0) {
        struct ap_buffer_s* apb;
        size_t space;
        size_t n;

        if (pb->stopped) {
            return -ECANCELED;
        }

        if (!pb->cur) {
            pb->cur = pb_take_free(pb);
            if (!pb->cur) {
                if (pb->stopped) {
                    return -ECANCELED;
                }
                if (pb->complete) {
                    return -EPIPE;
                }
                return -EIO;
            }
            pb->cur->nbytes = 0;
            pb->cur->curbyte = 0;
        }

        apb = pb->cur;
        space = apb->nmaxbytes - apb->nbytes;
        n = (space < remaining) ? space : remaining;

        memcpy(apb->samp + apb->nbytes, src, n);
        apb->nbytes += (apb_samp_t)n;
        src += n;
        remaining -= n;
        pb->total_written += n;

        /* [V924] every 32nd write: which session is feeding, how full
         * the pipeline is, and how much of the current apb is used. */
        if ((++pb->v924_writes % 32U) == 0U) {
            V1024S(LOG_ERR, "[V924]] W id=%u k=%u tot=%u out=%d cur=%d live=%d\n",
                pb->v924_id, pb->v924_writes,
                (unsigned)pb->total_written, pb->outstanding,
                pb->cur ? (int)pb->cur->nbytes : -1, v924_live());
        }

        if (apb->nbytes >= apb->nmaxbytes) {
            int ret = pb_flush_cur(pb);
            if (ret < 0) {
                return ret;
            }
        }
    }

    return (int)len;
}

void native_playback_stop(native_pb_t* pb)
{
    if (!pb || pb->stopped) {
        return;
    }

    pb->stopped = 1;

    /* Discard the partially-filled buffer; STOP makes the driver return
     * all in-flight buffers via AUDIO_MSG_DEQUEUE messages. */

    if (pb->cur) {
        pb->freeq[pb->nfree++] = pb->cur;
        pb->cur = NULL;
    }

    ioctl(pb->fd, AUDIOIOC_STOP, 0);
}

void native_playback_close(struct native_pb_s* pb)
{
    int ret;
    int i;

    if (!pb) {
        return;
    }

    V1024S(LOG_ERR, "[V924]] CLOSE id=%u out=%d tot=%u live=%d stopped=%d\n",
        pb->v924_id, pb->outstanding, (unsigned)pb->total_written,
        v924_live(), pb->stopped);

    if (!pb->stopped && pb->fd >= 0) {
        /* Graceful drain: flush the partial buffer, then wait until the
         * driver returned every enqueued buffer so nothing is cut off. */

        if (pb_flush_cur(pb) == 0) {
            struct audio_msg_s msg;

            while (pb->outstanding > 0) {
                ret = native_mq_receive_timed(pb->mq, &msg,
                    PB_DRAIN_TIMEOUT_SEC);
                if (ret <= 0) {
                    break;
                }

                if (msg.msg_id == AUDIO_MSG_DEQUEUE && msg.u.ptr &&
                    pb->outstanding > 0) {
                    pb->v924_deq++;
                    pb->outstanding--;
                    pb->freeq[pb->nfree++] =
                        (struct ap_buffer_s*)msg.u.ptr;
                } else if (msg.msg_id == AUDIO_MSG_COMPLETE ||
                           msg.msg_id == AUDIO_MSG_STOP) {
                    break;
                }
            }
        }
    }

    if (pb->fd >= 0) {
        ioctl(pb->fd, AUDIOIOC_STOP, 0);
    }

    if (pb->mq >= 0) {
        ioctl(pb->fd, AUDIOIOC_UNREGISTERMQ, (unsigned long)pb->mq);
        mq_close(pb->mq);
        mq_unlink(pb->mqname);
        pb->mq = -1;
    }

    if (pb->fd >= 0) {
        ioctl(pb->fd, AUDIOIOC_RELEASE, 0);

        if (pb->apbs) {
            for (i = 0; i < pb->nbuffers; i++) {
                native_free_buffer(pb->fd, pb->apbs[i]);
            }
        }

        close(pb->fd);
        pb->fd = -1;
    }

    free(pb->apbs);
    free(pb->freeq);

    {
        int v924_n = 0;

        if (pb->v924_counted) {
            pthread_mutex_lock(&g_v924_lock);
            if (g_v924_live > 0) {
                g_v924_live--;
            }
            v924_n = g_v924_live;
            pthread_mutex_unlock(&g_v924_lock);
        }

        V1024S(LOG_ERR, "[V924]] CLOSED id=%u deq=%u live=%d\n",
            pb->v924_id, pb->v924_deq, v924_n);
    }

    free(pb);

    syslog(LOG_INFO, "[%s] closed\n", TAG);
}
