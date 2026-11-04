/****************************************************************************
 * sleep_page.h - 静止/休息页
 *
 * 只有加速度计，**没有心率、没有体动分期**，所以这里不声称"睡眠" ——
 * 它如实显示"已经静止了多久"，以及本次开机以来累计活动时长。
 *
 * 这个取舍是刻意的：坐在椅子上不动也能连续静止两小时，
 * 那不是睡眠。做成"睡眠报告"就是在编数据。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/
#ifndef AI_AGENT_SLEEP_PAGE_H
#define AI_AGENT_SLEEP_PAGE_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *sleep_page_create(void);
void      sleep_page_delete(void);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_SLEEP_PAGE_H */
