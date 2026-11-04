/****************************************************************************
 * about_page.c - 关于页（深色主题，与其它页统一）
 *
 *   ┌──────────────────────────┐
 *   │ [返回]        ...        │  浮动返回键 84x60 @ (24,52)
 *   │           关于            │  24px 标题
 *   │      HerSen AI Agent     │  产品名（卡片上方）
 *   │  ┌────────────────────┐  │
 *   │  │ 版本  <构建版本>    │  │
 *   │  │ 平台  SF32LB52     │  │  信息卡 350x150，三行 18px
 *   │  │ 队伍  Team 181     │  │
 *   │  └────────────────────┘  │
 *   │   openvela AI 硬件大赛    │  页脚
 *   └──────────────────────────┘
 *
 * 旧版问题是字号 16px（低于 watchOS 最小可读字号），配色用的
 * 0x88ccff/0xcccccc 不在主题令牌里。这里全部改成令牌。
 *
 * 2026-11-03：版本号不再写死 "v1.0.0" —— 那个字符串从第一版起就没更新过，
 * 用户报障时没法确认手上跑的是哪一版。改成编译期宏 AGENT_BUILD_VERSION
 * （与开机日志同一来源）。信息行的标签与取值都用 18px：cjk_24 子集只有
 * 532 个字，"固"这类字不在里面，24px 会渲染成方块。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdio.h>

#include "about_page.h"
#include "agent_config.h"   /* AGENT_BUILD_VERSION：构建期版本，别再写死 v1.0.0 */
#include "ui_nav.h"         /* ui_nav_attach_back_swipe（之前是隐式声明） */
#include "ui_theme.h"

/****************************************************************************
 * 私有数据
 ****************************************************************************/

static lv_obj_t *s_page;
static about_back_callback_t s_back_callback;

/****************************************************************************
 * 事件
 ****************************************************************************/

static void on_back_clicked(lv_event_t *e)
{
    (void)e;

    if (s_back_callback != NULL)
      {
        s_back_callback();
      }
}

/****************************************************************************
 * 信息行
 ****************************************************************************/

static void info_row(lv_obj_t *parent, const char *key, const char *value)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), 44);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 标签和取值都用 18px：cjk_18 是全集；24px 是 532 字子集，
     * "固"（以及 values 里将来可能出现的字）不一定在里面。 */
    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_font(k, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(k, lv_color_hex(UI_TEXT_SECONDARY), 0);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, value);
    lv_obj_set_style_text_font(v, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(v, lv_color_hex(UI_TEXT_PRIMARY), 0);
}

/****************************************************************************
 * 公开接口
 ****************************************************************************/

lv_obj_t *about_page_create(void)
{
    lv_obj_t *title;
    lv_obj_t *name;
    lv_obj_t *card;
    lv_obj_t *foot;

    printf("[About] creating, version=%s\n", AGENT_BUILD_VERSION);

    s_page = lv_obj_create(lv_scr_act());
    ui_page_init(s_page);

    /* 不额外套白面板：各页自己的卡片/列表行已经是白色，
     * 直接跑在壁纸上即可。多套一层会把可用宽高再缩一圈。 */
    lv_obj_set_flex_flow(s_page, LV_FLEX_FLOW_COLUMN);
    /* 纵向居中：标题 / 产品名 / 信息卡 / 页脚作为一整组放屏幕中段。
     * 左上角是浮动返回键（占 y=52..112），标题居中后与它没有交叠。 */
    lv_obj_set_flex_align(s_page, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_page, 4, 0);

    title = lv_label_create(s_page);
    lv_label_set_text(title, "关于");
    lv_obj_set_style_text_font(title, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_TEXT_PRIMARY), 0);
    lv_obj_set_style_margin_bottom(title, 10, 0);

    /* 产品名在卡片上方，单独一块（Apple Watch 的关于页也是
     * "名称一行 + 明细一块"，层级要比明细高）。 */
    name = lv_label_create(s_page);
    lv_label_set_text(name, "HerSen AI Agent");
    lv_obj_set_style_text_font(name, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(UI_ACCENT), 0);
    lv_obj_set_style_margin_bottom(name, 14, 0);

    /* 三条信息装进一张卡：原来是三条裸行直接铺在壁上，行与行之间
     * 没有边界，读起来像一段没排版的文字。卡宽 350 = 屏宽 - 2*20 内边距，
     * 高 150 = 3*44(行) + 2*6(内边距) + 余量。 */
    card = ui_card_create(s_page, 350, 150);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(card, 6, 0);

    /* ★ 版本号取自编译期宏 AGENT_BUILD_VERSION（agent_config.h）：
     * 写死的 "v1.0.0" 已经和实际固件对不上，用户报障时没法核对版本。
     * 格式是 __DATE__ " " __TIME__，例如 "Nov  3 2026 01:12:11"。 */
    info_row(card, "版本", AGENT_BUILD_VERSION);
    info_row(card, "平台", "SF32LB52");
    info_row(card, "队伍", "Team 181");

    foot = lv_label_create(s_page);
    lv_label_set_text(foot, "openvela AI 硬件大赛 2026");
    lv_obj_set_style_text_font(foot, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(foot, lv_color_hex(UI_TEXT_TERTIARY), 0);
    lv_obj_set_style_margin_top(foot, 18, 0);

    /* 返回键：原来是页面 flex 流里的最后一个子对象，于是被排到
     * 底部**居中**（不是左下），60x60 也压在圆角上。改成主题里的
     * 浮动返回键：84x60 @ (24,52)，左上安全区，且不参与布局。 */
    (void)ui_back_button_create(s_page, on_back_clicked, NULL);

    ui_nav_attach_back_swipe(s_page);

    return s_page;
}

void about_page_set_back_callback(about_back_callback_t callback)
{
    s_back_callback = callback;
}

void about_page_delete(void)
{
    if (s_page != NULL)
      {
        lv_obj_delete(s_page);
        s_page = NULL;
      }

    s_back_callback = NULL;
}
