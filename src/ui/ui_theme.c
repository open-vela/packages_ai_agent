#include <lvgl/lvgl.h>
#include <math.h>            /* pow: 按钮标签颜色的相对亮度换算 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_theme.h"
#include "ui_gesture.h"      /* 防误触：btn_drag_guard 要查拖动标记 */
#include "ui_icon_runtime.h"


/****************************************************************************
 * 运行时主题：默认值 + ini 加载
 ****************************************************************************/

ui_theme_t g_theme;

/* ★ 2026-11-02 视觉方向第三版：浅色云光（Lumina）。
 *
 * 前两版的取舍记录在 docs_ble/34 号文：第一版深蓝渐变、第二版纯黑
 * （"AMOLED 黑底省电"），第三版把浅蓝壁纸请回来并加 45% 白蒙版。
 * 这一版按 docs_ble/92 号方案去掉壁纸，改纯色暖白底 + 实体卡片，
 * 顺带回收 351,000 B flash（img_background_watch_px 不再被引用）。
 *
 * 每一条颜色的对比度都在 docs_ble/92 号文里算过（WCAG 2.x 公式，
 * 纯色对纯色）。规则不变：正文 >= 4.5:1，大字 >= 3:1。
 *
 * ⚠️ 改动这里的任何色值都要重算对比度：ui_button_create 的标签颜色
 * 是按底色亮度自动选的（见 ui_text_on_bg()），换深色底就可能
 * 让深色文字落在深色按钮上，形成 2.5:1 的不可读按钮。 */
void ui_theme_defaults(void)
{
    /* 页面底色：暖白 #F8FBFD。不再用壁纸，这是最终可见的那一层。 */
    g_theme.bg_black        = 0xf8fbfd;
    /* 卡片 #E8F0F5 —— 比底色深一档，靠明度差而不是描边分层 */
    g_theme.surface         = 0xe8f0f5;
    /* 按下态 / 次级填充 #D2E4EC（深色文字压上去 11.2:1） */
    g_theme.surface_hi      = 0xd2e4ec;
    g_theme.text_primary    = 0x172a3d;   /* 14.1:1 on page, 12.7:1 on card */
    g_theme.text_secondary  = 0x435a70;   /*  6.9:1 on page,  6.2:1 on card */
    g_theme.text_tertiary   = 0x53697e;   /*  5.5:1 on page,  4.9:1 on card */
    /* 主色：把 Lumina 的 #087FA9 压深到 #0A6E93。
     * 原值配白字只有 4.55:1（贴门槛），压深后 5.73:1；配深色字 2.55:1
     * 不可用 —— 所以按钮标签必须自动选白字（见 ui_text_on_bg）。 */
    g_theme.accent          = 0x0a6e93;
    g_theme.color_heart     = 0xba3452;   /* 白字 5.67:1 / 浅底文字 5.46:1 */
    g_theme.color_step      = 0x1e7a46;   /* 白字 5.35:1；原 #30d158 在浅底仅 1.95:1 */
    g_theme.color_weather   = 0x9c6000;   /* 白字 5.14:1 / 浅底文字 4.94:1 */
    g_theme.divider         = 0x8fa1bb;   /* 分隔线，只需 3:1 可见性 */

    g_theme.pad_page        = 20;
    g_theme.gap_s           = 8;
    g_theme.gap_m           = 12;
    g_theme.gap_l           = 20;
    g_theme.statusbar_h     = 32;
    g_theme.radius_card     = 20;
    g_theme.radius_row      = 16;
    g_theme.radius_pill     = 28;

    g_theme.bubble_base     = 88;
    g_theme.bubble_min_pct  = 74;
}

/* 去掉行尾空白 */
static void rtrim(char *s)
{
    int n = (int)strlen(s);

    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' ' || s[n - 1] == '\t'))
      {
        s[--n] = '\0';
      }
}

/* 解析一段 ini 文本（文件和内嵌字符串共用） */
static void theme_parse(const char *text)
{
    char line[256];
    const char *p = text;

    while (p != NULL && *p != '\0')
      {
        const char *nl = strchr(p, '\n');
        size_t n = (nl != NULL) ? (size_t)(nl - p) : strlen(p);

        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = '\0';
        p = (nl != NULL) ? nl + 1 : NULL;

        {
        char *eq;
        char *key;
        char *val;
        unsigned long num;

        rtrim(line);

        /* 跳过空行和注释 */

        key = line;
        while (*key == ' ' || *key == '\t') key++;

        if (*key == '\0' || *key == '#' || *key == ';')
          {
            continue;
          }

        eq = strchr(key, '=');

        if (eq == NULL)
          {
            continue;
          }

        *eq = '\0';
        val = eq + 1;

        rtrim(key);
        while (*val == ' ' || *val == '\t') val++;

        num = strtoul(val, NULL, 0);

        /* 颜色 */

        if      (strcmp(key, "text_primary")   == 0) g_theme.text_primary   = (uint32_t)num;
        else if (strcmp(key, "text_secondary") == 0) g_theme.text_secondary = (uint32_t)num;
        else if (strcmp(key, "text_tertiary")  == 0) g_theme.text_tertiary  = (uint32_t)num;
        else if (strcmp(key, "accent")         == 0) g_theme.accent         = (uint32_t)num;
        else if (strcmp(key, "heart")          == 0) g_theme.color_heart    = (uint32_t)num;
        else if (strcmp(key, "step")           == 0) g_theme.color_step     = (uint32_t)num;
        else if (strcmp(key, "weather")        == 0) g_theme.color_weather  = (uint32_t)num;
        else if (strcmp(key, "surface")        == 0) g_theme.surface        = (uint32_t)num;
        else if (strcmp(key, "divider")        == 0) g_theme.divider        = (uint32_t)num;
        else if (strcmp(key, "bg")             == 0) g_theme.bg_black       = (uint32_t)num;

        /* 尺寸 */

        else if (strcmp(key, "pad_page")       == 0) g_theme.pad_page      = (int)num;
        else if (strcmp(key, "gap_s")          == 0) g_theme.gap_s         = (int)num;
        else if (strcmp(key, "gap_m")          == 0) g_theme.gap_m         = (int)num;
        else if (strcmp(key, "gap_l")          == 0) g_theme.gap_l         = (int)num;
        else if (strcmp(key, "statusbar_h")    == 0) g_theme.statusbar_h   = (int)num;
        else if (strcmp(key, "radius_card")    == 0) g_theme.radius_card   = (int)num;
        else if (strcmp(key, "radius_row")     == 0) g_theme.radius_row    = (int)num;
        else if (strcmp(key, "radius_pill")    == 0) g_theme.radius_pill   = (int)num;

        /* 蜂窝 */

        else if (strcmp(key, "bubble_base")    == 0) g_theme.bubble_base    = (int)num;
        else if (strcmp(key, "bubble_min_pct") == 0) g_theme.bubble_min_pct = (int)num;
      }

    }
}

void ui_theme_load_string(const char *ini)
{
    ui_theme_defaults();

    if (ini != NULL)
      {
        theme_parse(ini);
        printf("[Theme] 已加载内嵌主题\n");
      }
}

void ui_theme_load(const char *path)
{
    FILE *f;
    char *buf;
    long len;

    ui_theme_defaults();

    if (path == NULL)
      {
        return;
      }

    f = fopen(path, "r");

    if (f == NULL)
      {
        printf("[Theme] 没有 %s，用默认配色\n", path);
        return;
      }

    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);

    buf = (char *)malloc((size_t)len + 1);

    if (buf != NULL && fread(buf, 1, (size_t)len, f) == (size_t)len)
      {
        buf[len] = '\0';
        theme_parse(buf);
        printf("[Theme] 已加载 %s\n", path);
      }

    free(buf);
    fclose(f);
}

/****************************************************************************
 * ui_theme.c - 设计令牌的公共控件实现
 *
 * 目的：把"页面长什么样"从每个页面里抽出来，收敛到一处。
 *   之前 4 个页面各写一遍圆角/背景/内边距，值还都不一样，
 *   结果就是"看起来像几个人拼的"。这里统一成一套构造函数。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/


/****************************************************************************
 * 页面背景
 *
 * ★ 2026-11-02（批次 1）：**去掉全屏壁纸**。
 *
 * 之前是「390x450 云层壁纸 RGB565 + 45% 白蒙版 + 兜底色」三层叠出来
 * 的效果。换成纯色暖白底 + 实体卡片之后，这三层的存在理由都没了：
 *
 *   1. 壁纸要 351,000 B flash（img_background_watch_px），而它的作用
 *      只是"让背景不是纯色"。浅色云光方向下底色是 #F8FBFD，纯色反而
 *      更干净，卡片才有对比度。
 *   2. 45% 白蒙版是为了压低壁纸下半那条深蓝（RGB(0,105,230)）以保证
 *      文字对比度。没有壁纸就不需要蒙版，也省掉每页一个全屏对象的
 *      创建和合成开销。
 *   3. 兜底色原本取壁纸平均色 0x81bff8，现在底色本身就是最终可见层，
 *      直接写 UI_BG_BLACK（= #F8FBFD）。
 *
 * 省下来的 351,000 B 记在 docs_ble/92 号文的资源预算表里。
 ****************************************************************************/

void ui_page_apply_bg(lv_obj_t *page)
{
    lv_obj_set_style_bg_image_src(page, NULL, 0);
    lv_obj_set_style_bg_color(page, lv_color_hex(UI_BG_BLACK), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(page, LV_GRAD_DIR_NONE, 0);
}

void ui_page_init(lv_obj_t *page)
{
    lv_obj_set_size(page, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, UI_PAD_PAGE, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);

    ui_page_apply_bg(page);
}

lv_obj_t *ui_sheet_create(lv_obj_t *page)
{
    lv_obj_t *sheet = lv_obj_create(page);

    lv_obj_set_size(sheet, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(sheet, lv_color_hex(UI_SURFACE), 0);
    lv_obj_set_style_bg_opa(sheet, (lv_opa_t)(255 * 92 / 100), 0);
    lv_obj_set_style_border_width(sheet, 0, 0);
    lv_obj_set_style_radius(sheet, 28, 0);
    lv_obj_set_style_pad_all(sheet, UI_GAP_M, 0);
    lv_obj_clear_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(sheet, LV_SCROLLBAR_MODE_OFF);

    return sheet;
}

/****************************************************************************
 * 卡片
 ****************************************************************************/

lv_obj_t *ui_card_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *card = lv_obj_create(parent);

    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, UI_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);

    return card;
}

/****************************************************************************
 * 按钮
 ****************************************************************************/

/* 按钮上的文字标签用 obj 的 user_data 链找太脆弱，这里用一个固定的
 * 命名规则：按钮的唯一子对象就是文字标签。 */

static lv_obj_t *btn_label(lv_obj_t *btn)
{
    return lv_obj_get_child(btn, 0);
}

/* ★ 全局防误触闸门。
 *
 * 问题：LVGL 的 CLICKED 只看「按下和抬起是否落在同一个对象」，
 * 不看中间移动了多远。于是手指按在按钮上往上滑想翻页时，
 * 滑动被识别的同时按钮还会收到一次 CLICKED ——
 * 表现就是"想上滑，结果开始录音 / 进了详情页 / 屏幕灭了"。
 *
 * 解法：给每个 ui_button_create 出来的按钮**先**注册这个闸门。
 *   - ui_gesture 在 PRESSING 阶段就把拖动标记置好，早于 CLICKED
 *   - lv_event_send 按注册顺序遍历，遇到 stop_processing 就 break
 *   - 所以先注册的闸门一 break，后面用户注册的 CLICKED 根本不会执行
 *
 * 一处修改，所有用 ui_button_create 建的按钮（语音/音乐/计时器/心率/
 * 血氧/关于…）全部免疫，不用每个回调自己查。 */
static void btn_drag_guard(lv_event_t *e)
{
    if (ui_gesture_was_drag())
      {
        lv_event_stop_processing(e);
      }
}

/* ★ 按底色亮度选标签颜色（相对亮度公式与 WCAG 2.x 一致）。
 *
 * 为什么需要它：这套浅色主题里按钮底色有三种来源 ——
 *   1. UI_SURFACE（#E8F0F5，浅）→ 深色文字 12.7:1
 *   2. UI_ACCENT （#0A6E93，深）→ 深色文字只有 2.55:1，必须白字（5.73:1）
 *   3. UI_COLOR_HEART/STEP/WEATHER（都压深过）→ 同理必须白字
 *
 * 原来 ui_button_create 无条件用 UI_TEXT_PRIMARY，在深底按钮上是
 * 2.2~2.9:1 的不可读组合。与其在每个调用点手写颜色（那样一定会漏，
 * 本仓 25 处 UI_SURFACE / 12 处语义色调用点），不如在这里按亮度判一次。
 *
 * 阈值 0.35 的取法：三个语义色 + accent 的亮度都在 0.111~0.180，
 * 所有浅色面（0xE8F0F5=0.847 / 0xD2E4EC=0.746）都在 0.7 以上，
 * 中间有很宽的空档，0.35 落在空档里，不会因为微调色值而翻转。 */
uint32_t ui_text_on_bg(uint32_t bg)
{
    double r = (double)((bg >> 16) & 0xff) / 255.0;
    double g = (double)((bg >> 8) & 0xff) / 255.0;
    double b = (double)(bg & 0xff) / 255.0;
    double lin[3];
    double lum;
    int i;

    for (i = 0; i < 3; i++)
      {
        double c = (i == 0) ? r : (i == 1) ? g : b;

        lin[i] = (c <= 0.03928) ? c / 12.92
                                : pow((c + 0.055) / 1.055, 2.4);
      }

    lum = 0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2];

    return lum < 0.35 ? 0xffffff : UI_TEXT_PRIMARY;
}

lv_obj_t *ui_button_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                           uint32_t color, const char *text,
                           lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *lbl;

    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, h / 2, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    /* ★ 按下态 2026-11-02 改：原来是 UI_SURFACE_HI（#dfe8f6 浅灰蓝）。
     * 那是深色主题时代的写法（浅色蒙在当前深底上 = 按下去变亮）。
     * 现在底色是浅的，按下去变亮反而**看不出按下**；改成按底色自动压深
     * 一档，任何底色都能看出反馈。 */
    lv_obj_set_style_bg_color(btn,
                              lv_color_darken(lv_color_hex(color), 40),
                              LV_STATE_PRESSED);

    lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(lbl,
                                lv_color_hex(ui_text_on_bg(color)), 0);
    lv_obj_center(lbl);

    /* 闸门必须先注册：lv_event_send 按顺序遍历，先到先执行 */
    lv_obj_add_event_cb(btn, btn_drag_guard, LV_EVENT_CLICKED, NULL);

    if (cb != NULL)
      {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
      }

    return btn;
}

void ui_button_set_text(lv_obj_t *btn, const char *text)
{
    lv_obj_t *lbl = btn_label(btn);

    if (lbl != NULL && text != NULL)
      {
        lv_label_set_text(lbl, text);
      }
}

lv_obj_t *ui_float_button_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                                 lv_coord_t x, lv_coord_t y, uint32_t color,
                                 const char *text, lv_event_cb_t cb,
                                 void *user_data)
{
    lv_obj_t *btn = ui_button_create(parent, w, h, color, text, cb, user_data);

    /* 绝对坐标：父容器带 pad_page，所以先减掉 padding 再 set_pos。
     * IGNORE_LAYOUT 必须加：否则父级 flex 会把按钮重新排进内容流。 */
    lv_obj_add_flag(btn, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_pos(btn, x - UI_PAD_PAGE, y - UI_PAD_PAGE);
    lv_obj_move_foreground(btn);
    return btn;
}

lv_obj_t *ui_back_button_create(lv_obj_t *parent, lv_event_cb_t cb,
                                void *user_data)
{
    return ui_float_button_create(parent, UI_BACK_W, UI_BACK_H,
                                  UI_SAFE_X, UI_SAFE_Y, UI_SURFACE,
                                  "返回", cb, user_data);
}

void ui_button_set_color(lv_obj_t *btn, uint32_t color)
{
    lv_obj_t *lbl = btn_label(btn);

    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    /* 换底色的同时把标签颜色一起换掉。
     * 2026-11-02 之前只改底色：计时器页在「开始(accent 深蓝)」与
     * 「停止(heart 深红)」之间切，两个都是深色所以侥幸没暴露；
     * 但只要有谁把底色切到浅色，写死的白字就变成 1.2:1 不可读。 */
    if (lbl != NULL)
      {
        lv_obj_set_style_text_color(lbl, lv_color_hex(ui_text_on_bg(color)), 0);
      }
}

/****************************************************************************
 * 设置行
 ****************************************************************************/

lv_obj_t *ui_row_create(lv_obj_t *parent, const char *name,
                        const char *value, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_t *k;
    lv_obj_t *v;

    lv_obj_set_size(row, lv_pct(100), 54);
    /* 不用卡片底，只用一条细分隔线：
     * Apple Watch 的设置列表就是"文字直接排在黑底上 + 行间细线"，
     * 每行一个圆角灰块会变成手机上的卡片列表，不像手表。 */
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(UI_DIVIDER), 0);
    lv_obj_set_style_pad_hor(row, 20, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_SURFACE_HI),
                              LV_STATE_PRESSED);

    k = lv_label_create(row);
    lv_label_set_text(k, name);
    lv_obj_set_style_text_font(k, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(k, lv_color_hex(UI_TEXT_PRIMARY), 0);

    v = lv_label_create(row);
    lv_label_set_text(v, value != NULL ? value : "");
    lv_obj_set_style_text_font(v, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(v, lv_color_hex(UI_TEXT_SECONDARY), 0);

    if (cb != NULL)
      {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
      }

    return row;
}

void ui_row_set_value(lv_obj_t *row, const char *value)
{
    lv_obj_t *v = lv_obj_get_child(row, 1);

    if (v != NULL && value != NULL)
      {
        lv_label_set_text(v, value);
      }
}

/****************************************************************************
 * 状态栏
 ****************************************************************************/

/* 电量标签固定是状态栏里的第 2 个子对象（第 1 个是标题/占位） */

lv_obj_t *ui_statusbar_create(lv_obj_t *parent, const char *title)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_t *left;
    lv_obj_t *right;
    lv_obj_t *lbl;
    lv_obj_t *batt;

    lv_obj_set_size(bar, lv_pct(100), UI_STATUSBAR_H);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 左：标题（可为空） */

    left = lv_obj_create(bar);
    lv_obj_set_size(left, LV_SIZE_CONTENT, UI_STATUSBAR_H);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(left, LV_SCROLLBAR_MODE_OFF);

    lbl = lv_label_create(left);
    lv_label_set_text(lbl, title != NULL ? title : "");
    lv_obj_set_style_text_font(lbl, UI_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_TEXT_SECONDARY), 0);
    lv_obj_center(lbl);

    /* 右：电量百分比 + 电池图标 */

    right = lv_obj_create(bar);
    /* 固定宽度：LV_SIZE_CONTENT 在"flex 容器里再套 flex 容器"时
     * 会被算成 0，标签直接没宽度显示不出来（实测）。
     * 90 够放 "100%"（约 40px）+ 8 间距 + 22px 图标。 */
    lv_obj_set_size(right, 90, UI_STATUSBAR_H);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, 0, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(right, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 8, 0);

    lbl = lv_label_create(right);
    /* 演示阶段固定 100%：本构建没有电池驱动，各页状态栏原来一直挂 "--"。
     * 与 watchface 的 DEMO_BATTERY_ALWAYS_100 保持一致。 */
    lv_label_set_text(lbl, "100%");
    lv_obj_set_style_text_font(lbl, UI_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_TEXT_SECONDARY), 0);

    batt = lv_image_create(right);
    lv_image_set_src(batt, ui_icon("battery"));
    lv_image_set_scale(batt, 200);   /* 28px 图标缩到约 22px */

    return bar;
}

lv_obj_t *ui_statusbar_battery_label(lv_obj_t *statusbar)
{
    lv_obj_t *right = lv_obj_get_child(statusbar, 1);

    if (right == NULL)
      {
        return NULL;
      }

    return lv_obj_get_child(right, 0);
}
