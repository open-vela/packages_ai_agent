/****************************************************************************
 * hr_measure.h - One-shot heart-rate measurement (P1b).
 *
 * 桌宠页「测心率」按钮背后的单次测量服务：
 *   点按钮 -> hr_measure_start() 起一个独立 worker 线程，立刻返回；
 *   worker 用既有 CLI 路径 task_spawn("hr_sensor", {"start"/"stop"}) 起停采集，
 *   自己 poll /dev/uorb/algo_heartrate0 并缓存最后一次成功 copy 的值，
 *   结束时经 lvgl_ui_channel_send_kind() 一次完成上屏 + 表情 + 语音播报。
 *
 * 调用约束：任何线程都可以调 hr_measure_start()，函数内部只做线程创建，
 * 不碰 LVGL、不阻塞调用者（LVGL 是 LV_OS_NONE，绝不能在 render 线程上同步跑 8~15s）。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef AI_AGENT_HR_MEASURE_H
#define AI_AGENT_HR_MEASURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <nuttx/config.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 硬上限：到点无论有没有结果都停采集并报失败。
 * 2026-10-13: 15 -> 30 —— 真机实测算法从 start 到第一次 gate=ok 需要约 35s
 * (滤波器/DC 建立 + env 包络从启动瞬态衰减 + 3 个 RR + 连续确认),
 * 15s 窗口下「测心率」按钮**必然**以 timeout 收场(真机 22:41 已复现:
 * 倒计时走完没有数字, 日志 [hr_measure] failed: timeout, no data)。 */
#define HR_MEASURE_MAX_S        30
/* 已经出过数据之后，连续多久没有新更新就提前放弃 */
#define HR_MEASURE_STALL_MS     3000
/* 结果上屏后多久恢复时段问候语 */
#define HR_MEASURE_GREET_MS     5000

/**
 * 启动一次单次测量（用户按钮路径，会上「测量中」进度、结果上屏
 * 并进 TTS 播报）。
 *
 * 单飞：测量进行中再调用直接忽略。
 *
 * @return 0        已启动（worker 线程已创建）
 *         -EBUSY   测量进行中，本次点击忽略
 *         -ENOMEM  线程创建失败
 */
int hr_measure_start(void);

/**
 * 静默版单次测量（后台守护 hr_watch 用）。
 *
 * 与 hr_measure_start() 共用同一套 worker/采集/质量判据/收尾，
 * 仅跳过全部 UI 与语音：不上「测量中」进度、结果不上屏、不进 TTS、
 * 失败也不弹错误文案 —— 后台周期测量不能打扰用户。串口探测日志
 * 保留，排障仍可复盘。
 *
 * 单飞语义同上（与按钮路径共享同一个 busy 标志，任一在跑都 -EBUSY）。
 *
 * @return 0        已启动
 *         -EBUSY   测量进行中（可能是用户自己点的按钮）
 *         -ENOMEM  线程创建失败
 */
int hr_measure_start_silent(void);

/**
 * 最近一轮测量**有没有测到**（成功 = 出过被接受的 median）。
 *
 * 给后台守护用：失败轮（信号弱/超时）里 hr_measure_last_bpm() 可能
 * 残留最后一次 copy 到的非零值，只看 bpm 会把失败轮当成功轮。
 * 读结果的一方应**先看本函数、再看数值** —— bpm==0 / last_ok==false
 * 都绝不告警、绝不显示 0（诚实性红线）。
 */
bool hr_measure_last_ok(void);

/**
 * 最近一次成功测到的心率值（bpm）。
 *
 * 给 UI 用：心率页每秒刷新时读它。返回 0 表示还没测到过。
 * 任何线程可调，无锁（读一个 volatile uint32_t）。
 */
int hr_measure_last_bpm(void);

/**
 * 是否正在测量中。
 *
 * 给 UI 用：按钮据此显示"测量中…"还是"重新测量"。
 */
bool hr_measure_is_running(void);

/****************************************************************************
 * 心率历史（持久化环形缓冲）
 *
 * 288 条 ≈ 后台守护 hr_watch 每 10 分钟一轮的两天数据。每次测量成功
 * （手动 / 静默两条路径都经过 hr_report_ok）追加一条并立即落盘到
 * /data/ai_agent/health/hr_history.json：
 *
 *   {"items":[{"t":1730000000,"bpm":88},{"t":...}]}
 *
 * 失败语义全部是「从空开始 / 不崩」：
 *  - 文件不存在、读失败、JSON 损坏、字段非法：跳过该条或整体当空；
 *  - 保存失败只告警：RAM 环照常可用并继续累积，下一次成功保存会把
 *    整段历史补写进去，不会因为一次写盘失败丢掉数据；
 *  - 首次 start（worker 线程）或首次查询时从文件 load 一次，之后的
 *    snapshot / 追加都只碰内存。
 *
 * 所有函数任意线程可调，内部互斥。
 ****************************************************************************/

/** 历史容量（条）。 */
#define HR_MEASURE_HISTORY_CAP      288

/** 完整历史 JSON 的最坏长度上限（288 条 * ~27B + 外壳 + 余量）。
 *  用 hr_measure_history_json() 的一方按它分配输出缓冲区。 */
#define HR_MEASURE_HISTORY_JSON_MAX 12288

/**
 * 取最近的心率历史快照（给心率页预加载曲线用）。
 *
 * @param ts   输出时间戳（time(NULL) 截断成 uint32_t），旧 -> 新
 * @param bpm  输出 bpm 数组
 * @param max  最多取多少条
 * @return 实际条数（0..max）；ts/bpm 为空返回 -EINVAL，max<=0 返回 0。
 *
 * 历史不足 max 条时返回全部；超过时取**最近** max 条，仍按旧 -> 新
 * 排列。首次调用可能触发一次文件 load。
 */
int hr_measure_history_snapshot(uint32_t *ts, int *bpm, int max);

/**
 * 把完整历史输出成 JSON（最旧 -> 最新，最多 HR_MEASURE_HISTORY_CAP 条）：
 *
 *   {"items":[{"t":...,"bpm":...},...]}
 *
 * 文件不存在 / 历史为空时输出 {"items":[]}，仍算成功 —— 「没有历史」
 * 由调用方决定怎么表达，不是本函数的错误。
 *
 * @param out  输出缓冲区
 * @param cap  缓冲区大小；放不下完整 JSON 时返回 -ENOSPC，out 置空串
 * @return 0 成功；负数 errno（-EINVAL / -ENOMEM / -ENOSPC）
 */
int hr_measure_history_json(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_HR_MEASURE_H */
