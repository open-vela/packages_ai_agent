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

#include "voice/voice_channel.h"
#ifdef CONFIG_AI_AGENT_AUDIO_PREPROCESS
#include <compexp.h>
#include <echo_canceller.h>
#include <heap_api.h>
#endif
#include "core/message_bus.h"
#include "infra/config_store.h"
#include "agent_compat.h"
#include "agent_config.h"
#include <nuttx/audio/audio.h>

#include "system/nxrecorder.h"
#include "voice/audio_capture.h"
#include "voice/audio_playback.h"
#include "voice/voice_asr.h"
#include "voice/voice_tts.h"
#include "voice/volc_asr.h"
#include "voice/volc_tts.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/timers/pwm.h>

static const char* TAG = "voice";
static int s_backends_registered;

/* ── Audio preprocessing (NS + AGC via BES ec2float) ────── */

#ifdef CONFIG_AI_AGENT_AUDIO_PREPROCESS

#define PREPROC_HEAP_SIZE (150 * 1024)
#define PREPROC_FRAME_MS 15
#define PREPROC_FRAME_SIZE \
    (AGENT_VOICE_SAMPLE_RATE / 1000 * PREPROC_FRAME_MS)

static const Ec2FloatConfig s_ns_cfg = {
    .bypass = 0,
    .hpf_enabled = 1,
    .af_enabled = 0, /* no AEC */
    .adprop_enabled = 0,
    .varistep_enabled = 0,
    .nlp_enabled = 0, /* no NLP */
    .clip_enabled = 0,
    .stsupp_enabled = 0,
    .hfsupp_enabled = 0,
    .constrain_enabled = 0,
    .ns_enabled = 1, /* noise suppression ON */
    .cng_enabled = 0,
    .blocks = 1,
    .delay = 0,
    .gamma = 0.9f,
    .echo_band_start = 300,
    .echo_band_end = 1800,
    .min_ovrd = 2,
    .target_supp = -40,
    .highfre_band_start = 4000,
    .highfre_supp = 8.f,
    .noise_supp = -15,
    .cng_type = 0,
    .cng_level = -60,
    .clip_threshold = -20.f,
    .banks = 64,
};

static const CompexpConfig s_agc_cfg = {
    .bypass = 0,
    .type = 0,
    .comp_threshold = -20.f,
    .comp_ratio = 2.f,
    .expand_threshold = -45.f,
    .expand_ratio = 0.5556f,
    .attack_time = 0.001f,
    .release_time = 0.006f,
    .makeup_gain = 6,
    .delay = 32,
    .tav = 0.2f,
};

typedef struct {
    void* heap;
    Ec2FloatState* ns;
    CompexpState* agc;
} audio_preproc_t;

static audio_preproc_t* preproc_create(void)
{
    audio_preproc_t* pp = calloc(1, sizeof(*pp));
    if (!pp) {
        return NULL;
    }

    pp->heap = malloc(PREPROC_HEAP_SIZE);
    if (!pp->heap) {
        free(pp);
        return NULL;
    }
    med_heap_init(pp->heap, PREPROC_HEAP_SIZE);

    pp->ns = ec2float_create(AGENT_VOICE_SAMPLE_RATE,
        PREPROC_FRAME_SIZE, 0, &s_ns_cfg);
    pp->agc = compexp_create(AGENT_VOICE_SAMPLE_RATE,
        PREPROC_FRAME_SIZE, &s_agc_cfg);

    syslog(LOG_INFO, "[%s] preproc: NS=%p AGC=%p heap=%dKB\n",
        TAG, (void*)pp->ns, (void*)pp->agc,
        PREPROC_HEAP_SIZE / 1024);
    return pp;
}

static void preproc_destroy(audio_preproc_t* pp)
{
    if (!pp) {
        return;
    }
    if (pp->agc) {
        compexp_destroy(pp->agc);
    }
    if (pp->ns) {
        ec2float_destroy(pp->ns);
    }
    free(pp->heap);
    free(pp);
}

static void preproc_chunk(audio_preproc_t* pp,
    unsigned char* buf, int bytes)
{
    if (!pp || (!pp->ns && !pp->agc)) {
        return;
    }

    int16_t* samples = (int16_t*)buf;
    int total = bytes / 2;
    int fsz = PREPROC_FRAME_SIZE;
    int32_t in32[PREPROC_FRAME_SIZE];
    int32_t ref32[PREPROC_FRAME_SIZE];
    int32_t out32[PREPROC_FRAME_SIZE];

    memset(ref32, 0, sizeof(ref32));

    for (int off = 0; off < total; off += fsz) {
        int n = (off + fsz <= total) ? fsz : (total - off);

        for (int i = 0; i < n; i++) {
            in32[i] = samples[off + i];
        }

        if (pp->ns) {
            ec2float_process(pp->ns, in32, ref32, n, out32);
        } else {
            memcpy(out32, in32, n * sizeof(int32_t));
        }

        if (pp->agc) {
            for (int i = 0; i < n; i++) {
                out32[i] <<= 8;
            }
            compexp_process_int24(pp->agc, out32, n);
            for (int i = 0; i < n; i++) {
                out32[i] >>= 8;
            }
        }

        for (int i = 0; i < n; i++) {
            int32_t v = out32[i];
            if (v > 32767) {
                v = 32767;
            } else if (v < -32768) {
                v = -32768;
            }
            samples[off + i] = (int16_t)v;
        }
    }
}

#endif /* CONFIG_AI_AGENT_AUDIO_PREPROCESS */

/* ── Recording state machine (PTT mode) ─────────────────── */

enum voice_state {
    VOICE_IDLE = 0,
    VOICE_RECORDING,
    VOICE_STOPPING
};

static struct {
    enum voice_state state;
    pthread_mutex_t lock;
    audio_capture_t* cap;
    unsigned char* pcm_buf; /* accumulated PCM data */
    size_t pcm_len; /* bytes recorded so far */
    size_t pcm_cap; /* buffer capacity */
    pthread_t rec_thread;
    voice_asr_stream_t* asr_stream; /* streaming ASR handle */
    sem_t rec_ready; /* posted by recording_thread when ready to consume */
    volatile int tts_abort; /* set to 1 to stop active TTS playback */
    audio_playback_t* tts_pb; /* active TTS playback handle (or NULL) */
    pthread_mutex_t speak_lock; /* serialize concurrent speak calls */
} s_voice = {
    .state = VOICE_IDLE,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .speak_lock = PTHREAD_MUTEX_INITIALIZER,
};

/* ── File-based helpers (kept for QEMU test commands) ──── */

/* What a PCM file claims to be.  nxrecorder writes headerless PCM, so a
 * file may or may not describe itself. */
typedef struct {
    unsigned int channels;
    unsigned int rate;
    unsigned int bits;
} pcm_format_t;

/* nxrecorder's own defaults ("record" with no arguments), used when a file
 * carries no header and the caller did not say either. */
#define PCM_DEFAULT_CHANNELS 2
#define PCM_DEFAULT_RATE     48000
#define PCM_DEFAULT_BITS     16

static unsigned int rd_u16(const unsigned char* p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}
static unsigned int rd_u32(const unsigned char* p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

/* Scan a RIFF/WAVE container for its "fmt " chunk.  Returns the offset of
 * the sample data and fills in fmt, or -1 if this is not a WAV file. */
static long wav_probe(const unsigned char* buf, size_t len, pcm_format_t* fmt)
{
    size_t pos = 12;
    long data_off = -1;

    if (len < 44 || memcmp(buf, "RIFF", 4) != 0 ||
        memcmp(buf + 8, "WAVE", 4) != 0) {
        return -1;
    }

    while (pos + 8 <= len) {
        unsigned int csize = rd_u32(buf + pos + 4);
        size_t next;

        /* A chunk longer than the file is a corrupt size, not a big
         * chunk: bail rather than wrap around on the arithmetic below.
         */

        if (csize > len - pos - 8) {
            break;
        }

        next = pos + 8 + csize + (csize & 1);

        if (memcmp(buf + pos, "fmt ", 4) == 0 && csize >= 16) {
            fmt->channels = rd_u16(buf + pos + 10);
            fmt->rate = rd_u32(buf + pos + 12);
            fmt->bits = rd_u16(buf + pos + 22);
        } else if (memcmp(buf + pos, "data", 4) == 0 &&
                   data_off < 0) {
            data_off = (long)(pos + 8);
        }

        if (data_off >= 0 && fmt->rate && fmt->channels && fmt->bits) {
            return data_off;
        }

        pos = next;
    }

    return -1;
}

static int read_pcm_file(const char* path, pcm_format_t* fmt,
    unsigned char** out, size_t* out_len)
{
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        syslog(LOG_ERR, "[%s] Cannot open %s: %d\n",
            TAG, path, errno);
        return -errno;
    }

    off_t fsize = lseek(fd, 0, SEEK_END);

    if (fsize <= 0 || (size_t)fsize > AGENT_VOICE_PCM_FILE_MAX) {
        syslog(LOG_ERR, "[%s] File too large or empty: %ld (max %d)\n",
            TAG, (long)fsize, AGENT_VOICE_PCM_FILE_MAX);
        close(fd);
        return -EFBIG;
    }

    lseek(fd, 0, SEEK_SET);

    unsigned char* buf = malloc((size_t)fsize);

    if (!buf) {
        close(fd);
        return -ENOMEM;
    }

    ssize_t nread = read(fd, buf, (size_t)fsize);

    close(fd);

    if (nread != (ssize_t)fsize) {
        free(buf);
        return -EIO;
    }

    /* A WAV container describes itself, which beats anything the caller
     * guessed.  Otherwise fall back to the caller's idea of the format,
     * and failing that to what nxrecorder produces by default.
     */

    pcm_format_t parsed = { 0, 0, 0 };
    long data_off = wav_probe(buf, (size_t)fsize, &parsed);

    if (data_off >= 0) {
        *fmt = parsed;
    } else {
        data_off = 0;
    }

    if (!fmt->channels) {
        fmt->channels = PCM_DEFAULT_CHANNELS;
    }
    if (!fmt->rate) {
        fmt->rate = PCM_DEFAULT_RATE;
    }
    if (!fmt->bits) {
        fmt->bits = PCM_DEFAULT_BITS;
    }

    if (fmt->bits != 16) {
        syslog(LOG_ERR, "[%s] Only 16 bit samples are supported, got %u\n",
            TAG, fmt->bits);
        free(buf);
        return -ENOTSUP;
    }

    /* Slide the samples down over the container so the caller gets one
     * buffer of PCM and frees the pointer it was given. */

    if (data_off > 0) {
        memmove(buf, buf + data_off, (size_t)fsize - (size_t)data_off);
    }

    *out = buf;
    *out_len = (size_t)fsize - (size_t)data_off;
    return 0;
}

static int write_pcm_file(const char* path,
    const unsigned char* data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0) {
        syslog(LOG_ERR, "[%s] Cannot create %s: %d\n",
            TAG, path, errno);
        return -errno;
    }

    ssize_t nw = write(fd, data, len);

    close(fd);
    return (nw == (ssize_t)len) ? 0 : -EIO;
}

/* ── Recording cleanup helper ────────────────────────────── */

static void recording_cleanup_locked(voice_asr_stream_t* stream)
{
    /* Abort streaming ASR if active */
    if (stream) {
        voice_asr_stream_abort(stream);
        s_voice.asr_stream = NULL;
    }

    /* Close capture device */
    if (s_voice.cap) {
        audio_capture_close(s_voice.cap);
        s_voice.cap = NULL;
    }

    /* Free PCM buffer */
    free(s_voice.pcm_buf);
    s_voice.pcm_buf = NULL;
    s_voice.pcm_len = 0;

    /* Reset state to IDLE */
    s_voice.state = VOICE_IDLE;
}

/* Allocate fallback PCM buffer (must hold lock) */
static int alloc_fallback_buffer_locked(void)
{
    if (s_voice.pcm_buf) {
        return 0;
    }

    s_voice.pcm_buf = malloc(s_voice.pcm_cap);
    if (!s_voice.pcm_buf) {
        syslog(LOG_ERR, "[%s] fallback buffer alloc failed\n", TAG);
        return -ENOMEM;
    }

    s_voice.pcm_len = 0;
    return 0;
}

/* Accumulate PCM chunk to fallback buffer (must hold lock) */
static void accumulate_pcm_locked(const unsigned char* chunk, size_t len)
{
    if (s_voice.pcm_buf && s_voice.pcm_len + len <= s_voice.pcm_cap) {
        memcpy(s_voice.pcm_buf + s_voice.pcm_len, chunk, len);
        s_voice.pcm_len += len;
    }
}

/* Process one audio chunk: send to ASR or accumulate for fallback */
static int process_audio_chunk(voice_asr_stream_t** stream_ptr,
    const unsigned char* chunk, size_t len, int* need_fallback,
    size_t* bytes_sent)
{
    if (*need_fallback) {
        pthread_mutex_lock(&s_voice.lock);
        accumulate_pcm_locked(chunk, len);
        pthread_mutex_unlock(&s_voice.lock);
    }

    if (*stream_ptr) {
        int ret = voice_asr_stream_send(*stream_ptr, chunk, len);

        if (ret != 0) {
            syslog(LOG_ERR,
                "[%s] stream send failed: %d, batch fallback\n", TAG, ret);
            voice_asr_stream_abort(*stream_ptr);
            *stream_ptr = NULL;
            *need_fallback = 1;

            pthread_mutex_lock(&s_voice.lock);
            alloc_fallback_buffer_locked();
            pthread_mutex_unlock(&s_voice.lock);
        } else {
            *bytes_sent += len;
        }
    }

    return 0;
}

/* ── Recording thread (PTT mode — streaming ASR) ─────────── */

/* ── Capture-to-file diagnostic ─────────────────────────────────
 *
 * While PTT_RECORD_ONLY_PATH is defined, the push-to-talk key records the
 * raw microphone stream to that file and skips the ASR and the agent
 * entirely, so the capture path can be listened to on a PC.  Comment the
 * define out to go back to live recognition.
 */

#define PTT_RECORD_ONLY_PATH "/mnt/ptt.wav"

/* Capture format while recording to a file: the same one the nxrecorder
 * "record" command uses, which is the combination known to give clean
 * audio on this board.  Capturing at 16kHz instead comes out as loud
 * noise.  Live recognition asks the driver for 16kHz mono instead, and
 * audio_capture folds the 48kHz stereo device stream down to it. */

#ifdef PTT_RECORD_ONLY_PATH
#  define PTT_CAPTURE_RATE     48000
#  define PTT_CAPTURE_CHANNELS 2
/* Device native, so bytes track wall clock at 192KB/s: a 3200 byte read
 * would be 17ms of audio and the buffer could never get back to the
 * device in time. */
#  define CHUNK_SIZE           32768
#else
#  define PTT_CAPTURE_RATE     AGENT_VOICE_SAMPLE_RATE
#  define PTT_CAPTURE_CHANNELS AGENT_VOICE_CHANNELS
#  define CHUNK_SIZE           AGENT_ASR_CHUNK_SIZE
#endif

#ifdef PTT_RECORD_ONLY_PATH
static int s_dump_fd = -1;

/* The capture is written as a real RIFF/WAVE file so it drops straight
 * onto a PC without any format guessing.  The sizes are filled in on
 * close, when the length is known. */

static void wav_put16(unsigned char* p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
}

static void wav_put32(unsigned char* p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

static int wav_write_header(int fd, unsigned rate,
    unsigned channels, unsigned bits)
{
    unsigned char h[44];

    memset(h, 0, sizeof(h));
    memcpy(h, "RIFF", 4);
    wav_put32(h + 4, 36);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    wav_put32(h + 16, 16);
    wav_put16(h + 20, 1); /* PCM */
    wav_put16(h + 22, channels);
    wav_put32(h + 24, rate);
    wav_put32(h + 28, rate * channels * (bits / 8));
    wav_put16(h + 32, channels * (bits / 8));
    wav_put16(h + 34, bits);
    memcpy(h + 36, "data", 4);
    wav_put32(h + 40, 0);

    return write(fd, h, sizeof(h)) == sizeof(h) ? 0 : -EIO;
}

static void wav_finish(int fd)
{
    unsigned char b[4];
    off_t total = lseek(fd, 0, SEEK_END);

    if (total < 44) {
        return;
    }

    wav_put32(b, (unsigned)(total - 8));
    if (lseek(fd, 4, SEEK_SET) < 0 || write(fd, b, 4) != 4) {
        return;
    }

    wav_put32(b, (unsigned)(total - 44));
    if (lseek(fd, 40, SEEK_SET) < 0 || write(fd, b, 4) != 4) {
        return;
    }

    lseek(fd, 0, SEEK_END);
}

/* Per-channel statistics.  The device always captures stereo and one of
 * the two slots is the microphone while the other is a grounded input on
 * this board, so the two must be looked at separately: scanning only one
 * hides which one carries the speech.  The zero crossing rate is what
 * separates a voice recording from a noise floor - speech sits around
 * 500-2500 Hz while noise on a 48kHz stream runs up near 16000 Hz. */
static void wav_analyze(int fd, unsigned rate, unsigned channels)
{
    static unsigned char buf[4096];
    off_t off = 44;
    off_t total = lseek(fd, 0, SEEK_END);
    long frames = 0;
    unsigned c;

    long peak[2] = {0, 0};
    long sum[2] = {0, 0};
    long dmax[2] = {0, 0};
    long dsum[2] = {0, 0};
    long zc[2] = {0, 0};
    long big[2] = {0, 0};
    long sumbuf[64];
    long cntbuf[64];
    int16_t prev[2] = {0, 0};
    int have_prev = 0;

    if (channels == 0 || rate == 0) {
        return;
    }

    memset(sumbuf, 0, sizeof(sumbuf));
    memset(cntbuf, 0, sizeof(cntbuf));

    if (lseek(fd, off, SEEK_SET) < 0) {
        return;
    }

    while (off < total) {
        ssize_t n = read(fd, buf, sizeof(buf));
        size_t i;

        if (n < 0) {
            printf("analyze: read failed at %ld: %d\n", (long)off, errno);
            break;
        }
        if (n == 0) {
            break;
        }

        for (i = 0; i + channels * 2 <= (size_t)n; i += channels * 2) {
            for (c = 0; c < channels && c < 2; c++) {
                int16_t s = (int16_t)(buf[i + c * 2] |
                                      (buf[i + c * 2 + 1] << 8));
                long v = s < 0 ? -s : s;

                if (v > peak[c]) {
                    peak[c] = v;
                }
                sum[c] += v;

                /* Level per buffer: a dropped or starved buffer shows up
                 * here as a collapse, which a threshold on the sample
                 * size cannot show. */
                {
                    size_t b = (size_t)(frames / 8192);

                    if (b < sizeof(sumbuf) / sizeof(sumbuf[0])) {
                        sumbuf[b] += v;
                        cntbuf[b]++;
                    }
                }

                if (have_prev) {
                    long d = (long)s - (long)prev[c];

                    if (d < 0) {
                        d = -d;
                    }
                    dsum[c] += d;
                    if (d > dmax[c]) {
                        dmax[c] = d;
                    }
                    if (d > 4000) {
                        big[c]++;
                    }
                    if ((s < 0) != (prev[c] < 0)) {
                        zc[c]++;
                    }
                }

                prev[c] = s;
            }

            have_prev = 1;
            frames++;
        }

        off += n;
    }

    if (frames == 0) {
        printf("analyze: no samples\n");
        return;
    }

    printf("analyze: %ld frames @%uHz x%uch\n", frames, rate, channels);

    /* First frames as raw 32-bit words.  A stereo frame is one word with
     * one half word per channel, so the pattern here says outright
     * whether the layout is what the analyzer assumes. */
    printf("  first frames:");
    {
        unsigned char raw[64];
        size_t i;

        if (lseek(fd, 44, SEEK_SET) >= 0) {
            ssize_t nr = read(fd, raw, sizeof(raw));

            for (i = 0; i + 4 <= (size_t)nr; i += 4) {
                printf(" %02x%02x%02x%02x",
                    raw[i + 3], raw[i + 2], raw[i + 1], raw[i]);
            }
        }
    }
    printf("\n");

    if (lseek(fd, 44 + 4 * 4096, SEEK_SET) >= 0) {
        unsigned char raw[64];
        ssize_t nr = read(fd, raw, sizeof(raw));
        size_t i;

        printf("  mid frames:  ");
        for (i = 0; i + 4 <= (size_t)nr; i += 4) {
            printf(" %02x%02x%02x%02x",
                raw[i + 3], raw[i + 2], raw[i + 1], raw[i]);
        }
        printf("\n");
    }

    for (c = 0; c < channels && c < 2; c++) {
        printf("  ch%u: peak=%ld mean_abs=%ld step_max=%ld step_mean=%ld\n",
            c, peak[c], sum[c] / frames, dmax[c], dsum[c] / frames);
        printf("       mean/peak x1000=%ld  steps>4000=%ld  zcr=%ldHz\n",
            peak[c] ? (dsum[c] / frames) * 1000 / peak[c] : 0,
            big[c], zc[c] * (long)rate / (long)frames);
    }

    /* Mean level per buffer.  A gap or a starved buffer shows up as a
     * collapse here. */
    printf("  mean level per buffer:");
    {
        size_t nb = (size_t)(frames / 8192) + 1;
        size_t b;

        if (nb > sizeof(sumbuf) / sizeof(sumbuf[0])) {
            nb = sizeof(sumbuf) / sizeof(sumbuf[0]);
        }
        for (b = 0; b < nb; b++) {
            printf(" %ld", cntbuf[b] ? sumbuf[b] / cntbuf[b] : 0);
        }
    }
    printf("\n");
}
/* Analyze any recording on the card, not just the one the push-to-talk
 * path just made.  Lets a capture from nxrecorder be compared against one
 * from audio_capture without re-recording either. */
int voice_channel_analyze_file(const char* path)
{
    int fd;

    if (!path) {
        return -EINVAL;
    }

    fd = open(path, O_RDWR);
    if (fd < 0) {
        printf("cannot open %s: %d\n", path, errno);
        return -errno;
    }

    wav_analyze(fd, PTT_CAPTURE_RATE, PTT_CAPTURE_CHANNELS);
    close(fd);
    return 0;
}

#endif

static void* recording_thread(void* arg)
{
    (void)arg;
    unsigned char* chunk;
    int need_fallback = 0;
    size_t total_bytes_read = 0;
    size_t total_bytes_sent = 0;
    int chunk_count = 0;
    int abnormal_exit = 0;
#ifdef PTT_RECORD_ONLY_PATH
    long max_write_us = 0;
    struct timespec t_start;
#endif

    /* Sized so one read drains a whole device buffer.  Walking a buffer
     * in ten small reads starved the DMA queue: a buffer cannot go back
     * to the device until it has been fully consumed, so tiny reads mean
     * tiny effective buffering, and the 8 byte I2S FIFO then overruns and
     * drops samples - the same stuttering that a slow write target caused
     * before.  32KB is the ES8388 buffer size. */

    chunk = malloc(CHUNK_SIZE);
    if (!chunk) {
        syslog(LOG_ERR, "[%s] chunk alloc failed\n", TAG);
        sem_post(&s_voice.rec_ready);
        return NULL;
    }

#ifdef PTT_RECORD_ONLY_PATH
    clock_gettime(CLOCK_MONOTONIC, &t_start);
#endif

    syslog(LOG_INFO, "[%s] recording thread started (streaming)\n", TAG);

    /* Use pre-connected ASR stream if available (opened in
     * voice_channel_start to avoid TLS handshake delay during
     * recording).  Fall back to opening a new session if not. */
    pthread_mutex_lock(&s_voice.lock);
    voice_asr_stream_t* stream = s_voice.asr_stream;
    s_voice.asr_stream = NULL; /* thread now owns it */
    pthread_mutex_unlock(&s_voice.lock);

    if (!stream) {
#ifdef PTT_RECORD_ONLY_PATH
        /* Nothing to recognise: the audio is going to a file. */
        syslog(LOG_INFO, "[%s] capture-to-file, no ASR session\n", TAG);
        (void)need_fallback;
#else
        /* No pre-connected stream — open now (may lose first 300-800ms) */
        stream = voice_asr_stream_open();
        syslog(LOG_WARNING,
            "[%s] streaming ASR unavailable, batch fallback\n", TAG);
        need_fallback = 1;

        pthread_mutex_lock(&s_voice.lock);
        if (alloc_fallback_buffer_locked() != 0) {
            s_voice.state = VOICE_IDLE;
            pthread_mutex_unlock(&s_voice.lock);
            sem_post(&s_voice.rec_ready);
            free(chunk);
            return NULL;
        }
        pthread_mutex_unlock(&s_voice.lock);
#endif
    } else {
        syslog(LOG_INFO, "[%s] using pre-connected ASR stream\n", TAG);
    }

    /* Signal voice_channel_start() that we are ready to consume
     * audio frames. Without this, sem_wait in start() deadlocks. */
    sem_post(&s_voice.rec_ready);

    /* No flush needed: the new start sequence guarantees that
     * audio_capture_start() runs AFTER this thread is ready,
     * so there is no stale data in the queue. The old flush
     * was harmful — audio_capture_read() blocks, so it would
     * consume real audio frames instead of stale ones. */

    while (1) {
        pthread_mutex_lock(&s_voice.lock);
        enum voice_state st = s_voice.state;
        pthread_mutex_unlock(&s_voice.lock);

        if (st != VOICE_RECORDING) {
            break;
        }

#ifdef PTT_RECORD_ONLY_PATH
        if (s_dump_fd >= 0) {
            /* The sink writes the buffers from the device thread, so
             * there is nothing to pull here - just wait for the state to
             * change so the session can be closed. */
            usleep(20000);
            continue;
        }
#endif

        int n = audio_capture_read(s_voice.cap, chunk, CHUNK_SIZE);

        if (n <= 0) {
            syslog(LOG_WARNING,
                "[%s] capture read returned %d, stopping\n", TAG, n);
            abnormal_exit = 1;
            break;
        }

        total_bytes_read += (size_t)n;
        chunk_count++;

        /* Log audio level for first few chunks and periodically
         * to diagnose silence vs actual speech from QEMU mic.  The clip
         * count is what tells gain problems apart from dropouts: a hot
         * preamp squares the waveform off and that is what a "sparking"
         * crackle sounds like. */
        if (1) { /* every chunk: a take is only ~18 of them */
            int16_t* samples = (int16_t*)chunk;
            int sample_count = n / 2;
            int32_t peak = 0;
            int32_t clipped = 0;

            for (int i = 0; i < sample_count; i++) {
                int32_t v = samples[i] < 0 ? -samples[i] : samples[i];
                if (v > peak) {
                    peak = v;
                }
                if (v >= 30000) {
                    clipped++;
                }
            }
            syslog(LOG_INFO,
                "[%s] chunk#%d: %d bytes, peak=%ld clipped=%ld/%d%s\n",
                TAG, chunk_count, n, (long)peak, (long)clipped,
                sample_count, clipped ? "  <-- CLIPPING" : "");
        }

#ifdef PTT_RECORD_ONLY_PATH
        /* Raw capture to file, no ASR. */

        if (s_dump_fd >= 0) {
            struct timespec tw0;
            struct timespec tw1;
            long usec;

            clock_gettime(CLOCK_MONOTONIC, &tw0);

            if (write(s_dump_fd, chunk, (size_t)n) != n) {
                syslog(LOG_ERR, "[%s] dump write failed: %d\n",
                    TAG, errno);
            }

            clock_gettime(CLOCK_MONOTONIC, &tw1);
            usec = (tw1.tv_sec - tw0.tv_sec) * 1000000
                + (tw1.tv_nsec - tw0.tv_nsec) / 1000;
            if (usec > max_write_us) {
                max_write_us = usec;
            }
            continue;
        }
#endif

#ifdef CONFIG_AI_AGENT_AUDIO_PREPROCESS
        preproc_chunk(pp, chunk, n);
#endif

        process_audio_chunk(&stream, chunk, (size_t)n,
            &need_fallback, &total_bytes_sent);
    }

#ifdef CONFIG_AI_AGENT_AUDIO_PREPROCESS
    preproc_destroy(pp);
#endif

    /* Store stream handle for voice_channel_stop to finish */
    // if (dump_fd >= 0) {
    //     close(dump_fd);
    //     syslog(LOG_INFO, "[%s] PCM dump saved to /data/cap_dump.pcm\n", TAG);
    // }

    pthread_mutex_lock(&s_voice.lock);
    s_voice.asr_stream = stream;

    if (abnormal_exit && s_voice.state == VOICE_RECORDING) {
        syslog(LOG_WARNING, "[%s] abnormal exit, cleaning up\n", TAG);
        recording_cleanup_locked(stream);
    }
    pthread_mutex_unlock(&s_voice.lock);

    syslog(LOG_INFO,
        "[%s] recording thread exit: %d chunks, %zu read, %zu sent%s\n",
        TAG, chunk_count, total_bytes_read, total_bytes_sent,
        abnormal_exit ? " (abnormal)" : "");

#ifdef PTT_RECORD_ONLY_PATH
    {
        struct timespec t1;
        long wall_ms;
        long audio_ms;

        clock_gettime(CLOCK_MONOTONIC, &t1);
        wall_ms = (t1.tv_sec - t_start.tv_sec) * 1000
            + (t1.tv_nsec - t_start.tv_nsec) / 1000000;

        /* 48kHz stereo 16 bit is 192000 bytes of audio per second. */
        audio_ms = (long)(total_bytes_read * 1000 / 192000);

        total_bytes_read = audio_capture_sink_bytes(s_voice.cap);
        syslog(LOG_INFO,
            "[%s] capture: wall=%ldms audio=%ldms sink=%zu bytes "
            "worst_write=%ldus %s\n",
            TAG, wall_ms, audio_ms, total_bytes_read, max_write_us,
            audio_ms + 200 < wall_ms ? "STARVED" : "ok");
    }
#endif

    free(chunk);
    return NULL;
}

/* ── ASR worker thread ──────────────────────────────────── */

#define ASR_THREAD_STACK (24 * 1024)

static void* asr_and_dispatch(void* arg)
{
    unsigned char* pcm = (unsigned char*)arg;
    size_t pcm_len;

    /* pcm_len is stored in the first sizeof(size_t) bytes */
    memcpy(&pcm_len, pcm, sizeof(size_t));
    const unsigned char* audio = pcm + sizeof(size_t);

    syslog(LOG_INFO, "[%s] ASR: recognizing %zu bytes\n",
        TAG, pcm_len);

    char text[512];
    int ret = voice_asr_recognize(audio, pcm_len,
        text, sizeof(text));

    if (ret == 0 && text[0] != '\0') {
        syslog(LOG_INFO, "[%s] ASR result: %s\n", TAG, text);
        printf("ASR: %s\n", text);

        /* Recognized text goes to the console only; the panel shows the
         * state and then the agent's reply. */

        ls_lvgl_set_ai_text("thinking...", NULL);

        agent_msg_t msg;

        memset(&msg, 0, sizeof(msg));
        strncpy(msg.channel, AGENT_CHAN_VOICE,
            sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, "voice",
            sizeof(msg.chat_id) - 1);
        msg.content = strdup(text);
        if (msg.content) {
            message_bus_push_inbound(&msg);
        }
    } else {
        syslog(LOG_WARNING, "[%s] ASR failed or empty: %d\n",
            TAG, ret);
    }

    free(pcm);
    return NULL;
}

/* ── TTS streaming callback ──────────────────────────────── */

static struct timespec s_tts_start;
static int s_tts_first_chunk;
static struct timespec s_asr_done_ts;

static void tts_stream_cb(const unsigned char* pcm_data,
    size_t pcm_len, int is_last, void* user_data)
{
    audio_playback_t* pb = (audio_playback_t*)user_data;

    /* Abort early if PTT recording started */
    if (s_voice.tts_abort) {
        return;
    }

    if (pcm_data && pcm_len > 0 && pb) {
        if (!s_tts_first_chunk) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long ms = (now.tv_sec - s_tts_start.tv_sec) * 1000
                + (now.tv_nsec - s_tts_start.tv_nsec) / 1000000;
            syslog(LOG_INFO, "[%s] TTS first chunk: %ldms\n",
                TAG, ms);
            s_tts_first_chunk = 1;
        }
        audio_playback_write(pb, pcm_data, pcm_len);
    }

    (void)is_last;
}

/* ── Public API ──────────────────────────────────────────── */

int voice_channel_init(void)
{
    syslog(LOG_INFO, "[%s] Voice channel initializing\n", TAG);

    /* Reset state on init — static vars may persist across process restarts
     * in NuttX depending on how the binary is loaded. */
    s_voice.state = VOICE_IDLE;
    s_voice.cap = NULL;
    s_voice.pcm_buf = NULL;
    s_voice.pcm_len = 0;
    s_voice.asr_stream = NULL;
    s_voice.tts_abort = 0;
    s_voice.tts_pb = NULL;
    sem_init(&s_voice.rec_ready, 0, 0);

    if (!s_backends_registered) {
        volc_tts_register();
        volc_asr_register();
        s_backends_registered = 1;
    }

    return 0;
}

int voice_channel_start(void)
{
    pthread_mutex_lock(&s_voice.lock);
    if (s_voice.state != VOICE_IDLE) {
        pthread_mutex_unlock(&s_voice.lock);
        syslog(LOG_WARNING, "[%s] already recording\n", TAG);
        return -EBUSY;
    }

    /* Drain stale sem posts from previous sessions to prevent
     * sem_wait returning immediately before thread is ready. */
    while (sem_trywait(&s_voice.rec_ready) == 0)
        ;

    /* ── AEC workaround: stop active TTS to prevent echo ── */
    if (s_voice.tts_pb) {
        syslog(LOG_INFO,
            "[%s] PTT: stopping active TTS to prevent echo\n", TAG);
        s_voice.tts_abort = 1;
        audio_playback_stop(s_voice.tts_pb);

        /* Release lock so the speak thread can finish closing the
         * playback device.  Without this, we hold the lock through
         * the entire ASR pre-connect (300-800ms), blocking the speak
         * thread from calling audio_playback_close().  The playback
         * device stays open while we open the capture device, and on
         * BES hardware the shared codec (ci2s) gets confused, causing
         * incomplete or corrupted recording. */
        pthread_mutex_unlock(&s_voice.lock);

        /* Wait for the speak thread to finish — speak_lock is held
         * for the entire duration of voice_channel_speak(). */
        pthread_mutex_lock(&s_voice.speak_lock);
        pthread_mutex_unlock(&s_voice.speak_lock);

        syslog(LOG_INFO,
            "[%s] PTT: TTS playback fully closed\n", TAG);

        /* Re-acquire state lock and re-check — another thread may
         * have started recording while we released the lock. */
        pthread_mutex_lock(&s_voice.lock);
        if (s_voice.state != VOICE_IDLE) {
            pthread_mutex_unlock(&s_voice.lock);
            syslog(LOG_WARNING, "[%s] state changed during TTS stop\n", TAG);
            return -EBUSY;
        }
    }

    /* PCM buffer is lazy-allocated only when streaming ASR fails
     * (batch fallback). This saves 128KB RAM on the happy path. */
    s_voice.pcm_cap = AGENT_VOICE_PCM_BUF_SIZE;
    s_voice.pcm_buf = NULL;
    s_voice.pcm_len = 0;
    s_voice.asr_stream = NULL;

    /* Pre-open streaming ASR session HERE (before spawning the
     * recording thread) so that TLS handshake (~300-800ms) completes
     * before audio capture starts.  Without this, the handshake runs
     * inside recording_thread while audio is already flowing, causing
     * the first 300-800ms of speech to be lost.
     *
     * If the pre-open fails we fall back to batch mode as before. */
#ifdef PTT_RECORD_ONLY_PATH
    /* Diagnostic mode: the audio goes to a file, so no ASR session is
     * needed and the network is not touched at all. */
    voice_asr_stream_t* pre_stream = NULL;

    /* O_RDWR: wav_analyze() reads the file back once it is written, and
     * wav_finish() seeks around in it to patch the RIFF sizes. */
    s_dump_fd = open(PTT_RECORD_ONLY_PATH,
        O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (s_dump_fd < 0) {
        syslog(LOG_ERR, "[%s] cannot create %s: %d\n",
            TAG, PTT_RECORD_ONLY_PATH, errno);
    } else {
        if (wav_write_header(s_dump_fd, PTT_CAPTURE_RATE,
                PTT_CAPTURE_CHANNELS, AGENT_VOICE_BITS) != 0) {
            syslog(LOG_ERR, "[%s] WAV header write failed\n", TAG);
            close(s_dump_fd);
            s_dump_fd = -1;
        } else {
            syslog(LOG_INFO,
                "[%s] recording %dHz %dch WAV to %s (ASR disabled)\n",
                TAG, PTT_CAPTURE_RATE, PTT_CAPTURE_CHANNELS,
                PTT_RECORD_ONLY_PATH);
        }
    }
#else
    voice_asr_stream_t* pre_stream = voice_asr_stream_open();
    if (!pre_stream) {
        syslog(LOG_WARNING,
            "[%s] pre-open ASR failed, will retry in thread\n", TAG);
    } else {
        syslog(LOG_INFO, "[%s] ASR pre-connected\n", TAG);
        s_voice.asr_stream = pre_stream;
    }
#endif

    /* Open capture device (but do NOT start yet — start after the
     * recording thread is spawned so the consumer is ready before
     * audio frames begin flowing, preventing media_recorder queue
     * overflow "data queue is more than max count(4)"). */
    s_voice.cap = audio_capture_open(
        AGENT_AUDIO_CAPTURE_DEV,
        PTT_CAPTURE_RATE, PTT_CAPTURE_CHANNELS, AGENT_VOICE_BITS);

    if (!s_voice.cap) {
        if (pre_stream) {
            voice_asr_stream_abort(pre_stream);
            s_voice.asr_stream = NULL;
        }
        pthread_mutex_unlock(&s_voice.lock);
        syslog(LOG_ERR, "[%s] capture open failed\n", TAG);
        return -EIO;
    }

#ifdef PTT_RECORD_ONLY_PATH
    /* Record straight to the file from the device thread, the way
     * nxrecorder's record thread does it. */
    if (s_dump_fd >= 0) {
        audio_capture_set_sink(s_voice.cap, s_dump_fd);
    }
#endif

    s_voice.state = VOICE_RECORDING;
    pthread_mutex_unlock(&s_voice.lock);

    /* Spawn recording thread BEFORE starting capture.
     *
     * The priority matters: without it the thread inherits from the
     * push-to-talk worker, which in turn inherited from the panel, so it
     * ends up at the same priority as LVGL's own render thread and gets
     * time-sliced against it.  LVGL drawing plus the SSD1306 mirror over
     * I2C is enough to stall the reader long enough for the audio buffers
     * to run dry, which is the stuttering in the recording.  FIFO above
     * the UI so the reader always runs as soon as a buffer is ready. */
    pthread_attr_t attr;
    struct sched_param sparam;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, AGENT_VOICE_STACK);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    sparam.sched_priority = AGENT_VOICE_CAPTURE_PRIO;
    pthread_attr_setschedparam(&attr, &sparam);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

    int ret = pthread_create(&s_voice.rec_thread, &attr,
        recording_thread, NULL);

    pthread_attr_destroy(&attr);

    if (ret != 0) {
        pthread_mutex_lock(&s_voice.lock);
        s_voice.state = VOICE_IDLE;
        audio_capture_close(s_voice.cap);
        s_voice.cap = NULL;
        if (s_voice.asr_stream) {
            voice_asr_stream_abort(s_voice.asr_stream);
            s_voice.asr_stream = NULL;
        }
        free(s_voice.pcm_buf);
        s_voice.pcm_buf = NULL;
        pthread_mutex_unlock(&s_voice.lock);
        return -ret;
    }

    /* Now start capture — wait for recording thread to be ready first */
    sem_wait(&s_voice.rec_ready);

    if (audio_capture_start(s_voice.cap) < 0) {
        pthread_mutex_lock(&s_voice.lock);
        s_voice.state = VOICE_IDLE;
        if (s_voice.asr_stream) {
            voice_asr_stream_abort(s_voice.asr_stream);
            s_voice.asr_stream = NULL;
        }
        pthread_mutex_unlock(&s_voice.lock);
        pthread_join(s_voice.rec_thread, NULL);
        audio_capture_close(s_voice.cap);
        s_voice.cap = NULL;
        return -EIO;
    }

    syslog(LOG_INFO, "[%s] PTT recording started\n", TAG);
    printf("Voice: recording... (use voice_stop to finish)\n");
    return 0;
}

int voice_channel_stop(void)
{
    pthread_mutex_lock(&s_voice.lock);
    if (s_voice.state != VOICE_RECORDING) {
        pthread_mutex_unlock(&s_voice.lock);
        return -EINVAL;
    }
    s_voice.state = VOICE_STOPPING;
    pthread_mutex_unlock(&s_voice.lock);

    /* Wait for recording thread to finish */
    pthread_join(s_voice.rec_thread, NULL);

    /* Close capture device */
    audio_capture_close(s_voice.cap);

    pthread_mutex_lock(&s_voice.lock);
    s_voice.cap = NULL;
    size_t pcm_len = s_voice.pcm_len;
    voice_asr_stream_t* stream = s_voice.asr_stream;
    s_voice.asr_stream = NULL;
    s_voice.state = VOICE_IDLE;
    pthread_mutex_unlock(&s_voice.lock);

    syslog(LOG_INFO, "[%s] recorded %zu bytes (stream=%p)\n",
        TAG, pcm_len, (void*)stream);

#ifdef PTT_RECORD_ONLY_PATH
    if (s_dump_fd >= 0) {
        off_t written;

        wav_finish(s_dump_fd);
        wav_analyze(s_dump_fd, PTT_CAPTURE_RATE, PTT_CAPTURE_CHANNELS);
        written = lseek(s_dump_fd, 0, SEEK_END);
        close(s_dump_fd);
        s_dump_fd = -1;
        printf("Recorded %ld bytes to %s\n",
            (long)written, PTT_RECORD_ONLY_PATH);
        return 0;
    }
#endif

    /* If streaming ASR was active, finish it to get result.
     * Note: in streaming mode pcm_len is always 0 because audio
     * goes directly to the ASR server, not to pcm_buf. */
    if (stream) {
        printf("Voice: streaming ASR active, finishing...\n");
        struct timespec asr_t0;
        clock_gettime(CLOCK_MONOTONIC, &asr_t0);
        char text[512];
        int ret = voice_asr_stream_finish(stream, text,
            sizeof(text));
        struct timespec asr_t1;
        clock_gettime(CLOCK_MONOTONIC, &asr_t1);
        long asr_ms = (asr_t1.tv_sec - asr_t0.tv_sec) * 1000
            + (asr_t1.tv_nsec - asr_t0.tv_nsec) / 1000000;
        syslog(LOG_INFO, "[%s] ASR finish: %ldms\n", TAG, asr_ms);

        free(s_voice.pcm_buf); /* may be NULL */
        s_voice.pcm_buf = NULL;

        if (ret == 0 && text[0] != '\0') {
            syslog(LOG_INFO, "[%s] stream ASR: %s\n", TAG, text);
            printf("ASR: %s\n", text);
            clock_gettime(CLOCK_MONOTONIC, &s_asr_done_ts);

            agent_msg_t msg;

            memset(&msg, 0, sizeof(msg));
            strncpy(msg.channel, AGENT_CHAN_VOICE,
                sizeof(msg.channel) - 1);
            strncpy(msg.chat_id, "voice",
                sizeof(msg.chat_id) - 1);
            msg.content = strdup(text);
            if (msg.content) {
                message_bus_push_inbound(&msg);
            }
        } else {
            syslog(LOG_WARNING,
                "[%s] stream ASR failed: %d\n", TAG, ret);
        }
        return 0;
    }

    /* Batch fallback: pcm_buf must exist if we reach here */
    if (!s_voice.pcm_buf || pcm_len == 0) {
        free(s_voice.pcm_buf);
        s_voice.pcm_buf = NULL;
        syslog(LOG_ERR, "[%s] no PCM data for batch ASR\n", TAG);
        return -ENODATA;
    }

    /* Batch fallback: pack pcm_len + audio for ASR thread */
    size_t total = sizeof(size_t) + pcm_len;
    unsigned char* pkg = malloc(total);

    if (!pkg) {
        free(s_voice.pcm_buf);
        s_voice.pcm_buf = NULL;
        return -ENOMEM;
    }

    memcpy(pkg, &pcm_len, sizeof(size_t));
    memcpy(pkg + sizeof(size_t), s_voice.pcm_buf, pcm_len);
    free(s_voice.pcm_buf);
    s_voice.pcm_buf = NULL;

    /* Spawn ASR worker thread */
    int ret = agent_task_create(asr_and_dispatch, "asr_worker",
        ASR_THREAD_STACK, pkg, AGENT_VOICE_PRIO);

    if (ret != OK) {
        free(pkg);
        return -EIO;
    }

    return 0;
}

int voice_channel_stop_with_text(char* text_out, size_t text_cap)
{
    if (!text_out || text_cap == 0) {
        return -EINVAL;
    }

    text_out[0] = '\0';

    pthread_mutex_lock(&s_voice.lock);
    if (s_voice.state != VOICE_RECORDING) {
        pthread_mutex_unlock(&s_voice.lock);
        return -EINVAL;
    }
    s_voice.state = VOICE_STOPPING;
    pthread_mutex_unlock(&s_voice.lock);

    /* Wait for recording thread to finish */
    pthread_join(s_voice.rec_thread, NULL);

    /* Close capture device */
    audio_capture_close(s_voice.cap);

    pthread_mutex_lock(&s_voice.lock);
    s_voice.cap = NULL;
    size_t pcm_len = s_voice.pcm_len;
    voice_asr_stream_t* stream = s_voice.asr_stream;
    s_voice.asr_stream = NULL;
    s_voice.state = VOICE_IDLE;
    pthread_mutex_unlock(&s_voice.lock);

    syslog(LOG_INFO, "[%s] recorded %zu bytes (with_text, stream=%p)\n",
        TAG, pcm_len, (void*)stream);

#ifdef PTT_RECORD_ONLY_PATH
    /* Diagnostic mode: the capture went to a file, so there is nothing to
     * recognise and no agent to tell. */
    if (s_dump_fd >= 0) {
        off_t written;

        wav_finish(s_dump_fd);
        wav_analyze(s_dump_fd, PTT_CAPTURE_RATE, PTT_CAPTURE_CHANNELS);
        written = lseek(s_dump_fd, 0, SEEK_END);
        close(s_dump_fd);
        s_dump_fd = -1;
        printf("Recorded %ld bytes to %s\n",
            (long)written, PTT_RECORD_ONLY_PATH);
        syslog(LOG_INFO, "[%s] capture saved to %s (%ld bytes)\n",
            TAG, PTT_RECORD_ONLY_PATH, (long)written);
        snprintf(text_out, text_cap, "(recording saved to %s)",
            PTT_RECORD_ONLY_PATH);
        return 0;
    }
#endif

    /* Streaming ASR path: finish and copy text to caller.
     * In streaming mode audio goes directly to server, so
     * pcm_len may be 0 — that's OK, don't abort. */
    if (stream) {
        char text[512];
        int ret = voice_asr_stream_finish(stream, text,
            sizeof(text));

        free(s_voice.pcm_buf);
        s_voice.pcm_buf = NULL;

        if (ret == 0 && text[0] != '\0') {
            syslog(LOG_INFO, "[%s] stream ASR (with_text): %s\n",
                TAG, text);
            strncpy(text_out, text, text_cap - 1);
            text_out[text_cap - 1] = '\0';
        }
        return 0;
    }

    /* Batch fallback: synchronous ASR inline (no thread) */
    if (!s_voice.pcm_buf || pcm_len == 0) {
        free(s_voice.pcm_buf);
        s_voice.pcm_buf = NULL;
        syslog(LOG_ERR, "[%s] no PCM data for batch ASR\n", TAG);
        return -ENODATA;
    }

    syslog(LOG_INFO, "[%s] batch ASR (with_text): %zu bytes\n",
        TAG, pcm_len);

    char text[512];
    int ret = voice_asr_recognize(s_voice.pcm_buf, pcm_len,
        text, sizeof(text));

    free(s_voice.pcm_buf);
    s_voice.pcm_buf = NULL;

    if (ret == 0 && text[0] != '\0') {
        syslog(LOG_INFO, "[%s] batch ASR result: %s\n", TAG, text);
        strncpy(text_out, text, text_cap - 1);
        text_out[text_cap - 1] = '\0';
    } else if (ret != 0) {
        syslog(LOG_WARNING, "[%s] batch ASR failed: %d\n",
            TAG, ret);
    }

    return 0;
}

/* Strip Markdown formatting that TTS would read aloud.
 * Removes: **bold**, *italic*, # headings, - list bullets.
 * Operates in-place, result is always <= original length. */
static void tts_strip_markdown(char* s)
{
    char* r = s; /* read pointer */
    char* w = s; /* write pointer */

    while (*r) {
        /* Skip ** (bold markers) */
        if (r[0] == '*' && r[1] == '*') {
            r += 2;
            continue;
        }

        /* Skip lone * (italic markers) \xe2\x80\x94 but keep * in
         * contexts like "3*4" (digit before and after) */
        if (r[0] == '*') {
            if ((r == s || !isdigit((unsigned char)r[-1]))
                || !isdigit((unsigned char)r[1])) {
                r++;
                continue;
            }
        }

        /* Skip # at line start (headings) */
        if (r[0] == '#' && (r == s || r[-1] == '\n')) {
            while (*r == '#' || *r == ' ') {
                r++;
            }
            continue;
        }

        /* Skip "- " at line start (list bullets) */
        if (r[0] == '-' && r[1] == ' '
            && (r == s || r[-1] == '\n')) {
            r += 2;
            continue;
        }

        *w++ = *r++;
    }

    *w = '\0';
}

int voice_channel_speak(const char* text)
{
    if (!text || text[0] == '\0') {
        return -EINVAL;
    }

    /* ── Serialize concurrent speak calls ── */
    /* If another thread is already speaking, abort its TTS playback
     * so we don't overlap media_player sessions (which causes the
     * media framework to attempt a ~1.3GB allocation and crash). */
    pthread_mutex_lock(&s_voice.lock);
    if (s_voice.tts_pb) {
        syslog(LOG_INFO, "[%s] speak: aborting previous TTS\n", TAG);
        s_voice.tts_abort = 1;
        audio_playback_stop(s_voice.tts_pb);
    }
    pthread_mutex_unlock(&s_voice.lock);

    pthread_mutex_lock(&s_voice.speak_lock);

    /* ── AEC workaround: reject TTS while PTT is recording ── */
    pthread_mutex_lock(&s_voice.lock);
    if (s_voice.state != VOICE_IDLE) {
        pthread_mutex_unlock(&s_voice.lock);
        pthread_mutex_unlock(&s_voice.speak_lock);
        syslog(LOG_INFO,
            "[%s] speak: skipped, recording active\n", TAG);
        return -EBUSY;
    }
    pthread_mutex_unlock(&s_voice.lock);

    /* Work on a mutable copy so we can strip markdown in-place */
    size_t text_len = strlen(text);
    char* clean = malloc(text_len + 1);

    if (!clean) {
        pthread_mutex_unlock(&s_voice.speak_lock);
        return -ENOMEM;
    }

    memcpy(clean, text, text_len + 1);
    tts_strip_markdown(clean);

    /* If stripping left nothing, bail out */
    if (clean[0] == '\0') {
        free(clean);
        pthread_mutex_unlock(&s_voice.speak_lock);
        return 0;
    }

    syslog(LOG_INFO, "[%s] speak: \"%.*s\" (%zu bytes)\n",
        TAG, 60, clean, strlen(clean));

    clock_gettime(CLOCK_MONOTONIC, &s_tts_start);
    s_tts_first_chunk = 0;

    if (s_asr_done_ts.tv_sec > 0) {
        long llm_ms = (s_tts_start.tv_sec - s_asr_done_ts.tv_sec) * 1000
            + (s_tts_start.tv_nsec - s_asr_done_ts.tv_nsec) / 1000000;
        syslog(LOG_INFO, "[%s] LLM latency: %ldms\n", TAG, llm_ms);
    }

    /* Try streaming TTS first (callback per chunk) */
    audio_playback_t* pb = audio_playback_open(
        AGENT_AUDIO_PLAYBACK_DEV,
        AGENT_TTS_WS_SAMPLE_RATE,
        AGENT_VOICE_CHANNELS,
        AGENT_VOICE_BITS);

    if (!pb) {
        syslog(LOG_ERR, "[%s] playback open failed\n", TAG);
        free(clean);
        pthread_mutex_unlock(&s_voice.speak_lock);
        return -EIO;
    }

    /* Register active playback so voice_channel_start() can abort it */
    pthread_mutex_lock(&s_voice.lock);
    s_voice.tts_abort = 0;
    s_voice.tts_pb = pb;
    pthread_mutex_unlock(&s_voice.lock);

    /* Use streaming TTS: each chunk writes to playback file */
    int ret = voice_tts_speak_stream(clean,
        tts_stream_cb, pb);

    struct timespec tts_net;
    clock_gettime(CLOCK_MONOTONIC, &tts_net);
    long net_ms = (tts_net.tv_sec - s_tts_start.tv_sec) * 1000
        + (tts_net.tv_nsec - s_tts_start.tv_nsec) / 1000000;
    syslog(LOG_INFO, "[%s] TTS network done: %ldms\n", TAG, net_ms);

    /* Unregister active playback */
    pthread_mutex_lock(&s_voice.lock);
    s_voice.tts_pb = NULL;
    s_voice.tts_abort = 0;
    pthread_mutex_unlock(&s_voice.lock);

    audio_playback_close(pb);
    free(clean);

    if (ret != 0) {
        syslog(LOG_ERR, "[%s] TTS stream failed: %d\n", TAG, ret);
        pthread_mutex_unlock(&s_voice.speak_lock);
        return ret;
    }

    struct timespec tend;
    clock_gettime(CLOCK_MONOTONIC, &tend);
    long total_ms = (tend.tv_sec - s_tts_start.tv_sec) * 1000
        + (tend.tv_nsec - s_tts_start.tv_nsec) / 1000000;
    syslog(LOG_INFO, "[%s] speak done: total %ldms (play wait %ldms)\n",
        TAG, total_ms, total_ms - net_ms);
    pthread_mutex_unlock(&s_voice.speak_lock);
    return 0;
}

/* ── Push-to-talk, driven by the panel's KEY1 ─────────────── */

/* The panel reports key edges from its own UI tick, so everything that
 * blocks has to happen elsewhere: voice_channel_start() spends 300-800ms
 * on the TLS pre-connect, and stop_with_text() waits out the ASR.  One
 * worker drains a FIFO of edges so a quick release cannot overtake a
 * slow start and end up stopping a session that never opened. */

#define PTT_EVENT_QUEUE 4

static pthread_mutex_t s_ptt_lock = PTHREAD_MUTEX_INITIALIZER;
static sem_t s_ptt_sem;
static unsigned char s_ptt_events[PTT_EVENT_QUEUE];
static int s_ptt_head;
static int s_ptt_tail;
static pthread_t s_ptt_thread;
static bool s_ptt_running;

static void ptt_post(unsigned char ev)
{
    int next;

    pthread_mutex_lock(&s_ptt_lock);
    next = (s_ptt_head + 1) % PTT_EVENT_QUEUE;

    if (next != s_ptt_tail) {
        s_ptt_events[s_ptt_head] = ev;
        s_ptt_head = next;
        sem_post(&s_ptt_sem);
    } else {
        syslog(LOG_WARNING, "[%s] PTT queue full, dropping edge\n", TAG);
    }

    pthread_mutex_unlock(&s_ptt_lock);
}

/* The push-to-talk path records through nxrecorder - the same code the
 * "record" command uses.  It is the only capture path on this board that
 * is known to produce clean audio, and the one the audio_capture.c driver
 * could not be made to match byte for byte. */

/* The capture goes to the RAM-backed /tmp (mounted by the board bringup),
 * not to the SD card.  The file only has to survive until ASR has read
 * it a few seconds later, and keeping the SDIO DMA off the bus while the
 * I2S DMA runs removes one source of the short capture dropouts. */

#define PTT_CAPTURE_PATH "/tmp/ptt.wav"

static FAR struct nxrecorder_s *s_recorder;

static void ptt_handle_down(void)
{
    int ret;

    /* Stop any TTS playback BEFORE opening the capture device.  The
     * playback and capture nodes share one ES8388, and closing the
     * playback node resets every codec register, which wipes the ADC
     * configuration in the middle of a recording.  Recording while the
     * reply is still audible also captures the reply itself.  This is
     * the same workaround voice_channel_start() does; the PTT path grew
     * up separately and never picked it up. */

    pthread_mutex_lock(&s_voice.lock);
    if (s_voice.tts_pb) {
        syslog(LOG_INFO,
            "[%s] PTT: stopping active TTS to prevent echo\n", TAG);
        s_voice.tts_abort = 1;
        audio_playback_stop(s_voice.tts_pb);
        pthread_mutex_unlock(&s_voice.lock);

        /* Wait for the speak thread to finish closing the playback
         * device: speak_lock is held for the whole speak call. */

        pthread_mutex_lock(&s_voice.speak_lock);
        pthread_mutex_unlock(&s_voice.speak_lock);

        syslog(LOG_INFO, "[%s] PTT: TTS playback fully closed\n", TAG);
    } else {
        pthread_mutex_unlock(&s_voice.lock);
    }

    ls_lvgl_set_ai_text("listening...", "speak now");

    /* Silence the blue LED's PWM while the microphone is open.  Its
     * 1 kHz carrier couples into the 24 dB mic preamp and saturates the
     * capture: every take recorded while the LED was lit came back as
     * clipped noise and the recognizer answered "no text".  The panel
     * skips its own ramp while the key is held, so this zero duty holds
     * for the whole take; when the key is released the panel resumes
     * the animation on its own. */

    {
        int pwm_fd = open("/dev/pwm2", O_RDONLY);

        if (pwm_fd >= 0)
          {
            struct pwm_info_s pwm;

            memset(&pwm, 0, sizeof(pwm));
            pwm.frequency = 1000;
            pwm.duty = 0;

            /* START, not SETCHARACTERISTICS: the latter only reaches the
             * hardware when this file already owns a started session, and
             * the whole point here is to force the output low. */

            ioctl(pwm_fd, PWMIOC_START, (unsigned long)(uintptr_t)&pwm);
            close(pwm_fd);
          }
    }

    if (s_recorder != NULL) {
        nxrecorder_release(s_recorder);
        s_recorder = NULL;
    }

    s_recorder = nxrecorder_create();
    if (s_recorder == NULL) {
        ls_lvgl_set_ai_text("idle", "recorder unavailable");
        return;
    }

    nxrecorder_setdevice(s_recorder, AGENT_AUDIO_CAPTURE_DEV);

    ret = nxrecorder_recordinternal(s_recorder, PTT_CAPTURE_PATH,
                                    AUDIO_FMT_UNDEF, 2, 16, 48000, 0);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] PTT record failed: %d\n", TAG, ret);
        ls_lvgl_set_ai_text("idle", "cannot open microphone");
        nxrecorder_release(s_recorder);
        s_recorder = NULL;
    }
}

static void ptt_handle_up(void)
{
    ls_lvgl_set_ai_text("recognizing...", NULL);

    if (s_recorder != NULL) {
        /* Stops the record thread, which closes the file. */
        nxrecorder_stop(s_recorder);
        nxrecorder_release(s_recorder);
        s_recorder = NULL;
    }

    /* Give the recorder a moment to finish flushing before the recognizer
     * opens the file. */

    usleep(500000);

    /* Feed the recording to the recognizer.  The file is raw PCM, so the
     * format is given explicitly. */
    voice_channel_test_asr(PTT_CAPTURE_PATH, 2, 48000);
}

static void* ptt_worker(void* arg)
{
    (void)arg;

    for (;;) {
        unsigned char ev;

        while (sem_wait(&s_ptt_sem) != 0) {
            if (errno != EINTR) {
                return NULL;
            }
        }

        pthread_mutex_lock(&s_ptt_lock);
        if (s_ptt_tail == s_ptt_head) {
            pthread_mutex_unlock(&s_ptt_lock);
            continue;
        }

        ev = s_ptt_events[s_ptt_tail];
        s_ptt_tail = (s_ptt_tail + 1) % PTT_EVENT_QUEUE;
        pthread_mutex_unlock(&s_ptt_lock);

        if (ev) {
            ptt_handle_down();
        } else {
            ptt_handle_up();
        }
    }

    return NULL;
}

void ai_agent_voice_ptt(bool down)
{
    pthread_attr_t attr;

    if (!s_ptt_running) {
        pthread_mutex_lock(&s_ptt_lock);

        if (!s_ptt_running) {
            sem_init(&s_ptt_sem, 0, 0);
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, ASR_THREAD_STACK);

            if (pthread_create(&s_ptt_thread, &attr, ptt_worker, NULL) == 0) {
                s_ptt_running = true;
            } else {
                syslog(LOG_ERR, "[%s] PTT worker start failed\n", TAG);
            }

            pthread_attr_destroy(&attr);
        }

        pthread_mutex_unlock(&s_ptt_lock);
    }

    ptt_post(down ? 1 : 0);
}

/* ── Test commands (file-based, for QEMU) ───────────────── */

int voice_channel_test_tts(const char* text, const char* out_path)
{
    if (!text || !out_path) {
        printf("Usage: voice_test_tts <text> [output_path]\n");
        return -EINVAL;
    }

    printf("TTS: synthesizing \"%s\" ...\n", text);

    unsigned char* pcm = malloc(AGENT_VOICE_PCM_BUF_SIZE);

    if (!pcm) {
        printf("Error: out of memory\n");
        return -ENOMEM;
    }

    size_t pcm_len;
    int ret = voice_tts_speak(text, pcm,
        AGENT_VOICE_PCM_BUF_SIZE, &pcm_len);

    if (ret != 0) {
        free(pcm);
        printf("TTS failed: %d\n", ret);
        return ret;
    }

    ret = write_pcm_file(out_path, pcm, pcm_len);
    free(pcm);

    if (ret == 0) {
        printf("TTS OK: %zu bytes -> %s\n", pcm_len, out_path);
    } else {
        printf("Failed to write %s: %d\n", out_path, ret);
    }

    return ret;
}

typedef struct {
    char pcm_path[256];
    unsigned int channels; /* channels in the file */
    unsigned int rate;     /* sample rate of the file */
} asr_task_args_t;

/* Report how much signal each channel actually carries.  On this board the
 * microphone reaches LIN only and RIN is tied to AGND, so one channel is
 * expected to be loud and the other near-silent - and the channel order in
 * a frame is not something to take on faith. */
static void report_channel_levels(const unsigned char* src, size_t len,
    unsigned int channels)
{
    size_t nframes = len / (2 * channels);
    unsigned int c;

    for (c = 0; c < channels && c < 4; c++) {
        long peak = 0;
        double sq = 0;
        size_t f;

        for (f = 0; f < nframes; f++) {
            const unsigned char* p = src + (f * channels + c) * 2;
            long s = (long)(int16_t)(p[0] | (p[1] << 8));

            if (s < 0) {
                s = -s;
            }
            if (s > peak) {
                peak = s;
            }
            sq += (double)s * (double)s;
        }

        printf("ASR:   ch%u peak=%ld rms=%ld\n",
            c, peak,
            nframes ? (long)sqrt(sq / (double)nframes) : 0L);
    }
}

/* The microphone reaches only one ADC channel, but which one can change
 * during a take: the codec is clocked as an I2S slave, and every DMA
 * restart can slip the left/right alignment by one 16-bit slot, so the
 * voice wanders between the channels while the take is being recorded.
 * The channel not driving the microphone sits at near-Nyquist noise with
 * a very high zero-crossing rate, while voice crosses far less often.
 * Pick the calmer channel for each short window so a take whose channels
 * alternate still comes through as continuous voice. */

#define ASR_CHANNEL_WINDOW_MS 30

static unsigned char* build_channel_map(const unsigned char* src,
    size_t nframes, unsigned int channels, size_t win, size_t* nwin_out)
{
    size_t nwin = (nframes + win - 1) / win;
    unsigned int nc = channels > 4 ? 4 : channels;
    unsigned char* sel;
    size_t w;

    if (nwin == 0) {
        nwin = 1;
    }

    *nwin_out = nwin;

    sel = malloc(nwin);
    if (!sel) {
        return NULL;
    }

    for (w = 0; w < nwin; w++) {
        size_t f0 = w * win;
        size_t f1 = f0 + win;
        size_t zc[4] = { 0, 0, 0, 0 };
        int prev[4] = { 0, 0, 0, 0 };
        unsigned int best = 0;
        unsigned int c;
        size_t f;

        if (f1 > nframes) {
            f1 = nframes;
        }

        for (f = f0; f < f1; f++) {
            for (c = 0; c < nc; c++) {
                const unsigned char* p = src + (f * channels + c) * 2;
                int s = (int16_t)(p[0] | (p[1] << 8));
                int sign = (s > 0) - (s < 0);

                if (sign && prev[c] && sign != prev[c]) {
                    zc[c]++;
                }
                if (sign) {
                    prev[c] = sign;
                }
            }
        }

        for (c = 1; c < nc; c++) {
            if (zc[c] < zc[best]) {
                best = c;
            }
        }

        sel[w] = (unsigned char)best;
    }

    return sel;
}

/* Fold a captured stream down to the 16kHz mono that volc_asr declares to
 * the server, following the per-window channel choice (see above).
 * Anything above 16kHz is box-filtered down, which is crude but is what
 * the recognizer needs to see. */
static size_t pcm_to_asr_format(const unsigned char* src, size_t src_len,
    unsigned int channels, unsigned int rate,
    unsigned char* dst, size_t dst_cap)
{
    size_t nframes;
    size_t out_n;
    size_t i;
    size_t win;
    size_t nwin = 0;
    unsigned char* sel = NULL;

    if (channels < 1) {
        channels = 1;
    }
    if (rate < 1) {
        rate = AGENT_VOICE_SAMPLE_RATE;
    }

    nframes = src_len / (2 * channels);
    out_n = (size_t)((uint64_t)nframes * AGENT_VOICE_SAMPLE_RATE / rate);

    if (out_n * 2 > dst_cap) {
        out_n = dst_cap / 2;
    }

    win = (size_t)rate * ASR_CHANNEL_WINDOW_MS / 1000;
    if (win < 1) {
        win = 1;
    }

    if (channels > 1 && nframes > 0) {
        sel = build_channel_map(src, nframes, channels, win, &nwin);
    }

    for (i = 0; i < out_n; i++) {
        size_t f0 = (size_t)((uint64_t)i * rate / AGENT_VOICE_SAMPLE_RATE);
        size_t f1 =
            (size_t)((uint64_t)(i + 1) * rate / AGENT_VOICE_SAMPLE_RATE);
        size_t start = f0;
        unsigned int c = 0;
        int32_t sum = 0;
        int16_t sample;

        if (f1 <= f0) {
            f1 = f0 + 1;
        }
        if (f1 > nframes) {
            f1 = nframes;
        }

        if (sel && win) {
            size_t w = start / win;

            c = (w < nwin) ? sel[w] : sel[nwin - 1];
        }

        for (; f0 < f1; f0++) {
            const unsigned char* p = src + (f0 * channels + c) * 2;

            sum += (int16_t)(p[0] | (p[1] << 8));
        }

        sample = (int16_t)(sum / (int32_t)(f1 - start > 0
                                               ? f1 - start
                                               : 1));
        dst[i * 2] = (unsigned char)(sample & 0xff);
        dst[i * 2 + 1] = (unsigned char)((sample >> 8) & 0xff);
    }

    free(sel);

    return out_n * 2;
}

static void* asr_file_worker(void* arg)
{
    asr_task_args_t* a = (asr_task_args_t*)arg;
    unsigned char* pcm = NULL;
    size_t pcm_len = 0;
    pcm_format_t fmt = { a->channels, a->rate, AGENT_VOICE_BITS };

    /* A WAV header in the file overrides whatever was passed on the
     * command line; otherwise the arguments do, and failing both, what
     * nxrecorder writes by default.
     */

    int ret = read_pcm_file(a->pcm_path, &fmt, &pcm, &pcm_len);

    if (ret != 0) {
        printf("Failed to read %s: %d\n", a->pcm_path, ret);
        free(a);
        return NULL;
    }

    printf("ASR: recognizing %zu bytes from %s (%uHz %uch %ubit) ...\n",
        pcm_len, a->pcm_path, fmt.rate, fmt.channels, fmt.bits);

    report_channel_levels(pcm, pcm_len, fmt.channels);

    /* volc_asr declares 16kHz mono to the server; anything else has to be
     * folded down first or the recognition comes back empty. */

    unsigned char* mono = malloc(pcm_len);

    if (!mono) {
        printf("Error: out of memory\n");
        free(pcm);
        free(a);
        return NULL;
    }

    size_t mono_len = pcm_to_asr_format(pcm, pcm_len,
        fmt.channels, fmt.rate, mono, pcm_len);

    free(pcm);

    if (fmt.channels != 1 || fmt.rate != AGENT_VOICE_SAMPLE_RATE) {
        printf("ASR: %uHz %uch -> 16kHz mono, %zu bytes\n",
            fmt.rate, fmt.channels, mono_len);
    }

    char text[512];

    ret = voice_asr_recognize(mono, mono_len, text, sizeof(text));
    free(mono);

    if (ret == 0) {
        printf("ASR result: %s\n", text);

        /* The recognized text stays on the console; the panel only shows
         * the state and then the agent's own reply. */

        ls_lvgl_set_ai_text("thinking...", NULL);

        agent_msg_t msg;

        memset(&msg, 0, sizeof(msg));
        strncpy(msg.channel, AGENT_CHAN_VOICE,
            sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, "voice",
            sizeof(msg.chat_id) - 1);
        msg.content = strdup(text);
        if (msg.content) {
            message_bus_push_inbound(&msg);
            printf("Sent to agent.\n");
        }
    } else {
        printf("ASR failed: %d\n", ret);
    }

    free(a);
    return NULL;
}

int voice_channel_test_asr(const char* pcm_path,
    unsigned int channels, unsigned int rate)
{
    if (!pcm_path) {
        printf("Usage: voice_test_asr <audio_file> [channels] [rate]\n");
        return -EINVAL;
    }

    asr_task_args_t* args = malloc(sizeof(asr_task_args_t));

    if (!args) {
        printf("Error: out of memory\n");
        return -ENOMEM;
    }

    strncpy(args->pcm_path, pcm_path, sizeof(args->pcm_path) - 1);
    args->pcm_path[sizeof(args->pcm_path) - 1] = '\0';

    /* 0 means "not stated": a WAV header in the file wins, then these,
     * then the nxrecorder defaults. */

    args->channels = channels;
    args->rate = rate;

    int ret = agent_task_create(asr_file_worker, "asr_test",
        ASR_THREAD_STACK, args, AGENT_VOICE_PRIO);

    if (ret != OK) {
        printf("Error: failed to create ASR thread\n");
        free(args);
        return -EIO;
    }

    printf("ASR test started (background thread).\n");
    return 0;
}
