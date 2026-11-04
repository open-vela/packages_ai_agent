/****************************************************************************
 * ui_icon_runtime.h - 运行时图标覆盖
 *
 *   const lv_image_dsc_t *dsc = ui_icon("app_heart");
 *
 * 会先找 icons/app_heart.png，找不到就用内置图标。
 ****************************************************************************/

#ifndef UI_ICON_RUNTIME_H
#define UI_ICON_RUNTIME_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 取图标。icons/<name>.png 存在则用它，否则用内置的。 */
const lv_image_dsc_t *ui_icon(const char *name);

/** 这张图是不是内置的。
 *
 * 用途：内置图标是白色线条画，压在彩色圆上要染成白色；
 * 设计师自己放进 icons/ 的彩色图**不能染色**，否则会被漂成白色。 */
int ui_icon_is_baked(const lv_image_dsc_t *dsc);

/** 释放已加载的覆盖图标（切主题时用） */
void ui_icon_clear_cache(void);

/** 启动时打印可用图标名 */
void ui_icon_report(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_ICON_RUNTIME_H */
