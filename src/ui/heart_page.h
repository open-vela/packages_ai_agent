/****************************************************************************
 * heart_page.h - 心率页
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef AI_AGENT_HEART_PAGE_H
#define AI_AGENT_HEART_PAGE_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *heart_page_create(void);
void heart_page_delete(void);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_HEART_PAGE_H */
