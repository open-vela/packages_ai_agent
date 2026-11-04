/****************************************************************************
 * timer_page.c - 计时器页（见 timer_page.h 的设计说明）
 *
 * 倒计时用「绝对到期时刻」而不是「每秒减一」：
 *   这台机器 render 一圈 20~50ms（随 LVGL 负载变化），用累减会越来越慢，
 *   实测 5 分钟能差出好几秒。改成记一个到期 tick，每次刷新都算
 *   (到期 - 现在)，误差不累积。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include "timer_page.h"
#include "ui_gesture.h"      /* 挂手势：零清防误触状态 */
#include "ui_nav.h"          /* ui_nav_attach_back_swipe：下滑返回 */
#include "ui_theme.h"

#define TAG "[TimerPage]"

#define PRESET_COUNT 5

static const int s_preset_min[PRESET_COUNT] = { 1, 3, 5, 10, 15 };

static lv_obj_t *s_page;
static lv_obj_t *s_time_label;
static lv_obj_t *s_hint_label;
static lv_obj_t *s_start_btn;
static lv_obj_t *s_preset_btn[PRESET_COUNT];
static lv_timer_t *s_tick;

static int      s_preset_idx = 2;      /* 默认 5 分钟 */
static int      s_preset_painted = -1;  /* 屏幕上已画出的选中档；-1 = 还没画 */
static bool     s_running;
static uint32_t s_deadline;            /* 到期时的 lv_tick，仅在 s_running 时有效 */
static int      s_remaining_s;         /* 未运行时表示当前选中的时长 */

/****************************************************************************
 * 显示
 ****************************************************************************/

/* 提示行：文字 + 颜色一起设，避免"时间到"用着上一状态的灰色。 */
static void set_hint(const char *text, uint32_t color)
{
    if (s_hint_label == NULL)
      {
        return;
      }

    lv_label_set_text(s_hint_label, text);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(color), 0);
}

/* 到点/恢复整页底色。
 *
 * ★ 原实现改的是 lv_screen_active()，但每个页面都是不透明整屏对象（ui_page_init
 *   给 page 铺了 bg_opa=COVER），屏幕底色被页面盖住，**完全看不见**。
 *   现在改成改 s_page 自己；这里用浅一档的 UI_SURFACE_HI 而不是警示红：
 *   红底会让状态栏/提示行的深色字对比度掉到 2.5:1 左右，"时间到"反而更难读。 */
static void set_page_alert(bool on)
{
    if (s_page == NULL)
      {
        return;
      }

    lv_obj_set_style_bg_color(s_page,
                              lv_color_hex(on ? UI_SURFACE_HI : UI_BG_BLACK),
                              0);
}

/* 预设按钮选中态：5 个按钮同色时用户不知道当前选了哪档。
 * 只在选中档真的变化时改样式 —— refresh_display() 每秒都会走一遍，
 * 无条件 set_style 会让 5 个按钮每秒重绘一次。 */
static void refresh_preset_colors(void)
{
    int i;

    if (s_preset_painted == s_preset_idx)
      {
        return;
      }

    for (i = 0; i < PRESET_COUNT; i++)
      {
        if (s_preset_btn[i] != NULL)
          {
            /* 选中用主色（深底 -> 白字由 ui_button_set_color 自动换），
             * 未选中用卡片底色（浅底 -> 深字），不靠位置猜。 */
            ui_button_set_color(s_preset_btn[i],
                                i == s_preset_idx ? UI_ACCENT : UI_SURFACE);
          }
      }

    s_preset_painted = s_preset_idx;
}

static void fmt(int sec, char *buf, size_t sz)
{
    if (sec < 0)
      {
        sec = 0;
      }

    snprintf(buf, sz, "%02d:%02d", sec / 60, sec % 60);
}

static void refresh_display(void)
{
    char buf[16];

    /* 只有 MM:SS —— UI_FONT_TIME(sans 88) 只有数字和 ".-/:"，
     * 往里放中文会画成方块（"时间到"因此只在 18px 的 hint 里说）。 */
    fmt(s_remaining_s, buf, sizeof(buf));
    lv_label_set_text(s_time_label, buf);

    if (s_running)
      {
        lv_obj_set_style_text_color(s_time_label,
                                    lv_color_hex(UI_ACCENT), 0);
        ui_button_set_text(s_start_btn, "停止");
        ui_button_set_color(s_start_btn, UI_COLOR_WARNING);
      }
    else
      {
        lv_obj_set_style_text_color(s_time_label,
                                    lv_color_hex(UI_TEXT_PRIMARY), 0);
        ui_button_set_text(s_start_btn, "开始");
        ui_button_set_color(s_start_btn, UI_ACCENT);
      }

    refresh_preset_colors();
}

/****************************************************************************
 * 到点
 ****************************************************************************/

static void on_finished(void)
{
    char buf[16];

    s_running = false;
    s_remaining_s = s_preset_min[s_preset_idx] * 60;

    /* 大字只放 00:00；"时间到"放在 18px 的 hint 上并换成警示色。
     * 这是页面内**看得见**的到点反馈：原实现改 lv_screen_active() 背景，
     * 被不透明的页面整个盖住，屏上什么都不会变。 */
    fmt(0, buf, sizeof(buf));
    lv_label_set_text(s_time_label, buf);
    lv_obj_set_style_text_color(s_time_label,
                                lv_color_hex(UI_COLOR_WARNING), 0);
    set_hint("时间到", UI_COLOR_WARNING);
    set_page_alert(true);

    /* 不调 refresh_display()：它会把刚设好的"时间到"高亮重新刷成常态。
     * 按钮单独回到"可以再开始"的样子。 */
    if (s_start_btn != NULL)
      {
        ui_button_set_text(s_start_btn, "开始");
        ui_button_set_color(s_start_btn, UI_ACCENT);
      }

    syslog(LOG_INFO, "%s finished (%d min)\n",
           TAG, s_preset_min[s_preset_idx]);
}

/****************************************************************************
 * 1Hz 刷新
 ****************************************************************************/

static void tick_cb(lv_timer_t *t)
{
    uint32_t now;
    int left;

    (void)t;

    if (!s_running)
      {
        return;
      }

    now = lv_tick_get();

    /* 到期时刻减去当前，误差不累积（减法结果 <= 0 即到点） */
    if ((int32_t)(s_deadline - now) <= 0)
      {
        on_finished();
        return;
      }

    left = (int)((s_deadline - now) / 1000u);
    if (left != s_remaining_s)
      {
        s_remaining_s = left;
        refresh_display();
      }
}

/****************************************************************************
 * 事件
 ****************************************************************************/

static void on_start_clicked(lv_event_t *e)
{
    (void)e;

    if (s_running)
      {
        /* 停止 = 回到预备状态，保留当前选中的时长 */
        s_running = false;
        s_remaining_s = s_preset_min[s_preset_idx] * 60;
        set_hint("已停止", UI_TEXT_TERTIARY);
        set_page_alert(false);
        refresh_display();
        return;
      }

    s_remaining_s = s_preset_min[s_preset_idx] * 60;
    s_deadline = lv_tick_get() + (uint32_t)s_remaining_s * 1000u;
    s_running = true;

    set_hint("计时中…", UI_TEXT_TERTIARY);
    set_page_alert(false);
    refresh_display();
}

static void on_preset_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);

    if (idx < 0 || idx >= PRESET_COUNT)
      {
        return;
      }

    /* 运行中改预设没有意义，先停掉再换 */
    s_running = false;
    s_preset_idx = idx;
    s_remaining_s = s_preset_min[idx] * 60;

    set_hint("点开始计时", UI_TEXT_TERTIARY);
    set_page_alert(false);
    refresh_display();
}

/****************************************************************************
 * 公开接口
 ****************************************************************************/

lv_obj_t *timer_page_create(void)
{
    lv_obj_t *statusbar;
    lv_obj_t *row;
    char buf[8];

    if (s_page != NULL)
      {
        return s_page;
      }

    s_page = lv_obj_create(lv_screen_active());
    ui_page_init(s_page);
    lv_obj_set_flex_flow(s_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_page, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    statusbar = ui_statusbar_create(s_page, "计时器");
    (void)statusbar;

    /* 大号倒计时：88px 纯数字字体，越小越难在余光里读出来 */
    s_time_label = lv_label_create(s_page);
    lv_obj_set_style_text_font(s_time_label, UI_FONT_TIME, 0);
    lv_obj_set_style_margin_top(s_time_label, UI_GAP_L, 0);

    /* 预设按钮行 */
    row = lv_obj_create(s_page);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_margin_top(row, UI_GAP_L, 0);

    for (int i = 0; i < PRESET_COUNT; i++)
      {
        snprintf(buf, sizeof(buf), "%d", s_preset_min[i]);

        /* 64x64：56 低于 UI_TAP_MIN(60)，圆角屏边缘容易点空。
         * 选中态在 refresh_display() 里统一上色。 */
        s_preset_btn[i] = ui_button_create(row, 64, 64, UI_SURFACE, buf,
                                            on_preset_clicked,
                                            (void *)(intptr_t)i);
      }

    /* 开始/停止 */
    s_start_btn = ui_button_create(s_page, 160, 64, UI_ACCENT, "开始",
                                   on_start_clicked, NULL);
    lv_obj_set_style_margin_top(s_start_btn, UI_GAP_L, 0);

    /* 提示 */
    s_hint_label = lv_label_create(s_page);
    lv_obj_set_style_text_font(s_hint_label, UI_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_hint_label,
                                lv_color_hex(UI_TEXT_TERTIARY), 0);
    lv_obj_set_style_margin_top(s_hint_label, UI_GAP_M, 0);

    s_remaining_s = s_preset_min[s_preset_idx] * 60;
    s_running = false;
    s_preset_painted = -1;    /* 强制把 5 个预设按钮的选中态画一遍 */
    set_hint("点开始计时", UI_TEXT_TERTIARY);
    refresh_display();

    s_tick = lv_timer_create(tick_cb, 1000, NULL);

    /* ★ 2026-10-25 修正：同音乐页——原来传 NULL 只维护防误触状态，
     * 本页没有返回按钮，滑不出去。改用统一的下滑返回。 */
    ui_nav_attach_back_swipe(s_page);

    printf("[TimerPage] created\n");
    return s_page;
}

void timer_page_delete(void)
{
    /* 定时器必须先删：它每秒回调，页面对象删掉之后再碰 s_time_label
     * 这些指针就是 use-after-free。 */
    if (s_tick != NULL)
      {
        lv_timer_delete(s_tick);
        s_tick = NULL;
      }

    if (s_page != NULL)
      {
        lv_obj_delete(s_page);
        s_page = NULL;
      }

    s_time_label = NULL;
    s_hint_label = NULL;
    s_start_btn  = NULL;
    s_running    = false;
    s_preset_painted = -1;

    for (int i = 0; i < PRESET_COUNT; i++)
      {
        s_preset_btn[i] = NULL;
      }
}