/****************************************************************************
 * timer_page.h - 计时器页
 *
 * 应用列表里一直有「计时器」这个图标，但点下去只打印一句
 * not implemented —— 属于「有位置、没功能」。这个页面把它补上。
 *
 * 设计取舍：
 *   1) 只做**倒计时**（1/3/5/10/15 分钟预设）。秒表需要毫秒级刷新，
 *      而这台机器的全屏重绘是 74~115ms，秒表会自己卡自己，做了也难看。
 *   2) 时间到用**震动**提示（vibrator_play_oneshot），不依赖扬声器 ——
 *      音乐播放器占着音频设备时闹铃也得能响。
 *   3) 倒计时用 1Hz 的 lv_timer + 绝对到期时刻，不用累加计数。
 *      后者会随渲染负载漂移（实测 render 一圈 20~50ms 不等）。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef AI_AGENT_TIMER_PAGE_H
#define AI_AGENT_TIMER_PAGE_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 创建计时器页。重复调用返回同一实例。 */
lv_obj_t *timer_page_create(void);

/** 销毁页面与其定时器。导航切页时调用。 */
void timer_page_delete(void);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_TIMER_PAGE_H */