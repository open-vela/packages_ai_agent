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
#include "voice/demo_voice_cache.h"
#ifdef CONFIG_AI_AGENT_AUDIO_PREPROCESS
#include <compexp.h>
#include <echo_canceller.h>
#include <heap_api.h>
#endif
#include "core/message_bus.h"
#ifdef CONFIG_AI_AGENT_LVGL_UI
#include "ui/lvgl_ui_channel.h"
#endif
#include "infra/config_store.h"
#include "agent_compat.h"
#include "agent_config.h"
#include "voice/audio_capture.h"
#include "voice/audio_playback.h"
#include "voice/voice_asr.h"
#include "voice/edge_tts.h"
#include "voice/minimax_tts.h"
#include "voice/voice_tts.h"
#include "voice/voice_tts_cache.h"   /* VOICE-C：模板 TTS 离线缓存（speak_internal 先查） */
#include "voice/volc_asr.h"
#include "voice/minimax_asr.h"
#include "voice/mimo_tts.h"
#include "voice/mimo_asr.h"
#include "voice/volc_tts.h"

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

static const char* TAG = "voice";
static int s_backends_registered;

/* VOICE-B：voice_channel_start() 在文件前半段就要判断"还有未播完的流式
 * TTS/队列"，因此先声明实现（实体在 speak_enqueue 之后）。 */
static bool voice_channel_tts_pending(void);
static bool voice_llm_active(void);   /* P2A-COLLISION：LLM 回合是否仍在进行 */

/* VOICE-B-FIX：防止 stream_abort <-> tts_abort 互相递归（tts_abort 内部也会
 * 调 stream_abort；stream_abort 只在"确实有残句"时回调 tts_abort）。 */
static volatile int s_tts_abort_in_progress;

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
    int auto_pending; /* 采集线程因自动断句退出, 等监督线程收尾 */
} s_voice = {
    .state = VOICE_IDLE,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .speak_lock = PTHREAD_MUTEX_INITIALIZER,
};

/* ── 自动断句的收尾监督线程 ────────────────────────────────────────────
 *
 * 为什么需要单独一个线程: 采集线程发现"一句话说完了"之后**不能自己收尾** ——
 * 收尾的 voice_channel_stop() 里有 pthread_join(rec_thread, ...), 也就是
 * 线程 join 自己, 会直接死锁。所以让采集线程叫醒这个监督线程去调 stop。
 * 它平时就睡在信号量上, 不占 CPU。 */
static sem_t s_auto_stop_sem;
static int   s_auto_stop_started;

static void* auto_stop_thread(void* arg)
{
    (void)arg;

    for (;;) {
        int pending;

        if (sem_wait(&s_auto_stop_sem) != 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        pthread_mutex_lock(&s_voice.lock);
        pending = s_voice.auto_pending;
        s_voice.auto_pending = 0;
        pthread_mutex_unlock(&s_voice.lock);

        /* 不是自动断句触发的(或状态已经变了)就忽略 */
        if (!pending) {
            continue;
        }

        syslog(LOG_INFO, "[%s] auto-stop: speech ended, finishing\n", TAG);
        printf("Voice: speech ended, finishing...\n");
        voice_channel_stop();
    }

    return NULL;
}

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

static void* recording_thread(void* arg)
{
    (void)arg;
    unsigned char chunk[AGENT_ASR_CHUNK_SIZE];
    int need_fallback = 0;
    int auto_stopped = 0;      /* 因自动断句而结束录制 */
    /* 注: 排查音频质量时曾在这里临时加过"把采集到的原始 PCM 落盘"的诊断,
     * 配合 pcm-audio 技能的 audio_analyzer.py 用。两条经验留在这里备查:
     *   1) **落 tmpfs(/tmp), 绝不落 /data** —— /data 在 NOR flash 上, 而代码
     *      是 XIP 从同一片 NOR 执行的; 边录音边以 32KB/s 写 flash 会长时间
     *      占住 flash 控制器, 真机上录制十几秒后**无 fault dump 静默复位**。
     *   2) 要留**最后 N 秒**(环形缓冲), 不是开头 N 秒 —— 说话人通常在按
     *      "停录"之前才开口, 留开头往往只抓到环境声。
     * 诊断完即删: 正常固件不该每次录音都写文件。 */
    size_t total_bytes_read = 0;
    size_t total_bytes_sent = 0;
    int chunk_count = 0;
    int abnormal_exit = 0;

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
        syslog(LOG_WARNING,
            "[%s] streaming ASR unavailable, batch fallback\n", TAG);
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

        if (n == -EAGAIN || n == -EWOULDBLOCK) {
            /* Non-blocking capture socket has no data yet (common right
             * after start before the first frames arrive). Retry instead
             * of treating it as a fatal error. */
            usleep(10000);
            continue;
        }

        if (n <= 0) {
            syslog(LOG_WARNING,
                "[%s] capture read returned %d, stopping\n", TAG, n);
            abnormal_exit = 1;
            break;
        }

        total_bytes_read += (size_t)n;
        chunk_count++;

        /* Log audio level for first few chunks and periodically
         * to diagnose silence vs actual speech from QEMU mic. */
        if (chunk_count <= 3 || chunk_count % 50 == 0) {
            int16_t* samples = (int16_t*)chunk;
            int sample_count = n / 2;
            int32_t peak = 0;

            for (int i = 0; i < sample_count; i++) {
                int32_t v = samples[i] < 0 ? -samples[i] : samples[i];
                if (v > peak) {
                    peak = v;
                }
            }
            syslog(LOG_INFO,
                "[%s] chunk#%d: %d bytes, peak=%ld\n",
                TAG, chunk_count, n, (long)peak);
        }

#ifdef CONFIG_AI_AGENT_AUDIO_PREPROCESS
        preproc_chunk(pp, chunk, n);
#endif

        process_audio_chunk(&stream, chunk, (size_t)n,
            &need_fallback, &total_bytes_sent);

        /* 自动断句: VAD 判定"这一句说完了"就主动结束录制。
         * 收尾不在这里做 —— 见 auto_stop_thread 上面的说明(join 自己会死锁)。 */
        if (stream != NULL &&
            voice_asr_stream_utterance_done(stream)) {
            syslog(LOG_INFO,
                "[%s] auto-stop: utterance done after %d chunks\n",
                TAG, chunk_count);
            auto_stopped = 1;
            break;
        }
    }

#ifdef CONFIG_AI_AGENT_AUDIO_PREPROCESS
    preproc_destroy(pp);
#endif

    /* Store stream handle for voice_channel_stop to finish */
    /* ★ 在本线程(采集线程, 栈已抬到 64KB)把最后不足一帧的残余编掉再交出去。
     *   绝不能留给 voice_channel_stop 里的 voice_asr_stream_finish() —— 那个
     *   函数跑在**调用者的线程**上, 真机上是 16KB 栈的 vela CLI 线程, 在里面
     *   调 opus_encode 会直接栈溢出硬 fault(backtrace 落在 silk_Encode,
     *   2026-10-14 真机踩到, 板子随即被看门狗复位)。 */
    if (stream != NULL) {
        int fret = voice_asr_stream_end_input(stream);

        if (fret != 0) {
            syslog(LOG_WARNING,
                "[%s] stream end_input failed: %d\n", TAG, fret);
        }
    }

    pthread_mutex_lock(&s_voice.lock);
    s_voice.asr_stream = stream;

    if (abnormal_exit && s_voice.state == VOICE_RECORDING) {
        syslog(LOG_WARNING, "[%s] abnormal exit, cleaning up\n", TAG);
        recording_cleanup_locked(stream);
    }
    pthread_mutex_unlock(&s_voice.lock);

    syslog(LOG_INFO,
        "[%s] recording thread exit: %d chunks, %zu read, %zu sent%s%s\n",
        TAG, chunk_count, total_bytes_read, total_bytes_sent,
        abnormal_exit ? " (abnormal)" : "",
        auto_stopped ? " (auto-stop)" : "");

    /* 自动断句: 叫醒监督线程去走 voice_channel_stop()。放在最后,
     * 保证 stream 已经存进 s_voice.asr_stream。 */
    if (auto_stopped) {
        pthread_mutex_lock(&s_voice.lock);
        s_voice.auto_pending = 1;
        pthread_mutex_unlock(&s_voice.lock);
        sem_post(&s_auto_stop_sem);
    }

    return NULL;
}

/* ★ v111：单回合门禁 —— 上一轮语音提问已提交、回答/播报尚未结束时，
 * 拒绝开始新的录音。真机证据（00:48:32–00:49:00）：第二个 ASR 的 32KB WAV
 * 上传与上一轮 LLM 处理并发走 BT PAN → zblue 缓冲耗尽 → 整机冻死重启。 */
static volatile int s_turn_active;
static volatile time_t s_turn_ts;
static volatile int s_tts_closing;   /* v113：播放器 close/drain 收尾窗口 */
static volatile int s_tts_sessions;  /* v113b：本 boot 已发起的 TTS 会话数 */
#define VOICE_TURN_MAX_S  90
/* v113b：真机实测第 3 次 TTS 复用 TLS 连接时会在读取阶段整机停摆；
 * 演示期先限制每 boot 最多 2 次播报，第 3 次起只上屏不出声（日志可查）。 */
/* v142c（2026-11-04）：v142 TLS 截断检测/keep=false/回退链预算后，5 次/ boot
 * 对正常使用太低（用户第 6 次起只上屏不出声）。升到 20 并保留可回滚；
 * 若复现旧 TLS 复用停摆，再降回 5。 */
#define TTS_MAX_SESSIONS_PER_BOOT  20

int voice_channel_turn_active(void)
{
    if (!s_turn_active) {
        return 0;
    }
    if (time(NULL) - s_turn_ts > VOICE_TURN_MAX_S) {
        s_turn_active = 0;      /* 异常卡住时兜底超时，避免永久拒绝 */
        return 0;
    }
    return 1;
}

/* 2026-11-02 v112：语音通道的播报走 agent_main 的同步 voice_channel_speak，
 * 不经过 speak_worker；v111 只在 speak_worker 里清 s_turn_active，导致同步路径
 * 回答播完后仍永久显示"等我说完再问～"。这里提供显式清除。 */
void voice_channel_turn_clear(void)
{
    s_turn_active = 0;
}

/* ── ASR worker thread ──────────────────────────────────── */

/* 24KB -> 64KB (2026-10-14): 识别前要先做 Opus 编码, 而 libopus 的
 * opus_encode() 栈开销很大 —— 真机上用 16KB 栈跑它直接栈溢出硬 fault
 * (backtrace 落在 celt_encode_with_ec)。本板 PSRAM 有 7MB 空闲, 任务栈从
 * 堆里分配, 64KB 毫无压力。 */
#define ASR_THREAD_STACK (64 * 1024)

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

        /* 用户说的话也要进对话历史 —— 其他渠道(ask/ws/按键)都写了,
         * 唯独语音这条三个成功点全漏了, 于是"我说的话"在历史里看不到。
         * is_user=true: ① 这确实是用户说的; ② 只有 true 才不会触发
         * history_log_async_cb 里"回答"才该有的副作用(跳桌宠页/覆盖回答)。 */
#ifdef CONFIG_AI_AGENT_LVGL_UI
        lvgl_ui_channel_log(text, true);
#endif

        agent_msg_t msg;

        memset(&msg, 0, sizeof(msg));
        strncpy(msg.channel, AGENT_CHAN_VOICE,
            sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, "voice",
            sizeof(msg.chat_id) - 1);
        msg.content = strdup(text);
        if (msg.content) {
            s_turn_active = 1;
            s_turn_ts = time(NULL);
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

/* 这几个量是跨线程共享的静态量（P2b 语义未变，v125 P0-B 补了统计与时序）：
 *   s_tts_start / s_tts_first_chunk —— 只在 voice_channel_speak() 内部与它
 *     自己的回调 tts_stream_cb() 里读写，而 voice_channel_speak() 全程持有
 *     s_voice.speak_lock，且 P2b 之后异步路径只有一个 worker 线程，因此写入
 *     者唯一、读写都在锁内，没有新增竞态。
 *   s_asr_done_ts —— 由录音线程(ASR 完成处)写，由 speak 起头**读一次并清零**，
 *     只用来打"本轮 ASR 完成 → TTS 开始"的单轮差值（v125 P0-B 起）。旧代码
 *     读后不清零，真机打出过 273261/342047/397047 ms 的累计值（perf-net
 *     §2.5）；跨线程 8 字节 timespec 的非原子读是改动前就有的良性竞态，
 *     最坏只是日志里一个偏掉的毫秒数。
 *   s_tts_pcm_written / s_tts_write_errors —— 同样只在 speak 线程内读写，
 *     把"后端说成功、播放器 0 字节"从间接日志变成直接证据。 */
static struct timespec s_tts_start;
static int s_tts_first_chunk;
/* P0-NET：最近一次 TTS 首块延迟与序号（供 agent_loop [lat] 聚合；只读）。 */
static long s_last_tts_first_ms = -1;
static unsigned s_last_tts_first_seq;
static struct timespec s_asr_done_ts;
/* v125 P0-B（2026-11-03）：本句实际写进播放器的 PCM 字节数与写失败次数。
 * 旧 tts_stream_cb 忽略 audio_playback_write() 的返回值，真机上出现过
 * "PCM 已生成、播放器却 0 字节播出"只能靠 [audio_pb] 的间接日志判断
 * （perf-net §3.5 / log:1949）。现在 speak 结束时把这两个数直接打出来。 */
static size_t s_tts_pcm_written;
static int s_tts_write_errors;

/* P0-NET：只读 getter（见 voice_channel.h 注释）。 */
void voice_channel_get_last_tts_first_ms(long* ms, unsigned* seq)
{
    if (ms != NULL)
        *ms = s_last_tts_first_ms;
    if (seq != NULL)
        *seq = s_last_tts_first_seq;
}

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
            s_last_tts_first_ms = ms;   /* P0-NET：[lat] tts_first_ms 数据源 */
            s_last_tts_first_seq++;
        }
        int wrote = audio_playback_write(pb, pcm_data, pcm_len);

        if (wrote < 0) {
            /* 不再吞掉返回值：写失败必须能从日志一路追到 speak 结束。 */
            if (s_tts_write_errors == 0) {
                syslog(LOG_WARNING, "[%s] audio_playback_write failed: %d\n",
                    TAG, wrote);
            }
            s_tts_write_errors++;
        } else {
            s_tts_pcm_written += (size_t)wrote;
        }
    }

    (void)is_last;
}

/* ── Public API ──────────────────────────────────────────── */

static int speak_worker_ensure_started(void);
static int voice_channel_speak_internal(const char* text, bool count_budget);

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
    s_voice.auto_pending = 0;

    /* 自动断句的收尾监督线程 (起一次即可) */
    if (!s_auto_stop_started) {
        s_auto_stop_started = 1;
        sem_init(&s_auto_stop_sem, 0, 0);

        pthread_attr_t ast;
        pthread_attr_init(&ast);
        pthread_attr_setstacksize(&ast, 16 * 1024);

        pthread_t th;
        if (pthread_create(&th, &ast, auto_stop_thread, NULL) == 0) {
            pthread_detach(th);
            syslog(LOG_INFO,
                "[%s] auto-stop supervisor started\n", TAG);
        } else {
            syslog(LOG_ERR, "[%s] auto-stop supervisor failed\n", TAG);
        }

        pthread_attr_destroy(&ast);
    }
    sem_init(&s_voice.rec_ready, 0, 0);

    if (!s_backends_registered) {
        /* 2026-10-26: Xiaomi MiMo Token Plan —— LLM / TTS / ASR 全走同一把
         * key(token-plan-cn.xiaomimimo.com, OpenAI 兼容)。MiMo TTS 先注册,
         * voice_tts_register() 保留第一个 => 它是默认后端; MiniMax/Edge/Volc
         * 留在链上作兜底。ASR 同理先注册 MiMo。 */
        mimo_tts_register();
        volc_tts_register();
        /* Edge and MiniMax are registered after volc so neither becomes the
         * default backend (voice_tts_register() keeps the first one): Edge is
         * free but currently answers 403, MiniMax is the paid provider whose
         * key does work here.  The order they are tried in is decided by
         * voice_tts_speak_stream(), not by this list. */
        edge_tts_register();
        minimax_tts_register();
        /* 2026-10-14: ASR 改用 MiniMax (同一个账号 / 同一个 key / 同一条
         * 真机验证过的 HTTPS 通道), 见 voice/minimax_asr.c。
         * 火山后端仍留在源码里, 需要时把下面这行换回 volc_asr_register()
         * 即可 —— 但注意火山还额外需要 app_id/token/cluster 三个凭证,
         * 而本机 config 里从来没配过。 */
        mimo_asr_register();      /* 先注册者优先 => MiMo 是默认 ASR */
        minimax_asr_register();
        s_backends_registered = 1;
    }

    /* v125 现场定位：speak_worker 如果在短生命周期的 NSH app 上下文里创建，
     * app main 返回后线程会被任务组清理掉，"speak worker started" 打了但
     * 永远不会执行。这里在 voice_channel_init（ai_agent 主进程上下文）
     * 预创建，worker 归属 ai_agent，任何调用方都只入队。 */
    if (speak_worker_ensure_started() != 0) {
        syslog(LOG_WARNING, "[%s] speak worker pre-start failed\n", TAG);
    }

    /* DEMO-VOICE-A：缓存 worker 也必须在 ai_agent 主进程里预创建，否则
     * 从 NSH 短生命周期 app 懒创建时会话组清理后不再执行。 */
    if (demo_voice_init() != 0) {
        syslog(LOG_WARNING, "[%s] demo voice worker pre-start failed\n", TAG);
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

    if (voice_channel_turn_active() || s_tts_closing) {
        pthread_mutex_unlock(&s_voice.lock);
        syslog(LOG_WARNING,
            "[%s] previous turn still active/closing, reject new recording\n", TAG);
        return -EBUSY;
    }

    /* Drain stale sem posts from previous sessions to prevent
     * sem_wait returning immediately before thread is ready. */
    while (sem_trywait(&s_voice.rec_ready) == 0)
        ;

    /* ── AEC workaround: stop active TTS to prevent echo ──
     * VOICE-B：除了正在播的 pb，还要覆盖"流式首句已入队但还没播完"的情况
     * （s_voice.tts_pb 可能为 NULL，queue/stream 仍有句子）。 */
    if (s_voice.tts_pb || voice_channel_tts_pending()
        || voice_tts_cache_is_playing()) {
        syslog(LOG_INFO,
            "[%s] PTT: stopping active/queued TTS to prevent echo\n", TAG);
        s_voice.tts_abort = 1;
        if (s_voice.tts_pb) {
            audio_playback_stop(s_voice.tts_pb);
        }

        /* Release lock so the speak thread can finish closing the
         * playback device.  Without this, we hold the lock through
         * the entire ASR pre-connect (300-800ms), blocking the speak
         * thread from calling audio_playback_close().  The playback
         * device stays open while we open the capture device, and on
         * BES hardware the shared codec (ci2s) gets confused, causing
         * incomplete or corrupted recording. */
        pthread_mutex_unlock(&s_voice.lock);

        /* VOICE-B：不持 s_voice.lock 调 tts_abort（它内部会再取 s_voice.lock），
         * 清空句子队列 + 流式状态，避免录音时的回声/旧句残留。
         * VOICE-C：缓存播放也在这里就近打断（tts_abort 内部亦会 stop）。 */
        voice_tts_cache_stop();
        voice_channel_tts_abort();

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
    voice_asr_stream_t* pre_stream = voice_asr_stream_open();
    if (!pre_stream) {
        syslog(LOG_WARNING,
            "[%s] pre-open ASR failed, will retry in thread\n", TAG);
    } else {
        syslog(LOG_INFO, "[%s] ASR pre-connected\n", TAG);
        s_voice.asr_stream = pre_stream;
    }

    /* Open capture device (but do NOT start yet — start after the
     * recording thread is spawned so the consumer is ready before
     * audio frames begin flowing, preventing media_recorder queue
     * overflow "data queue is more than max count(4)"). */
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
        audio_capture_abort(s_voice.cap);
        pthread_join(s_voice.rec_thread, NULL);
        audio_capture_close(s_voice.cap);
        s_voice.cap = NULL;
        return -EIO;
    }

    syslog(LOG_INFO, "[%s] PTT recording started\n", TAG);
    printf("Voice: recording... (use voice_stop to finish)\n");
    return 0;
}

int voice_channel_is_recording(void)
{
    int rec;

    pthread_mutex_lock(&s_voice.lock);
    rec = (s_voice.state == VOICE_RECORDING);
    pthread_mutex_unlock(&s_voice.lock);

    return rec;
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

    audio_capture_abort(s_voice.cap);

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

            /* 用户话术入历史(真机走的就是这条流式路径) */
#ifdef CONFIG_AI_AGENT_LVGL_UI
            lvgl_ui_channel_log(text, true);
#endif

            agent_msg_t msg;

            memset(&msg, 0, sizeof(msg));
            strncpy(msg.channel, AGENT_CHAN_VOICE,
                sizeof(msg.channel) - 1);
            strncpy(msg.chat_id, "voice",
                sizeof(msg.chat_id) - 1);
            msg.content = strdup(text);
            if (msg.content) {
                s_turn_active = 1;
                s_turn_ts = time(NULL);
                message_bus_push_inbound(&msg);
            }
        } else {
            /* 2026-11-01 v104：区分"没听清"和"网络失败"。
             * 旧文案无论 ret!=0 还是"识别成功但文本为空"都提示
             * "网络不太稳"，把静音/没说话误报成网络问题（真机 15:54:01
             * 就是 ret==0、文本为空，却被当成网络不稳定）。 */
            syslog(LOG_WARNING,
                "[%s] stream ASR failed: ret=%d text_len=%zu\n",
                TAG, ret, strlen(text));
#ifdef CONFIG_AI_AGENT_LVGL_UI
            if (ret == 0 || ret == -ENODATA)
              {
                /* 没检测到有效语音（静音 / 声音太小 / 没听清） */
                lvgl_ui_channel_send_kind(
                    "没听清，请再说一次", AGENT_MSG_KIND_ERROR);
              }
            else
              {
                /* 真正的 ASR/网络错误才提示网络 */
                lvgl_ui_channel_send_kind(
                    "语音识别失败，请检查网络", AGENT_MSG_KIND_ERROR);
              }
#endif
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

    audio_capture_abort(s_voice.cap);

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

/* ── 异步播报: 一个常驻 worker + 有界队列 (P2b) ──────────────
 *
 * 旧实现每收到一句就 pthread_create + detach：没有队列、没有深度上限，
 * 也没有显式栈属性 —— 而 Edge 后端要在这条线程里跑 mbedtls TLS 握手 +
 * minimp3 解码，默认栈大小是听天由命。现在：
 *   - worker 只在第一次需要时创建，之后复用；
 *   - 队列深 2，"满则丢最旧、留最新"，与 voice_channel_speak() 里
 *     "新答案打断旧答案"的语义一致(手机上连问两句不会积压一串旧答案，
 *     也不会出现两个 speak 同时抢播放设备)；
 *   - worker 显式 setstacksize(见 SPEAK_WORKER_STACK)。
 *
 * worker 常驻到进程结束(不 join、不退出)：退出路径上没有它要保存的状态，
 * 停机时随进程一起消失即可，避免引入 teardown 顺序问题。 */
#define SPEAK_QUEUE_DEPTH 8
/* P2A-COLLISION（task-57）：队列条目来源。用户回答（USER）优先于主动关怀
 * 本地语音（CARE）；care 队列另有独立上限且满时绝不淘汰用户回答。 */
#define SPEAK_ORIGIN_USER 0
#define SPEAK_ORIGIN_CARE 1
#define SPEAK_CARE_MAX    2
/* 16KB(NuttX 默认): Edge 路径(TLS + minimp3)本来就在默认栈上出过声; 24KB 只从
 * 只有 ~70KB 的 SRAM 堆里永久多拿 8KB, 并不是挂死的原因(审查结论)。 */
#define SPEAK_WORKER_STACK (32 * 1024)

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    char* slots[SPEAK_QUEUE_DEPTH]; /* slots[0] 最旧, slots[count-1] 最新 */
    bool  budget[SPEAK_QUEUE_DEPTH]; /* 该句是否占用每 boot TTS 会话额度 */
    uint8_t origin[SPEAK_QUEUE_DEPTH]; /* SPEAK_ORIGIN_USER / SPEAK_ORIGIN_CARE */
    int count;
    pthread_t thread;
    int started;
} s_speak_q = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

/* ══ P2A-COLLISION（task-57）：队列来源/诊断 + care cache-only 播放 ══════ */

static uint32_t speak_hash(const char* text)
{
    uint32_t h = 2166136261u;

    while (text != NULL && *text != '\0')
    {
        h ^= (uint8_t)*text++;
        h *= 16777619u;
    }
    return h;
}

/* 调用方持 s_speak_q.lock：移除下标 idx（数组整体前移）。 */
static void speak_queue_remove_locked(int idx)
{
    int i;
    int last = s_speak_q.count - 1;

    if (idx < 0 || idx > last)
    {
        return;
    }
    free(s_speak_q.slots[idx]);
    for (i = idx; i < last; i++)
    {
        s_speak_q.slots[i] = s_speak_q.slots[i + 1];
        s_speak_q.budget[i] = s_speak_q.budget[i + 1];
        s_speak_q.origin[i] = s_speak_q.origin[i + 1];
    }
    s_speak_q.slots[last] = NULL;
    s_speak_q.budget[last] = false;
    s_speak_q.origin[last] = SPEAK_ORIGIN_USER;
    s_speak_q.count = last;
}

static int speak_queue_origin_count_locked(uint8_t origin)
{
    int i;
    int n = 0;

    for (i = 0; i < s_speak_q.count; i++)
    {
        if (s_speak_q.origin[i] == origin)
        {
            n++;
        }
    }
    return n;
}

/* 调用方持 s_speak_q.lock：返回最旧的指定来源条目下标，找不到 -1。 */
static int speak_queue_find_locked(uint8_t origin)
{
    int i;

    for (i = 0; i < s_speak_q.count; i++)
    {
        if (s_speak_q.origin[i] == origin)
        {
            return i;
        }
    }
    return -1;
}

/* P2A-COLLISION F4：care 队列条目的唯一播放路径。
 * cache-only：只查模板缓存并本地 PCM 播放；**不** tts_begin（不占媒体回合、
 * 不会被 LLM 阻塞）、不占每 boot TTS 额度；未命中/-EBUSY/其它错误直接丢弃，
 * **绝不回退网络 TTS**。录音态丢弃（PTT 优先）。故意不动 s_turn_active
 * （care 不管理用户回合）。 */
static void voice_care_cache_only(const char* text)
{
    const void* pcm = NULL;
    size_t bytes = 0;
    bool recording;
    int rc;

    if (text == NULL || text[0] == '\0')
    {
        return;
    }

    pthread_mutex_lock(&s_voice.lock);
    recording = (s_voice.state != VOICE_IDLE);
    pthread_mutex_unlock(&s_voice.lock);
    if (recording)
    {
        syslog(LOG_INFO,
            "[%s] care-voice: drop (recording, cache_only=1)\n", TAG);
        return;
    }

    if (voice_tts_cache_lookup(text, &pcm, &bytes) != 0)
    {
        syslog(LOG_INFO,
            "[%s] care-voice: drop (cache miss, cache_only=1, no network TTS)\n",
            TAG);
        return;
    }

    /* P2A-感知增强（task-62）：care 专用播放 —— 同一 audio_playback 会话内
     * 先播短提示音 + 关怀音量，再播缓存正文（不二次 open）。 */
    rc = voice_tts_cache_play_care(text);
    if (rc == 0)
    {
        syslog(LOG_INFO,
            "[%s] care-voice: played (cache_only=1, prompt_tone=1, "
            "pcm=%zu, no tts session)\n",
            TAG, bytes);
    }
    else if (rc == -EBUSY)
    {
        syslog(LOG_INFO,
            "[%s] care-voice: drop (cache busy, cache_only=1)\n", TAG);
    }
    else if (rc == -ECANCELED)
    {
        syslog(LOG_INFO, "[%s] care-voice: canceled (PTT stop)\n", TAG);
    }
    else
    {
        syslog(LOG_WARNING,
            "[%s] care-voice: drop rc=%d (cache_only=1, no network fallback)\n",
            TAG, rc);
    }
}

/* 后台线程里念一句话。TTS 要走网络+解码+播放, 正常也要几秒到几十秒, 绝不能
 * 占住 UI 线程或 agent 的派发线程 —— 之前的"屏幕卡死"就是同步调用造成的。 */
static void* speak_worker(void* arg)
{
    (void)arg;

    for (;;) {
        char* text;
        uint8_t origin;
        int ret;
        int i;

        pthread_mutex_lock(&s_speak_q.lock);
        while (s_speak_q.count == 0) {
            pthread_cond_wait(&s_speak_q.cond, &s_speak_q.lock);
        }
        /* FIFO 出队(队列本身只有 1~2 项, 直接搬数组) */
        text = s_speak_q.slots[0];
        bool count_budget = s_speak_q.budget[0];
        origin = s_speak_q.origin[0];
        for (i = 1; i < s_speak_q.count; i++) {
            s_speak_q.slots[i - 1] = s_speak_q.slots[i];
            s_speak_q.budget[i - 1] = s_speak_q.budget[i];
            s_speak_q.origin[i - 1] = s_speak_q.origin[i];
        }
        s_speak_q.slots[s_speak_q.count - 1] = NULL;
        s_speak_q.budget[s_speak_q.count - 1] = false;
        s_speak_q.origin[s_speak_q.count - 1] = SPEAK_ORIGIN_USER;
        s_speak_q.count--;
        pthread_mutex_unlock(&s_speak_q.lock);

        /* P2A-COLLISION F4：care 走独立 cache-only 路径 —— 不 tts_begin
         * （因此不会被 LLM 回合阻塞）、不占每 boot TTS 额度、未命中/-EBUSY
         * 直接丢弃且绝不回退网络 TTS；也不清 s_turn_active。 */
        if (origin == SPEAK_ORIGIN_CARE) {
            syslog(LOG_INFO,
                "[%s] queue dequeue: origin=care budget=0 text_hash=%08x\n",
                TAG, (unsigned)speak_hash(text));
            voice_care_cache_only(text);
            free(text);
            continue;
        }

        /* 用户回答：正常网络 TTS 路径（F2：voice_channel_tts_begin 已有 30s
         * 等待上限，不会永久卡在 LLM 互斥上）。 */
        syslog(LOG_INFO,
            "[%s] queue dequeue: origin=user budget=%d text_hash=%08x\n",
            TAG, (int)count_budget, (unsigned)speak_hash(text));
        voice_channel_tts_begin();
        ret = voice_channel_speak_internal(text, count_budget);
        voice_channel_tts_end();

        /* B7 / P2A-COLLISION：多句排队、流式仍在进行、或 LLM 回合还未结束
         * 都不能提前清 turn_active（否则 PTT 会在 LLM/TTS 窗口里开始录音，
         * 与用户回答抢音频）。只有"队列空 + 无流式 + LLM 已结束"才结束本轮。 */
        if (!voice_channel_tts_pending() && !voice_llm_active())
        {
            s_turn_active = 0;  /* v111：本轮回答播报结束，允许下一句 */
        }
        if (ret != 0) {
            syslog(LOG_WARNING, "[%s] async speak failed: %d\n", TAG, ret);
        }
        syslog(LOG_INFO,
            "[%s] user-speak done: origin=user budget=%d turn_active=%d\n",
            TAG, (int)count_budget, (int)s_turn_active);

        free(text);
    }

    return NULL;
}

/* 不再要求持锁调用: pthread_create 是重活(申请栈 + TCB), 之前它跑在
 * s_speak_q.lock 里 = 锁内做重活, 会让已经在排队的说话请求一起卡住。 */
static int speak_worker_ensure_started(void)
{
    pthread_attr_t attr;
    int ret;

    pthread_mutex_lock(&s_speak_q.lock);
    if (s_speak_q.started) {
        pthread_mutex_unlock(&s_speak_q.lock);
        return 0;
    }
    /* 先在锁内置"创建中", 让并发的调用方(UI 线程 / agent 线程)只建一个
     * worker; 创建失败时再回滚。 */
    s_speak_q.started = 1;
    pthread_mutex_unlock(&s_speak_q.lock);

    pthread_attr_init(&attr);
    /* 显式栈: Edge 路径在这条线程里跑 TLS 握手 + minimp3, 不能靠默认值。 */
    pthread_attr_setstacksize(&attr, SPEAK_WORKER_STACK);
    ret = pthread_create(&s_speak_q.thread, &attr, speak_worker, NULL);
    pthread_attr_destroy(&attr);

    if (ret != 0) {
        pthread_mutex_lock(&s_speak_q.lock);
        s_speak_q.started = 0;
        pthread_mutex_unlock(&s_speak_q.lock);
        syslog(LOG_ERR, "[%s] speak worker create failed: %d\n", TAG, ret);
        return -EIO;
    }

    syslog(LOG_INFO, "[%s] speak worker started (stack=%d, queue=%d)\n",
        TAG, SPEAK_WORKER_STACK, SPEAK_QUEUE_DEPTH);
    return 0;
}

/* 播报长度上限：语音只用来说"结论"，完整回答留在屏幕上。
 * 真机 2026-10-25 实测：长回答 445 字节 -> TTS 269KB PCM / 21 秒，
 * 大流量把 BT H4 接收队列打爆（rx overflow / drop invalid h4）。
 * 按 UTF-8 字符数截断，避免切出半个汉字。 */
/* 2026-11-01 v102a：屏幕显示全文，只把这部分送去 TTS。
 * v114b5：45→60 字，并改成"句界截断"——优先在句末标点收尾，
 * 避免用户听到"…包括父母、"这种半句话（报告 85 的 A0/A2）。 */
/* v114b9（A 报告 88）：90 字对"第一个句号在 55 字、第二个在 131 字"的回答无效，
 * 抬到 120 也不够；再加 floor=0.75×cap 的"最小填充"，避免停在过短的第一个句号。
 * 注意：234 字单请求会打满 512KB 响应缓冲，完整长答需同会话分段（不在本版）。 */
#define TTS_SPEAK_MAX_CHARS 120   /* v114b13：不再回退，按 A 报告做 120 字 + floor=0.75，长回答完整播报 */

static size_t utf8_count(const char *s)
{
    size_t n = 0;
    while (*s) {
        if ((*s & 0xC0) != 0x80) n++;
        s++;
    }
    return n;
}

static size_t utf8_len(const char *s)
{
    unsigned char c = (unsigned char)*s;
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    return 4;
}

static void speak_text_cap(char *s, size_t cap)
{
    size_t total = utf8_count(s);
    size_t chars = 0;
    size_t i = 0;
    size_t hard = 0;
    size_t strong = 0, strong_chars = 0;
    size_t weak = 0, weak_chars = 0;

    while (s[i] != '\0') {
        if ((s[i] & 0xC0) != 0x80) {
            if (chars == TTS_SPEAK_MAX_CHARS) { hard = i; break; }
            chars++;
        }
        i++;
    }
    if (s[i] == '\0') return;                /* 未超上限，原样 */

    /* 扫描 [0, hard)：按字符计数，记录最后一个句末标点/逗号的位置 */
    {
        size_t c = 0;
        for (size_t k = 0; k < hard; ) {
            unsigned char b = (unsigned char)s[k];
            size_t len = utf8_len(&s[k]);
            const unsigned char *p = (const unsigned char *)&s[k];
            int is_strong = 0, is_weak = 0;

            if (len == 3) {
                if (p[0]==0xE3 && p[1]==0x80 && p[2]==0x82) is_strong = 1;      /* 。 */
                else if (p[0]==0xEF && p[1]==0xBC && p[2]==0x81) is_strong=1;   /* ！ */
                else if (p[0]==0xEF && p[1]==0xBC && p[2]==0x9F) is_strong=1;   /* ？ */
                else if (p[0]==0xEF && p[1]==0xBC && p[2]==0x9B) is_strong=1;   /* ； */
                else if (p[0]==0xEF && p[1]==0xBC && p[2]==0x8C) is_weak=1;     /* ， */
                else if (p[0]==0xE3 && p[1]==0x80 && p[2]==0x81) is_weak=1;     /* 、 */
            }
            if ((b & 0xC0) != 0x80) c++;
            if (is_strong) { strong = k + len; strong_chars = c; }
            if (is_weak)   { weak = k + len;   weak_chars = c; }
            k += len;
        }
    }

    size_t floor_chars = (TTS_SPEAK_MAX_CHARS * 3) / 4;
    size_t cut; const char *kind;
    if (strong && strong_chars >= floor_chars) { cut = strong; kind = "strong"; }
    else if (weak) { cut = weak; kind = "weak"; }
    else { cut = hard; kind = "hard"; }

    if (cut + 4 <= cap) {
        s[cut] = (char)0xE2; s[cut+1] = (char)0x80; s[cut+2] = (char)0xA6; s[cut+3] = '\0';
    } else {
        s[cut] = '\0';
    }
    syslog(LOG_INFO, "[%s] speak_budget total=%zu spoken=%zu floor=%zu cut=%s\n",
           TAG, total, utf8_count(s), floor_chars, kind);
}

/* 是否还有 TTS 在拉流/播放/排队。
 * 2026-11-01 v102c：真机复现"上一轮 TTS 拉流 + 下一轮 LLM 请求"并发时
 * BT PAN 上两条 TLS 流会把 zblue 小缓冲打爆 → 整机冻死。agent_loop 在
 * 处理下一条消息前用本函数等待上一轮播报彻底结束，避免网络并发。 */
/* ── LLM / TTS 媒体回合互斥（v102d）────────────────────────────
 * 真机复现：上一轮 TTS 拉流与下一轮 LLM 请求并发时，BT PAN 两条 TLS 流
 * 打爆 zblue 小缓冲 → 整机冻死。is_busy 的"先检查后启动"存在竞态窗口，
 * 这里用条件变量做原子互斥：LLM 请求进行时 TTS 等待，反之亦然。
 *
 * ★ 2026-11-03 v125 P0-B：LLM 侧的等待改为**有界**（≤500 ms）。
 * 原来这里是无限等待：perf-net 实测 ask3 的 iter1 54.6 s 里有约 46 s 是在
 * 等 TTS 失败回退链（log:1640 “llm waits for tts to finish”、报告 §2.4），
 * 该等待对所有通道生效，BLE/cli 回合也会被拖住。跳过等待不会产生
 * POST 级 TLS 并发风险：LLM 与 MiMo TTS 的 HTTPS POST 都走
 * vela_https_post_json() 的**全局串行锁**（infra/vela_tls.c）；剩下的
 * edge/minimax 自带 TLS 是短连接且单次有界（403/鉴权失败秒级返回）。
 * TTS 侧等 LLM 的语义保持不变（TTS 由异步 worker 承担，不值得冒并发险）。 */
#define LLM_WAIT_TTS_MAX_MS 30000
/* P2A-COLLISION F2：TTS 侧等 LLM 回合的上限。原来无限 cond_wait：LLM 请求
 * 漏调 llm_end/异常时 speak worker 永久卡在 tts_begin，队列里用户回答一起
 * 被堵死（"TTS 再也不响"）。30s 后打 WARNING 并继续，避免永久静默。 */
#define TTS_WAIT_LLM_MAX_MS 30000

static pthread_mutex_t s_media_turn_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_media_turn_cond = PTHREAD_COND_INITIALIZER;
static int s_llm_active_count;
static int s_tts_active_count;

/* P2A-COLLISION：speak worker 判定"用户回合是否真的结束"用（决定能否清
 * s_turn_active）。加锁读计数，与 llm_begin/end 保持一致。 */
static bool voice_llm_active(void)
{
    bool active;

    pthread_mutex_lock(&s_media_turn_lock);
    active = (s_llm_active_count > 0);
    pthread_mutex_unlock(&s_media_turn_lock);

    return active;
}

void voice_channel_llm_begin(void)
{
    struct timespec deadline;

    pthread_mutex_lock(&s_media_turn_lock);

    if (s_tts_active_count > 0)
    {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += (time_t)(LLM_WAIT_TTS_MAX_MS / 1000);
        deadline.tv_nsec += (long)(LLM_WAIT_TTS_MAX_MS % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L)
        {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }

        syslog(LOG_INFO, "[%s] llm waits for tts to finish (max %dms)\n",
            TAG, LLM_WAIT_TTS_MAX_MS);

        while (s_tts_active_count > 0)
        {
            if (pthread_cond_timedwait(&s_media_turn_cond,
                    &s_media_turn_lock, &deadline) != 0)
            {
                break;      /* 超时：不再等，直接进 LLM */
            }
        }

        if (s_tts_active_count > 0)
        {
            syslog(LOG_WARNING,
                "[%s] tts still active after %dms, proceeding with LLM "
                "(HTTPS POSTs serialized by vela_https_post_json)\n",
                TAG, LLM_WAIT_TTS_MAX_MS);
        }
    }

    s_llm_active_count++;
    pthread_mutex_unlock(&s_media_turn_lock);
}

void voice_channel_llm_end(void)
{
    pthread_mutex_lock(&s_media_turn_lock);
    if (s_llm_active_count > 0)
    {
        s_llm_active_count--;
    }
    pthread_cond_broadcast(&s_media_turn_cond);
    pthread_mutex_unlock(&s_media_turn_lock);
}

void voice_channel_tts_begin(void)
{
    struct timespec deadline;
    bool waited = false;
    int rc;

    pthread_mutex_lock(&s_media_turn_lock);
    while (s_llm_active_count > 0)
    {
        if (!waited)
        {
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += (time_t)(TTS_WAIT_LLM_MAX_MS / 1000);
            deadline.tv_nsec +=
                (long)(TTS_WAIT_LLM_MAX_MS % 1000) * 1000000L;
            if (deadline.tv_nsec >= 1000000000L)
            {
                deadline.tv_sec++;
                deadline.tv_nsec -= 1000000000L;
            }
            waited = true;
            syslog(LOG_INFO,
                "[%s] tts waits for llm to finish (max %dms, llm_active=%d)\n",
                TAG, TTS_WAIT_LLM_MAX_MS, s_llm_active_count);
        }

        rc = pthread_cond_timedwait(&s_media_turn_cond, &s_media_turn_lock,
            &deadline);
        if (rc != 0)
        {
            syslog(LOG_WARNING,
                "[%s] tts_begin: wait llm timeout %dms (llm_active=%d) -> "
                "proceed (F2, avoid permanent TTS block)\n",
                TAG, TTS_WAIT_LLM_MAX_MS, s_llm_active_count);
            break;      /* F2：超时不再等，避免永久阻塞 */
        }
    }
    if (waited && s_llm_active_count == 0)
    {
        syslog(LOG_INFO, "[%s] tts_begin: llm finished, proceed\n", TAG);
    }
    s_tts_active_count++;
    pthread_mutex_unlock(&s_media_turn_lock);
}

void voice_channel_tts_end(void)
{
    pthread_mutex_lock(&s_media_turn_lock);
    if (s_tts_active_count > 0)
    {
        s_tts_active_count--;
    }
    pthread_cond_broadcast(&s_media_turn_cond);
    pthread_mutex_unlock(&s_media_turn_lock);
}

int voice_channel_is_busy(void)
{
    int busy = 0;

    pthread_mutex_lock(&s_speak_q.lock);
    if (s_speak_q.count > 0)
    {
        busy = 1;
    }
    pthread_mutex_unlock(&s_speak_q.lock);

    if (!busy && s_voice.tts_pb != NULL)
    {
        busy = 1;
    }

    /* v113：close/drain 窗口内 tts_pb 已被清空，但设备/播放器还在收尾；
     * 这里补一个 closing 标志，避免门禁在这 1–4 秒内误判空闲。 */
    if (!busy && s_tts_closing)
    {
        busy = 1;
    }

    /* VOICE-C：模板缓存本地播放不经过 speak 队列/tts_pb，但同样占着音频
     * 设备；对 agent_loop 的回合门禁必须算 busy。 */
    if (!busy && voice_tts_cache_is_playing())
    {
        busy = 1;
    }

    return busy;
}

/* 入队一句；budget=true 表示占用每 boot TTS 会话额度（每轮回答只第一句占）。
 * P2A-COLLISION：增加 origin（用户回答/主动关怀），队列满时按来源取舍：
 *   - care 条目：只淘汰最旧 care，care 上限 SPEAK_CARE_MAX；队列全被用户
 *     回答占满时丢弃新 care，**绝不淘汰用户回答**；
 *   - user 条目：先淘汰 care，仍满再按旧语义淘汰最旧 user（新回答优先）。
 * 每次入队/淘汰都打诊断日志（origin/hash/qlen/evicted）。 */
static int speak_enqueue_ex(const char* text, bool budget, bool drop_oldest,
                            uint8_t origin)
{
    char* copy;
    int before;
    int evicted = -1;

    if (!text || text[0] == '\0') {
        return -EINVAL;
    }

    copy = strdup(text);
    if (!copy) {
        return -ENOMEM;
    }
    if (speak_worker_ensure_started() != 0) {
        free(copy);
        return -EIO;
    }

    pthread_mutex_lock(&s_speak_q.lock);
    before = s_speak_q.count;

    if (origin == SPEAK_ORIGIN_CARE)
    {
        /* care 队列上限：多了先淘汰最旧 care（绝不淘汰 user 回答）。 */
        while (speak_queue_origin_count_locked(SPEAK_ORIGIN_CARE)
               >= SPEAK_CARE_MAX)
        {
            int idx = speak_queue_find_locked(SPEAK_ORIGIN_CARE);
            if (idx < 0)
            {
                break;
            }
            speak_queue_remove_locked(idx);
            evicted = SPEAK_ORIGIN_CARE;
        }
        if (s_speak_q.count >= SPEAK_QUEUE_DEPTH)
        {
            int idx = speak_queue_find_locked(SPEAK_ORIGIN_CARE);
            if (idx >= 0)
            {
                speak_queue_remove_locked(idx);
                evicted = SPEAK_ORIGIN_CARE;
            }
            else
            {
                pthread_mutex_unlock(&s_speak_q.lock);
                free(copy);
                syslog(LOG_WARNING,
                    "[%s] care enqueue dropped: queue full of user answers "
                    "(qlen=%d, never drop user)\n", TAG, before);
                return -ENOMEM;
            }
        }
    }
    else if (s_speak_q.count >= SPEAK_QUEUE_DEPTH)
    {
        /* 用户回答优先：先淘汰 care，再按旧语义淘汰最旧 user。 */
        int idx = speak_queue_find_locked(SPEAK_ORIGIN_CARE);

        if (idx >= 0)
        {
            speak_queue_remove_locked(idx);
            evicted = SPEAK_ORIGIN_CARE;
        }
        else if (drop_oldest)
        {
            speak_queue_remove_locked(0);
            evicted = SPEAK_ORIGIN_USER;
        }
        else
        {
            pthread_mutex_unlock(&s_speak_q.lock);
            free(copy);
            syslog(LOG_WARNING,
                "[%s] speak queue full, drop new sentence (origin=user)\n",
                TAG);
            return -ENOMEM;
        }
    }

    s_speak_q.slots[s_speak_q.count] = copy;
    s_speak_q.budget[s_speak_q.count] = budget;
    s_speak_q.origin[s_speak_q.count] = origin;
    s_speak_q.count++;
    pthread_cond_signal(&s_speak_q.cond);

    syslog(LOG_INFO,
        "[%s] queue enqueue: origin=%s hash=%08x budget=%d drop_oldest=%d "
        "qlen=%d->%d evicted=%s\n",
        TAG, (origin == SPEAK_ORIGIN_CARE) ? "care" : "user",
        (unsigned)speak_hash(text), (int)budget, (int)drop_oldest,
        before, s_speak_q.count,
        (evicted == SPEAK_ORIGIN_CARE) ? "care"
            : ((evicted == SPEAK_ORIGIN_USER) ? "user" : "none"));

    pthread_mutex_unlock(&s_speak_q.lock);
    return 0;
}

/* 旧入口：用户回答（dispatch/分句/流式）统一 origin=user，语义不变。 */
static int speak_enqueue(const char* text, bool budget, bool drop_oldest)
{
    return speak_enqueue_ex(text, budget, drop_oldest, SPEAK_ORIGIN_USER);
}

/* P2A-COLLISION F4：主动关怀本地语音专用入队（详见 voice_channel.h）。
 * 不占每 boot TTS 额度、cache-only、低优先级；返回 0 已入队，负 errno
 * 表示被拒（care 永不让用户回答被淘汰）。 */
int voice_channel_speak_care_async(const char* text)
{
    int rc = speak_enqueue_ex(text, false, false, SPEAK_ORIGIN_CARE);

    if (rc != 0)
    {
        syslog(LOG_INFO,
            "[%s] care voice rejected: rc=%d (cache_only=1, no user drop)\n",
            TAG, rc);
    }
    return rc;
}

/* ── VOICE-B：LLM 流式首句 TTS（docs_ble/112 §2B）───────────────────
 *
 * agent_loop 的 voice 流式 delta 线程调用 begin/feed/end：
 *   begin -> 清状态；
 *   feed  -> 攒到第一个完整句子（。！？；ASCII .!?; 或换行）就 speak_enqueue，
 *            不阻塞网络回调；后续句子继续入队（首句 budget=true，其余不扣额度）；
 *   end   -> flush 尾句 + 登记 takeover（随后 outbound dispatch 对同一段文本
 *            调 speak_async 时被哈希+长度匹配跳过，不重复整段）；
 *   abort -> 失败/工具轮/PTT 打断：丢弃流式状态，dispatch 回退原整段 TTS。
 * 播放器所有权仍在既有 speak worker；PTT abort 会清队列+清 stream 状态。 */
#define STREAM_BUF_BYTES   224
#define STREAM_TAKEOVER_MS 30000

typedef struct
{
    pthread_mutex_t lock;
    bool     active;
    bool     failed;
    bool     budget_used;
    int      sentences;
    char     buf[STREAM_BUF_BYTES];
    size_t   len;
    bool     takeover;
    uint32_t take_hash;
    uint32_t take_len;
    uint32_t take_deadline_ms;
} voice_stream_t;

static voice_stream_t s_stream = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static uint32_t stream_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static uint32_t stream_hash(const char* s)
{
    uint32_t h = 2166136261u;

    while (s != NULL && *s != '\0')
    {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

/* 3 字节 CJK 句末标点：。！？； */
static bool stream_is_term3(const char* p)
{
    const unsigned char* q = (const unsigned char*)p;

    return (q[0] == 0xE3 && q[1] == 0x80 && q[2] == 0x82)
        || (q[0] == 0xEF && q[1] == 0xBC && q[2] == 0x81)
        || (q[0] == 0xEF && q[1] == 0xBC && q[2] == 0x9F)
        || (q[0] == 0xEF && q[1] == 0xBC && q[2] == 0x9B);
}

/* 调用方持 stream lock：把一段（含标点）入队给 speak worker。
 * 返回 false 表示入队失败（end 不再 takeover，上层回退整段 TTS）。 */
static bool stream_emit_locked(const char* seg, size_t n)
{
    char tmp[STREAM_BUF_BYTES + 8];
    char* p;
    size_t plen;
    bool budget;

    if (n == 0 || n >= sizeof(tmp))
    {
        return true;
    }

    memcpy(tmp, seg, n);
    tmp[n] = '\0';

    p = tmp;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
    {
        p++;
    }
    plen = strlen(p);
    while (plen > 0
           && (p[plen - 1] == ' ' || p[plen - 1] == '\t'
               || p[plen - 1] == '\r' || p[plen - 1] == '\n'))
    {
        p[--plen] = '\0';
    }
    if (plen == 0)
    {
        return true;
    }

    budget = !s_stream.budget_used;
    if (speak_enqueue(p, budget, false) != 0)
    {
        s_stream.failed = true;
        syslog(LOG_WARNING,
            "[%s] stream TTS enqueue failed (sentence %d), fallback full speak\n",
            TAG, s_stream.sentences + 1);
        return false;
    }

    s_stream.budget_used = true;
    s_stream.sentences++;
    syslog(LOG_INFO,
        "[%s] stream TTS sentence #%d queued (%zu bytes, budget=%d)\n",
        TAG, s_stream.sentences, plen, (int)budget);
    return true;
}

void voice_channel_stream_begin(void)
{
    pthread_mutex_lock(&s_stream.lock);
    if (!s_stream.active)
    {
        s_stream.active = true;
        s_stream.failed = false;
        s_stream.budget_used = false;
        s_stream.sentences = 0;
        s_stream.len = 0;
        s_stream.takeover = false;
        syslog(LOG_INFO, "[%s] stream TTS begin (voice delta streaming)\n", TAG);
    }
    pthread_mutex_unlock(&s_stream.lock);
}

void voice_channel_stream_feed(const char* delta)
{
    size_t i;

    if (delta == NULL || delta[0] == '\0')
    {
        return;
    }

    pthread_mutex_lock(&s_stream.lock);
    if (!s_stream.active)
    {
        /* 没有 begin / 已被 tts_abort（PTT 打断）清掉：丢弃后续 delta，
         * 避免用户开始录音后还把上一条回答的句子排进队列。 */
        pthread_mutex_unlock(&s_stream.lock);
        return;
    }

    for (i = 0; delta[i] != '\0'; i++)
    {
        unsigned char c = (unsigned char)delta[i];
        bool term = false;

        /* 超长无标点：先硬切，避免首句等太久/缓冲越界。
         * 切点回退到 UTF-8 字符起始，保证不把半个汉字交给 TTS。 */
        if (s_stream.len >= STREAM_BUF_BYTES)
        {
            size_t cut = s_stream.len - 1;

            while (cut > 0
                   && ((unsigned char)s_stream.buf[cut] & 0xC0) == 0x80)
            {
                cut--;
            }
            if (cut > 0)
            {
                stream_emit_locked(s_stream.buf, cut);
                memmove(s_stream.buf, s_stream.buf + cut,
                    s_stream.len - cut);
                s_stream.len -= cut;
            }
            else
            {
                stream_emit_locked(s_stream.buf, s_stream.len);
                s_stream.len = 0;
            }
        }

        s_stream.buf[s_stream.len++] = (char)c;

        if (c == '\n' || c == '!' || c == '?' || c == ';' || c == '.')
        {
            term = true;
        }
        else if (s_stream.len >= 3
                 && stream_is_term3(&s_stream.buf[s_stream.len - 3]))
        {
            term = true;
        }

        if (term)
        {
            if (s_stream.len >= 4)
            {
                /* 至少 2 个汉字左右才念：避免“。”单独成句 */
                stream_emit_locked(s_stream.buf, s_stream.len);
                s_stream.len = 0;
            }
            /* 太短（如“好。”）先留着，与后文合并成一句 */
        }
    }

    {
        bool failed = s_stream.failed;
        pthread_mutex_unlock(&s_stream.lock);

        /* VOICE-B-FIX（B2）：入队失败不能留残句——立刻 tts_abort 清队列 +
         * 清 stream 状态，让 dispatch 只走一次整段 TTS 兜底。 */
        if (failed)
        {
            syslog(LOG_WARNING,
                "[%s] stream TTS feed: enqueue failed -> abort/clear queue, "
                "dispatch will full-speak once\n", TAG);
            voice_channel_tts_abort();
        }
    }
}

void voice_channel_stream_end(const char* full_text)
{
    bool takeover = false;
    bool failed;
    int sentences;

    pthread_mutex_lock(&s_stream.lock);
    if (!s_stream.active)
    {
        pthread_mutex_unlock(&s_stream.lock);
        return;
    }

    if (s_stream.len > 0)
    {
        stream_emit_locked(s_stream.buf, s_stream.len);
        s_stream.len = 0;
    }

    sentences = s_stream.sentences;
    failed = s_stream.failed;

    if (!failed && sentences > 0 && full_text != NULL
        && full_text[0] != '\0')
    {
        s_stream.takeover = true;
        s_stream.take_hash = stream_hash(full_text);
        s_stream.take_len = (uint32_t)strlen(full_text);
        s_stream.take_deadline_ms = stream_now_ms() + STREAM_TAKEOVER_MS;
        takeover = true;
    }

    s_stream.active = false;
    s_stream.len = 0;
    s_stream.failed = false;
    pthread_mutex_unlock(&s_stream.lock);

    syslog(LOG_INFO,
        "[%s] stream TTS end: sentences=%d takeover=%d failed=%d (full=%zu)\n",
        TAG, sentences, (int)takeover, (int)failed,
        (full_text != NULL) ? strlen(full_text) : (size_t)0);

    if (failed)
    {
        /* VOICE-B-FIX（B2）：有句子入队失败 -> 清掉已入队残句，避免
         * "残句 + dispatch 整段" 双重播放；dispatch 会整段 TTS 一次。 */
        syslog(LOG_WARNING,
            "[%s] stream TTS end failed -> tts_abort residual queue\n", TAG);
        voice_channel_tts_abort();
    }
}

void voice_channel_stream_abort(void)
{
    int sentences;
    bool active;

    pthread_mutex_lock(&s_stream.lock);
    active = s_stream.active;
    sentences = s_stream.sentences;
    s_stream.active = false;
    s_stream.failed = false;
    s_stream.budget_used = false;
    s_stream.sentences = 0;
    s_stream.len = 0;
    s_stream.takeover = false;
    pthread_mutex_unlock(&s_stream.lock);

    if (active || sentences > 0)
    {
        syslog(LOG_INFO,
            "[%s] stream TTS abort: sentences=%d -> flush queue (tts_abort) + "
            "dispatch full-speak fallback\n", TAG, sentences);

        /* VOICE-B-FIX（B1）：失败/工具轮/watchdog/cascade 等 abort 路径必须
         * 同时清掉已入队的残句并停当前播放，否则 dispatch 再整段 TTS 时
         * 用户会听到"残句 + 整段"重复播放。
         * 注意：tts_abort 内部也会回调本函数，用 in_progress 防递归。 */
        if (!s_tts_abort_in_progress)
        {
            voice_channel_tts_abort();
        }
    }
}

/* 流式 end 成功后，outbound dispatch 会用同一段文本调 speak_async/speak；
 * 这里哈希+长度匹配则跳过（一次性，30s 内有效）。 */
static bool voice_channel_stream_consume_takeover(const char* text)
{
    bool hit = false;

    if (text == NULL)
    {
        return false;
    }

    pthread_mutex_lock(&s_stream.lock);
    if (s_stream.takeover
        && stream_now_ms() <= s_stream.take_deadline_ms
        && (uint32_t)strlen(text) == s_stream.take_len
        && stream_hash(text) == s_stream.take_hash)
    {
        s_stream.takeover = false;
        hit = true;
    }
    pthread_mutex_unlock(&s_stream.lock);
    return hit;
}

/* PTT 打断用：还有流式句/队列未播完？ */
static bool voice_channel_tts_pending(void)
{
    bool pending;

    pthread_mutex_lock(&s_speak_q.lock);
    pending = (s_speak_q.count > 0);
    pthread_mutex_unlock(&s_speak_q.lock);

    if (!pending)
    {
        pthread_mutex_lock(&s_stream.lock);
        pending = s_stream.active;
        pthread_mutex_unlock(&s_stream.lock);
    }

    return pending;
}

/* 返回 0 表示文本已入队(不代表念成功, 失败只记日志)。 */
int voice_channel_speak_async(const char* text)
{
    if (voice_channel_stream_consume_takeover(text))
    {
        syslog(LOG_INFO,
            "[%s] stream takeover: skip duplicate full TTS (%zu bytes)\n",
            TAG, strlen(text));
        return 0;
    }
    return speak_enqueue(text, true, true);
}


/* 2026-11-03 分句 TTS：把最终回答按句号/问号/感叹号/分号拆开，逐句入队。
 *
 * 为什么：MiMo TTS 是按整段文本生成音频的（实测约 0.13s/汉字 + 1s 固定），
 * 66 字回答整段首块要 9-11s；拆成 15-20 字的首句则 3-4s 就出声，后面的句子
 * 在播放首句时继续排队合成。TTS 队列从 1 深度升到 8，配合 speak_worker 串行
 * 播放；第一句占用一次每 boot 会话额度，后续句子不再扣额度（否则一次回答就
 * 吃光 5 次上限）。 */
int voice_channel_speak_sentences(const char* text)
{
    /* 2026-11-03：分句 TTS 实验版出现队列 worker 不消费回归，先透传整段。 */
    return voice_channel_speak_async(text);

    char seg[192];
    size_t seglen = 0;
    size_t len;
    size_t i;
    bool first = true;
    int enq = 0;

    if (!text || text[0] == '\0') {
        return -EINVAL;
    }
    len = strlen(text);

    for (i = 0; i < len; ) {
        unsigned char c = (unsigned char)text[i];
        size_t n = 1;

        if ((c & 0xE0) == 0xC0) n = 2;
        else if ((c & 0xF0) == 0xE0) n = 3;
        else if ((c & 0xF8) == 0xF0) n = 4;
        if (i + n > len) n = 1;

        /* 超长无标点：到 150 字节切一次，避免首句等太久 */
        if (seglen + n >= sizeof(seg) - 4 && seglen > 0) {
            seg[seglen] = '\0';
            if (speak_enqueue(seg, first, false) == 0) { first = false; enq++; }
            seglen = 0;
        }

        memcpy(seg + seglen, text + i, n);
        seglen += n;

        bool term = false;
        if (n == 3) {
            const unsigned char* q = (const unsigned char*)(text + i);
            term = (q[0] == 0xE3 && q[1] == 0x80 && q[2] == 0x82) /* 。 */
                || (q[0] == 0xEF && q[1] == 0xBC && q[2] == 0x81) /* ！ */
                || (q[0] == 0xEF && q[1] == 0xBC && q[2] == 0x9F) /* ？ */
                || (q[0] == 0xEF && q[1] == 0xBC && q[2] == 0x9B); /* ； */
        } else if (n == 1) {
            term = (c == '!' || c == '?' || c == ';' || c == '\n');
        }
        i += n;

        if (term) {
            if (seglen >= 6) {                       /* 至少两个字才念，避免“。”单独一句 */
                seg[seglen] = '\0';
                if (speak_enqueue(seg, first, false) == 0) { first = false; enq++; }
            }
            seglen = 0;
        }
    }

    if (seglen > 0) {
        seg[seglen] = '\0';
        if (speak_enqueue(seg, first, false) == 0) { first = false; enq++; }
    }
    if (enq == 0) {
        return speak_enqueue(text, true, true);
    }
    syslog(LOG_INFO, "[%s] sentence TTS queued %d chunks\n", TAG, enq);
    return 0;
}

/* 2026-11-03：停止当前 TTS 播放并清空排队文本。
 *
 * 用途：新用户回合（尤其 App/BLE 聊天）到达时，不再“等待上一句 TTS 播完”，
 * 而是直接打断播报、优先处理新消息。这样：
 *   1) 聊天回复不用等 30s（体验）；
 *   2) 不会出现「旧 TTS 还在拉流/播放 + 新 LLM POST」并发（稳定性）。
 * 语音通道自己的 PTT 仍然走 voice_channel_start() 里既有的防回声停播逻辑。 */
void voice_channel_tts_abort(void)
{
    int i;

    if (s_tts_abort_in_progress)
    {
        return;     /* 防 stream_abort <-> tts_abort 递归 */
    }
    s_tts_abort_in_progress = 1;

    /* VOICE-C：缓存播放也要立刻停（PTT/新回合打断时 cache 可能正占播放器） */
    voice_tts_cache_stop();

    /* 清空 speak 队列（如果有排队中的句子）；诊断按来源计数，
     * 便于确认 PTT/失败路径是否清掉了 care 残句。 */
    pthread_mutex_lock(&s_speak_q.lock);
    {
        int care_n = speak_queue_origin_count_locked(SPEAK_ORIGIN_CARE);
        int user_n = s_speak_q.count - care_n;

        for (i = 0; i < s_speak_q.count; i++) {
            free(s_speak_q.slots[i]);
            s_speak_q.slots[i] = NULL;
        }
        s_speak_q.count = 0;
        pthread_mutex_unlock(&s_speak_q.lock);

        if (care_n > 0 || user_n > 0) {
            syslog(LOG_INFO,
                "[%s] tts_abort: cleared queue (user=%d care=%d)\n",
                TAG, user_n, care_n);
        }
    }

    /* VOICE-B：清掉流式首句 TTS 状态（begin/feed 可能还在网络回调里跑，
     * 清 active 后后续 feed 会自启新流；PTT 场景下新回合会重新 begin）。 */
    voice_channel_stream_abort();

    /* 中断正在进行的 TTS 流/播放 */
    pthread_mutex_lock(&s_voice.lock);
    if (s_voice.tts_pb != NULL) {
        syslog(LOG_INFO, "[%s] tts_abort: stop active playback\n", TAG);
        s_voice.tts_abort = 1;
        audio_playback_stop(s_voice.tts_pb);
    }
    pthread_mutex_unlock(&s_voice.lock);

    s_turn_active = 0;
    s_tts_abort_in_progress = 0;
    syslog(LOG_INFO, "[%s] tts_abort: queue cleared\n", TAG);
}

static int voice_channel_speak_internal(const char* text, bool count_budget)
{
    if (!text || text[0] == '\0') {
        return -EINVAL;
    }

    /* v114b2：2 秒内相同文本只播一次（B 报告：麦克风自检会重复播报、吃掉配额） */
    {
        static uint32_t s_last_hash;
        static uint32_t s_last_ms;
        uint32_t h = 2166136261u;
        const char* p;
        struct timespec ts;
        uint32_t now;

        for (p = text; *p; p++) {
            h ^= (uint8_t)*p;
            h *= 16777619u;
        }
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
        if (s_last_hash == h && (uint32_t)(now - s_last_ms) < 2000) {
            syslog(LOG_INFO, "[%s] duplicate speak suppressed\n", TAG);
            return 0;
        }
        s_last_hash = h;
        s_last_ms = now;
    }

    /* ── VOICE-C 接线（docs/112 §2C，perf-fw C1/C5）──────────────────
     * 顺序：PTT 录音态检查 -> 缓存 -> 每 boot TTS 额度检查/自增。
     * - 命中：本地 PCM 播放（无网络 TTS、**不占 TTS session 额度**）；
     * - 未命中/忙/设备错误：回落原网络 TTS 路径，行为不变；
     * - 录音态不查缓存（AEC，后面原逻辑会 -EBUSY）；
     * - speak_internal 在 speak worker 线程内调用（同步 NSH 路径也会经这里，
     *   缓存播放器自身用 trylock 防多播放器，无并发风险）。 */
    {
        bool recording;

        pthread_mutex_lock(&s_voice.lock);
        recording = (s_voice.state != VOICE_IDLE);
        pthread_mutex_unlock(&s_voice.lock);

        if (!recording) {
            int cache_ret = voice_tts_cache_play(text);

            if (cache_ret == 0) {
                syslog(LOG_INFO,
                    "[%s] voice_cache hit -> local PCM played, skip network TTS\n",
                    TAG);
                return 0;
            }
            if (cache_ret == -ECANCELED) {
                syslog(LOG_INFO,
                    "[%s] voice_cache canceled by stop (PTT), silent done\n",
                    TAG);
                return 0;
            }
            if (cache_ret == -EBUSY) {
                syslog(LOG_WARNING,
                    "[%s] voice_cache busy -> fallback network TTS\n", TAG);
            } else if (cache_ret != -ENOENT) {
                syslog(LOG_WARNING,
                    "[%s] voice_cache play failed: %d -> fallback network TTS\n",
                    TAG, cache_ret);
            }
        }
    }

    /* v113b：每 boot TTS 次数上限（真机第 3 次复用 TLS 连接会整机停摆）
     * 2026-11-03：分句 TTS 只有首句 count_budget=true，后续句不再扣额度。 */
    if (count_budget && s_tts_sessions >= TTS_MAX_SESSIONS_PER_BOOT) {
        syslog(LOG_WARNING,
            "[%s] TTS per-boot limit reached (%d), text only\n",
            TAG, (int)s_tts_sessions);
        return -EAGAIN;
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

    /* 播报截断：屏幕显示全文，TTS 只念前 100 个字符（体验与流量都受控） */
    speak_text_cap(clean, text_len + 1);

    syslog(LOG_INFO, "[%s] speak: \"%.*s\" (%zu bytes)\n",
        TAG, 60, clean, strlen(clean));

    if (count_budget) {
        s_tts_sessions++;
    }
    syslog(LOG_INFO, "[%s] TTS session %d/%d (budget=%d)\n", TAG,
        (int)s_tts_sessions, (int)TTS_MAX_SESSIONS_PER_BOOT, (int)count_budget);

    /* 注: 回答的"上屏/入历史"**不在这里做**。曾经加在这里, 结果与 outbound
     * 通道的镜像**重复写两次**(真机 2026-10-16 实测: PetPage 同一句被更新两遍,
     * 一条不带"(本地)"、一条带)。
     * 正确位置是 outbound_dispatch_task 的各通道分支 —— 那里一处覆盖
     * cli / websocket / lvgl_ui / care / voice, 且每条回答只经过一次。
     * speak() 是所有播报的汇聚点, 但**不是**所有回答的必经点。 */
    clock_gettime(CLOCK_MONOTONIC, &s_tts_start);
    s_tts_first_chunk = 0;
    s_tts_pcm_written = 0;
    s_tts_write_errors = 0;

    /* v125 P0-B：per-turn 计时起点（TTS 开始）。后面 first chunk / network
     * done / speak done / play wait 全部相对这里算，单轮不累计。 */
    syslog(LOG_INFO, "[%s] TTS begin (backend '%s', %u Hz)\n",
        TAG, voice_tts_get_backend() ? voice_tts_get_backend() : "none",
        voice_tts_get_sample_rate());

    /* v125 P0-B：旧代码用陈旧的 s_asr_done_ts 打 "LLM latency"，实测打出
     * 273261 / 342047 / 397047 ms 的累计墙钟（perf-net §2.5，
     * log:1214/1617/2026）。现在：① 只在 voice 回合（s_turn_active 由 ASR
     * 完成置位）且时间戳非 0 时才打；② **读后清零**，一轮只用一次，
     * 不可能跨轮累加。语义 = "本轮 ASR 完成 → 本轮 TTS 开始"的毫秒差
     * （其中包含 LLM 与派发耗时，可与 agent_loop 的 [turn] llm_ms 对账）。 */
    {
        struct timespec asr_done = s_asr_done_ts;

        s_asr_done_ts.tv_sec = 0;
        s_asr_done_ts.tv_nsec = 0;

        if (asr_done.tv_sec > 0 && s_turn_active) {
            long turn_ms = (s_tts_start.tv_sec - asr_done.tv_sec) * 1000
                + (s_tts_start.tv_nsec - asr_done.tv_nsec) / 1000000;
            /* 验收用字段名：tts_start_ms（ASR 完成 → 本轮 TTS 开始，单轮） */
            syslog(LOG_INFO,
                "[%s] tts_start_ms=%ld (ASR done -> TTS begin, voice turn)\n",
                TAG, turn_ms);
        }
    }

    /* Try streaming TTS first (callback per chunk).  The device is opened
     * with the rate of the *active* backend; voice_tts_speak_stream()
     * resamples if a fallback backend produces a different rate (the
     * Volcengine HTTP one-shot is 16kHz, the WS stream and Edge are 24kHz). */
    unsigned int tts_rate = voice_tts_get_sample_rate();

    syslog(LOG_INFO, "[%s] speak: playback %u Hz, backend '%s'\n", TAG,
        tts_rate, voice_tts_get_backend() ? voice_tts_get_backend() : "none");

    audio_playback_t* pb = audio_playback_open(
        AGENT_AUDIO_PLAYBACK_DEV,
        tts_rate,
        AGENT_VOICE_CHANNELS,
        AGENT_VOICE_BITS);

    if (!pb) {
        syslog(LOG_ERR, "[%s] playback open failed\n", TAG);
        if (count_budget && s_tts_sessions > 0) s_tts_sessions--;   /* v114b9：失败退款 */
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
    s_tts_closing = 1;      /* v113：close 收尾窗口，is_busy/start 都算忙 */
    pthread_mutex_unlock(&s_voice.lock);

    audio_playback_close(pb);
    s_tts_closing = 0;
    free(clean);

    if (ret != 0) {
        syslog(LOG_ERR,
            "[%s] TTS stream failed: %d (pcm_written=%zu bytes, write_err=%d, first_chunk=%d)\n",
            TAG, ret, s_tts_pcm_written, s_tts_write_errors, s_tts_first_chunk);
        if (count_budget && s_tts_first_chunk == 0 && s_tts_sessions > 0) {
            s_tts_sessions--;                       /* v114b9：没出首块的失败退款 */
            syslog(LOG_INFO, "[%s] TTS session refunded (no first chunk)\n", TAG);
        }
        pthread_mutex_unlock(&s_voice.speak_lock);
        return ret;
    }

    if (s_tts_pcm_written == 0) {
        /* "后端说成功、播放器一字节没收到"必须显式报出来：真机以前只能从
         * [audio_pb] closing (0 bytes written) 反推（perf-net §3.5）。 */
        syslog(LOG_WARNING,
            "[%s] TTS ok but 0 PCM bytes written (write_err=%d)\n",
            TAG, s_tts_write_errors);
    }

    struct timespec tend;
    clock_gettime(CLOCK_MONOTONIC, &tend);
    long total_ms = (tend.tv_sec - s_tts_start.tv_sec) * 1000
        + (tend.tv_nsec - s_tts_start.tv_nsec) / 1000000;
    /* 验收用字段名：tts_total_ms（本轮 TTS 开始 → 播放完毕，单轮） */
    syslog(LOG_INFO,
        "[%s] speak done: total %ldms (play wait %ldms, pcm_written=%zu bytes, "
        "write_err=%d) tts_total_ms=%ld\n",
        TAG, total_ms, total_ms - net_ms, s_tts_pcm_written,
        s_tts_write_errors, total_ms);
    pthread_mutex_unlock(&s_voice.speak_lock);
    return 0;
}

int voice_channel_speak(const char* text)
{
    /* VOICE-B：若本轮已流式播过同一段文本，跳过整段重复 TTS。 */
    if (voice_channel_stream_consume_takeover(text))
    {
        syslog(LOG_INFO,
            "[%s] stream takeover: skip duplicate full TTS (sync, %zu bytes)\n",
            TAG, strlen(text));
        return 0;
    }
    return voice_channel_speak_internal(text, true);
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

        /* 用户话术入历史(NSH voice_test_asr 调试路径) */
#ifdef CONFIG_AI_AGENT_LVGL_UI
        lvgl_ui_channel_log(text, true);
#endif

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
