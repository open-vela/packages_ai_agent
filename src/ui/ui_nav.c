/****************************************************************************
 * ui_nav.c - 页面导航中枢
 *
 * 职责：页面切换的唯一入口。先删旧页，再建新页。
 *
 * 为什么必须"先删"：
 *   每页都自带 1Hz 刷新定时器。如果建新页前不删旧页，旧定时器会继续
 *   按 1Hz 访问已经被 lv_obj_clean() 释放的 label —— use-after-free。
 *   这是之前 launcher_page.c 崩溃的根因（见交接简报 §5）。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdio.h>

#include "ui_nav.h"
#include "ui_gesture.h"      /* 统一手势识别 */
#include "watchface_page.h"
#include "applist_page.h"
#include "heart_page.h"
#include "spo2_page.h"
#include "music_page.h"
#include "timer_page.h"
#include "sport_page.h"   /* 运动页 */
#include "sleep_page.h"   /* 静止/休息页 */
#include "pet_page.h"
#include "settings_page.h"
#include "about_page.h"
#include "alarm_page.h"     /* 闹钟（cron 持久化任务） */
#include "weather_page.h"   /* 天气（北京，Open-Meteo） */
#include "expression_page.h" /* 小云表情选择（2026-11-02 批次 3） */

/****************************************************************************
 * 私有数据
 ****************************************************************************/

static ui_page_t s_current = UI_PAGE_WATCHFACE;

/****************************************************************************
 * 页面销毁
 ****************************************************************************/

static void destroy_current(void)
{
    switch (s_current)
      {
        case UI_PAGE_WATCHFACE:
          watchface_page_delete();
          break;

        case UI_PAGE_HEART:
          heart_page_delete();
          break;

        case UI_PAGE_SPO2:
          spo2_page_delete();
          break;

        case UI_PAGE_MUSIC:
          music_page_delete();
          break;

        case UI_PAGE_TIMER:
          timer_page_delete();
          break;

        case UI_PAGE_SPORT:
          sport_page_delete();
          break;

        case UI_PAGE_SLEEP:
          sleep_page_delete();
          break;

        case UI_PAGE_APPLIST:
          applist_page_delete();
          break;

        case UI_PAGE_AI:
          pet_page_delete();
          break;

        case UI_PAGE_SETTINGS:
          settings_page_delete();
          break;

        case UI_PAGE_ABOUT:
          about_page_delete();
          break;

        case UI_PAGE_ALARM:
          alarm_page_delete();
          break;

        case UI_PAGE_WEATHER:
          weather_page_delete();
          break;

        case UI_PAGE_EXPRESSION:
          expression_page_delete();
          break;

        default:
          /* 未知页：兜底清屏，避免残留对象 */
          lv_obj_clean(lv_scr_act());
          break;
      }
}

/****************************************************************************
 * 页面创建
 ****************************************************************************/

static void on_app_selected(applist_app_t app)
{
    switch (app)
      {
        case APPLIST_APP_HEART:
          ui_nav_goto(UI_PAGE_HEART);
          break;

        case APPLIST_APP_SPO2:
          ui_nav_goto(UI_PAGE_SPO2);
          break;

        case APPLIST_APP_MUSIC:
          ui_nav_goto(UI_PAGE_MUSIC);
          break;

        case APPLIST_APP_TIMER:
          ui_nav_goto(UI_PAGE_TIMER);
          break;

        case APPLIST_APP_SPORT:
          ui_nav_goto(UI_PAGE_SPORT);
          break;

        case APPLIST_APP_SLEEP:
          ui_nav_goto(UI_PAGE_SLEEP);
          break;

        case APPLIST_APP_SETTINGS:
          ui_nav_goto(UI_PAGE_SETTINGS);
          break;

        case APPLIST_APP_ABOUT:
          ui_nav_goto(UI_PAGE_ABOUT);
          break;

        case APPLIST_APP_ALARM:
          ui_nav_goto(UI_PAGE_ALARM);
          break;

        case APPLIST_APP_WEATHER:
          ui_nav_goto(UI_PAGE_WEATHER);
          break;

        case APPLIST_APP_AI:
          /* 云朵桌宠页：用户主动进入 AI 对话。
           * 之前只能等 agent 回复时被动切过去（见 lvgl_ui_channel.c 的
           * bubble_update_async_cb），应用列表里没有入口。 */
          ui_nav_goto(UI_PAGE_AI);
          break;

        case APPLIST_APP_EXPRESSION:
          /* 2026-11-02 批次 3：表情选择页。选完点格子会自己回表盘。 */
          ui_nav_goto(UI_PAGE_EXPRESSION);
          break;

        default:
          /* 只剩未实现的项会走到这里。不做假页面。 */
          printf("[Nav] app %d not implemented yet\n", (int)app);
          break;
      }
}

static void create_page(ui_page_t page)
{
    switch (page)
      {
        case UI_PAGE_WATCHFACE:
          {
            lv_obj_t *p = watchface_page_create();
            watchface_page_set_swipe_cb(NULL); /* 手势由 key/触摸层驱动 */
            (void)p;
          }
          break;

        case UI_PAGE_APPLIST:
          {
            lv_obj_t *p = applist_page_create();
            applist_page_set_open_cb(on_app_selected);
            (void)p;
          }
          break;

        case UI_PAGE_HEART:
          heart_page_create();
          break;

        case UI_PAGE_SPO2:
          spo2_page_create();
          break;

        case UI_PAGE_MUSIC:
          music_page_create();
          break;

        case UI_PAGE_TIMER:
          timer_page_create();
          break;

        case UI_PAGE_SPORT:
          sport_page_create();
          break;

        case UI_PAGE_SLEEP:
          sleep_page_create();
          break;

        case UI_PAGE_SETTINGS:
          {
            lv_obj_t *p = settings_page_create();
            settings_page_set_back_callback(ui_nav_back);
            (void)p;
          }
          break;

        case UI_PAGE_ABOUT:
          {
            lv_obj_t *p = about_page_create();
            about_page_set_back_callback(ui_nav_back);
            (void)p;
          }
          break;

        case UI_PAGE_ALARM:
          alarm_page_create();
          break;

        case UI_PAGE_WEATHER:
          weather_page_create();
          break;

        case UI_PAGE_EXPRESSION:
          expression_page_create();
          break;

        case UI_PAGE_AI:
          {
            lv_obj_t *p = pet_page_create();
            pet_page_set_back_callback(ui_nav_back);
            (void)p;
          }
          break;

        default:
          watchface_page_create();
          break;
      }
}

/****************************************************************************
 * 公开接口
 ****************************************************************************/

void ui_nav_goto(ui_page_t page)
{
    if (page == s_current)
      {
        return;
      }

    printf("[Nav] %d -> %d\n", (int)s_current, (int)page);

    destroy_current();
    s_current = page;
    create_page(page);
}

void ui_nav_back(void)
{
    ui_nav_goto(UI_PAGE_WATCHFACE);
}

ui_page_t ui_nav_current(void)
{
    return s_current;
}

void ui_nav_init(void)
{
    s_current = UI_PAGE_WATCHFACE;
    create_page(UI_PAGE_WATCHFACE);
}

/****************************************************************************
 * 下滑返回手势（公共实现）
 *
 * 为什么不用 LV_EVENT_GESTURE：
 *   那个事件由滚动子系统产生，要求对象可滚动。我们的页面都显式关掉了
 *   滚动（避免误滚和滚动条），所以 GESTURE 永远不会触发。
 *   直接比按下/抬起坐标最可靠。
 ****************************************************************************/

/* 阈值全部搬到 ui_gesture.c 了，这里不再有自己的判定 */

/* DEMO-UI（2026-11-03）：设置页新增「演示与测试」10 行后变成可滚动页面。
 * 页面自己还没滚回顶部时，下滑是"往回滚动列表"，不是"返回上一页" ——
 * 否则用户在设置页里往回滚会直接被弹回表盘。非滚动页面 scroll_y 恒为 0，
 * 行为与以前完全一致。 */
static bool active_page_scrolled(void)
{
    lv_obj_t *scr = lv_screen_active();
    int32_t i;

    for (i = lv_obj_get_child_count(scr) - 1; i >= 0; i--)
      {
        lv_obj_t *c = lv_obj_get_child(scr, i);

        if (c != NULL && !lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN))
          {
            return lv_obj_get_scroll_y(c) > 0;
          }
      }

    return false;
}

/* 收到手势判定的结果。只有 SWIPE_DOWN 会回调过来。 */
static void on_back_gesture(ui_gesture_dir_t direction)
{
    if (direction != UI_GESTURE_SWIPE_DOWN)
      {
        return;
      }

    if (active_page_scrolled())
      {
        return;    /* 列表还没回到顶部：这一下滑动用于滚列表 */
      }

    ui_nav_back();
}

/* 旧的 back_swipe_released / enable_bubble_recursive 已由 ui_gesture 统一接管。
 * 原来那套的问题：
 *   - 要求 dy 落在 60..400，手指从顶滑到底（>400）反而不触发
 *   - 只看位移不看速度，快速轻扫不认
 *   - 冒泡要每个页面自己记得开，漏一个就"能进不能退"
 * 这些现在都在 ui_gesture.c 里统一处理。 */
static void unused_enable_bubble_recursive(lv_obj_t *obj)
{
    uint32_t i;
    uint32_t n;

    if (obj == NULL)
      {
        return;
      }

    n = lv_obj_get_child_count(obj);

    for (i = 0; i < n; i++)
      {
        lv_obj_t *child = lv_obj_get_child(obj, i);

        lv_obj_add_flag(child, LV_OBJ_FLAG_EVENT_BUBBLE);
        unused_enable_bubble_recursive(child);
      }
}

void ui_nav_attach_back_swipe(lv_obj_t *page)
{
    if (page == NULL)
      {
        return;
      }

    /* 统一手势：只关心 SWIPE_DOWN。上限/速度/方向比都由 ui_gesture 决定，
     * 同时也由它负责给子对象打开冒泡。 */
    ui_gesture_attach(page, on_back_gesture);
}
