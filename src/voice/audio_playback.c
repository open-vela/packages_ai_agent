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

/* audio_playback.c - Direct ES8311 + I2S0-TX playback backend for VOX3.
 *
 * The previous native backend drove the codec through the NuttX AUDIO
 * upper-half (AUDIOIOC_* ioctls on /dev/audio/pcm0p).  On this board that
 * path received PCM from the TTS server but never produced sound: the opaque
 * framework left the ES8311 DAC output path disabled.  This backend instead
 * programs the ES8311 DAC and the ESP32-S3 I2S0 TX DMA directly, using the
 * exact register sequence the proven Arduino firmware (voice_chat_src) uses to
 * make the same hardware speak.  It bypasses the AUDIO upper-half entirely.
 *
 * The public API (audio_playback_open/write/stop/close) is unchanged so
 * voice_channel.c and the Makefile need no edits.
 */

#include "voice/audio_playback.h"
#include "agent_config.h"
#include "voice/http_tts.h"

#include <nuttx/config.h>
#include <nuttx/audio/audio.h>
#include <nuttx/audio/i2s.h>
#include <nuttx/i2c/i2c_master.h>
#include <arch/board/board.h>

/* The ESP32-S3 bus-init entry points are provided by the arch layer and are
 * not always surfaced through the board header in package builds. */
struct i2c_master_s;
struct i2s_dev_s;
FAR struct i2c_master_s *esp32s3_i2cbus_initialize(int port);
FAR struct i2s_dev_s    *esp32s3_i2sbus_initialize(int port);

/* [V222] Self-contained, ESP-IDF-ordered I2S0 TX one-shot write implemented in
 * arch/xtensa/src/esp32s3/esp32s3_i2s.c.  Declared here directly, exactly like
 * the two bus initialisers above, so no shared header has to change. */


#include <errno.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>          /* [V710] FILE/fopen/fgetc/fclose for /tmp/pp.txt */
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

/* Wiring mirrors esp32s3_board_audio.c */
#define ES_I2C_PORT  0
#define ES_I2C_ADDR  0x18
#define ES_I2C_FREQ  400000
#define ES_I2S_PORT  0

#define ES_SAMPLE_RATE  16000
#define ES_BITS         16
#define ES_CHANNELS     1

/* ES8311 register addresses */
#define ES_REG_RESET    0x00
#define ES_REG_CLKPOW   0x01
#define ES_REG_DAC_EQ   0x37
#define ES_REG_ANA1     0x0D
#define ES_REG_ANA2     0x0E
#define ES_REG_PWR_DAC  0x12
#define ES_REG_OUT_EN   0x13
#define ES_REG_DAC_VOL  0x32

static const char *TAG = "audio_direct";
static int g_tx_timeouts;

/* [V671] Path counters exported by esp32s3_i2s.c.  "full" counts transfers
 * that had to reconfigure and reset the transmitter, "light" the ones that
 * only re-armed the DMA.  A stutter fix that silently fell back to the old
 * per-buffer reset would show up here as light == 0. */
void esp32s3_i2s_v671_stats(uint32_t *full, uint32_t *light);

static struct i2c_master_s *g_i2c;
static struct i2s_dev_s    *g_i2s;
static bool                 g_inited;

struct audio_playback
{
  bool stopped;
};

/* ---- ES8311 I2C helpers ---- */

/* [V856] Snapshot of the register file as written by the ONE init that
 * verified.
 *
 * es8311_dac_init() is a 15-step read-modify-write chain and it is run
 * on EVERY audio_playback_open() -- i.e. before every single TTS reply.
 * Any one of those reads returning a bad byte writes that byte straight
 * back into the codec, and reg 0x14 of the chain is the "analogue mic,
 * not digital" bit: one bad read and the microphone goes silent while
 * every I2S register still looks perfect.
 *
 * That is the shape of the report -- works once, dead afterwards.
 *
 * So: run the real init once, remember every value it settled on, and
 * replay those values on later opens.  A replay is all absolute writes,
 * so there is no read left to get wrong. */
#define ES_SNAP_N 0x50
static uint8_t g_es_snap[ES_SNAP_N];
static uint8_t g_es_seen[ES_SNAP_N];
static int     g_es_snap_n;
static int     g_es_snap_ok;
static int     g_es_recording;

static int g_i2c_errs;

static void es_writereg(uint8_t reg, uint8_t val)
{
  int rc;

  /* [V856] Remember the last value written to each register while the
   * one real init is running. */
  if (g_es_recording && reg < ES_SNAP_N)
    {
      if (!g_es_seen[reg])
        {
          g_es_seen[reg] = 1;
          g_es_snap_n++;
        }
      g_es_snap[reg] = val;
    }

  if (!g_i2c)
    {
      syslog(LOG_ERR, "[%s] es_writereg(0x%02x): no i2c bus\n", TAG, reg);
      return;
    }

  uint8_t buf[2] = { reg, val };
  struct i2c_config_s cfg =
    {
      .frequency = ES_I2C_FREQ,
      .address   = ES_I2C_ADDR,
      .addrlen   = 7
    };

  rc = i2c_write(g_i2c, &cfg, buf, 2);
  if (rc < 0)
    {
      if (g_i2c_errs < 6)
        {
          g_i2c_errs++;
          syslog(LOG_ERR, "[%s] es_writereg(0x%02x,0x%02x) FAILED rc=%d\n",
                 TAG, reg, val, rc);
        }
    }
}

static uint8_t es_readreg(uint8_t reg)
{
  if (!g_i2c)
    {
      return 0;
    }

  uint8_t w = reg;
  uint8_t r = 0;
  struct i2c_config_s cfg =
    {
      .frequency = ES_I2C_FREQ,
      .address   = ES_I2C_ADDR,
      .addrlen   = 7
    };

  /* [V854] Both return codes used to be thrown away, so a failed transfer
   * silently returned whatever happened to be in r.  Every read-modify-
   * write in es8311_dac_init() then wrote that value straight back into
   * the codec -- including reg 0x14 ("analogue mic, not digital"), which
   * is how the microphone ended up reading pure silence while i2c_errs
   * still reported 0.  Check the codes and retry before giving up.
   */
  {
    int attempt;
    uint8_t prev = 0xff;

    /* [V855] v854 stopped throwing the return codes away, but a transfer
     * that reports success can still carry a corrupt byte -- the v853
     * captures had i2c_errs == 0 next to r00=0xff.  Require two
     * consecutive reads to agree before the value is trusted, because
     * every use of it is a read-modify-write: one bad byte is written
     * straight back into the codec, and reg 0x14 of those is exactly the
     * "analogue mic" bit that leaves the microphone silent. */
    for (attempt = 0; attempt < 8; attempt++)
      {
        int rc = i2c_write(g_i2c, &cfg, &w, 1);

        if (rc < 0)
          {
            usleep(1000);
            continue;
          }

        rc = i2c_read(g_i2c, &cfg, &r, 1);

        if (rc < 0)
          {
            usleep(1000);
            continue;
          }

        if (r == prev)
          {
            return r;
          }

        prev = r;
      }
  }

  return r;
}

/* Program ES8311 for DAC playback using the proven register sequence. */
/* [V811] Alternating DAC mute, to place the hiss on one side of the codec.
 *
 * Every digital measurement comes back clean: RX registers 0x2f3de38f on all
 * 146 captures, RX data OR 0x0000ffff (the driver's own classifier calls that
 * good), and the TX stream v810 dumps is an ordinary signed waveform.  I2C
 * reports no errors and v248's five register fixes are all in place.
 *
 * What that cannot separate is a hiss made inside the DAC from one made after
 * it -- amplifier, speaker wiring, supply.  At the speaker the two are
 * indistinguishable, and everything measured so far sits upstream of the split.
 *
 * So alternate it.  Four seconds on, four seconds off, forever, while the
 * playback stream keeps running untouched.  Listen across a transition:
 *
 *   hiss present in BOTH phases  -> nothing upstream is responsible;
 *                                    it is after the codec (analogue or board)
 *   hiss present only when on   -> it is inside the codec or before it
 *
 * The stream is never stopped, because stopping playback would change the
 * DAC's operating state and destroy the comparison.  Enabled only when
 * CONFIG_AI_AGENT_AUDIO_DAC_MUTE_ALT is set, so it costs nothing normally.
 */
/* [V811e] Not guarded by a config symbol on purpose.  The configuration for
 * this build is injected rather than declared in Kconfig (there is no entry for
 * it anywhere in the tree), so an #ifdef here would never be true and the whole
 * diagnostic would be dead code -- which is the mistake this comment exists to
 * prevent.  Remove this block once the split has been made. */
extern int es8311_ext_setmute(int mute);
/* [V858] Put the port back into its boot shape once this playback is over. */
extern void esp32s3_i2s_v858_after_playback(void);
static void v811_mute_alternate(void)
{
  /* [V811-RETIRED 2026-10-29] 交替静音实验已拆除: 它每 ~96 次写调用
   * (约 12 s 音频)硬切一次 DAC 静音, 后几次播报因此忽响忽哑, 被用户
   * 描述为"一卡一卡"。函数保留为空以维持调用点与告警面不变;
   * 静音状态由每次 open 的 [V856] 快照重放固定为不静音。 */
}

/* [V856] Re-apply every register the verified init settled on, in
 * ascending register order.  Pure writes: no read can go wrong, so the
 * microphone cannot be clobbered by this path.
 *
 * Writing 0x00 (0x80) part-way through resets the codec, which is fine:
 * every remaining register is written right after it with exactly the
 * value the full sequence produced, so the end state is identical. */
static void es8311_replay_snapshot(void)
{
  int i;

  for (i = 0; i < ES_SNAP_N; i++)
    {
      if (g_es_seen[i])
        {
          es_writereg((uint8_t)i, g_es_snap[i]);
        }
    }
}

static void es8311_dac_init(void)
{
  uint8_t rv;

  /* [V248] *** ES8311 initialisation rewritten to the official esp_codec_dev
   * sequence, taken from the reference source tree that ships with the working
   * firmware on this board
   *   <VOX3>/ai_vox_ref/ai_vox-main/src/core/espressif_esp_codec_dev/es8311.c
   * (es8311_open() + es8311_set_bits_per_sample() + es8311_config_fmt() +
   *  es8311_config_sample() + es8311_start()).
   *
   * The previous table was wrong in five places, two of which are fatal:
   *
   *   reg 0x07 (LRCK divider high)  was 0x3F, official 0x00
   *   reg 0x08 (LRCK divider low)   was 0x10, official 0xFF
   *   reg 0x04 (DAC OSR)            was 0x10, official 0x20
   *   reg 0x44 (internal reference) was 0x50, official 0x58
   *   reg 0x37 (DAC equaliser)      was 0x48, official 0x08
   *
   * 0x07/0x08 are the codec's LRCK division for MCLK = 4096 kHz / fs = 16 kHz
   * (coeff_div[] entry {4096000, 16000, ... lrck_h 0x00, lrck_l 0xff ...}).
   * With them wrong the codec's internal sample clock does not match the I2S
   * frame at all, so the DAC only ever emits its noise floor -- the hiss.
   */

  /* ---- es8311_open(): power-on sequence ---- */

  es_writereg(0x44, 0x08);            /* I2C noise immunity (twice)      */
  es_writereg(0x44, 0x08);

  es_writereg(0x01, 0x30);
  es_writereg(0x02, 0x00);
  es_writereg(0x03, 0x10);
  es_writereg(0x16, 0x24);
  es_writereg(0x04, 0x10);
  es_writereg(0x05, 0x00);
  es_writereg(0x0B, 0x00);
  es_writereg(0x0C, 0x00);
  es_writereg(0x10, 0x1F);
  es_writereg(0x11, 0x7F);
  es_writereg(0x00, 0x80);

  rv = es_readreg(0x00) & 0xBF;       /* slave mode                      */
  es_writereg(0x00, rv);

  es_writereg(0x01, 0x3F);            /* MCLK from pin, not inverted     */

  rv = es_readreg(0x06) & ~0x20;      /* SCLK not inverted               */
  es_writereg(0x06, rv);

  es_writereg(0x13, 0x10);
  es_writereg(0x1B, 0x0A);
  es_writereg(0x1C, 0x6A);

  es_writereg(0x44, 0x58);            /* internal reference ADCL + DACR  */

  /* ---- 16-bit word length ---- */

  rv = es_readreg(0x09) | 0x0C;
  es_writereg(0x09, rv);
  rv = es_readreg(0x0A) | 0x0C;
  es_writereg(0x0A, rv);

  /* ---- format: standard I2S (clear bit 6 on both directions) ---- */

  rv = es_readreg(0x09) & ~0x40;
  es_writereg(0x09, rv);
  rv = es_readreg(0x0A) & ~0x40;
  es_writereg(0x0A, rv);

  /* ---- clock: MCLK 4096 kHz, fs 16000 Hz (coeff_div entry) ---- */

  rv = es_readreg(0x02) & 0x07;       /* pre_div = 1, pre_multi = 1      */
  es_writereg(0x02, rv);

  es_writereg(0x05, 0x00);            /* adc_div = dac_div = 1           */

  rv = (es_readreg(0x03) & 0x80) | 0x10;
  es_writereg(0x03, rv);              /* fs_mode = 0, adc_osr = 0x10     */

  rv = (es_readreg(0x04) & 0x80) | 0x20;
  es_writereg(0x04, rv);              /* dac_osr = 0x20                  */

  rv = es_readreg(0x07) & 0xC0;       /* lrck_h = 0x00                   */
  es_writereg(0x07, rv);

  es_writereg(0x08, 0xFF);            /* lrck_l = 0xFF                   */

  rv = (es_readreg(0x06) & 0xE0) | (0x04 - 1);
  es_writereg(0x06, rv);              /* bclk_div = 4                    */

  /* ---- es8311_start(): enable the analogue path ---- */

  rv = es_readreg(0x09) & ~0x40;
  es_writereg(0x09, rv);
  rv = es_readreg(0x0A) & ~0x40;
  es_writereg(0x0A, rv);

  es_writereg(0x17, 0xBF);
  es_writereg(0x0E, 0x02);
  es_writereg(0x12, 0x00);
  es_writereg(0x14, 0x1A);

  rv = es_readreg(0x14) & ~0x40;      /* analogue mic, not digital       */
  es_writereg(0x14, rv);

  es_writereg(0x0D, 0x01);
  es_writereg(0x15, 0x40);
  es_writereg(0x37, 0x08);
  es_writereg(0x45, 0x00);

  /* ---- DAC volume ---- */

  es_writereg(ES_REG_DAC_VOL, 0xBF);
}

/* ---- I2S TX completion callback ---- */

static void direct_tx_done(FAR struct i2s_dev_s *dev,
                           FAR struct ap_buffer_s *apb,
                           FAR void *arg, int result)
{
  sem_t *done = (sem_t *)arg;
  if (done)
    {
      sem_post(done);
    }
}

/* [V207] CRITICAL FIX.
 *
 * This probe used to call http_tts_speak_stream(), i.e. it asked the TTS
 * server to synthesise speech *from inside audio_playback_open()*.  The open
 * path runs under voice_channel's speak_lock, so the first playback of every
 * boot paid four extra blocking HTTP round trips (4 attempts x 15 s connect +
 * 8 s recv budget) before a single sample reached the codec -- and when the
 * link was slow the whole speak path never returned, which is exactly the
 * "board goes silent ~10 s after boot" symptom.  The beacon also exercised
 * the very audio path it was meant to observe, so it could never have been a
 * valid probe.
 *
 * Probes now travel on the bridge's lightweight /diag endpoint (3 s connect
 * cap) and are always mirrored to syslog so they show up on the serial
 * console as well. */
static void ap_beacon(const char *tag)
{
  syslog(LOG_INFO, "[%s] probe %s\n", TAG, tag);
  diag_report(tag);
}

/* ---- public API ---- */

/* [V846] Echo-gate state.  g_ap_in_use is raised while a playback
 * session is open and dropped once its last transfer has been reaped;
 * g_ap_stop_us records when that happened so the gate can hold for a
 * short tail afterwards. */
static volatile int      g_ap_in_use;
static volatile uint64_t g_ap_stop_us;


audio_playback_t *audio_playback_open(const char *dev_path,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample)
{
  struct audio_playback *pb;

  if (!g_inited)
    {
      g_i2c = esp32s3_i2cbus_initialize(ES_I2C_PORT);
      g_i2s = esp32s3_i2sbus_initialize(ES_I2S_PORT);
      if (!g_i2c || !g_i2s)
        {
          syslog(LOG_ERR, "[%s] i2c/i2s init failed (i2c=%p i2s=%p)\n",
                 TAG, g_i2c, g_i2s);
          return NULL;
        }

      ap_beacon("B7_AP_BUSOK");
      g_inited = true;
    }

  /* Configure codec DAC + I2S0 TX for 16k/16bit/mono.  These ops also start
   * the TX channel, so playback begins as soon as we push PCM. */
  /* [V854] Program the codec, then PROVE it took.
   *
   * es8311_dac_init() is a long chain of read-modify-write steps and it
   * also brings up the microphone path (0x44 reference ADCL+DACR, 0x14
   * "analogue mic, not digital").  When the read behind one of those steps
   * returns garbage the wrong value is written back, the ADC never starts,
   * and the microphone reads silence -- while every I2S register still
   * looks correct, which is why this went unnoticed for so long.
   *
   * Measured across builds: r00=0x80 with r32=0xbf means the mic works;
   * r00=0x20 / 0x0a / 0xff means it reads 0 -- and i2c_errs is 0 either
   * way, so the failure is invisible unless you verify.
   *
   * So verify, and re-run the whole sequence when it does not check out.
   * Note the old message claimed "want 0xCB"; the sequence writes 0xBF,
   * so 0xbf is the value that actually means success.
   */
  {
    uint8_t rb0  = 0;
    uint8_t rb32 = 0;
    int     attempt;

    for (attempt = 0; attempt < 5; attempt++)
      {
        /* [V856] First boot (or a codec that will not verify) runs the
         * real init with the snapshot recorder on.  Every later open
         * replays it. */
        if (g_es_snap_ok && attempt == 0)
          {
            es8311_replay_snapshot();
          }
        else
          {
            g_es_recording = 1;
            es8311_dac_init();
            g_es_recording = 0;
          }

        usleep(10000);

        rb0  = es_readreg(ES_REG_RESET);
        rb32 = es_readreg(ES_REG_DAC_VOL);

        if (rb0 == 0x80 && rb32 == 0xBF)
          {
            if (!g_es_snap_ok && g_es_seen[ES_REG_RESET])
              {
                g_es_snap_ok = 1;
                syslog(LOG_INFO,
                       "[%s] [V856] codec snapshot captured: %d regs "
                       "(r00=0x%02x r32=0x%02x)\n",
                       TAG, g_es_snap_n, rb0, rb32);
              }
            break;
          }

        usleep(50000);
      }

    if (rb0 != 0x80 || rb32 != 0xBF)
      {
        syslog(LOG_ERR,
               "[%s] [V854] codec did not verify after %d attempts "
               "(r00=0x%02x r32=0x%02x) -- the mic path is probably dead\n",
               TAG, attempt, rb0, rb32);
      }
    else if (attempt > 0)
      {
        syslog(LOG_WARNING,
               "[%s] [V854] codec verified on attempt %d\n",
               TAG, attempt + 1);
      }

    syslog(LOG_INFO,
           "[%s] codec readback: r00=0x%02x (want 0x80) r32=0x%02x (want 0xbf) "
           "i2c_errs=%d\n", TAG, rb0, rb32, g_i2c_errs);
  }
  ap_beacon("B7_AP_ESOK");

  /* [V208] Every one of these returns a meaningful value: a 0 from
   * I2S_TXSAMPLERATE means i2s_check_mclkfrequency() rejected the rate, in
   * which case the TX channel is left STOPPED and I2S_SEND never completes
   * (the write then blocks forever, because the timeout we pass is 0). */
  {
    uint32_t dw = I2S_TXDATAWIDTH(g_i2s, ES_BITS);
    uint32_t mk = I2S_SETMCLKFREQUENCY(g_i2s, ES_SAMPLE_RATE * 256);
    int      ch = I2S_TXCHANNELS(g_i2s, ES_CHANNELS);
    uint32_t sr = I2S_TXSAMPLERATE(g_i2s, ES_SAMPLE_RATE);

    syslog(LOG_INFO,
           "[%s] i2s cfg: datawidth=%u(rc) mclk=%u rc_txchannels=%d "
           "samplerate_rc=%u\n", TAG, (unsigned)dw, (unsigned)mk, ch,
           (unsigned)sr);
  }
  ap_beacon("B7_AP_I2SOK");

  pb = calloc(1, sizeof(struct audio_playback));
  if (!pb)
    {
      return NULL;
    }

  pb->stopped = false;
  syslog(LOG_INFO,
         "[%s] direct playback opened (req rate=%u ch=%u bits=%u)\n",
         TAG, sample_rate, channels, bits_per_sample);
  ap_beacon("B7_AP_RET");
  g_ap_in_use++;   /* [V846] speaker is live from here */
  return pb;
}

/* ── [V710] TX pipeline + turnaround instrumentation ────────────────────
 *
 * Until v709 audio_playback_write() ran ap_write_once() per chunk: submit,
 * then block in sem_timedwait() until that transfer retired.  The driver
 * keeps at most one buffer in priv->tx.act, so at the moment the DMA EOF
 * ISR fires priv->tx.pend is empty, the transmitter stops, and the next
 * buffer cannot be armed until alloc + memcpy + I2S_SEND have finished in
 * the caller.  The TX FIFO holds ~64 samples (4 ms at 16 kHz); any longer
 * turnaround is silence, and at one chunk per 64 ms that is a hole per
 * syllable -- the reported stutter.
 *
 * Keeping a window of transfers in flight fixes it structurally: the next
 * chunk is submitted *before* the oldest is retired, so the ISR always finds
 * a descriptor waiting in tx.pend and the FIFO never runs dry.
 *
 * /tmp/pp.txt selects the window depth and is re-read every 64 calls:
 *     absent / anything else -> g_pipe_depth (built-in default)
 *     '0'                    -> off, i.e. the exact v709 path
 *     '1'..'(AP_PIPE_MAX-1)' -> that many transfers in flight  [V870]
 *
 * The counters are reported once per playback session by
 * audio_playback_close(); turn_avg / turn_max / slow are the audible holes.
 */

#define AP_PIPE_MAX 8        /* [V870] was 4: a 375 KB reply still
                              * showed turn_max 230 ms at depth 4,
                              * because the window was pinned at the
                              * ceiling and could not absorb more. */

struct ap_flight
{
  FAR struct ap_buffer_s *apb;
  sem_t                   done;
  int                     live;
  int                     quarantined;   /* [V711] leaked: never free/re-use */
};

static struct ap_flight g_flight[AP_PIPE_MAX];
static int              g_flight_q[AP_PIPE_MAX];   /* live slots, oldest first */
static int              g_flight_qn;

static int              g_pipe_depth = 8;   /* [V870] was 4: 8 x 127ms = 1 s in flight */
static unsigned         g_pipe_tick;

static uint32_t         g_ap_writes;
static uint32_t         g_ap_chunks;
static uint64_t         g_ap_bytes;
static uint64_t         g_ap_busy_us;
static uint64_t         g_ap_turn_us;
static uint32_t         g_ap_turn_n;
static uint32_t         g_ap_turn_max;
static uint32_t         g_ap_slow;
static uint64_t         g_ap_last_us;

static uint64_t ap_now_us(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

static void ap_turn_note(uint64_t turn)
{
  g_ap_turn_us += turn;
  g_ap_turn_n++;

  if (turn > (uint64_t)g_ap_turn_max)
    {
      g_ap_turn_max = (uint32_t)(turn > 0xffffffffull ? 0xffffffffull : turn);
    }

  if (turn > 5000ull)
    {
      g_ap_slow++;
    }
}

static void ap_pipe_refresh(void)
{
  FILE *f;
  int   c;

  if ((g_pipe_tick++ & 0x3fu) != 0u)
    {
      return;
    }

  f = fopen("/tmp/pp.txt", "r");

  if (f != NULL)
    {
      c = fgetc(f);
      fclose(f);

      if (c >= '0' && c <= '0' + (AP_PIPE_MAX - 1))   /* [V870] */
        {
          g_pipe_depth = c - '0';
        }
    }
}

static void ap_pipe_drop(struct ap_flight *f)
{
  int idx = (int)(f - g_flight);
  int i;

  for (i = 0; i < g_flight_qn; i++)
    {
      if (g_flight_q[i] == idx)
        {
          int j;

          for (j = i; j + 1 < g_flight_qn; j++)
            {
              g_flight_q[j] = g_flight_q[j + 1];
            }

          g_flight_qn--;
          break;
        }
    }
}

static struct ap_flight *ap_pipe_free(void)
{
  int i;

  for (i = 0; i < AP_PIPE_MAX; i++)
    {
      if (!g_flight[i].live)
        {
          return &g_flight[i];
        }
    }

  return NULL;
}

static int ap_retire(struct ap_flight *f)
{
  struct timespec ts;
  uint64_t        t0;
  int             wrc;

  if (f == NULL || !f->live)
    {
      return 0;
    }

  if (f->quarantined)
    {
      /* [V711] Already written off.  Waiting again would burn the whole
       * budget for a transfer that is never going to be reported, so fail
       * fast instead. */

      return -ETIMEDOUT;
    }

  /* [V711] *** CLOCK_REALTIME, not CLOCK_MONOTONIC. ***
   *
   * sem_timedwait() is specified against CLOCK_REALTIME and this NuttX
   * forwards it verbatim:
   *
   *     int sem_timedwait(FAR sem_t *sem, FAR const struct timespec *abstime)
   *     {
   *       return sem_clockwait(sem, CLOCK_REALTIME, abstime);
   *     }
   *
   * v710 built the deadline from CLOCK_MONOTONIC, i.e. uptime.  CLOCK_REALTIME
   * on this board is NTP-set to 2026 and the two clocks are ~1.79e9 s apart,
   * so "uptime + 1.5 s" was already in the past and every single wait
   * returned ETIMEDOUT immediately -- the log shows three "1.5 second"
   * timeouts inside 62 ms (36.365, 36.365, 36.427).  The driver was then told
   * that a live transfer had died, the apb was freed and the semaphore
   * destroyed under a DMA that was still running, and the resulting
   * container / bounce-slot imbalance killed the board inside
   * i2s_buf_allocate().  ap_write_once() below has always used CLOCK_REALTIME;
   * this now matches it exactly. */

  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec  += 1;
  ts.tv_nsec += 500000000;          /* +1.5 s, same budget as before */

  if (ts.tv_nsec >= 1000000000)
    {
      ts.tv_sec  += 1;
      ts.tv_nsec -= 1000000000;
    }

  t0  = ap_now_us();
  wrc = sem_timedwait(&f->done, &ts);
  g_ap_busy_us += ap_now_us() - t0;

  if (wrc < 0)
    {
      uint32_t spent_ms = (uint32_t)((ap_now_us() - t0) / 1000ull);

      if (g_tx_timeouts < 4)
        {
          g_tx_timeouts++;
          syslog(LOG_ERR,
                 "[%s] I2S TX completion TIMEOUT after %lums (n=%d) -- the "
                 "TX DMA never drained; speaker cannot sound\n",
                 TAG, (unsigned long)spent_ms, g_tx_timeouts);
        }

      /* [V711] *** Quarantine, do not free. ***
       *
       * A genuine timeout means the TX DMA still owns this transfer and its
       * completion callback is still going to post &f->done.  Returning the
       * apb to the heap now lets the very next apb_alloc() mint the same
       * block (v710: "apb free ... refcount=1" followed immediately by
       * "apb new apb=0x3c221628"), and sem_destroy() on a semaphore that is
       * about to be posted corrupts the object the next flight re-inits in
       * this slot.  Leak both instead: park the flight so ap_pipe_free()
       * never hands it out again.  Losing a slot is recoverable; corrupting
       * the heap is not. */

      f->quarantined = 1;
      g_ap_last_us   = ap_now_us();
      return -ETIMEDOUT;
    }

  apb_free(f->apb);
  sem_destroy(&f->done);
  f->apb  = NULL;
  f->live = 0;
  g_ap_last_us = ap_now_us();
  return 0;
}

static int ap_arm(struct ap_flight *f, const void *buf, size_t len)
{
  struct audio_buf_desc_s desc;
  FAR struct ap_buffer_s *apb = NULL;
  uint64_t                now;
  int                     ret;

  if (g_i2s == NULL)
    {
      return -ENODEV;
    }

  desc.numbytes  = len;
  desc.u.pbuffer = &apb;

  ret = apb_alloc(&desc);
  if (ret < 0 || apb == NULL)
    {
      syslog(LOG_ERR, "[%s] apb_alloc(%u) failed: %d\n",
             TAG, (unsigned)len, ret);
      return ret < 0 ? ret : -ENOMEM;
    }

  /* [V711] Defensive: g_flight_q holds at most one entry per live slot, so
   * this can never trip -- but an out-of-range store here would be silent
   * corruption, so drop the transfer instead. */

  if (g_flight_qn >= AP_PIPE_MAX)
    {
      apb_free(apb);
      return -ENOMEM;
    }

  memcpy(apb->samp, buf, len);
  apb->nbytes   = len;
  apb->curbyte  = 0;
  apb->nsamples = len / (ES_BITS / 8 * ES_CHANNELS);

  sem_init(&f->done, 0, 0);
  f->apb         = apb;
  f->live        = 1;
  f->quarantined = 0;

  ret = I2S_SEND(g_i2s, apb, direct_tx_done, &f->done, MSEC2TICK(500));
  if (ret < 0)
    {
      syslog(LOG_ERR, "[%s] i2s_send failed: %d\n", TAG, ret);
      apb_free(apb);
      sem_destroy(&f->done);
      f->apb  = NULL;
      f->live = 0;
      return ret;
    }

  now = ap_now_us();

  if (g_ap_last_us != 0ull)
    {
      ap_turn_note(now - g_ap_last_us);
    }

  g_flight_q[g_flight_qn++] = (int)(f - g_flight);
  g_ap_chunks++;
  g_ap_bytes += (uint64_t)len;
  return (int)len;
}

static void ap_drain_all(void)
{
  while (g_flight_qn > 0)
    {
      struct ap_flight *f = &g_flight[g_flight_q[0]];

      ap_retire(f);
      ap_pipe_drop(f);
    }

  g_ap_last_us = 0ull;
}

/* [V212] Upper bound for a single transfer.
 *
 * The I2S driver copies every transfer into an internal-SRAM DMA bounce
 * buffer (esp32s3_i2s.c, I2S_DMABUF_SLOT_SZ) because ESP32-S3 GDMA cannot
 * fetch from PSRAM, and that slot is deliberately small to keep .bss off the
 * already-tight internal heap.  Anything larger is refused by the driver, so
 * audio_playback_write() splits the caller's buffer into chunks of this
 * size. */
#define AP_WRITE_CHUNK_MAX  4064

/* Single DMA transfer.  Must never be called with len > AP_WRITE_CHUNK_MAX;
 * use audio_playback_write() instead. */
static int ap_write_once(audio_playback_t *pb, const void *buf, size_t len)
{
  struct audio_buf_desc_s desc;
  FAR struct ap_buffer_s *apb = NULL;
  sem_t done;
  int ret;

  if (!pb)
    {
      return -EINVAL;
    }

  if (pb->stopped)
    {
      return -ECANCELED;
    }

  if (!g_i2s)
    {
      return -ENODEV;
    }

  if (len == 0)
    {
      return 0;
    }

  desc.numbytes  = len;
  desc.u.pbuffer = &apb;
  ret = apb_alloc(&desc);
  if (ret < 0 || apb == NULL)
    {
      syslog(LOG_ERR, "[%s] apb_alloc(%u) failed: %d\n",
             TAG, (unsigned)len, ret);
      return ret < 0 ? ret : -ENOMEM;
    }

  memcpy(apb->samp, buf, len);
  apb->nbytes   = len;
  apb->curbyte  = 0;
  apb->nsamples = len / (ES_BITS / 8 * ES_CHANNELS);

  sem_init(&done, 0, 0);

  /* [V208] The driver treats timeout==0 as "no timeout", so a TX channel that
   * never drains turned every write into a permanent block -- the board simply
   * stopped producing log output and no sound.  Give the transfer a real
   * deadline and a backstop wait so a dead DMA channel degrades into a logged
   * error the agent can survive. */
  ret = I2S_SEND(g_i2s, apb, direct_tx_done, &done, MSEC2TICK(500));
  if (ret < 0)
    {
      syslog(LOG_ERR, "[%s] i2s_send failed: %d\n", TAG, ret);
      apb_free(apb);
      sem_destroy(&done);
      return ret;
    }

  /* [V671] *** Nothing happens between the queue and the wait any more. ***
   *
   * v663..v668 ran their rate probes right here: esp32s3_i2s_v663_lrck()
   * busy-waited 50 ms on two transfers while re-enabling the IO_MUX input
   * buffers of the very pads the I2S was driving, esp32s3_i2s_v666_force_mono()
   * rewrote three TX registers plus a TX_UPDATE on every transfer, and each
   * transfer printed one or two syslog records.  All of that landed in the gap
   * between two DMA transfers, where the TX FIFO holds only about 64 samples
   * (4 ms at 16 kHz), so every probe became an audible dropout.
   *
   * The probes have served their purpose (the V669 TDM fix, the V670 clock
   * fix and the V671 light re-arm are all documented in esp32s3_i2s.c), and
   * the write path is silent again.  The one remaining report is a single
   * line per utterance from audio_playback_close(). */

  {
    struct timespec ts;
    int wrc;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += 1;
    ts.tv_nsec += 500000000;          /* +1.5 s */
    if (ts.tv_nsec >= 1000000000)
      {
        ts.tv_sec  += 1;
        ts.tv_nsec -= 1000000000;
      }

    wrc = sem_timedwait(&done, &ts);
    if (wrc < 0)
      {
        if (g_tx_timeouts < 4)
          {
            g_tx_timeouts++;
            syslog(LOG_ERR,
                   "[%s] I2S TX completion TIMEOUT after 1.5 s (n=%d) -- the "
                   "TX DMA never drained; speaker cannot sound\n",
                   TAG, g_tx_timeouts);
          }

        apb_free(apb);
        sem_destroy(&done);
        return -ETIMEDOUT;
      }
  }

  apb_free(apb);
  sem_destroy(&done);

  return pb->stopped ? -ECANCELED : (int)len;
}

/* [V212] Public entry point: splits arbitrarily large PCM buffers into
 * transfers the internal-SRAM DMA slot can hold, and reports partial progress
 * if a later chunk fails. */
int audio_playback_write(audio_playback_t *pb, const void *buf, size_t len)
{
  /* [V811d] Drive the alternating mute from the playback path itself, so the
   * switch only ever runs while audio is actually flowing -- which is when the
   * comparison is valid. */
  {
    static unsigned long v811d_ticks;

    if ((v811d_ticks++ % 96u) == 0u)
      {
        v811_mute_alternate();
      }
  }

  const unsigned char *p = (const unsigned char *)buf;
  size_t done = 0;
  int    depth;

  if (pb == NULL)
    {
      return -EINVAL;
    }

  if (len == 0)
    {
      return 0;
    }

  ap_pipe_refresh();
  depth = g_pipe_depth;

  if (depth > AP_PIPE_MAX)
    {
      depth = AP_PIPE_MAX;
    }

  g_ap_writes++;

  if (pb->stopped && g_flight_qn > 0)
    {
      ap_drain_all();
    }

  if (depth <= 0)
    {
      /* [V710] v709 behaviour, kept verbatim as the A/B baseline.  The
       * counters are still maintained so the two paths can be compared. */

      while (done < len)
        {
          size_t   n = len - done;
          uint64_t t0;
          int      ret;

          if (n > AP_WRITE_CHUNK_MAX)
            {
              n = AP_WRITE_CHUNK_MAX;
            }

          t0 = ap_now_us();

          if (g_ap_last_us != 0ull)
            {
              ap_turn_note(t0 - g_ap_last_us);
            }

          ret = ap_write_once(pb, p + done, n);
          g_ap_bytes += (uint64_t)n;
          g_ap_chunks++;

          if (ret < 0)
            {
              g_ap_last_us = ap_now_us();
              g_ap_busy_us += g_ap_last_us - t0;
              return done > 0 ? (int)done : ret;
            }

          g_ap_last_us = ap_now_us();
          g_ap_busy_us += g_ap_last_us - t0;
          done += n;
        }

      return (int)done;
    }

  while (done < len)
    {
      struct ap_flight *f;
      size_t            n = len - done;
      int               ret;

      if (n > AP_WRITE_CHUNK_MAX)
        {
          n = AP_WRITE_CHUNK_MAX;
        }

      if (pb->stopped)
        {
          return done > 0 ? (int)done : -ECANCELED;
        }

      /* Retire only when the window is full or no slot is free.  That is the
       * whole fix: at the moment the oldest transfer retires, the buffers
       * submitted after it are already sitting in the driver's tx.pend, so
       * the EOF ISR has something to promote and the FIFO never runs dry. */

      f = ap_pipe_free();

      if (f == NULL || g_flight_qn >= depth)
        {
          f = &g_flight[g_flight_q[0]];

          ret = ap_retire(f);
          ap_pipe_drop(f);

          if (ret < 0)
            {
              return done > 0 ? (int)done : ret;
            }

          f = ap_pipe_free();
        }

      if (f == NULL)
        {
          return done > 0 ? (int)done : -ENOMEM;
        }

      ret = ap_arm(f, p + done, n);
      if (ret < 0)
        {
          return done > 0 ? (int)done : ret;
        }

      done += n;
    }

  return (int)done;
}

/* [V244] Read back every register the official sequence touches, so the codec
 * state can be verified against what was written instead of assumed. */
static const uint8_t g_es_dump[] =
{
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
  0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
  0x1B, 0x1C, 0x32, 0x37, 0x44, 0x45
};

void audio_direct_dump_codec(void)
{
  char line[160];
  int n = 0;
  unsigned int k;

  line[0] = '\0';
  for (k = 0; k < sizeof(g_es_dump) / sizeof(g_es_dump[0]); k++)
    {
      n += snprintf(line + n, sizeof(line) - n, "%02x=%02x ",
                    g_es_dump[k], es_readreg(g_es_dump[k]));
      if (n > 110)
        {
          syslog(LOG_INFO, "[%s] codec regs: %s\n", TAG, line);
          n = 0;
          line[0] = '\0';
        }
    }

  if (n > 0)
    {
      syslog(LOG_INFO, "[%s] codec regs: %s\n", TAG, line);
    }
}

/* [V234] Expose the ES8311 DAC volume register so the boot smoke test can
 * sweep it and settle the direction of the scale empirically.  On several
 * codec families this register is an attenuation (0x00 = 0 dB, 0xFF = quietest)
 * rather than a gain, which would make our 0xFF write the *quietest* setting. */
void audio_direct_set_dac_volume(unsigned int v)
{
  es_writereg(ES_REG_DAC_VOL, (uint8_t)v);
  syslog(LOG_INFO, "[%s] ES8311 reg 0x32 (DAC vol) = 0x%02x\n", TAG,
         (unsigned)(uint8_t)v);
}

void audio_playback_stop(audio_playback_t *pb)
{
  if (!pb)
    {
      return;
    }

  /* [V710] Only latch the flag.  Draining here would block the PTT/abort
   * caller for up to one transfer per in-flight slot; the speak thread owns
   * the queue and reaps it on its next write, and audio_playback_close()
   * drains unconditionally. */

  pb->stopped = true;
}

/* [V846] Is the speaker audible right now?
 *
 * Two terms: an open playback session, plus a 400 ms tail after it
 * closes.  The tail matters -- ap_drain_all() has retired the DMA
 * transfer but the sound has not left the room, and the log shows the
 * VAD re-opening on the decay of the device's own reply. */
int audio_playback_echo_active(void)
{
  if (g_ap_in_use > 0)
    {
      return 1;
    }

  if (g_ap_stop_us != 0ull &&
      (ap_now_us() - g_ap_stop_us) < 400000ull)
    {
      return 1;
    }

  return 0;
}


void audio_playback_close(audio_playback_t *pb)
{
  /* [V671] One line per playback session: how many transfers took the light
   * re-arm path and how many had to reconfigure the transmitter. */

  /* [V710] Reap whatever is still in flight and report the turnaround
   * statistics for this session.  count / busy come straight from the audio
   * layer; turn_avg and turn_max are the gaps between a transfer retiring
   * and the next one being submitted, i.e. the audible holes. */

  ap_drain_all();

  /* [V858] *** Hand the port back to the microphone. ***
   *
   * This function used to stop at ap_drain_all() and never touch the
   * I2S port, so everything i2s_txdma_start() had latched for the
   * speaker -- MONO / CHAN_EQUAL / LEFT_ALIGN cleared, the playback
   * TDM frame, and, after an "I2S TX completion TIMEOUT", a
   * quarantined container whose DMA descriptor is still linked -- was
   * what the next capture inherited.  That capture then produced all
   * zeros or a frozen DC for the rest of the boot.
   *
   * Release whatever the playback abandoned, then re-lock.  Skipped
   * internally while a capture is streaming; a playback and a capture
   * never run at the same time on this board anyway. */

  esp32s3_i2s_v858_after_playback();

  /* [V846] The DMA queue is drained, so the speaker stops here. */
  g_ap_in_use--;

  if (g_ap_in_use < 0)
    {
      g_ap_in_use = 0;
    }

  g_ap_stop_us = ap_now_us();

  {
    uint32_t w     = g_ap_writes;
    uint32_t c     = g_ap_chunks;
    uint64_t bytes = g_ap_bytes;
    uint64_t busy  = g_ap_busy_us;
    uint64_t turn  = g_ap_turn_us;
    uint32_t tn    = g_ap_turn_n;
    uint32_t tmax  = g_ap_turn_max;
    uint32_t slow  = g_ap_slow;

    g_ap_writes  = 0u;
    g_ap_chunks  = 0u;
    g_ap_bytes   = 0ull;
    g_ap_busy_us = 0ull;
    g_ap_turn_us = 0ull;
    g_ap_turn_n  = 0u;
    g_ap_turn_max = 0u;
    g_ap_slow    = 0u;
    g_ap_last_us = 0ull;

    syslog(LOG_INFO,
           "[%s] [V710] pb: writes=%lu chunks=%lu pcm=%lu busy=%lums "
           "turn_avg=%lums turn_max=%lums slow=%lu pipe=%d\n",
           TAG, (unsigned long)w, (unsigned long)c,
           (unsigned long)(bytes / 1024ull),
           (unsigned long)(busy / 1000ull),
           (unsigned long)(tn ? (turn / (uint64_t)tn) / 1000ull : 0ull),
           (unsigned long)(tmax / 1000u),
           (unsigned long)slow, g_pipe_depth);
  }

  {
    static uint32_t s_full;
    static uint32_t s_light;
    uint32_t full  = 0u;
    uint32_t light = 0u;

    esp32s3_i2s_v671_stats(&full, &light);

    syslog(LOG_ERR,
           "[%s] [V671] tx re-arm this session: full=%lu light=%lu "
           "(cumulative full=%lu light=%lu)\n",
           TAG,
           (unsigned long)(full - s_full), (unsigned long)(light - s_light),
           (unsigned long)full, (unsigned long)light);

    s_full  = full;
    s_light = light;
  }

  if (pb)
    {
      free(pb);
    }
}
