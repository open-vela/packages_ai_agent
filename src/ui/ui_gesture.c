/****************************************************************************
 * ui_gesture.c - 统一触摸手势识别（设计说明见 ui_gesture.h）
 *
 * 判定分两段：
 *   PRESSING（手指还在动）：只做一件事 —— 判断「有没有变成拖动」。
 *     一旦超过容差就置 s_drag，这样按钮的 CLICKED（在 RELEASED 之后）
 *     一定能看到正确的值。
 *   RELEASED（手指抬起）：算最终方向 + 速度，得出 SWIPE_UP/DOWN。
 *
 * 为什么用「位移 或 速度」两个条件：
 *   只按位移，快速轻扫（flick）要滑满 45px 才认，手感很钝；
 *   加上速度判据后，一个 20px 的快速上挑也能立刻响应，
 *   这正是手机上手势该有的感觉。
 *
 * 为什么去掉 400px 上限：
 *   原来 `dy < -60 && dy > -400`，手指从屏幕底部一路滑到顶部（>400px）
 *   反而**不触发** —— 用户越用力越没反应。上限没有存在理由。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <math.h>
#include <stdlib.h>      /* abs */
#include <string.h>

#include "ui_gesture.h"

/* ── 判据 ────────────────────────────────────────────────────────
 *   DIST_MIN 比原来的 60 宽松：配合速度判据，慢速轻拖也不会误判，
 *   而快速轻扫靠 VEL_MIN 兜住。
 *   DIR_RATIO 1.3 而不是原来的 1.0：手划手势极少是笔直的，
 *   1.0 会让「斜着往上滑」被判成水平而被丢弃。 */
#define GESTURE_DIST_MIN    45      /* px */
#define GESTURE_VEL_MIN     350     /* px/s */
#define GESTURE_DIR_RATIO   1.3f
#define GESTURE_DRAG_TOL    8       /* px，超过它就算拖动，用于防误触 */

/* ── 状态 ──────────────────────────────────────────────────────── */

static lv_point_t s_start;
static lv_point_t s_last;
static uint32_t   s_start_tick;
static bool       s_pressed;
static bool       s_drag;          /* 防误触用：PRESSING 阶段就置位 */
static uint32_t   s_drag_tick;     /* 置位时刻，用于 800ms 后自动作废 */

static ui_gesture_dir_t s_pending; /* 待取走的手势结论 */

static ui_gesture_cb_t s_cb;       /* 页面注册的回调 */

/****************************************************************************
 * 事件
 ****************************************************************************/

static void on_pressed(lv_event_t *e)
{
  lv_indev_t *indev = lv_indev_active();

  (void)e;

  if (indev == NULL)
    {
      return;
    }

  lv_indev_get_point(indev, &s_start);
  s_last        = s_start;
  s_start_tick  = lv_tick_get();
  s_pressed     = true;
  s_drag        = false;
  s_pending     = UI_GESTURE_NONE;
}

static void on_pressing(lv_event_t *e)
{
  lv_indev_t *indev = lv_indev_active();
  int32_t dx;
  int32_t dy;

  (void)e;

  if (!s_pressed || indev == NULL)
    {
      return;
    }

  lv_indev_get_point(indev, &s_last);

  /* ★ 在 PRESSING 阶段就把拖动标记置上。
   * 按钮的 CLICKED 在 RELEASED 之后才发，所以它一定能读到正确的值 ——
   * 这是「上滑误触说话按钮」的根治点。 */
  dx = s_last.x - s_start.x;
  dy = s_last.y - s_start.y;

  if (!s_drag && (abs((int)dx) > GESTURE_DRAG_TOL ||
                  abs((int)dy) > GESTURE_DRAG_TOL))
    {
      s_drag = true;
      s_drag_tick = lv_tick_get();
    }
}

static void on_released(lv_event_t *e)
{
  lv_indev_t *indev = lv_indev_active();
  int32_t dx;
  int32_t dy;
  int32_t adx;
  int32_t ady;
  uint32_t dt;
  float dist;
  float vel;

  (void)e;

  if (!s_pressed)
    {
      return;
    }

  s_pressed = false;

  if (indev != NULL)
    {
      lv_indev_get_point(indev, &s_last);
    }

  dx  = s_last.x - s_start.x;
  dy  = s_last.y - s_start.y;
  adx = dx < 0 ? -dx : dx;
  ady = dy < 0 ? -dy : dy;

  dist = sqrtf((float)dx * dx + (float)dy * dy);

  dt = lv_tick_get() - s_start_tick;
  if (dt == 0)
    {
      dt = 1;
    }

  vel = dist * 1000.0f / (float)dt;

  /* 既没滑够距离、也没够快 → 这是一次点击，不产生滑动手势 */
  if (dist < GESTURE_DIST_MIN && vel < GESTURE_VEL_MIN)
    {
      s_pending = UI_GESTURE_TAP;
      return;
    }

  /* 主方向必须明显压过次方向，否则不算（避免斜着划被误判） */
  if ((float)ady > (float)adx * GESTURE_DIR_RATIO)
    {
      s_pending = (dy < 0) ? UI_GESTURE_SWIPE_UP : UI_GESTURE_SWIPE_DOWN;

      if (s_cb != NULL)
        {
          s_cb(s_pending);
        }
    }
  else
    {
      s_pending = UI_GESTURE_NONE;   /* 横向为主：本期不用 */
    }
}

/****************************************************************************
 * 冒泡
 ****************************************************************************/

/* LVGL 把 PRESSED 只发给最上层可点击对象。页面里的卡片、列表行、气泡
 * 默认都带 CLICKABLE，会把事件吃掉 —— 挂在页面上的手势就永远收不到。
 * 这里递归给所有子对象打开冒泡（本身可点击的照旧可点击，只是事件会往上走）。 */
static void enable_bubble_recursive(lv_obj_t *obj)
{
  uint32_t i;
  uint32_t n;

  if (obj == NULL)
    {
      return;
    }

  lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);

  n = lv_obj_get_child_count(obj);
  for (i = 0; i < n; i++)
    {
      enable_bubble_recursive(lv_obj_get_child(obj, (int32_t)i));
    }
}

/****************************************************************************
 * 公开接口
 ****************************************************************************/

void ui_gesture_attach(lv_obj_t *page, ui_gesture_cb_t cb)
{
  if (page == NULL)
    {
      return;
    }

  s_cb = cb;

  s_pressed = false;
  s_drag    = false;
  s_pending = UI_GESTURE_NONE;

  enable_bubble_recursive(page);

  lv_obj_add_event_cb(page, on_pressed,  LV_EVENT_PRESSED,  NULL);
  lv_obj_add_event_cb(page, on_pressing, LV_EVENT_PRESSING, NULL);
  lv_obj_add_event_cb(page, on_released, LV_EVENT_RELEASED, NULL);
}

ui_gesture_dir_t ui_gesture_take(void)
{
  ui_gesture_dir_t d = s_pending;

  s_pending = UI_GESTURE_NONE;
  return d;
}

bool ui_gesture_was_drag(void)
{
  if (!s_drag)
    {
      return false;
    }

  /* ★ 标记必须会自己过期。
   *
   * s_drag 只在 on_pressed 里清零，而 on_pressed 只有 ui_gesture_attach
   * 过的页面才收得到。没有挂手势的页面（音乐页、计时器页曾经就是）
   * 永远等不到那个 PRESSED，s_drag 就停在 true —— btn_drag_guard 会用它
   * 挡掉**所有**按钮，表现正是"按钮非常不灵敏"。
   *
   * 时间闸：拖动标记只在抬手后很短的窗口内有效，足够覆盖紧随 RELEASED
   * 的 CLICKED，之后自动作废，任何页面都不可能被永久锁死。 */
  if ((uint32_t)(lv_tick_get() - s_drag_tick) > 800)
    {
      s_drag = false;
      return false;
    }

  return true;
}