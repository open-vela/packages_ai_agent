/****************************************************************************
 * watchface_page.h - 表盘
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef AI_AGENT_WATCHFACE_PAGE_H
#define AI_AGENT_WATCHFACE_PAGE_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*watchface_swipe_cb_t)(void);

/**
 * 建表盘页。返回页面对象。
 */
lv_obj_t *watchface_page_create(void);

/**
 * 设置"上滑"回调（进入应用列表）。
 */
void watchface_page_set_swipe_cb(watchface_swipe_cb_t cb);

/**
 * 销毁表盘页，同时停掉刷新定时器。
 *
 * 定时器必须在这里删：页面对象被删后 label 指针失效，
 * 而定时器回调还在按 1Hz 访问它们 —— 就是 use-after-free。
 */
void watchface_page_delete(void);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_WATCHFACE_PAGE_H */
