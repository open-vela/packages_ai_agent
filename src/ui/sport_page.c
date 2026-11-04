/****************************************************************************
 * sport_page.c - 运动页
 *
 * 数据来源只有一处：health/pedometer.c。它是真实读 /dev/mpu0 的，
 * 25Hz 采样 → 0.7-3.5Hz 带通 → 峰值检测。
 *
 * 换算假设（写在这里，别让它变成隐藏的魔法数字）：
 *   步长 0.70 m   —— 成人步行的常见值；手表没有 GPS/身高输入，
 *                    只能用这个经验值，界面上标注"估算"。
 *   体重 60 kg    —— 同上。卡路里按 0.05 kcal/步/60kg 线性换算。
 *
 * 距离显示分两档（2026-11-03）：< 1000 m 用"米"（1 位小数），
 * >= 1000 m 用"公里"（2 位小数），单位标签跟着换。旧实现把折算出的
 * 厘米直接按 %u.%02u 当公里格式化，14 步显示成 9.80 km。
 *
 * IMU 不可用时，页面显示"不可用"，不显示 0 —— 0 步和"没数据"是两件事。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdio.h>
#include <string.h>

#include "sport_page.h"
#include "ui_gesture.h"
#include "ui_theme.h"
#include "ui_nav.h"            /* ui_nav_attach_back_swipe: 下滑返回 */
#include "health/pedometer.h"

#define STEP_LEN_M      0.70f
#define KCAL_PER_STEP   0.05f

static lv_obj_t *s_page;

static lv_obj_t *s_steps_val;
static lv_obj_t *s_cadence_val;
static lv_obj_t *s_dist_val;
static lv_obj_t *s_dist_unit;   /* 距离单位：米 / 公里，随值切换 */
static lv_obj_t *s_kcal_val;
static lv_obj_t *s_note;
static lv_timer_t *s_tick;

/****************************************************************************
 * 刷新
 ****************************************************************************/

static void refresh(void);

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;
    refresh();
}

static void refresh(void)
{
    char buf[32];
    char note[160];
    uint32_t steps;
    uint32_t cadence;

    if (!pedometer_is_alive())
      {
        lv_label_set_text(s_steps_val, "--");
        lv_label_set_text(s_cadence_val, "--");
        lv_label_set_text(s_dist_val, "--");
        lv_label_set_text(s_dist_unit, "");
        lv_label_set_text(s_kcal_val, "--");
        lv_label_set_text(s_note, "IMU 不可用");
        return;
      }

    steps   = pedometer_get_steps();
    cadence = pedometer_get_cadence();

    snprintf(buf, sizeof(buf), "%u", (unsigned)steps);
    lv_label_set_text(s_steps_val, buf);

    /* 静止 3 秒后计步器把步频归 0。这时的 0 不是"测得步频 0"，
     * 而是"没在走路、不估算"，所以显示 -- 并在脚注说明（2026-11-03）。 */
    if (steps == 0 || cadence == 0)
      {
        lv_label_set_text(s_cadence_val, "--");
      }
    else
      {
        snprintf(buf, sizeof(buf), "%u", (unsigned)cadence);
        lv_label_set_text(s_cadence_val, buf);
      }

    snprintf(note, sizeof(note),
             "距离/消耗为估算（步长 0.70m、体重 60kg）");

    /* 距离：pedometer 只给步数，这里按步长折算成厘米再做量纲换算。
     * 全程用整数运算，避开无 FPU 时浮点格式化的开销。
     *
     * ★ 2026-11-03 修正：原实现把厘米当公里（14 步 -> 9.80 km）。
     *   现在按真实量纲分档显示，单位标签一起换。 */
    {
        uint32_t cm = (uint32_t)(steps * STEP_LEN_M * 100.0f);

        if (steps > 0 && cm / steps > 200u)
          {
            /* 保险：平均步长 > 2 m 只可能是数据异常，
             * 宁可显示 -- 也不给一个假距离。 */
            lv_label_set_text(s_dist_val, "--");
            lv_label_set_text(s_dist_unit, "");
            strncat(note, " · 距离待校准",
                    sizeof(note) - strlen(note) - 1);
          }
        else if (cm < 100000u)   /* < 1000 m */
          {
            uint32_t dm = cm / 10u;   /* 分米：正好 1 位小数 */

            snprintf(buf, sizeof(buf), "%u.%u",
                     (unsigned)(dm / 10u), (unsigned)(dm % 10u));
            lv_label_set_text(s_dist_val, buf);
            lv_label_set_text(s_dist_unit, "米");
          }
        else                     /* >= 1000 m：公里，2 位小数 */
          {
            uint32_t km100 = cm / 1000u;   /* 公里 * 100 */

            snprintf(buf, sizeof(buf), "%u.%02u",
                     (unsigned)(km100 / 100u), (unsigned)(km100 % 100u));
            lv_label_set_text(s_dist_val, buf);
            lv_label_set_text(s_dist_unit, "公里");
          }
    }

    {
        uint32_t kcal10 = (uint32_t)(steps * KCAL_PER_STEP * 10.0f);

        snprintf(buf, sizeof(buf), "%u.%u",
                 (unsigned)(kcal10 / 10u), (unsigned)(kcal10 % 10u));
        lv_label_set_text(s_kcal_val, buf);
    }

    if (steps == 0 || cadence == 0)
      {
        strncat(note, " · 静止时不估算步频",
                sizeof(note) - strlen(note) - 1);
      }

    lv_label_set_text(s_note, note);
}

/****************************************************************************
 * 布局
 ****************************************************************************/

/* unit_out 非空时把单位标签一并交出去：距离的单位要随值在"米/公里"
 * 之间切换，不能像其它行那样写死。 */
static lv_obj_t *make_row(lv_obj_t *parent, const char *title,
                          const char *unit, lv_obj_t **val_out,
                          lv_obj_t **unit_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_t *name;
    lv_obj_t *val;
    lv_obj_t *u;

    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, UI_GAP_S, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    name = lv_label_create(row);
    lv_label_set_text(name, title);
    lv_obj_set_style_text_font(name, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(UI_TEXT_TERTIARY), 0);

    val = lv_label_create(row);
    lv_obj_set_style_text_font(val, UI_FONT_TITLE, 0);
    lv_label_set_text(val, "--");

    u = lv_label_create(row);
    lv_label_set_text(u, unit);
    lv_obj_set_style_text_font(u, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(u, lv_color_hex(UI_TEXT_TERTIARY), 0);

    *val_out = val;
    if (unit_out != NULL)
      {
        *unit_out = u;
      }

    return row;
}

/****************************************************************************
 * 公开接口
 ****************************************************************************/

lv_obj_t *sport_page_create(void)
{
    if (s_page != NULL)
      {
        return s_page;
      }

    s_page = lv_obj_create(lv_screen_active());
    ui_page_init(s_page);
    lv_obj_set_flex_flow(s_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_page, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    (void)ui_statusbar_create(s_page, "运动");

    make_row(s_page, "步数",   "步",    &s_steps_val,   NULL);
    make_row(s_page, "步频",   "步/分", &s_cadence_val, NULL);
    make_row(s_page, "距离",   "米",    &s_dist_val,    &s_dist_unit);
    make_row(s_page, "消耗",   "kcal",  &s_kcal_val,    NULL);

    s_note = lv_label_create(s_page);
    lv_obj_set_style_text_font(s_note, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_note, lv_color_hex(UI_TEXT_TERTIARY), 0);
    lv_obj_set_style_margin_top(s_note, UI_GAP_S, 0);
    /* 脚注会在"估算"基础上追加静止/待校准说明，必须限宽并居中换行 */
    lv_obj_set_width(s_note, lv_pct(100));
    lv_obj_set_style_text_align(s_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_note, "");

    /* 必须挂手势：否则 ui_gesture 的 s_drag 标志永远不清，
     * 全局按钮闸门会一直拦截点击（v39 踩过这个坑）。
     *
     * ★ 2026-10-24 修正：原来这里传 NULL 回调 —— 手势状态维护有了，
     *   但**本页没有任何返回按钮，页面就滑不出去**（用户实测反馈）。
     *   改用统一的 ui_nav_attach_back_swipe()，它内部就是
     *   ui_gesture_attach(page, on_back_gesture)：
     *   既保留 s_drag 状态维护，又把"下滑返回"接上。
     *   heart / spo2 / settings / about 一直是这么做的。 */
    ui_nav_attach_back_swipe(s_page);

    refresh();

    /* 1 秒刷一次。计步器内部 20Hz 采样，界面不需要那么快。 */
    s_tick = lv_timer_create(refresh_cb, 1000, NULL);

    return s_page;
}

void sport_page_delete(void)
{
    if (s_page != NULL)
      {
        lv_obj_delete(s_page);
        s_page = NULL;
      }

    if (s_tick != NULL)
      {
        lv_timer_delete(s_tick);
        s_tick = NULL;
      }
}
