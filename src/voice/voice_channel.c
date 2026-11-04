#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <nuttx/arch.h>
#include <nuttx/clock.h>   /* [V689] clock_systime_ticks() */
#include <nuttx/sched.h>
#include <nuttx/tls.h>

// [V76] one-shot listen-path beacons
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
#include "servo.h"
#include "voice/audio_capture.h"
#include "voice/audio_playback.h"
#include "voice/tts_offline_data.h"   /* [TTS-CACHE] 内置离线语音 */
#include "voice/voice_asr.h"
#include "voice/voice_tts.h"
#include "voice/volc_asr.h"
#include "voice/volc_tts.h"
#include "voice/http_tts.h"
#include "voice/http_asr.h"
#include "voice/mimo_tts.h"
#include "voice/mimo_asr.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <unistd.h>

/* [V1024] Probe gate -- see fortify_stubs.c. */
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

/* ── Always-on listening: local endpointing ──────────────────────────────
 *
 * Nothing in this build decides when the user stopped talking: there is no
 * wake-word engine, and the HTTP ASR endpoint is plain request/response.  The
 * decision is therefore made locally from the captured level.
 *
 * Peak (16-bit samples) against an open and a close threshold, with hysteresis,
 * so one click cannot open a session and a natural pause inside a sentence
 * cannot close one.
 */

/* [V65] Thresholds lowered from 700 / 500.
 *
 * 700 of 32767 is about -33 dBFS, which a MEMS microphone behind this codec
 * did not reach: the log was a wall of "no speech for 8000 ms" and not one
 * byte of audio ever reached the ASR endpoint.  The codec input volume is
 * already at maximum (CONFIG_ES8311_INPUT_INITVOLUME = 1000), so the ceiling
 * was on this side. */

#define VOICE_VAD_OPEN_PEAK     2000   /* level that counts as speech */
#define VOICE_VAD_CLOSE_PEAK    1200   /* below this we count silence */

/* [V733] *** Adaptive endpointing instead of two fixed numbers. ***
 *
 * Measured on v732 in a quiet room (_work\_v733_sweep.log, healthy boot,
 * per-second maxima so these are ceilings, not medians):
 *
 *     silence  peak  p10 4764  p50 6066  p90 7464
 *     silence  adev  p10 1078  p50 1406  p90 2274
 *
 * against the v732 gates of peak 4000 / adev 1200.  The room floor is
 * ABOVE the gate, so "speech detected" fires the moment listening opens,
 * VOICE_VAD_MAX_MS (15 s) fills with pure noise, and the ASR answers with
 * a hallucination ("二百四。").  That closed cycle is the reported
 * "stuck on 正在思考 with white noise in the background".
 *
 * There is no safe fixed gate: the floor moves with the MIC gain, with
 * the room, and with whether this boot's RX stream came up aligned.  Track
 * it instead -- floor is a slow lower envelope of adev (fast to fall, very
 * slow to rise, so one sentence does not move it) and the gate is a
 * multiple of it with an absolute minimum.
 *
 * /tmp/vadgate.txt still wins when it exists (g733_ovr), which is how the
 * numbers below were found without a reflash.
 */

#define VOICE_VAD_FLOOR_MIN     600    /* never call the room quieter */
#define VOICE_VAD_OPEN_MUL      2      /* [V862] was 3: speech is only
                                        * 2..5x the room, a 3x gate sat
                                        * mid-sentence and dropped most
                                        * of it. */
#define VOICE_VAD_CLOSE_MUL     1      /* [V862b] was 2: with OPEN_MUL
                                        * down to 2 the open and close
                                        * gates became the same number
                                        * and their clamps cross over,
                                        * so close is now floor x 1. */
#define VOICE_VAD_OPEN_MIN      3000   /* [V867] was 2400: a quiet
                                        * second still throws one
                                        * burst to adev 4432. */
#define VOICE_VAD_CLOSE_MIN     2600   /* [V871] was 2200, just under
                                        * the room p90 2304: the door
                                        * stayed open 70 s of 103 and
                                        * every utterance became an
                                        * 8 s, 256 KB ASR request. */

/* [V857] *** Why the VAD never opened -- measured, not guessed. ***
 *
 * v856 field log, quiet room:
 *     [V733] vad raw peak=32768 adev=16276 floor=16276 thr=65104/32552 started=0
 *
 * floor is a lower envelope of adev, so when the device plays its own TTS
 * reply the microphone (same board, 3 cm from the speaker) reports
 * adev ~17000 and floor climbs to match.  open is floor * OPEN_MUL, i.e.
 * 16276 * 4 = 65104 -- and the largest adev a 16-bit chunk can ever have is
 * 32768.  The gate was therefore unreachable by construction: no human
 * volume, at any distance, could open the VAD again for the rest of the
 * boot.  That is the reported "speak to it and nothing happens".
 *
 * Two caps and one freeze:
 *   FLOOR_MAX  -- the room is never allowed to be "noisy" past this;
 *   OPEN_MAX / CLOSE_MAX -- the gate can never exceed what a 16-bit chunk
 *                           can physically reach;
 *   while our own TTS is playing, the floor is pulled DOWN instead of up,
 *   so self-echo cannot raise the gate we judge the user against.
 */

#define VOICE_VAD_FLOOR_MAX     2000   /* [V857] room is never louder  */
#define VOICE_VAD_OPEN_MAX      6000   /* [V857] gate is always reachable */
#define VOICE_VAD_CLOSE_MAX     3600   /* [V857] */

/* [V857] VAD telemetry.
 *
 * Everything above answered "was it speech?" with one number per second
 * (the per-second MAXIMUM), which cannot separate "the room is at 1000 and
 * speech never came" from "speech arrived at 1200 and the gate is 3000".
 * Report percentiles instead: p50 is the room, p90/max is the loudest thing
 * that happened, and the gate-pass counters say which of the two tests
 * (peak, adev) is the one that refused.
 *
 * Storage is a fixed 128-bucket histogram of 256 sample-units per metric --
 * exact percentiles at +/-256 resolution, no allocation, no sorting. */

#define V857_NB   128
#define V857_BW   256

static uint32_t      g857_pk_s[V857_NB];   /* peak, this second   */
static uint32_t      g857_ad_s[V857_NB];   /* adev, this second   */
static uint32_t      g857_pk_e[V857_NB];   /* peak, this session  */
static uint32_t      g857_ad_e[V857_NB];   /* adev, this session  */
static unsigned long g857_n_s;
static unsigned long g857_n_e;
static long          g857_pkmax_s;
static long          g857_admax_s;
static long          g857_pkmax_e;
static long          g857_admax_e;
static unsigned long g857_pkpass_e;        /* chunks over the peak gate */
static unsigned long g857_adpass_e;        /* chunks over the adev gate */
static unsigned long g857_both_e;          /* chunks over both          */
static unsigned long g857_beat;            /* ui_state heartbeat counter */
static int          g857_run;              /* [V857b] consecutive chunks
                                            * currently over the gate  */

/* [V857e] *** No new session until this wall-clock second. ***
 *
 * QA measured the board's own voice through its own microphone at
 * ad p50 5376..9728 and p90 up to 12800 -- ABOVE OPEN_MAX (6000).
 * VOICE_LISTEN_SETTLE_MS is 800 ms and does not cover the playback
 * tail plus the pipeline buffers, so the board recognised its own
 * reply (transcript: Cantonese hallucination), answered it, and
 * looped.  This is only armed when TTS really played, so a normal
 * turn is not slowed down. */
#define VOICE_VAD_POSTPLAY_QUIET_S 2

static volatile time_t g857_quiet_until;  /* [V857e] */
static int             g857_dead_s;       /* [V857e] silent seconds  */
static int             g857_dead_rep;     /* [V857e] dumped once     */

/* [V858] Consecutive frozen seconds, and how many heals this boot. */

static int             g858_dead_s;
static int             g858_heals_here;
static volatile int    g857_beat_stop;

static void *v857_beat_task(void *arg);

/* [V857e] in audio_playback.c: prints the whole ES8311 register file.
 * Used by the RX-freeze watchdog below. */
extern void audio_direct_dump_codec(void);

/* [V858] Driver-side re-lock of the I2S receiver. */
extern void     esp32s3_i2s_v858_heal(void);
extern void     esp32s3_i2s_v858_relock_current(void);
extern uint32_t esp32s3_i2s_v858_rxconf(void);

/* [V858b] A whole second whose loudest chunk stays under this is not a
 * room: the measured healthy range is ad_max 2100..10159, and the
 * post-playback window measured 232..1420. */
#define V858_WEAK_MAX 1500L

/* [V857b] How many consecutive chunks must be over the gate before the
 * session counts as speech.  Measured: 1 chunk in 10000 of pure room
 * noise crossed the gate and cost a full ASR round trip on 63 ms of
 * noise.  A chunk is ~64 ms, so 3 = ~192 ms of continuous audio -- no
 * noise spike lasts that long, and no syllable is shorter than that. */
#define VOICE_VAD_OPEN_CHUNKS   2      /* [V865] was 1: one loud
                                        * chunk is a desk click,
                                        * two in a row is a voice. */

/* [V862] A 16-bit chunk whose DC-free mean deviation is above this
 * is not audio.  The boot-time capture fault delivers a
 * 0x0000/0x8000 square wave whose adev is 16336; real speech on
 * this board peaked at 9912 and a quiet room at ~2070.  Such a
 * chunk must not arm the VAD and must not be allowed to raise the
 * room floor, because the gate that floor sets then stays out of
 * reach of real speech for the rest of the boot. */
#define VOICE_VAD_ADEV_SANITY 12000

static void v857_push(uint32_t *h, int32_t v, long *pmax)
{
  int32_t a = (v < 0) ? -v : v;
  int     b = (int)(a / V857_BW);

  if (b < 0)
    {
      b = 0;
    }

  if (b > V857_NB - 1)
    {
      b = V857_NB - 1;
    }

  h[b]++;

  if (pmax != NULL && (long)a > *pmax)
    {
      *pmax = (long)a;
    }
}

/* Percentile of a histogram: the lower edge of the bucket in which the
 * cumulative count first reaches pct% of n.  0 when nothing was sampled. */

static long v857_pct(const uint32_t *h, unsigned long n, int pct)
{
  unsigned long acc  = 0;
  unsigned long want;
  int           b;

  if (n == 0)
    {
      return 0;
    }

  want = (n * (unsigned long)pct) / 100UL;

  if (want == 0)
    {
      want = 1;
    }

  for (b = 0; b < V857_NB; b++)
    {
      acc += (unsigned long)h[b];

      if (acc >= want)
        {
          return (long)(b * V857_BW);
        }
    }

  return (long)((V857_NB - 1) * V857_BW);
}

static void v857_second_reset(void)
{
  memset(g857_pk_s, 0, sizeof(g857_pk_s));
  memset(g857_ad_s, 0, sizeof(g857_ad_s));
  g857_n_s      = 0;
  g857_pkmax_s  = 0;
  g857_admax_s  = 0;
}

static void v857_session_reset(void)
{
  memset(g857_pk_e, 0, sizeof(g857_pk_e));
  memset(g857_ad_e, 0, sizeof(g857_ad_e));
  g857_n_e      = 0;
  g857_pkmax_e  = 0;
  g857_admax_e  = 0;
  g857_pkpass_e = 0;
  g857_adpass_e = 0;
  g857_both_e   = 0;
}

static int32_t g733_floor;                    /* slow lower envelope   */
static long    g733_open  = VOICE_VAD_OPEN_MIN;
static long    g733_close = VOICE_VAD_CLOSE_MIN;
static int     g733_ovr;                      /* vadgate.txt in force  */
static volatile int g733_rec_done;            /* rec thread last line  */
static int     g733_stuck;                    /* rec thread never left */

/* [V743] forward declaration -- defined just above recording_thread() */

static int rec_join_bounded(void);

/* [V731] *** Energy gate. ***
 *
 * The peak test alone cannot tell a quiet room from speech any more:
 *
 *   - about 1% of chunks contain exactly -32768 (0x8000), so
 *     "speech detected (peak=32768)" fires at the top of every session;
 *   - vpeak includes the codec's slow DC wander, which is most of what a
 *     silent chunk's peak is made of.
 *
 * With the microphone 8x hotter after v730 the quiet-room chunk peak is
 * 1500..6700 (p50 1524, p90 8740) against the old gate of 150, so the
 * VAD could never close: "end of utterance (15057 ms speech, 0 ms
 * silence)" -> thinking -> ASR hallucinates on noise -> back to
 * listening -> speech detected again, forever.  That is the
 * "stuck on 正在思考" report.  (v727 was already marginal: p50 230,
 * p90 699 vs 150.)
 *
 * So the chunk is also judged by its DC-free average absolute deviation
 *     adev = sum|x - mean| / n
 * which ignores the DC wander and is robust to isolated bad samples (one
 * -32768 in 1023 moves adev by only ~32).
 *
 * The numbers are re-read once a second from /tmp/vadgate.txt
 * ("<open> <close>", decimal) so they can be calibrated without a
 * reflash.  Defaults are a first guess -- see _work\\_v731_cal.log. */

#define VOICE_VAD_OPEN_ADEV     1200   /* DC-free level that opens */
#define VOICE_VAD_CLOSE_ADEV    700    /* below this we count silence */
#define VOICE_VAD_SILENCE_MS    700    /* [V871] was 900 */
#define VOICE_VAD_MIN_SPEECH_MS 200    /* [V871] was 250: at close
                                        * 2600 only about 40% of the
                                        * chunks inside an utterance
                                        * clear the gate, so 250 ms
                                        * of voiced audio took longer
                                        * to accumulate than a short
                                        * question lasts. */
#define VOICE_VAD_MAX_MS        15000  /* hard cap on one utterance */

/* [V865] A dip below the close gate shorter than this is a gap
 * between syllables, not the end of the sentence.  Without it a
 * two-word question is cut into two utterances that are each
 * too short to reach VOICE_VAD_MIN_SPEECH_MS. */
#define VOICE_VAD_HANGOVER_MS   128    /* [V866] was 300: at close
                                        * 1888 the room clears the
                                        * gate about once in eight
                                        * chunks and reset a 300 ms
                                        * hangover before it ever
                                        * expired, so silence_ms
                                        * stayed at 0 and the door
                                        * never shut. */

/* [V65] Defined further down; used here to ship the level report. */

static void vc_beacon_cb(const unsigned char *d, size_t l, int f, void *u);

static volatile int s_vad_started;     /* speech seen in this session */
static volatile int s_vad_silence_ms;  /* trailing silence, milliseconds */
static volatile int s_vad_speech_ms;   /* speech so far, milliseconds */
static volatile int s_vad_hang_ms;     /* [V865] ms below gate so far */

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
    volatile int vad_ended; /* endpointing closed this utterance */
} s_voice = {
    .state = VOICE_IDLE,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .speak_lock = PTHREAD_MUTEX_INITIALIZER,
};

/* [SPEAK-EPOCH 2026-10-29] 用户打断（BOOT 开始聆听 / V719 停止）时
 * epoch++，所有还在排队的旧回答句子拿到锁后发现时代不符即自弃。 */
static volatile unsigned s_speak_epoch   = 0;
static volatile unsigned s_speak_waiters = 0;

/* ── File-based helpers (kept for QEMU test commands) ──── */

static int read_pcm_file(const char* path,
    unsigned char** out, size_t* out_len)
{
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        syslog(LOG_ERR, "[%s] Cannot open %s: %d\n",
            TAG, path, errno);
        return -errno;
    }

    off_t fsize = lseek(fd, 0, SEEK_END);

    if (fsize <= 0 || (size_t)fsize > AGENT_VOICE_PCM_BUF_SIZE) {
        syslog(LOG_ERR, "[%s] File too large or empty: %ld\n",
            TAG, (long)fsize);
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

    *out = buf;
    *out_len = (size_t)fsize;
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

/* [V745] Session accounting.
 *
 * The always-on listener was seen dropping heap from 8,073,816 down to
 * 250,856 B, which is what left the board dead a few minutes after boot.
 * Idle sampling shows no drift at all, so whatever is leaking is on the
 * session path -- count the alloc/release pairs here so the next occurrence
 * can be attributed to a code path instead of guessed at.
 *
 * `held` in the [V745] heartbeat is the number of outstanding 256 KB
 * fallback PCM buffers: steady = clean, climbing = leaked. */

static unsigned long g745_sess;      /* sessions started             */
static unsigned long g745_pcm_alloc; /* fallback PCM buffers handed out */
static unsigned long g745_pcm_free;  /* fallback PCM buffers returned   */

/* [V745] Forward declaration -- recording_cleanup_locked() is defined above
 * the helper and is the first caller. */

static void v745_pcm_release(void);

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
    v745_pcm_release();
    s_voice.pcm_len = 0;

    /* Reset state to IDLE */
    s_voice.state = VOICE_IDLE;
}

/* Allocate fallback PCM buffer (must hold lock) */
/* [V745] Release the fallback PCM buffer and count the pair.  Every
 * teardown that owns s_voice.pcm_buf must come through here; NULL is a
 * no-op.  The bug this round fixed was a raw overwrite of the pointer, so
 * the rule "never assign s_voice.pcm_buf without releasing it first" now has
 * exactly one implementation. */

static void v745_pcm_release(void)
{
    unsigned char *p = s_voice.pcm_buf;

    s_voice.pcm_buf = NULL;
    if (p) {
        g745_pcm_free++;
        free(p);
    }
}

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
    g745_pcm_alloc++;   /* [V745] */
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

static void vc_beacon(const char *tag);   /* [V67] defined below */

static void vc_probe(const char *tag, long a, long b);  /* [V69] */

/* [V98] 语音情绪钩子：写 /tmp/emotion.txt，faced 读取并显示表情徽章 */
static void ui_state_set_ask(const char *text);   /* [V855] */
static void ui_state_beat(void);                  /* [V857] */

/* [V855] A question has been handed to the agent and its reply has not
 * been spoken yet.  The always-on listen loop must NOT open a new
 * recording session in the meantime.
 *
 * v854 measured "speak: channel acquired after 2000 ms" on EVERY turn:
 * the listener went straight back to voice_channel_start() after
 * dispatching, so by the time the LLM reply arrived the microphone was
 * already owned again and voice_channel_speak() had to wait out another
 * hand-over before a single sample reached the codec.  Holding the
 * listener off until the reply is on the air removes that 2 s and stops
 * the board from recording its own speaker.
 *
 * s_reply_deadline is the safety net: a reply that never comes must not
 * deafen the board. */
static volatile int    s_reply_pending;
static volatile time_t s_reply_deadline;
/* [V855b] */
static void emotion_set(const char *name);

/* [V743] *** One bounded join, shared by every stop path. ***
 *
 * v733 only armed voice_channel_stop().  voice_channel_discard(),
 * voice_channel_stop_with_text() and the start() failure branch kept the
 * bare pthread_join(), so a recording thread parked on the capture
 * semaphore froze any of them forever: s_voice.state stayed
 * VOICE_STOPPING(=2), every later voice_channel_start() returned -EBUSY,
 * and the board went mute until a power cycle.  That is the "pressing BOOT
 * does nothing, screen stuck on 正在思考" field report, end to end.
 *
 * 8000 ms is deliberate, not a guess: the worst-case exit path of
 * recording_thread is native_mq_receive_timed(CAP_MSG_TIMEOUT_SEC=1)
 * retried up to CAP_MQ_MAX_TIMEOUTS=6 times -> 6 s before it can even
 * look at ->stopped.  v733's 4000 ms window was therefore shorter than a
 * HEALTHY exit and always timed out, which then skipped
 * audio_capture_close() and leaked /dev/audio/pcm0c.  Freeing a capture
 * underneath a live thread is what produced the V713 mm_forcefree panic
 * (EXCCAUSE=001d, vaddr=0x8), so the timeout path detaches instead.
 *
 * g743_gen is a session generation, not a flag: a zombie from an earlier
 * session that was detached can still run its last line and set
 * g733_rec_done.  Without the generation check the next stop() would
 * trust that stale flag and pthread_join() a thread that is still running
 * -- i.e. hang by construction.  A new session cannot begin while one is
 * in flight (voice_channel_start() returns -EBUSY unless the state is
 * IDLE), so g743_gen is stable for the lifetime of a session. */

static volatile unsigned g743_gen;
static volatile unsigned g743_finished_gen;

/* [V744] consecutive -EIO/-EPIPE retries of the CURRENT session.  It used to
 * be a block-scope static inside the read loop, i.e. shared by every session
 * for the whole uptime and only cleared on a non-retryable error -- five
 * transient I2S errors EVER were enough to disable the retry path for good.
 * Reset once per session in recording_thread(). */

static int s_eio_cnt;

static int rec_join_bounded(void)
{
    unsigned my_gen = g743_gen;
    int      ms;

    for (ms = 0; ms < 8000; ms += 20)
      {
        if (g733_rec_done && g743_finished_gen == my_gen)
          {
            break;
          }

        usleep(20000);
      }

    if (g733_rec_done && g743_finished_gen == my_gen)
      {
        /* [V749] 不再 pthread_join()。线程在 voice_channel_start() 里
         * 已经 detach，内核会在它真正退出时回收栈与 TCB，这里只需要
         * 确认「最后一行之前的活都干完了」（见 H1 的顺序约定）。
         *
         * 被删掉的 pthread_join() 就是 v748 僵死的直接成因：它会为一个
         * 永不结束的线程无限期阻塞，让上面那个 8s 有界循环完全失去意义。 */

        g733_stuck = 0;
        return 0;
      }

    /* 超时：线程还活着，capture 不能关（关了就是 V713 的 mm_forcefree
     * panic），只能泄漏。线程已 detach，无需再 detach 一次。 */

    g733_stuck  = 1;

    syslog(LOG_ERR, "[%s] [V743] recording thread did not exit in %d ms -- "
           "detached, capture leaked\n", TAG, ms);

    return -ETIMEDOUT;
}

static void* recording_thread(void* arg)
{
    unsigned my_gen = g743_gen;   /* [V743] session generation */

    g745_sess++;                  /* [V745] */

    /* [V744] this session starts with a clean retry budget */

    s_eio_cnt = 0;
    (void)arg;
    unsigned char chunk[AGENT_ASR_CHUNK_SIZE];
    int need_fallback = 0;
    // int dump_fd = -1;
    size_t total_bytes_read = 0;
    size_t total_bytes_sent = 0;
    int chunk_count = 0;
    int abnormal_exit = 0;
    int32_t max_peak = 0;   /* [V660] loudest sample of the whole session */

    syslog(LOG_INFO, "[%s] recording thread started (streaming)\n", TAG);

    /* Use pre-connected ASR stream if available (opened in
     * voice_channel_start to avoid TLS handshake delay during
     * recording).  Fall back to opening a new session if not. */
    pthread_mutex_lock(&s_voice.lock);
    voice_asr_stream_t* stream = s_voice.asr_stream;
    s_voice.asr_stream = NULL; /* thread now owns it */
    pthread_mutex_unlock(&s_voice.lock);

    if (!stream) {
        /* No pre-connected stream — open now (may lose first 300-800ms) */
        stream = voice_asr_stream_open();
        /* [V693] Batch is the normal path with MiMo, so this is INFO. */
        syslog(LOG_INFO,
            "[%s] batch ASR: accumulating PCM, recognising on release\n",
            TAG);
        need_fallback = 1;

        pthread_mutex_lock(&s_voice.lock);
        if (alloc_fallback_buffer_locked() != 0) {
            s_voice.state = VOICE_IDLE;
            pthread_mutex_unlock(&s_voice.lock);
            sem_post(&s_voice.rec_ready);
            return NULL;
        }
        pthread_mutex_unlock(&s_voice.lock);
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

        int n = audio_capture_read(s_voice.cap,
            chunk, sizeof(chunk));

        /* [V846] *** Acoustic echo gate. ***
         *
         * The microphone hears the speaker on this board.  The log is one
         * self-answering turn after another:
         *
         *   [MIMO-TTS] streamed 66560 bytes to playback
         *   [voice]    end of utterance (2079 ms speech)
         *   [MIMO-ASR] transcript (adpcm): 专业小跟班，语音就绪。
         *   [voice]    PTT recording started
         *
         * i.e. the device transcribed the greeting it had just spoken and
         * fed it straight back in.  Throw these chunks away instead of
         * letting them reach the VAD; the listener simply waits for real
         * speech once the speaker is quiet. */

        if (n > 0 && audio_playback_echo_active())
          {
            static int s_v846_n;

            if ((s_v846_n++ % 16) == 0)
              {
                V1024S(LOG_ERR, "[V846]] echo gate: dropped %d-byte capture chunk "
                       "while the speaker is playing\n", n);
              }

            continue;
          }

        /* [V67] One-shot capture probes.  "B7_REC_FIRST_OK" proves audio is
         * being delivered at all; "B7_REC_ERR<n>" reports the first hard
         * error.  Both are visible in the TTS bridge log. */

        {
            static int s_v67_first;
            static int s_v67_err;

            if (n > 0 && !s_v67_first) {
                s_v67_first = 1;
                syslog(LOG_INFO,
                    "[%s] [V67] first capture chunk: %d bytes\n", TAG, n);
                vc_beacon("B7_REC_FIRST_OK");
            } else if (n < 0 && n != -EAGAIN && !s_v67_err) {
                char tb[32];

                s_v67_err = 1;
                snprintf(tb, sizeof(tb), "B7_REC_ERR%d", -n);
                vc_beacon(tb);
            }
        }

        if (n == -EAGAIN || n == -EWOULDBLOCK) {
            /* [V82] Count the empty polls.  "The microphone is silent" has to
             * be a measured fact, not an inference from a probe that never
             * fired - otherwise a missing B7_REC_FIRST_OK is unfalsifiable. */

            {
                static int s_sil;

                if ((++s_sil % 100) == 0) {
                    vc_probe("B7_RECZ", (long)total_bytes_read, (long)s_sil);
                }
            }

            /* Non-blocking capture socket has no data yet (common right
             * after start before the first frames arrive). Retry instead
             * of treating it as a fatal error. */
            usleep(10000);
            continue;
        }

        if (n <= 0) {
            if (s_voice.vad_ended) {
                /* The endpointing closed the stream on purpose.  Taking the
                 * abnormal path here would call recording_cleanup_locked(),
                 * which aborts the ASR stream - and with it the transcript we
                 * are waiting for. */

                syslog(LOG_INFO,
                    "[%s] capture closed by endpointing\n", TAG);
            } else {
                /* [V117] Retry on transient I2S bus errors instead of
                 * killing the thread permanently.  ES8311 returns -EIO
                 * when the bus is momentarily busy after a failed TTS
                 * playback attempt.  Up to 5 retries with 200ms gap. */

                /* [V693] -EPIPE is retried like -EIO.  It is what the
                 * capture layer used to answer after a spurious
                 * playback-side AUDIO_MSG_COMPLETE notice, and even with
                 * that fixed a wedged DMA descriptor should not cost the
                 * user the whole recording. */
                if ((n == -EIO || n == -EPIPE) && s_eio_cnt < 5) {
                    s_eio_cnt++;
                    syslog(LOG_WARNING,
                        "[%s] capture %s, retry %d/5\n", TAG,
                        (n == -EPIPE) ? "EPIPE" : "EIO", s_eio_cnt);
                    usleep(200000);
                    continue;   /* back to while(1) read loop */
                }
                s_eio_cnt = 0;
                syslog(LOG_WARNING,
                    "[%s] capture read returned %d, stopping\n", TAG, n);
                abnormal_exit = 1;
            }

            break;
        }

        total_bytes_read += (size_t)n;
        chunk_count++;

        /* Dump raw PCM to file */
        // if (dump_fd >= 0) {
        //     write(dump_fd, chunk, (size_t)n);
        // }

        /* Level of this chunk.  The peak used to be computed only for the
         * periodic log line; the endpointing below needs it for every chunk,
         * so it is measured unconditionally now. */

        int16_t* samples = (int16_t*)chunk;
        int sample_count = (int)((size_t)n / sizeof(int16_t));
        int32_t peak = 0;

        for (int i = 0; i < sample_count; i++) {
            int32_t v = samples[i] < 0 ? -samples[i] : samples[i];
            if (v > peak) {
                peak = v;
            }
        }

        if (peak > max_peak) { max_peak = peak; }   /* [V660] */

        if (chunk_count <= 3 || chunk_count % 50 == 0) {
            /* [V683] min tells the three candidate signatures apart:
             *   min == 0  && peak == 0   -> all zeros (no data at all)
             *   min == 0  && peak small -> sparse/one-sided constant
             *   min == -peak             -> a real bipolar AC signal
             */
            int32_t v683_min = 0;
            int     v683_i;

            for (v683_i = 0; v683_i < sample_count; v683_i++) {
                if (samples[v683_i] < v683_min) {
                    v683_min = samples[v683_i];
                }
            }

            syslog(LOG_INFO,
                "[%s] chunk#%d: %d bytes, peak=%ld min=%ld\n",
                TAG, chunk_count, n, (long)peak, (long)v683_min);

            if (chunk_count <= 2) {
                char v683_hb[240];
                int  v683_hp = 0;

                for (v683_i = 0;
                     v683_i < 32 && v683_i < sample_count;
                     v683_i++) {
                    v683_hp += snprintf(v683_hb + v683_hp,
                                        sizeof(v683_hb) - (size_t)v683_hp,
                                        "%04x ",
                                        (unsigned)(samples[v683_i] & 0xffff));
                }

                V1024S(LOG_ERR, "[V683]] chunk#%d hex: %s\n",
                       chunk_count, v683_hb);
            }
        }

        /* [V732] *** Measure the chunk BEFORE the preprocessor. ***
         *
         * The compressor plus its 6 dB makeup gain lifts a silent room by
         * about 10x (raw peak p50 1582 -> post-AGC 16308), so after
         * preproc_chunk() silence and speech both sit near full scale and
         * no threshold can separate them -- that is why the v731 gate
         * could not be calibrated.  Endpointing has to look at what the
         * microphone actually delivered.
         *
         * peak is max|sample|; adev is the DC-free average absolute
         * deviation, which ignores the codec's slow DC wander and is not
         * decided by one bad sample. */

        int32_t v732_raw_peak = 0;
        int32_t v732_raw_adev = 0;

        {
            int16_t* v732s = (int16_t*)chunk;
            int v732n = (int)((size_t)n / sizeof(int16_t));
            int32_t v732mean = 0;
            int64_t v732sum = 0;
            int64_t v732dev = 0;
            int v732i;

            for (v732i = 0; v732i < v732n; v732i++)
              {
                int32_t v = v732s[v732i] < 0 ? -v732s[v732i] : v732s[v732i];

                if (v > v732_raw_peak)
                  {
                    v732_raw_peak = v;
                  }

                v732sum += v732s[v732i];
              }

            if (v732n > 1)
              {
                v732mean = (int32_t)(v732sum / v732n);

                for (v732i = 0; v732i < v732n; v732i++)
                  {
                    int32_t vd = (int32_t)v732s[v732i] - v732mean;

                    if (vd < 0)
                      {
                        vd = -vd;
                      }

                    v732dev += vd;
                  }

                v732_raw_adev = (int32_t)(v732dev / v732n);
              }

            /* [V733] Slow lower envelope of the room.
             *
             * Falls fast (a quieter chunk drags it down by a quarter) and
             * rises very slowly (1/256 per chunk, ~2 s of continuous
             * speech to move it appreciably), so a sentence does not
             * raise the gate it is being judged against. */

            {
              int32_t v733a = v732_raw_adev;

              /* [V862] see VOICE_VAD_ADEV_SANITY. */
              {
                static int v862_rep;

                if (v732_raw_adev > VOICE_VAD_ADEV_SANITY)
                  {
                    if (!v862_rep)
                      {
                        v862_rep = 1;
                        V1024S(LOG_ERR, "[V862]] implausible adev %ld -- "
                               "capture fault, not fed to the "
                               "floor or the VAD\n",
                               (long)v732_raw_adev);
                      }

                    v733a = (int32_t)g733_floor;
                  }
              }


              /* [V866] *** Speech is not evidence about the room. ***
               *
               * The rise term below is (v733a - floor) / 256 per
               * chunk.  With v733a = 12000 and floor = 1500 that is
               * +41 a chunk, +650 a second: two seconds of talking
               * pins the floor at VOICE_VAD_FLOOR_MAX, which pins
               * open at 4000 and close at 2000 -- and both are
               * derived from floor, so the next sentence is judged
               * against the previous one.  That is the reported
               * answers once, then sits on listening forever.
               *
               * Measured on this board: floor 600..980 on a boot
               * that never played TTS, floor 1415..1888 on a boot
               * that did, with the room at adev p50 1280 both
               * times.  Same room, different estimate.
               *
               * Setting v733a to the current floor makes both
               * update branches add 0 -- the same no-op [V862]
               * uses for the implausible-adev clamp. */

              if (g733_floor != 0 && g733_open > 0 &&
                  (s_vad_started || v733a >= g733_open))
                {
                  v733a = (int32_t)g733_floor;
                }
              if (g733_floor == 0)
                {
                  g733_floor = (v733a < VOICE_VAD_FLOOR_MIN)
                               ? VOICE_VAD_FLOOR_MIN : v733a;
                }
              else if (v733a < g733_floor)
                {
                  /* [V857d] was /4.  At /4 a single one-second capture
                   * hiccup (the stream delivered n=1 chunk, adev 8)
                   * dragged the floor from ~1500 to FLOOR_MIN and the
                   * gate down with it, below the room's own p90 -- six
                   * noise-triggered ASR round trips per 120 s.
                   * 1/64 still follows a genuinely quieter room in a
                   * few seconds; it just cannot collapse in one. */
                  g733_floor += (v733a - g733_floor) / 64;
                }
              else
                {
                  g733_floor += (v733a - g733_floor) / 256;
                }

              /* [V857] This chunk is louder than the current floor.
               * If the loudness is our own TTS coming back through the
               * microphone it must NOT raise the gate the user is about
               * to be judged against -- pull the floor down instead.
               * (v856: adev 16276 while speaking -> open 65104 -> VAD
               * unreachable for the rest of the boot.) */

              if (s_voice.tts_pb != NULL)
                {
                  g733_floor += (VOICE_VAD_FLOOR_MIN - g733_floor) / 4;
                }

              if (g733_floor < VOICE_VAD_FLOOR_MIN)
                {
                  g733_floor = VOICE_VAD_FLOOR_MIN;
                }

              /* [V857] hard ceiling on how loud the room may be called */

              if (g733_floor > VOICE_VAD_FLOOR_MAX)
                {
                  g733_floor = VOICE_VAD_FLOOR_MAX;
                }
            }
        }

#ifdef CONFIG_AI_AGENT_AUDIO_PREPROCESS
        preproc_chunk(pp, chunk, n);
#endif

        /* ── Endpointing ────────────────────────────────────────────────
         * Measured after preprocessing so the AGC gain counts towards the
         * decision (a quiet talker is boosted before it is judged). */

        {
            int vcount = (int)((size_t)n / sizeof(int16_t));
            int32_t vpeak = v732_raw_peak;   /* [V732] raw, pre-AGC */
            int32_t vadev = v732_raw_adev;
            int chunk_ms = (vcount * 1000) / AGENT_VOICE_SAMPLE_RATE;

            /* [V731] live energy-gate thresholds, refreshed once a second
             * from /tmp/vadgate.txt further down. */

            static long s731_open  = VOICE_VAD_OPEN_ADEV;
            static long s731_close = VOICE_VAD_CLOSE_ADEV;

            if (chunk_ms <= 0) {
                chunk_ms = 1;
            }

            /* [V732] vpeak / vadev already hold the raw, pre-AGC values
             * computed above. */

            /* [V857] Feed the histograms and count, per chunk, which of
             * the two tests would have passed.  At the end of a session
             * that says exactly which gate rejected the user. */

            {
                int pk_ok = (vpeak >= VOICE_VAD_OPEN_PEAK);
                int ad_ok = (vadev >= g733_open);

                v857_push(g857_pk_s, vpeak, &g857_pkmax_s);
                v857_push(g857_ad_s, vadev, &g857_admax_s);
                v857_push(g857_pk_e, vpeak, &g857_pkmax_e);
                v857_push(g857_ad_e, vadev, &g857_admax_e);

                g857_n_s++;
                g857_n_e++;

                if (pk_ok) { g857_pkpass_e++; }
                if (ad_ok) { g857_adpass_e++; }
                if (pk_ok && ad_ok) { g857_both_e++; }
            }

            /* [V64] Level forensics.
             *
             * The VAD opens at VOICE_VAD_OPEN_PEAK, and the log repeats
             * "no speech for 8000 ms" -- which is either a silent capture
             * path (I2S/ES8311 gain) or a threshold above whatever the
             * microphone really delivers.  Report the loudest chunk seen
             * each second so the two can be told apart. */

            {
                static int32_t s_v64_peak;
                static int32_t s_v64_adev;
                static time_t s_v64_ts;
                time_t v64_now = time(NULL);

                if (vpeak > s_v64_peak) {
                    s_v64_peak = vpeak;
                }

                if (vadev > s_v64_adev) {
                    s_v64_adev = vadev;
                }

                /* [V731] Once per second, and no longer capped at three
                 * lines: the split between the noise floor and real speech
                 * has to be measurable over a whole capture. */

                if (v64_now != s_v64_ts) {
                    FILE *v731f;

                    s_v64_ts = v64_now;

                    /* Live threshold override: "<open> <close>", decimal. */

                    v731f = fopen("/tmp/vadgate.txt", "r");

                    if (v731f != NULL)
                      {
                        char v731l[64];

                        if (fgets(v731l, sizeof(v731l), v731f) != NULL)
                          {
                            int v731a = -1;
                            int v731b = -1;

                            if (sscanf(v731l, "%d %d", &v731a, &v731b) >= 1
                                && v731a >= 0)
                              {
                                s731_open = v731a;
                                g733_ovr  = 1;

                                if (v731b >= 0)
                                  {
                                    s731_close = v731b;
                                  }
                              }
                          }

                        fclose(v731f);
                      }

                    /* [V733] Effective gates for the next second. */

                    if (g733_ovr)
                      {
                        g733_open  = s731_open;
                        g733_close = s731_close;
                      }
                    else
                      {
                        g733_open  = (long)g733_floor * VOICE_VAD_OPEN_MUL;
                        g733_close = (long)g733_floor * VOICE_VAD_CLOSE_MUL;

                        if (g733_open < VOICE_VAD_OPEN_MIN)
                          {
                            g733_open = VOICE_VAD_OPEN_MIN;
                          }

                        if (g733_close < VOICE_VAD_CLOSE_MIN)
                          {
                            g733_close = VOICE_VAD_CLOSE_MIN;
                          }
                      }

                    /* [V857] *** Ceiling. ***
                     *
                     * Without this the gate could be set above 32768,
                     * the largest adev any 16-bit chunk can have, and
                     * then no amount of speech can ever open the VAD.
                     * Applies to the /tmp/vadgate.txt override too: a
                     * hand-written gate must not be able to deafen the
                     * board either. */

                    if (g733_open > VOICE_VAD_OPEN_MAX)
                      {
                        g733_open = VOICE_VAD_OPEN_MAX;
                      }

                    if (g733_close > VOICE_VAD_CLOSE_MAX)
                      {
                        g733_close = VOICE_VAD_CLOSE_MAX;
                      }

                    V1024S(LOG_ERR, "[V733]] vad raw peak=%ld adev=%ld floor=%ld "
                           "thr=%ld/%ld started=%d\n",
                           (long)s_v64_peak, (long)s_v64_adev,
                           (long)g733_floor, g733_open, g733_close,
                           s_vad_started);

                    /* [V857] One line per second: what the microphone
                     * actually delivered (p50 = the room, p90/max = the
                     * loudest thing that happened) against the gate it
                     * is being judged by.  peakgate is the fixed peak
                     * test, open/close the adaptive adev test. */

                    V1024S(LOG_ERR, "[V857]] sec n=%lu pk p50=%ld p90=%ld max=%ld "
                           "ad p50=%ld p90=%ld max=%ld | floor=%ld "
                           "open=%ld close=%ld peakgate=%d started=%d "
                           "run=%d\n",
                           g857_n_s,
                           v857_pct(g857_pk_s, g857_n_s, 50),
                           v857_pct(g857_pk_s, g857_n_s, 90),
                           g857_pkmax_s,
                           v857_pct(g857_ad_s, g857_n_s, 50),
                           v857_pct(g857_ad_s, g857_n_s, 90),
                           g857_admax_s,
                           (long)g733_floor, g733_open, g733_close,
                           VOICE_VAD_OPEN_PEAK, s_vad_started,
                           g857_run);

                    /* [V865b] While the door is open the three
                     * counters are the whole story: sp has to reach
                     * VOICE_VAD_MIN_SPEECH_MS, si has to reach
                     * VOICE_VAD_SILENCE_MS, and hg says whether the
                     * hangover is eating the gaps inside a sentence. */
                    if (s_vad_started)
                      {
                        V1024S(LOG_ERR, "[V865b]] open sp=%d si=%d hg=%d "
                               "gate=%ld/%ld\n",
                               s_vad_speech_ms, s_vad_silence_ms,
                               s_vad_hang_ms, g733_open, g733_close);
                      }

                    /* [V857e] *** RX freeze watchdog. ***
                     *
                     * QA hit this in 3 of 4 cold boots: after one
                     * TTS playback the capture returns all zeros (or
                     * a frozen DC such as 0x0012, peak 18) forever,
                     * while the codec readback stays healthy.  A
                     * whole second of chunks with adev == 0 is that
                     * state.  Reopening the capture does not clear
                     * it (the listener already does that every 8 s),
                     * so this does not pretend to fix it -- it pins
                     * the moment and dumps the register file so the
                     * driver-side fix has evidence. */

                    /* [V858] *** Self-heal, before the dump below. ***
                     *
                     * A whole second of chunks whose DC-free deviation
                     * is zero is the frozen-capture state: either every
                     * word is 0 or every word is the same constant
                     * (0x0012, peak 18 in the qa3 boot).  Reopening the
                     * capture does not clear it -- the listener already
                     * does that every few seconds -- because the port,
                     * not the stream, is what is wrong.
                     *
                     * So every second frozen second, re-lock the
                     * receiver and flip its clock role.  The role the
                     * microphone comes back on is printed with the
                     * heal; if the capture stays frozen across several
                     * heals the [V858] log shows whether one of the two
                     * roles ever works at all on this boot.
                     *
                     * The VAD gates themselves are untouched: this only
                     * moves hardware. */

                    /* [V858b] A second whose loudest chunk is under
                     * V858_WEAK_MAX is the degraded state; a second with
                     * no deviation at all is the frozen one and counts
                     * twice, so it is repaired after one second instead
                     * of two.
                     *
                     * The first repair is a plain re-lock: the registers
                     * are already correct, so what has to move is the bit
                     * phase, and a fresh start edge draws a new one.
                     * Only if that has not helped after two attempts
                     * does the clock role flip -- a flip changes the
                     * shape of the capture, so it is held in reserve. */

                    if (g857_admax_s == 0)
                      {
                        g858_dead_s += 2;
                      }
                    else if (g857_n_s > 0 && g857_admax_s < V858_WEAK_MAX)
                      {
                        g858_dead_s += 1;
                      }
                    else
                      {
                        g858_dead_s   = 0;
                        g858_heals_here = 0;
                      }

                    if (g858_dead_s >= 2)
                      {
                        g858_dead_s = 0;
                        g858_heals_here++;

                        if (g858_heals_here > 2)
                          {
                            esp32s3_i2s_v858_heal();
                          }
                        else
                          {
                            esp32s3_i2s_v858_relock_current();
                          }

                        V1024S(LOG_ERR, "[V858]] RX weak %ld (ad_max) n=%lu pkmax=%ld "
                               "-> repair #%d %s rxconf=0x%08x\n",
                               g857_admax_s, g857_n_s, g857_pkmax_s,
                               g858_heals_here,
                               (g858_heals_here > 2) ? "role-flip"
                                                     : "relock",
                               (unsigned long)esp32s3_i2s_v858_rxconf());
                      }

                    if (g857_n_s > 0 && g857_admax_s == 0)
                      {
                        g857_dead_s++;
                      }
                    else if (g857_admax_s > 0)
                      {
                        if (g857_dead_s >= 3 && g857_dead_rep)
                          {
                            g857_dead_rep = 0;
                            V1024S(LOG_ERR, "[V857e]] RX recovered after %d s "
                                   "of silence\n", g857_dead_s);
                          }

                        g857_dead_s = 0;
                      }

                    if (g857_dead_s >= 3 && !g857_dead_rep)
                      {
                        g857_dead_rep = 1;
                        V1024S(LOG_ERR, "[V857e]] RX FROZEN: %d s of chunks "
                               "with adev=0 (n=%lu, peak max=%ld) -- "
                               "dumping ES8311\n",
                               g857_dead_s, g857_n_s, g857_pkmax_s);
                        audio_direct_dump_codec();
                      }

                    v857_second_reset();

                    s_v64_peak = 0;
                    s_v64_adev = 0;
                }
            }

            /* [V731] Both tests have to agree: the peak test alone is
             * decided by one -32768 sample, the deviation test alone would
             * open on any steady tone. */

            /* [V857b] Length of the current above-gate run. */

            if (vpeak >= VOICE_VAD_OPEN_PEAK && vadev >= g733_open &&
                vadev <= VOICE_VAD_ADEV_SANITY) {
                /* [V857e] Our own voice is louder than OPEN_MAX; do
                 * not let it arm the VAD during the cooldown. */
                if (time(NULL) < g857_quiet_until) {
                    g857_run = 0;
                } else {
                    g857_run++;
                }
            } else {
                g857_run = 0;
            }

            /* [V857b] Same two tests as before, plus: to OPEN, the
             * run must have lasted VOICE_VAD_OPEN_CHUNKS chunks.  To
             * STAY open the old test alone is enough, so hysteresis
             * and endpointing are untouched. */

            /* [V865] *** Hysteresis: open high, stay open low. ***
             *
             * Before this the SAME gate (g733_open) decided both
             * whether to open the VAD and whether the chunk counts
             * towards s_vad_speech_ms, so every 64 ms window of an
             * utterance had to be at full speech level to count at
             * all.  Human speech has syllabic dips far below the
             * open gate, so speech_ms crept up in single-chunk
             * fragments and never reached VOICE_VAD_MIN_SPEECH_MS;
             * the field log is a wall of
             *     [V862c] releasing latch: 189 ms speech < 250 ms
             *                              after 945 ms silence
             * Once started, judge the chunk against the CLOSE gate
             * instead -- that is what a close gate is for. */
            if (vpeak >= (s_vad_started ? VOICE_VAD_CLOSE_PEAK
                                        : VOICE_VAD_OPEN_PEAK) &&
                vadev >= (s_vad_started ? g733_close : g733_open) &&
                vadev <= VOICE_VAD_ADEV_SANITY &&
                (s_vad_started || g857_run >= VOICE_VAD_OPEN_CHUNKS)) {
                if (!s_vad_started) {
                    s_vad_started = 1;
                    syslog(LOG_INFO,
                        "[%s] voice: speech detected (peak=%ld)\n",
                        TAG, (long)vpeak);
                }

                s_vad_speech_ms += chunk_ms;
                s_vad_silence_ms = 0;
                s_vad_hang_ms = 0;
            } else if (s_vad_started &&
                       (vpeak < VOICE_VAD_CLOSE_PEAK ||
                        vadev < g733_close)) {
                /* [V865] *** Hangover. ***
                 *
                 * A gap shorter than VOICE_VAD_HANGOVER_MS is a
                 * gap inside a sentence, not the end of one.  It
                 * must not feed s_vad_silence_ms, or a question
                 * is cut into fragments each too short to send. */
                s_vad_hang_ms += chunk_ms;

                if (s_vad_hang_ms > VOICE_VAD_HANGOVER_MS)
                  {
                    s_vad_silence_ms += chunk_ms;
                  }

                /* [V862c] An open VAD that never reached
                 * VOICE_VAD_MIN_SPEECH_MS can never satisfy the
                 * end-of-utterance test below, so s_vad_started stays
                 * 1 for the whole session -- and the listener's own
                 * no-speech timeout is guarded by !s_vad_started, so
                 * the session never restarts and the UI sits on
                 * "listening" indefinitely.  Release the latch. */
                if (s_vad_speech_ms < VOICE_VAD_MIN_SPEECH_MS &&
                    s_vad_silence_ms >= VOICE_VAD_SILENCE_MS)
                  {
                    V1024S(LOG_ERR, "[V862c]] releasing latch: %d ms speech < %d "
                           "ms minimum after %d ms silence\n",
                           s_vad_speech_ms, VOICE_VAD_MIN_SPEECH_MS,
                           s_vad_silence_ms);

                    s_vad_started     = 0;
                    s_vad_speech_ms   = 0;
                    s_vad_hang_ms     = 0;
                    s_vad_silence_ms  = 0;
                  }
            }

            /* [V733] *** A 15 s "utterance" with ZERO silence is noise. ***
             *
             * The field log is full of
             *     end of utterance (15057 ms speech, 0 ms silence)
             * and every one of them was ambient noise: human speech has
             * inter-word and inter-sentence gaps, and the close gate is
             * far enough below the open gate that real speech always
             * produces some silence count.  Sending those 15 s to the ASR
             * is what produced "二百四。" and kept the loop alive.
             *
             * Do NOT set vad_ended here: the listener's own
             * VOICE_LISTEN_NO_SPEECH_MS timeout then closes the session
             * from outside, which is the order V720 found to work. */

            if (s_vad_started && s_vad_speech_ms >= VOICE_VAD_MIN_SPEECH_MS &&
                s_vad_silence_ms == 0 &&
                s_vad_speech_ms + s_vad_silence_ms >= VOICE_VAD_MAX_MS)
              {
                syslog(LOG_INFO,
                       "[%s] [V733] dropped: %d ms with 0 ms silence "
                       "(floor=%ld open=%ld)\n",
                       TAG, s_vad_speech_ms, (long)g733_floor, g733_open);

                s_vad_started   = 0;
                s_vad_speech_ms = 0;
                s_vad_hang_ms = 0;
                s_vad_silence_ms = 0;
              }
            else if (s_vad_started &&
                s_vad_speech_ms >= VOICE_VAD_MIN_SPEECH_MS &&
                (s_vad_silence_ms >= VOICE_VAD_SILENCE_MS ||
                 s_vad_speech_ms + s_vad_silence_ms >= VOICE_VAD_MAX_MS)) {
                syslog(LOG_INFO,
                    "[%s] voice: end of utterance "
                    "(%d ms speech, %d ms silence)\n",
                    TAG, s_vad_speech_ms, s_vad_silence_ms);

                pthread_mutex_lock(&s_voice.lock);
                s_voice.vad_ended = 1;
                pthread_mutex_unlock(&s_voice.lock);

                /* [V720] *** Do NOT abort the capture from this thread. ***
                 *
                 * This used to call audio_capture_abort(s_voice.cap) here,
                 * and the thread never came back out of it: ps on the wedged
                 * board shows recording_thread parked on a semaphore while
                 * voice_listen_task sits in pthread_join, so the channel is
                 * stuck in VOICE_STOPPING for good and every later session
                 * fails with -EBUSY.
                 *
                 * Only this path aborts from inside the recording thread --
                 * the silence gate and the BOOT key both abort from the
                 * listener, which is the order that has always worked, and
                 * the listener is already waiting on exactly the flag this
                 * block sets.  Setting it is enough: within one 20 ms poll
                 * the listener calls voice_channel_stop(), which aborts the
                 * capture from outside and then finalises the ASR. */

                break;
            }
        }

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

    /* [V660] maxpeak is the single number that answers "did the microphone
     * put anything into the stream at all?".  The v658 field run reported a
     * constant peak=6 on every chunk (= the [V380] loopback/idle figure),
     * which no human speech can produce. */
    /* [V743] stamp the generation so a stale zombie cannot satisfy the
     * next session's wait (see rec_join_bounded()). */

    /* [V749] 顺序很重要：exit 日志必须打在 done 标志之前。
     *
     * rec_join_bounded() 现在只看 g733_rec_done（线程已在 H2 里 detach，
     * 无法再 join）。若标志仍排在 syslog 之前，那么"标志已置位"与"线程
     * 真的结束"之间就隔着一个可能阻塞的 syslog()，调用方据此去
     * audio_capture_close() 就会关到一个活线程手上的设备 —— 那正是
     * V713 的 mm_forcefree panic (EXCCAUSE=001d, vaddr=0x8)。
     * 把日志挪到前面之后，done=1 与 return NULL 之间再无任何调用。 */

    syslog(LOG_INFO,
        "[%s] recording thread exit: %d chunks, %zu read, %zu sent%s"
        " maxpeak=%ld\n",
        TAG, chunk_count, total_bytes_read, total_bytes_sent,
        abnormal_exit ? " (abnormal)" : "", (long)max_peak);

    g743_finished_gen = my_gen;
    g733_rec_done = 1;
    return NULL;
}


/* ── Wake-word gate ─────────────────────────────────────────────────────
 * This build has no on-device WakeNet, so "wake-up" is keyword matching on
 * the ASR transcript.  Once woken, the conversation stays open for
 * AGENT_VOICE_WAKE_ACTIVE_SEC so follow-up sentences need no wake word.
 * Side effect: it also drops ASR hallucinations caused by ambient noise.
 */
#ifndef AGENT_VOICE_WAKE_ENABLE
#define AGENT_VOICE_WAKE_ENABLE 1
#endif

#ifndef AGENT_VOICE_WAKE_ACTIVE_SEC
#define AGENT_VOICE_WAKE_ACTIVE_SEC 30
#endif

#ifndef AGENT_VOICE_WAKE_WORDS
/* 唤醒词：小沃优先（机器人人格名），保留小智作 ASR 容错（转写歧义）。 */
/* [V849] Homophone tolerance.  ASR writes 小沃 as 小我 / 小窝 / 小喔 often
 * enough that a literal strstr() over one spelling never matches; every
 * capture so far shows "wake word not found" on 100% of transcripts. */
#define AGENT_VOICE_WAKE_WORDS "\u4f60\u597d\u5c0f\u6c83,\u5c0f\u6c83,\u5c0f\u6211,\u5c0f\u7a9d,\u5c0f\u559a,\u5c0f\u8717,\u5c0f\u6c83\u5c0f\u6c83,\u4f60\u597d\u5c0f\u667a,\u5c0f\u667a,\u5c0f\u5fd7,\u5c0f\u77e5,\u4f60\u597d\u5c0f\u7a9d,\u4f60\u597d\u5c0f\u6211"
#endif

static time_t s_wake_active_ts;

/* [V712] Set when an utterance was recognised but thrown away because it did
 * not contain a wake word.  The always-on listening loop reads it to stop
 * waiting for a reply that was never going to come. */

static volatile int s_wake_missed;

/* Returns 1 when the utterance should be dispatched to the agent. */
/* [V850] Runtime wake-gate switch.
 *
 * BOOT (GPIO0) is electrically dead on this unit: 3200+ samples and several
 * real presses gave lo=0 / chg=0, while GPIO45 and GPIO46 on the same
 * sampler changed 1598 times.  Sampling runs about 21 times a second, so a
 * half-second press would have been seen roughly ten times -- it is not a
 * missed poll.  A pad that never moves cannot be fixed in firmware.
 *
 * That left the wake word as the only way in, and it only matches if ASR
 * spells 小沃 exactly as the table does.  So the gate is OFF by default:
 * every non-empty transcript is accepted.
 *
 * Put it back without rebuilding:
 *
 *     echo 1 > /data/wakegate.txt     persistent
 *     echo 1 > /tmp/wakegate.txt      until the next reboot
 *
 * /data wins over /tmp.  Any first character other than '0' enables it. */
static int v850_wake_gate_on(void)
{
  static const char *paths[2] =
  {
    "/data/wakegate.txt",
    "/tmp/wakegate.txt"
  };
  int i;

  for (i = 0; i < 2; i++)
    {
      FILE *f = fopen(paths[i], "r");

      if (f != NULL)
        {
          int c = fgetc(f);

          fclose(f);

          if (c != EOF && c != '0')
            {
              return 1;
            }
        }
    }

  return 0;
}

static int voice_wake_accepted(const char *text)
{
#if !AGENT_VOICE_WAKE_ENABLE
    (void)text;
    return 1;
#else
    char list[256];

    if (text == NULL || text[0] == '\0') {
        return 0;
    }

    /* [V850] Gate switched off -> accept everything spoken.  The 30 s
     * follow-up window still applies, so a conversation stays open. */

    if (!v850_wake_gate_on())
      {
        s_wake_active_ts = time(NULL);
        syslog(LOG_INFO,
               "[%s] [V850] wake gate OFF (no /data/wakegate.txt): accepted "
               "without a wake word\n", TAG);
        return 1;
      }

    /* Conversation already open -> accept without repeating the wake word. */
    if (s_wake_active_ts != 0 &&
        (time(NULL) - s_wake_active_ts) < AGENT_VOICE_WAKE_ACTIVE_SEC) {
        s_wake_active_ts = time(NULL);
        return 1;
    }

    strncpy(list, AGENT_VOICE_WAKE_WORDS, sizeof(list) - 1);
    list[sizeof(list) - 1] = '\0';

    for (char *w = strtok(list, ","); w != NULL; w = strtok(NULL, ",")) {
        if (*w != '\0' && strstr(text, w) != NULL) {
            s_wake_active_ts = time(NULL);
            syslog(LOG_INFO, "[%s] wake word matched: %s\n", TAG, w);
            diag_report("WAKE_HIT");
            return 1;
        }
    }

    return 0;
#endif
}

/* \u2500\u2500 Button (BOOT / GPIO0) wake \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
 * The AI-VOX3 board exposes a single tactile BOOT button wired to
 * GPIO0 (BUTTON_BOOT), configured INPUT+PULLUP by the board at boot.
 * A press reads as active-LOW; board_buttons() (standard NuttX board
 * API) returns bit 0x1 while held.  We poll it and, on a fresh press,
 * inject the *same* wake event the ASR path produces:
 *   - open the 30s wake window (s_wake_active_ts)
 *   - push an inbound voice message so the agent greets / opens the
 *     conversation.  Voice (ASR) wake below is kept fully intact.
 */
extern int esp32s3_gpioread(int pin);  /* arch GPIO read, defined in libarch.a */
extern int esp32s3_configgpio(uint32_t pin, uint16_t attr);  /* arch GPIO config */

/* [V690] Audio output volume, implemented in nuttx/drivers/audio/es8311.c.
 * FLAT build, so the app links straight against it.  Scale = 0..1000. */
extern void es8311_ext_setvolume(uint16_t volume);

static void button_wake_trigger(void)
{
    syslog(LOG_INFO, "[%s] BUTTON wake triggered\n", TAG);
    diag_report("BTN_WAKE");
    /* [V314] 用户要求 BOOT 只作唤醒、不要带动舵机。
     * 原来这里每次唤醒都 servo_shake() 摇头，看起来像"BOOT 绑定了舵机"。 */

    /* open the conversation window so follow-up speech bypasses the gate */
    s_wake_active_ts = time(NULL);

    agent_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    strncpy(msg.channel, AGENT_CHAN_VOICE, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, "voice", sizeof(msg.chat_id) - 1);
    msg.content = strdup("\u4f60\u597d\u5c0f\u6c83");
    if (msg.content != NULL) {
        message_bus_push_inbound(&msg);
    }
}

/* [V315] BOOT as a true push-to-talk key with on-screen status text.
 *   press   -> on-screen listen prompt + voice_channel_start()
 *   release -> on-screen thinking prompt + voice_channel_stop_with_text()
 * voice_channel_start fails -> fall back to the old wake injection so the
 * button always does something. */
/* [V315] 屏幕状态由 emotion_set() -> /tmp/emotion.txt -> info_screen 显示，
 * 不再需要 LVGL chat UI（本固件未启用 CONFIG_AI_AGENT_LVGL_UI）。 */

static bool s_ptt_recording = false;

/* [V369] The release edge is NOT reliable on this board: GPIO0's pad can
 * be re-routed by another driver, which freezes GPIO_IN_REG at whatever
 * level it held at the time -- so after a press it can read LOW forever
 * and button_ptt_release() is never called.  The session then hangs in
 * "listening" and no ASR ever runs.  Cap the session so it always ends. */
#define VOICE_PTT_MAX_SEC  8

/* [V744] Hard cap on how long a PTT press may stay latched.  s_ptt_hold is
 * set by button_ptt_press() and cleared only by button_ptt_release(), so a
 * lost RELEASE edge (pad bounce, console contention, a poller that died
 * mid-hold) would keep the session in VOICE_RECORDING for good.  30 s is far
 * longer than any real utterance, so nothing that used to work changes --
 * this only guarantees the latch always unwinds. */

#define VOICE_PTT_HOLD_MAX_SEC  30
/* [V370] minimum recording length.  One physical press produced THREE
 * press/release pairs (V369 log) because the pad level glitches; the
 * release fired ~0.3s in and killed the capture thread with
 * "0 chunks, 0 read".  A real utterance needs a couple of seconds. */
/* [V700] Was 3 s.  A push-to-talk key is pressed for as long as the
 * user speaks, and a normal utterance here is one to two seconds; the
 * 3 s floor refused the release and the session then kept recording
 * silence until VOICE_PTT_MAX_SEC fired, so the ASR was handed seven
 * seconds of nothing.  1 s still covers the v370 pad-glitch case
 * (the release used to fire 0.3 s in) without eating real speech. */
/* [V705] Was 1 s.  The v704 field capture shows the user's natural
 * press is 0.98 s / 0.48 s -- "release ignored: 980ms recorded (min
 * 1000ms)" -- so the 1 s floor refused the release and the capture ran on
 * to the 8 s ceiling: 125 chunks x 2046 = 255750 B = 7.99 s of pure dead
 * air before the ASR was handed anything.  That is the "answered very
 * slowly" the user reported.
 *
 * The v370 pad-glitch case this floor guarded against no longer exists:
 * the same capture logged 9 clean BTN_EDGE RELEASE edges and not a single
 * "0 chunks, 0 read" session.  A release now stops the capture at once. */
#define VOICE_PTT_MIN_SEC  0
static time_t s_ptt_start = 0;
/* [V700] time(NULL) only resolves to a whole second, so a 1.9 s press
 * was reported as "0s recorded" and refused.  Keep a 10 ms tick stamp
 * next to it and judge the release on that instead. */
static clock_t s_ptt_start_tick = 0;
static int    s_btn_level  = 0;   /* debounced level  */
static int    s_btn_stable = 0;   /* polls it has held */

/* [V687] PTT session gate.
 *
 * The v686 field run (_v686_btnfix.log) showed ONE physical press opening
 * THREE complete capture sessions:
 *     144.550 BUTTON PTT press -> listen / capture started
 *     144.740 BUTTON PTT press -> listen / capture started
 *     148.482 a third capture, then
 *     150.916 / 151.479 / 152.664 "recording thread exit: 6 chunks, 9208 read"
 * The two starts at 144.5/144.7 carry different bounce slots
 * (0x3c23dbe8 vs 0x3c224608), so they are two real runs, not a kmsg replay.
 *
 * The pad dithers, so no amount of debouncing makes its edges trustworthy.
 * Instead make the SESSION atomic: latch the edge before acting on it,
 * allow only one session at a time, and cool down afterwards.
 */

/* [V700] Was 2 s.  The log shows "BTN_PRESS ignored: 1530ms cooldown
 * (< 2000ms)" three times in a row: the user pressed, nothing happened,
 * pressed again, and every retry was discarded. */
#define VOICE_PTT_COOLDOWN_SEC  1

static volatile int s_ptt_lock = 0;   /* one decider at a time        */
static volatile int s_ptt_hold = 0;   /* session in flight (start..done) */

/* [V712] Set by the BOOT key while it is taking the microphone away from the
 * always-on listener.  The listener must not open a session while it is set,
 * and must abandon the one it is holding.  The key clears it once
 * voice_channel_start() has run. */

static volatile int s_ptt_preempt;
/* [V689] 10 ms ticks (CONFIG_USEC_PER_TICK=10000), NOT seconds: with a
 * 1-second resolution the first ghost press 250 ms after the real one still
 * looked like a fresh session. */

static clock_t      s_ptt_evt_ticks = 0;   /* last session start/end  */

/* [V705] Latency probe.  Pure logging: clock_systime_ticks() + syslog()
 * only -- no pad, no GPIO register, no session state -- so unlike the v699
 * [V699] keyraw walk it cannot perturb the button sample.
 *
 * One press prints t0 / asr_go / asr_rsp / tts_go, which splits the reply
 * delay into capture, upload+ASR and dispatch+LLM+TTS. */
static clock_t      s_p1_tick = 0;

#define P1_MS() \
    (s_p1_tick ? (long)((clock_systime_ticks() - s_p1_tick) * 10) : -1L)

/* ─────────────────────────────────────────────────────────────
 * [V719] Dialog layer, ported from xiaozhi-esp32 (AI-VOX3 firmware).
 *
 *   application.cc  HandleToggleChatEvent(): a single BOOT click is a
 *                   three-way dispatch -- idle opens the channel,
 *                   listening closes it (no ASR), speaking aborts the
 *                   TTS playback.
 *   device_state_machine.cc: transitions are validated against a
 *                   whitelist; an illegal one is refused and logged.
 *   ai_vox3_board.cc ButtonPollTask(): a click is judged on RELEASE;
 *                   a hold of 1200 ms or more is the long-press path.
 *
 * The always-on listener (v712) is NOT governed by this machine: it is
 * xiaozhi's wake-word/auto path and keeps its own loop.  s_dlg tracks
 * only the click-opened manual session, the same layering xiaozhi uses
 * between a manually opened channel and a wake-word one.
 * ───────────────────────────────────────────────────────────── */

#define V719_CLICK_MS        1200  /* xiaozhi BUTTON_LONG_PRESS_TIME_MS */
#define V719_NO_SPEECH_MS    8000  /* give up on a silent click session */
#define V719_REPLY_WAIT_MS   20000 /* answer starts within this */
#define V719_REPLY_MAX_MS    30000 /* answer finishes within this */

enum dlg_state {
    DLG_IDLE = 0,
    DLG_LISTENING,
    DLG_THINKING,
    DLG_SPEAKING,
};

static volatile enum dlg_state s_dlg = DLG_IDLE;

/* judged on release (see the poll loop): stamp of the press edge */
static clock_t s_v719_press_tick;
static volatile int s_v719_armed;
static volatile int s_v719_long;

/* one manual session at a time */
static volatile int s_dlg_task_live;

/* [V719-5] raised by button_ptt_press() while it waits for the mic, so a
 * recording manual session hands the channel over instead of making the
 * long-press PTT wait out its 1.5 s timeout. */
static volatile int s_v719_ptt_req;

static const char *dlg_name(enum dlg_state s)
{
    switch (s) {
    case DLG_IDLE:      return "idle";
    case DLG_LISTENING: return "listening";
    case DLG_THINKING:  return "thinking";
    case DLG_SPEAKING:  return "speaking";
    default:            return "?";
    }
}

/* [V719] xiaozhi DeviceStateMachine::IsValidTransition: whitelist only.
 * Legal -> move and log; illegal -> refuse with a warning, so two racing
 * closers of one session can never both "win". */

static int dlg_transition(enum dlg_state next)
{
    int ok = 0;

    switch (s_dlg) {
    case DLG_IDLE:
        ok = (next == DLG_LISTENING);
        break;
    case DLG_LISTENING:
        ok = (next == DLG_IDLE || next == DLG_THINKING);
        break;
    case DLG_THINKING:
        ok = (next == DLG_SPEAKING || next == DLG_IDLE);
        break;
    case DLG_SPEAKING:
        ok = (next == DLG_IDLE || next == DLG_LISTENING);
        break;
    }

    if (!ok) {
        syslog(LOG_WARNING, "[%s] [V719] dlg: REJECT %s -> %s\n",
               TAG, dlg_name(s_dlg), dlg_name(next));
        return -1;
    }

    syslog(LOG_INFO, "[%s] [V719] dlg: %s -> %s\n",
           TAG, dlg_name(s_dlg), dlg_name(next));
    s_dlg = next;
    return 0;
}

/* [V719] xiaozhi OpenAudioChannel + SendStartListening(auto).  Reuses the
 * PTT preemption handshake so the always-on listener stays out for the
 * whole turn (xiaozhi keeps the channel open across the turn too), then
 * relies on the LOCAL VAD for endpointing -- the behavioural equivalent
 * of xiaozhi's server-side VAD in auto mode. */

static void dialog_manual_run(void)
{
    int v719_ms = 0;
    enum voice_state v719_st = s_voice.state;
    int v719_waited;
    int v719_waited2;

    s_ptt_preempt = 1;          /* listener stays out for the whole turn */

    while (v719_ms < 1500) {
        int v719_cap;

        pthread_mutex_lock(&s_voice.lock);
        v719_st = s_voice.state;
        v719_cap = (s_voice.cap != NULL);
        pthread_mutex_unlock(&s_voice.lock);

        if (v719_st == VOICE_IDLE && !v719_cap) {
            break;
        }

        usleep(20000);
        v719_ms += 20;
    }

    if (v719_st != VOICE_IDLE) {
        syslog(LOG_WARNING,
               "[%s] [V719] open channel: mic busy (state=%d)\n",
               TAG, (int)v719_st);

        /* [V743] this used to be a silent return: the emotion state was
         * left as-is and the screen kept whatever it showed last, which is
         * exactly what "pressing BOOT does nothing" looks like. */

        emotion_set("idle");
        return;
    }

    /* The click IS the wake word as far as the gate is concerned: this
     * turn dispatches without one (same as the PTT key). */

    s_wake_active_ts = time(NULL);
    s_wake_missed = 0;

    emotion_set("listening");   /* xiaozhi: listening bar */

    if (voice_channel_start() != 0) {
        syslog(LOG_ERR, "[%s] [V719] open channel: start failed\n", TAG);
        emotion_set("idle");
        return;
    }

    dlg_transition(DLG_LISTENING);
    syslog(LOG_INFO, "[%s] [V719] TOGGLE: channel open, listening\n", TAG);

    /* Wait for the local VAD endpoint, but never hold a silent session
     * open forever (xiaozhi's server has its own idle timeout). */

    v719_waited = 0;
    while (s_voice.state == VOICE_RECORDING && !s_voice.vad_ended &&
           v719_waited < V719_NO_SPEECH_MS) {
        usleep(50000);
        v719_waited += 50;

        /* [V719-5] the long-press PTT wants the microphone */
        if (s_v719_ptt_req) {
            break;
        }
    }

    if (s_dlg != DLG_LISTENING) {
        /* closed by a second click -- that path owns the teardown */
        return;
    }

    if (s_v719_ptt_req) {
        syslog(LOG_INFO,
               "[%s] [V719] BOOT held: handing mic to the PTT key\n", TAG);
        voice_channel_discard();
        dlg_transition(DLG_IDLE);
        return;
    }

    if (!s_voice.vad_ended) {
        /* silence: throw the session away, exactly like the v715
         * listener gate -- no ASR, no dispatch */
        syslog(LOG_INFO, "[%s] [V719] no speech in %d ms, closing\n",
               TAG, v719_waited);
        voice_channel_discard();
        dlg_transition(DLG_IDLE);
        emotion_set("idle");
        return;
    }

    dlg_transition(DLG_THINKING);

    /* ASR + dispatch; the reply comes back through voice_channel_speak()
     * on the agent thread. */

    if (voice_channel_stop() != 0) {
        /* the session was closed from under us */
        if (s_dlg == DLG_THINKING) {
            dlg_transition(DLG_IDLE);
            emotion_set("idle");
        }
        return;
    }

    if (s_dlg != DLG_THINKING) {
        /* closed by a click while we were inside voice_channel_stop() */
        return;
    }

    if (s_wake_missed) {
        /* empty transcript -- nothing was dispatched */
        dlg_transition(DLG_IDLE);
        emotion_set("idle");
        return;
    }

    /* Let the answer arrive and finish playing (xiaozhi: tts start ->
     * speaking, tts stop -> listening/idle). */

    v719_waited2 = 0;
    while (v719_waited2 < V719_REPLY_WAIT_MS) {
        int v719_pb;

        pthread_mutex_lock(&s_voice.lock);
        v719_pb = (s_voice.tts_pb != NULL);
        pthread_mutex_unlock(&s_voice.lock);

        if (v719_pb) {
            break;
        }

        usleep(50000);
        v719_waited2 += 50;
    }

    if (v719_waited2 >= V719_REPLY_WAIT_MS) {
        syslog(LOG_WARNING, "[%s] [V719] no reply within %d ms\n",
               TAG, V719_REPLY_WAIT_MS);
        dlg_transition(DLG_IDLE);
        emotion_set("idle");
        return;
    }

    dlg_transition(DLG_SPEAKING);

    v719_waited2 = 0;
    while (v719_waited2 < V719_REPLY_MAX_MS) {
        int v719_pb;

        pthread_mutex_lock(&s_voice.lock);
        v719_pb = (s_voice.tts_pb != NULL);
        pthread_mutex_unlock(&s_voice.lock);

        if (!v719_pb) {
            break;
        }

        usleep(50000);
        v719_waited2 += 50;
    }

    /* voice_channel_speak() sets the closing emotion itself */

    dlg_transition(DLG_IDLE);
    syslog(LOG_INFO, "[%s] [V719] turn complete, channel closed\n", TAG);
}

static void *dialog_manual_task(void *arg)
{
    (void)arg;
    dialog_manual_run();
    s_dlg_task_live = 0;
    s_ptt_preempt = 0;
    return NULL;
}

/* [V719] xiaozhi Application::HandleToggleChatEvent -- the three-way
 * dispatch on a BOOT click. */

static void toggle_chat_click(void)
{
    int v719_tts;

    pthread_mutex_lock(&s_voice.lock);
    v719_tts = (s_voice.tts_pb != NULL);
    pthread_mutex_unlock(&s_voice.lock);

    /* speaking -> AbortSpeaking(kAbortReasonNone) */
    if (v719_tts) {
        syslog(LOG_INFO, "[%s] [V719] TOGGLE: abort speaking\n", TAG);
        diag_report("V719_ABORT");
        pthread_mutex_lock(&s_voice.lock);
        s_voice.tts_abort = 1;
        http_tts_abort_active();   /* [TTS-ABORT] 立刻关掉旧 TTS 的 socket，防连接泄漏 */
        pthread_mutex_unlock(&s_voice.lock);
        s_speak_epoch++;           /* [SPEAK-EPOCH] 排队中的旧句一并作废 */
        return;
    }

    /* listening (manual session) -> CloseAudioChannel, no ASR */
    if (s_dlg == DLG_LISTENING) {
        syslog(LOG_INFO, "[%s] [V719] TOGGLE: close channel\n", TAG);
        diag_report("V719_CLOSE");
        if (voice_channel_discard() != 0) {
            /* the session already wound down (the VAD raced us) -- the
             * task thread finishes the teardown */
            syslog(LOG_INFO,
                   "[%s] [V719] close: session already gone\n", TAG);
            return;
        }
        dlg_transition(DLG_IDLE);
        emotion_set("idle");
        return;
    }

    if (s_dlg != DLG_IDLE) {
        /* thinking: ASR/LLM round trip in flight, nothing to abort --
         * xiaozhi has no on-device action for this state either */
        syslog(LOG_INFO, "[%s] [V719] TOGGLE ignored: state %s\n",
               TAG, dlg_name(s_dlg));
        return;
    }

    /* idle (or the listener holds the mic) -> OpenAudioChannel */
    if (__sync_lock_test_and_set(&s_dlg_task_live, 1) != 0) {
        syslog(LOG_INFO,
               "[%s] [V719] TOGGLE ignored: manual task in flight\n", TAG);
        return;
    }

    syslog(LOG_INFO, "[%s] [V719] TOGGLE: open channel\n", TAG);
    diag_report("V719_OPEN");

    {
        pthread_attr_t v719_attr;
        pthread_t v719_tid;

        pthread_attr_init(&v719_attr);
        pthread_attr_setstacksize(&v719_attr, 16384);

        if (pthread_create(&v719_tid, &v719_attr,
                           dialog_manual_task, NULL) == 0) {
            pthread_detach(v719_tid);
        } else {
            syslog(LOG_ERR,
                   "[%s] [V719] manual task create failed\n", TAG);
            s_dlg_task_live = 0;
        }

        pthread_attr_destroy(&v719_attr);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * [V748] 按钮命令队列 —— 把阻塞的设备操作搬离 20ms 轮询线程
 *
 * V747 报告 §5.4 / 需决策 D5。改造前 button_wake_task() 在自己的循环里
 * 同步调用 button_ptt_press() 与 button_ptt_release()，而这两者会阻塞：
 *
 *   press   -> voice_channel_start() -> pthread_mutex_lock(&s_voice.speak_lock)
 *              voice_channel_speak() 全程持有该锁（一次 TTS 播报，实测
 *              "speak done: total 9960ms"）；抢占等待再 <=1500ms
 *   release -> voice_channel_stop()  -> rec_join_bounded() <= 8000ms
 *
 * 一轮会话合计可达 20s：这段时间 BOOT 引脚一次都不采样，用户按下去完全
 * 没有反应，屏幕上还停在「正在思考」——这正是主诉。
 *
 * 方案：press / release 拆成两段
 *
 *   button_ptt_press() / button_ptt_release()          —— *_begin
 *       门闩检查 + 状态记账。仍在按钮线程里、仍在同一时刻置位
 *       s_ptt_hold / s_ptt_recording / s_ptt_start / s_ptt_start_tick，
 *       所以状态机与改造前逐帧一致，只是不再碰设备。
 *
 *   button_ptt_press_commit() / button_ptt_release_commit()  —— *_commit
 *       1.5s 抢占等待、voice_channel_start()、voice_channel_stop()。由下面
 *       的 button_cmd_worker 线程串行执行（FIFO，press 一定先于 release，
 *       与改造前的调用顺序一致）。
 *
 * 结果：按钮线程的采样周期恒为 20ms，不再受网络、TTS、采集线程 join
 * 的任何影响。
 * ══════════════════════════════════════════════════════════════════════ */

#define BTN_CMDQ_DEPTH 16

enum btn_cmd_e {
    BTN_CMD_NONE    = 0,
    BTN_CMD_PRESS   = 1,   /* run button_ptt_press_commit()   */
    BTN_CMD_RELEASE = 2    /* run button_ptt_release_commit() */
};

static volatile int s_btnq_cmd[BTN_CMDQ_DEPTH];
static volatile int s_btnq_head;
static volatile int s_btnq_tail;
static volatile int s_btnq_lock;    /* SPSC spinlock: 按钮线程写, worker 读 */
static volatile int s_btnq_busy;    /* 队列中 + 正在执行的命令条数 */
static volatile int s_btnq_worker_live;
static sem_t s_btnq_sem;

/* 计量：post 投递数 / run 执行数 / drop 丢弃数 */
static unsigned long g748_post;
static unsigned long g748_run;
static unsigned long g748_drop;

static void button_ptt_press_commit(void);
static void button_ptt_release_commit(void);

/* 投递一条命令。队尾已是同一条命令时合并（pad 抖动不会灌满队列）；队列满
 * 则丢弃并计数 —— 只有在工作线程被卡住 16 条以上时才可能，[V748] 心跳里
 * 的 drop 会把它暴露出来。 */
static void btn_cmd_post(int cmd)
{
    int t;

    if (!s_btnq_worker_live) {
        /* 工作线程还没起来（开机极早期）：退回内联执行，行为与 v747 相同 */
        if (cmd == BTN_CMD_PRESS) {
            button_ptt_press_commit();
        } else if (cmd == BTN_CMD_RELEASE) {
            button_ptt_release_commit();
        }
        return;
    }

    while (__sync_lock_test_and_set(&s_btnq_lock, 1) != 0) {
        usleep(1000);
    }

    t = s_btnq_tail;

    if (t != s_btnq_head &&
        s_btnq_cmd[(t - 1 + BTN_CMDQ_DEPTH) % BTN_CMDQ_DEPTH] == cmd) {
        s_btnq_lock = 0;          /* 队尾已是同一条命令 —— 合并 */
        return;
    }

    if (((t + 1) % BTN_CMDQ_DEPTH) == s_btnq_head) {
        s_btnq_lock = 0;
        g748_drop++;
        V1024S(LOG_WARNING, "[V748]] cmdq full -- dropped cmd=%d\n", cmd);
        return;
    }

    s_btnq_cmd[t] = cmd;
    s_btnq_tail = (t + 1) % BTN_CMDQ_DEPTH;
    s_btnq_busy++;
    s_btnq_lock = 0;

    g748_post++;
    sem_post(&s_btnq_sem);
}

static void* button_cmd_worker(void* arg)
{
    (void)arg;

    for (;;) {
        int cmd = BTN_CMD_NONE;
        int h;

        while (sem_wait(&s_btnq_sem) != 0) {
            if (errno != EINTR) {
                break;
            }
        }

        while (__sync_lock_test_and_set(&s_btnq_lock, 1) != 0) {
            usleep(1000);
        }

        h = s_btnq_head;
        if (h != s_btnq_tail) {
            cmd = s_btnq_cmd[h];
            s_btnq_cmd[h] = BTN_CMD_NONE;
            s_btnq_head = (h + 1) % BTN_CMDQ_DEPTH;
        }
        s_btnq_lock = 0;

        if (cmd == BTN_CMD_NONE) {
            continue;
        }

        g748_run++;

        if (cmd == BTN_CMD_PRESS) {
            button_ptt_press_commit();
        } else if (cmd == BTN_CMD_RELEASE) {
            button_ptt_release_commit();
        }

        s_btnq_busy--;
    }

    return NULL;
}


static void button_ptt_press(void)
{
    /* [V715] Entry line.  Every rejection path below logs too, but a line
     * can still be lost to console contention, and "did the pad see the
     * press at all?" is the one question the field runs have not answered.
     * One line per press is cheap. */

    syslog(LOG_INFO, "[%s] BUTTON PTT press (enter)\n", TAG);

    /* [V688] Hard one-session-at-a-time gate, INSIDE the handler.
     *
     * v687 enforced this in the poll loop.  The v687 field run still shows
     * three capture sessions for one press (each with its own full ES8311
     * train at 322.050 / 324.728 / 328.909) and three
     * "BUTTON PTT release -> thinking" 0.9 s apart, which a single poller
     * blocked inside voice_channel_stop_with_text() cannot emit.  Uphold
     * the invariant here instead: the first caller latches s_ptt_hold and
     * every later caller bails out before it touches the codec, no matter
     * how many pollers reach this function or how the pad dithers. */

    if (s_ptt_recording || s_ptt_hold) {
        syslog(LOG_INFO,
               "[%s] BUTTON PTT press ignored: session already live\n", TAG);
        return;
    }

    /* [V689] Stamp the cooldown at the START as well as at the end, and do
     * it in ticks.  v688 still opened two sessions for one press (35.301 /
     * 35.553 in _v688_press.log): s_ptt_evt was still 0 on the first press,
     * so the check was skipped entirely, and time(NULL) only has 1-second
     * resolution anyway.  Now no two sessions may begin within
     * VOICE_PTT_COOLDOWN_SEC of each other, which is precisely the
     * "one press = one session" invariant. */

    if (s_ptt_evt_ticks != 0 &&
        (clock_systime_ticks() - s_ptt_evt_ticks) <
        (clock_t)(VOICE_PTT_COOLDOWN_SEC * 100)) {
        syslog(LOG_INFO,
               "[%s] BUTTON PTT press ignored: %ldms cooldown (< %dms)\n",
               TAG,
               (long)((clock_systime_ticks() - s_ptt_evt_ticks) * 10),
               VOICE_PTT_COOLDOWN_SEC * 1000);
        return;
    }

    /* [V712] Ask the always-on listener for the microphone.  It holds the
     * channel in VOICE_RECORDING for most of every duty cycle, so without
     * this voice_channel_start() below returns -EBUSY and the key degrades
     * into the canned button_wake_trigger() greeting. */


    /* [V748] 同步占位。s_ptt_hold / s_ptt_recording 必须在这里、在按钮
     * 线程里立刻置位：否则工作线程还没跑到 commit 的那几十毫秒里，轮询
     * 循环会看到「没有会话」而再投一条 press。置位时刻与改造前完全
     * 相同，状态机时序不变，只是不再碰设备。 */
    s_ptt_hold = 1;
    s_wake_active_ts = time(NULL);
    emotion_set("listening");   /* 屏显 开始聆听... */
    s_ptt_recording = true;
    s_ptt_start = time(NULL);
    s_ptt_start_tick = clock_systime_ticks();   /* [V700] */

    /* [V748] 从此往下全部是阻塞操作 —— 交给工作线程。 */
    btn_cmd_post(BTN_CMD_PRESS);
}

static void button_ptt_press_commit(void)
{
    {
        int ms = 0;
        enum voice_state st = s_voice.state;

        s_ptt_preempt = 1;
        s_v719_ptt_req = 1;      /* [V719] manual session hands over */

        while (ms < 1500) {
            int cap_open;

            pthread_mutex_lock(&s_voice.lock);
            st = s_voice.state;
            cap_open = (s_voice.cap != NULL);
            pthread_mutex_unlock(&s_voice.lock);

            /* [V713] Take the microphone only once the previous session has
             * really closed the capture device. */

            if (st == VOICE_IDLE && !cap_open) {
                break;
            }

            usleep(20000);
            ms += 20;
        }

        syslog(LOG_INFO, "[%s] PTT preempt: waited %dms (state=%d)\n",
               TAG, ms, (int)st);
    }

    s_ptt_hold = 1;
    s_ptt_evt_ticks = clock_systime_ticks();

    syslog(LOG_INFO, "[%s] BUTTON PTT press -> listen\n", TAG);
    diag_report("BTN_PTT_PRESS");

    /* keep the wake-window semantics so follow-up speech bypasses the gate */
    s_wake_active_ts = time(NULL);

    emotion_set("listening");   /* 屏显 开始聆听... */

    /* [V686] Claim the session BEFORE voice_channel_start(), which takes a
     * few hundred milliseconds (device open + AUDIOIOC_CONFIGURE + the whole
     * ES8311 train).  Without this, concurrent callers all saw
     * s_ptt_recording == false in that window and each started its own
     * capture, killing the one that was already running. */

    s_ptt_recording = true;
    s_ptt_start = time(NULL);
    s_ptt_start_tick = clock_systime_ticks();   /* [V700] */

    int rc = voice_channel_start();

    /* [V712] The listener may look after the microphone again from here on.
     * On success it stays out anyway (the channel is no longer IDLE); on
     * failure this restores the pre-V712 behaviour exactly. */

    s_ptt_preempt = 0;
    s_v719_ptt_req = 0;         /* [V719] request served or withdrawn */

    if (rc != 0) {
        syslog(LOG_ERR,
               "[%s] PTT voice_channel_start failed: %d -- fallback to wake\n",
               TAG, rc);
        s_ptt_recording = false;
        s_ptt_start = 0;
        /* [V687] Drop the session hold and start the cooldown HERE so the
         * same still-held button cannot retry on the next poll: v686 logged
         * "BUTTON PTT press -> listen" at 144.550 and again 190 ms later,
         * followed by a second full capture start. */
        s_ptt_hold = 0;
        s_ptt_evt_ticks = clock_systime_ticks();
        emotion_set("thinking");
        button_wake_trigger();
        return;
    }
}

/* [V687] The body of a release, split out so the session hold can be
 * lifted on EVERY exit path (stop failure, empty ASR, message pushed). */

static void button_ptt_process(void)
{
    emotion_set("thinking");   /* 屏显 正在思考... */

    /* [V746] This used to call voice_channel_stop_with_text(), which runs
     * the whole HTTPS recognition inline -- on THIS thread, the button
     * poller.  A recognition legitimately costs 25 s when the backend is
     * slow (measured: "asr_rsp +25850ms"), and for that entire time the
     * poller is not in its loop: the BOOT pad is not sampled, so every
     * press during the window is simply lost.  That is the field report
     * "pressing BOOT does nothing and the screen sits on thinking".
     *
     * voice_channel_stop() performs the identical batch fallback
     * asynchronously: it packs the PCM into a heap block and hands it to
     * asr_and_dispatch() on its own 24 KB thread, which runs the
     * recognition, applies the wake gate and pushes the transcript into
     * the message bus -- exactly what the code below used to do by hand.
     * The poller is therefore only here for microseconds and the key keeps
     * working while audio is being recognised. */

    int rc = voice_channel_stop();

    if (rc != 0) {
        syslog(LOG_ERR, "[%s] PTT stop failed: %d\n", TAG, rc);
        emotion_set("idle");
        return;
    }
}

static void button_ptt_release(void)
{
    syslog(LOG_INFO, "[%s] BUTTON PTT release -> thinking\n", TAG);
    diag_report("BTN_PTT_RELEASE");
    s_p1_tick = clock_systime_ticks();             /* [V705] latency t0 */
    syslog(LOG_INFO, "[%s] [P1] t0 release\n", TAG);

    if (!s_ptt_recording) {
        return;   /* fallback path already handled the press */
    }

    s_ptt_recording = false;


    /* [V748] button_ptt_process() -> voice_channel_stop() 里的
     * rec_join_bounded() 最长 8s，交给工作线程。 */
    btn_cmd_post(BTN_CMD_RELEASE);
}

static void button_ptt_release_commit(void)
{
    button_ptt_process();

    /* [V687] The session is over: drop the hold and start the cooldown so
     * the dithering pad cannot open a fresh capture on top of it. */

    s_ptt_hold = 0;
    s_ptt_evt_ticks = clock_systime_ticks();
}

/* [V660] 按键采样：短突发 + 多数表决。
 *
 * v658 实测（单次 300 s）：23 次按下沿、0 次松手沿；
 * BTN_LEVEL 打出来的唯一一种形态就是 committed=1 stable=0
 * —— 旧代码要求的「连续 4 个 poll 读回 HIGH」从未成立。
 * 这说明 pad 在休眠时也在快速抖动，单次采样根本不可信。
 *
 * 把每次 poll 变成一个 700 us 的短突发取多数，对人手按压
 * 几乎没有后果（-> 人手的压压不会短于几十毫秒），却能把抖动过滤掉。
 *
 * 必须用 up_udelay：CONFIG_USEC_PER_TICK=10000，usleep() 分辨不了
 * 10 ms 以下的间隔。 */

/* [V696] Polarity-agnostic, floating-tolerant key sampling.
 *
 * Every software layer for these pads is correct (pin numbers match the
 * vendor README; 0x09 == INPUT(1<<0)|PULLUP(1<<3); the default
 * esp32s3_configgpio() branch writes MCU_SEL = PIN_FUNC_GPIO(1) = plain
 * GPIO; the IO_MUX address (pin+1)*4 lands on PERIPHS_IO_MUX_GPIO45_U
 * (BASE+0xb8) / GPIO46_U (BASE+0xbc); esp32s3_gpioread() reads
 * GPIO_IN1 bit (pin-32)).  Yet no press was ever seen, on this firmware or
 * on the vendor's ESP-IDF build, and both report the same idle level:
 *
 *     gpio_get_level 读到的空闲电平是对的(=1)
 *
 * GPIO45 and GPIO46 are strapping pins whose reset default MUST be 0
 * (GPIO46 = SPI boot mode, GPIO45 = VDD_SPI 3.3 V; ESP32-S3 datasheet
 * Table 3-1/3-3/3-4: both default to weak pull-DOWN), so the board cannot
 * fit external pull-ups on them -- that would break booting.  An A/B key
 * that leaks to VCC is therefore ruled out; the keys must be ACTIVE HIGH
 * (short the pad to VCC when pressed, idle held at 0 by the internal weak
 * pull-down).  Our code enabled INPUT|PULLUP (0x09), which pins the idle
 * level at 1, so a press -- also 1 -- can never be told apart from idle.
 * That is exactly what we measured: idle = 1 on A(46)/B(45) and chg = 0
 * over seven captures, while BOOT(0) did move (lo 69 / chg 2 in v693).
 *
 * Sampling with an internal pull-up and testing for LOW can never see an
 * active-high key, which is also why the vendor's own iot_button build
 * failed: xiaozhi's Button() defaults to active_level = 0.
 *
 * So calibrate each pad's idle level under BOTH pull configurations and
 * report "pressed" when EITHER configuration deviates from its own idle
 * reading.  Correct for all four wirings (active high/low x hard external
 * pull / floating pad), and it needs no hardware knowledge.
 */

#define BTN_ATTR_INPUT_PU   0x09    /* INPUT | PULLUP   */
#define BTN_ATTR_INPUT_PD   0x11    /* INPUT | PULLDOWN */

#define BTN_V696_KEYS       3

static const int   s_v696_pins[BTN_V696_KEYS]    = { 0, 46, 45 };
static const char *s_v696_names[BTN_V696_KEYS]   = { "BOOT", "A", "B" };
static int  s_v696_ref_pu[BTN_V696_KEYS]  = { -1, -1, -1 };
static int  s_v696_ref_pd[BTN_V696_KEYS]  = { -1, -1, -1 };
static int  s_v696_last_pu[BTN_V696_KEYS];
static int  s_v696_last_pd[BTN_V696_KEYS];
static int  s_v696_prev[BTN_V696_KEYS];
static unsigned long s_v696_hits[BTN_V696_KEYS];
static unsigned long s_v696_edges[BTN_V696_KEYS];
static int  s_v696_calibrated;

static int btn_v696_majority(int pin, int attr)
{
    int i;
    int ones = 0;

    for (i = 0; i < 8; i++)
      {
        esp32s3_configgpio(pin, (uint16_t)attr);
        up_udelay(60);
        if (esp32s3_gpioread(pin))
          {
            ones++;
          }
      }

    return (ones >= 4) ? 1 : 0;
}

static void btn_v696_calibrate(void)
{
    int k;

    for (k = 0; k < BTN_V696_KEYS; k++)
      {
        s_v696_ref_pu[k] = btn_v696_majority(s_v696_pins[k],
                                             BTN_ATTR_INPUT_PU);
        s_v696_ref_pd[k] = btn_v696_majority(s_v696_pins[k],
                                             BTN_ATTR_INPUT_PD);
        s_v696_prev[k]   = 0;

        syslog(LOG_INFO,
               "[%s] [V696] key %s(pin %d) idle: pullup=%d pulldown=%d\n",
               TAG, s_v696_names[k], s_v696_pins[k],
               s_v696_ref_pu[k], s_v696_ref_pd[k]);
      }

    s_v696_calibrated = 1;
}

static int btn_v696_key_index(int pin)
{
    int k;

    for (k = 0; k < BTN_V696_KEYS; k++)
      {
        if (s_v696_pins[k] == pin)
          {
            return k;
          }
      }

    return -1;
}

static int btn_pin_burst(int pin)
{
    int k = btn_v696_key_index(pin);
    int i;
    int hit = 0;
    int state;

    if (k < 0)
      {
        return 0;
      }

    if (!s_v696_calibrated)
      {
        btn_v696_calibrate();
      }

    for (i = 0; i < 4; i++)
      {
        int pu;
        int pd;

        esp32s3_configgpio(pin, BTN_ATTR_INPUT_PU);
        up_udelay(60);
        pu = esp32s3_gpioread(pin) ? 1 : 0;

        esp32s3_configgpio(pin, BTN_ATTR_INPUT_PD);
        up_udelay(60);
        pd = esp32s3_gpioread(pin) ? 1 : 0;

        s_v696_last_pu[k] = pu;
        s_v696_last_pd[k] = pd;

        if (pu != s_v696_ref_pu[k] || pd != s_v696_ref_pd[k])
          {
            hit++;
          }
      }

    if (hit > 0)
      {
        s_v696_hits[k]++;
      }

    state = (hit >= 2) ? 1 : 0;

    if (state != s_v696_prev[k])
      {
        s_v696_prev[k] = state;
        s_v696_edges[k]++;
      }

    return state;
}

static int btn_v660_burst(void)
{
    return btn_pin_burst(0);
}

/* [V690] Volume keys -- ported from the official nulllab-ai-vox-v3 board.
 *
 *   config.h:36  VOLUME_UP_BUTTON_GPIO   GPIO_NUM_46
 *   config.h:37  VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_45
 *   config.h:39  VOLUME_STEP             5      (user asked for 5)
 *
 * The official ButtonPollTask() runs on a 20 ms vTaskDelayUntil() loop
 * because iot_button cannot see edges on those two strapping pads; the
 * xiaozhi port instead registers a callback.  We reuse the same burst +
 * majority vote that made the BOOT pad readable here (see btn_pin_burst):
 * a single sample of a dithering pad is worthless, 7 samples over 700 us
 * are not.
 *
 * Semantics, matching the official board:
 *   click                -> volume += VOL_STEP (up) / -= VOL_STEP (down)
 *   hold >= 1200 ms      -> jump straight to 100 (up) / 0 = mute (down)
 * Volume is kept on a 0..100 scale and handed to the codec as vol * 10,
 * which is the AUDIO_FU_VOLUME range es8311_setvolume() expects.
 */

#define VOL_GPIO_UP     46
#define VOL_GPIO_DOWN   45
#define VOL_STEP        5
#define VOL_MAX         100
#define VOL_LONG_MS     1200
#define VOL_TICK_MS     20      /* poll period */

static void vol_apply(int vol)
{
    if (vol < 0)
      {
        vol = 0;
      }
    else if (vol > VOL_MAX)
      {
        vol = VOL_MAX;
      }

    syslog(LOG_INFO, "[%s] VOLUME %d/100\n", TAG, vol);
    es8311_ext_setvolume((uint16_t)(vol * 10));
}

static void* button_wake_task(void* arg)
{
    (void)arg;
    /* 0x8D decoded = INPUT|PULLUP|FUNCTION_2 -> the pin was routed to IO-MUX
     * function 2, so GPIO_IN_REG did NOT track the pad and the BOOT
     * press was never seen.  0x09 = INPUT|PULLUP (plain GPIO0). */
    esp32s3_configgpio(0, 0x09);  /* INPUT | PULLUP, IO-MUX function 0 */
    esp32s3_configgpio(VOL_GPIO_UP,   0x09);   /* [V690] GPIO46, INPUT|PULLUP */
    esp32s3_configgpio(VOL_GPIO_DOWN, 0x09);   /* [V690] GPIO45, INPUT|PULLUP */
    int btn_prev = 0;    /* [V660] previous *committed* state, not the raw level */

    /* [V690] volume key state (button_wake_task is the only writer) */
    /* [CALM-VOL 2026-10-27] 40 -> 70：本地播报走 es8311_ext_setvolume(s_vol*10)，
     * 即 400/1000 = 40%；而云端播报在 audio_output_engine.cpp 里是 set_volume(70)
     * = 70%。两条路音量差近 5 dB，用户听到的就是"一会儿大声一会儿小声"。
     * 统一到 70（也是上游 xiaozhi 的默认值）。 */
    static int  s_vol = 70;      /* 0..100; 70 = 700/1000，与云端一致 */
    static int  vol_up_prev = 0, vol_dn_prev = 0;
    static int  vol_up_deb = 0,  vol_dn_deb = 0;
    static long vol_up_t0 = 0,   vol_dn_t0 = 0;
    static int  vol_seeded = 0;

    /* [V686] Only ONE button task may poll the pad.
     *
     * voice_channel_init() has three call sites (agent_main.c P3-early,
     * agent_main.c P3 and mini_memo_core.c) and the v685b field run logged
     * "button wake task started" three times.  Each task keeps its OWN
     * btn_prev while sharing s_ptt_recording / s_ptt_start / s_btn_level,
     * so one physical press produced three "BTN_PRESS -> PTT" events and
     * every extra press tore the running capture down and started a new
     * one.  That is why every session ended after 3..5 buffers with
     * "recording thread exit: 5 chunks, 5616 read" / "capture read -32",
     * and why the BOOT key looked dead.
     */

    {
        static volatile int s_btn_task_live;

        if (__sync_lock_test_and_set(&s_btn_task_live, 1) != 0)
          {
            syslog(LOG_ERR,
                   "[%s] duplicate button task -- exiting (only one polls)\n",
                   TAG);
            return NULL;
          }
    }

    syslog(LOG_INFO, "[%s] button wake task started (BOOT=GPIO0)\n", TAG);
    int bw_tick = 0;

    for (;;) {
        /* [V660] Debounce with hysteresis on the COMMITTED state.
         *
         * The pad dithers, so:
         *   - each poll is a majority vote over a 700 us burst (btn_v660_burst),
         *     which is far shorter than any human press but long enough to
         *     outvote the dither;
         *   - a level change must repeat over 2 polls (40 ms) before it is
         *     accepted, on BOTH edges -- release included.  The old code
         *     accepted a press on a single LOW sample and then demanded 4
         *     consecutive HIGH samples to release, which v658 showed never
         *     happens;
         *   - btn_prev tracks the committed state (the old code tracked the
         *     raw level, so one stray HIGH re-armed the press edge and 18 of
         *     23 presses were logged as "already recording").
         */
        int level     = btn_v660_burst();       /* 1 = pressed (active LOW) */
        int committed = s_btn_level;

        /* [V691] Raw, single-sample pad truth for BOOT(0) / A(46) / B(45).
         *
         * The official board reached the same conclusion we did
         * (ai_vox3_board.cc:188-200): iot_button sees no edge on these
         * pads, "boot 也无法唤醒", and a plain 20 ms gpio_get_level() poll
         * is what actually works -- so one sample is the reference.
         *
         * Counters go to /tmp/btn.txt, not just to kmsg: the 32 KB ring
         * drops more than it keeps, and this is what finally separates
         * "the pad never went LOW" from "the debounce ate the edge". */

        {
            static unsigned long v691_polls;
            static unsigned long v691_bootlo;
            static unsigned long v691_alo;
            static unsigned long v691_blo;
            static unsigned long v691_chg0;
            static unsigned long v691_chg46;
            static unsigned long v691_chg45;
            static int v691_last0  = -1;
            static int v691_last46 = -1;
            static int v691_last45 = -1;
            static int v691_seeded;
            int r0;
            int r46;
            int r45;
            /* [V799] Report every BOOT pad level change immediately.
             *
             * The counters in the periodic summary are averages over 250 polls,
             * and the raw level was only printed once at idle.  If the pad never
             * leaves its idle level, that is indistinguishable from the poll
             * never running -- which is exactly the ambiguity that has kept this
             * key unresolved across several revisions.  A line per transition
             * settles it: no lines means the pad is static, lines that all read
             * the idle value mean the pin is stuck, and a line carrying the
             * other value means the key works and the comparison is at fault. */
            static int v799_last_boot = -1;
            int v799_raw;

            if (!v691_seeded)
              {
                v691_seeded = 1;
                r0  = esp32s3_gpioread(0) ? 0 : 1;
                r46 = esp32s3_gpioread(VOL_GPIO_UP) ? 0 : 1;
                r45 = esp32s3_gpioread(VOL_GPIO_DOWN) ? 0 : 1;
                syslog(LOG_INFO,
                       "[%s] [V691] btn idle raw BOOT(%d)=%d A(%d)=%d "
                       "B(%d)=%d (1 = pressed/LOW)\n",
                       TAG, 0, r0, VOL_GPIO_UP, r46, VOL_GPIO_DOWN, r45);
              }

            r0  = esp32s3_gpioread(0) ? 0 : 1;
            r46 = esp32s3_gpioread(VOL_GPIO_UP) ? 0 : 1;
            r45 = esp32s3_gpioread(VOL_GPIO_DOWN) ? 0 : 1;

            /* [V799] the pad level itself, not the inverted "pressed" value */
            v799_raw = esp32s3_gpioread(0) ? 1 : 0;

            if (v799_last_boot >= 0 && v799_raw != v799_last_boot)
              {
                syslog(LOG_ERR,
                       "[%s] [V799] BOOT pad %d -> %d  (attr now 0x%x)\n",
                       TAG, v799_last_boot, v799_raw,
                       (unsigned)0x11);
              }

            v799_last_boot = v799_raw;

            v691_polls++;
            if (r0 != 0)  { v691_bootlo++; }
            if (r46 != 0) { v691_alo++; }
            if (r45 != 0) { v691_blo++; }

            if (v691_last0 < 0)  { v691_last0 = r0; }
            if (v691_last46 < 0) { v691_last46 = r46; }
            if (v691_last45 < 0) { v691_last45 = r45; }

            if (r0 != v691_last0)
              {
                v691_last0 = r0;
                v691_chg0++;
              }
            if (r46 != v691_last46)
              {
                v691_last46 = r46;
                v691_chg46++;
              }
            if (r45 != v691_last45)
              {
                v691_last45 = r45;
                v691_chg45++;
              }

            if ((v691_polls % 100) == 0)
              {
                FILE *v691f = fopen("/tmp/btn.txt", "w");

                if (v691f != NULL)
                  {
                    fprintf(v691f,
                            "polls=%lu boot_lo=%lu a_lo=%lu b_lo=%lu "
                            "chg=%lu/%lu/%lu raw=%d/%d/%d rec=%d hold=%d\n",
                            v691_polls, v691_bootlo, v691_alo, v691_blo,
                            v691_chg0, v691_chg46, v691_chg45,
                            r0, r46, r45,
                            (int)s_ptt_recording, (int)s_ptt_hold);
                    fclose(v691f);
                  }
              }

            if ((v691_polls % 250) == 0)
              {
                syslog(LOG_INFO,
                       "[%s] [V691] btn alive raw BOOT=%d A=%d B=%d "
                       "(lo %lu/%lu/%lu chg %lu/%lu/%lu)\n",
                       TAG, r0, r46, r45,
                       v691_bootlo, v691_alo, v691_blo,
                       v691_chg0, v691_chg46, v691_chg45);
              }

            if ((v691_polls % 250) == 7)
              {
                syslog(LOG_INFO,
                       "[%s] [V691] btn summary %lu/%lu/%lu "
                       "chg %lu/%lu/%lu\n",
                       TAG, v691_bootlo, v691_alo, v691_blo,
                       v691_chg0, v691_chg46, v691_chg45);
              }
        }

        /* [V696] The line that settles it.  "pu"/"pd" are the raw levels
         * under the internal pull-up / pull-down, "hit" counts samples that
         * deviated from the calibrated idle reading and "edge" counts
         * pressed/released transitions.  A press MUST move one of them. */

        /* [V714] Edge-driven instead of every 25 ticks.  The 500 ms
         * heartbeat printed ~2 lines/s forever, and together with the [V692]
         * probe it made the console unreadable.  A key event moves one of
         * the hit/edge counters, so printing on change still reports a press
         * within one 20 ms poll, while an idle board stays quiet apart from
         * a 10 s heartbeat. */

        {
          static unsigned long s_v714_kseen;
          unsigned long v714_know = s_v696_hits[0] + s_v696_hits[1] +
                                    s_v696_hits[2] + s_v696_edges[0] +
                                    s_v696_edges[1] + s_v696_edges[2];

          if (v714_know != s_v714_kseen || (bw_tick % 500) == 0)
            {
              s_v714_kseen = v714_know;
              syslog(LOG_INFO,
                   "[%s] [V696] keys pu/pd/hit/edge  BOOT %d/%d/%lu/%lu "
                   "A %d/%d/%lu/%lu B %d/%d/%lu/%lu\n",
                   TAG,
                   s_v696_last_pu[0], s_v696_last_pd[0],
                   s_v696_hits[0], s_v696_edges[0],
                   s_v696_last_pu[1], s_v696_last_pd[1],
                   s_v696_hits[1], s_v696_edges[1],
                   s_v696_last_pu[2], s_v696_last_pd[2],
                   s_v696_hits[2], s_v696_edges[2]);
            }
        }

        /* [V696] On-demand pad scanner.  If the keys are not on 0/45/46,
         * this finds them in one run instead of guesswork:
         *
         *     nsh> echo 1 > /tmp/gpioscan.txt
         *     nsh> cat /tmp/gpioscan_out.txt
         *
         * Only pads we never drive are touched, and never 19/20 (USB) or
         * 26-37 (flash / octal PSRAM).  Both pull configurations are
         * reported, so the pin that changes when a key is held is obvious. */

        if ((bw_tick % 25) == 12)
          {
            FILE *v696gf = fopen("/tmp/gpioscan.txt", "r");

            if (v696gf != NULL)
              {
                int v696c = fgetc(v696gf);

                fclose(v696gf);

                if (v696c != EOF)
                  {
                    static const int v696scan[] =
                      { 0, 5, 6, 43, 44, 45, 46, 47, 48 };
                    FILE *v696of = fopen("/tmp/gpioscan_out.txt", "w");

                    if (v696of != NULL)
                      {
                        unsigned int v696i;

                        for (v696i = 0;
                             v696i < sizeof(v696scan) / sizeof(v696scan[0]);
                             v696i++)
                          {
                            int v696p = v696scan[v696i];

                            fprintf(v696of, "%d %d %d\n", v696p,
                                    btn_v696_majority(v696p,
                                                      BTN_ATTR_INPUT_PU),
                                    btn_v696_majority(v696p,
                                                      BTN_ATTR_INPUT_PD));
                          }

                        fprintf(v696of, "keys %d/%d/%d pins %d/%d/%d\n",
                                s_v696_last_pu[0], s_v696_last_pd[0], 0,
                                s_v696_pins[0], s_v696_pins[1],
                                s_v696_pins[2]);

                        fclose(v696of);
                      }

                    /* Empty the trigger so the scan runs exactly once. */

                    {
                      FILE *v696tf = fopen("/tmp/gpioscan.txt", "w");

                      if (v696tf != NULL)
                        {
                          fclose(v696tf);
                        }
                    }

                    syslog(LOG_INFO, "[%s] [V696] pad scan written\n", TAG);
                  }
              }
          }

        if (level != s_btn_level) {
            if (s_btn_stable < 255) { ++s_btn_stable; }
            if (s_btn_stable >= 4) {            /* [V686] 4 bursts = 80 ms */
                s_btn_level  = level;
                s_btn_stable = 0;
                committed    = level;
            }
        } else {
            s_btn_stable = 0;
        }

        /* [V687] Latch the edge BEFORE acting on it.
         *
         * v686 logged "BUTTON PTT press -> listen" at 144.550 and again at
         * 144.740 - the very same physical press, 190 ms apart, and BOTH
         * started a capture (bounce slots 0x3c23dbe8 vs 0x3c224608, so two
         * real runs, not a kmsg replay).
         *
         * Why: button_ptt_press() blocks for the whole device open +
         * AUDIOIOC_CONFIGURE + ES8311 train, but btn_prev was only written at
         * the bottom of the loop, so the still-held button looked like a
         * fresh edge on the next poll.  Whenever the action left
         * s_ptt_recording false (the voice_channel_start() rollback) that
         * second visit opened ANOTHER capture on top of the one that was
         * already streaming; the two then fought over the same RX DMA and
         * every session died with "recording thread exit: 6 chunks, 9208
         * read".  Three such sessions were logged for one press.
         *
         * Latch first, act second, and serialize the session itself. */

        {
            int edge_up   = (committed && !btn_prev);
            int edge_down = (!committed && btn_prev);

            btn_prev = committed;

            if (edge_up || edge_down) {
                syslog(LOG_INFO, "[%s] BTN_EDGE %s (level=%d stable=%d)\n",
                       TAG, committed ? "PRESS" : "RELEASE", level,
                       s_btn_stable);
            }

            if (edge_up) {
                /* [V719] xiaozhi ToggleChatState: a short click is judged
                 * on RELEASE, never here.  Arm the stamp only; the poll
                 * loop below fires the legacy PTT once the hold reaches
                 * V719_CLICK_MS (xiaozhi BUTTON_LONG_PRESS_TIME_MS). */

                s_v719_press_tick = clock_systime_ticks();
                s_v719_armed = 1;
                s_v719_long = 0;
            }

            if (s_v719_armed && !s_v719_long && committed &&
                (long)((clock_systime_ticks() - s_v719_press_tick) * 10)
                    >= V719_CLICK_MS) {
                s_v719_armed = 0;
                s_v719_long = 1;
                syslog(LOG_INFO, "[%s] BTN_HOLD -> PTT (legacy path)\n",
                       TAG);

                /* 1 = start, 2 = stop, 3 = too short, 4 = session in flight,
                 * 5 = cooldown, 6 = another decider holds the lock. */

                int act = 6;

                if (__sync_lock_test_and_set(&s_ptt_lock, 1) == 0) {
                    long el =
                        s_ptt_start ? (long)(time(NULL) - s_ptt_start) : 0;
                    long since_ms = s_ptt_evt_ticks ?
                        (long)((clock_systime_ticks() - s_ptt_evt_ticks) * 10)
                        : 1000000L;

                    if (s_ptt_recording) {
                        /* [V660] Release edges are unreliable on this pad, so
                         * a second press after the minimum utterance is the
                         * natural "stop talking". */

                        act = (el >= VOICE_PTT_MIN_SEC) ? 2 : 3;
                    } else if (s_ptt_hold) {
                        act = 4;
                    } else if (since_ms <
                               (long)(VOICE_PTT_COOLDOWN_SEC * 1000)) {
                        act = 5;
                    } else {
                        /* [V688] button_ptt_press() latches the hold
                         * itself, so it stays correct even when this
                         * latch is not the one that wins the race. */

                        act = 1;
                    }

                    s_ptt_lock = 0;
                }

                switch (act) {
                case 1:
                    syslog(LOG_INFO, "[%s] BTN_PRESS -> PTT (level=%d)\n",
                           TAG, level);
                    button_ptt_press();
                    break;

                case 2:
                    syslog(LOG_INFO,
                           "[%s] BTN_PRESS -> stop (%lds, release edge missed)\n",
                           TAG,
                           s_ptt_start ? (long)(time(NULL) - s_ptt_start) : 0);
                    button_ptt_release();
                    break;

                case 3:
                    syslog(LOG_INFO,
                           "[%s] BTN_PRESS ignored: only %lds (< %ds)\n",
                           TAG,
                           s_ptt_start ? (long)(time(NULL) - s_ptt_start) : 0,
                           VOICE_PTT_MIN_SEC);
                    break;

                case 4:
                    syslog(LOG_INFO,
                           "[%s] BTN_PRESS ignored: previous session still running\n",
                           TAG);
                    break;

                case 5:
                    syslog(LOG_INFO,
                           "[%s] BTN_PRESS ignored: %ldms cooldown (< %dms)\n",
                           TAG,
                           s_ptt_evt_ticks ?
                             (long)((clock_systime_ticks() -
                                     s_ptt_evt_ticks) * 10) : -1L,
                           VOICE_PTT_COOLDOWN_SEC * 1000);
                    break;

                default:
                    syslog(LOG_INFO, "[%s] BTN_PRESS ignored: busy\n", TAG);
                    break;
                }
            } else if (edge_down) {
                /* [V719] Release before the long-press threshold = a
                 * CLICK: xiaozhi ToggleChatState three-way dispatch
                 * (open channel / close channel without ASR / abort
                 * speaking). */

                if (s_v719_armed) {
                    long v719_held = (long)((clock_systime_ticks() -
                                             s_v719_press_tick) * 10);

                    s_v719_armed = 0;
                    syslog(LOG_INFO,
                           "[%s] [V719] click (%ldms) -> toggle\n",
                           TAG, v719_held);
                    toggle_chat_click();
                } else if (s_v719_long) {

                /* [V700] Judge the recording on the 10 ms tick, not on
                 * time(NULL): a 1.9 s press used to read as "0s". */

                long el_ms = s_ptt_start_tick
                    ? (long)((clock_systime_ticks() - s_ptt_start_tick) * 10)
                    : 0;

                if (!s_ptt_recording) {
                    /* nothing in flight -- this edge is just the pad */
                } else if (el_ms < (long)(VOICE_PTT_MIN_SEC * 1000)) {
                    /* too short to hold any speech -- keep recording */
                    syslog(LOG_INFO,
                           "[%s] release ignored: %ldms recorded (min %dms)\n",
                           TAG, el_ms, (int)(VOICE_PTT_MIN_SEC * 1000));
                } else {
                    button_ptt_release();
                }
                }   /* [V719] end of the s_v719_long release path */
            }
        }
        /* [V690] Volume keys: GPIO46 = up, GPIO45 = down.
         *
         * Same 20 ms cadence as the official board's ButtonPollTask, with the
         * burst vote and a 2-poll (40 ms) confirmation so pad dither cannot
         * emit phantom steps.  A click is any release shorter than
         * VOL_LONG_MS; holding past that repeats nothing and instead snaps to
         * the end stop, which is what xiaozhi does (ShowNotification
         * MAX_VOLUME / MUTED).
         */
        {
            int up = btn_pin_burst(VOL_GPIO_UP);
            int dn = btn_pin_burst(VOL_GPIO_DOWN);

            if (up == vol_up_prev) { vol_up_deb = 0; }
            else if (++vol_up_deb >= 2) { vol_up_prev = up; vol_up_deb = 0; }

            if (dn == vol_dn_prev) { vol_dn_deb = 0; }
            else if (++vol_dn_deb >= 2) { vol_dn_prev = dn; vol_dn_deb = 0; }

            if (vol_up_prev && vol_up_t0 == 0)
              {
                vol_up_t0 = clock_systime_ticks();
              }
            else if (!vol_up_prev && vol_up_t0 != 0)
              {
                long held = (long)((clock_systime_ticks() - vol_up_t0) * 10);

                vol_up_t0 = 0;
                if (held >= VOL_LONG_MS) { s_vol = VOL_MAX; }
                else                     { s_vol += VOL_STEP; }
                vol_apply(s_vol);
                s_vol = (s_vol > VOL_MAX) ? VOL_MAX : s_vol;
              }

            if (vol_dn_prev && vol_dn_t0 == 0)
              {
                vol_dn_t0 = clock_systime_ticks();
              }
            else if (!vol_dn_prev && vol_dn_t0 != 0)
              {
                long held = (long)((clock_systime_ticks() - vol_dn_t0) * 10);

                vol_dn_t0 = 0;
                if (held >= VOL_LONG_MS) { s_vol = 0; }
                else                     { s_vol -= VOL_STEP; }
                vol_apply(s_vol);
                s_vol = (s_vol < 0) ? 0 : s_vol;
              }

            if (!vol_seeded && s_vol >= 0)
              {
                vol_seeded = 1;   /* first pass: publish the codec default */
                es8311_ext_setvolume((uint16_t)(s_vol * 10));
              }
        }

        /* [V691] Hands-free capture driver -- DIAGNOSTIC ONLY.
         *
         * When /tmp/autoptt.txt exists, run a press -> 5 s -> release cycle
         * every ~9 s.  Several earlier rounds drew the wrong conclusion
         * ("the BOOT key is dead") purely because the user's presses fell
         * outside the 4-minute capture window; this removes the human from
         * the loop so the microphone can be measured repeatably.
         *
         *     nsh> echo 1 > /tmp/autoptt.txt     # start
         *     nsh> rm /tmp/autoptt.txt           # stop
         */

        {
            static unsigned int v691_auto_ph;
            FILE *v691af = NULL;
            int v691_on = 0;

            if ((bw_tick % 45) == 0)
              {
                v691af = fopen("/tmp/autoptt.txt", "r");

                if (v691af != NULL)
                  {
                    fclose(v691af);
                    v691_on = 1;
                  }

                if (!v691_on)
                  {
                    v691_auto_ph = 0;
                  }
                else
                  {
                    v691_auto_ph++;

                    if (v691_auto_ph == 1)
                      {
                        if (!s_ptt_recording && !s_ptt_hold)
                          {
                            syslog(LOG_INFO,
                                   "[%s] [V691] autoptt press\n", TAG);
                            button_ptt_press();
                          }
                      }
                    else if (v691_auto_ph == 6)
                      {
                        syslog(LOG_INFO, "[%s] [V691] autoptt release\n",
                               TAG);
                        button_ptt_release();
                      }
                    else if (v691_auto_ph >= 9)
                      {
                        v691_auto_ph = 0;
                      }
                  }
              }
        }

        /* [V749] 命令工作线程看门狗。
         *
         * v748 的实测教训：worker 一旦被卡住（当时卡在 rec_join_bounded()
         * 的 pthread_join），s_ptt_hold 就永远解不开 —— 之后 105 次
         * release 全部空转、press 全部被拒，屏幕停在「正在思考」，按 BOOT
         * 完全没有反应，且日志里没有任何显式报错。
         *
         * 这里给「有命令在飞」加一个 15s 上限。超时就强制解锁并告警：
         * 按键恢复可用，同时日志里一定留下 [V749] 这条证据。根因还是要靠
         * H1~H3 修，这层只是保证用户永远不会再遇到「永久无响应」。 */

        {
            static clock_t s_wd_tick;
            static int     s_wd_armed;

            if (s_btnq_busy != 0)
              {
                if (!s_wd_armed)
                  {
                    s_wd_tick = clock_systime_ticks();
                    s_wd_armed = 1;
                  }
                else
                  {
                    long wd_ms = (long)((clock_systime_ticks() -
                                         s_wd_tick) * 10);

                    if (wd_ms >= 15000)
                      {
                        syslog(LOG_ERR,
                               "[%s] [V749] cmd worker stuck %ldms "
                               "(busy=%d post=%lu run=%lu) -- force unwind\n",
                               TAG, wd_ms, (int)s_btnq_busy,
                               g748_post, g748_run);
                        diag_report("BTN_WORKER_STUCK");

                        s_ptt_hold = 0;
                        s_ptt_recording = false;
                        s_ptt_evt_ticks = clock_systime_ticks();
                        s_wd_tick = clock_systime_ticks();   /* 再给一轮 */
                      }
                  }
              }
            else
              {
                s_wd_armed = 0;
              }
        }

        /* [V369] never let a PTT session hang open waiting for an edge
         * that may never arrive. */
        if (s_ptt_recording && s_ptt_start != 0 &&
            (time(NULL) - s_ptt_start) >= VOICE_PTT_MAX_SEC) {
            syslog(LOG_INFO,
                   "[%s] PTT auto-stop after %lds (release edge never seen)\n",
                   TAG, (long)(time(NULL) - s_ptt_start));
            button_ptt_release();
        }
        else if (!s_ptt_recording && s_ptt_hold && s_ptt_evt_ticks != 0 &&
                 (long)((clock_systime_ticks() - s_ptt_evt_ticks) * 10)
                     >= (long)(VOICE_PTT_HOLD_MAX_SEC * 1000)) {
            /* [V744] The branch above only fires while the session is still
             * marked recording.  Once s_ptt_recording has gone false but
             * s_ptt_hold is still latched, nothing else clears it, and the
             * board stays deaf (-EBUSY on every start) until a power cycle or
             * a second press.  Cap the hold so it always unwinds. */

            syslog(LOG_WARNING,
                   "[%s] [V744] PTT hold latched %ldms -- force release\n",
                   TAG,
                   (long)((clock_systime_ticks() - s_ptt_evt_ticks) * 10));
            s_ptt_evt_ticks = 0;
            button_ptt_release();
        }
        btn_prev = committed;
        usleep(20000);
        ++bw_tick;
        /* [V369] re-arm the pad every 100ms: re-applying INPUT|PULLUP is
         * what made presses visible at all (V368), and doing it 10x more
         * often also unfreezes the level in time for the release. */
        if ((bw_tick % 5) == 0) {
            esp32s3_configgpio(0, 0x09);  /* INPUT | PULLUP */
            esp32s3_configgpio(VOL_GPIO_UP,   0x09);   /* [V690] */
            esp32s3_configgpio(VOL_GPIO_DOWN, 0x09);   /* [V690] */
        }
        /* periodic liveness beacon (~30s) -- fires once network is up */
        if (bw_tick >= 1500) {
            bw_tick = 0;
            diag_report("BTNWK_TASK");

            /* [V745] One line every 30 s: how many sessions have run and how
             * many of the 256 KB fallback buffers are still outstanding.  A
             * steady `held` means clean; a climbing `held` names the leak. */

            V1024S(LOG_WARNING, "[V745]] sess=%lu pcm_a=%lu pcm_f=%lu held=%lu st=%d "
                   "cap=%p hold=%d rec=%d\n",
                   g745_sess, g745_pcm_alloc, g745_pcm_free,
                   g745_pcm_alloc - g745_pcm_free, (int)s_voice.state,
                   (void*)s_voice.cap, (int)s_ptt_hold,
                   (int)s_ptt_recording);

            /* [V748] 队列计量：run 应始终追上 post，drop 必须为 0，
             * busy 在会话结束后回到 0。busy 长期非 0 = 工作线程被卡住。 */
            V1024S(LOG_WARNING, "[V748]] post=%lu run=%lu drop=%lu busy=%d q=%d\n",
                   g748_post, g748_run, g748_drop, (int)s_btnq_busy,
                   (int)((s_btnq_tail - s_btnq_head + BTN_CMDQ_DEPTH)
                         % BTN_CMDQ_DEPTH));
        }
    }
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
        diag_report("ASR_OK");
        printf("ASR: %s\n", text);

        /* Wake-word gate: drop anything that is not addressed to us. */
        if (!voice_wake_accepted(text)) {
            /* [V849] the text is the only way to tell "never said it"
             * from "ASR spelled it differently". */
            syslog(LOG_INFO,
                   "[%s] [V849] wake word not found, dropped: <%s>\n",
                   TAG, text ? text : "(null)");
            diag_report("WAKE_MISS");
        /* [V855] same two steps as the streaming path */
        ui_state_set_ask(text);
        s_reply_pending  = 1;
        s_reply_deadline = time(NULL) + 25;

            printf("Wake: dropped\n");
            s_wake_missed = 1;      /* [V712] let the listener move on */
            free(pcm);
            return NULL;
        }

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

        /* [V715] Nothing was dispatched, so the listener must not wait
         * VOICE_LISTEN_REPLY_WAIT_MS (20 s) for a reply that will never
         * come.  Only the wake-word drop used to set this flag, which is why
         * an empty transcript deafened the board for 20 s. */

        s_wake_missed = 1;
    }

    free(pcm);
    return NULL;
}


/* [V869] *** Coalesce the stream before it reaches the DMA. ***
 *
 * tts_stream_cb used to call audio_playback_write() on every
 * 2 KB piece the backend delivered, and [V671] reports one light
 * TX re-arm per write -- i.e. the transmitter ran dry and had to
 * be kicked once per 64 ms of audio.  Every one of those stops is
 * a click at the speaker, which is the reported noise; the holes
 * between them are turn_max (110..300 ms measured on v868).
 *
 * Buffer the pieces and hand the driver 1.5 s at a time. */

/* [DMA-FIT 2026-10-27] 48 KB 超过了 I2S 管道深度（AP_PIPE_MAX 8 x 4096 = 32 KB），
 * 流式 TTS 走这批数据时 i2s_send 直接返回 -12(ENOMEM)，一句话只播出一半。
 * 24 KB = 0.75 s，安全落在队列以内；相比原来每 2 KB 写一次，
 * TX re-arm 次数仍少一个数量级。 */
#define VOICE_TTS_FLUSH_BYTES (512u * 1024u)  /* [WHOLE-UTTER] 整句一次写，32 s 音频封顶 */

static unsigned char *s_pb_buf;
static size_t         s_pb_len;
static size_t         s_pb_cap;

static void tts_buf_reset(void)
{
  if (s_pb_buf != NULL)
    {
      free(s_pb_buf);
      s_pb_buf = NULL;
    }

  s_pb_len = 0u;
  s_pb_cap = 0u;
}

static void tts_buf_flush(audio_playback_t *pb)
{
  size_t off;

  if (pb == NULL || s_pb_len == 0u)
    {
      return;
    }

  /* [REALTIME-FEED 2026-10-27] 按实时速率分帧喂 I2S。
   *
   * 与 tts_offline_play() 里 [TTS-CACHE-FIX] 同一个道理，只是这条路径
   * 一直没做：I2S 管线只保留 AP_PIPE_MAX(8) x 4096 B = 32 KB 在飞，
   * 一次性 write 一整句（本地 2301 服务 0.8 s 就返回 142 KB）会把管线
   * 撑爆，i2s_send 返回 -12(ENOMEM)，audio_playback_write 提前返回，
   * 实测一句话只播出前 15-30 KB（[V710] pcm=15KB / 源 142KB）。
   *
   * 4096 B @ 16 kHz/s16 = 128 ms 音频，所以每帧喂完等 110 ms；
   * 写失败再小步重试（<= 40 次 x 20 ms = 800 ms）。 */
  for (off = 0u; off < s_pb_len; off += 4096u)
    {
      size_t n = s_pb_len - off;
      int    tries = 0;

      /* [SPEAK-EPOCH 2026-10-29] 被打断（BOOT 聆听 / V719）时直接丢弃
       * 残句缓冲，不再把半截音频续播出来（"断断续续"碎片来源之二）。 */
      if (s_voice.tts_abort)
        {
          syslog(LOG_WARNING,
                 "[%s] flush aborted, drop %u B residual audio\n",
                 TAG, (unsigned)(s_pb_len - off));
          s_pb_len = 0u;
          return;
        }

      if (n > 4096u)
        {
          n = 4096u;
        }

      while (audio_playback_write(pb, s_pb_buf + off, n) < 0 &&
             tries++ < 40 && !s_voice.tts_abort)
        {
          usleep(20000);
        }

      usleep(110000);
    }

  s_pb_len = 0u;
}

static void tts_buf_push(audio_playback_t *pb,
                        const unsigned char *d, size_t n)
{
  if (s_pb_len + n > s_pb_cap)
    {
      size_t nc = (s_pb_cap != 0u) ? s_pb_cap : (64u * 1024u);
      unsigned char *nb;

      while (nc < s_pb_len + n)
        {
          nc *= 2u;
        }

      nb = (unsigned char *)realloc(s_pb_buf, nc);

      if (nb == NULL)
        {
          /* Out of memory: fall back to writing straight through.
           * Noisier, but it still speaks. */
          tts_buf_flush(pb);
          audio_playback_write(pb, d, n);
          return;
        }

      s_pb_buf = nb;
      s_pb_cap = nc;
    }

  memcpy(s_pb_buf + s_pb_len, d, n);
  s_pb_len += n;
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
        tts_buf_push(pb, pcm_data, pcm_len);

        /* [WHOLE-UTTER 2026-10-27] 不再边收边播。
         *
         * 流式写入的速率远高于播放速率（本地 2301 服务 0.5 s 就吐完
         * 142 KB，而播完要 4.4 s），I2S 驱动的在飞槽位被瞬间占满，
         * i2s_send 返回 -12(ENOMEM)，audio_playback_write 提前返回 ->
         * 一句话只播出前 20%（[V710] pcm=23KB / 源 142KB）。
         *
         * 离线缓存那条路径是整段一次性 write，实测 71 KB 完整播出
         * （pcm=70KB），所以这里也改成"收完整句再播"。代价是首字晚
         * 0.3-0.5 s，换来的是不丢字。 */
        if (s_pb_len >= VOICE_TTS_FLUSH_BYTES)
          {
            /* 占位：阈值已放大到 512 KB，正常一句话不会走到这里 */
            tts_buf_flush(pb);
          }
    }

    if (is_last)
      {
        tts_buf_flush(pb);
      }
}

/* ── Public API ──────────────────────────────────────────── */

int voice_channel_init(void)
{
    diag_report("VC_INIT");
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

    /* [FIX-LAMPTTS 2026-10-26] Register the MiMo TTS backend even though
     * the voice channel itself stays disabled.  sensor_chat local replies
     * (台灯/传感器) speak via voice_channel_speak -> voice_tts_speak_stream,
     * which needs a registered backend or it returns -ENOSYS and the
     * speaker stays silent (the lamp-announce bug: "streaming TTS failed
     * (-38)").  TTS is HTTPS only and never touches the microphone, so
     * this does not violate the [V872] single-mic-owner rule.  ASR stays
     * unregistered on purpose. */
    if (!s_backends_registered) {
        extern int mimo_tts_register(void);
        mimo_tts_register();
        /* [LOCAL-TTS 2026-10-27] init 把 tts_http_url 从 config_store 读进 http_tts.c
         * （板上没配就落到 kconfig 默认值 = 用户 PC 的 2301 端口）。 */
        http_tts_init();
        s_backends_registered = 1;
    }

    /* [V872] *** aivox owns the microphone. ***
     *
     * The embedded xiaozhi is the only voice assistant on this
     * board.  ai_agent still runs its agent, LLM and tools -- and
     * still writes the /tmp files the panel reads -- but it must
     * not open /dev/audio/pcm0c: two owners of one I2S port is what
     * produced the xtensa_panic and the double answers.
     *
     * Returning 0 keeps every caller happy: run_init_to() only logs
     * the code, and nothing downstream dereferences a voice device
     * that was never opened. */
    syslog(LOG_ERR,
           "[%s] [V872] voice channel disabled: aivox is the only "
           "voice assistant\n", TAG);

    return 0;

    if (!s_backends_registered) {
        /* [V693] Xiaomi MiMo is now the one and only speech backend.
         *
         * voice_asr_register() and voice_tts_register() both make the
         * FIRST registered backend the default.  The comment that used to
         * sit here claimed the opposite ("registered last so it becomes
         * the default") while the order was volc_tts, http_asr, volc_asr,
         * mimo -- so the live defaults were the self-hosted HTTP ASR of
         * the 42 server (port 19009, long gone: five seconds of connect
         * timeout on every utterance, and the transcript never arrived)
         * and the volcengine TTS (no credentials, and with
         * VOICE_TTS_PUBLIC_FALLBACK=0 it answered "no public TTS backend
         * selected").
         *
         * MiMo covers the whole loop with the key that is already compiled
         * in: mimo-v2.5-asr reads one WAV per utterance and mimo-v2.5-tts
         * speaks the assistant message.  The volc and self-hosted sources
         * stay in the tree and are still reachable from the shell
         * (`asr http`, `tts http`) if anybody re-registers them. */
        extern int mimo_tts_register(void);
        extern int mimo_asr_register(void);
        mimo_tts_register();
        mimo_asr_register();

        s_backends_registered = 1;
    }

    /* [V693] http_tts_init() used to be called here.  All it did was load
     * the 42 server's self-hosted TTS base URL into http_tts.c; that
     * backend is no longer registered, so the call only produced a
     * "base URL from config" line naming a host that is not reachable. */

    /* start BOOT-button wake poller (independent of voice wake) */
    static int s_btn_started = 0;
    if (!s_btn_started) {
        s_btn_started = 1;
        pthread_t bt;
        pthread_attr_t battr;

        /* [V648-B] button_wake_task is NOT a shallow poller: on release
         * it calls voice_channel_stop_with_text() synchronously, which
         * blocks through the ASR request -- an HTTPS/mbedTLS handshake
         * (vela_tls.c) -- on this very stack.  With NULL attributes it
         * only got CONFIG_PTHREAD_STACK_DEFAULT - TLS = 4096 - 32 =
         * 4064 bytes (dump_task: 0x3c1aada0 4064).  The handshake ran
         * past that into the pthread TLS block, which tls_get_info()
         * locates at the lowest address of the stack allocation:
         *   #define tls_get_info() \
         *       ((FAR struct tls_info_s *)this_task()->stack_alloc_ptr)
         * That destroyed tl_task (offset 0), and the syslog right
         * after the handshake (vela_tls.c:560 "Handshake OK") died in
         * vsyslog()/task_get_info() with EXCCAUSE=001c and
         * vaddr=0x2b2a5e.  Use the same 24 KB the ASR thread uses
         * (ASR_THREAD_STACK).
         */

        pthread_attr_init(&battr);
        pthread_attr_setstacksize(&battr, 24 * 1024);


        /* [V748] 会话命令工作线程：press/release 的阻塞部分在这里跑，
         * 按钮轮询线程只做采样 + 投递。栈与按钮线程同为 24KB —— 它要跑
         * voice_channel_start() / voice_channel_stop()，比按钮线程现在
         * 干的事更轻（HTTPS 已在 v746 移走）。 */
        sem_init(&s_btnq_sem, 0, 0);
        {
            pthread_t cw;
            pthread_attr_t cattr;

            pthread_attr_init(&cattr);
            pthread_attr_setstacksize(&cattr, 24 * 1024);

            if (pthread_create(&cw, &cattr, button_cmd_worker, NULL) != 0) {
                syslog(LOG_ERR,
                       "[%s] [V748] failed to start button cmd worker\n",
                       TAG);
            } else {
                pthread_detach(cw);
                s_btnq_worker_live = 1;
                syslog(LOG_INFO, "[%s] [V748] button cmd worker up\n", TAG);
            }

            pthread_attr_destroy(&cattr);
        }

        if (pthread_create(&bt, &battr, button_wake_task, NULL) != 0) {
            syslog(LOG_ERR, "[%s] failed to start button wake task\n", TAG);
        } else {
            pthread_detach(bt);
        }

        pthread_attr_destroy(&battr);
    }

    /* [V857e] *** Independent ui_state heartbeat. ***
     *
     * ui_state_beat() used to be called only from the listen loop,
     * which blocks for 10-20 s inside voice_channel_stop()'s
     * synchronous TLS + HTTP round trip.  The beat froze for the
     * whole turn and the UI's staleness guard then wiped the
     * subtitle at exactly the wrong moment (QA: 'speaking -> idle'
     * immediately followed by 'subtitle: (cleared)').  This thread
     * does nothing but beat, so it keeps running while every other
     * voice task is parked on the network. */

    {
        pthread_t      hbt;
        pthread_attr_t hbattr;

        pthread_attr_init(&hbattr);
        pthread_attr_setstacksize(&hbattr, 8192);

        if (pthread_create(&hbt, &hbattr, v857_beat_task, NULL) != 0)
          {
            syslog(LOG_ERR,
                   "[%s] [V857e] heartbeat task create failed\n", TAG);
          }
        else
          {
            pthread_detach(hbt);
          }

        pthread_attr_destroy(&hbattr);
    }

    return 0;
}

int voice_channel_start(void)
{
    vc_beacon("B7_VC_ENTER");

    /* [SPEAK-EPOCH 2026-10-29] 开始新聆听 = 旧回答整体作废：当前句由
     * 下面的 abort 逻辑掐掉，排队中的句子靠 epoch 不符自弃。 */
    s_speak_epoch++;

    pthread_mutex_lock(&s_voice.lock);
    if (s_voice.state != VOICE_IDLE || s_voice.cap != NULL) {
        /* [V713] s_voice.cap is the authoritative "the capture device is
         * really closed" flag; the state alone is not.  See the V713 note in
         * voice_channel_stop(). */

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
        http_tts_abort_active();   /* [TTS-ABORT] 立刻关掉旧 TTS 的 socket，防连接泄漏 */
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

    emotion_set("listening");   /* 麦克风即将打开：显示“聆听”表情 */

    /* PCM buffer is lazy-allocated only when streaming ASR fails
     * (batch fallback). This saves 128KB RAM on the happy path. */
    s_voice.pcm_cap = AGENT_VOICE_PCM_BUF_SIZE;

    /* [V745] was a bare `s_voice.pcm_buf = NULL;` -- a session torn down on
     * an error path leaked its 256 KB buffer because this dropped the only
     * reference to it. */

    v745_pcm_release();
    s_voice.pcm_len = 0;
    s_voice.asr_stream = NULL;
    s_voice.vad_ended = 0;
    s_vad_started = 0;
    s_vad_silence_ms = 0;
    s_vad_speech_ms = 0;
    s_vad_hang_ms = 0;
    v857_session_reset();   /* [V857] */
    g857_run = 0;           /* [V857b] */

    /* Pre-open streaming ASR session HERE (before spawning the
     * recording thread) so that TLS handshake (~300-800ms) completes
     * before audio capture starts.  Without this, the handshake runs
     * inside recording_thread while audio is already flowing, causing
     * the first 300-800ms of speech to be lost.
     *
     * If the pre-open fails we fall back to batch mode as before. */
    voice_asr_stream_t* pre_stream = voice_asr_stream_open();
    if (!pre_stream) {
        /* [V693] NULL now simply means "this backend has no streaming
         * API", which is the normal case for MiMo: the PCM is accumulated
         * while the button is held and recognised in one request on
         * release.  It is not a failure and must not be logged as one. */
        syslog(LOG_INFO,
            "[%s] ASR in batch mode (backend has no stream API)\n", TAG);
    } else {
        syslog(LOG_INFO, "[%s] ASR pre-connected\n", TAG);
        s_voice.asr_stream = pre_stream;
    }

    /* Open capture device (but do NOT start yet — start after the
     * recording thread is spawned so the consumer is ready before
     * audio frames begin flowing, preventing media_recorder queue
     * overflow "data queue is more than max count(4)"). */
    vc_beacon("B7_VC_PRECAP");

    s_voice.cap = audio_capture_open(
        AGENT_AUDIO_CAPTURE_DEV,
        AGENT_VOICE_SAMPLE_RATE,
        AGENT_VOICE_CHANNELS,
        AGENT_VOICE_BITS);

    if (!s_voice.cap) {
        if (pre_stream) {
            voice_asr_stream_abort(pre_stream);
            s_voice.asr_stream = NULL;
        }
        pthread_mutex_unlock(&s_voice.lock);
        syslog(LOG_ERR, "[%s] capture open failed\n", TAG);
        return -EIO;
    }

    s_voice.state = VOICE_RECORDING;
    pthread_mutex_unlock(&s_voice.lock);

    /* Spawn recording thread BEFORE starting capture */
    pthread_attr_t attr;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, AGENT_VOICE_STACK);

    g733_rec_done = 0;
    g743_gen++;                 /* [V743] new session generation */

    int ret = pthread_create(&s_voice.rec_thread, &attr,
        recording_thread, NULL);

    /* [V749] 创建即 detach —— 回收交给内核，代码里再也不 join。
     *
     * 这一句是修掉 v748 僵死的关键。join 一个有去无回的线程会永久阻塞，
     * 而 NuttX 本树没有 pthread_tryjoin_np：s_voice.rec_thread 只要被
     * join 过一次（voice_channel_discard() 就会 join），或者 gen 判定
     * 命中了别的会话的线程，下一次 join 就是无期徒刑。detach 之后
     * rec_join_bounded() 退化为纯粹的有界标志等待，不可能再挂住。 */

    if (ret == 0)
      {
        pthread_detach(s_voice.rec_thread);
      }

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
        v745_pcm_release();
        pthread_mutex_unlock(&s_voice.lock);
        return -ret;
    }

    /* Now start capture — wait for recording thread to be ready first */
    sem_wait(&s_voice.rec_ready);

    vc_beacon("B7_VC_PRESTARTCAP");

    if (audio_capture_start(s_voice.cap) < 0) {
        pthread_mutex_lock(&s_voice.lock);
        s_voice.state = VOICE_IDLE;
        if (s_voice.asr_stream) {
            voice_asr_stream_abort(s_voice.asr_stream);
            s_voice.asr_stream = NULL;
        }
        pthread_mutex_unlock(&s_voice.lock);
        audio_capture_abort(s_voice.cap);

        /* [V743] never close the capture underneath a live thread --
         * that is the V713 mm_forcefree panic (EXCCAUSE=001d, vaddr=0x8). */

        if (rec_join_bounded() == 0) {
            audio_capture_close(s_voice.cap);
        }

        s_voice.cap = NULL;

        /* [V745] recording_thread() already allocated the fallback buffer;
         * after this return nothing else can reach it.  (The duplicated
         * `s_voice.cap = NULL;` that used to be here is gone.) */

        v745_pcm_release();
        s_voice.pcm_len = 0;
        return -EIO;
    }

    syslog(LOG_INFO, "[%s] PTT recording started\n", TAG);
    printf("Voice: recording... (use voice_stop to finish)\n");
    return 0;
}

/* ── Always-on listening ────────────────────────────────────────────────────
 *
 * Loops the existing session machinery: start capture, let the endpointing
 * close the utterance, stop - which finalises the ASR and pushes the transcript
 * into the agent's message bus - then let the answer be spoken before listening
 * again.
 *
 * The last step is not optional: without echo cancellation, a session started
 * while TTS is playing records the answer and the assistant talks to itself.
 */

#define VOICE_LISTEN_NO_SPEECH_MS  8000  /* give up on a silent session */
#define VOICE_LISTEN_POLL_MS       20
#define VOICE_LISTEN_REPLY_WAIT_MS 20000 /* answer starts within this */  /* [V73] LLM round trips exceed 12 s on this link */
#define VOICE_LISTEN_REPLY_MAX_MS  30000 /* answer finishes within this */
#define VOICE_LISTEN_SETTLE_MS     800   /* quiet before listening again */

static volatile int s_listen_stop;
static volatile int s_listen_running;

/* [V67] Set while voice_channel_speak() is waiting for the channel.  The
 * listen loop watches it and winds its current session down immediately so
 * the speaker does not have to wait out the no-speech timeout. */

static volatile int s_speak_pending;

/* [V712] Throw away the recording in progress without running ASR and without
 * pushing anything to the agent.  Mirrors the tail of voice_channel_stop()
 * minus the recognition step. */

int voice_channel_discard(void)
{
    pthread_mutex_lock(&s_voice.lock);
    if (s_voice.state != VOICE_RECORDING) {
        pthread_mutex_unlock(&s_voice.lock);
        return -EINVAL;
    }
    s_voice.state = VOICE_STOPPING;
    pthread_mutex_unlock(&s_voice.lock);

    audio_capture_abort(s_voice.cap);

    /* Wait for the recording thread to finish */
    (void)rec_join_bounded();   /* [V743] bounded: 不再裸 join */

    pthread_mutex_lock(&s_voice.lock);
    recording_cleanup_locked(s_voice.asr_stream);
    s_voice.asr_stream = NULL;
    s_voice.vad_ended = 1;
    pthread_mutex_unlock(&s_voice.lock);

    vc_probe("B7_LST_DISCARD", (long)s_voice.state, 0);
    syslog(LOG_INFO, "[%s] session discarded (microphone handed over)\n",
        TAG);
    return 0;
}

static void* voice_listen_task(void* arg)
{
    (void)arg;

    vc_beacon("B7_LST_TASK");

    syslog(LOG_INFO, "[%s] always-on listening active\n", TAG);

    /* [V857] Print the gates this build shipped with, so a capture always
     * says which numbers produced the verdicts below it. */

    V1024S(LOG_ERR, "[V857]] vad gates: peak=%d/%d floor=[%d..%d] mul=%d/%d "
           "open=[%d..%d] close=[%d..%d] silence=%d min=%d max=%d\n",
           VOICE_VAD_OPEN_PEAK, VOICE_VAD_CLOSE_PEAK,
           VOICE_VAD_FLOOR_MIN, VOICE_VAD_FLOOR_MAX,
           VOICE_VAD_OPEN_MUL, VOICE_VAD_CLOSE_MUL,
           VOICE_VAD_OPEN_MIN, VOICE_VAD_OPEN_MAX,
           VOICE_VAD_CLOSE_MIN, VOICE_VAD_CLOSE_MAX,
           VOICE_VAD_SILENCE_MS, VOICE_VAD_MIN_SPEECH_MS, VOICE_VAD_MAX_MS);

    while (!s_listen_stop) {
        ui_state_beat();   /* [V857] keep /tmp/ui_state.txt fresh */

        /* [V73] Do not open a session while somebody is waiting to speak:
         * voice_channel_start() would move the channel straight back into
         * VOICE_RECORDING and the speaker's hand-over wait would expire
         * again, which is exactly what silenced the greeting and any reply
         * that landed while a session was being restarted. */

        /* [V712] The BOOT key wants the microphone: stay out of its way
         * until it has claimed the session. */

        if (s_ptt_preempt) {
            usleep(20000);
            continue;
        }

        if (s_speak_pending) {
            usleep(50000);
            continue;
        }

        if (s_voice.state != VOICE_IDLE || s_voice.tts_pb != NULL ||
            s_voice.cap != NULL) {
            static int s_v76_waitb;
            if (!s_v76_waitb) { s_v76_waitb = 1; vc_beacon("B7_LST_WAITB"); }
            /* [V69] Report a one-shot probe the first time the loop finds
             * the channel busy, so a stuck state is visible immediately. */

            static int s_v69_waiting;
            static int s_v69_waiting_state;

            if (!s_v69_waiting_state) {
                s_v69_waiting_state = (int)s_voice.state + 1;
                vc_probe("B7_LST_WAIT", (long)s_voice.state,
                    (long)(s_voice.tts_pb != NULL));
            }

            if (s_v69_waiting < 40) {
                s_v69_waiting++;
            } else if (s_v69_waiting == 40) {
                s_v69_waiting = 41;
                vc_probe("B7_LST_STUCK", (long)s_voice.state, 0);
            }

            usleep(VOICE_LISTEN_POLL_MS * 1000);
            continue;
        }

        /* [V855] A reply is on its way: do not touch the microphone. */
        {
          static int s_v855_hold;

          if (s_reply_pending)
            {
              if (!s_v855_hold)
                {
                  s_v855_hold = 1;
                  syslog(LOG_INFO,
                         "[%s] [V855] listen: holding off until the "
                         "reply is spoken\n", TAG);
                }

              if (s_reply_deadline != 0 &&
                  time(NULL) > s_reply_deadline)
                {
                  s_reply_pending = 0;
                  s_v855_hold    = 0;
                  syslog(LOG_WARNING,
                         "[%s] [V855] reply wait expired, resuming "
                         "listen\n", TAG);
                }
              else
                {
                  usleep(50000);
                  continue;
                }
            }
          else
            {
              s_v855_hold = 0;
            }
        }

        /* [V857e] Post-playback quiet period: our own reply is still
         * in the room and the microphone hears it louder than
         * OPEN_MAX.  Waiting here costs nothing on a normal turn
         * (g857_quiet_until is only set when TTS really played). */

        if (time(NULL) < g857_quiet_until) {
            ui_state_beat();
            usleep(50000);
            continue;
        }

        vc_probe("B7_LST_SESS", 0, 0);

        s_wake_missed = 0;      /* [V712] per-session */

        if (voice_channel_start() != 0) {
            syslog(LOG_WARNING, "[%s] listen: session start failed\n", TAG);
            sleep(1);
            continue;
        }

        /* Wait for the endpointing, but never hold a silent session open: the
         * streaming ASR session has its own server-side idle timeout. */

        int waited = 0;
        int discard = 0;    /* [V712] session thrown away for the BOOT key */

        while (!s_listen_stop && !s_voice.vad_ended) {
            usleep(VOICE_LISTEN_POLL_MS * 1000);
            ui_state_beat();   /* [V857] */
            waited += VOICE_LISTEN_POLL_MS;

            /* [V67] Someone wants to speak: hand the channel over now. */

            if (s_speak_pending) {
                vc_probe("B7_LST_YIELD", (long)s_voice.state, waited);
                syslog(LOG_INFO,
                    "[%s] listen: yielding channel to speaker\n", TAG);
                break;
            }

            /* [V712] BOOT pressed: drop this session on the floor -- no ASR
             * and no dispatch -- and let the key have the microphone. */

            if (s_ptt_preempt) {
                vc_probe("B7_LST_YIELD", (long)s_voice.state, waited);
                syslog(LOG_INFO,
                    "[%s] listen: yielding channel to the BOOT key\n", TAG);
                discard = 1;
                break;
            }

            if (waited >= VOICE_VAD_MAX_MS + 2000) {
                break;
            }

            if (!s_vad_started && waited >= VOICE_LISTEN_NO_SPEECH_MS) {
                syslog(LOG_INFO,
                    "[%s] listen: no speech for %d ms, restarting session\n",
                    TAG, waited);
                break;
            }
        }

        vc_probe("B7_LST_STOPPRE", (long)s_voice.state, waited);

        if (discard) {
            /* [V712] Cut short on purpose: throw it away instead of running
             * ASR on a fragment and dispatching whatever it hallucinated. */

            voice_channel_discard();
            s_wake_missed = 0;
            continue;
        }

        /* [V715] *** Silent-session gate. ***
         *
         * s_vad_started is cleared by voice_channel_start() and set the first
         * time a chunk reaches VOICE_VAD_OPEN_PEAK, so at this point it
         * answers exactly one question -- was anything in this session loud
         * enough to be speech?  The "no speech for 8000 ms" break above just
         * established that it was not, and the old code ran the ASR anyway:
         * a full TLS handshake plus a round trip every time (measured:
         * "Pooled connection stale, reconnecting" -> new handshake -> 3+ s)
         * which then fell into the 20 s VOICE_LISTEN_REPLY_WAIT_MS for a
         * reply that could not arrive.  Measured on v714 (_v714_stuck.log),
         * one cycle, with maxpeak=6 on every session (VAD opens at 150):
         *
         *   12.332 listen: no speech for 8000 ms, restarting session
         *   12.394 recording thread exit: 203 chunks, maxpeak=6
         *   13.457 ui_state: listening -> thinking
         *   14.454 recorded 207674 bytes (stream=0)
         *   19.016 ASR failed or empty: 0
         *   39.670 ui_state: thinking -> listening       <- 20.65 s later
         *
         * The board was awake 8 s then deaf for 23 s showing 正在思考, so
         * every BOOT press landed in the dead window.  Throw a silent
         * session away exactly like the BOOT-preempt case above: no ASR, no
         * dispatch, no UI change, straight back to listening.
         *
         * This is also the energy gate the listener always needed, so the
         * MiMo ASR budget stops being spent on silence.  When the microphone
         * really does produce speech (peak >= VOICE_VAD_OPEN_PEAK) nothing
         * below changes. */

        if (!s_vad_started) {
            /* [V857] The session is being thrown away: print what the
             * microphone delivered for the whole session, so "the user
             * spoke and was not heard" can be told apart from "nobody
             * spoke".  pkpass/adpass say which gate said no. */

            V1024S(LOG_ERR, "[V857]] sess SILENT waited=%d n=%lu "
                   "pk p50=%ld p90=%ld max=%ld "
                   "ad p50=%ld p90=%ld max=%ld | floor=%ld open=%ld "
                   "peakgate=%d | pass pk=%lu ad=%lu both=%lu\n",
                   waited, g857_n_e,
                   v857_pct(g857_pk_e, g857_n_e, 50),
                   v857_pct(g857_pk_e, g857_n_e, 90),
                   g857_pkmax_e,
                   v857_pct(g857_ad_e, g857_n_e, 50),
                   v857_pct(g857_ad_e, g857_n_e, 90),
                   g857_admax_e,
                   (long)g733_floor, g733_open, VOICE_VAD_OPEN_PEAK,
                   g857_pkpass_e, g857_adpass_e, g857_both_e);

            vc_probe("B7_LST_SILENT", (long)s_voice.state, waited);
            syslog(LOG_INFO,
                "[%s] listen: silence (peak < %d), no ASR, listening on\n",
                TAG, VOICE_VAD_OPEN_PEAK);
            voice_channel_discard();
            s_wake_missed = 0;
            continue;
        }

        /* [V857] Same report for a session that DID open, so the speech
         * level the board actually hears is on record next to the gate. */

        V1024S(LOG_ERR, "[V857]] sess SPEECH n=%lu "
               "pk p50=%ld p90=%ld max=%ld "
               "ad p50=%ld p90=%ld max=%ld | floor=%ld open=%ld "
               "peakgate=%d | speech_ms=%d silence_ms=%d "
               "pass pk=%lu ad=%lu both=%lu\n",
               g857_n_e,
               v857_pct(g857_pk_e, g857_n_e, 50),
               v857_pct(g857_pk_e, g857_n_e, 90),
               g857_pkmax_e,
               v857_pct(g857_ad_e, g857_n_e, 50),
               v857_pct(g857_ad_e, g857_n_e, 90),
               g857_admax_e,
               (long)g733_floor, g733_open, VOICE_VAD_OPEN_PEAK,
               s_vad_speech_ms, s_vad_silence_ms,
               g857_pkpass_e, g857_adpass_e, g857_both_e);

        voice_channel_stop(); /* ASR + message_bus_push_inbound */
        vc_probe("B7_LST_STOPDONE", (long)s_voice.state, 0);

        /* [V712] Recognised, but not addressed to us (no wake word, or an
         * ASR hallucination).  Nothing was dispatched, so waiting
         * VOICE_LISTEN_REPLY_WAIT_MS for a reply would just deafen the
         * microphone for twenty seconds after every stray sentence. */

        if (s_wake_missed) {
            syslog(LOG_INFO,
                "[%s] listen: not addressed to me, listening on\n", TAG);
            s_wake_missed = 0;
            usleep(VOICE_LISTEN_SETTLE_MS * 1000);
            continue;
        }

        /* Let the answer arrive and finish playing before the microphone is
         * live again. */

        int waited2 = 0;

        while (!s_listen_stop && s_voice.tts_pb == NULL &&
            waited2 < VOICE_LISTEN_REPLY_WAIT_MS) {
            usleep(50000);
            waited2 += 50;
            ui_state_beat();   /* [V857] */

            /* [V715] Re-test the "nothing was dispatched" flag on every
             * tick.  voice_channel_stop()'s batch fallback spawns
             * asr_and_dispatch() on its own thread, so the flag can be set
             * AFTER the test further up has already run -- which is exactly
             * why an empty transcript still cost the full 20 s of deafness.
             * Breaking early cannot lose a reply: the outer loop refuses to
             * open a session while tts_pb != NULL, so a late answer is still
             * played in full before the microphone comes back. */

            if (s_wake_missed) {
                break;
            }
        }

        waited2 = 0;

        while (!s_listen_stop && s_voice.tts_pb != NULL &&
            waited2 < VOICE_LISTEN_REPLY_MAX_MS) {
            usleep(50000);
            waited2 += 50;
            ui_state_beat();   /* [V857] */
        }

        usleep(VOICE_LISTEN_SETTLE_MS * 1000);
    }

    s_listen_running = 0;
    syslog(LOG_INFO, "[%s] always-on listening stopped\n", TAG);
    return NULL;
}

int voice_channel_listen_start(void)
{
    if (s_listen_running) {
        return 0;
    }

    s_listen_stop = 0;

    pthread_attr_t attr;
    pthread_t tid;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16384);

    int ret = pthread_create(&tid, &attr, voice_listen_task, NULL);

    diag_report("LISTEN_START");
    vc_beacon(ret == 0 ? "B7_LS_OK" : "B7_LS_FAIL");

    pthread_attr_destroy(&attr);

    if (ret != 0) {
        syslog(LOG_ERR, "[%s] listen task create failed: %d\n", TAG, ret);
        return -ret;
    }

    pthread_detach(tid);
    s_listen_running = 1;
    return 0;
}

void voice_channel_listen_stop(void)
{
    s_listen_stop = 1;
}

int voice_channel_stop(void)
{
    /* [V749] 进出锁各打一个探针。v748 僵死时只能靠排除法定位，这两个点
     * 让下一次僵死一眼可辨：看到 STOP_ENTER 没有 STOP_LOCKED = 锁被别人
     * 持有；两个都有但没有 "recorded %zu bytes" = 卡在 abort / join /
     * close 三者之一。 */

    vc_probe("B7_STOP_ENTER", (long)s_voice.state, 0);

    pthread_mutex_lock(&s_voice.lock);

    vc_probe("B7_STOP_LOCKED", (long)s_voice.state, 0);

    if (s_voice.state != VOICE_RECORDING) {
        pthread_mutex_unlock(&s_voice.lock);
        return -EINVAL;
    }
    s_voice.state = VOICE_STOPPING;
    pthread_mutex_unlock(&s_voice.lock);

    /* [V750] 分步探针。vc_probe() 在本 build 里不产生可见输出（v749 回归
     * 日志里一个 B7_STOP_* 都没有），改用真 syslog，下一次僵死可以一眼
     * 定位到具体步骤。 */

    V1024S(LOG_INFO, "[V750]] stop: abort\n");

    audio_capture_abort(s_voice.cap);

    V1024S(LOG_INFO, "[V750]] stop: join\n");

    /* [V733] *** Bounded wait: the old pthread_join() hung for good. ***
     *
     * V720 already recorded the deadlock -- recording_thread parked on a
     * semaphore while the caller sits in pthread_join -- but the fix then
     * only removed the abort that caused it from the VAD path.  It still
     * happens from the listener and from the BOOT key: state stays
     * VOICE_STOPPING, every later voice_channel_start() returns -EBUSY,
     * and the board is mute until a power cycle.  That is exactly the
     * field report "pressing BOOT does nothing, screen stuck on 正在思考".
     *
     * Wait for the thread to reach its last line, then join (immediate).
     * If it never gets there, detach it and remember that in g733_stuck so
     * the capture is leaked rather than freed underneath a live thread --
     * V713's mm_forcefree panic (EXCCAUSE=001d, vaddr=0x8) came from
     * exactly that double free.  A leaked capture costs RAM; a double free
     * costs the board. */

    /* [V743] 统一走公共有界 join：原内联块窗口 4000ms 短于录音线程
     * 最坏退出时间 6s，正常情况也会误判超时。 */

    (void)rec_join_bounded();

    /* [V69] Release the channel as soon as the recording is over.  This used
     * to stay in VOICE_STOPPING until after audio_capture_close() and
     * voice_asr_stream_finish(), and that finish is a network round trip -
     * seconds on this link.  Everything that wanted the channel (the boot
     * greeting, an agent reply) burned its whole wait in the meantime and
     * gave up with -EBUSY, which is why a session that ended correctly still
     * left the device mute. */

    /* [V713] *** Do NOT publish VOICE_IDLE here. ***
     *
     * This used to run BEFORE audio_capture_close() below, so any second
     * session -- the always-on listener, or the BOOT key preempting it --
     * could pass its "state == VOICE_IDLE" test and reach
     * audio_capture_open() while this thread was still tearing the capture
     * down.  audio_capture_open() then hit its "force closing stale capture"
     * rescue and ran native_capture_close() on a capture its owner was
     * closing at the same time: two free()s of one native_cap_t, and the
     * panic landed in mm_forcefree (EXCCAUSE=001d, vaddr=0x8):
     *
     *   button_ptt_press -> voice_channel_start -> audio_capture_open ->
     *   native_capture_open -> free -> mm_free -> mm_forcefree
     *
     * The state is published below instead, immediately after the device is
     * really closed.  The slow part this early release was hiding (the ASR
     * round trip) still happens after that point, so voice_channel_speak()
     * is not held up any longer than before. */

    emotion_set("thinking");   /* 录音结束：进入识别/思考阶段 */

    vc_probe("B7_STOP_IDLE", (long)s_voice.state, 0);

    /* [V733] A thread that never came back still holds this capture.
     * Freeing it here is the V713 mm_forcefree panic, so leak it: the
     * channel is released either way, which is what keeps the board
     * usable. */

    /* [V750] 探针放在 if 之前（放在 } 和 else 之间会把 if/else 切断）。
     * 拿到 stuck 与 cap 指针，就能判断这一步到底会不会去 close。 */

    V1024S(LOG_INFO, "[V750]] stop: pre-close stuck=%d cap=%p\n",
           g733_stuck, (void*)s_voice.cap);

    if (!g733_stuck)
      {
        /* Close capture device */
        audio_capture_close(s_voice.cap);
      }
    else
      {
        syslog(LOG_ERR, "[%s] [V733] capture leaked (thread stuck)\n", TAG);
      }

    pthread_mutex_lock(&s_voice.lock);
    s_voice.cap = NULL;
    size_t pcm_len = s_voice.pcm_len;
    voice_asr_stream_t* stream = s_voice.asr_stream;
    s_voice.asr_stream = NULL;
    s_voice.state = VOICE_IDLE;
    pthread_mutex_unlock(&s_voice.lock);

    syslog(LOG_INFO, "[%s] recorded %zu bytes (stream=%p)\n",
        TAG, pcm_len, (void*)stream);

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

        v745_pcm_release();/* may be NULL */
        s_voice.pcm_buf = NULL;

        if (ret == 0 && text[0] != '\0') {
            syslog(LOG_INFO, "[%s] stream ASR: %s\n", TAG, text);
            printf("ASR: %s\n", text);
            clock_gettime(CLOCK_MONOTONIC, &s_asr_done_ts);

            /* [V712] The always-on path reaches the agent through THIS
             * function, and it used to dispatch every recognised utterance
             * without ever consulting the wake gate.  Ambient speech and ASR
             * hallucinations were therefore answered by the LLM, whose reply
             * the microphone heard again.  Gate it here exactly like
             * asr_and_dispatch() does. */

            if (!voice_wake_accepted(text)) {
                syslog(LOG_INFO,
                    "[%s] [V849] wake word not found, dropped (stream): "
                    "<%s>\n", TAG, text ? text : "(null)");
                diag_report("WAKE_MISS");
                s_wake_missed = 1;
                return 0;
            }

            /* [V855] (1) push the recognised question to the panel
             *       (2) mark the reply as in flight so the listener
             *           keeps its hands off the microphone. */
            ui_state_set_ask(text);
            s_reply_pending  = 1;
            s_reply_deadline = time(NULL) + 25;

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
            /* [V715] See V715-2: an empty transcript is also "nothing
             * dispatched". */
            s_wake_missed = 1;
        }
        return 0;
    }

    /* Batch fallback: pcm_buf must exist if we reach here */
    if (!s_voice.pcm_buf || pcm_len == 0) {
        v745_pcm_release();
        syslog(LOG_ERR, "[%s] no PCM data for batch ASR\n", TAG);
        return -ENODATA;
    }

    /* Batch fallback: pack pcm_len + audio for ASR thread */
    size_t total = sizeof(size_t) + pcm_len;
    unsigned char* pkg = malloc(total);

    if (!pkg) {
        v745_pcm_release();
        return -ENOMEM;
    }

    memcpy(pkg, &pcm_len, sizeof(size_t));
    memcpy(pkg + sizeof(size_t), s_voice.pcm_buf, pcm_len);
    v745_pcm_release();

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

    /* [P2] The v705 capture showed t0 -> asr_go = 2110 ms: two full
     * seconds evaporate before the upload even starts.  Stamp the two
     * calls that can block so the next run names the culprit.
     * (Only stamped here: PTT goes through _with_text, and this anchor is
     * disambiguated by the "text_out[0]" line above -- voice_channel_stop()
     * has the identical abort+join pair.) */
    struct timespec p2_t0;
    struct timespec p2_t1;
    long p2_ms;

    clock_gettime(CLOCK_MONOTONIC, &p2_t0);

    audio_capture_abort(s_voice.cap);

    clock_gettime(CLOCK_MONOTONIC, &p2_t1);
    p2_ms = (p2_t1.tv_sec - p2_t0.tv_sec) * 1000
        + (p2_t1.tv_nsec - p2_t0.tv_nsec) / 1000000;
    syslog(LOG_INFO, "[%s] [P2] capture_abort +%ldms\n", TAG, p2_ms);

    /* Wait for recording thread to finish */
    (void)rec_join_bounded();   /* [V743] bounded: 不再裸 join */

    clock_gettime(CLOCK_MONOTONIC, &p2_t1);
    p2_ms = (p2_t1.tv_sec - p2_t0.tv_sec) * 1000
        + (p2_t1.tv_nsec - p2_t0.tv_nsec) / 1000000;
    syslog(LOG_INFO, "[%s] [P2] join +%ldms\n", TAG, p2_ms);

    /* [V69] Release the channel as soon as the recording is over.  This used
     * to stay in VOICE_STOPPING until after audio_capture_close() and
     * voice_asr_stream_finish(), and that finish is a network round trip -
     * seconds on this link.  Everything that wanted the channel (the boot
     * greeting, an agent reply) burned its whole wait in the meantime and
     * gave up with -EBUSY, which is why a session that ended correctly still
     * left the device mute. */

    /* [V713] *** Do NOT publish VOICE_IDLE here. ***
     *
     * This used to run BEFORE audio_capture_close() below, so any second
     * session -- the always-on listener, or the BOOT key preempting it --
     * could pass its "state == VOICE_IDLE" test and reach
     * audio_capture_open() while this thread was still tearing the capture
     * down.  audio_capture_open() then hit its "force closing stale capture"
     * rescue and ran native_capture_close() on a capture its owner was
     * closing at the same time: two free()s of one native_cap_t, and the
     * panic landed in mm_forcefree (EXCCAUSE=001d, vaddr=0x8):
     *
     *   button_ptt_press -> voice_channel_start -> audio_capture_open ->
     *   native_capture_open -> free -> mm_free -> mm_forcefree
     *
     * The state is published below instead, immediately after the device is
     * really closed.  The slow part this early release was hiding (the ASR
     * round trip) still happens after that point, so voice_channel_speak()
     * is not held up any longer than before. */

    emotion_set("thinking");   /* 录音结束：进入识别/思考阶段 */

    vc_probe("B7_STOP_IDLE", (long)s_voice.state, 0);

    /* [V733] A thread that never came back still holds this capture.
     * Freeing it here is the V713 mm_forcefree panic, so leak it: the
     * channel is released either way, which is what keeps the board
     * usable. */

    /* [V750] 探针放在 if 之前（放在 } 和 else 之间会把 if/else 切断）。
     * 拿到 stuck 与 cap 指针，就能判断这一步到底会不会去 close。 */

    V1024S(LOG_INFO, "[V750]] stop: pre-close stuck=%d cap=%p\n",
           g733_stuck, (void*)s_voice.cap);

    if (!g733_stuck)
      {
        /* Close capture device */
        audio_capture_close(s_voice.cap);
      }
    else
      {
        syslog(LOG_ERR, "[%s] [V733] capture leaked (thread stuck)\n", TAG);
      }

    pthread_mutex_lock(&s_voice.lock);
    s_voice.cap = NULL;
    size_t pcm_len = s_voice.pcm_len;
    voice_asr_stream_t* stream = s_voice.asr_stream;
    s_voice.asr_stream = NULL;
    s_voice.state = VOICE_IDLE;
    pthread_mutex_unlock(&s_voice.lock);

    syslog(LOG_INFO, "[%s] recorded %zu bytes (with_text, stream=%p)\n",
        TAG, pcm_len, (void*)stream);

    /* Streaming ASR path: finish and copy text to caller.
     * In streaming mode audio goes directly to server, so
     * pcm_len may be 0 — that's OK, don't abort. */
    if (stream) {
        char text[512];
        int ret = voice_asr_stream_finish(stream, text,
            sizeof(text));

        v745_pcm_release();

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
        v745_pcm_release();
        syslog(LOG_ERR, "[%s] no PCM data for batch ASR\n", TAG);
        return -ENODATA;
    }

    syslog(LOG_INFO, "[%s] batch ASR (with_text): %zu bytes\n",
        TAG, pcm_len);
    syslog(LOG_INFO, "[%s] [P1] asr_go +%ldms\n", TAG, P1_MS());

    char text[512];
    int ret = voice_asr_recognize(s_voice.pcm_buf, pcm_len,
        text, sizeof(text));

    syslog(LOG_INFO, "[%s] [P1] asr_rsp +%ldms ret=%d\n", TAG,
           P1_MS(), ret);

    /* [V706] The existing "LLM latency" line in speak() is gated on
     * s_asr_done_ts, which was only ever set on the STREAM asr path -- and
     * this board always takes the batch path, so the number never printed.
     * Stamp it here too. */
    clock_gettime(CLOCK_MONOTONIC, &s_asr_done_ts);

    v745_pcm_release();

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

/* [DIAG] server-visible probe beacons (same trick as audio_playback.c):
 * each unique ping is logged by the TTS server, so we can see how far the
 * speak path got without a serial console. */
/* Still used by the V65 mic-level probe, which legitimately wants a /tts
 * request (that is how the peak value reaches the server). */
static void vc_beacon_cb(const unsigned char *d, size_t l, int f, void *u)
{
  (void)d; (void)l; (void)f; (void)u;
}
static void vc_beacon(const char *tag)
{
  /* [V68] Probes now travel on the bridge's lightweight /diag endpoint.
   * They used to call http_tts_speak_stream(), which made the server
   * synthesise speech and return a 30 kB PCM body for every probe tag: slow,
   * and it exercised the very audio path the probe was meant to observe. */
  diag_report(tag);
}

/* [V69] Probe carrying two numbers, e.g. "B7_SP_BUSY3_12000" = state 3 after
 * 12 s.  Enough to identify which state transition never happens, on a board
 * whose only usable output is the network. */

static void vc_probe(const char *tag, long a, long b)
{
  char tb[48];

  snprintf(tb, sizeof(tb), "%s%ld_%ld", tag, a, b);
  diag_report(tb);
}

/* ── 表情信号 ─────────────────────────────────────────────────────────
 * 把语音会话状态写入 /tmp/emotion.txt，faced 据此在屏幕左上角显示
 * 对应表情徽章（idle/happy/thinking/sad/listening/speaking/sleeping）。
 * 写文件开销极小（几十微秒），每状态切换一次即可。
 *
 * [V672] 本固件里 faced 没有启用（CONFIG_EXAMPLES_FACED 未设），屏幕上真正
 * 在跑的是 contest2026_307_xiaozhi_ui，它每 1 s 轮询 /tmp/ui_state.txt：
 *
 *     state=idle|listening|thinking|speaking
 *     chat=<要显示的字幕文字>
 *
 * 而 ai_agent 一侧从来没人写过这个文件 —— 这就是"按了 BOOT 屏幕没反应"
 * 的原因。下面两个函数补上写入端，并由上面这个 emotion_set() 统一转发，
 * 于是所有已有的 status 切换点（BOOT 按下/松开、录音结束、开始说话、
 * 说完）自动生效，不必逐个再插一刀。
 *
 * 写法是"整文件重写"：状态最多每几百毫秒变一次，微不足道。
 */

static char        s_ui_chat[192];
static char        s_ui_ask[192];   /* [V855] the recognised question */
static const char *s_ui_phase = "idle";

static void ui_state_flush(void)
{
  static const char ui_path[] = "/tmp/ui_state.txt";
  char buf[704];   /* [V855] state + ask + chat, each up to 192 B */
  int  n;
  int  fd;

  /* [V855] "ask=" is the question the panel shows in the upper bubble,
   * "chat=" the reply in the lower one. */
  /* [V857] beat= is a monotonic counter the UI reads.  The agent rewrites
   * this file once a second, so an unchanged beat for 15 s means the
   * writer is gone and whatever is on screen is a leftover -- the UI
   * clears it instead of showing stale text forever. */
  n = snprintf(buf, sizeof(buf), "state=%s\nask=%s\nchat=%s\nbeat=%lu\n",
               s_ui_phase != NULL ? s_ui_phase : "idle",
               s_ui_ask, s_ui_chat, g857_beat);
  if (n <= 0 || n >= (int)sizeof(buf))
    {
      return;
    }

  fd = open(ui_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    {
      return;
    }

  (void)write(fd, buf, (size_t)n);
  close(fd);
}

/* [V672] 设置会话阶段。listening 表示新的一轮开始，顺手把上一轮的回复
 * 字幕清掉，免得底部一直挂着上一次说的话。 */
/* [V857] *** /tmp/ui_state.txt heartbeat. ***
 *
 * ui_state_flush() used to run only on a phase change, so while the board
 * sat idle the file kept whatever it last had -- including text an NSH
 * session had written into it by hand, which then stayed on the panel for
 * hours and read as "the dialog boxes are stuck / in English".  Rewrite
 * the file once a second from the resident listen loop: the file is then
 * always the agent's real state, and any outside write is overwritten
 * within a second.
 *
 * time() has 1 s resolution here, so this fires about once a second no
 * matter how often it is called (the listen loop polls at 20 ms). */
static void ui_state_beat(void)
{
  static time_t s_last;
  time_t        now = time(NULL);

  if (s_last == 0)
    {
      s_last = now;
      return;
    }

  if (now - s_last < 1)
    {
      return;
    }

  s_last = now;
  g857_beat++;
  ui_state_flush();
}

/* [V857e] Nothing but the heartbeat.  250 ms so a beat lands well
 * inside any window the UI uses, and small enough (8 KB) that it is
 * never the thing that cannot be scheduled. */
static void *v857_beat_task(void *arg)
{
  (void)arg;

  syslog(LOG_ERR, "[%s] [V857e] ui_state heartbeat task up\n", TAG);

  while (!g857_beat_stop)
    {
      ui_state_beat();
      usleep(250000);
    }

  return NULL;
}

static void ui_state_set(const char *phase)
{
  const char *prev = s_ui_phase;

  s_ui_phase = (phase != NULL) ? phase : "idle";

  if (strcmp(s_ui_phase, "listening") == 0)
    {
      s_ui_chat[0] = '\0';
    }

  ui_state_flush();

  /* [V673] One line per transition, so the agent half of the status-bar chain
   * is visible on the console.  Prints the OLD value so the very first
   * "idle -> speaking" of a boot is reported too. */
  if (strcmp(prev, s_ui_phase) != 0)
    {
      syslog(LOG_INFO, "[%s] [V672] ui_state: %s -> %s\n",
             TAG, prev, s_ui_phase);
    }
}

/* [V672] 回复正文：在底部字幕里显示。 */

/* [V855] 问句正文：屏幕上方的“问”对话框。
 * 与 s_ui_chat 分开存放，因为两者的生命周期不同：
 * 问句在下一次识别到时才被覆盖，而回答在
 * 新一轮开始听时就会被清掉（ui_state_set("listening")。 */
static void ui_state_set_ask(const char *text)
{
  if (text != NULL)
    {
      snprintf(s_ui_ask, sizeof(s_ui_ask), "%s", text);
    }
  else
    {
      s_ui_ask[0] = '\0';
    }

  ui_state_flush();
}
static void ui_state_set_chat(const char *text)
{
  if (text != NULL)
    {
      snprintf(s_ui_chat, sizeof(s_ui_chat), "%s", text);
    }
  else
    {
      s_ui_chat[0] = '\0';
    }

  ui_state_flush();
}

static void emotion_set(const char *name)
{
#ifdef CONFIG_LIBC_TMPDIR
  static const char emo_path[] = CONFIG_LIBC_TMPDIR "/emotion.txt";
#else
  static const char emo_path[] = "/tmp/emotion.txt";
#endif
  int fd;

  fd = open(emo_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    {
      goto ui_state;
    }

  (void)write(fd, name, strlen(name));
  close(fd);

ui_state:
  /* [V672] 同步给屏幕上真正在跑的那个 UI。
   *   listening / thinking / speaking  -> 顶部状态条
   *   其它（idle/happy/sad/sleeping...） -> 收起来 */
  if (strcmp(name, "listening") == 0)
    {
      ui_state_set("listening");
    }
  else if (strcmp(name, "thinking") == 0)
    {
      ui_state_set("thinking");
    }
  else if (strcmp(name, "speaking") == 0)
    {
      ui_state_set("speaking");
    }
  else
    {
      ui_state_set("idle");
    }
}

/* [SPK-AUDIT] 播报审计：每次本地播报（含被拦的）追加一行到 /data/spk_audit.txt。
 * 目的：串口日志被 LCDDIAG 刷屏冲掉、开机早期永远抓不到，只能落盘后开机 cat。
 * who = LOCAL / LOCAL-BLOCKED / LAMP 等；/data 挂不上时退到 /tmp。 */
void spk_audit(const char *who, const char *text)
{
    if (who == NULL) {
        who = "?";
    }
    if (text == NULL) {
        text = "(null)";
    }
    int fd = open("/data/spk_audit.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) {
        fd = open("/tmp/spk_audit.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
    }
    if (fd < 0) {
        return;
    }
    {   /* [SPK-AUDIT] 单调时钟秒数，便于与用户听到的时刻对齐 */
        struct timespec ts_;
        long mono_ = (clock_gettime(CLOCK_MONOTONIC, &ts_) == 0) ? (long)ts_.tv_sec : -1L;
        printf("[@@AUD] %ld|%s|%s\n", mono_, who, text);
    }
    char aline[200];
    long mono2_ = -1L;
    {
        struct timespec ts2_;
        if (clock_gettime(CLOCK_MONOTONIC, &ts2_) == 0) {
            mono2_ = (long)ts2_.tv_sec;
        }
    }
    int an = snprintf(aline, sizeof(aline), "%ld|%s|%s\n", mono2_, who, text);
    if (an > (int)sizeof(aline) - 1) {
        an = (int)sizeof(aline) - 1;
    }
    if (an > 0) {
        ssize_t ignored = write(fd, aline, (size_t)an);
        (void)ignored;
    }
    close(fd);
}


/* [TTS-CACHE 2026-10-27] 内置离线语音。
 *
 * 背景：本地回复（"好的，这就为您点亮台灯" 等）走 voice_channel_speak ->
 * MiMo 云端 TTS。2026-10-27 该 key 返回 HTTP 429 "quota exhausted"，
 * volc 后端又没有凭据，结果屏幕有字幕、喇叭一声不吭。
 *
 * 办法：把固定回复的 PCM 预先合成好烧进固件（Windows SAPI，16kHz/16bit/mono，
 * 见 tts_offline_data.h），播报前先按文本精确匹配；命中就直接写播放设备，
 * 不联网、不消耗额度，也不受断网影响。
 *
 * 返回 0 = 已用离线语音播完；-1 = 没有对应的离线片段，调用方走原网络路径。 */
static int tts_offline_play(const char *text, audio_playback_t *pb)
{
  size_t i;

  if (text == NULL || pb == NULL)
    {
      return -1;
    }

  for (i = 0; i < TTS_OFF_COUNT; i++)
    {
      const tts_off_entry_t *e = &g_tts_off[i];
      size_t off;

      if (strcmp(e->text, text) != 0)
        {
          continue;
        }

      syslog(LOG_WARNING, "[TTS-CACHE] offline clip hit: \"%s\" (%u B)\n",
             e->text, (unsigned)e->len);

      /* [TTS-CACHE-FIX] 必须按实时速率喂数据。
       * 播放管线只保留 8 帧（8 x 4096B = 1 秒）在飞，一次性把 70 KB 全推
       * 进去会把管线撑爆：实测 [audio_direct] i2s_send failed: -12（ENOMEM），
       * 结果只有前几帧出声。4096 B @16 kHz/16 bit = 128 ms，所以每帧写完
       * 等 120 ms；写失败再小步重试。 */
      for (off = 0; off < e->len; off += 4096u)
        {
          size_t n = e->len - off;
          int    tries = 0;

          if (n > 4096u)
            {
              n = 4096u;
            }

          while (audio_playback_write(pb, e->pcm + off, n) < 0 && tries++ < 40)
            {
              usleep(20000);
            }

          usleep(120000);
        }

      return 0;
    }

  return -1;
}

int voice_channel_speak(const char* text)
{
    if (!text || text[0] == '\0') {
        return -EINVAL;
    }

    vc_beacon("B7_SP_ENTER");
    spk_audit("LOCAL", text);          /* [SPK-AUDIT] 谁让它说的、说了什么 */
    /* [V1027] Who is speaking, and what?  The board reports a
     * boot-time weather line played three times; this names the
     * caller of every synthesised line.  printf, not syslog:
     * the ramlog drainer dies early and syslog never reaches
     * the console. */
    printf("[V1027] speak: %.48s\n", text);

    /* [SPEAK-EPOCH] 记录入队时代：排队期间被打断则自弃 */
    unsigned my_epoch = s_speak_epoch;

    /* ── Serialize concurrent speak calls ── */
    /* [SPEAK-QUEUE 2026-10-29] 旧逻辑在入场时掐掉上一句的播放——但云端
     * 长回答的下一句文本总在上一句还在播时就到达（诗行播 5~19s，LLM
     * 2~4s 就推下一句），回答于是自己打断自己成碎片（字幕全有、声音
     * 残缺 = "断断续续"）。speak_lock 本就完整串行化媒体路径（防
     * media_player 会话重叠的 1.3GB 分配崩溃），入场无需杀前一句。
     * 真正的用户打断由 voice_channel_start()/V719 的 abort + epoch++ 处理。
     * 等待数达上限则丢弃最陈旧尾句，防线程/DRAM 积压。 */
    if (s_speak_waiters >= 10) {
        syslog(LOG_WARNING,
               "[%s] [SPEAK-QUEUE] %u pending, drop stale tail: %.32s\n",
               TAG, s_speak_waiters, text);
        return -EBUSY;
    }
    s_speak_waiters++;

    pthread_mutex_lock(&s_voice.speak_lock);
    s_speak_waiters--;

    if (s_speak_epoch != my_epoch) {
        pthread_mutex_unlock(&s_voice.speak_lock);
        syslog(LOG_WARNING,
               "[%s] [SPEAK-EPOCH] stale after interrupt, drop: %.32s\n",
               TAG, text);
        return -ECANCELED;
    }

    /* ── [V67] Half-duplex hand-over instead of an instant -EBUSY ──
     *
     * The always-on listen loop keeps the channel in VOICE_RECORDING for
     * most of its cycle, so the old guard dropped the boot greeting every
     * time and also dropped replies that arrived while a session was open.
     * Raise s_speak_pending, which makes the listen loop finish the session
     * it is in, then wait for it to release the channel. */

    if (s_voice.state != VOICE_IDLE) {
        int waited_ms = 0;

        vc_probe("B7_SP_WAIT", (long)s_voice.state, 0);

        s_speak_pending = 1;

        while (s_voice.state != VOICE_IDLE && waited_ms < 12000) {
            usleep(100000);
            waited_ms += 100;
        }

        s_speak_pending = 0;

        if (s_voice.state != VOICE_IDLE) {
            pthread_mutex_unlock(&s_voice.speak_lock);
            syslog(LOG_WARNING,
                "[%s] speak: channel busy after %d ms\n", TAG, waited_ms);
            vc_probe("B7_SP_BUSY", (long)s_voice.state, (long)waited_ms);
            return -EBUSY;
        }

        vc_probe("B7_SP_GOT", (long)waited_ms, 0);

        syslog(LOG_INFO, "[%s] speak: channel acquired after %d ms\n",
            TAG, waited_ms);
    }

    /* Work on a mutable copy so we can strip markdown in-place */
    size_t text_len = strlen(text);
    char* clean = malloc(text_len + 1);

    if (!clean) {
        vc_beacon("B7_SP_NOMEM");
        pthread_mutex_unlock(&s_voice.speak_lock);
        return -ENOMEM;
    }

    memcpy(clean, text, text_len + 1);
    tts_strip_markdown(clean);

    /* If stripping left nothing, bail out */
    if (clean[0] == '\0') {
        vc_beacon("B7_SP_EMPTY");
        free(clean);
        pthread_mutex_unlock(&s_voice.speak_lock);
        return 0;
    }

    /* [NOWEATHER-TTS] 内容级永久禁播：用户明确要求板子永不播报该天气类记忆句。 */
    {
      /* [NOWEATHER-TTS v2] 顺序无关：气温/三度/晴 三词共现任二即拦，
       * 覆盖 LLM 换序改写（"晴天气温三度"等）。[DRAM-FIX] 全栈上判定。 */
      int nw_hits = (strstr(clean, "气温") != NULL)
                  + (strstr(clean, "三度") != NULL)
                  + (strstr(clean, "晴") != NULL);
      if (nw_hits >= 2)
      {
        syslog(LOG_WARNING,
            "[%s] [NOWEATHER-TTS] suppressed local tts\n", TAG);
        printf("[NOWEATHER-TTS] suppressed local tts\n");
        spk_audit("LOCAL-BLOCKED", text);   /* [SPK-AUDIT] 被禁播拦下的也记 */
        free(clean);
        pthread_mutex_unlock(&s_voice.speak_lock);
        return 0;
      }
    }

    syslog(LOG_INFO, "[%s] [P1] tts_go +%ldms\n", TAG, P1_MS());
    syslog(LOG_INFO, "[%s] speak: \"%.*s\" (%zu bytes)\n",
        TAG, 60, clean, strlen(clean));

    /* [V672] 把回复正文交给屏幕底部字幕（顶部状态条此时是"正在思考"，
     * 正文先出来，用户能一边听一边看）。 */
    ui_state_set_chat(clean);

    clock_gettime(CLOCK_MONOTONIC, &s_tts_start);
    s_tts_first_chunk = 0;
    tts_buf_reset();          /* [V869] start with an empty buffer */

    if (s_asr_done_ts.tv_sec > 0) {
        long llm_ms = (s_tts_start.tv_sec - s_asr_done_ts.tv_sec) * 1000
            + (s_tts_start.tv_nsec - s_asr_done_ts.tv_nsec) / 1000000;
        syslog(LOG_INFO, "[%s] LLM latency: %ldms\n", TAG, llm_ms);
    }

    /* Try streaming TTS first (callback per chunk) */
    vc_beacon("B7_SP_PRE");
    audio_playback_t* pb = audio_playback_open(
        AGENT_AUDIO_PLAYBACK_DEV,
        AGENT_TTS_WS_SAMPLE_RATE,
        AGENT_VOICE_CHANNELS,
        AGENT_VOICE_BITS);
    /* [V855] The reply owns the channel now (or TTS failed outright).
     * Either way the listener may run again: it stays out of the way on
     * its own because s_voice.tts_pb != NULL. */
    s_reply_pending = 0;

    vc_beacon(pb ? "B7_SP_POST_OK" : "B7_SP_POST_NULL");

    if (!pb) {
        syslog(LOG_ERR, "[%s] playback open failed; fetching TTS without "
            "playback (null-sink) so the request is still observable\n", TAG);
        /* Fall through with pb == NULL: tts_stream_cb is null-safe and drops
         * the PCM, but http_tts_speak_stream still issues the HTTP request,
         * letting us confirm the greeting path ran via the TTS server logs
         * even when the codec/playback device cannot be opened. */
    }

    /* Register active playback so voice_channel_start() can abort it */
    if (pb) {
        pthread_mutex_lock(&s_voice.lock);
        s_voice.tts_abort = 0;
        s_voice.tts_pb = pb;
        pthread_mutex_unlock(&s_voice.lock);

        emotion_set("speaking");   /* 开始说话：显示“说话”表情 */
    }

    /* Use the generic streaming entry point: it honours the selected
     * backend (MiMo) and falls back to the self-hosted HTTP service and
     * then to volc.  HTTP TTS/ASR need no paid credentials. */
    int ret;
    /* [TTS-CACHE] 固定回复优先用内置离线语音：不联网、不受 TTS 配额影响。 */
    if (tts_offline_play(clean, pb) == 0) {
        ret = 0;
        syslog(LOG_WARNING, "[TTS-CACHE] played offline, network TTS skipped\n");
    } else {
        ret = voice_tts_speak_stream(clean, tts_stream_cb, pb);
    }
    if (ret != 0) {
        syslog(LOG_WARNING,
            "[%s] streaming TTS failed (%d)\n", TAG, ret);
    }

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

    /* [V857e] The speaker has just stopped: give the room (and the
     * playback pipeline buffers) time to actually go quiet before
     * the microphone is trusted again. */

    g857_quiet_until = time(NULL) + VOICE_VAD_POSTPLAY_QUIET_S;
    V1024S(LOG_ERR, "[V857e]] post-playback quiet for %d s (until %ld)\n",
           VOICE_VAD_POSTPLAY_QUIET_S, (long)g857_quiet_until);

    /* [V869] The backend is finished; anything still buffered has
     * to reach the speaker before the device is closed. */
    tts_buf_flush(pb);
    if (pb) {
        audio_playback_close(pb);
    }
    free(clean);

    if (ret != 0) {
        syslog(LOG_ERR, "[%s] TTS stream failed: %d\n", TAG, ret);
        emotion_set("sad");   /* 播放失败：显示“难过”表情 */
        pthread_mutex_unlock(&s_voice.speak_lock);
        return ret;
    }

    emotion_set("happy");   /* 回复说完：开心一下 */

    struct timespec tend;
    clock_gettime(CLOCK_MONOTONIC, &tend);
    long total_ms = (tend.tv_sec - s_tts_start.tv_sec) * 1000
        + (tend.tv_nsec - s_tts_start.tv_nsec) / 1000000;
    syslog(LOG_INFO, "[%s] speak done: total %ldms (play wait %ldms)\n",
        TAG, total_ms, total_ms - net_ms);
    pthread_mutex_unlock(&s_voice.speak_lock);
    return 0;
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
} asr_task_args_t;

static void* asr_file_worker(void* arg)
{
    asr_task_args_t* a = (asr_task_args_t*)arg;
    unsigned char* pcm = NULL;
    size_t pcm_len = 0;

    int ret = read_pcm_file(a->pcm_path, &pcm, &pcm_len);

    if (ret != 0) {
        printf("Failed to read %s: %d\n", a->pcm_path, ret);
        free(a);
        return NULL;
    }

    printf("ASR: recognizing %zu bytes from %s ...\n",
        pcm_len, a->pcm_path);

    char text[512];

    ret = voice_asr_recognize(pcm, pcm_len, text, sizeof(text));
    free(pcm);

    if (ret == 0) {
        printf("ASR result: %s\n", text);

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

int voice_channel_test_asr(const char* pcm_path)
{
    if (!pcm_path) {
        printf("Usage: voice_test_asr <pcm_file>\n");
        return -EINVAL;
    }

    asr_task_args_t* args = malloc(sizeof(asr_task_args_t));

    if (!args) {
        printf("Error: out of memory\n");
        return -ENOMEM;
    }

    strncpy(args->pcm_path, pcm_path, sizeof(args->pcm_path) - 1);
    args->pcm_path[sizeof(args->pcm_path) - 1] = '\0';

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

void voice_channel_cleanup(void)
{
    voice_channel_stop();
}

