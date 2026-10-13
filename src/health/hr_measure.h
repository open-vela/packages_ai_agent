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
 * 启动一次单次测量。
 *
 * 单飞：测量进行中再调用直接忽略。
 *
 * @return 0        已启动（worker 线程已创建）
 *         -EBUSY   测量进行中，本次点击忽略
 *         -ENOMEM  线程创建失败
 */
int hr_measure_start(void);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_HR_MEASURE_H */
