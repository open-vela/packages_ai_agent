/****************************************************************************
 * mic_selfcheck.c - On-demand microphone self test.
 *
 * 见 mic_selfcheck.h 的设计说明。这里只强调三件事:
 *
 *  1) **采集序列照抄 audio_rec_test_main.c**, 一行都没有自己发明:
 *     RESERVE -> CONFIGURE(16k/16bit/mono) -> mq -> REGISTERMQ ->
 *     GETBUFFERINFO -> ALLOCBUFFER+ENQUEUEBUFFER xN -> START ->
 *     mq_timedreceive(AUDIO_MSG_DEQUEUE) -> 用完 re-enqueue -> STOP。
 *
 *  2) **退出条件必须是墙钟**, 不是"空闲轮数"。参考实现用
 *     idle_loops < RATE 配 2s 超时, 驱动不投递时理论上是 16000 x 2s。
 *     这里给每次 receive 200ms 超时 + 总时长硬上限 (秒数 + 2s)。
 *
 *  3) **增益档从寄存器读回**, 不靠猜。HAL_AUDCODEC_Config_ADCPath_Volume()
 *     把 dB 编码进 ADC_CH0_CFG 的 ROUGH_VOL[11:8] / FINE_VOL[15:12]:
 *         rough = (volume + 60) / 6
 *         fine  = ((volume + 60) % 6) << 1
 *     反解 volume = rough*6 + (fine>>1) - 60。所以自检不需要驱动配合就能
 *     把"当前 Config_ADCPath_Volume 的实参"打出来 (+30dB 实验档)。
 *     寄存器只在采集成功后读 —— 那时 audcodec 时钟/电源一定开着, 读寄存器
 *     是安全的; 设备没开时读一个关时钟的外设寄存器在部分 SoC 上会挂总线。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <mqueue.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "health/mic_selfcheck.h"
#include "agent_config.h"
#include "agent_compat.h"

#ifdef CONFIG_AUDIO

#include <nuttx/audio/audio.h>

#if defined(CONFIG_EXAMPLES_AUDIO_SETUP_DEVNAME)
#  define MIC_CHECK_DEVPATH "/dev/audio/" CONFIG_EXAMPLES_AUDIO_SETUP_DEVNAME
#elif defined(AGENT_AUDIO_CAPTURE_DEV)
#  define MIC_CHECK_DEVPATH AGENT_AUDIO_CAPTURE_DEV
#else
#  define MIC_CHECK_DEVPATH "/dev/audio/pcm0c"
#endif

/* 采集参数: 与 AGENT_VOICE_* 一致 (16kHz/16bit/mono) */
#define MIC_CHECK_RATE      AGENT_VOICE_SAMPLE_RATE
#define MIC_CHECK_CHANNELS  AGENT_VOICE_CHANNELS
#define MIC_CHECK_BITS      AGENT_VOICE_BITS

#define MIC_CHECK_MQNAME    "/mic_selfcheck_mq"
#define MIC_CHECK_MQ_MSGS   sizeof(struct audio_msg_s)
#define MIC_CHECK_MQ_DEPTH  16

/* 单次 mq_timedreceive 超时: 200ms 足够 (驱动半缓冲周期 ~16Hz) */
#define MIC_CHECK_RECV_MS   200
/* 总时长余量: 录制秒数之外多给这么多, 到点无条件收摊 */
#define MIC_CHECK_SLACK_MS  2000

#define MIC_CHECK_TARGET    \
    ((long)MIC_SELFCHECK_SECONDS * MIC_CHECK_RATE * (MIC_CHECK_BITS / 8) * \
     MIC_CHECK_CHANNELS)

/* SF32LB52 AUDCODEC 寄存器: AUDCODEC_BASE + offsetof(AUDCODEC_TypeDef,
 * ADC_CH0_CFG) = 0x50088000 + 8*4 = 0x50088020。
 * 本模块不 include 厂商寄存器头 (ai_agent 的 include 路径里没有 cmsis 目录,
 * 硬加会把 core_cm33.h 整条依赖拖进来), 所以这里只固定这一个只读地址,
 * 并把原始值一起打出来供人工核对。
 *
 * 只在 SF32LB52 上读: 换芯片时那个地址不是 audcodec, 读它没有任何意义
 * (更糟的是可能挂总线)。其它芯片上 gain_known 报 false, 报告里就不带增益。 */
#if defined(CONFIG_ARCH_CHIP_SF32LB52)
#  define MIC_CHECK_HAVE_ADC_CFG  1
#  ifndef MIC_CHECK_ADC_CFG_ADDR
#    define MIC_CHECK_ADC_CFG_ADDR  0x50088020UL
#  endif
#  define MIC_CHECK_ADC_CFG_MSK   0x0000FFFFUL
#endif

static pthread_mutex_t s_run_lock = PTHREAD_MUTEX_INITIALIZER;

/* ── helpers ─────────────────────────────────────────────────── */

static long mic_check_ms_since(const struct timespec *t0)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)(now.tv_sec - t0->tv_sec) * 1000L +
           (long)(now.tv_nsec - t0->tv_nsec) / 1000000L;
}

/* 读回 ADC path volume (dB)。采集成功后调用才安全 —— 那时 audcodec 的
 * 时钟/电源一定开着 (start_capture 里 HAL_PMU_EnableAudio + RCC 都开了),
 * 读寄存器才是安全的。 */
static bool mic_check_read_gain(int *db, uint32_t *raw)
{
#ifdef MIC_CHECK_HAVE_ADC_CFG
    uint32_t v;
    uint32_t rough;
    uint32_t fine;

    v = *(volatile uint32_t *)(uintptr_t)MIC_CHECK_ADC_CFG_ADDR;
    rough = (v >> 8) & 0xFU;
    fine  = (v >> 12) & 0xFU;

    /* HAL 的编码: rough = (vol+60)/6, fine = ((vol+60)%6) << 1, 定义域
     * -60..30。反解回 dB 后与 Config_ADCPath_Volume 的实参一致。 */
    if (db) {
        *db = (int)(rough * 6U + (fine >> 1)) - 60;
    }
    if (raw) {
        *raw = v & MIC_CHECK_ADC_CFG_MSK;
    }
    return true;
#else
    if (db) {
        *db = 0;
    }
    if (raw) {
        *raw = 0;
    }
    return false;
#endif
}

static int mic_check_enqueue(int afd, struct ap_buffer_s *apb)
{
    struct audio_buf_desc_s desc;

    apb->nbytes   = apb->nmaxbytes;
    desc.numbytes = apb->nbytes;
    desc.u.buffer = apb;

    return ioctl(afd, AUDIOIOC_ENQUEUEBUFFER, (uintptr_t)&desc);
}

/* 打开采集设备。设备节点要先注册 (audio_setup 做的事); 这里先试 open(),
 * 拿到 -ENOENT 说明没人注册过, 就自己补注册一次 —— 这样自检不依赖开机
 * 顺序, 也不依赖串口能不能敲 audio_setup。 */
static int mic_check_open(bool *registered)
{
    int fd;

    if (registered) {
        *registered = false;
    }

    fd = open(MIC_CHECK_DEVPATH, O_RDWR);
    if (fd >= 0) {
        return fd;
    }

    if (errno != ENOENT) {
        /* errno == 0 would make -errno == 0, and the caller reads fd 0 as a
         * valid descriptor and then ioctl()s on stdin. NuttX sets errno on a
         * failed open(), but this costs nothing and removes the trap. */
        int err = errno ? errno : EIO;

        syslog(LOG_ERR, "[mic_check] open %s failed: %d\n",
               MIC_CHECK_DEVPATH, err);
        return -err;
    }

#ifdef CONFIG_EXAMPLES_AUDIO_SETUP
    /* 弱引用: 若这个镜像里没有链接采集驱动 (例如别的板子/别的 target),
     * 符号为 NULL, 这里如实报错而不是链接失败。 */
    extern int sf32lb52_audcodec_register(void) __attribute__((weak));

    if (sf32lb52_audcodec_register == NULL) {
        syslog(LOG_ERR,
               "[mic_check] %s missing and sf32lb52_audcodec_register() "
               "not linked\n", MIC_CHECK_DEVPATH);
        return -ENODEV;
    }

    {
        int rc = sf32lb52_audcodec_register();

        syslog(LOG_INFO,
               "[mic_check] %s was not registered, sf32lb52_audcodec_register"
               "() -> %d\n", MIC_CHECK_DEVPATH, rc);
        if (registered) {
            *registered = true;
        }
    }

    fd = open(MIC_CHECK_DEVPATH, O_RDWR);
    if (fd < 0) {
        int err = errno ? errno : EIO;

        syslog(LOG_ERR, "[mic_check] open %s after register failed: %d\n",
               MIC_CHECK_DEVPATH, err);
        return -err;
    }
    return fd;
#else
    syslog(LOG_ERR,
           "[mic_check] %s missing and CONFIG_EXAMPLES_AUDIO_SETUP is off "
           "(no driver to register)\n", MIC_CHECK_DEVPATH);
    return -ENODEV;
#endif
}

/* ── the capture run ─────────────────────────────────────────── */

static int mic_check_capture(mic_selfcheck_result_t *r, bool dump_pcm)
{
    int afd = -1;
    int ofd = -1;
    mqd_t mq = (mqd_t)-1;
    struct audio_caps_desc_s cap_desc;
    struct ap_buffer_info_s binfo;
    struct mq_attr attr;
    struct ap_buffer_s *bufs[MIC_SELFCHECK_NBUFS];
    int nbufs = 0;
    long target = MIC_CHECK_TARGET;
    long total = 0;
    long deadline_ms = MIC_SELFCHECK_SECONDS * 1000L + MIC_CHECK_SLACK_MS;
    struct timespec t0;
    uint64_t *seen = NULL;      /* 取值位图: 65536 bit = 8KB */
    uint32_t samples = 0;
    uint32_t zero_count = 0;
    uint32_t neg1_count = 0;
    uint32_t run = 0;
    uint32_t longest = 0;
    uint32_t distinct = 0;
    int32_t peak = 0;
    double sum_sq = 0.0;
    int i;

    /* 逐秒 RMS: 每秒一个桶。bucket 用 samples 计数而不是墙钟 —— 采样率固定
     * 16kHz, 样本数就是时间, 且不受调度抖动影响。 */
    uint32_t seg_idx = 0;
    uint32_t seg_samples = 0;
    double   seg_sum_sq = 0.0;
    uint32_t seg_samples_per = (uint32_t)MIC_CHECK_RATE;   /* 1 秒 */
    double   seg_max = 0.0;
    double   seg_min = 1e9;
    uint32_t seg_filled = 0;

    /* 本函数可能被反复调用 (每次 ask / 每次开机), 所有状态都必须是本次
     * 局部的 —— 静态/全局变量在 NuttX 平铺构建下不会自动清零。 */
    memset(bufs, 0, sizeof(bufs));
    r->capture_ok = false;
    r->capture_err = 0;

    seen = (uint64_t *)calloc(65536 / 64, sizeof(uint64_t));
    if (seen == NULL) {
        snprintf(r->detail, sizeof(r->detail), "out of memory");
        return ERROR;
    }

    afd = mic_check_open(&r->registered_dev);
    if (afd < 0) {
        r->capture_err = afd;
        snprintf(r->detail, sizeof(r->detail),
                 "open %s failed (%d)", MIC_CHECK_DEVPATH, afd);
        free(seen);
        return ERROR;
    }

    /* 1) RESERVE */
    if (ioctl(afd, AUDIOIOC_RESERVE) < 0) {
        r->capture_err = -errno;
        snprintf(r->detail, sizeof(r->detail), "RESERVE failed (%d)", errno);
        goto out;
    }

    /* 2) CONFIGURE 16kHz/16bit/mono (字段位置与 nxrecorder 一致) */
    memset(&cap_desc, 0, sizeof(cap_desc));
    cap_desc.caps.ac_len            = sizeof(cap_desc.caps);
    cap_desc.caps.ac_type           = AUDIO_TYPE_INPUT;
    cap_desc.caps.ac_channels       = MIC_CHECK_CHANNELS;
    cap_desc.caps.ac_controls.hw[0] = MIC_CHECK_RATE;
    cap_desc.caps.ac_controls.b[2]  = MIC_CHECK_BITS;
    cap_desc.caps.ac_subtype        = AUDIO_FMT_PCM;
    if (ioctl(afd, AUDIOIOC_CONFIGURE, (uintptr_t)&cap_desc) < 0) {
        r->capture_err = -errno;
        snprintf(r->detail, sizeof(r->detail), "CONFIGURE failed (%d)", errno);
        goto out;
    }

    /* 3) mqueue: O_RDWR —— 内核 file_mq_send 要往这里写 */
    attr.mq_maxmsg  = MIC_CHECK_MQ_DEPTH;
    attr.mq_msgsize = MIC_CHECK_MQ_MSGS;
    attr.mq_flags   = 0;
    mq = mq_open(MIC_CHECK_MQNAME, O_CREAT | O_RDWR, 0644, &attr);
    if (mq == (mqd_t)-1) {
        r->capture_err = -errno;
        snprintf(r->detail, sizeof(r->detail), "mq_open failed (%d)", errno);
        goto out;
    }
    if (ioctl(afd, AUDIOIOC_REGISTERMQ, (uintptr_t)mq) < 0) {
        r->capture_err = -errno;
        snprintf(r->detail, sizeof(r->detail), "REGISTERMQ failed (%d)", errno);
        goto out;
    }

    /* 4) 缓冲几何: 驱动有默认就听驱动的 */
    memset(&binfo, 0, sizeof(binfo));
    if (ioctl(afd, AUDIOIOC_GETBUFFERINFO, (uintptr_t)&binfo) != OK) {
        binfo.buffer_size = MIC_SELFCHECK_BUFBYTES;
        binfo.nbuffers    = MIC_SELFCHECK_NBUFS;
    }
    if (binfo.buffer_size <= 0) {
        binfo.buffer_size = MIC_SELFCHECK_BUFBYTES;
    }
    if (binfo.nbuffers <= 0 || binfo.nbuffers > MIC_SELFCHECK_NBUFS) {
        binfo.nbuffers = MIC_SELFCHECK_NBUFS;
    }

    /* 5) ALLOCBUFFER xN + 清零 + 入队 */
    for (i = 0; i < (int)binfo.nbuffers; i++) {
        struct audio_buf_desc_s desc;

        memset(&desc, 0, sizeof(desc));
        desc.numbytes  = binfo.buffer_size;
        desc.u.pbuffer = &bufs[i];
        if (ioctl(afd, AUDIOIOC_ALLOCBUFFER, (uintptr_t)&desc) !=
            sizeof(desc)) {
            r->capture_err = -EIO;
            snprintf(r->detail, sizeof(r->detail),
                     "ALLOCBUFFER %d failed", i);
            goto out;
        }

        if (bufs[i] == NULL) {
            /* 参考实现没有这一步 (它直接 enqueue), 但 apb 为空时 enqueue
             * 会解引用空指针 —— 这里如实报错而不是崩在驱动里。 */
            r->capture_err = -ENOMEM;
            snprintf(r->detail, sizeof(r->detail),
                     "ALLOCBUFFER %d returned NULL", i);
            goto out;
        }

        memset(bufs[i]->samp, 0, bufs[i]->nmaxbytes);
        bufs[i]->nbytes  = 0;
        bufs[i]->curbyte = 0;
        nbufs++;

        if (mic_check_enqueue(afd, bufs[i]) < 0) {
            r->capture_err = -errno;
            snprintf(r->detail, sizeof(r->detail),
                     "ENQUEUEBUFFER %d failed (%d)", i, errno);
            goto out;
        }
    }

    /* 6) 可选落盘: 给离线分析 (pcm-audio skill / audio_analyzer.py) 留素材 */
    if (dump_pcm) {
        snprintf(r->pcm_path, sizeof(r->pcm_path), "%s/mic_selfcheck.pcm",
                 AGENT_DATA_DIR);
        ofd = open(r->pcm_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (ofd < 0) {
            r->pcm_path[0] = '\0';
        }
    }

    /* 7) START */
    if (ioctl(afd, AUDIOIOC_START) < 0) {
        r->capture_err = -errno;
        snprintf(r->detail, sizeof(r->detail), "START failed (%d)", errno);
        goto out;
    }

    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* 8) 收数据 + 统计, 直到收满 target 或撞上墙钟上限 */
    while (total < target) {
        struct audio_msg_s msg;
        struct timespec ts;
        ssize_t n;

        if (mic_check_ms_since(&t0) >= deadline_ms) {
            if (total == 0) {
                r->capture_err = -ETIMEDOUT;
                snprintf(r->detail, sizeof(r->detail),
                         "no data in %ldms", (long)deadline_ms);
            }
            break;
        }

        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_nsec += (long)MIC_CHECK_RECV_MS * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec  += ts.tv_nsec / 1000000000L;
            ts.tv_nsec %= 1000000000L;
        }

        n = mq_timedreceive(mq, (char *)&msg, MIC_CHECK_MQ_MSGS, NULL, &ts);
        if (n < 0) {
            if (errno == ETIMEDOUT || errno == EINTR) {
                continue;
            }
            if (total == 0) {
                r->capture_err = -errno;
                snprintf(r->detail, sizeof(r->detail),
                         "mq_receive err %d", errno);
            }
            break;
        }

        if (msg.msg_id != AUDIO_MSG_DEQUEUE || msg.u.ptr == NULL) {
            continue;
        }

        {
            struct ap_buffer_s *apb = msg.u.ptr;
            int avail = (int)apb->curbyte;   /* 驱动填了多少字节 */
            const int16_t *d = (const int16_t *)apb->samp;
            int cnt;
            int k;

            if (avail > 0 && avail <= (int)apb->nmaxbytes) {
                if (ofd >= 0) {
                    (void)write(ofd, apb->samp, avail);
                }
                total += avail;
                cnt = avail / 2;
                for (k = 0; k < cnt; k++) {
                    int16_t v = d[k];
                    int32_t a = (v < 0) ? -(int32_t)v : (int32_t)v;

                    if (a > peak) {
                        peak = a;
                    }
                    sum_sq += (double)v * (double)v;
                    seg_sum_sq += (double)v * (double)v;
                    samples++;
                    seg_samples++;

                    /* 收满一秒就封一段 */
                    if (seg_samples >= seg_samples_per) {
                        double sr = sqrt(seg_sum_sq / (double)seg_samples);

                        if (seg_idx < MIC_SELFCHECK_SEGS) {
                            r->seg_rms[seg_idx] = sr;
                            seg_idx++;
                        }
                        if (sr > seg_max) seg_max = sr;
                        if (sr < seg_min) seg_min = sr;
                        seg_filled++;
                        seg_samples = 0;
                        seg_sum_sq = 0.0;
                    }

                    if (v == 0) {
                        zero_count++;
                    }
                    if (v == -1) {
                        neg1_count++;
                    }

                    if (v == 0 || v == -1) {
                        run++;
                        if (run > longest) {
                            longest = run;
                        }
                    } else {
                        run = 0;
                    }

                    /* 取值位图 (65536 bit) */
                    {
                        uint16_t uv = (uint16_t)v;
                        uint64_t bit = (uint64_t)1 << (uv & 63U);

                        if ((seen[uv >> 6] & bit) == 0) {
                            seen[uv >> 6] |= bit;
                            distinct++;
                        }
                    }
                }
            }

            /* 用完标记并还回去 (与参考实现一致) */
            apb->nbytes = apb->curbyte;
            if (mic_check_enqueue(afd, apb) < 0) {
                break;
            }
        }
    }

    /* 9) 增益档: 采集成功后读回, 此时 audcodec 一定带电 */
    r->gain_known = mic_check_read_gain(&r->adc_gain_db, &r->adc_cfg_raw);

    (void)ioctl(afd, AUDIOIOC_STOP, 0);

    /* 收尾: 不足一秒的尾巴也算一段, 否则短采集就没有分段可看 */
    if (seg_samples > 0) {
        double sr = sqrt(seg_sum_sq / (double)seg_samples);

        if (seg_idx < MIC_SELFCHECK_SEGS) {
            r->seg_rms[seg_idx] = sr;
            seg_idx++;
        }
        if (sr > seg_max) seg_max = sr;
        if (sr < seg_min) seg_min = sr;
        seg_filled++;
    }
    r->seg_count = seg_idx;

    r->samples     = samples;
    r->bytes       = (uint32_t)total;
    r->peak        = peak;
    r->distinct    = distinct;
    r->zero_count  = zero_count;
    r->neg1_count  = neg1_count;
    r->longest_run = longest;
    r->rms = (samples > 0) ? sqrt(sum_sq / (double)samples) : 0.0;

    if (samples > 0) {
        r->capture_ok = true;
        r->capture_err = 0;

        /* 判据分两半, 满足任一即算"麦有响应":
         *   a) 整段 RMS 明显高于静音底噪 (麦在把声音送进 ADC); 或
         *   b) 逐秒 RMS 之间出现明显落差 (seg_max >= 2x seg_min 且 seg_max
         *      本身高于门限) —— 这正是"敲一下桌子某一段跳起来"的形态,
         *      即使整段平均值被安静的两秒拉低也不会漏判。
         * 只有两者都不成立才报 NO-RESPONSE。 */
        bool loud = (r->rms >= MIC_SELFCHECK_RMS_LIVE);
        bool varies = (seg_filled >= 2) &&
                      (seg_max >= MIC_SELFCHECK_RMS_LIVE) &&
                      (seg_max >= 2.0 * seg_min);

        /* 先过"ADC 真在转换"这一关: 取值太少的信号是常数/直流偏移,
         * 不是麦克风的响应, 无论 RMS 多大都不算 OK。 */
        bool converting = (r->distinct >= MIC_SELFCHECK_MIN_DISTINCT);

        r->mic_ok = converting && (loud || varies);
    } else if (r->capture_err == 0) {
        r->capture_err = -ENODATA;
        snprintf(r->detail, sizeof(r->detail), "no samples captured");
    }

out:
    if (ofd >= 0) {
        close(ofd);
    }

    for (i = 0; i < nbufs && i < MIC_SELFCHECK_NBUFS; i++) {
        if (bufs[i] != NULL) {
            struct audio_buf_desc_s desc;

            desc.u.buffer = bufs[i];
            (void)ioctl(afd, AUDIOIOC_FREEBUFFER, (uintptr_t)&desc);
            bufs[i] = NULL;
        }
    }

    if (mq != (mqd_t)-1) {
        (void)ioctl(afd, AUDIOIOC_UNREGISTERMQ);
        (void)mq_close(mq);
        (void)mq_unlink(MIC_CHECK_MQNAME);
    }

    if (afd >= 0) {
        (void)ioctl(afd, AUDIOIOC_RELEASE);
        close(afd);
    }

    free(seen);
    return r->capture_ok ? OK : ERROR;
}

/* ── public API ──────────────────────────────────────────────── */

int mic_selfcheck_run(mic_selfcheck_result_t *out, bool dump_pcm)
{
    int ret;

    if (out == NULL) {
        return ERROR;
    }

    memset(out, 0, sizeof(*out));

    /* 串行化: 音频设备同一时刻只能有一个会话, 并发自检会把彼此的 apb
     * 和 mqueue 搅在一起。 */
    pthread_mutex_lock(&s_run_lock);
    ret = mic_check_capture(out, dump_pcm);
    pthread_mutex_unlock(&s_run_lock);

    return ret;
}

int mic_selfcheck_report(const mic_selfcheck_result_t *r, char *text,
                         size_t text_size)
{
    int n;

    if (r == NULL || text == NULL || text_size == 0) {
        return 0;
    }

    if (!r->capture_ok) {
        n = snprintf(text, text_size, "麦克风自检失败：%s",
                     r->detail[0] ? r->detail : "capture error");
        return n;
    }

    /* "麦克风自检：RMS 12.9（每秒 12.8/13.0/12.9），峰值 45，取值 104，
     *  增益 +30dB，结论：麦克风无响应。"
     *
     * 逐秒 RMS 是判读的关键: 若只有某一秒明显偏高, 说明麦确实响应了那一下
     * (例如敲桌), 只是另外两秒很安静。 */
    {
        char segs[64];
        int off = 0;

        segs[0] = '\0';
        for (uint32_t i = 0; i < r->seg_count && i < 3; i++) {
            off += snprintf(segs + off, sizeof(segs) - (size_t)off,
                            "%s%.1f", i ? "/" : "", r->seg_rms[i]);
            if ((size_t)off >= sizeof(segs)) {
                break;
            }
        }

        n = snprintf(text, text_size,
                     "麦克风自检：RMS %.1f（每秒 %s），峰值 %d，取值 %u，"
                     "%s%d dB，结论：%s",
                     r->rms, segs, (int)r->peak, (unsigned)r->distinct,
                     r->gain_known ? "增益 " : "",
                     r->gain_known ? r->adc_gain_db : 0,
                     r->mic_ok ? "麦克风正常" : "麦克风无响应");
    }
    return n;
}

int mic_selfcheck_run_json(char *out, size_t out_size)
{
    mic_selfcheck_result_t r;
    char report[192];
    int rc;

    if (out == NULL || out_size == 0) {
        return ERROR;
    }

    rc = mic_selfcheck_run(&r, true);
    mic_selfcheck_report(&r, report, sizeof(report));

    /* 手拼 JSON: 字段都是定长数字/受控文本, 没有需要转义的引号,
     * 比拉一份 cJSON 出来更省事 (report 里只有中文和数字)。 */
    {
        char segs[80];
        int off = 0;

        segs[0] = '\0';
        for (uint32_t i = 0; i < r.seg_count && i < 4; i++) {
            off += snprintf(segs + off, sizeof(segs) - (size_t)off,
                            "%s%.2f", i ? "," : "", r.seg_rms[i]);
            if ((size_t)off >= sizeof(segs)) {
                break;
            }
        }

        snprintf(
            out, out_size,
            "{\"verdict\":\"%s\",\"report\":\"%s\",\"capture_ok\":%s,"
            "\"rms\":%.1f,\"seg_rms\":[%s],\"peak\":%d,\"distinct\":%u,"
            "\"zero_pct\":%.2f,\"neg1_pct\":%.2f,\"longest_run\":%u,"
            "\"samples\":%u,\"adc_gain_db\":%d,\"adc_cfg\":%u,"
            "\"gain_known\":%s,\"registered\":%s,\"detail\":\"%s\","
            "\"pcm\":\"%s\"}",
            r.mic_ok ? "mic OK" : "mic NO-RESPONSE",
            report,
            r.capture_ok ? "true" : "false",
            r.rms, segs, (int)r.peak, (unsigned)r.distinct,
            r.samples ? 100.0 * (double)r.zero_count / (double)r.samples : 0.0,
            r.samples ? 100.0 * (double)r.neg1_count / (double)r.samples : 0.0,
            (unsigned)r.longest_run, (unsigned)r.samples,
            r.gain_known ? r.adc_gain_db : 0,
            (unsigned)r.adc_cfg_raw,
            r.gain_known ? "true" : "false",
            r.registered_dev ? "true" : "false",
            r.detail,
            r.pcm_path);
    }

    /* 采集失败也要把 JSON 交出去 (工具层要能把失败原因说清楚), 但返回值
     * 如实反映成败, 便于调用方判断。 */
    return rc;
}

/* ── boot probe ──────────────────────────────────────────────── */

#ifndef MIC_SELFCHECK_BOOT
#define MIC_SELFCHECK_BOOT 1
#endif

#if MIC_SELFCHECK_BOOT

static void *mic_check_boot_task(void *arg)
{
    mic_selfcheck_result_t r;
    char report[192];

    (void)arg;

    if (mic_selfcheck_run(&r, true) != OK && !r.capture_ok) {
        mic_selfcheck_report(&r, report, sizeof(report));
        syslog(LOG_WARNING, "[mic_check] boot probe: %s\n", report);
        printf("[mic_check] %s\n", report);
        return NULL;
    }

    mic_selfcheck_report(&r, report, sizeof(report));

    /* 每次开机都留下这一行结论, 即使没人 ask。 */
    syslog(LOG_INFO,
           "[mic_check] boot probe: %s (rms=%.1f segs=%.1f/%.1f/%.1f peak=%d "
           "distinct=%u zero=%.2f%% neg1=%.2f%% run=%u gain=%d dB cfg=0x%04x)\n",
           report, r.rms,
           r.seg_count > 0 ? r.seg_rms[0] : 0.0,
           r.seg_count > 1 ? r.seg_rms[1] : 0.0,
           r.seg_count > 2 ? r.seg_rms[2] : 0.0,
           (int)r.peak, (unsigned)r.distinct,
           r.samples ? 100.0 * (double)r.zero_count / (double)r.samples : 0.0,
           r.samples ? 100.0 * (double)r.neg1_count / (double)r.samples : 0.0,
           (unsigned)r.longest_run,
           r.gain_known ? r.adc_gain_db : 0, (unsigned)r.adc_cfg_raw);
    printf("[mic_check] %s\n", report);

    return NULL;
}

int mic_selfcheck_start_async(void)
{
    /* trylock: 开机探针撞上用户手动自检时直接让路, 不排队 —— 开机路径
     * 不该被 3 秒采集堵住。 */
    if (pthread_mutex_trylock(&s_run_lock) != 0) {
        syslog(LOG_INFO, "[mic_check] boot probe skipped, run in progress\n");
        return -EBUSY;
    }
    pthread_mutex_unlock(&s_run_lock);

    if (agent_task_create(mic_check_boot_task, "mic_check", 12288, NULL, 60)
        != OK) {
        syslog(LOG_WARNING, "[mic_check] boot probe thread create failed\n");
        return ERROR;
    }
    return OK;
}

#else  /* !MIC_SELFCHECK_BOOT */

int mic_selfcheck_start_async(void)
{
    return OK;
}

#endif /* MIC_SELFCHECK_BOOT */

#else  /* !CONFIG_AUDIO */

/* 没有音频子系统时保留符号, 让上层不用到处加 #ifdef, 但如实报不可用。 */

int mic_selfcheck_run(mic_selfcheck_result_t *out, bool dump_pcm)
{
    (void)dump_pcm;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->capture_err = -ENOTSUP;
        snprintf(out->detail, sizeof(out->detail), "CONFIG_AUDIO is off");
    }
    return ERROR;
}

int mic_selfcheck_report(const mic_selfcheck_result_t *r, char *text,
                         size_t text_size)
{
    (void)r;
    if (text == NULL || text_size == 0) {
        return 0;
    }
    return snprintf(text, text_size, "mic selfcheck unavailable "
                    "(CONFIG_AUDIO off)");
}

int mic_selfcheck_run_json(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ERROR;
    }
    snprintf(out, out_size,
             "{\"verdict\":\"unavailable\",\"report\":\"mic selfcheck "
             "unavailable\",\"capture_ok\":false}");
    return ERROR;
}

int mic_selfcheck_start_async(void)
{
    return OK;
}

#endif /* CONFIG_AUDIO */
