/*
 * companion_mood.h — 桌面小跟班「触觉情绪」模块 (openvela / AI-VOX3)
 *
 * 输入（两个都是**输入**传感器，接在板级 devgpio 上）：
 *   /dev/gpio1 = GPIO42  TTP223 触摸   → 主人抚摸它 → mood +1 → 点头
 *   /dev/gpio2 = GPIO48  SW-420 拍打   → 主人拍了它 → mood -1 → 摇头
 *
 * 输出：舵机动作（复用 servo_nod / servo_shake）+ 一个 mood 数值。
 *
 * 注意：板子上没有震动马达，"震动"指的是**感知被拍**，不是震动反馈。
 */
#ifndef __PACKAGES_AI_AGENT_SRC_COMPANION_MOOD_H
#define __PACKAGES_AI_AGENT_SRC_COMPANION_MOOD_H

#ifdef __cplusplus
extern "C"
{
#endif

/* 启动轮询任务。返回 0 表示任务已创建；失败返回负数（不致命）。 */
int  companion_mood_start(void);

/* 停止轮询任务。 */
void companion_mood_stop(void);

/* 当前情绪值，范围 -2 .. +2（sad / unhappy / neutral / happy / excited）。 */
int  companion_mood_get(void);

/* 情绪值转文字，便于日志与 UI 显示。返回静态字符串。 */
const char *companion_mood_name(int mood);

/* 累计触发次数，用于验收（摸了几次 / 拍了几次）。 */
void companion_mood_counts(int *n_touch, int *n_tap);

#ifdef __cplusplus
}
#endif
#endif
