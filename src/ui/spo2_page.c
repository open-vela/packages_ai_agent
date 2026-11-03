/****************************************************************************
 * spo2_page.c - 血氧页
 *
 *   ┌──────────────────────────┐
 *   │           血氧            │
 *   │                          │
 *   │           98             │  88px 大数字
 *   │           SpO2 %         │
 *   │                          │
 *   │       本次测量有效        │  状态（18px，随测量结果变）
 *   │                          │
 *   │      [ 开始测量 ]         │
 *   │  参考范围 95~100%（非诊断）│  固定参考（18px 三级色）
 *   └──────────────────────────┘
 *
 * 与心率页的区别：
 *   血氧不走 uORB —— 框架里没有 algo_spo2 话题。结果由 max30102.c 的
 *   全局变量 g_max30102_spo2_{pct,ok} 交出来，这里直接读那两个全局量。
 *   测量一次约 19 秒（MODE_SPO2 双通道采集），比心率慢，所以按钮文案
 *   和提示都写清楚时长，避免用户以为卡住。
 *
 * ★ 2026-11-03：状态和参考范围拆成两个 label。原来是同一个 label ——
 *   没数据时主值 "--" 却写着"正常范围 95~100"，会被读成"测出来正常"。
 *   现在状态只描述本次测量，参考范围固定在页面底部。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "spo2_page.h"
#include "ui_theme.h"
#include "ui_text_runtime.h"
#include "ui_icon_runtime.h"
#include "ui_nav.h"              /* ui_nav_attach_back_swipe: 下滑返回 */
#include "health/spo2_measure.h"

/****************************************************************************
 * 外部资源与传感器结果
 ****************************************************************************/


/* max30102.c 暴露的结果（框架没有 algo_spo2 话题） */

extern float g_max30102_spo2_pct;
extern int   g_max30102_spo2_ok;

/****************************************************************************
 * 私有数据
 ****************************************************************************/

static lv_obj_t *s_page;
static lv_obj_t *s_value_label;
static lv_obj_t *s_unit_label;
static lv_obj_t *s_status_label;   /* 主值下方：本次测量的状态（18px） */
static lv_obj_t *s_range_label;    /* 页面底部：固定参考范围（18px 三级色） */
static lv_obj_t *s_btn;
static lv_obj_t *s_btn_label;
static lv_timer_t *s_timer;

static int s_measuring;
static int s_last_pct;

/****************************************************************************
 * 状态文案
 ****************************************************************************/

/* 状态色只表达语义：成功=步数绿，失败/偏低=心率红，其余用次要灰。
 * 18px 字体（cjk_18）是全字集，这些汉字都在。 */
static void set_status(const char *text, uint32_t color)
{
    if (s_status_label == NULL)
      {
        return;
      }

    lv_label_set_text(s_status_label, text);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(color), 0);
}

/****************************************************************************
 * 事件
 ****************************************************************************/

/* 进入"测量中"外观。点按钮和"从宠物页起的轮次"共用，避免两处状态不一致。 */
static void show_measuring(void)
{
    lv_label_set_text(s_btn_label, "测量中…");
    /* 浅灰禁用态：标签必须跟着变深色，否则白字 1.23:1（2026-11-02） */
    lv_obj_set_style_bg_color(s_btn, lv_color_hex(UI_SURFACE_HI), 0);
    lv_obj_set_style_text_color(s_btn_label,
                                lv_color_hex(ui_text_on_bg(UI_SURFACE_HI)), 0);
    /* 状态只描述"本次测量"，参考范围在页面底部单独一行 */
    set_status("测量中…保持手臂静止", UI_TEXT_SECONDARY);
}

static void on_measure_clicked(lv_event_t *e)
{
    int rc;

    (void)e;

    if (s_measuring)
      {
        return;
      }

    s_measuring = 1;
    show_measuring();

    rc = spo2_measure_start();
    if (rc != 0 && rc != -EBUSY)
      {
        /* 线程没起来：立刻回落，别让页面永远停在"测量中"。
         * -EBUSY 表示已有一轮在跑，继续等它的结果即可。 */
        s_measuring = 0;
        lv_label_set_text(s_btn_label, "重新测量");
        lv_obj_set_style_bg_color(s_btn, lv_color_hex(UI_COLOR_STEP), 0);
        lv_obj_set_style_text_color(s_btn_label,
                                    lv_color_hex(ui_text_on_bg(UI_COLOR_STEP)),
                                    0);
        set_status("测量失败，请重试", UI_COLOR_HEART);
      }
}

/****************************************************************************
 * 刷新
 ****************************************************************************/

static void refresh_cb(lv_timer_t *timer)
{
    char buf[32];

    (void)timer;

    /* 从宠物页起的测量也要如实显示"测量中" —— 页面可能是测量进行中
     * 才被打开的，这时不能还写着"尚未测量"（2026-11-03）。 */
    if (!s_measuring && spo2_measure_busy())
      {
        s_measuring = 1;
        show_measuring();
      }

    /* 没测过 -> 显示占位符，不显示 0（0 会被误读成"测出来是 0"） */

    if (s_last_pct <= 0)
      {
        lv_label_set_text(s_value_label, "--");
      }
    else
      {
        snprintf(buf, sizeof(buf), "%d", s_last_pct);
        lv_label_set_text(s_value_label, buf);
      }

    /* 测量结束：收结果、恢复按钮。结果只写状态 label，
     * 绝不写参考范围 label（那是固定的非诊断参考）。 */

    if (s_measuring && !spo2_measure_busy())
      {
        s_measuring = 0;
        lv_label_set_text(s_btn_label, "重新测量");
        lv_obj_set_style_bg_color(s_btn, lv_color_hex(UI_COLOR_STEP), 0);
        lv_obj_set_style_text_color(s_btn_label,
                                    lv_color_hex(ui_text_on_bg(UI_COLOR_STEP)),
                                    0);

        if (g_max30102_spo2_ok && g_max30102_spo2_pct > 0.0f)
          {
            s_last_pct = (int)(g_max30102_spo2_pct + 0.5f);
            snprintf(buf, sizeof(buf), "%d", s_last_pct);
            lv_label_set_text(s_value_label, buf);

            if (s_last_pct >= 95)
              {
                set_status("本次测量有效", UI_COLOR_STEP);
              }
            else
              {
                set_status("偏低，建议复测", UI_COLOR_HEART);
              }
          }
        else
          {
            /* ★ 审计修复：失败后不能把上一次的旧值继续当成本次结果显示。
             * 清主值并在状态行说明失败。 */
            s_last_pct = 0;
            lv_label_set_text(s_value_label, "--");
            set_status("测量失败，请重试", UI_COLOR_HEART);
          }
      }
}

/****************************************************************************
 * 公开接口
 ****************************************************************************/

lv_obj_t *spo2_page_create(void)
{
    lv_obj_t *title;

    s_page = lv_obj_create(lv_scr_act());
    ui_page_init(s_page);

    /* 壁纸做背景，内容全部放在白色面板上以保证对比度 */
    /* 不额外套白面板：各页自己的卡片/列表行已经是白色，
     * 直接跑在壁纸上即可。多套一层会把可用宽高再缩一圈，
     * 3 列网格会退化成 2 列、纵向也会溢出。 */
    lv_obj_t *sheet = s_page;
    lv_obj_set_flex_flow(sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sheet, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(sheet, 6, 0);

    title = lv_label_create(sheet);
    lv_label_set_text(title, "血氧");
    lv_obj_set_style_text_font(title, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_TEXT_SECONDARY), 0);
    lv_obj_set_style_margin_top(title, 10, 0);

    /* 图标：给一屏文字一个视觉锚点 */

    lv_obj_t *icon = lv_image_create(s_page);
    lv_image_set_src(icon, ui_icon("app_spo2"));
    lv_obj_set_style_image_recolor(icon, lv_color_hex(UI_COLOR_STEP), 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_top(icon, 16, 0);

    s_value_label = lv_label_create(sheet);
    lv_label_set_text(s_value_label, "--");
    lv_obj_set_style_text_font(s_value_label, UI_FONT_TIME, 0);
    lv_obj_set_style_text_color(s_value_label, lv_color_hex(UI_TEXT_PRIMARY), 0);
    lv_obj_set_style_margin_top(s_value_label, -4, 0);

    s_unit_label = lv_label_create(sheet);
    lv_label_set_text(s_unit_label, "SpO2  %");
    lv_obj_set_style_text_font(s_unit_label, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_unit_label, lv_color_hex(UI_TEXT_SECONDARY), 0);
    lv_obj_set_style_margin_top(s_unit_label, -12, 0);

    /* 状态：只描述本次测量。初始必须是"尚未测量" —— 不能拿参考范围
     * 充当结果（2026-11-03 修的就是这个误导）。 */
    s_status_label = lv_label_create(sheet);
    lv_label_set_text(s_status_label, "尚未测量");
    lv_obj_set_style_text_font(s_status_label, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_status_label,
                                lv_color_hex(UI_TEXT_SECONDARY), 0);
    lv_obj_set_style_margin_top(s_status_label, 10, 0);

    s_btn = lv_button_create(s_page);
    lv_obj_set_size(s_btn, 200, 56);
    lv_obj_set_style_radius(s_btn, UI_RADIUS_PILL, 0);
    lv_obj_set_style_bg_color(s_btn, lv_color_hex(UI_COLOR_STEP), 0);
    lv_obj_set_style_bg_opa(s_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_btn, 0, 0);
    lv_obj_set_style_margin_top(s_btn, 16, 0);
    lv_obj_add_event_cb(s_btn, on_measure_clicked, LV_EVENT_CLICKED, NULL);

    s_btn_label = lv_label_create(s_btn);
    lv_label_set_text(s_btn_label, ui_str("spo2.measure", "开始测量"));
    lv_obj_set_style_text_font(s_btn_label, UI_FONT_BODY, 0);
    /* 深色语义底（UI_COLOR_STEP）必须白字，否则 2.74:1（2026-11-02） */
    lv_obj_set_style_text_color(s_btn_label,
                                lv_color_hex(ui_text_on_bg(UI_COLOR_STEP)), 0);
    lv_obj_center(s_btn_label);

    /* 参考范围固定钉在页面底部：它和"本次结果"是两件事，不随测量状态变。
     * IGNORE_LAYOUT 让它不参与上面的 flex 纵向排版。 */
    s_range_label = lv_label_create(s_page);
    lv_label_set_text(s_range_label, "参考范围 95~100%（非诊断）");
    lv_obj_set_style_text_font(s_range_label, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_range_label,
                                lv_color_hex(UI_TEXT_TERTIARY), 0);
    lv_obj_add_flag(s_range_label, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(s_range_label, LV_ALIGN_BOTTOM_MID, 0, -20);

    s_measuring = 0;
    s_last_pct = 0;

    /* 若之前已经测过，进来就显示上次的值（全局量只在本机运行期内有效） */

    if (g_max30102_spo2_ok && g_max30102_spo2_pct > 0.0f)
      {
        s_last_pct = (int)(g_max30102_spo2_pct + 0.5f);

        if (s_last_pct >= 95)
          {
            /* ★ 审计修复：这是本开机内上一次测量留下的全局值，不是本次
             * 测量结果 —— 进页面时只能标"上次测量"。 */
            set_status("上次测量（非本次）", UI_COLOR_STEP);
          }
        else
          {
            set_status("上次测量偏低，建议复测", UI_COLOR_HEART);
          }
      }

    refresh_cb(NULL);
    s_timer = lv_timer_create(refresh_cb, 500, NULL);

    printf("[SpO2] created\n");
    ui_nav_attach_back_swipe(s_page);

    return s_page;
}

void spo2_page_delete(void)
{
    if (s_timer != NULL)
      {
        lv_timer_delete(s_timer);
        s_timer = NULL;
      }

    if (s_page != NULL)
      {
        lv_obj_delete(s_page);
        s_page = NULL;
      }

    s_value_label = NULL;
    s_unit_label = NULL;
    s_status_label = NULL;
    s_range_label = NULL;
    s_btn = NULL;
    s_btn_label = NULL;
}
