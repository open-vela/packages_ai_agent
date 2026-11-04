/****************************************************************************
 * expression_page.h - 小云表情选择页
 *
 * 2026-11-02（批次 3）：先做 8 款（1 页 6 格 + 1 页 2 格，上限可扩到
 * 9 页 54 格以容纳设计稿的 50 款）。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef AI_AGENT_EXPRESSION_PAGE_H
#define AI_AGENT_EXPRESSION_PAGE_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *expression_page_create(void);
void expression_page_delete(void);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_EXPRESSION_PAGE_H */
