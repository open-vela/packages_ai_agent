/****************************************************************************
 * pcm_media_player.c - media_player_* 实现在 NuttX 音频设备上 (CONFIG_MEDIA off)
 *
 * ai_agent 的语音链路 (voice_channel_speak -> audio_playback.c) 使用
 * media_player_open/prepare/start/write_data/stop/close，而本工作区没有 media
 * 框架（frameworks/multimedia/media 需要 cutils/properties.h，编译不过）。
 * 这里把这套 API 直接实现在真机验证过的播放设备上：
 *
 *   PCM -> /dev/audio/pcm0p -> AUDPRC -> AUDCODEC DAC -> 模拟 DAC1 -> PA10 -> 喇叭
 *   (docs_ble/30 §10/§11: 16kHz、零欠载、寄存器级验证)
 *
 * 设备只支持 16kHz/单声道/16bit；TTS 推流为 24kHz (AGENT_TTS_WS_SAMPLE_RATE)，
 * 因此写入时做线性插值重采样（任意输入率 -> 16000）。
 *
 * 线程模型：write_data() 只做重采样 + 入环形缓冲（满则丢最旧并计数），worker
 * 线程把 4096B 一块的 apb 投给设备并回收 DEQUEUE 回来的 apb —— TTS 的 socket
 * 回调不会被播放节奏阻塞。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/audio/audio.h>

/* Media player over the NuttX audio device (CONFIG_MEDIA off).
 *
 * ai_agent 的语音链路 (voice_channel_speak -> audio_playback.c) 走的是
 * media_player_open/prepare/start/write_data/stop/close。本工作区没有 media
 * 框架 (frameworks/multimedia/media 需要 cutils/properties.h)，所以这里把这套
 * API 直接实现在**真机验证过的**播放设备上:
 *
 *   PCM -> /dev/audio/pcm0p -> AUDPRC -> AUDCODEC DAC -> 模拟 DAC1 -> PA10 -> 喇叭
 *   (见 docs_ble/30 §10/§11: 16kHz、零欠载、寄存器级验证)
 *
 * 设备只支持 16kHz/单声道/16bit；TTS 推流是 24kHz (AGENT_TTS_WS_SAMPLE_RATE)，
 * 因此写入时做线性插值重采样 (任意输入率 -> 16000)。
 *
 * 线程模型: write_data() 只做重采样 + 入环形缓冲(满则丢最旧并计数)，worker
 * 线程负责把 4096B 一块的 apb 投给音频设备并回收 DEQUEUE 回来的 apb —— 这样
 * TTS 的 socket 回调不会被播放节奏阻塞。
 */
#ifndef CONFIG_MEDIA
#  include <errno.h>
#  include <fcntl.h>
#  include <mqueue.h>
#  include <pthread.h>
#  include <stdio.h>
#  include <stdlib.h>
#  include <string.h>
#  include <sys/ioctl.h>
#  include <syslog.h>
#  include <time.h>

#  include <nuttx/audio/audio.h>

/* 注意: 本文件故意不 include <unistd.h> —— 它会声明 gettid(), 与本文件上方
 * 的 gettid 弱桩冲突 (sim 构建需要那个桩)。open() 来自 <fcntl.h>, close()
 * 与延时这里自行声明/用 nanosleep 实现。 */
extern int close(int fd);

static void pcm_delay_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* AUDIO-FIX：pthread_cond_timedwait 统一用 CLOCK_MONOTONIC 算 abstime。
 * 旧代码用 CLOCK_REALTIME：BLE/RTC 对时跳变时，一次 20ms 的条件等待会被
 * 放大成分钟/小时级 —— worker 看起来"卡死"（stop 超时报 worker still
 * running，随后泄漏/后续 TTS reap=0 的触发条件之一）。
 * 条件变量在 media_player_open() 里用 condattr_setclock(MONOTONIC) 初始化。 */
static void pcm_cond_deadline(struct timespec* ts, unsigned ms)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_nsec += (long)ms * 1000000L;
    while (ts->tv_nsec >= 1000000000L)
    {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

#  define PCM_DEV_DEFAULT "/dev/audio/pcm0p"
#  define PCM_OUT_RATE    16000
/* v114b13：TTS 语速提升 10%（用户反馈"说话速度太慢"）。
 * 线性插值步进放大 10%，输出时长缩短 1/1.1；音调随之略升，属可接受折中。 */
#  define PCM_TTS_SPEED_PCT  110
/* 2026-10-26: 96KB -> 256KB。TTS 现在下载快了(15KB/s 链路)，
 * 整段 PCM 会在 decode 完成后**一次性**推给播放器，例如 100KB MP3 解出
 * 500KB/24kHz PCM(>300KB/16kHz)，96KB 环会直接丢最旧数据 —— 真机日志
 * dropped=28800(=57.6KB≈1.8s 语音)，用户听到的就是"话说不完整"。
 * 环放在 heap(PSRAM) 上，扩到 256KB 可容纳 8s@16kHz。 */
#  define PCM_RING_BYTES  (256 * 1024)
#  define PCM_MAX_APBS    8
/* 2026-11-01 v102a：固定 mq 名 + 16 格会被"2 客户端 × 8 APB = 16 条
 * DEQUEUE"恰好塞满 → audio_sendmsg 在关中断临界区里 file_mq_send 永久
 * 睡眠 → 整机停摆。改为每个 player 实例唯一名字 + 加大队列，并让客户端
 * mq 带 O_NONBLOCK（满时立即返回 EAGAIN，绝不阻塞上层）。 */
#  define PCM_MQ_NAME_FMT "/pcm_media_q%d"
#  define PCM_MQ_MAXMSG   64

struct pcm_media_s
{
    int      fd;
    mqd_t    mq;
    char     mq_name[32];
    pthread_t worker;
    pthread_mutex_t lock;
    pthread_cond_t  cv;
    struct ap_buffer_s* apbs[PCM_MAX_APBS];
    int      napbs;
    struct ap_buffer_s* freeq[PCM_MAX_APBS];
    int      nfree;
    unsigned char ring[PCM_RING_BYTES];
    volatile unsigned rhead;
    unsigned rtail;
    size_t   rbytes;
    int      rate_in;
    int      rate_out;
    volatile int running;
    volatile int draining;
    volatile int failed;
    int      worker_started;
    unsigned long written;
    unsigned long dropped;
    unsigned long underruns;
    /* 2026-11-01 诊断计数：定位"暂停后声音卡死/大量丢数据"的断点 */
    unsigned long enq_apbs;       /* 成功入队的 APB 数 */
    unsigned long reap_deqs;      /* 回收到的 DEQUEUE 数 */
    unsigned long foreign_deqs;   /* 广播收到的非本 player APB 数 */
    unsigned long ring_max;       /* ring 最高水位 */
    unsigned long wait_ms_total;  /* write_data 背压总等待毫秒 */
    unsigned long reap_empty;     /* 没有可回收 DEQUEUE 的次数 */
    char     dev[64];

    /* 2026-11-04 v142k AUDIO-FIX（TTS 欠载/APB 不回收）：
     * worker join 超时后**禁止 free**（UAF 会污染后续播放的 fd/mq/audio 状态）：
     *   leaked=1        -> player 已登记到 g_leaked，close 只 defer，不释放；
     *   mq_msgs         -> mq 收到消息总数（诊断 deq=0：是否根本没收到 DEQUEUE）；
     *   stop_timeouts   -> stop 等 worker 超时次数（软重启后异常的可观测判据）。 */
    int      leaked;
    volatile int stop_done;       /* P2：本 player 已完成一次 stop（重复调用直接返回） */
    volatile int worker_done;     /* worker 真正退出（置位后才可 join+free） */
    unsigned long mq_msgs;
    unsigned long stop_timeouts;
    /* AUDIO-FIX F4（perf-fw RCA §4）：stop 串行化。
     * PTT 打断（audio_playback_stop）与 speak 线程 close 会对同一 player
     * 并发调 media_player_stop → 双等待/双 join（UB）/标志撕裂。 */
    pthread_mutex_t stop_lock;
};

/* ── AUDIO-FIX：泄漏 player 回收表（纯 RAM，无新线程）─────────────────
 * stop 超时后 worker 可能仍活着，直接 free p/fd/mq → UAF（后续 TTS reap=0）。
 * 这里只挂指针、保留全部资源；等 worker 自己退出（running==0）后，由下一次
 * media_player_open() 在既有调用线程里 join+free（上限 2，O(2) 无阻塞）。
 * 若 worker 永不退出，保持泄漏到重启（远好于 UAF/死机）。 */
#define PCM_MAX_LEAKED 2
static struct pcm_media_s* g_leaked[PCM_MAX_LEAKED];
static int g_nleaked;
static pthread_mutex_t g_leak_lock = PTHREAD_MUTEX_INITIALIZER;

static void pcm_free_player(struct pcm_media_s* p);
static void pcm_leak_register(struct pcm_media_s* p);
static void pcm_reclaim_leaked(void);

static void pcm_ring_push(struct pcm_media_s* p, const unsigned char* src,
    size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        if (p->rbytes >= PCM_RING_BYTES)
        {
            /* 环形缓冲满: 丢最旧的一个字节对(保持样点对齐) */
            p->rtail = (p->rtail + 2) % PCM_RING_BYTES;
            p->rbytes -= 2;
            p->dropped++;
        }
        p->ring[p->rhead] = src[i];
        p->rhead = (p->rhead + 1) % PCM_RING_BYTES;
        p->rbytes++;
        if (p->rbytes > p->ring_max) p->ring_max = p->rbytes;
    }
}

static size_t pcm_ring_pop(struct pcm_media_s* p, unsigned char* dst,
    size_t want)
{
    size_t n = (want < p->rbytes) ? want : p->rbytes;
    n &= ~1u;                                  /* 16bit 样点对齐 */
    for (size_t i = 0; i < n; i++)
    {
        dst[i] = p->ring[p->rtail];
        p->rtail = (p->rtail + 1) % PCM_RING_BYTES;
    }
    p->rbytes -= n;
    return n;
}

/* 线性插值重采样 (in_rate -> PCM_OUT_RATE), 输入/输出均为 16bit 单声道 */
static size_t pcm_resample(struct pcm_media_s* p, const short* in, size_t in_n,
    short* out, size_t out_cap)
{
    if (p->rate_in == p->rate_out)
    {
        size_t n = (in_n < out_cap) ? in_n : out_cap;
        memcpy(out, in, n * sizeof(short));
        return n;
    }

    size_t out_n = 0;
    double step = (double)p->rate_in / (double)p->rate_out;
    step = step * (double)PCM_TTS_SPEED_PCT / 100.0;   /* v114b13 语速 */
    for (double pos = 0.0; out_n < out_cap; pos += step)
    {
        size_t i0 = (size_t)pos;
        if (i0 + 1 >= in_n) break;
        double frac = pos - (double)i0;
        double v = (double)in[i0] * (1.0 - frac) + (double)in[i0 + 1] * frac;
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        out[out_n++] = (short)v;
    }
    return out_n;
}

static struct ap_buffer_s* pcm_take_free(struct pcm_media_s* p)
{
    struct ap_buffer_s* apb = NULL;

    pthread_mutex_lock(&p->lock);
    if (p->nfree > 0) apb = p->freeq[--p->nfree];
    pthread_mutex_unlock(&p->lock);
    return apb;
}

static void pcm_put_free(struct pcm_media_s* p, struct ap_buffer_s* apb)
{
    pthread_mutex_lock(&p->lock);
    if (p->nfree < PCM_MAX_APBS) p->freeq[p->nfree++] = apb;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->lock);
}

/* 回收设备交还的 apb (非阻塞) */
static void pcm_reap(struct pcm_media_s* p)
{
    struct audio_msg_s msg;
    struct timespec ts;

    for (;;)
    {
        clock_gettime(CLOCK_REALTIME, &ts);
        if (mq_timedreceive(p->mq, (char*)&msg, sizeof(msg), NULL, &ts) <= 0)
            return;
        p->mq_msgs++;
        if (msg.msg_id == AUDIO_MSG_DEQUEUE && msg.u.ptr != NULL)
        {
            /* 2026-11-01: upper 的 DEQUEUE 是"广播"给所有注册客户端，
             * 可能收到别的 player 的 APB —— 只回收自己池子里的，防止
             * 把别人的缓冲塞进自己的 freeq 造成串音/重复入队。 */
            int own = 0;
            for (int i = 0; i < p->napbs; i++)
            {
                if (p->apbs[i] == (struct ap_buffer_s*)msg.u.ptr)
                {
                    own = 1;
                    break;
                }
            }
            if (!own)
            {
                p->foreign_deqs++;
                continue;
            }
            pcm_put_free(p, (struct ap_buffer_s*)msg.u.ptr);
            p->reap_deqs++;
        }
    }
}

static void* pcm_worker(void* arg)
{
    struct pcm_media_s* p = (struct pcm_media_s*)arg;

    while (p->running)
    {
        size_t have;

        pthread_mutex_lock(&p->lock);
        while (p->running && p->rbytes < 4096 && !p->draining)
        {
            struct timespec ts;

            pcm_cond_deadline(&ts, 20);
            pthread_cond_timedwait(&p->cv, &p->lock, &ts);
        }
        have = p->rbytes;
        pthread_mutex_unlock(&p->lock);

        if (!p->running) break;

        if (have == 0)
        {
            if (p->draining) break;              /* 排空完毕 */
            continue;
        }

        /* AUDIO-FIX：stop 超时后 stop/close 会置 failed，worker 别再把
         * 新 APB 投给已 STOP 的设备（否则残留到下一句 TTS 里）。 */
        if (p->failed)
        {
            break;
        }

        struct ap_buffer_s* apb = pcm_take_free(p);
        if (!apb)
        {
            pcm_reap(p);                         /* 没有空闲 apb: 回收再试 */
            pcm_delay_ms(2);
            continue;
        }

        pthread_mutex_lock(&p->lock);
        size_t n = pcm_ring_pop(p, (unsigned char*)apb->samp, apb->nmaxbytes);
        pthread_mutex_unlock(&p->lock);

        if (n == 0)
        {
            pcm_put_free(p, apb);
            continue;
        }

        apb->nbytes = n;
        apb->curbyte = 0;

        struct audio_buf_desc_s d;
        memset(&d, 0, sizeof(d));
        d.u.buffer = apb;
        d.numbytes = n;
        if (ioctl(p->fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&d) < 0)
        {
            p->failed = 1;
            pcm_put_free(p, apb);
            break;
        }
        p->enq_apbs++;
        p->written += n;
        pcm_reap(p);

        /* 诊断：每 5 秒打一次内部状态 */
        {
            static uint32_t dbg_last;
            static uint32_t dbg_ticks;
            uint32_t now = clock_systime_ticks();

            if (dbg_last == 0) dbg_last = now;
            if ((uint32_t)(now - dbg_last) >= (uint32_t)(5 * CLK_TCK))
              {
                dbg_last = now;
                syslog(LOG_INFO,
                       "[pcm_media] dbg rbytes=%u nfree=%d held=%d enq=%lu "
                       "reap=%lu mq=%lu written=%lu dropped=%lu ring_max=%lu "
                       "wait_ms=%lu leaked=%d rate=%d/%d\n",
                       (unsigned)p->rbytes, p->nfree,
                       (int)(p->napbs - p->nfree), p->enq_apbs,
                       p->reap_deqs, p->mq_msgs, p->written, p->dropped,
                       p->ring_max, p->wait_ms_total, p->leaked,
                       p->rate_in, p->rate_out);
                (void)dbg_ticks;
              }
        }
    }

    /* ★ 关键：worker 退出前自己清 running。
     *
     * 原来的 media_player_stop() 里是
     *     for (int i = 0; i < 500 && p->running; i++) pcm_delay_ms(10);
     *     p->running = 0;
     * 而 p->running 只由 stop 自己在循环之后清 —— worker 从不写它，
     * 于是循环条件恒为真，**每一次停止都固定空转 5 秒**。
     * 这 5 秒里它仍持有设备 fd、8 个 APB 和已注册的 mqueue，
     * 是"连点几次歌就有几个播放器重叠"的直接原因。
     *
     * 在这里清掉之后，stop 的等待会在 worker 真正退出时立刻结束。
     * AUDIO-FIX：worker_done 单独标记"线程真的走到返回点"——stop 超时路径
     * 也会把 running 置 0，reclaim 只能用 worker_done 判断能否安全 join。 */
    p->worker_done = 1;
    p->running = 0;
    return NULL;
}

/* AUDIO-FIX：真正释放 player（只在 worker 已退出/join 且设备已 STOP 后调用）。 */
static void pcm_free_player(struct pcm_media_s* p)
{
    for (int i = 0; i < p->napbs; i++)
    {
        if (p->apbs[i])
        {
            struct audio_buf_desc_s d;
            d.u.buffer = p->apbs[i];
            ioctl(p->fd, AUDIOIOC_FREEBUFFER, (unsigned long)&d);
            p->apbs[i] = NULL;
        }
    }
    if (p->mq != (mqd_t)-1)
    {
        if (p->fd >= 0) ioctl(p->fd, AUDIOIOC_UNREGISTERMQ, 0);
        mq_close(p->mq);
        if (p->mq_name[0]) mq_unlink(p->mq_name);
        p->mq = (mqd_t)-1;
    }
    if (p->fd >= 0)
    {
        close(p->fd);
        p->fd = -1;
    }
    pthread_mutex_destroy(&p->stop_lock);
    pthread_mutex_destroy(&p->lock);
    pthread_cond_destroy(&p->cv);
    free(p);
}

/* AUDIO-FIX：登记泄漏 player（去重；表满则保持泄漏到重启）。 */
static void pcm_leak_register(struct pcm_media_s* p)
{
    int i;

    if (p == NULL)
    {
        return;
    }

    pthread_mutex_lock(&g_leak_lock);
    for (i = 0; i < g_nleaked; i++)
    {
        if (g_leaked[i] == p)
        {
            pthread_mutex_unlock(&g_leak_lock);
            return;
        }
    }
    if (g_nleaked < PCM_MAX_LEAKED)
    {
        g_leaked[g_nleaked++] = p;
        pthread_mutex_unlock(&g_leak_lock);
        syslog(LOG_WARNING,
            "[pcm_media] leak registered (%d/%d): worker kept alive, "
            "fd/mq/APBs retained\n", g_nleaked, PCM_MAX_LEAKED);
        return;
    }
    pthread_mutex_unlock(&g_leak_lock);
    syslog(LOG_ERR,
        "[pcm_media] leak table full (%d), player %p stays leaked until reboot\n",
        PCM_MAX_LEAKED, p);
}

/* AUDIO-FIX：回收已退出的泄漏 player。由下一个 media_player_open() 在
 * speak 线程里调用：running==0 才 join+free，否则原地保留（O(2) 无阻塞）。 */
static void pcm_reclaim_leaked(void)
{
    struct pcm_media_s* free_list[PCM_MAX_LEAKED];
    int nf = 0;
    int i = 0;

    pthread_mutex_lock(&g_leak_lock);
    while (i < g_nleaked)
    {
        struct pcm_media_s* p = g_leaked[i];

        if (p != NULL && p->worker_done != 0)
        {
            g_leaked[i] = g_leaked[--g_nleaked];
            g_leaked[g_nleaked] = NULL;
            free_list[nf++] = p;
        }
        else
        {
            i++;
        }
    }
    pthread_mutex_unlock(&g_leak_lock);

    for (i = 0; i < nf; i++)
    {
        struct pcm_media_s* p = free_list[i];

        syslog(LOG_INFO,
            "[pcm_media] reclaim leaked player: worker exited -> join+free "
            "(mq=%lu enq=%lu reap=%lu)\n",
            p->mq_msgs, p->enq_apbs, p->reap_deqs);
        if (p->worker_started)
        {
            pthread_join(p->worker, NULL);
            p->worker_started = 0;
        }
        pcm_free_player(p);
    }
}

/* AUDIO-FIX（perf-fw 建议）：统计"worker 还活着"的泄漏 player。
 * 有僵尸 worker 时拒绝新开：设备级共享的 audio core 里旧客户端状态未清，
 * 新 player 与僵尸 worker 抢设备会把后续 TTS 状态机搞乱（reap=0/deq=0）。
 * 返回 >0 表示应拒绝本次 open（上层会报"音频忙"）。 */
static int pcm_leaked_alive_count(void)
{
    int i;
    int n = 0;

    pthread_mutex_lock(&g_leak_lock);
    for (i = 0; i < g_nleaked; i++)
    {
        if (g_leaked[i] != NULL && g_leaked[i]->worker_done == 0)
        {
            n++;
        }
    }
    pthread_mutex_unlock(&g_leak_lock);

    return n;
}

void* media_player_open(const char* url)
{
    struct pcm_media_s* p;
    int zombies;
    (void)url;

    /* AUDIO-FIX：先回收上一轮泄漏的 player（worker 已退出时可安全 free）。 */
    pcm_reclaim_leaked();

    zombies = pcm_leaked_alive_count();
    if (zombies > 0)
    {
        syslog(LOG_ERR,
            "[pcm_media] open REJECTED: %d leaked player(s) with worker still "
            "alive (refuse to share audio device; retry later / reboot)\n",
            zombies);
        return NULL;
    }

    p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->fd = -1;
    /* calloc 之后 mq 是 0，但 close 里的哨兵是 (mqd_t)-1。
     * 不显式置 -1 的话，任何在 mq_open 之前失败的 prepare 路径都会走到
     *     mq_close(0)  ->  close(0)
     * 把进程的 fd 0 关掉，并且 mq_unlink 一个自己没创建的队列。 */
    p->mq = (mqd_t)-1;
    p->rate_in = PCM_OUT_RATE;
    p->rate_out = PCM_OUT_RATE;
    snprintf(p->dev, sizeof(p->dev), "%s", PCM_DEV_DEFAULT);
    pthread_mutex_init(&p->lock, NULL);
    pthread_mutex_init(&p->stop_lock, NULL);   /* F4：stop 串行化 */

    /* AUDIO-FIX：条件变量用 MONOTONIC 时钟（配合 pcm_cond_deadline），
     * 避免 RTC/BLE 对时跳变把 20ms 等待放大成小时级 → worker "卡死"。 */
    {
        pthread_condattr_t cattr;

        pthread_condattr_init(&cattr);
        pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
        pthread_cond_init(&p->cv, &cattr);
        pthread_condattr_destroy(&cattr);
    }
    return p;
}

int media_player_prepare(void* player, const char* url, const char* opts)
{
    struct pcm_media_s* p = (struct pcm_media_s*)player;
    struct audio_caps_s caps;
    struct ap_buffer_info_s binfo;
    struct mq_attr attr;
    int rate = PCM_OUT_RATE;

    (void)url;
    if (!p) return -EINVAL;

    /* opts: "format=s16le:sample_rate=24000:ch_layout=mono" */
    if (opts)
    {
        const char* r = strstr(opts, "sample_rate=");
        if (r) rate = atoi(r + 12);
        if (strstr(opts, "s24") || strstr(opts, "s32"))
        {
            syslog(LOG_ERR, "[pcm_media] only s16le supported\n");
            return -ENOTSUP;
        }
        if (strstr(opts, "ch_layout=stereo"))
        {
            syslog(LOG_ERR, "[pcm_media] only mono supported\n");
            return -ENOTSUP;
        }
    }
    if (rate <= 0) rate = PCM_OUT_RATE;
    p->rate_in = rate;

    p->fd = open(p->dev, O_WRONLY);
    if (p->fd < 0)
    {
        syslog(LOG_ERR, "[pcm_media] open %s failed: %d\n", p->dev, errno);
        return -errno;
    }

    memset(&caps, 0, sizeof(caps));
    caps.ac_len            = sizeof(caps);
    caps.ac_type           = AUDIO_TYPE_OUTPUT;
    caps.ac_subtype        = AUDIO_FMT_PCM;
    caps.ac_channels       = 1;
    caps.ac_controls.hw[0] = PCM_OUT_RATE;
    caps.ac_controls.b[2]  = 16;
    if (ioctl(p->fd, AUDIOIOC_CONFIGURE, (unsigned long)&caps) < 0)
    {
        syslog(LOG_ERR, "[pcm_media] CONFIGURE failed: %d\n", errno);
        return -errno;
    }

    memset(&binfo, 0, sizeof(binfo));
    if (ioctl(p->fd, AUDIOIOC_GETBUFFERINFO, (unsigned long)&binfo) != OK ||
        binfo.buffer_size <= 0 || binfo.nbuffers <= 0)
    {
        binfo.buffer_size = 4096;
        binfo.nbuffers = PCM_MAX_APBS;
    }
    if (binfo.nbuffers > PCM_MAX_APBS) binfo.nbuffers = PCM_MAX_APBS;

    memset(&attr, 0, sizeof(attr));
    attr.mq_maxmsg  = PCM_MQ_MAXMSG;
    attr.mq_msgsize = sizeof(struct audio_msg_s);
    {
        /* 唯一队列名：避免多播放器共用同一 mq 互相丢/串 DEQUEUE */
        static volatile int s_mq_seq;
        int seq = (int)__sync_fetch_and_add(&s_mq_seq, 1);
        snprintf(p->mq_name, sizeof(p->mq_name), PCM_MQ_NAME_FMT, seq);
    }
    p->mq = mq_open(p->mq_name, O_RDWR | O_CREAT | O_NONBLOCK, 0666, &attr);
    if (p->mq == (mqd_t)-1)
    {
        syslog(LOG_ERR, "[pcm_media] mq_open(%s) failed: %d\n", p->mq_name, errno);
        return -errno;
    }
    syslog(LOG_INFO, "[pcm_media] mq %s opened (maxmsg=%d, NONBLOCK)\n",
           p->mq_name, PCM_MQ_MAXMSG);
    if (ioctl(p->fd, AUDIOIOC_REGISTERMQ, (unsigned long)p->mq) < 0)
    {
        syslog(LOG_ERR, "[pcm_media] REGISTERMQ failed: %d\n", errno);
        return -errno;
    }

    for (int i = 0; i < binfo.nbuffers; i++)
    {
        struct ap_buffer_s* apb = NULL;
        struct audio_buf_desc_s d;
        memset(&d, 0, sizeof(d));
        d.numbytes = binfo.buffer_size;
        d.u.pbuffer = &apb;
        if (ioctl(p->fd, AUDIOIOC_ALLOCBUFFER, (unsigned long)&d) != sizeof(d))
        {
            syslog(LOG_ERR, "[pcm_media] ALLOCBUFFER %d failed\n", i);
            break;
        }
        p->apbs[p->napbs++] = apb;
        p->freeq[p->nfree++] = apb;
    }
    if (p->napbs == 0) return -ENOMEM;

    syslog(LOG_INFO, "[pcm_media] prepared: %s, in=%dHz out=%dHz, %d x %d B\n",
        p->dev, p->rate_in, p->rate_out, p->napbs, binfo.buffer_size);

    /* 诊断：音频上半部是设备级共享的，第二个客户端 prepare 会打断正在播的流。
     * 音乐播放期间如果有别人（TTS 等）也来 prepare，这里一定要叫出来，
     * 否则又会出现"30 秒只有一小段卡住的声音"却找不到原因。 */
    if (p->rate_in != p->rate_out)
      {
        syslog(LOG_WARNING,
               "[pcm_media] ★ 有第二个客户端在准备音频设备（in=%dHz out=%dHz）——"
               "若此时音乐在播，会被打断\n",
               p->rate_in, p->rate_out);
      }

    return 0;
}

int media_player_start(void* player)
{
    struct pcm_media_s* p = (struct pcm_media_s*)player;
    if (!p || p->fd < 0) return -EINVAL;

    p->running = 1;
    p->draining = 0;
    p->failed = 0;
    if (pthread_create(&p->worker, NULL, pcm_worker, p) != 0)
    {
        p->running = 0;
        syslog(LOG_ERR, "[pcm_media] worker create failed\n");
        return -EIO;
    }
    p->worker_started = 1;

    if (ioctl(p->fd, AUDIOIOC_START) < 0)
    {
        syslog(LOG_ERR, "[pcm_media] START failed: %d\n", errno);
        return -errno;
    }
    return 0;
}

ssize_t media_player_write_data(void* player, const void* data, size_t len)
{
    struct pcm_media_s* p = (struct pcm_media_s*)player;
    short* tmp;
    size_t in_n, cap, out_n;

    if (!p || !data || len < 2) return -EINVAL;
    if (p->failed) return -EIO;

    in_n = len / 2;
    cap = (size_t)((double)in_n * p->rate_out / p->rate_in) + 8;
    tmp = malloc(cap * sizeof(short));
    if (!tmp) return -ENOMEM;

    out_n = pcm_resample(p, (const short*)data, in_n, tmp, cap);

    /* 满环时**背压等待**而不是丢数据(2026-10-26 修"语音不完整"主因)。
     * TTS 回调是独立线程，这里最多等 ~200ms 让 worker 把数据喂给设备；
     * 仍然满才退回"丢最旧"的保底策略(计数 dropped, 便于诊断)。 */
    size_t need = out_n * sizeof(short);
    int waited_ms = 0;

    pthread_mutex_lock(&p->lock);
    while (p->rbytes > 0 && p->rbytes + need > PCM_RING_BYTES
           && waited_ms < 200 && !p->failed)
    {
        struct timespec ts;

        pcm_cond_deadline(&ts, 10);
        pthread_cond_timedwait(&p->cv, &p->lock, &ts);
        waited_ms += 10;
    }
    if (waited_ms > 0)
      {
        static uint32_t last_log;

        p->wait_ms_total += (unsigned long)waited_ms;
        if ((uint32_t)(clock_systime_ticks() - last_log) >=
            (uint32_t)(2 * CLK_TCK))
          {
            last_log = clock_systime_ticks();
            syslog(LOG_WARNING,
                   "[pcm_media] backpressure waited=%dms rbytes=%u "
                   "nfree=%d enq=%lu reap=%lu dropped=%lu\n",
                   waited_ms, (unsigned)p->rbytes, p->nfree,
                   p->enq_apbs, p->reap_deqs, p->dropped);
          }
      }

    pcm_ring_push(p, (const unsigned char*)tmp, need);
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->lock);

    free(tmp);
    return (ssize_t)len;                        /* 已接收全部输入字节 */
}

int media_player_stop(void* player)
{
    struct pcm_media_s* p = (struct pcm_media_s*)player;
    if (!p) return -EINVAL;

    int worker_hung = 0;
    int pending_ms;

    /* AUDIO-FIX F4（perf-fw RCA §4）：PTT 打断 + speak close 可能并发调
     * stop（voice_channel.c:858/1733/1787 与 1920），串行化整个 stop，
     * 避免双等待/双 pthread_join（UB）/leaked 标志撕裂。 */
    pthread_mutex_lock(&p->stop_lock);

    /* P2（perf-fw RCA）：正常/泄漏的重复 stop 直接返回，避免重跑
     * 3–20s 预算、重复 drain 与重复 stopped 日志。
     * 注意：并发中的 stop 仍由 stop_lock 串行等待（close 依赖它的完成），
     * 这里只优化"已完成一次 stop 之后的再次调用"。 */
    if (p->stop_done)
    {
        pthread_mutex_unlock(&p->stop_lock);
        return 0;
    }

    /* AUDIO-FIX：已泄漏的 player 不再等待（worker 可能还活着）。重复 stop
     * 若再等/再 JOIN 会把下一句 TTS 拖慢；close 会继续 defer 回收。 */
    if (p->leaked)
    {
        syslog(LOG_WARNING,
               "[pcm_media] stop: player already leaked (worker_started=%d "
               "running=%d), skip wait; close will defer\n",
               p->worker_started, (int)p->running);
        p->stop_done = 1;
        pthread_mutex_unlock(&p->stop_lock);
        return 0;
    }

    /* v114b8（C 报告）：按积压估算收尾预算。
     * pending = 环里字节 / 32KB/s + 设备队列 APB × 128ms；预算 = pending + 1500ms，
     * clamp 3–20s；worker 与设备两段等待共用同一预算，避免长 TTS 固定 4s 被切尾。 */
    pthread_mutex_lock(&p->lock);
    pending_ms = (int)(p->rbytes / 32) + (int)((p->napbs - p->nfree) * 128);
    pthread_mutex_unlock(&p->lock);

    int budget_ms = pending_ms + 1500;
    if (budget_ms < 3000) budget_ms = 3000;
    if (budget_ms > 20000) budget_ms = 20000;
    int budget_iters = budget_ms / 10;

    if (p->worker_started)
    {
        pthread_mutex_lock(&p->lock);
        p->draining = 1;
        pthread_cond_broadcast(&p->cv);
        pthread_mutex_unlock(&p->lock);

        for (int i = 0; i < budget_iters && p->running; i++) pcm_delay_ms(10);

        /* AUDIO-FIX：再给 500ms grace —— worker 可能恰好卡在一次 ioctl/
         * 条件等待里；立刻判死会制造"泄漏句柄 + 后续 reap=0"。 */
        if (p->running)
        {
            for (int g = 0; g < 50 && p->running; g++) pcm_delay_ms(10);
        }

        if (p->running)
        {
            worker_hung = 1;
            p->failed = 1;      /* 让 worker 的下一轮 ENQUEUE 失败并退出 */
            p->stop_timeouts++;
            syslog(LOG_ERR,
                "[pcm_media] stop: worker STILL running after %dms -> force "
                "running=0, try short join before LEAK (worker_hung=1)\n",
                budget_ms);
        }

        /* AUDIO-FIX（按 Lead 实测补充）：无论是否 hung，先把 running 强制置 0
         * 再广播，然后给一个短窗口（200ms）让 worker 自己走到退出点；
         * 退出了就正常 join（不泄漏），只有真卡死才登记 leak。 */
        p->running = 0;
        pthread_cond_broadcast(&p->cv);

        if (worker_hung)
        {
            for (int g = 0; g < 20 && p->worker_done == 0; g++) pcm_delay_ms(10);
        }

        if (p->worker_done)
        {
            pthread_join(p->worker, NULL);
            p->worker_started = 0;
            p->leaked = 0;
            syslog(LOG_INFO,
                "[pcm_media] worker exited%s, join OK (no leak)\n",
                worker_hung ? " after force-stop" : "");
        }
        else
        {
            p->leaked = 1;
            pcm_leak_register(p);
            syslog(LOG_ERR,
                "[pcm_media] stop: worker STILL alive 200ms after force-stop "
                "-> LEAK player (keep fd/mq/APBs, no join/free; reclaimed when "
                "worker_done)\n");
        }
    }

    /* 等设备把已入队的 APB 全部播完再 STOP: worker 退出只代表"喂完"，
     * 设备队列里通常还有 ~0.5-1s 音频，直接 STOP 会把尾音切掉 —— 这是
     * 2026-10-26 用户反馈"语音不能完整全部出来"的第二个原因。
     * AUDIO-FIX：leaked 时只短等 500ms（worker 状态未知，长等只会拖慢下一句）。 */
    if (p->fd >= 0 && p->napbs > 0)
    {
        int iters = p->leaked ? 50 : budget_iters;
        int waited = 0;
        for (int i = 0; i < iters; i++)
        {
            int free_all;
            pthread_mutex_lock(&p->lock);
            free_all = (p->nfree == p->napbs);
            pthread_mutex_unlock(&p->lock);
            if (free_all) break;
            pcm_reap(p);
            pcm_delay_ms(10);
            waited += 10;
        }
        syslog(LOG_INFO,
               "[pcm_media] stop wait reason=%s waited=%dms pending=%dms "
               "rbytes=%u held=%d mq=%lu leaked=%d\n",
               p->leaked ? "worker_hung"
                         : ((p->nfree == p->napbs) ? "free" : "timeout"),
               waited, pending_ms, (unsigned)p->rbytes,
               (int)(p->napbs - p->nfree), p->mq_msgs, p->leaked);
    }

    /* v114b4：APB 全部 reap 只代表 DMA/上层消费完，DAC/PA 里还有最后一点
     * 采样没放完；用户实测"最后两个字没说出来"。STOP 前留 150ms。 */
    if (p->fd >= 0)
    {
        pcm_delay_ms(150);
        ioctl(p->fd, AUDIOIOC_STOP, 0);
    }

    syslog(LOG_INFO,
        "[pcm_media] stopped (written=%lu dropped=%lu enq=%lu reap=%lu "
        "mq=%lu ring_max=%lu wait_ms=%lu leaked=%d stop_timeouts=%lu)\n",
        p->written, p->dropped, p->enq_apbs, p->reap_deqs, p->mq_msgs,
        p->ring_max, p->wait_ms_total, p->leaked, p->stop_timeouts);
    p->stop_done = 1;                       /* P2：后续 stop 快速返回 */
    pthread_mutex_unlock(&p->stop_lock);   /* F4 */
    return 0;
}

int media_player_close(void* player, int dummy)
{
    struct pcm_media_s* p = (struct pcm_media_s*)player;
    (void)dummy;
    if (!p) return -EINVAL;

    /* AUDIO-FIX：未被 stop 的错误路径先尝试正常收尾（stop 幂等）。 */
    if (p->worker_started && !p->leaked)
    {
        media_player_stop(p);
    }

    /* AUDIO-FIX（2026-11-04 v142k）：worker 仍活着（stop 超时/从未 stop）
     * 绝不能 free 句柄：旧 worker 会继续用 p/fd/mq/APBs；free 后 fd 号被
     * 下一次 TTS 复用，正是"前次 stop 超时 → 后续每次 reap=0/deq=0"的
     * 根因路径。这里只登记泄漏、保留 p/fd/mq/APBs，等 worker 退出后由
     * 下一次 media_player_open() 回收（无新线程、O(2)）。
     * 返回 -EBUSY 让上层知道"资源仍占用（安全泄漏）"。 */
    if (p->worker_started || p->leaked)
    {
        p->leaked = 1;
        p->failed = 1;   /* 让仍活着的 worker 尽快退出（ioctl ENQUEUE 会被拒） */
        /* 2026-11-04 v142k AUDIO-FIX（按 Lead 实测补充）：**绝不 free 存活
         * worker 的句柄**。旧路径清了 worker_started 又不 join，随后 close
         * free p/fd/mq/APB，旧 worker 继续用 → UAF；且 fd 号被下一次 TTS
         * 复用后，旧 worker 的 ioctl 会打到新会话上（"前次 stop 超时 →
         * 后续每次 reap=0/deq=0"）。
         * 这里只登记泄漏、保留 p/fd/mq/APB 原样；worker_done 置位后由下一次
         * media_player_open() 在既有线程里 join+free（无新线程）。 */
        pcm_leak_register(p);
        syslog(LOG_ERR,
            "[pcm_media] close DEFERRED: worker alive/leaked "
            "(worker_started=%d leaked=%d fd=%d mq=%d) -> KEEP p/fd/mq/APBs "
            "(never free live worker); reclaimed when worker_done\n",
            p->worker_started, p->leaked, p->fd, (int)p->mq);
        return -EBUSY;
    }

    pcm_free_player(p);
    return 0;
}

int media_player_pause(void* player)
{
    (void)player;
    return 0;
}

int media_player_seek(void* player, unsigned int msec)
{
    (void)player; (void)msec;
    return -ENOTSUP;
}

int media_player_set_event_callback(void* player, void* cookie, void* cb)
{
    (void)player; (void)cookie; (void)cb;
    return 0;
}

int media_player_get_position(void* player, unsigned int* msec)
{
    struct pcm_media_s* p = (struct pcm_media_s*)player;
    if (!p || !msec) return -EINVAL;
    *msec = (unsigned int)(p->written * 1000 / (PCM_OUT_RATE * 2));
    return 0;
}

int media_player_get_duration(void* player, unsigned int* msec)
{
    (void)player;
    if (msec) *msec = 0;
    return -ENOTSUP;
}

/* 用**播放器自己的 fd** 设音量。
 *
 * 为什么必须这样：audio_tx 驱动的 tx_configure 会把音量写进设备级的
 * g_tx.volume，但"又来一个 open"会让它重新初始化通路 —— 实测在播放过程中
 * 另开一个 /dev/audio/pcm0p 去发音量 ioctl，会直接把正在播的声音打断
 * （表现就是"改个音量，歌没了"）。所以音量只能走已经在用的那个 fd。 */
int media_player_set_volume(void* player, int volume)
{
    struct pcm_media_s* p = (struct pcm_media_s*)player;
    struct audio_caps_s caps;

    if (!p || p->fd < 0) return -EINVAL;

    memset(&caps, 0, sizeof(caps));
    caps.ac_len            = sizeof(caps);
    caps.ac_type           = AUDIO_TYPE_FEATURE;
    caps.ac_format.hw      = AUDIO_FU_VOLUME;
    caps.ac_controls.hw[0] = (uint16_t)(int16_t)volume;

    if (ioctl(p->fd, AUDIOIOC_CONFIGURE, (unsigned long)&caps) < 0)
        return -errno;

    return 0;
}

int media_policy_set_stream_volume(const char* stream, int volume)
{
    (void)stream; (void)volume;
    return 0;
}

#endif /* !CONFIG_MEDIA */
