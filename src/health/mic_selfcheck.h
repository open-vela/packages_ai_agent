/****************************************************************************
 * mic_selfcheck.h - On-demand microphone self test.
 *
 * 为什么需要它: /dev/audio/pcm0c 的采集链路 (AUDCODEC ADC1 -> DMAC1_CH4 ->
 * NuttX audio upper-half) 已经被证明能出数据, 但"能出数据"不等于"麦对声音
 * 有响应" —— 真机上说话/敲桌/放 1kHz 三种激励的 PCM 统计完全一样
 * (RMS 12.9 LSB, peak 40~65), 说明模拟前端没有把声音送进 ADC。
 *
 * 而 audio_setup / audio_rec_test 是 nsh 命令, 开机 20~30 秒后控制台被 agent
 * CLI (vela>) 占用, 串口再也敲不进 nsh 命令。所以把诊断做进 agent 自己:
 *   - 工具 mic_selfcheck: "ask 麦克风自检" 即可触发 (走 NL 快通道, 不依赖云端);
 *   - 开机后台跑一次, 每次开机都有一行结论。
 *
 * 采集序列照抄 contest 仓 audio_rec_test_main.c (已真机验证的正确用法):
 *   open(O_RDWR) -> RESERVE -> CONFIGURE(16k/16bit/mono) -> mq_open(O_RDWR)
 *   -> REGISTERMQ -> GETBUFFERINFO -> ALLOCBUFFER xN -> ENQUEUEBUFFER xN
 *   -> START -> mq_receive(AUDIO_MSG_DEQUEUE) -> 统计 -> re-enqueue
 *
 * 与参考实现的两处**有意**差异 (都是为了不再挂死):
 *   1) 参考实现的退出条件是 total < target && idle_loops < RATE, 每次
 *      mq_timedreceive 超时是 2s —— 驱动一旦不投递, 循环要跑 16000 x 2s。
 *      这里改用**墙钟硬上限** (录制秒数 + 2s 余量), 到点无条件退出。
 *   2) 统计在收数据的同时完成, 不落盘也能出结论; PCM 落盘是可选项。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef AI_AGENT_MIC_SELFCHECK_H
#define AI_AGENT_MIC_SELFCHECK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 录制时长 (秒)。3 秒足够覆盖 "MICBIAS 稳定 + 一句短话/一次敲桌"。 */
#define MIC_SELFCHECK_SECONDS   3

/* 判定为"麦有响应"的 RMS 门限 (LSB, 16bit 满量程 32768)。
 *
 * 依据是真机基线: +12dB 档下, 说话/敲桌/播放 1kHz 三种激励的 RMS 都是
 * 12.9 LSB (peak 40~65), 即"无响应"的底噪水平。板上当前跑的是 +30dB 实验
 * 档 (+18dB 约 8 倍), 若模拟前端是活的, RMS 应升到 ~100 LSB 量级; 若前端
 * 悬空/麦没接通, RMS 仍停在 ~13 LSB。20 LSB 把这两种情况分开, 且明显高于
 * 观测到的 12.9 基线。这是一个**判据**, 不是物理常量 —— 报告里会把实测值
 * 和门限一起打出来, 便于人工复核。 */
#define MIC_SELFCHECK_RMS_LIVE  20.0

/* 判定"ADC 真的在转换"的最少不同取值个数。
 *
 * 这条专门防"成片常数": 历史上驱动出过 0x0000 / 0xFFFF 交替的 PCM
 * (缺参考电压, 见 sf32lb52_audcodec.c:703-713 的注释), 那种信号只有 1~2 个
 * 不同取值, 却可能因为常数偏移而 RMS 很高 —— 只看 RMS 会误判成 mic OK。
 *
 * 取 8 而不是更大的值: 一个**纯** 1kHz 正弦在 16kHz 采样下每周期只有 16 个
 * 样本 (即 ~16 个不同取值), 门限设高了会把"正在响的纯音"也判成常数。
 * 8 明显高于任何卡死的常数 (1~4 个取值), 又明显低于真机正常采集的
 * 98~117 个取值。恒定直流偏移 (无论多大) 都会因为这条被判 NO-RESPONSE。 */
#define MIC_SELFCHECK_MIN_DISTINCT  8

/* 采集缓冲: 8 x 4096B = 32KB, 约 1s @16kHz/16bit/mono 的缓冲深度 */
#define MIC_SELFCHECK_BUFBYTES  4096
#define MIC_SELFCHECK_NBUFS     8

/* 逐秒 RMS 的段数 (= 录制秒数, 多留一段给越界的那部分样本) */
#define MIC_SELFCHECK_SEGS      (MIC_SELFCHECK_SECONDS + 1)

/* 单次自检结果 */
typedef struct {
    bool     capture_ok;     /* 采集序列是否跑完 (START 成功且收到过数据) */
    int      capture_err;    /* !capture_ok 时的负 errno */
    bool     registered_dev; /* 是否由本模块补做了设备注册 (open 得到 -ENOENT) */

    uint32_t samples;        /* 参与统计的样本数 */
    uint32_t bytes;          /* 参与统计的字节数 */
    double   rms;            /* 均方根, 单位 LSB */
    int32_t  peak;           /* 绝对值峰值, 单位 LSB */
    uint32_t distinct;       /* 不同取值个数 (满量程 65536) */
    uint32_t zero_count;     /* 精确 0 的样本数 */
    uint32_t neg1_count;     /* 精确 -1 的样本数 */
    uint32_t longest_run;    /* 最长连续 {0,-1} 段长度 (样本数) */

    /* 逐秒 RMS。单看整段 RMS 无法区分"麦不响"和"这 3 秒恰好很安静",
     * 而"随声音变化"正是判据的另一半: 敲一下桌子, 某一段的 RMS 会跳起来。
     * seg_count 是实际填了几段 (<= MIC_SELFCHECK_SEGS)。 */
    uint32_t seg_count;
    double   seg_rms[MIC_SELFCHECK_SEGS];

    bool     gain_known;     /* ADC 增益档是否读回成功 */
    int      adc_gain_db;    /* 读回的 ADC path volume, 单位 dB */
    uint32_t adc_cfg_raw;    /* AUDCODEC_ADC_CH0_CFG 原始值, 便于人工核对 */

    bool     mic_ok;         /* 最终结论: true = mic OK */
    char     pcm_path[64];   /* 落盘路径, 空串表示没落盘 */
    char     detail[96];     /* 失败/异常的一句话说明 */
} mic_selfcheck_result_t;

/**
 * 跑一次完整自检 (阻塞, 约 3~5 秒)。
 *
 * 单飞: 内部持一把全局锁, 并发调用会排队而不是互相踩音频设备。
 *
 * @param out       结果输出, 不可为 NULL
 * @param dump_pcm  true 时把 PCM 写到 AGENT_DATA_DIR/mic_selfcheck.pcm
 * @return OK  采集序列完成 (结果里仍可能是 mic NO-RESPONSE)
 *         ERROR 采集失败 (out->capture_err / out->detail 给出原因)
 */
int mic_selfcheck_run(mic_selfcheck_result_t *out, bool dump_pcm);

/** 把结果拼成一句可直接上屏/播报的中文结论。返回写入长度。 */
int mic_selfcheck_report(const mic_selfcheck_result_t *r, char *text,
                         size_t text_size);

/**
 * 跑一次自检并输出 JSON (工具层用)。
 * JSON 字段: verdict / report / capture_ok / rms / peak / distinct /
 *            zero_pct / neg1_pct / longest_run / samples / adc_gain_db /
 *            adc_cfg / gain_known / registered / detail / pcm
 */
int mic_selfcheck_run_json(char *out, size_t out_size);

/**
 * 后台跑一次自检并把结论打日志 (开机探针用)。立即返回。
 * 已有自检在跑时直接跳过 (不排队), 返回 -EBUSY。
 */
int mic_selfcheck_start_async(void);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_MIC_SELFCHECK_H */
