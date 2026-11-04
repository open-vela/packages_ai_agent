/****************************************************************************
 * alarm_page.h - 闹钟页（定时任务列表）
 *
 * 页面的数据全部来自 infra/cron_service.c 的真实任务表，
 * 页面自己不持有状态，delete 之后什么都不留。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef AI_AGENT_UI_ALARM_PAGE_H
#define AI_AGENT_UI_ALARM_PAGE_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 建页面（幂等：已经建过就直接返回旧指针）。 */
lv_obj_t *alarm_page_create(void);

/** 删页面 + 自己的刷新定时器，指针全部置 NULL。 */
void alarm_page_delete(void);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_UI_ALARM_PAGE_H */
