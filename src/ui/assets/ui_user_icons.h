#ifndef UI_USER_ICONS_H
#define UI_USER_ICONS_H

#include <lvgl/lvgl.h>

typedef struct { const char *name; const lv_image_dsc_t *dsc; } ui_user_icon_t;

extern const ui_user_icon_t ui_user_icon_table[];
extern const unsigned int ui_user_icon_count;

#endif
