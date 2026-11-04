/****************************************************************************
 * heart_page.c - 心率页
 *
 *   ┌──────────────────────────┐
 *   │ 85%  [▮▮▮]               │  statusbar
 *   ├──────────────────────────┤
 *   │           心率            │  24px 标题
 *   │                          │
 *   │           72             │  88px 大数字
 *   │           BPM            │  24px 单位
 *   │                          │
 *   │   ╱╲    ╱╲    ╱╲        │  心率曲线（最近 30 次采样）
 *   │  ╱  ╲__╱  ╲__╱  ╲       │
 *   │                          │
 *   │      [ 重新测量 ]         │  胶囊按钮
 *   └──────────────────────────┘
 *
 * 数据降级：没测过显示 "--"，不显示 0。
 * 测量是异步的（hr_measure 起独立线程，30~40 秒），所以按钮按下后
 * 显示"测量中…"，结果由 1 秒轮询取回。
 *
 * ★ 2026-11-03：只看 hr_measure_last_bpm() 会把失败轮残留的旧值当成本次
 *   结果（hr_measure.h:68-76 明确警告）。现在先看 hr_measure_last_ok()：
 *   失败轮保留上一次**有效**值并标注"上次有效值"，并显示那次测量的时间；
 *   从未成功过才显示 "--"。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "heart_page.h"
#include "ui_theme.h"
#include "ui_text_runtime.h"
#include "ui_icon_runtime.h"
#include "ui_nav.h"              /* ui_nav_attach_back_swipe: 下滑返回 */
#include "health/hr_measure.h"

/****************************************************************************
 * 外部资源
 ****************************************************************************/


/****************************************************************************
 * 私有数据
 ****************************************************************************/

#define HISTORY_LEN 30

static lv_obj_t *s_page;
static lv_obj_t *s_value_label;
static lv_obj_t *s_unit_label;
static lv_obj_t *s_bars;      /* 心率柱状条容器（用 lv_obj 画，不用 lv_chart） */
static lv_obj_t *s_bars_hint; /* 无数据时显示"暂无记录" */
static lv_obj_t *s_bar_obj[HISTORY_LEN];
static lv_obj_t *s_btn;
static lv_obj_t *s_btn_label;
static lv_obj_t *s_hint_label;
static lv_obj_t *s_last_measure_label;  /* "最近测量 HH:MM"（单位标签下一行） */
static lv_timer_t *s_timer;

static int s_history[HISTORY_LEN];
static int s_hist_count;
static int s_measuring;
/* 上一次已经推进曲线的 bpm（-1 = 还没推过）。文件级而不是 refresh_cb 里的
 * 函数静态：页面创建时要用预填的最新历史值初始化它，避免重复 push。 */
static int s_last_pushed = -1;
/* 最近一次**有效**测量的 bpm（0 = 从未成功）。失败轮不能覆盖它 ——
 * 主值显示的是"上次有效值"，不是本轮失败值。 */
static int s_last_valid_bpm;
/* 上面那次有效测量的墙上时间（time(NULL) 截断到 uint32；0 = 时间未知）。
 * 预填时取历史里最后一条的 ts，成功推进曲线时取当前时间。 */
static uint32_t s_last_ts;

/****************************************************************************
 * 曲线
 ****************************************************************************/

/**
 * 刷新柱状条。
 *
 * 为什么不用 lv_chart：本板 Kconfig 里 CONFIG_LV_USE_CHART 和 LV_USE_LINE
 * 都没开，用图表控件会编译不过。而打开它们要多链接一个控件模块。
 * 心率曲线本质上就是"最近 N 次的相对高度"，用 lv_obj 的矩形条完全够用，
 * 而且不增加任何可选依赖。
 */
static void chart_update(void)
{
    int i;
    int maxv = 0;
    int minv = 999;

    if (s_bars == NULL)
      {
        return;
      }

    for (i = 0; i < s_hist_count; i++)
      {
        if (s_history[i] > maxv) maxv = s_history[i];
        if (s_history[i] < minv) minv = s_history[i];
      }

    if (maxv - minv < 20)
      {
        /* 数值波动太小：给个固定量程，否则柱子不是全高就是全矮 */
        maxv = minv + 20;
      }

    for (i = 0; i < HISTORY_LEN; i++)
      {
        if (s_bar_obj[i] == NULL)
          {
            continue;
          }

        if (i < s_hist_count)
          {
            int h = 8 + (s_history[i] - minv) * 52 / (maxv - minv);

            lv_obj_set_height(s_bar_obj[i], h);
            lv_obj_clear_flag(s_bar_obj[i], LV_OBJ_FLAG_HIDDEN);
          }
        else
          {
            lv_obj_add_flag(s_bar_obj[i], LV_OBJ_FLAG_HIDDEN);
          }
      }

    if (s_bars_hint != NULL)
      {
        if (s_hist_count > 0)
          {
            lv_obj_add_flag(s_bars_hint, LV_OBJ_FLAG_HIDDEN);
          }
        else
          {
            lv_obj_clear_flag(s_bars_hint, LV_OBJ_FLAG_HIDDEN);
          }
      }
}

static void chart_push(int bpm)
{
    if (s_hist_count < HISTORY_LEN)
      {
        s_history[s_hist_count++] = bpm;
      }
    else
      {
        memmove(&s_history[0], &s_history[1],
                sizeof(s_history[0]) * (HISTORY_LEN - 1));
        s_history[HISTORY_LEN - 1] = bpm;
      }

    chart_update();
}

/**
 * 用持久化的心率历史预填曲线（页面创建时调一次）。
 *
 * snapshot 返回「最近 N 条、旧 -> 新」，N 最多 HISTORY_LEN —— 正好是曲线
 * 容量，历史更长时只保留最近一段。这里只填数据，绘图仍走 chart_update()，
 * 不改动 1 秒刷新 / chart_push 的既有逻辑。
 *
 * 预填的最后一条 = 当前最新值：把去重游标对齐到它，页面第一拍 refresh_cb
 * 就不会把同一次测量再 push 一遍。
 */
static void chart_preload(void)
{
    uint32_t ts[HISTORY_LEN];
    int bpm[HISTORY_LEN];
    int n;
    int i;

    s_hist_count = 0;
    s_last_pushed = -1;
    s_last_valid_bpm = 0;
    s_last_ts = 0;

    n = hr_measure_history_snapshot(ts, bpm, HISTORY_LEN);
    if (n > 0)
      {
        for (i = 0; i < n; i++)
          {
            s_history[s_hist_count++] = bpm[i];
          }

        s_last_pushed = bpm[n - 1];
        /* 历史最后一条 = 最近一次有效测量。主值与"最近测量"时间都以它
         * 为准，冷启动时大数字也能追溯到具体哪次测量（2026-11-03）。 */
        s_last_valid_bpm = bpm[n - 1];
        s_last_ts = ts[n - 1];
      }

    chart_update();
}

/****************************************************************************
 * 测量
 ****************************************************************************/

static void on_measure_clicked(lv_event_t *e)
{
    int rc;

    (void)e;

    if (s_measuring)
      {
        return;
      }

    s_measuring = 1;
    lv_label_set_text(s_btn_label, "测量中…");
    /* 按钮切成浅灰（禁用态观感）：底色变浅，标签必须跟着变深色，
     * 否则白字落在 #D2E4EC 上是 1.23:1（2026-11-02 浅色主题后新增的坑）。 */
    lv_obj_set_style_bg_color(s_btn, lv_color_hex(UI_SURFACE_HI), 0);
    lv_obj_set_style_text_color(s_btn_label,
                                lv_color_hex(ui_text_on_bg(UI_SURFACE_HI)), 0);
    /* 出一份结果要等滤波器和连续 RR 确认，通常二十几秒；旧文案
     * 8~15 秒与实际不符，用户会以为卡死。按产品要求写 30~40 秒
     * （2026-11-03）。 */
    lv_label_set_text(s_hint_label, "测量中…保持手臂静止 30~40 秒");

    /* hr_measure_start() 起独立线程，立刻返回。
     * 结果由 1 秒轮询 refresh_cb 取回；这里不做同步等待 ——
     * LVGL 是单线程，阻塞会冻住整个界面。 */

    rc = hr_measure_start();
    if (rc != 0 && rc != -EBUSY)
      {
        /* 线程没起来（-ENOMEM 等）：立刻回落，别让按钮永远停在"测量中"。
         * -EBUSY 表示后台已有一轮在测，继续等它的结果即可。 */
        s_measuring = 0;
        lv_label_set_text(s_btn_label, "重新测量");
        lv_obj_set_style_bg_color(s_btn, lv_color_hex(UI_COLOR_HEART), 0);
        lv_obj_set_style_text_color(s_btn_label,
                                    lv_color_hex(ui_text_on_bg(UI_COLOR_HEART)),
                                    0);
        lv_label_set_text(s_hint_label, "测量失败，请重试");
      }
}

/****************************************************************************
 * 定时刷新（从 hr_measure 取最新值）
 ****************************************************************************/

static void refresh_cb(lv_timer_t *timer)
{
    char buf[64];
    bool ok;
    int bpm;
    int stale;
    time_t ts;
    struct tm tmv;

    (void)timer;

    /* 先处理"测量结束"这一拍：本次成功结果在本拍就能推进曲线，
     * 不用再等下一秒（2026-11-03）。 */
    if (s_measuring && !hr_measure_is_running())
      {
        s_measuring = 0;
        lv_label_set_text(s_btn_label, "重新测量");
        lv_obj_set_style_bg_color(s_btn, lv_color_hex(UI_COLOR_HEART), 0);
        lv_obj_set_style_text_color(s_btn_label,
                                    lv_color_hex(ui_text_on_bg(UI_COLOR_HEART)),
                                    0);

        /* 失败轮（信号弱 / 超时）不能显示成"测量完成" */
        if (hr_measure_last_ok())
          {
            int done_bpm = hr_measure_last_bpm();

            if (done_bpm > 0)
              {
                /* ★ 2026-11-03 审计修复：复测得到与上次相同的 bpm 时，
                 * 下面 branch 因 bpm == s_last_pushed 不会更新 s_last_ts，
                 * "最近测量 HH:MM" 会一直停在上一次不同值的那次。
                 * 测量真正结束的这一拍无条件刷新时间（值相同也算一次新测量）。 */
                s_last_valid_bpm = done_bpm;
                s_last_ts = (uint32_t)time(NULL);
              }

            lv_label_set_text(s_hint_label,
                              ui_str("heart.hint", "点按钮开始测量"));
          }
        else
          {
            lv_label_set_text(s_hint_label, "信号弱，请重试");
          }
      }

    /* 主值：只有 last_ok==true 的 bpm 才是本次有效结果。
     * 失败轮里 last_bpm 可能残留旧值，直接显示等于撒谎。 */
    ok    = hr_measure_last_ok();
    bpm   = hr_measure_last_bpm();
    stale = 0;

    if (ok && bpm > 0)
      {
        snprintf(buf, sizeof(buf), "%d", bpm);
        lv_label_set_text(s_value_label, buf);
        s_last_valid_bpm = bpm;

        if (!s_measuring)
          {
            /* 静止状态下来的新值，推进曲线。s_last_pushed 已由预填的
             * 最新历史值初始化，同一条不会重复进曲线。 */
            if (bpm != s_last_pushed)
              {
                chart_push(bpm);
                s_last_pushed = bpm;
                /* 曲线推进 = 刚刚拿到一次新的有效测量 */
                s_last_ts = (uint32_t)time(NULL);
              }
          }
      }
    else if (s_last_valid_bpm > 0)
      {
        /* 没有新的有效值，但以前测到过：保留大数字并标注"上次有效值" */
        snprintf(buf, sizeof(buf), "%d", s_last_valid_bpm);
        lv_label_set_text(s_value_label, buf);
        stale = 1;
      }
    else
      {
        lv_label_set_text(s_value_label, "--");
      }

    /* 单位标签下一行：最近一次有效测量发生在什么时候。
     * 时间戳未知（0）就不显示 —— 不编造"刚刚测过"。 */
    ts = (time_t)s_last_ts;
    if (ts > (time_t)0 && localtime_r(&ts, &tmv) != NULL)
      {
        if (stale)
          {
            snprintf(buf, sizeof(buf), "上次有效值 · 最近测量 %02d:%02d",
                     tmv.tm_hour, tmv.tm_min);
          }
        else
          {
            snprintf(buf, sizeof(buf), "最近测量 %02d:%02d",
                     tmv.tm_hour, tmv.tm_min);
          }

        lv_label_set_text(s_last_measure_label, buf);
      }
    else
      {
        lv_label_set_text(s_last_measure_label, stale ? "上次有效值" : "");
      }
}

/****************************************************************************
 * 公开接口
 ****************************************************************************/

lv_obj_t *heart_page_create(void)
{
    lv_obj_t *title;
    lv_obj_t *btn;

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
    lv_obj_set_style_pad_row(sheet, 8, 0);

    title = lv_label_create(sheet);
    lv_label_set_text(title, "心率");
    lv_obj_set_style_text_font(title, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_TEXT_SECONDARY), 0);
    lv_obj_set_style_margin_top(title, 8, 0);

    /* 大数字 */

    s_value_label = lv_label_create(sheet);
    lv_label_set_text(s_value_label, "--");
    lv_obj_set_style_text_font(s_value_label, UI_FONT_TIME, 0);
    lv_obj_set_style_text_color(s_value_label, lv_color_hex(UI_TEXT_PRIMARY), 0);
    lv_obj_set_style_margin_top(s_value_label, -6, 0);

    s_unit_label = lv_label_create(sheet);
    lv_label_set_text(s_unit_label, "BPM");
    lv_obj_set_style_text_font(s_unit_label, UI_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_unit_label, lv_color_hex(UI_TEXT_SECONDARY), 0);
    lv_obj_set_style_margin_top(s_unit_label, -10, 0);

    /* 最近测量时间：单位标签下的一行 18px。
     * 时间戳要么来自持久化历史（chart_preload），要么来自本次成功推进
     * 曲线的时刻；两者都没有就留空 —— 不编造"刚刚测过"。 */
    s_last_measure_label = lv_label_create(sheet);
    lv_label_set_text(s_last_measure_label, "");
    lv_obj_set_style_text_font(s_last_measure_label, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_last_measure_label,
                                lv_color_hex(UI_TEXT_TERTIARY), 0);

    /* 心率柱状条（自绘图元，见 chart_update 的说明） */

    s_bars = lv_obj_create(sheet);
    lv_obj_set_size(s_bars, 320, 84);
    /* 不要灰底方块：Apple 用留白和灰字表达"还没有数据" */
    lv_obj_set_style_bg_opa(s_bars, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_bars, 0, 0);
    lv_obj_set_style_radius(s_bars, 0, 0);
    lv_obj_set_style_pad_all(s_bars, 4, 0);
    lv_obj_clear_flag(s_bars, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_bars, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_bars, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_margin_top(s_bars, 8, 0);

    {
        int i;

        for (i = 0; i < HISTORY_LEN; i++)
          {
            lv_obj_t *bar = lv_obj_create(s_bars);

            lv_obj_set_size(bar, 6, 8);
            lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_HEART), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(bar, 0, 0);
            lv_obj_set_style_radius(bar, 3, 0);
            lv_obj_set_style_pad_all(bar, 0, 0);
            lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
            s_bar_obj[i] = bar;
          }
    }

    /* 一条记录都没有时，柱状区是空的，看起来像画错了。
     * 这里盖一行提示，让"空"这件事有明确含义。 */
    s_bars_hint = lv_label_create(s_bars);
    lv_label_set_text(s_bars_hint, "暂无记录");
    lv_obj_set_style_text_font(s_bars_hint, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_bars_hint, lv_color_hex(UI_TEXT_TERTIARY), 0);
    lv_obj_center(s_bars_hint);

    /* 按钮 */

    btn = lv_button_create(s_page);
    s_btn = btn;
    lv_obj_set_size(btn, 200, 56);
    lv_obj_set_style_radius(btn, UI_RADIUS_PILL, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_HEART), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_margin_top(btn, 12, 0);
    lv_obj_add_event_cb(btn, on_measure_clicked, LV_EVENT_CLICKED, NULL);

    s_btn_label = lv_label_create(btn);
    lv_label_set_text(s_btn_label, ui_str("heart.measure", "开始测量"));
    lv_obj_set_style_text_font(s_btn_label, UI_FONT_BODY, 0);
    /* 底色是深色语义色（UI_COLOR_HEART），标签必须白字，否则 2.58:1。
     * 用 ui_text_on_bg 而不是写死，这样改主题色时不会漏（2026-11-02）。 */
    lv_obj_set_style_text_color(s_btn_label,
                                lv_color_hex(ui_text_on_bg(UI_COLOR_HEART)), 0);
    lv_obj_center(s_btn_label);

    s_hint_label = lv_label_create(sheet);
    lv_label_set_text(s_hint_label, ui_str("heart.hint", "点按钮开始测量"));
    lv_obj_set_style_text_font(s_hint_label, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(UI_TEXT_TERTIARY), 0);
    lv_obj_set_style_margin_top(s_hint_label, 6, 0);

    /* 定时刷新 */

    s_measuring = 0;
    chart_preload();   /* 先拿持久化历史把柱子填上，再进 1 秒刷新 */
    refresh_cb(NULL);
    s_timer = lv_timer_create(refresh_cb, 1000, NULL);

    printf("[Heart] created\n");
    ui_nav_attach_back_swipe(s_page);

    return s_page;
}

void heart_page_delete(void)
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
    s_bars = NULL;
    s_bars_hint = NULL;
    s_btn = NULL;
    s_btn_label = NULL;
    s_hint_label = NULL;
    s_last_measure_label = NULL;
}
