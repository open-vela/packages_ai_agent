/****************************************************************************
 * bubble_grid.h - Apple Watch 风格的蜂窝气泡网格
 *
 * 参考实现：LVGL 官方博客《解析 Apple Watch Bubble 组件》
 *   https://lvgl.io/blog/zh/tutorial-recreating-apple-watch-bubble-component
 * 源码借鉴：ElenixOS（基于 LVGL 的开源手表系统）
 *   https://github.com/ElenixOS/ElenixOS/src/ui/widgets/bubble_grid
 *
 * 为什么自己写一个而不是直接用 ElenixOS：
 *   1. 它依赖 eos_image / eos_widget_data / eos_theme 一套私有框架，
 *      搬到我们这个 NuttX + LVGL 9.1 的工程里要连带搬一堆东西
 *   2. 它是给 466x466 圆屏 + 十几个应用调的参，我们 390x450 方屏 + 8 个应用
 *
 * 所以这里按它的**算法**重新实现，参数按我们的屏幕重算。
 *
 * 核心三件事（官方教程里也是这么拆的）：
 *   1. 蜂窝错列布局：偶数行 3 个、奇数行 4 个，每行独立居中
 *   2. 距离场缩放：靠中心的图标大，靠边缘的小
 *   3. 物理交互：拖动跟手 + 松手回弹；按下局部缩小变暗
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef AI_AGENT_BUBBLE_GRID_H
#define AI_AGENT_BUBBLE_GRID_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 一个气泡的描述 */
typedef struct
{
    const lv_image_dsc_t *icon;   /* 图标（会被染成白色），可为 NULL */
    const char *label;            /* 下方标签，可为 NULL */
    const char *glyph;            /* icon 为空时用文字代替图标（如 "i"） */
    uint32_t tint;                /* 圆底颜色 */
    int id;                       /* 回调里回传的标识 */
} bubble_item_t;

typedef void (*bubble_click_cb_t)(int id);

/**
 * 创建气泡网格，占满 parent。
 *
 * @param parent  父对象（通常是页面）
 * @param items   气泡数组
 * @param count   气泡数量
 * @param cb      点击回调
 */
lv_obj_t *bubble_grid_create(lv_obj_t *parent, const bubble_item_t *items,
                             int count, bubble_click_cb_t cb);

/** 销毁（会停掉内部定时器） */
void bubble_grid_delete(void);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_BUBBLE_GRID_H */
