/****************************************************************************
 * sleep_page.c - 静止/休息页
 *
 * 只有加速度计，没有心率、没有体动分期，所以这里**不声称医疗级"睡眠"**。
 * 页面如实显示三件事：
 *
 *   昨夜休息 —— 夜间连续静止 >=45 分钟的体动估算（持久化在 rest.json）
 *   静止时长 —— 最后一次明显运动到现在（pedometer 里 bp 超阈值即算"在动"）
 *   活动时长 —— 本次开机以来有运动的总时间
 *
 * 夜间判定只看"连续静止 + 本地小时落在 22:00-08:00"，坐在椅子上不动
 * 也能满足。所以界面/工具/日志统一写"体动估算"，并明确标注"非医疗级"。
 * 做成"睡眠报告"就是在编数据，和之前的假心率是同一类问题。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "sleep_page.h"
#include "ui_gesture.h"
#include "ui_theme.h"
#include "ui_nav.h"            /* ui_nav_attach_back_swipe: 下滑返回 */
#include "health/pedometer.h"
#include "health/rest_track.h" /* 夜间休息（体动估算） */

static lv_obj_t *s_page;
static lv_obj_t *s_last_night_val;   /* 昨夜休息：主值（时长） */
static lv_obj_t *s_last_night_sub;   /* 昨夜休息：副行（真实起止钟点） */
static lv_obj_t *s_still_val;        /* 当前静止：主值（时长） */
static lv_obj_t *s_still_sub;        /* 当前静止：副行（从几点开始） */
static lv_obj_t *s_active_val;       /* 本次活动：开机以来活动时长 */
static lv_obj_t *s_state;
static lv_timer_t *s_tick;

static void refresh(void);

/* 标签可能存在也可能不在（创建失败 / 页面已删），统一走这个壳，
 * 免得每秒一次的定时刷新踩到空指针。 */
static void set_label(lv_obj_t *lbl, const char *txt)
{
    if (lbl != NULL && txt != NULL)
      {
        lv_label_set_text(lbl, txt);
      }
}

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;
    refresh();
}

/* "1小时23分" / "45分12秒" / "12秒" */
static void fmt_duration(uint32_t sec, char *buf, size_t sz)
{
    if (sec >= 3600u)
      {
        snprintf(buf, sz, "%u时%u分",
                 (unsigned)(sec / 3600u), (unsigned)((sec % 3600u) / 60u));
      }
    else if (sec >= 60u)
      {
        snprintf(buf, sz, "%u分%u秒",
                 (unsigned)(sec / 60u), (unsigned)(sec % 60u));
      }
    else
      {
        snprintf(buf, sz, "%u秒", (unsigned)sec);
      }
}

/* 夜间记录只精确到分钟：秒对"体动估算"没有意义。 */
static void fmt_night_duration(uint32_t sec, char *buf, size_t sz)
{
    if (sec >= 3600u)
      {
        snprintf(buf, sz, "%u时%u分（体动估算）",
                 (unsigned)(sec / 3600u), (unsigned)((sec % 3600u) / 60u));
      }
    else
      {
        snprintf(buf, sz, "%u分（体动估算）", (unsigned)(sec / 60u));
      }
}

/* 把 epoch 秒格式化成 "22:31"。时钟不可信时返回 false，调用方写 "--"
 * 或干脆不显示 —— 宁可空着，也不编一个时间出来。
 *
 * 年份检查与 last_night_recent() 同一判据：本板没校时时 RTC 会读到 2036
 * 这种离谱值，那种时间戳算出来的钟点没有意义。
 *
 * 注：本板 CONFIG_LIBC_LOCALTIME 没开，localtime_r() 实际等同 gmtime()，
 * 但 RTC 里存的就是本地时间（见 agent_config.h 的 AGENT_TIMEZONE 说明），
 * 所以拆出来的字段就是用户看到的墙钟。 */
static bool fmt_clock(uint32_t ts, char *buf, size_t sz)
{
    time_t t = (time_t)ts;
    struct tm tmv;
    int year;

    if (t <= (time_t)0 || localtime_r(&t, &tmv) == NULL)
      {
        return false;
      }

    year = tmv.tm_year + 1900;
    if (year < 2024 || year > 2027)
      {
        return false;
      }

    snprintf(buf, sz, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    return true;
}

/* 只把 24 小时内的记录标成"昨夜"。更早的记录仍然在盘上（工具能查），
 * 但页面上写"昨夜"就是编数据。 */
#define LAST_NIGHT_MAX_AGE_S (24u * 3600u)

static bool last_night_recent(uint32_t end)
{
    time_t now = time(NULL);
    struct tm tmv;
    int year;

    if (now <= (time_t)0 || localtime_r(&now, &tmv) == NULL)
      {
        return false;
      }

    year = tmv.tm_year + 1900;
    if (year < 2024 || year > 2027)
      {
        return false;
      }

    /* 时钟被往回拨 / 记录在未来：都不显示成"昨夜"。 */
    if ((uint64_t)now < (uint64_t)end)
      {
        return false;
      }

    return ((uint64_t)now - (uint64_t)end) <= LAST_NIGHT_MAX_AGE_S;
}

static void refresh_last_night(void)
{
    char buf[64];
    char t0[8];
    char t1[8];
    uint32_t start = 0;
    uint32_t end   = 0;
    uint32_t dur_s = 0;

    if (rest_track_get_last_night(&start, &end, &dur_s) != 1 ||
        !last_night_recent(end) || dur_s == 0)
      {
        /* 没有记录（昨夜没连续静止够 45 分钟）/ 记录超过 24 小时 /
         * 文件里 dur_s 为 0（坏记录）/ 时钟不可信：
         * 一律 "--" + 说明，不拿"0 分"冒充一夜。 */
        set_label(s_last_night_val, "--");
        set_label(s_last_night_sub, "暂无昨夜记录（需连续静止 45 分钟）");
        return;
      }

    fmt_night_duration(dur_s, buf, sizeof(buf));
    set_label(s_last_night_val, buf);

    /* 副行是这段记录**真实的起止钟点** "23:10 - 06:40" —— 直接来自
     * rest.json 的 start/end，不是按"应该睡多久"倒推出来的。
     * 任一端算不出钟点就留空：写 "--:--" 反而像"零点"。
     * 用 ASCII 连字符，字库里没有 U+2013 短破折号（会显示成方块）。 */
    if (fmt_clock(start, t0, sizeof(t0)) &&
        fmt_clock(end, t1, sizeof(t1)))
      {
        snprintf(buf, sizeof(buf), "%s - %s", t0, t1);
        set_label(s_last_night_sub, buf);
      }
    else
      {
        set_label(s_last_night_sub, "");
      }
}

static void refresh(void)
{
    char buf[40];
    char t0[8];
    uint32_t still;
    time_t now;
    int state;

    /* 即使 IMU 现在不可用，盘上的昨夜记录也应该显示出来。 */
    refresh_last_night();

    if (!pedometer_is_alive())
      {
        set_label(s_still_val, "--");
        set_label(s_still_sub, "--");
        set_label(s_active_val, "--");
        set_label(s_state, "IMU 不可用");
        return;
      }

    still = rest_track_current_quiet_s();

    fmt_duration(still, buf, sizeof(buf));
    set_label(s_still_val, buf);

    /* 副行 "自 22:31 起"：安静段的起点 = 现在 - 已静止秒数。
     * 刚动过（still==0，起点就是"现在"，没有意义）、系统时间还没
     * 校（now<=0）、或者还原出来的时间戳离谱，一律 "--"。 */
    now = time(NULL);
    if (still == 0 || now <= (time_t)still ||
        !fmt_clock((uint32_t)((uint64_t)now - (uint64_t)still),
                   t0, sizeof(t0)))
      {
        set_label(s_still_sub, "--");
      }
    else
      {
        snprintf(buf, sizeof(buf), "自 %s 起", t0);
        set_label(s_still_sub, buf);
      }

    fmt_duration(pedometer_get_active_seconds(), buf, sizeof(buf));
    set_label(s_active_val, buf);

    state = rest_track_get_state();
    if (state == REST_STATE_NIGHT)
      {
        set_label(s_state, "夜间休息");
      }
    else if (state == REST_STATE_QUIET)
      {
        set_label(s_state, "静止中");
      }
    else
      {
        set_label(s_state, "清醒");
      }
}

/* 一张 350x120 的卡片：标题（18px 次要色）+ 主值（24px）+ 副行（18px）。
 *
 * 为什么拆成卡：原来是三块"透明块 + 居中文字"，全页没有视觉分组，
 * "昨夜休息"和"已静止"两个 24px 数字上下贴在一起，容易看串。
 * 数字 label 的指针仍由 val_out / sub_out 交回调用方，刷新逻辑不变。
 *
 * 字号：标题和副行一律 18px（cjk_18 是全集）；主值用 24px 子集 ——
 * "时分（体动估算）"都在子集里，但"昨/夜/当"不在（见
 * assets/ui_font_cjk_24.c），所以这两处文案绝不能提到 24px。 */
static void make_block(lv_obj_t *parent, const char *title,
                       lv_obj_t **val_out, lv_obj_t **sub_out)
{
    lv_obj_t *card = ui_card_create(parent, 350, 120);
    lv_obj_t *name;
    lv_obj_t *val;
    lv_obj_t *sub;

    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_row(card, 2, 0);

    name = lv_label_create(card);
    lv_label_set_text(name, title);
    lv_obj_set_style_text_font(name, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(UI_TEXT_TERTIARY), 0);

    val = lv_label_create(card);
    lv_obj_set_style_text_font(val, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(val, lv_color_hex(UI_TEXT_PRIMARY), 0);
    lv_label_set_text(val, "--");

    sub = lv_label_create(card);
    lv_label_set_text(sub, "--");
    lv_obj_set_style_text_font(sub, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(UI_TEXT_SECONDARY), 0);

    if (val_out != NULL)
      {
        *val_out = val;
      }

    if (sub_out != NULL)
      {
        *sub_out = sub;
      }
}

lv_obj_t *sleep_page_create(void)
{
    lv_obj_t *note;

    if (s_page != NULL)
      {
        return s_page;
      }

    s_page = lv_obj_create(lv_screen_active());
    ui_page_init(s_page);
    lv_obj_set_flex_flow(s_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_page, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 2026-11-03：本板默认主题给 flex 行距 10px(PAD_SMALL)。按 0 行距
     * 算的高度账会溢出 28px、最后一行被屏幕下沿裁掉（host 实测 437..457
     * vs 屏高 450）。这里显式清零，行距全部由各块自己的 margin 控制。 */
    lv_obj_set_style_pad_row(s_page, 0, 0);

    (void)ui_statusbar_create(s_page, "休息");

    /* 卡 1：昨夜休息（体动估算）。没有数据 / 记录超过 24 小时都显示 --，
     * 副行是记录的真实起止钟点。 */
    make_block(s_page, "昨夜休息", &s_last_night_val, &s_last_night_sub);

    note = lv_label_create(s_page);
    lv_obj_set_style_text_font(note, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(note, lv_color_hex(UI_TEXT_TERTIARY), 0);
    lv_obj_set_style_margin_top(note, UI_GAP_S, 0);
    lv_label_set_text(note, "基于体动估算，非医疗级");

    /* 卡 2：当前静止。副行 "自 22:31 起" 是这段安静的真实起点。 */
    make_block(s_page, "当前静止", &s_still_val, &s_still_sub);

    /* 本次活动：本次开机以来**有运动**的总时长，和上面两张卡的"静止"
     * 是相反的量，所以不并进卡片，单列一行。数据源和其它页一致
     * （pedometer_get_active_seconds），刷新在 refresh() 里。 */
    {
        lv_obj_t *row = ui_row_create(s_page, "本次活动", "--", NULL, NULL);

        s_active_val = (row != NULL) ? lv_obj_get_child(row, 1) : NULL;
    }

    s_state = lv_label_create(s_page);
    lv_obj_set_style_text_font(s_state, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_state, lv_color_hex(UI_TEXT_SECONDARY), 0);
    lv_obj_set_style_margin_top(s_state, UI_GAP_M, 0);
    lv_label_set_text(s_state, "");

    /* ★ 同运动页：原来传 NULL 只是维护手势状态，本页没有返回按钮，
     *   等于滑不出去。改用统一的下滑返回。 */
    ui_nav_attach_back_swipe(s_page);

    /* 幂等：页面可能是第一次打开，也可能是开机时已经启动过。 */
    (void)rest_track_start();

    refresh();
    s_tick = lv_timer_create(refresh_cb, 1000, NULL);

    return s_page;
}

void sleep_page_delete(void)
{
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

    /* 页面对象已经没了，这些 label 指针必须一起清掉：
     * 下一帧万一还有 refresh()（例如 nav 删页与定时器不同步），
     * set_label() 会拿它们当有效对象去写。 */
    s_last_night_val = NULL;
    s_last_night_sub = NULL;
    s_still_val      = NULL;
    s_still_sub      = NULL;
    s_active_val     = NULL;
    s_state          = NULL;
}
