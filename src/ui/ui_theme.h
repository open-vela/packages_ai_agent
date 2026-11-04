/****************************************************************************
 * ui_theme.h - 全局设计令牌
 *
 * 这个文件存在的理由：
 *   改之前颜色和尺寸散落在 5 个页面里，写成裸的 0xffffff / 60 / 20。
 *   同一个产品里出现两套相反的配色（浅底白字 / 深底浅字），改一处配色
 *   要翻四个文件，而且没有任何依据说明"为什么是这个值"。
 *
 * 规则：
 *   1. 页面里不允许再出现裸颜色字面量，只能用下面的令牌
 *   2. 任何"文字 + 背景"组合，对比度 >= 4.5:1（正文）/ 3:1（大字号）
 *   3. 令牌名描述用途（TEXT_PRIMARY），不描述外观（WHITE）
 *   4. 尺寸一律取 8 的倍数（8pt 栅格），保证跨页面对齐
 *
 * 视觉方向（2026-10-19 第二次调整）：
 *   第一版用了 0x1a1a2e，在 AMOLED 上看着仍然像黑色（RGB 26,26,46）。
 *   这一版改成明显的深蓝渐变（上亮下暗），一眼能看出是蓝，
 *   又保持深底高对比。渐变由 LVGL 的 bg_grad 逐行插值，开销很低。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef AI_AGENT_UI_THEME_H
#define AI_AGENT_UI_THEME_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 配色：Apple Watch 内页风格 ──────────────────────────────────
 *
 * 苹果手表 App 内部的视觉规则（对照 wOS 真机）：
 *   1. **纯黑背景**。OLED 黑像素不发光，省电，也是 Apple Watch 识别度
 *      最强的特征。
 *   2. **没有卡片**。信息靠字号层级和灰阶区分，不靠白底方块。
 *      卡片是手机/网页的语汇，手表上一块白底会把整个屏压死。
 *   3. **文字只用三级**：白（主）/ 灰（次要）/ 深灰（提示）。
 *   4. **强调色用系统色**：Apple 的 #FF375F 心率、#30D158 活动、
 *      #0A84FF 蓝。
 *   5. 可点元素是**圆角胶囊**，居中，宽度不撑满。
 */

/* ── 运行时主题 ──────────────────────────────────────────────────
 *
 * 这些值原来是编译期常量（#define），改一个颜色就要重新编译。
 * 现在搬到运行时的结构体里，启动时从 theme.ini 读，缺项用默认值。
 *
 * 这样设计师在 Windows 上改 theme.ini 保存、刷新页面就能看到效果，
 * **完全不需要编译器**。
 */

typedef struct
{
    /* 颜色 */
    uint32_t bg_black;
    uint32_t surface;
    uint32_t surface_hi;
    uint32_t text_primary;
    uint32_t text_secondary;
    uint32_t text_tertiary;
    uint32_t accent;
    uint32_t color_heart;
    uint32_t color_step;
    uint32_t color_weather;
    uint32_t divider;

    /* 尺寸 */
    int pad_page;
    int gap_s;
    int gap_m;
    int gap_l;
    int statusbar_h;
    int radius_card;
    int radius_row;
    int radius_pill;

    /* 蜂窝 */
    int bubble_base;
    int bubble_min_pct;      /* 边缘气泡最小缩放（百分比） */
} ui_theme_t;

extern ui_theme_t g_theme;

/** 恢复默认值 */
void ui_theme_defaults(void);

/** 从 ini 读主题（key = 0xRRGGBB）。文件不存在就用默认值，不报错。 */
void ui_theme_load(const char *path);

/** 从内存里的 ini 文本加载（真机用） */
void ui_theme_load_string(const char *ini);

/* 代码里原来的写法不变，继续用这些名字 */

#define UI_BG_BLACK       (g_theme.bg_black)
#define UI_SURFACE        (g_theme.surface)
#define UI_SURFACE_HI     (g_theme.surface_hi)
#define UI_TEXT_PRIMARY   (g_theme.text_primary)
#define UI_TEXT_SECONDARY (g_theme.text_secondary)
#define UI_TEXT_TERTIARY  (g_theme.text_tertiary)
#define UI_ACCENT         (g_theme.accent)
#define UI_COLOR_HEART    (g_theme.color_heart)
#define UI_COLOR_STEP     (g_theme.color_step)
#define UI_COLOR_WEATHER  (g_theme.color_weather)
#define UI_COLOR_BATTERY  (g_theme.color_step)
#define UI_COLOR_WARNING  (g_theme.color_heart)
#define UI_DIVIDER        (g_theme.divider)

#define UI_PAD_PAGE       (g_theme.pad_page)
#define UI_GAP_S          (g_theme.gap_s)
#define UI_GAP_M          (g_theme.gap_m)
#define UI_GAP_L          (g_theme.gap_l)
#define UI_STATUSBAR_H    (g_theme.statusbar_h)
#define UI_RADIUS_CARD    (g_theme.radius_card)
#define UI_RADIUS_ROW     (g_theme.radius_row)
#define UI_RADIUS_PILL    (g_theme.radius_pill)

#define UI_SCREEN_W       390
#define UI_SCREEN_H       450
#define UI_TAP_MIN        60

/* ── 字体令牌 ────────────────────────────────────────────────────
 *
 * 中文字形只在 cjk_* 里。sans_88_bold 只有 15 个数字字形，只能给纯数字用。
 */

extern const lv_font_t ui_font_sans_16;
extern const lv_font_t ui_font_cjk_18;
extern const lv_font_t ui_font_cjk_24;
extern const lv_font_t ui_font_sans_88_bold;

#define UI_FONT_CAPTION   (&ui_font_cjk_18)
#define UI_FONT_SMALL     (&ui_font_cjk_18)
#define UI_FONT_BODY      (&ui_font_cjk_24)
#define UI_FONT_TITLE     (&ui_font_cjk_24)
#define UI_FONT_TIME      (&ui_font_sans_88_bold)

/* ── 公共控件构造函数（实现见 ui_theme.c）─────────────────────── */

/** 给页面贴壁纸 + 内边距 + 禁止滚动。所有页面第一行都调它。 */
void ui_page_init(lv_obj_t *page);

/** 只贴背景（壁纸 + 蒙版），给数据驱动引擎复用 */
void ui_page_apply_bg(lv_obj_t *page);

/** 白色内容面板：占满页面内容区，圆角，90% 不透明白底。
 *  所有文字都应该放在它（或卡片）上面，不要直接压在壁纸上。 */
lv_obj_t *ui_sheet_create(lv_obj_t *page);

/** 标准卡片：圆角 + 表面色 + 不可滚动 + 默认不可点。 */
lv_obj_t *ui_card_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h);

/** 标准胶囊按钮。文字可用 ui_button_set_text 更新。 */
lv_obj_t *ui_button_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                           uint32_t color, const char *text,
                           lv_event_cb_t cb, void *user_data);

void ui_button_set_text(lv_obj_t *btn, const char *text);
/* ── 安全区与浮动按钮（2026-11-03）────────────────────────────────
 *
 * 圆角屏上，可点元素不要落在 x<24 或 y<52 的区域：那正是圆角与
 * 状态栏挤压的地方。设计包 lumina_watch_ui.c 用 24/52，本仓取同样
 * 数值；返回键尺寸用 84x60（>= UI_TAP_MIN 60 的触控下限）。
 */
#define UI_SAFE_X         24
#define UI_SAFE_Y         52
/* 圆角屏底部安全区：可点元素/正文不要压到最后 40px 内，
 * 否则会被屏幕圆角/边框遮住（桌宠页"开始说话"按钮实测被遮）。 */
#define UI_SAFE_BOTTOM    40
#define UI_BACK_W         84
#define UI_BACK_H         60

/** 浮动按钮：IGNORE_LAYOUT + 绝对坐标（x/y 是相对屏幕的最终坐标，
 *  函数内部自动减去页面 padding）。用于返回键、右上角“表情/记录”。 */
lv_obj_t *ui_float_button_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                                 lv_coord_t x, lv_coord_t y, uint32_t color,
                                 const char *text, lv_event_cb_t cb,
                                 void *user_data);

/** 标准返回键：84x60，左上安全区（24,52），文字“返回”。 */
lv_obj_t *ui_back_button_create(lv_obj_t *parent, lv_event_cb_t cb,
                                void *user_data);

void ui_button_set_color(lv_obj_t *btn, uint32_t color);

/**
 * 按底色亮度返回可读的标签颜色（白或 UI_TEXT_PRIMARY）。
 *
 * 浅色主题（2026-11-02 起）里按钮底色分两类：浅色面（#E8F0F5 卡片色、
 * #D2E4EC 按下态）配深色文字，深色面（#0A6E93 主色、#BA3452 心率、
 * #1E7A46 步数、#9C6000 天气）必须配白字，否则对比度只有 2.2~2.9:1。
 *
 * 凡是**自己拼 lv_button_create + lv_label_create** 的地方（心率/血氧/
 * 天气/闹钟/关于页，因为 24px 子集字体缺字才没用 ui_button_create），
 * 都应该用这个函数决定标签颜色，不要写死 UI_TEXT_PRIMARY。
 * 阈值取相对亮度 0.35，落在浅色面（>=0.74）与深色面（<=0.26）之间的空档。
 */
uint32_t ui_text_on_bg(uint32_t bg);

/** 设置项一行：左名称 + 右值。 */
lv_obj_t *ui_row_create(lv_obj_t *parent, const char *name,
                        const char *value, lv_event_cb_t cb, void *user_data);
void ui_row_set_value(lv_obj_t *row, const char *value);

/** 顶部状态栏：左侧标题（可为 NULL），右侧电量 + 电池图标。 */
lv_obj_t *ui_statusbar_create(lv_obj_t *parent, const char *title);

/** 状态栏里的电量文本标签，供定时刷新用。 */
lv_obj_t *ui_statusbar_battery_label(lv_obj_t *statusbar);

/** 内置图标（由 tools/gen_icons.py 生成） */
extern const lv_image_dsc_t ui_icon_battery;

/* 2026-11-02：壁纸符号 `img_background_watch_px` 已下线。
 * 浅色云光主题改用纯色底（见 ui_page_apply_bg()），不再需要
 * 390x450 全屏位图，省 351,000 B flash。img_background_watch.c 仍留在
 * 磁盘上作参考，但已不再加入编译（packages/ai_agent/CMakeLists.txt）。 */

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_UI_THEME_H */
