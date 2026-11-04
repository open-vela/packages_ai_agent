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

/* audio_capture_native.c - Streaming capture through the standard NuttX
 * audio character device, bypassing the Vela media framework.
 *
 * Data flow (ASR voice path):
 *   ES8311 ADC -> I2S0 RX -> es8311 lower half fills apbs
 *   -> AUDIO_MSG_DEQUEUE on our private mq -> native_capture_read()
 *   copies s16le PCM out and re-arms the buffer
 *   -> audio_capture_read() applies capture gain
 *   -> voice_channel recording thread -> voice_asr (16 kHz mono).
 *
 * start() primes the pipeline with empty buffers before AUDIOIOC_START.
 * read() drains filled buffers (partial-buffer state is kept across
 * calls) and immediately re-enqueues drained ones to avoid overruns.
 */

#include "audio_native.h"

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <sched.h>
#include <nuttx/audio/audio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <syslog.h>
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


/* [V85] Cross-module diagnostic; diag_report() lives in voice/http_tts.c.
 * Rate-limited because a 1 Hz poll would otherwise fill the 2301 channel. */

extern int diag_report(const char *msg);



/* [V94] Counters published by arch/xtensa/src/esp32s3/esp32s3_i2s.c. */

extern volatile unsigned int g_v94_recv;
extern volatile int          g_v94_setupret;
extern volatile unsigned int g_v94_start;
extern volatile unsigned int g_v94_isr;
extern volatile unsigned int g_v94_eof;
extern volatile unsigned int g_v94_pb;
extern volatile int          g_v94_i2sret;
extern volatile unsigned int g_v94_done;
extern volatile int          g_v94_rxonly;

/* [V380] See the note at the top of patch_v380.py: phase 1 captures the
 * transmitter through the internal loopback, phase 2 captures the real
 * ES8311 with the transmitter supplying BCK/WS.
 */

extern volatile int          g_v380_loopback;
extern volatile int          g_v380_txmaster;
/* [V660] Capture-path health dump, implemented in esp32s3_i2s.c and called
 * on the capture thread (NOT the interrupt stack). */
extern void esp32s3_i2s_rx_health(void);
extern void esp32s3_i2s_rx_flush(void);   /* [V697] release in-flight RX bufs */
/* [V858] Re-lock the receiver before this capture arms its buffers. */
extern void esp32s3_i2s_v858_capture_begin(void);
extern void esp32s3_i2s_v858_heal(void);
extern uint32_t esp32s3_i2s_v858_rxconf(void);
/* [V644] Diagnostic residue retired: phase 1 armed the I2S internal
 * loopback (g_v380_loopback = 1) inside the *production* capture prime
 * path, because nothing but native_capture_abort() ever advanced the
 * phase past 1.  Flipping that flag mid-capture makes
 * esp32s3_i2s_set_rxonly() fail its early-return test and stop BOTH
 * engines while the receiver is already streaming -- exactly the
 * "EXCCAUSE=001c task: es8311" signature that [V381]/[V379] recorded.
 * Start in phase 2: loopback OFF, TX master kept, real codec. */
static int                   s_v380_phase = 2;
static void nc_probe(const char *tag)
{
    static int s_n;

    s_n++;
    if (s_n <= 12 || (s_n % 20) == 0) {
        diag_report(tag);
        /* [V660] diag_report() does not reach /dev/kmsg, so every NC_* tag
         * (NC_ST_BEG / NC_ST_OK_* / NC_MQ_GIVEUP / re-arm) has been
         * invisible to the serial log all along.  Mirror it into syslog;
         * the counter cap keeps this from becoming a flood. */
        syslog(LOG_INFO, "[audio_cap_nat] [NC] %s (n=%d)\n", tag, s_n);
    }
}

static const char* TAG = "audio_cap_nat";

/* [V88] Clock keep-alive.
 *
 * RX is a slave on this port: i2s_configure() forces I2S_RX_SLAVE_MOD
 * whenever TX is also enabled, and sets I2S_SIG_LOOPBACK so RX takes BCLK
 * and WS from the transmitter.  The ESP32-S3 transmitter only drives them
 * while it has DMA descriptors to shift out - which is why playback is
 * audible and capture is silent.
 *
 * Rather than fake a transmitter in the driver, feed the real one: while a
 * capture runs, stream zeroed PCM to /dev/audio/pcm0p.  Zero samples are
 * inaudible but keep BCLK and WS alive, so the RX DMA sees frames and the
 * lower half starts dequeuing.
 *
 * Same primitives the TTS path uses, so nothing here is untested code. */

#define CAP_CLOCK_DEV       "/dev/audio/pcm0p"
#define CAP_CLOCK_RATE      16000
#define CAP_CLOCK_CHAN      1
#define CAP_CLOCK_BITS      16
#define CAP_CLOCK_STACK     16384

static pthread_t   s_clktid;
static volatile int s_clkrun;
static int         s_clkstarted;

static void* cap_clock_thread(void* arg)
{
    native_pb_t* pb = NULL;
    static uint16_t silence[512];      /* 1 KB of quiet PCM, in .bss */
    int rc;
    int k;

    /* [V380] Not silence any more: a DC value of 8 LSB (-72 dBFS,
     * inaudible on the speaker) makes the stream recognisable in the
     * capture dump, so "the RX path works" can be told apart from
     * "the RX path returns whatever the codec sends".
     */

    for (k = 0; k < 512; k++)
      {
        silence[k] = 0x0008;
      }

    rc = native_playback_open(&pb, CAP_CLOCK_DEV, CAP_CLOCK_RATE,
                              CAP_CLOCK_CHAN, CAP_CLOCK_BITS);
    if (rc != 0 || pb == NULL)
    {
        nc_probe("NC_CLK_OPENFAIL");
        return NULL;
    }

    nc_probe("NC_CLK_OPEN");

    while (s_clkrun)
    {
        int n = native_playback_write(pb, silence, sizeof(silence));

        if (n < 0)
        {
            nc_probe("NC_CLK_WRFAIL");
            break;
        }
    }

    native_playback_close(pb);
    nc_probe("NC_CLK_STOP");
    return NULL;
}

static void cap_clock_start(void)
{
    pthread_attr_t attr;
    struct sched_param sp;

    if (s_clkstarted)
    {
        return;
    }

    s_clkrun = 1;

    pthread_attr_init(&attr);
    /* Deliberately below the codec worker: this thread feeds it,
     * it must not starve it. */
    sp.sched_priority = sched_get_priority_max(SCHED_FIFO) - 12;
    pthread_attr_setschedparam(&attr, &sp);
    pthread_attr_setstacksize(&attr, CAP_CLOCK_STACK);

    if (pthread_create(&s_clktid, &attr, cap_clock_thread, NULL) != 0)
    {
        s_clkrun = 0;
        nc_probe("NC_CLK_TFAIL");
        return;
    }

    pthread_detach(s_clktid);
    s_clkstarted = 1;
    nc_probe("NC_CLK_TSTART");
}

static void cap_clock_stop(void)
{
    if (!s_clkstarted)
    {
        return;
    }

    s_clkrun = 0;
    s_clkstarted = 0;
}

/* Pipeline geometry when the driver does not report its own (es8311 in
 * this build does not).  4096 B @ 16 kHz mono s16le = 128 ms per buffer,
 * 4 buffers deep (~512 ms of protection against read-side hiccups). */

#define CAP_FALLBACK_NBUFFERS  2   /* [V312] 2 slots: shares the I2S driver's
                                     * 2-slot internal-SRAM bounce pool with
                                     * TX (they never run concurrently, V89).
                                     * 4 would need +16KB static BSS, which
                                     * shifts .bss and breaks WiFi assoc. */
/* [V688] 4092, not 4096.
 *
 * The GDMA splits a 4096-byte buffer into a 4095-byte descriptor plus a
 * 1-byte tail, and only the FIRST EOF-marked descriptor is ever counted
 * (see the V687 note in esp32s3_i2s.c).  4092 is
 * ESP32S3_DMA_BUFLEN_MAX_4B_ALIGNED, so the whole buffer fits in ONE
 * descriptor and the EOF always lands on the descriptor that holds it. */

#define CAP_FALLBACK_BUFSIZE   4092

#define CAP_MAX_NBUFFERS       2   /* [V312] see CAP_FALLBACK_NBUFFERS */
#define CAP_MAX_BUFSIZE        16384

/* read() wait per mq_receive attempt (retried until stopped). */

#define CAP_MSG_TIMEOUT_SEC    1

/* [V85] How many consecutive empty waits before read() gives up and lets the
 * caller restart.  Without this the retry loop below never exits. */

#define CAP_MQ_MAX_TIMEOUTS    6

struct native_cap_s {
    int fd;
    mqd_t mq;
    char mqname[NATIVE_MQ_NAME_LEN];

    struct ap_buffer_s** apbs;    /* all allocated apbs */
    int nbuffers;
    int outstanding;              /* armed/enqueued at the driver */

    struct ap_buffer_s** freeq;   /* apbs drained and waiting re-arm */
    int nfree;

    struct ap_buffer_s* pending;  /* filled apb being consumed */
    size_t pending_off;

    volatile int started;
    volatile int stopped;
    int complete;

    /* [V743] consecutive empty mq waits.  This used to be a function-local
     * static, i.e. process-global and never reset: after six timeouts in the
     * whole uptime, read() gave up instantly on every later session, so the
     * board could never capture speech again. */

    int mq_timeouts;
};

/* [V698] guard every cap->freeq[] push (defined further down) */

static void cap_v698_push(struct native_cap_s* cap, struct ap_buffer_s* apb);

/* Fetch the next filled buffer from the mq (or keep draining stale
 * empties).  Returns 1 with cap->pending set, 0 on timeout, -1 when the
 * stream has ended (stopped/completed) or on error. */

static int cap_next_filled(struct native_cap_s* cap)
{
    struct audio_msg_s msg;
    int ret;

    while (1) {
        ret = native_mq_receive_timed(cap->mq, &msg, CAP_MSG_TIMEOUT_SEC);
        if (ret <= 0) {
            if (cap->stopped) {
                nc_probe("NC_MQ_STOP");
                return -1;
            }

            /* [V85] This used to be a bare continue, which made the timeout
             * meaningless: with no filled buffer ever arriving, read() sat
             * here for good and took the whole listen thread with it.
             * Count the empty waits and give up, so the caller sees -EIO
             * and can restart the pipeline instead of hanging. */

            cap->mq_timeouts++;
            if ((cap->mq_timeouts % 3) == 0) {
                char tb[48];

                snprintf(tb, sizeof(tb),
                         "V94_R%u_S%d_T%u_I%u_E%u_P%u_X%d_D%u_O%d",
                         (unsigned)g_v94_recv, (int)g_v94_setupret,
                         (unsigned)g_v94_start, (unsigned)g_v94_isr,
                         (unsigned)g_v94_eof, (unsigned)g_v94_pb,
                         (int)g_v94_i2sret, (unsigned)g_v94_done,
                         (int)g_v94_rxonly);
                diag_report(tb);
            }

            if (cap->mq_timeouts >= CAP_MQ_MAX_TIMEOUTS) {
                nc_probe("NC_MQ_GIVEUP");
                return -1;
            }

            continue;
        }

        switch (msg.msg_id) {
        case AUDIO_MSG_DEQUEUE:
            {
                static int s_dq;

                s_dq++;
                if (s_dq <= 8 || (s_dq % 25) == 0) {
                    char db[32];

                    snprintf(db, sizeof(db), "NC_DQ_%d", s_dq);
                    nc_probe(db);
                }
            }
            if (!msg.u.ptr || cap->outstanding <= 0) {
                break;
            }

            cap->outstanding--;
            if (((struct ap_buffer_s*)msg.u.ptr)->nbytes > 0) {
                cap->pending = (struct ap_buffer_s*)msg.u.ptr;
                cap->pending_off = 0;

                /* [V743] real audio arrived: the silence streak is over.
                 * Without this the counter is "cumulative for the session"
                 * instead of "consecutive", and a talkative session with a
                 * few empty polls still tripped CAP_MQ_MAX_TIMEOUTS and
                 * dropped a perfectly good utterance. */

                cap->mq_timeouts = 0;
                return 1;
            }

            /* Empty buffer (returned on stop / spurious): keep it for
             * re-arming instead of dropping it. */

            cap_v698_push(cap, (struct ap_buffer_s*)msg.u.ptr);
            break;

        case AUDIO_MSG_COMPLETE:
            /* [V688] NEVER let this end a capture.
             *
             * AUDIO_CALLBACK_COMPLETE is raised by the ES8311 *worker
             * thread* as it shuts down
             * (drivers/audio/es8311.c:2766 "Send an AUDIO_MSG_COMPLETE
             * message to the client"), i.e. it is a playback-side
             * teardown notice.  Since v647 pcm0p and pcm0c share one
             * upper half, that notice is delivered to the capture
             * client's mq as well: v687 logged
             *   "[audio_cap_nat] driver completed capture"
             * 0.2 s into every PTT session, and because outstanding was
             * back to 0 the read loop returned -EPIPE and discarded a
             * capture that was still running.
             *
             * A capture ends when WE say so (native_capture_stop), or
             * when the starve detector in cap_next_filled() gives up --
             * never because a *playback* instance finished. */

            /* [V693] The comment above is right; the code was not.
             *
             * cap->complete is read by native_capture_read(): on the very
             * next poll with no filled buffer it returns -EPIPE (-32),
             * which recording_thread() takes as a fatal error.  That path
             * runs recording_cleanup_locked(), which frees the PCM that
             * has been accumulated and resets the state to VOICE_IDLE;
             * voice_channel_stop_with_text() then sees
             * state != VOICE_RECORDING, returns -EINVAL before issuing any
             * ASR request, and the UI stays on the thinking prompt for
             * ever.  v692 showed it on every PTT session, ~190 ms in:
             *
             *   [voice] capture read returned -32, stopping
             *   [voice] abnormal exit, cleaning up
             *
             * A playback instance shutting down is not a capture event.
             * Log it (twice, then stay quiet) and drop it. */
            {
              static int s_v693_complete_seen;

              if (s_v693_complete_seen < 2)
                {
                  s_v693_complete_seen++;
                  syslog(LOG_INFO,
                      "[%s] (ignored) complete notice from the playback side\n",
                      TAG);
                }
            }
            break;

        case AUDIO_MSG_STOP:
            cap->stopped = 1;
            return -1;

        default:
            break;
        }
    }
}

/* Re-arm a drained buffer: enqueue it back as empty so the driver can
 * fill it again.  After stop/abort the buffer just goes to the free
 * pool.  Returns 0 or -errno. */

/* [V698] cap->freeq is calloc(cap->nbuffers, sizeof(*cap->freeq)), so
 * pushing when nfree == nbuffers writes past the array and corrupts whatever
 * heap block follows it.  Count it, report it, and drop the entry rather
 * than scribbling over the heap. */

static void cap_v698_push(struct native_cap_s* cap, struct ap_buffer_s* apb)
{
    if (cap->nfree >= cap->nbuffers) {
        V1024S(LOG_ERR, "[V698]] *** freeq OVERFLOW nfree=%d nb=%d apb=%p\n",
               cap->nfree, cap->nbuffers, apb);
        return;
    }

    cap->freeq[cap->nfree++] = apb;
}

static int cap_rearm(struct native_cap_s* cap, struct ap_buffer_s* apb)
{
    int ret;

    if (cap->stopped || !cap->started) {
        cap_v698_push(cap, apb);
        return 0;
    }

    apb->nbytes = apb->nmaxbytes;   /* [V86] see native_capture_start() */
    apb->curbyte = 0;

    ret = native_enqueue_buffer(cap->fd, apb);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] re-arm enqueue failed: %d\n", TAG, ret);
        cap_v698_push(cap, apb);
        return ret;
    }

    cap->outstanding++;
    return 0;
}

int native_capture_open(native_cap_t** out_cap, const char* dev_path,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample)
{
    struct native_cap_s* cap;
    struct ap_buffer_info_s bufinfo;
    size_t buf_size;
    int ret;
    int i;

    if (!out_cap || !dev_path || channels == 0 || channels > 2) {
        return -EINVAL;
    }

    *out_cap = NULL;

    cap = calloc(1, sizeof(*cap));
    if (!cap) {
        return -ENOMEM;
    }

    cap->fd = -1;
    cap->mq = -1;

    /* Open the raw audio device (e.g. /dev/audio/pcm0c). */

    cap->fd = open(dev_path, O_RDWR);
    if (cap->fd < 0) {
        ret = -errno;
        syslog(LOG_ERR, "[%s] open(%s) failed: %d\n", TAG, dev_path, ret);
        goto err_out;
    }

    if (ioctl(cap->fd, AUDIOIOC_RESERVE, 0) != OK) {
        ret = -errno;
        syslog(LOG_ERR, "[%s] AUDIOIOC_RESERVE failed: %d\n", TAG, ret);
        goto err_out;
    }

    ret = native_mq_create(cap->mqname, sizeof(cap->mqname), &cap->mq);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] mq_create failed: %d\n", TAG, ret);
        goto err_out;
    }

    if (ioctl(cap->fd, AUDIOIOC_REGISTERMQ, (unsigned long)cap->mq) != OK) {
        ret = -errno;
        syslog(LOG_ERR, "[%s] REGISTERMQ failed: %d\n", TAG, ret);
        goto err_out;
    }

    ret = native_check_caps(cap->fd, AUDIO_TYPE_INPUT);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] %s is not an input audio device\n",
            TAG, dev_path);
        goto err_out;
    }

    ret = native_configure(cap->fd, AUDIO_TYPE_INPUT, sample_rate,
        channels, bits_per_sample);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] configure %uHz %uch %ubit failed: %d\n",
            TAG, sample_rate, channels, bits_per_sample, ret);
        goto err_out;
    }

    /* Pipeline geometry: prefer the driver's own numbers. */

    cap->nbuffers = CAP_FALLBACK_NBUFFERS;
    buf_size = CAP_FALLBACK_BUFSIZE;
    if (ioctl(cap->fd, AUDIOIOC_GETBUFFERINFO,
              (unsigned long)&bufinfo) == OK &&
        bufinfo.nbuffers > 0 && bufinfo.buffer_size > 0) {
        cap->nbuffers = (bufinfo.nbuffers > CAP_MAX_NBUFFERS)
            ? CAP_MAX_NBUFFERS : (int)bufinfo.nbuffers;
        buf_size = (bufinfo.buffer_size > CAP_MAX_BUFSIZE)
            ? CAP_MAX_BUFSIZE : (size_t)bufinfo.buffer_size;
    }

    /* [V688] One GDMA descriptor per buffer -- see CAP_FALLBACK_BUFSIZE. */

    if (buf_size > 4092u) {
        buf_size = 4092u;
    }

    cap->apbs = calloc(cap->nbuffers, sizeof(*cap->apbs));
    cap->freeq = calloc(cap->nbuffers, sizeof(*cap->freeq));
    if (!cap->apbs || !cap->freeq) {
        ret = -ENOMEM;
        goto err_out;
    }

    for (i = 0; i < cap->nbuffers; i++) {
        cap->apbs[i] = native_alloc_buffer(cap->fd, buf_size);
        if (!cap->apbs[i]) {
            ret = -ENOMEM;
            syslog(LOG_ERR, "[%s] alloc apb %d failed\n", TAG, i);
            goto err_out;
        }

        cap->freeq[cap->nfree++] = cap->apbs[i];
    }

    *out_cap = cap;
    syslog(LOG_INFO,
        "[%s] native capture ready (%s, %uHz %uch %ubit, %d x %u B)\n",
        TAG, dev_path, sample_rate, channels, bits_per_sample,
        cap->nbuffers, (unsigned)buf_size);
    return 0;

err_out:
    native_capture_close(cap);
    return ret;
}

int native_capture_start(struct native_cap_s* cap)
{
    int ret;
    int i;

    if (!cap) {
        return -EINVAL;
    }

    if (cap->started) {
        return 0;
    }

    if (cap->stopped) {
        return -ECANCELED;
    }

    /* [V85] Prime the pipeline: all empty buffers armed before START so the
     * ADC DMA has somewhere to put the first frames.  Each step is probed
     * because if start() never returns, AC_ST_BEG is the last thing the
     * board ever says and there is no way to tell enqueue from ioctl. */

    /* [V858] *** Re-lock the receiver before anything is armed. ***
     *
     * A capture that starts on a port a playback has just walked away
     * from inherits the transmitter in its playback shape, and the
     * receiver sitting at whatever bit offset that left behind; the
     * [V382] clock-only start inside i2s_rx_channel_start() is skipped
     * in exactly that case (its guard is "if TX is not already
     * started"), and i2s_rx_channel_start() itself early-returns once
     * priv->rx_started is set, so it cannot repair it either.
     *
     * Do it here, before the prime loop, so no descriptor of ours is
     * in flight when the modules are reset.  The counter part in
     * voice_channel.c heals a capture that froze mid-session. */

    esp32s3_i2s_v858_capture_begin();

    nc_probe("NC_ST_BEG");

    /* [V373] Re-enable the silence-playback clock keeper.
     * WHY: the ES8311 only shifts ADC samples out while it is being
     * clocked, and on this board BCLK/WS come from the ESP32-S3 TX
     * generator.  With TX stopped there is no frame clock on the pins,
     * so the codec never puts a single bit on DIN and the RX GDMA waits
     * forever:
     *   [RXDUMP] arm#1 ... isr=0 eof=0  ->  capture EIO, retry 1/5..5/5
     *   [voice] listen: no speech for 8000 ms
     * [V89] had disabled this because opening pcm0p WHILE pcm0c was
     * already streaming deadlocked the two es8311 instances.  Starting
     * the playback side BEFORE the capture is armed avoids that order. */
    /* [V379] cap_clock_start() is OFF again.
     *
     * It kept the I2S transmitter alive by streaming zeroed PCM to
     * /dev/audio/pcm0p during a capture, which is what made RX produce data
     * in the first place.  It is also what kills the board: every 1 KB write
     * restarts the TX DMA (captured live: "[V347] txdma_start #29" followed
     * immediately by "xtensa_user_panic: EXCCAUSE=001c task: es8311"), i.e.
     * the second es8311 instance running while the capture instance is
     * already streaming -- the exact hazard the [V89] note above describes.
     * Enlarging the worker stack to 4096 (V377) did not save it: the dump
     * showed both workers already at 4064 when it died.
     *
     * With V374 having removed the boot speaker test and the always-on
     * listen, nothing else opens pcm0p, so i2s_configure() no longer forces
     * I2S_RX_SLAVE_MOD and the capture side can clock itself. */

    /* [V380] Back ON: phase 1 needs a running transmitter to loop back,
     * phase 2 needs it to put BCK/WS on the pins for the ES8311.
     * The port ping-pong that used to crash here is gone because
     * g_v380_txmaster makes esp32s3_i2s_set_rxonly() a no-op.
     */

    /* [V382] OFF again.  Opening pcm0p while pcm0c is streaming is the
     * [V89] hazard, and v381 proved it still panics the board even with
     * the port ownership locked (EXCCAUSE=001c in the es8311 worker as
     * soon as the keeper's TX DMA starts).  The clock now comes from the
     * [V382] tx clock-only start inside i2s_rx_channel_start().
     */

    /* cap_clock_start(); */

    /* [V380] Phase 1 routes the transmitter back into the receiver; it
     * is off everywhere else so it cannot disturb TTS playback.
     */

    /* [V382] The loopback experiment is parked: with the clock keeper
     * gone the transmitter has no payload to loop back, so it could only
     * ever produce zeros and would tell us nothing.  Phase 1 now just
     * labels the boot self-test.
     */

    if (s_v380_phase == 1)
      {
        /* [V383] Loop the transmitter back into the receiver inside the
         * chip.  The transmitter is running clock-only, so what comes
         * back is zeros -- but the point is whether ANY buffer comes
         * back at all.  If it does, the RX GDMA + SUC_EOF path is fine
         * and the codec / pins are the remaining suspects; if it does
         * not, our receiver cannot move a frame even when one is handed
         * to it on chip.
         */

        g_v380_loopback = 1;
        
      }

    for (i = 0; i < cap->nfree; i++) {
        struct ap_buffer_s* apb = cap->freeq[i];
        char eb[32];

        /* [V86] nbytes is what the driver is being asked to transfer, not
         * what it has already transferred.  es8311_processbegin() turns
         * (nbytes - curbyte) into the I2S timeout, so arming an empty
         * capture buffer with nbytes = 0 asks for zero bytes in zero
         * ticks: I2S_RECEIVE gives up at once, the lower half breaks out
         * of its loop, and no buffer is ever dequeued back to us.  Offer
         * the whole buffer instead. */

        apb->nbytes = apb->nmaxbytes;
        apb->curbyte = 0;

        snprintf(eb, sizeof(eb), "NC_ST_EQ%d_N%u", i,
                 (unsigned)apb->nmaxbytes);
        nc_probe(eb);

        ret = native_enqueue_buffer(cap->fd, apb);
        if (ret < 0) {
            syslog(LOG_ERR, "[%s] prime enqueue failed: %d\n", TAG, ret);
            nc_probe("NC_ST_EQFAIL");
            return ret;
        }

        cap->outstanding++;
    }
    cap->nfree = 0;

    nc_probe("NC_ST_IOCTL");

    if (ioctl(cap->fd, AUDIOIOC_START, 0) != OK) {
        ret = -errno;
        syslog(LOG_ERR, "[%s] AUDIOIOC_START failed: %d\n", TAG, ret);
        nc_probe("NC_ST_IOFAIL");
        return ret;
    }

    cap->started = 1;
    syslog(LOG_INFO, "[%s] capture started (%d buffers armed)\n",
        TAG, cap->outstanding);

    {
        char ob[32];

        snprintf(ob, sizeof(ob), "NC_ST_OK_%d", cap->outstanding);
        nc_probe(ob);
    }

    /* [V660] Ask the I2S layer whether the clocks are actually moving.  The
     * v658 field run captured a constant peak=6, which is neither the [V380]
     * loopback figure (8) nor live microphone audio, so the first thing to
     * establish is whether MCLK/BCLK/WS/DIN are toggling at all. */
    esp32s3_i2s_rx_health();

    return 0;
}

int native_capture_read(struct native_cap_s* cap, void* buf, size_t len)
{
    unsigned char* dst = buf;

    if (!cap || !buf || len == 0) {
        return -EINVAL;
    }

    if (cap->stopped) {
        return -ECANCELED;
    }

    while (1) {
        struct ap_buffer_s* apb;
        size_t avail;
        size_t n;

        if (cap->stopped) {
            return -ECANCELED;
        }

        /* Serve the partially-consumed filled buffer first. */

        apb = cap->pending;
        if (apb) {
            avail = (size_t)apb->nbytes - cap->pending_off;
            if (avail == 0) {
                if (cap_rearm(cap, apb) < 0) {
                    return -EIO;
                }
                cap->pending = NULL;
                continue;
            }

            n = (avail < len) ? avail : len;
            memcpy(dst, apb->samp + cap->pending_off, n);
            cap->pending_off += n;

            /* [V380] What did the port actually hand us?  A healthy
             * loopback shows bytes 08 00 08 00 ... with nz == n/2 and
             * peak == 8.  Real microphone audio shows a much larger
             * peak.  All-zero with nz == 0 is the starvation case.
             */

            {
              static int s_v380_buf;
              const unsigned char *vp = (const unsigned char *)apb->samp +
                                        cap->pending_off - n;
              size_t j;
              size_t nz = 0;
              long peak = 0;
              /* [V729] AC statistics over the whole buffer. */
              long v729min = 0;
              long v729max = 0;
              unsigned long long v729sq = 0ULL;
              unsigned long v729cnt = 0UL;

              s_v380_buf++;

              for (j = 0; j + 1 < n; j += 2)
                {
                  long v = (long)(short)((unsigned)vp[j]
                                          | ((unsigned)vp[j + 1] << 8));
                  /* [V729] Whole-buffer AC statistics.  The [V380]
                   * peak is max|sample|, so it is dominated by the
                   * codec's slow DC wander, and the 8 samples
                   * printed by [V718] span 0.5 ms so their range
                   * only sees the top octave.  Both of those are
                   * noise generated AFTER the MIC PGA, which is
                   * why the v728 REG16 sweep (0x04 -> 0xC4) moved
                   * neither of them.  Buffer-wide min/max and mean
                   * square do cover the speech band, which does go
                   * through the PGA. */
                  if (v < v729min)
                    {
                      v729min = v;
                    }

                  if (v > v729max)
                    {
                      v729max = v;
                    }

                  v729sq += (unsigned long long)((long)v * (long)v);
                  v729cnt++;


                  if (v < 0)
                    {
                      v = -v;
                    }

                  if (v > peak)
                    {
                      peak = v;
                    }

                  if (vp[j] != 0 || vp[j + 1] != 0)
                    {
                      nz++;
                    }
                }

              /* [V899] Bit-phase sanity check.  On a bad boot the RX path
               * comes up with the wrong I2S clock role and every sample is the
               * healthy stream shifted right by 11 bits: the raw-byte OR of the
               * DMA buffer collapses from 0x0000fffc to 0x0000001f.  Measured on
               * V895 (two boots of the same binary, complementary RX/TX).
               * Flip the role once and re-lock -- [V858] heal is the only move
               * that changes the bit phase, and nothing on the ai_vox capture
               * path was calling it (only voice_channel.c did).  Pure insert:
               * the verdict does not alter the samples, it only reports and
               * flips once on the first buffers of a capture start. */
              {
                static unsigned s_v899_bufs;
                static unsigned s_v899_or;
                static int s_v899_healed;
                size_t v899j;
              
                /* [V914] This loop used to fold single BYTES
                 * (s_v899_or |= (unsigned)vp[v899j]), so the
                 * accumulator was capped at 0xff and the verdict
                 * below -- s_v899_or < 0xf000u -- was true on every
                 * boot whatever the microphone was doing.  Three V913
                 * runs printed or=0x000000ff, which is the cap, not a
                 * measurement.  Every boot therefore fired
                 * esp32s3_i2s_v858_heal() on the 8th application
                 * buffer, and heal() relocks, and a relock is a
                 * stop/start that redraws the RX bit phase.
                 *
                 * Fold the little-endian 16-bit words the [V380] /
                 * [V729] loop above uses, so this matches the
                 * driver's [V909] bor.  Bound `v899j + 1 < n` keeps
                 * vp[v899j + 1] in range and drops a trailing odd
                 * byte, which is what the [V380] loop does too. */

                for (v899j = 0; v899j + 1 < n; v899j += 2)
                  {
                    s_v899_or |= (unsigned)vp[v899j]
                                 | ((unsigned)vp[v899j + 1] << 8);
                  }
              
                if (s_v899_bufs < 8u)
                  {
                    s_v899_bufs++;
              
                    if (s_v899_bufs == 8u)
                      {
                        s_v899_healed = (s_v899_or < 0xf000u);
              
                        if (s_v899_healed)
                          {
                            esp32s3_i2s_v858_heal();
                          }
              
                        V1024P("[V899]] or=0x%08x verdict=%s heal=%d "
                               "bufs=%u peak=%ld nz=%zu\n",
                               s_v899_or,
                               (s_v899_or < 0xf000u) ? "BAD" : "GOOD",
                               s_v899_healed, s_v899_bufs, peak, nz);
                      }
                  }
              }
              /* [V718] This body was empty, so the app's view of a buffer
               * was never compared with the driver's.  That comparison is
               * the open question: [V709] post -- read straight out of
               * bfcontainer->apb->samp -- shows audio, while the app's own
               * chunk dump shows 7fff/8000 rails and maxpeak=32768.  The
               * apb pointer below is what pairs the two views, because the
               * driver prints its apb too. */

              if ((s_v380_buf % 4) == 1 && n >= 8)
                {
                  char v718h[160];
                  int  v718p = 0;
                  size_t v718j;

                  for (v718j = 0; v718j < 8 && (v718j * 2 + 1) < n; v718j++)
                    {
                      unsigned v718u =
                          (unsigned)vp[v718j * 2]
                          | ((unsigned)vp[v718j * 2 + 1] << 8);

                      v718p += snprintf(v718h + v718p,
                                        sizeof(v718h) - (size_t)v718p,
                                        "%04x ", v718u);
                    }

                  V1024S(LOG_INFO, "[V718]] app apb=%p off=%u n=%u peak=%ld nz=%u "
                         "ac=%ld ms=%lu b=%s\n",
                         (void *)apb, (unsigned)cap->pending_off,
                         (unsigned)n, peak, (unsigned)nz,
                         (long)(v729max - v729min),
                         (unsigned long)(v729cnt
                             ? (unsigned long)(v729sq / v729cnt)
                             : 0UL),
                         v718h);
                }
            }

            if (cap->pending_off >= (size_t)apb->nbytes) {
                if (cap_rearm(cap, apb) < 0) {
                    return -EIO;
                }
                cap->pending = NULL;
            }

            return (int)n;
        }

        /* No pending data: wait for the driver to hand one back. */

        if (cap_next_filled(cap) <= 0) {
            if (cap->stopped) {
                return -ECANCELED;
            }
            if (cap->complete) {
                return -EPIPE;
            }
            return -EIO;
        }
    }
}

/* [V381] Boot-time capture probe.
 *
 * The V380 diagnostics only print while a capture session is open, and a
 * session used to need somebody to hold the BOOT key at exactly the right
 * moment -- v380run.log has BTN_PRESS = 0 for the whole window, so the one
 * measurement that matters was never taken.  This runs the same open/start/
 * read/abort sequence by itself, once, right after the boot greeting.
 *
 * native_capture_start() is what arms the phase-1 loopback and the clock
 * keeper, so the [V380] dump lines inside native_capture_read() tell us
 * whether the RX DMA path moves any bytes at all.  native_capture_abort()
 * then flips the board to phase 2, so a later real BOOT press measures the
 * true ES8311 path in the same firmware.
 */

/* [V401] 采集自检开关。
 * 1 = 开机跑（当前必崩，用于复现）；0 = 跳过（设备可用）。
 *
 * V398/V400 实测：探针跑起来后 RX 确实收到了第一帧 ——
 *   [RXEOF] first SUC_EOF! isr=1、[RXBOUNCE] 回灌到 PSRAM 缓冲、
 *   [AUDIO-R42] xfer ret=0
 * 但紧接着系统崩溃：EXCCAUSE=001c、VADDR=0x00000000，调用链为
 *   irq_dispatch -> wDev_ProcessFiq -> xtensa_user -> xtensa_user_panic
 *   -> _assert -> up_saveusercontext
 * 也就是崩溃转储时向空指针保存现场（tcb->xcp.regs 为 NULL 时
 * xtensa_user_panic 把 NULL 传了下去），真正的第一现场在 WiFi 闭源 FIQ
 * 与 RX 中断的并发里，被这次二次崩溃掩盖了。
 * 中断栈实测只用了 336 字节（4096 / 6144 / 8192 三档都一样），
 * 所以不是栈容量问题。今晚先把设备保住，置 1 即可继续排查。 */
#define V401_CAPTURE_PROBE 0

void native_capture_selftest(void)
{
#if V401_CAPTURE_PROBE == 0
    V1024S(LOG_INFO, "[V401]] capture probe parked "
                     "(V401_CAPTURE_PROBE=0)\n");
    return;
#else
    native_cap_t* cap = NULL;
    static unsigned char sbuf[4096];
    int rc;
    int i;
    int got = 0;

    

    rc = native_capture_open(&cap, "/dev/audio/pcm0c", 16000, 1, 16);
    if (rc != 0 || cap == NULL)
      {
        
        return;
      }

    rc = native_capture_start(cap);
    if (rc != 0)
      {
        
        native_capture_abort(cap);
        native_capture_close(cap);
        return;
      }

    for (i = 0; i < 8; i++)
      {
        int n = native_capture_read(cap, sbuf, sizeof(sbuf));

        if (n > 0)
          {
            got += n;
            if (got >= 8192)
              {
                break;
              }
          }
        else if (n == -EIO)
          {
            /* cap_next_filled() already waited CAP_MSG_TIMEOUT_SEC. */
            continue;
          }
        else
          {
            
            break;
          }
      }

    

    native_capture_abort(cap);
    native_capture_close(cap);
#endif
}

int native_capture_abort(struct native_cap_s* cap)
{
    struct audio_msg_s msg;

    if (!cap) {
        return -EINVAL;
    }

    /* [V88] Let the silence stream go: nothing is capturing any more. */

    cap_clock_stop();

    /* [V380] Leave the loopback behind and remember that the next
     * session listens to the real ES8311.
     */

    g_v380_loopback = 0;

    if (s_v380_phase == 1)
      {
        s_v380_phase = 2;
        
      }

    if (cap->started) {
        cap->started = 0;
        ioctl(cap->fd, AUDIOIOC_STOP, 0);
    }

    V1024S(LOG_INFO, "[V750]] abort: stopped, flushing\n");

    /* [V697] Hand back whatever the RX DMA still owns BEFORE the buffers are
     * freed.  Without this the driver kept a container pointing at an apb
     * this function is about to release, and the next DMA completion wrote
     * into freed memory -- the heap corruption that took the board down
     * (mm_malloc, EXCCAUSE=001d, vaddr=0x8) and left the screen on
     * "正在聆听". */

    esp32s3_i2s_rx_flush();

    cap->stopped = 1;

    /* [V750] Drain completion messages -- WITH A BOUND.
     *
     * 原来的 while (native_mq_receive_timed(cap->mq, &msg, 1) > 0) 没有上界：
     * 只要驱动还在往 mq 里投递消息（RX DMA 没真正停下来、V380 环回仍在喂
     * 数、或队列里压着大量非 DEQUEUE 消息），它就一轮接一轮永不退出 ——
     * 每条消息都是"立刻返回"，所以连那个 1 秒超时也帮不上忙。
     *
     * 实测后果（v749 8 分钟回归，t=76.6）：voice_channel_stop() 卡在这个
     * 循环里 400 s，state 永远停在 VOICE_STOPPING，命令工作线程随之永久
     * 卡死，s_ptt_hold 再也解不开，按 BOOT 完全没有反应。
     *
     * 加双重上界：最多 64 条消息、整体不超过 2000 ms。超了就放弃剩下的
     * （泄漏几个 apb 远比整机僵死好）。 */

    V1024S(LOG_INFO, "[V750]] abort: drain start mq=%d outstanding=%d\n",
           cap->mq, cap->outstanding);

    {
        /* [V984] *** Do not pay a 1 s timeout for a queue that is empty. ***
         *
         * native_mq_receive_timed() takes SECONDS and the loop below calls it
         * with 1, so on the normal path -- esp32s3_i2s_rx_flush() has already
         * handed the DMA buffers back, outstanding is stale at 1 and no
         * completion message will ever arrive -- the first receive waits the
         * full second, times out, and only then breaks.  Measured: abort
         * drain start 111.22 -> abort done 112.22, exactly 1.00 s, 123 times
         * in one 508 s session, i.e. ~24 % of the time with capture stopped.
         *
         * So look first: mq_curmsgs is how many messages are actually queued.
         * The loop below then stops before issuing a receive it does not
         * expect to succeed, which makes an empty drain cost nothing.
         * -1 means mq_getattr failed: fall back to the old behaviour. */
        int          v984_pending = -1;
        struct mq_attr v984_attr;

        if (mq_getattr(cap->mq, &v984_attr) == OK)
          {
            v984_pending = (int)v984_attr.mq_curmsgs;
          }

        int   drained = 0;
        clock_t t0 = clock_systime_ticks();

        while (drained < 512 &&   /* [V886] was 64 */
               (long)((clock_systime_ticks() - t0) * 10) < 6000L)  /* [V886] 2000 */
          {
            /* [V984] Everything that was queued has been drained: stop before
             * the blocking receive, which would otherwise sit out its full
             * 1 s timeout and return nothing. */
            if (v984_pending >= 0 && drained >= v984_pending)
              {
                break;
              }

            if (native_mq_receive_timed(cap->mq, &msg, 1) <= 0)
              {
                break;
              }

            if (msg.msg_id == AUDIO_MSG_DEQUEUE && msg.u.ptr &&
                cap->outstanding > 0)
              {
                cap->outstanding--;
                cap_v698_push(cap, (struct ap_buffer_s*)msg.u.ptr);
              }

            drained++;
          }

        /* [V984] Proof for the next capture: pending is what we saw queued,
         * drained is what we actually took.  Both 0 means the 1 s wait is
         * gone; pending>0 drained<pending means messages were still in
         * flight and we abandoned them (the trade V750 already accepts). */
        {
          static unsigned s_v984_n;

          V1024S(LOG_INFO, "[V984]] drain #%u pending=%d drained=%d out=%d\n",
                 ++s_v984_n, v984_pending, drained, cap->outstanding);
        }

        if (drained >= 512 ||   /* [V886] was 64 */
            (long)((clock_systime_ticks() - t0) * 10) >= 6000L) /* [V886] 2000 */
          {
            V1024S(LOG_ERR, "[V750]] abort: drain hit bound (%d msgs, %ld ms) -- "
                   "%d buffer(s) abandoned\n",
                   drained,
                   (long)((clock_systime_ticks() - t0) * 10),
                   cap->outstanding);
          }
    }

    V1024S(LOG_INFO, "[V750]] abort: done\n");

    return 0;
}

void native_capture_close(struct native_cap_s* cap)
{
    struct audio_msg_s msg;
    int i;

    if (!cap) {
        return;
    }

    /* Stop the stream so in-flight buffers come back before we free
     * them, then drain the completion queue. */

    V1024S(LOG_INFO, "[V750]] close: enter fd=%d started=%d out=%d\n",
           cap->fd, (int)cap->started, cap->outstanding);

    if (cap->fd >= 0 && (cap->started || cap->outstanding > 0)) {
        cap->started = 0;
        ioctl(cap->fd, AUDIOIOC_STOP, 0);
    }

    /* [V697] See native_capture_abort().  close() can be reached without
     * abort() (open() failure paths), so flush here as well. */

    esp32s3_i2s_rx_flush();

    cap->stopped = 1;

    if (cap->mq >= 0) {
        /* [V750] 同 native_capture_abort()：排空必须有上界，否则 DMA 一直
         * 投递消息时这里就是无期徒刑。 */

        /* [V984] *** Do not pay a 1 s timeout for a queue that is empty. ***
         *
         * native_mq_receive_timed() takes SECONDS and the loop below calls it
         * with 1, so on the normal path -- esp32s3_i2s_rx_flush() has already
         * handed the DMA buffers back, outstanding is stale at 1 and no
         * completion message will ever arrive -- the first receive waits the
         * full second, times out, and only then breaks.  Measured: abort
         * drain start 111.22 -> abort done 112.22, exactly 1.00 s, 123 times
         * in one 508 s session, i.e. ~24 % of the time with capture stopped.
         *
         * So look first: mq_curmsgs is how many messages are actually queued.
         * The loop below then stops before issuing a receive it does not
         * expect to succeed, which makes an empty drain cost nothing.
         * -1 means mq_getattr failed: fall back to the old behaviour. */
        int          v984_pending = -1;
        struct mq_attr v984_attr;

        if (mq_getattr(cap->mq, &v984_attr) == OK)
          {
            v984_pending = (int)v984_attr.mq_curmsgs;
          }

        int   drained = 0;
        clock_t t0 = clock_systime_ticks();

        while (drained < 512 &&   /* [V886] was 64 */
               (long)((clock_systime_ticks() - t0) * 10) < 6000L)  /* [V886] 2000 */
          {
            /* [V984] Everything that was queued has been drained: stop before
             * the blocking receive, which would otherwise sit out its full
             * 1 s timeout and return nothing. */
            if (v984_pending >= 0 && drained >= v984_pending)
              {
                break;
              }

            if (native_mq_receive_timed(cap->mq, &msg, 1) <= 0)
              {
                break;
              }

            if (msg.msg_id == AUDIO_MSG_DEQUEUE && msg.u.ptr &&
                cap->outstanding > 0)
              {
                cap->outstanding--;
                cap_v698_push(cap, (struct ap_buffer_s*)msg.u.ptr);
              }

            drained++;
          }

        /* [V984] Proof for the next capture: pending is what we saw queued,
         * drained is what we actually took.  Both 0 means the 1 s wait is
         * gone; pending>0 drained<pending means messages were still in
         * flight and we abandoned them (the trade V750 already accepts). */
        {
          static unsigned s_v984_n;

          V1024S(LOG_INFO, "[V984]] drain #%u pending=%d drained=%d out=%d\n",
                 ++s_v984_n, v984_pending, drained, cap->outstanding);
        }

        if (drained >= 512 ||   /* [V886] was 64 */
            (long)((clock_systime_ticks() - t0) * 10) >= 6000L) /* [V886] 2000 */
          {
            V1024S(LOG_ERR, "[V750]] close: drain hit bound (%d msgs, %ld ms) -- "
                   "%d buffer(s) abandoned\n",
                   drained,
                   (long)((clock_systime_ticks() - t0) * 10),
                   cap->outstanding);
          }

        ioctl(cap->fd, AUDIOIOC_UNREGISTERMQ, (unsigned long)cap->mq);
        mq_close(cap->mq);
        mq_unlink(cap->mqname);
        cap->mq = -1;
    }

    if (cap->fd >= 0) {
        ioctl(cap->fd, AUDIOIOC_RELEASE, 0);

        /* [V886] NEVER free a buffer the driver may still be DMA-ing into.
         *
         * The drain above is bounded, so it can and does finish with
         * cap->outstanding > 0 ("N buffer(s) abandoned").  Freeing the
         * apbs anyway hands their memory back to the heap while the I2S
         * RX descriptor still points at it -- a use-after-free whose
         * symptom is exactly what we see: an InstructionFetchErrorCause
         * (EXCCAUSE 2) in the es8311 worker a few seconds after a
         * capture starts, at a PC that is not a code address.
         *
         * v733 already established the rule for the sibling path: when
         * the owner has not let go, LEAK rather than free.  A leaked
         * buffer costs bytes; a freed one costs the board. */
        if (cap->apbs) {
            if (cap->outstanding == 0) {
                for (i = 0; i < cap->nbuffers; i++) {
                    native_free_buffer(cap->fd, cap->apbs[i]);
                }
            } else {
                V1024S(LOG_ERR, "[V886]] close: %d buffer(s) still outstanding, "
                       "leaking them on purpose (freeing would be a UAF)\n",
                       cap->outstanding);
            }
        }

        close(cap->fd);
        cap->fd = -1;
    }

    free(cap->apbs);
    free(cap->freeq);
    free(cap);

    syslog(LOG_INFO, "[%s] closed\n", TAG);
}
