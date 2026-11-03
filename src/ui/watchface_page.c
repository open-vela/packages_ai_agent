/****************************************************************************
 * watchface_page.c - 表盘
 *
 * 布局（对齐 Apple Watch / 华为 GT 的通用结构）：
 *
 *   ┌──────────────────────────────┐
 *   │                       🔋 85% │  状态栏 32px
 *   │                              │
 *   │            14:45             │  88px 主视觉
 *   │        周五 10月19日          │  18px 次要
 *   │                              │
 *   │  ┌──────┐ ┌──────┐ ┌──────┐ │
 *   │  │  ❤   │ │  👟  │ │  ☀   │ │  108x104 数据卡
 *   │  │  72  │ │ 8432 │ │  15° │ │  图标 / 数值 / 标签
 *   │  │ 心率  │ │ 步数  │ │ 天气 │ │
 *   │  └──────┘ └──────┘ └──────┘ │
 *   │                              │
 *   │        上滑进入应用           │  提示
 *   └──────────────────────────────┘
 *
 * 为什么这样排：
 *   1. 时间占最大字号且垂直居中偏上 —— 抬腕第一眼（HIG: glanceable）
 *   2. 电量固定在右上角 —— 手表最需要随时可见的状态，主流都放这
 *   3. 三张数据卡等宽等距 —— 8pt 栅格，横向扫描一次读完
 *   4. 每张卡可点进详情 —— 没有"更多"这种二级入口，发现成本最低
 *
 * 数据降级：心率要测才有值、天气要联网才有值、步数依赖 fitness 算法。
 * 任何一个取不到都显示 "--"，不显示 0 —— 0 会被误读成"测出来是 0"。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>   /* syslog: 定时器/页面创建诊断 */
#include <time.h>

#include "watchface_page.h"
#include "ui_theme.h"
#include "ui_text_runtime.h"
#include "ui_icon_runtime.h"
#include "ui_nav.h"
#include "ui/expression.h"        /* 当前小云表情（批次 3） */
#include "health/hr_measure.h"   /* hr_measure_last_bpm: 表盘心率卡回填 */
#include "ui_gesture.h"          /* 统一手势识别 + 防误触 */
#include "voice/voice_channel.h"

#ifdef CONFIG_UORB
#include <uORB/uORB.h>
#include <system/state.h>
#endif

#if defined(CONFIG_UORB) && defined(CONFIG_FITNESS_ALGO)
#include <fitness/algo_pedometer.h>
#endif

/****************************************************************************
 * 外部资源
 ****************************************************************************/


/****************************************************************************
 * 私有数据
 ****************************************************************************/

#define CARD_W   104
#define CARD_H   60

/* 表盘表情的显示缩放：1.25x = 320/256。
 *
 * 为什么不是 1:1：资产是 120px 长边（批次 3），1:1 显示时在这块 390x450
 * 的屏上偏小，撑不起"表盘主视觉"。垂直预算实测（见下）：
 *   页面内容 410 = 状态栏 32 + 中部(mid) + 卡片 60 + 底部一行 64
 *   -> mid = 254；mid 里 日期 21-4 + 时间 88 = 105
 *   -> 表情可用 149；1.25x 下最高的一款 120x103 -> 150x129 ✓
 * 1.5x 会得到 155x139，也刚好放得下但没余量，且放大更糊，故取 1.25x。 */
#define WATCH_EXPR_SCALE 320

static lv_obj_t *s_page;
static lv_obj_t *s_time_label;
static lv_obj_t *s_date_label;
static lv_obj_t *s_batt_label;
static lv_obj_t *s_hr_label;
static lv_obj_t *s_step_label;
static lv_obj_t *s_expression_img;  /* 当前小云表情（批次 3） */
static int       s_expression_shown = -1; /* 已显示的索引，-1 = 还没设过 */
static lv_timer_t *s_timer;

/* 语音按钮：物理 KEY2 目前是唯一入口，但用户不会知道侧键能说话 ——
 * 屏幕上有按钮才符合直觉。这个按钮把已有的 voice_channel_start/stop
 * 直接接出来，VAD 自动断句的逻辑完全复用，不新增任何语音逻辑。 */
static lv_obj_t *s_voice_btn;
static lv_obj_t *s_voice_label;
static lv_timer_t *s_voice_timer;

static watchface_swipe_cb_t s_swipe_cb;

/* 点击 / 滑动消歧 */
#define CLICK_MAX_MOVE  20
/* 触摸状态已全部交给 ui_gesture，这里不再自己记坐标 */

/* on_page_pressed / on_page_released 已由 ui_gesture 接管 */

/****************************************************************************
 * 数据读取
 ****************************************************************************/

#ifdef CONFIG_UORB
static int orb_read_once(const struct orb_metadata *meta, void *buf)
{
    int fd = orb_subscribe(meta);
    int ret;

    if (fd < 0)
      {
        return -1;
      }

    ret = orb_copy(meta, fd, buf);
    orb_unsubscribe(fd);
    return ret;
}
#endif

/* 模拟器没有 uORB，用它给一组有真实感的数据，方便看设计效果。
 * 真机构建里 UI_SIM 不会定义，走下面的 uORB 分支。 */
#ifdef UI_SIM
extern int  ui_sim_battery(void);
extern long ui_sim_steps(void);
#endif

/* 演示阶段固定电量：本构建没有电池驱动/电量发布者（orb_read_once 恒失败），
 * 原来显示 "--" 在演示时不直观。按产品要求演示期间固定 100%。
 * 真机接上电池驱动后，把下面的 DEMO_BATTERY_ALWAYS_100 删掉即可恢复真实值。 */
#define DEMO_BATTERY_ALWAYS_100 1

static int read_battery(void)
{
#if DEMO_BATTERY_ALWAYS_100
    return 100;
#else
#ifdef UI_SIM
    return ui_sim_battery();
#elif defined(CONFIG_UORB)
    struct battery_state bs;

    memset(&bs, 0, sizeof(bs));
    if (orb_read_once(ORB_ID(battery_state), &bs) < 0)
      {
        return -1;
      }

    return (int)bs.level;
#else
    return -1;
#endif /* UI_SIM / CONFIG_UORB */
#endif /* !DEMO_BATTERY_ALWAYS_100 */
}

static long read_steps(void)
{
#ifdef UI_SIM
    return ui_sim_steps();
#elif defined(CONFIG_UORB) && defined(CONFIG_FITNESS_ALGO)
    struct algo_pedometer pd;

    memset(&pd, 0, sizeof(pd));
    if (orb_read_once(ORB_ID(algo_pedometer), &pd) < 0)
      {
        return -1;
      }

    return (long)pd.count;
#else
    return -1;
#endif
}

/****************************************************************************
 * 数据卡
 ****************************************************************************/

static void on_card_clicked(lv_event_t *e)
{
    ui_page_t page = (ui_page_t)(lv_intptr_t)lv_event_get_user_data(e);

    /* 从卡片上开始上下滑时，LVGL 仍会补一个 CLICKED（不可滚动的对象
     * 不做 drag 取消）。用统一手势模块的拖动标记过滤，
     * 避免"想翻页却进了详情页"。 */
    if (ui_gesture_was_drag())
      {
        return;
      }

    ui_nav_goto(page);
}

/* 数据条：横排「图标 + 数值」。
 *
 * 对照真机 Apple Watch 表盘（reference/apple_face.png）：
 *   Apple 的 complication 是"小图标 + 一个数字"，没有中文标签，
 *   也不画卡片底。之前我们是"竖排三行 + 白色大卡片"，
 *   三块白方块在浅色壁纸上非常抢眼，把时间压下去了。
 *   改成紧凑的横排条，视觉重量轻很多，仍然保留图标表达含义。
 */
static lv_obj_t *data_card_create(lv_obj_t *parent, const lv_image_dsc_t *icon,
                                  uint32_t tint, const char *label,
                                  lv_obj_t **value_out, ui_page_t target)
{
    lv_obj_t *card = ui_card_create(parent, CARD_W, CARD_H);
    lv_obj_t *img;
    lv_obj_t *val;

    (void)label;

    /* 去掉卡片底：Apple 的 complication 是"图标 + 数字"直接放在黑底上，
     * 不画任何方块。深灰方块在纯黑底上像一块补丁。 */
    lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(card, 0, 0);

    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(card, 8, 0);

    img = lv_image_create(card);
    lv_image_set_src(img, icon);
    lv_image_set_scale(img, 180);

    /* 只给内置线条图标上色；设计师的彩色图保持原样 */
    if (ui_icon_is_baked(icon))
      {
        lv_obj_set_style_image_recolor(img, lv_color_hex(tint), 0);
        lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
      }

    val = lv_label_create(card);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_font(val, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(val, lv_color_hex(UI_TEXT_PRIMARY), 0);

    if (value_out != NULL)
      {
        *value_out = val;
      }

    /* 卡片可点 + 自己上报 press/release（不依赖事件冒泡，实测不可靠） */

    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    /* 不再单独注册 PRESSED/RELEASED —— ui_gesture 已经给整棵树打开了冒泡。
     * 只保留 CLICKED，并在里面查"这次是不是拖动"。 */
    lv_obj_add_event_cb(card, on_card_clicked, LV_EVENT_CLICKED,
                        (void *)(lv_intptr_t)target);

    return card;
}

/****************************************************************************
 * 刷新
 ****************************************************************************/

static void refresh_cb(lv_timer_t *timer)
{
    time_t now;
    struct tm tmv;
    static const char *const wday_cn[7] =
      {
        "周日", "周一", "周二", "周三", "周四", "周五", "周六"
      };
    char buf[64];
    int batt;
    long steps;

    (void)timer;

    /* 诊断：确认这个 1Hz 定时器到底有没有在跑。 */
    {
      static unsigned int s_cb_count;

      s_cb_count++;
      if (s_cb_count <= 3 || (s_cb_count % 30) == 0)
        {
          syslog(LOG_INFO, "[watchface] refresh_cb #%u\n", s_cb_count);
        }
    }

    time(&now);
    localtime_r(&now, &tmv);

    /* 未校时降级：RTC 未同步时可能是 1970 或 2036（pedometer.c 有记录），
     * 直接显示会变成“1970-01-01 / 00:00”，在主屏上非常刺眼。
     * 2024..2027 是本项目可信年份区间，区间外按“时间未同步”处理。 */
    if (tmv.tm_year + 1900 < 2024 || tmv.tm_year + 1900 > 2027)
      {
        if (strcmp(lv_label_get_text(s_time_label), "--:--") != 0)
          {
            lv_label_set_text(s_time_label, "--:--");
          }

        if (strcmp(lv_label_get_text(s_date_label), "时间未同步") != 0)
          {
            lv_label_set_text(s_date_label, "时间未同步");
          }
      }
    else
      {
        snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        if (strcmp(lv_label_get_text(s_time_label), buf) != 0)
          {
            lv_label_set_text(s_time_label, buf);
          }

        snprintf(buf, sizeof(buf), "%s  %d月%d日",
                 wday_cn[tmv.tm_wday % 7], tmv.tm_mon + 1, tmv.tm_mday);
        if (strcmp(lv_label_get_text(s_date_label), buf) != 0)
          {
            lv_label_set_text(s_date_label, buf);
          }
      }

    batt = read_battery();
    if (s_batt_label != NULL)
      {
        if (batt < 0)
          {
            lv_label_set_text(s_batt_label, "--");
          }
        else
          {
            snprintf(buf, sizeof(buf), "%d%%", batt);
            lv_label_set_text(s_batt_label, buf);
          }
      }

    steps = read_steps();
    if (steps < 0)
      {
        lv_label_set_text(s_step_label, "--");
      }
    else if (steps >= 10000)
      {
        snprintf(buf, sizeof(buf), "%ld.%ldk", steps / 1000,
                 (steps % 1000) / 100);
        lv_label_set_text(s_step_label, buf);
      }
    else
      {
        snprintf(buf, sizeof(buf), "%ld", steps);
        lv_label_set_text(s_step_label, buf);
      }

    /* 天气卡已于 2026-11-02（批次 3）移入应用入口：表盘只保留心率/步数
     * 两张卡，天气走「应用入口 -> 天气」。原来这段读 weather_page 模块级
     * 缓存的逻辑一并删除 —— 表盘不再显示温度。 */

    /* 当前小云表情：表情页/设置里改了之后，表盘是常驻页不会重建，
     * 这里每秒对齐一次；只有索引真的变了才 set_src（避免每秒重绘图片）。
     * ★ 只在这里读 expression_current()，它第一次调用才 open 文件，
     *   之后全走内存态，无 flash 读。 */
    if (s_expression_img != NULL)
      {
        int expr = expression_current();

        if (expr != s_expression_shown)
          {
            s_expression_shown = expr;
            lv_image_set_src(s_expression_img, expression_get(expr)->img);
          }
      }

    /* 心率卡：读 hr_measure 留下的最近一次成功值（volatile 直读，无锁）。
     * ★ 2026-10-25 修正：原来这里从不更新（初始化写死 "--" 后再没人碰），
     *   心率页测完也不会回填表盘 —— 团队审计 A 发现的"恒为 --"问题。
     * last_bpm()==0 表示从没测到过，维持 "--"（不显示 0：0 会被误读成
     * "测出来是 0"，与步数卡的降级策略一致）。 */
    {
      int bpm = hr_measure_last_bpm();

      if (bpm > 0)
        {
          snprintf(buf, sizeof(buf), "%d", bpm);
        }
      else
        {
          snprintf(buf, sizeof(buf), "--");
        }

      if (strcmp(lv_label_get_text(s_hr_label), buf) != 0)
        {
          lv_label_set_text(s_hr_label, buf);
        }
    }
}

/****************************************************************************
 * 手势
 ****************************************************************************/

/* 上滑进应用列表。
 *
 * 不用 LV_EVENT_GESTURE：那个事件由滚动子系统产生，要求对象可滚动，
 * 而表盘明确关掉了滚动，所以永远不会触发。这里直接比按下/抬起坐标。
 * 阈值 60px ≈ 屏高 13%：太小吃误触，太大又要划很久（原来 tileview
 * 是整屏翻页，就是用户抱怨"滑得很大才切换"的原因）。
 */

/* 统一手势：这里只关心上滑。
 *
 * 原来自己实现了一套「按下记点 → 抬起做差 → 落在 -60..-400 才算」，两个问题：
 *   1) 上限 400 —— 从屏幕底部一路滑到顶部（>400px）反而不触发
 *   2) 没有速度判据 —— 快速轻扫要滑满 60px 才认，手感很钝
 * 判定逻辑已搬到 ui_gesture.c，这里只保留"上滑做什么"。 */
static void on_watchface_swipe(ui_gesture_dir_t direction)
{
    if (direction != UI_GESTURE_SWIPE_UP)
      {
        return;
      }

    if (s_swipe_cb != NULL)
      {
        s_swipe_cb();
      }
    else
      {
        ui_nav_goto(UI_PAGE_APPLIST);
      }
}

/****************************************************************************
 * 语音按钮
 ****************************************************************************/

/* ★ v108 P0-1：voice_channel_start/stop 可能阻塞（等 TTS/speak_lock、ASR 收尾
 * 网络 POST），绝不能在 LVGL 渲染线程里同步调用 —— 之前会导致 UI 冻死 3s 触发
 * [stall] → 停止喂狗 → 20s 后 IWDT 复位（"卡住黑屏重启"）。与云朵按钮一致，
 * 丢到一次性 pthread 里执行。 */
static void *voice_click_thread(void *arg)
{
    if ((intptr_t)arg != 0)
      {
        voice_channel_start();
      }
    else
      {
        voice_channel_stop();
      }

    return NULL;
}

/* 点一下开始录音，再点一下结束。录音中由 VAD 自动断句，和 KEY2 同一套逻辑。 */
static void on_voice_clicked(lv_event_t *e)
{
    (void)e;

    /* ★ 防误触：手指按在这个按钮上往上滑想翻页时，LVGL 仍会补一个
     * CLICKED（不可滚动的对象不做 drag 取消），于是"想上滑"变成"开始录音"。
     * ui_gesture 在 PRESSING 阶段就把拖动标记置好了，这里查一下就能免疫。 */
    if (ui_gesture_was_drag())
      {
        return;
      }

    /* v114：200ms 去抖，消灭连点导致的 start/stop 竞态 */
    {
        static uint32_t s_last_click_ms;
        struct timespec ts;
        uint32_t now;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
        if (s_last_click_ms != 0 && (uint32_t)(now - s_last_click_ms) < 1000)
          {
            syslog(LOG_INFO, "[watchface] voice click debounced\n");
            return;
          }
        s_last_click_ms = now;
    }

    /* v108 P0-1：录音启停放到独立线程，LVGL 线程只更新按钮文案 */
    {
        int want_stop = voice_channel_is_recording();
        pthread_t tid;

        if (pthread_create(&tid, NULL, voice_click_thread,
                           (void *)(intptr_t)(want_stop ? 0 : 1)) == 0)
          {
            pthread_detach(tid);
          }
        else
          {
            syslog(LOG_WARNING, "[watchface] voice thread create failed\n");
          }

        if (s_voice_label != NULL)
          {
            lv_label_set_text(s_voice_label, want_stop ? "说话" : "停止");
          }
    }
}

/* 500ms 一次：录音可能被 VAD 自动结束，按钮文字要跟着回退。 */
static void voice_state_cb(lv_timer_t *t)
{
    const char *want;

    (void)t;

    if (s_voice_label == NULL)
      {
        return;
      }

    want = voice_channel_is_recording() ? "停止" : "说话";

    if (strcmp(lv_label_get_text(s_voice_label), want) != 0)
      {
        lv_label_set_text(s_voice_label, want);
      }
}

/****************************************************************************
 * 公开接口
 ****************************************************************************/

lv_obj_t *watchface_page_create(void)
{
    lv_obj_t *bar;
    lv_obj_t *mid;
    lv_obj_t *cloud;
    lv_obj_t *cards;
    lv_obj_t *hint;

    s_page = lv_obj_create(lv_scr_act());
    ui_page_init(s_page);

    lv_obj_set_flex_flow(s_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_page, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 1. 状态栏（只有电量，没有标题） */

    bar = ui_statusbar_create(s_page, NULL);
    s_batt_label = ui_statusbar_battery_label(bar);

    /* 2. 时间 + 日期 + 当前小云表情（Lumina 表盘的三个视觉层）
     *
     * 2026-11-02（批次 3）改版：原来是「88px 时间 + 日期」居中，下面再从
     * 三张数据卡里挤信息。参考的 Lumina 设计稿把「时间」和「小云」作为
     * 表盘的第一视觉层，数据卡降为两张（心率/步数），天气挪进应用入口
     * —— 一块 390x450 的屏同时塞时间、日期、三张卡、语音按钮、提示，
     * 视觉上必然糊。取舍记录在 docs_ble/92 号方案第 7 节决策 3。 */

    mid = lv_obj_create(s_page);
    lv_obj_set_width(mid, lv_pct(100));
    lv_obj_set_flex_grow(mid, 1);
    lv_obj_set_style_bg_opa(mid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mid, 0, 0);
    lv_obj_set_style_pad_all(mid, 0, 0);
    lv_obj_clear_flag(mid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(mid, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_time_label = lv_label_create(mid);
    lv_label_set_text(s_time_label, "--:--");
    lv_obj_set_style_text_font(s_time_label, UI_FONT_TIME, 0);
    lv_obj_set_style_text_color(s_time_label, lv_color_hex(UI_TEXT_PRIMARY), 0);

    s_date_label = lv_label_create(mid);
    lv_label_set_text(s_date_label, "---");
    lv_obj_set_style_text_font(s_date_label, UI_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_date_label, lv_color_hex(UI_TEXT_SECONDARY), 0);
    lv_obj_set_style_margin_top(s_date_label, -4, 0);

    /* 当前小云表情。
     *
     * 注意 lv_image 的行为：**lv_obj_set_size 对图片只裁剪、不缩放**
     * （LVGL 9.1 lv_image.c:580 默认 ALIGN_CENTER），要缩放必须
     * lv_image_set_scale()。docs_ble/91c P0-1 在 14 页预览里实测了 9 处
     * 这个坑。这里按 WATCH_EXPR_SCALE = 320（1.25x）缩放，配 120px 源图
     * 得到最大 150x129 的显示尺寸 —— 1.25x 是按垂直预算算出来的
     * （见 WATCH_EXPR_SCALE 定义处的推导），不是随手取的值。
     *
     * 表情切走后本页不重建（表盘是常驻页），由 refresh_cb 每秒对齐一次
     * —— 只在索引真的变了才换图，避免每秒重设 src 触发重绘。 */

    cloud = lv_image_create(mid);
    lv_image_set_src(cloud, expression_get(expression_current())->img);
    lv_image_set_scale(cloud, WATCH_EXPR_SCALE);
    lv_obj_set_style_margin_top(cloud, 2, 0);
    lv_obj_set_style_margin_bottom(cloud, 2, 0);
    s_expression_img = cloud;

    /* 3. 两张数据卡（心率 / 步数）。天气卡已移入应用入口（决策 3）。 */

    cards = lv_obj_create(s_page);
    lv_obj_set_size(cards, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(cards, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cards, 0, 0);
    lv_obj_set_style_pad_all(cards, 0, 0);
    lv_obj_clear_flag(cards, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(cards, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(cards, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cards, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    data_card_create(cards, ui_icon("heart"), UI_COLOR_HEART, "心率",
                     &s_hr_label, UI_PAGE_HEART);
    /* ★ 2026-10-25：卡片的跳转目标要与内容对得上（审计 A 指出过）。
     * 点步数看运动、点心率看心率。天气卡进应用入口后由 applist 直达。 */
    data_card_create(cards, ui_icon("step"), UI_COLOR_STEP, "步数",
                     &s_step_label, UI_PAGE_SPORT);

    /* 心率默认是 "--"：要测才有值 */

    lv_label_set_text(s_hr_label, "--");

    /* 3.5 底部一行：语音按钮 + 手势提示
     *
     * 2026-11-02（批次 3）改版：原来是「160x60 语音按钮」独占一行、
     * 下面再一行提示文字，垂直占掉 121px —— 加上时间(88)、日期(21)、
     * 卡片(60)、状态栏(32)，中部只剩 ~197px，表情图连 1:1 都放不下
     * （最高一款要 103px，预算只有 98px）。
     *
     * 合成一行后（按钮缩到 132x44，提示文字放它右边）只占 64px，
     * 中部回到 254px，表情 1.25x（最高 129px）才放得下。
     *
     * 语音入口不能删：之前语音只能靠物理 KEY2（PA11）触发，屏幕上没有
     * 任何提示，用户不可能猜到侧键能说话。这里仍然调同一个
     * voice_channel_start()，VAD 自动断句、"说话/停止"文字自动切换。 */

    {
        lv_obj_t *bottom = lv_obj_create(s_page);

        lv_obj_set_size(bottom, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(bottom, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(bottom, 0, 0);
        lv_obj_set_style_pad_all(bottom, 0, 0);
        lv_obj_set_style_pad_column(bottom, UI_GAP_M, 0);
        lv_obj_clear_flag(bottom, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(bottom, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(bottom, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(bottom, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_margin_top(bottom, UI_GAP_M, 0);
        /* 圆角屏底部安全区 40：原来只留 8px，按钮底边到 422 离屏底 28px，
         * 圆角会吃掉一点。这里 margin 20 -> 按钮底边 410。 */
        lv_obj_set_style_margin_bottom(bottom, 20, 0);

        s_voice_btn = ui_button_create(bottom, 132, 44, UI_ACCENT, "说话",
                                       on_voice_clicked, NULL);
        s_voice_label = lv_obj_get_child(s_voice_btn, 0);
        s_voice_timer = lv_timer_create(voice_state_cb, 500, NULL);

        hint = lv_label_create(bottom);
        lv_label_set_text(hint, ui_str("watchface.hint", "上滑进入应用"));
        lv_obj_set_style_text_font(hint, UI_FONT_CAPTION, 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(UI_TEXT_TERTIARY), 0);
    }

    /* 5. 手势 */

    /* 统一手势（上滑进应用列表）。内部会递归给子对象打开冒泡，
     * 所以卡片、按钮上的按下事件也能传上来。 */
    ui_gesture_attach(s_page, on_watchface_swipe);

    /* 6. 定时刷新 */

    refresh_cb(NULL);
    s_timer = lv_timer_create(refresh_cb, 1000, NULL);

    syslog(LOG_INFO, "[watchface] page created\n");
    printf("[Watchface] created\n");
    return s_page;
}

void watchface_page_set_swipe_cb(watchface_swipe_cb_t cb)
{
    s_swipe_cb = cb;
}

void watchface_page_delete(void)
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

    if (s_voice_timer != NULL)
      {
        lv_timer_delete(s_voice_timer);
        s_voice_timer = NULL;
      }

    s_time_label = NULL;
    s_date_label = NULL;
    s_batt_label = NULL;
    s_hr_label = NULL;
    s_step_label = NULL;
    s_voice_btn = NULL;
    s_voice_label = NULL;
    s_expression_img = NULL;
    /* 重置「已显示哪款」，下次建页时第一帧就按当前索引设一次图 */
    s_expression_shown = -1;
}
