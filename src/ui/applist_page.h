/****************************************************************************
 * applist_page.h - 应用列表
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef AI_AGENT_APPLIST_PAGE_H
#define AI_AGENT_APPLIST_PAGE_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 应用标识（回调里告诉上层点了哪个） */

typedef enum
{
  APPLIST_APP_HEART = 0,
  APPLIST_APP_SPO2,
  APPLIST_APP_SPORT,
  APPLIST_APP_TIMER,
  APPLIST_APP_MUSIC,
  APPLIST_APP_SLEEP,
  APPLIST_APP_SETTINGS,
  APPLIST_APP_ABOUT,
  APPLIST_APP_ALARM,       /* 闹钟：cron 持久化任务列表 */
  APPLIST_APP_WEATHER,     /* 天气：北京 Open-Meteo */
  APPLIST_APP_AI,          /* AI 对话（云朵桌宠页） */
  APPLIST_APP_EXPRESSION   /* 小云表情选择（2026-11-02 批次 3） */
} applist_app_t;

typedef void (*applist_open_cb_t)(applist_app_t app);

lv_obj_t *applist_page_create(void);
void applist_page_set_open_cb(applist_open_cb_t cb);
void applist_page_delete(void);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_APPLIST_PAGE_H */
