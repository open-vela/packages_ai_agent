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

#  define PCM_DEV_DEFAULT "/dev/audio/pcm0p"
#  define PCM_OUT_RATE    16000
#  define PCM_RING_BYTES  (96 * 1024)
#  define PCM_MAX_APBS    8
#  define PCM_MQ_NAME     "/pcm_media_q"

struct pcm_media_s
{
    int      fd;
    mqd_t    mq;
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
    char     dev[64];
};

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
        if (msg.msg_id == AUDIO_MSG_DEQUEUE && msg.u.ptr != NULL)
        {
            pcm_put_free(p, (struct ap_buffer_s*)msg.u.ptr);
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
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 20 * 1000 * 1000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
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
        p->written += n;
        pcm_reap(p);
    }

    return NULL;
}

void* media_player_open(const char* url)
{
    struct pcm_media_s* p = calloc(1, sizeof(*p));
    (void)url;
    if (!p) return NULL;
    p->fd = -1;
    p->rate_in = PCM_OUT_RATE;
    p->rate_out = PCM_OUT_RATE;
    snprintf(p->dev, sizeof(p->dev), "%s", PCM_DEV_DEFAULT);
    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->cv, NULL);
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
    attr.mq_maxmsg  = 16;
    attr.mq_msgsize = sizeof(struct audio_msg_s);
    p->mq = mq_open(PCM_MQ_NAME, O_RDWR | O_CREAT, 0666, &attr);
    if (p->mq == (mqd_t)-1)
    {
        syslog(LOG_ERR, "[pcm_media] mq_open failed: %d\n", errno);
        return -errno;
    }
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

    pthread_mutex_lock(&p->lock);
    pcm_ring_push(p, (const unsigned char*)tmp, out_n * sizeof(short));
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->lock);

    free(tmp);
    return (ssize_t)len;                        /* 已接收全部输入字节 */
}

int media_player_stop(void* player)
{
    struct pcm_media_s* p = (struct pcm_media_s*)player;
    if (!p) return -EINVAL;

    if (p->worker_started)
    {
        pthread_mutex_lock(&p->lock);
        p->draining = 1;
        pthread_cond_broadcast(&p->cv);
        pthread_mutex_unlock(&p->lock);

        for (int i = 0; i < 500 && p->running; i++) pcm_delay_ms(10);
        p->running = 0;
        pthread_cond_broadcast(&p->cv);
        pthread_join(p->worker, NULL);
        p->worker_started = 0;
    }

    if (p->fd >= 0) ioctl(p->fd, AUDIOIOC_STOP, 0);

    syslog(LOG_INFO, "[pcm_media] stopped (written=%lu dropped=%lu)\n",
        p->written, p->dropped);
    return 0;
}

int media_player_close(void* player, int dummy)
{
    struct pcm_media_s* p = (struct pcm_media_s*)player;
    (void)dummy;
    if (!p) return -EINVAL;

    if (p->worker_started) media_player_stop(p);

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
        mq_unlink(PCM_MQ_NAME);
    }
    if (p->fd >= 0) close(p->fd);

    pthread_mutex_destroy(&p->lock);
    pthread_cond_destroy(&p->cv);
    free(p);
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

int media_policy_set_stream_volume(const char* stream, int volume)
{
    (void)stream; (void)volume;
    return 0;
}

#endif /* !CONFIG_MEDIA */
