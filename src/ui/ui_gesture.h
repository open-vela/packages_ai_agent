/****************************************************************************
 * ui_gesture.h - 统一触摸手势识别
 *
 * 为什么要有这个模块：
 *   原来表盘、应用列表、返回手势各写了一套「记按下点 → 抬起点做差 →
 *   落在 60..400 之间才算」。三份重复代码，同样三个毛病：
 *     1) 有 400px 上限 —— 滑得太快太远反而不触发
 *     2) 只看位移不看速度 —— 快速轻扫（flick）永远不认
 *     3) 按钮的 CLICKED 与滑动毫无关系 —— 手指按在按钮上上滑，
 *        滑动被识别的同时按钮也收到一次 CLICKED（误触录音/误进页面）
 *
 * 这个模块解决三件事：
 *   - 判定用「位移 或 速度」，并去掉上限；方向比放宽到 1.3
 *   - 只有一个全局状态机，三处共用
 *   - 提供 ui_gesture_was_drag()，按钮在 CLICKED 里查一下就能免疫误触
 *     （拖动标记在 PRESSING 阶段就置位，早于 RELEASED/CLICKED，时序可靠）
 *
 * 用法：页面创建时 ui_gesture_attach(page)，然后在 ui_nav 里
 *       switch (ui_gesture_take()) { SWIPE_UP: 进列表; SWIPE_DOWN: 返回; }
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef AI_AGENT_UI_GESTURE_H
#define AI_AGENT_UI_GESTURE_H

#include <stdbool.h>

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
  UI_GESTURE_NONE = 0,
  UI_GESTURE_SWIPE_UP,     /* 上滑：表盘 → 应用列表 */
  UI_GESTURE_SWIPE_DOWN,   /* 下滑：返回上一级 */
  UI_GESTURE_TAP,          /* 原地点击（页面级，不消费子对象的事件）*/
} ui_gesture_dir_t;

/** 手势回调。direction 只会是 SWIPE_UP / SWIPE_DOWN（TAP 不回调）。 */
typedef void (*ui_gesture_cb_t)(ui_gesture_dir_t direction);

/**
 * 给页面挂上识别器。
 * 事件靠冒泡从子对象传上来，内部会递归打开冒泡。
 * cb 收到判定结果；传 NULL 表示先挂着但不要回调（之后可用 ui_gesture_take 取）。
 */
void ui_gesture_attach(lv_obj_t *page, ui_gesture_cb_t cb);

/**
 * 取走一次手势结论（取走即清空）。没有结论时返回 UI_GESTURE_NONE。
 * 页面在 RELEASED 之后调用一次即可。
 */
ui_gesture_dir_t ui_gesture_take(void);

/**
 * 本次触摸是否已经构成拖动。
 * 供按钮的 CLICKED 回调做防误触：
 *     if (ui_gesture_was_drag()) return;
 * 标记在 PRESSING 阶段就置位，一定早于 CLICKED。
 */
bool ui_gesture_was_drag(void);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_UI_GESTURE_H */