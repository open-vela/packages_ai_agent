/****************************************************************************
 * spo2_measure.h - One-shot blood-oxygen (SpO2) measurement.
 *
 * 桌宠页「测血氧」按钮背后的单次测量服务：
 *   点按钮 -> spo2_measure_start() 起一个独立 worker 线程，立刻返回；
 *   worker 调用 hr_sensor 的 spo2 子命令（MODE_SPO2 双通道采集 ~19s），
 *   结束后读 max30102.c 暴露的全局结果，经 lvgl_ui_channel_send_kind()
 *   一次完成上屏 + 语音播报。
 *
 * 与 hr_measure 的区别：血氧不走 uORB（框架没有 algo_spo2 话题），
 *   而是由 max30102.c 的 spo2_measure() 直接算 R 比值并通过全局变量
 *   g_max30102_spo2_{r,pct,ok} 交出来。
 *
 * 调用约束：任何线程都可以调，函数内部只做线程创建，不碰 LVGL、
 *   不阻塞调用者（LVGL 是 LV_OS_NONE，绝不能在 render 线程上同步跑 19s）。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef AI_AGENT_SPO2_MEASURE_H
#define AI_AGENT_SPO2_MEASURE_H

#include <stdbool.h>

#include <nuttx/config.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 启动一次血氧测量。
 *
 * @return 0        已成功起线程
 *         -EBUSY   上一轮还没结束
 *         其他负值 线程创建失败
 */
int spo2_measure_start(void);

/** 是否正在测量（给 UI 判断用） */
bool spo2_measure_busy(void);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_SPO2_MEASURE_H */
