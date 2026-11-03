/****************************************************************************
 * spo2_page.h - 血氧页
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef AI_AGENT_SPO2_PAGE_H
#define AI_AGENT_SPO2_PAGE_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *spo2_page_create(void);
void spo2_page_delete(void);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_SPO2_PAGE_H */
