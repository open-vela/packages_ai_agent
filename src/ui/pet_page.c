/****************************************************************************
 * Pet Display Page
 *
 * Cloud pet page shown when the "桌宠" icon is tapped on the launcher.
 *
 * Layout (launcher wallpaper, padding 20 / bottom 16):
 *   ┌──────────────────────────────┐
 *   │ [ 返回 ]          [ 记录 ]   │   layer 1: 84x60 浮动按钮，落在圆角屏
 *   │                              │            安全区 (24,52)/(282,52)
 *   │    中午好！今天怎么样？        │   layer 2: AI text (time greeting)
 *   │                              │
 *   │          ( ☁️ 云朵 )          │   layer 3: cloud image (expression)
 *   │                              │
 *   │        [ 开始说话 ]           │   layer 4: 350x60 主按钮（距底 16）
 *   └──────────────────────────────┘
 *
 * Cloud behaviors:
 *  - Touch: tapping the cloud shows a playful reply in the AI text layer.
 *  - 底部「开始说话」是主入口，云朵保留为副路径：两者共用 on_cloud_clicked，
 *    去抖(1s)与语音线程都不复制一份。
 *
 * The cloud is drawn with LVGL primitives.  It used to be two 300x300
 * ARGB8888 frames blended into intermediate blink frames; those cost 720 KB
 * of flash, which is now spent on the on-device language model.
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "pet_page.h"
#include "ui_theme.h"
#include "ui/expression.h"       /* 2026-11-02 批次 3：云朵改用表情资产 */
#include "ui_nav.h"              /* ui_nav_attach_back_swipe（之前是隐式声明） */
#include "lvgl_ui_channel.h"
#include "pet_care.h"
#include "voice/voice_channel.h"
#include "health/hr_measure.h"
#include "health/spo2_measure.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 2026-11-02：壁纸已下线（浅色云光主题改纯色底，见 ui_theme.c
 * ui_page_apply_bg()）。原来的 `extern img_background_watch_px` 与
 * `s_bg_dsc` 描述符只被 launcher 用，这里留着只会把 351 KB 的全屏位图
 * 拖回链接 —— 已删除。 */

/* Fonts */
extern const lv_font_t ui_font_cjk_18;  /* greeting text (CJK subset) */

#define PET_CLOUD_SIZE   300
/* 表情图的显示缩放：1.5x = 384/256。资产是 120px 长边（批次 3），
 * 1.5x 之后约 180px 宽，正好落在本页 273px 的可用宽度里，放大倍率也
 * 还在双线性插值不糊的范围内。将来资产做成 240px 时把这里改成 256。 */
#define PET_EXPR_SCALE      384

static lv_obj_t *pet_page_container;
static lv_obj_t *cloud_img;      /* layer 3: the cloud */
static lv_obj_t *ai_text_label;  /* layer 2: AI returned text */
static pet_back_callback_t back_callback = NULL;

/* History overlay */
static lv_obj_t *s_hist_page;    /* full-page conversation log, lazily built */
static lv_obj_t *s_hist_list;    /* scrollable container of bubbles */

/* Touch reply auto-restore */
static lv_timer_t *s_greet_timer;  /* restores the greeting after tap */

/* Forward declarations */
static void greeting_build(char *buf, size_t len);
static void on_hr_measure_clicked(lv_event_t *e);

/****************************************************************************
 * Cloud drawing
 ****************************************************************************/

/****************************************************************************
 * History overlay
 ****************************************************************************/

#define HIST_MSG_MAX 512

static void hist_bubble_create(lv_obj_t *parent, const char *text, bool is_user)
{
    lv_obj_t *row;
    lv_obj_t *cap;
    lv_obj_t *bubble;
    lv_obj_t *lbl;
    uint32_t bg = is_user ? UI_ACCENT : UI_SURFACE;

    row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_margin_bottom(row, 10, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    /* 列方向：main=纵向靠上，cross=横向；用户靠右、小云靠左。 */
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          is_user ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER);

    cap = lv_label_create(row);
    lv_label_set_text(cap, is_user ? "我" : "小云");
    lv_obj_set_style_text_font(cap, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(UI_TEXT_TERTIARY), 0);
    lv_obj_set_style_margin_bottom(cap, 2, 0);

    bubble = lv_obj_create(row);
    lv_obj_set_width(bubble, lv_pct(86));
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, 16, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_pad_all(bubble, 10, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    lbl = lv_label_create(bubble);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, lv_pct(100));
    /* 动态文本必须 cjk_18 全集，24px 子集会出现方块 □ */
    lv_obj_set_style_text_font(lbl, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(ui_text_on_bg(bg)), 0);
    lv_obj_set_style_text_line_space(lbl, 3, 0);
}

/* 空态：当前表情 + 两行说明，风格与本机浅色主题一致。 */
static void hist_empty_create(void)
{
    lv_obj_t *box;
    lv_obj_t *imgbox;
    lv_obj_t *img;
    lv_obj_t *title;
    lv_obj_t *hint;

    box = lv_obj_create(s_hist_list);
    lv_obj_set_width(box, lv_pct(100));
    lv_obj_set_height(box, lv_pct(100));
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    {
        const expression_t *ex = expression_get(expression_current());
        int img_w = (int)ex->img->header.w * 160 / 256;
        int img_h = (int)ex->img->header.h * 160 / 256;

        imgbox = lv_obj_create(box);
        lv_obj_set_size(imgbox, img_w + 8, img_h + 8);
        lv_obj_set_style_bg_opa(imgbox, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(imgbox, 0, 0);
        lv_obj_set_style_pad_all(imgbox, 0, 0);
        lv_obj_clear_flag(imgbox, LV_OBJ_FLAG_SCROLLABLE);

        img = lv_image_create(imgbox);
        lv_image_set_src(img, ex->img);
        lv_image_set_scale(img, 160);
        lv_obj_center(img);
    }

    title = lv_label_create(box);
    lv_label_set_text(title, "还没有对话记录");
    lv_obj_set_style_text_font(title, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_TEXT_PRIMARY), 0);
    lv_obj_set_style_margin_top(title, 10, 0);

    hint = lv_label_create(box);
    lv_label_set_text(hint, "点底部 开始说话 按钮，和小云聊一句");
    lv_obj_set_style_text_font(hint, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_TEXT_SECONDARY), 0);
    lv_obj_set_style_margin_top(hint, 6, 0);
}

/* Refill from the chat ring.  Newest at the bottom, so walk the ring
 * backwards from the oldest entry we have. */
static void hist_rebuild(void)
{
    char text[HIST_MSG_MAX];
    bool is_user;
    int n;
    int i;

    if (s_hist_list == NULL) {
        return;
    }

    lv_obj_clean(s_hist_list);
    n = lvgl_ui_history_count();
    if (n == 0) {
        hist_empty_create();
        return;
    }

    for (i = n - 1; i >= 0; i--) {
        if (lvgl_ui_history_get(i, text, sizeof(text), &is_user)) {
            hist_bubble_create(s_hist_list, text, is_user);
        }
    }
    lv_obj_scroll_to_y(s_hist_list, LV_COORD_MAX, LV_ANIM_OFF);
}

static void on_hist_close(lv_event_t *e)
{
    (void)e;
    if (s_hist_page != NULL) {
        lv_obj_add_flag(s_hist_page, LV_OBJ_FLAG_HIDDEN);
    }
}

static void hist_page_build(void)
{
    lv_obj_t *bar;
    lv_obj_t *title;
    lv_obj_t *hint;

    s_hist_page = lv_obj_create(pet_page_container);
    lv_obj_set_size(s_hist_page, 390, 450);
    /* 全屏覆盖层：IGNORE_LAYOUT + (-20,-20) 抵消父容器 padding，
     * 铺满整屏且不参与父容器 flex 布局。 */
    lv_obj_add_flag(s_hist_page, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_pos(s_hist_page, -20, -20);
    lv_obj_set_style_bg_color(s_hist_page, lv_color_hex(UI_BG_BLACK), 0);
    lv_obj_set_style_bg_opa(s_hist_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_hist_page, 0, 0);
    lv_obj_set_style_radius(s_hist_page, 0, 0);
    lv_obj_set_style_pad_left(s_hist_page, UI_PAD_PAGE, 0);
    lv_obj_set_style_pad_right(s_hist_page, UI_PAD_PAGE, 0);
    /* 顶部 52 / 底部 40：文字与按钮都避开圆角屏的上下圆角。 */
    lv_obj_set_style_pad_top(s_hist_page, UI_SAFE_Y, 0);
    lv_obj_set_style_pad_bottom(s_hist_page, UI_SAFE_BOTTOM, 0);
    lv_obj_clear_flag(s_hist_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_hist_page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_layout(s_hist_page, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_hist_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_hist_page, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_hist_page, 0, 0);

    /* 标题行：标题 + 返回（沿用本机 84x60 胶囊按钮风格） */
    bar = lv_obj_create(s_hist_page);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, UI_BACK_H);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    title = lv_label_create(bar);
    lv_label_set_text(title, "对话记录");
    lv_obj_set_style_text_font(title, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_TEXT_PRIMARY), 0);

    (void)ui_button_create(bar, UI_BACK_W, UI_BACK_H, UI_SURFACE,
                           "返回", on_hist_close, NULL);

    /* 可滚动列表：只让列表滚，页面本身固定 */
    s_hist_list = lv_obj_create(s_hist_page);
    lv_obj_set_width(s_hist_list, lv_pct(100));
    lv_obj_set_flex_grow(s_hist_list, 1);
    lv_obj_set_style_bg_opa(s_hist_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_hist_list, 0, 0);
    lv_obj_set_style_pad_all(s_hist_list, 0, 0);
    lv_obj_set_style_pad_right(s_hist_list, 6, 0);
    /* 0：grow 项的外边距不会被扣进轨道，8px 会让底部提示行溢出 8px。 */
    lv_obj_set_style_margin_top(s_hist_list, 0, 0);
    lv_obj_set_layout(s_hist_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_hist_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_hist_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_hist_list, 0, 0);

    hint = lv_label_create(s_hist_page);
    lv_label_set_text(hint, "最新记录在下方 · 上下滑动查看");
    lv_obj_set_style_text_font(hint, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_TEXT_TERTIARY), 0);
    /* 0：overlay 底边 410，hint 高 21；原来再加 8px margin 会超出 8px。 */
    lv_obj_set_style_margin_top(hint, 0, 0);
}

static void on_hist_open(lv_event_t *e)
{
    (void)e;

    if (s_hist_page == NULL) {
        hist_page_build();
    }
    if (s_hist_page == NULL) {
        return;
    }
    hist_rebuild();
    lv_obj_remove_flag(s_hist_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_hist_page);
}

/****************************************************************************
 * Touch interaction
 ****************************************************************************/

/**
 * Restore the time greeting after a tap reply
 */
static void greet_restore_timer_cb(lv_timer_t *timer)
{
    char greeting[64];

    lv_timer_delete(timer);
    s_greet_timer = NULL;
    if (ai_text_label != NULL)
    {
        greeting_build(greeting, sizeof(greeting));
        lv_label_set_text(ai_text_label, greeting);
    }
}

/**
 * Public: schedule the greeting restore (LVGL thread only).
 *
 * Shared by the cloud tap reply and by the one-shot heart-rate measurement,
 * which overwrites the same text layer with progress and result lines.
 */
void pet_page_schedule_greeting_restore(uint32_t delay_ms)
{
    if (ai_text_label == NULL)
    {
        return; /* page already gone: nothing to restore */
    }
    if (s_greet_timer != NULL)
    {
        lv_timer_delete(s_greet_timer);
    }
    s_greet_timer = lv_timer_create(greet_restore_timer_cb, delay_ms, NULL);
}

/**
 * 「测心率」按钮：起一次单次测量
 *
 * 这里只做两件事 —— 复位空闲关怀计时（防止测量中被「空闲关怀」气泡打断），
 * 然后把活儿交给 hr_measure_start()。它内部只创建一个 worker 线程就返回，
 * 8~15s 的采集/轮询/上屏全部在那个线程里跑，本回调（LVGL render 线程）
 * 绝不阻塞。测量进行中重复点击会被单飞挡掉（返回 -EBUSY）。
 */
static void on_hr_measure_clicked(lv_event_t *e)
{
    int ret;

    /* 无条件打印(含事件码): 以后真机上"点了没反应"时, 这一行就能区分
     * "事件根本没到 LVGL" 与 "事件到了但启动失败" —— 之前排查这个问题时,
     * 就是因为拿不到这条日志而在触摸/布局上绕了很久。 */
    printf("[PetPage] HR measure button: code=%d target=%p\n",
           (int)lv_event_get_code(e), (void *)lv_event_get_target(e));

    /* 触摸活动：复位空闲关怀计时，测量期间别让桌宠主动开口抢文本层 */
    pet_care_note_activity();

    ret = hr_measure_start();
    printf("[PetPage] hr_measure_start -> %d%s\n", ret,
           ret == 0 ? "" : " (ignored)");
}

/**
 * 「测血氧」按钮：起一次单次测量
 *
 * 和测心率一样只做两件事 —— 复位空闲关怀计时，把活儿交给
 * spo2_measure_start()。它内部只创建一个 worker 线程就返回，
 * ~19 秒的双通道采集全在那个线程里跑，本回调（LVGL render 线程）绝不阻塞。
 *
 * 注意: 血氧要求传感器工作在 RED+IR 双通道, 与心率的单红光模式不同。
 * spo2 自己会切模式并在结束时切回, 所以两个按钮可共存, 但**不能同时测**
 * (同一个传感器; 心率在跑时 spo2 会检测到 running 直接失败并提示)。
 */
static void on_spo2_measure_clicked(lv_event_t *e)
{
    int ret;

    printf("[PetPage] SpO2 button: code=%d target=%p\n",
           (int)lv_event_get_code(e), (void *)lv_event_get_target(e));

    pet_care_note_activity();

    ret = spo2_measure_start();
    printf("[PetPage] spo2_measure_start -> %d%s\n", ret,
           ret == 0 ? "" : " (ignored: busy)");
}

/* 云朵点击 = 语音对话开关（2026-11-01）
 *
 * 之前云朵只是卖萌文本，桌宠页**没有任何说话入口**（只有侧键和表盘按钮）。
 * 现在：单击开始录音（VAD 自动断句）、录音中再点一下提前结束。
 *
 * voice_channel_start()/stop() 可能阻塞（要等上一句 TTS 关闭 / ASR 收尾），
 * 绝不能占住 LVGL 线程 —— 之前音频相关同步调用已经导致过"屏幕卡死"，
 * 所以丢到一次性 pthread 里跑，UI 线程只更新文案。 */
static void *cloud_voice_thread(void *arg)
{
    intptr_t start = (intptr_t)arg;
    int ret;

    if (start)
    {
        ret = voice_channel_start();
        printf("[PetPage] cloud voice start -> %d\n", ret);
    }
    else
    {
        ret = voice_channel_stop();
        printf("[PetPage] cloud voice stop -> %d\n", ret);
    }

    return NULL;
}

/**
 * Cloud tapped: start/stop voice conversation
 */
static void on_cloud_clicked(lv_event_t *e)
{
    int recording;
    pthread_t tid;
    (void)e;

    /* v114：200ms 去抖，消灭连点导致的 start/stop 竞态 */
    {
        static uint32_t s_last_cloud_ms;
        struct timespec ts;
        uint32_t now;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
        if (s_last_cloud_ms != 0 && (uint32_t)(now - s_last_cloud_ms) < 1000)
        {
            printf("[PetPage] Cloud tapped debounced\n");
            return;
        }
        s_last_cloud_ms = now;
    }

    recording = voice_channel_is_recording();
    printf("[PetPage] Cloud tapped (recording=%d)\n", recording);

    /* 触摸活动：复位空闲关怀计时（闹钟 pending 时视为唤醒确认） */
    pet_care_note_activity();

    /* v111：上一轮还没回答/播报完时，不许再开新录音（防 ASR 上传与
     * 上一轮 LLM 并发打爆 BT PAN）。 */
    if (!recording && voice_channel_turn_active())
    {
        if (ai_text_label != NULL)
        {
            lv_label_set_text(ai_text_label, "等我说完再问～");
        }
        printf("[PetPage] Cloud tapped ignored: previous turn active\n");
        return;
    }

    if (ai_text_label != NULL)
    {
        lv_label_set_text(ai_text_label, recording
                          ? "收到，正在识别…"
                          : "我在听…说完再点一下云朵");
        if (s_greet_timer != NULL)
        {
            lv_timer_delete(s_greet_timer);
            s_greet_timer = NULL;
        }
    }

    if (pthread_create(&tid, NULL, cloud_voice_thread,
                       (void *)(intptr_t)(recording ? 0 : 1)) == 0)
    {
        pthread_detach(tid);
    }
    else
    {
        printf("[PetPage] cloud voice pthread_create failed\n");
    }
}

/****************************************************************************
 * Private helpers
 ****************************************************************************/

/**
 * Handle back button click
 */
static void on_back_clicked(lv_event_t *e)
{
    (void)e;
    printf("[PetPage] Back button clicked\n");
    pet_care_note_activity();
    if (back_callback != NULL)
    {
        back_callback();
    }
}

/**
 * Release all pet page resources (timers)
 */
static void pet_cleanup(void)
{
    if (s_greet_timer != NULL)
    {
        lv_timer_delete(s_greet_timer);
        s_greet_timer = NULL;
    }
    cloud_img = NULL;
    ai_text_label = NULL;
    /* children of pet_page_container; LVGL already freed them */
    s_hist_page = NULL;
    s_hist_list = NULL;
}

/**
 * Container delete event: LVGL calls this when the page object is deleted
 * (launcher deletes the page via lv_obj_del, not pet_page_delete()).
 */
static void on_container_delete(lv_event_t *e)
{
    (void)e;
    pet_cleanup();
}

/**
 * Pick the greeting prefix from the current hour:
 *   05:00-10:59 -> 早上好   11:00-12:59 -> 中午好
 *   13:00-17:59 -> 下午好   18:00-04:59 -> 晚上好
 */
static const char *greeting_prefix(int hour)
{
    if (hour >= 5 && hour < 11)
    {
        return "\xe6\x97\xa9\xe4\xb8\x8a\xe5\xa5\xbd"; /* 早上好 */
    }
    if (hour >= 11 && hour < 13)
    {
        return "\xe4\xb8\xad\xe5\x8d\x88\xe5\xa5\xbd"; /* 中午好 */
    }
    if (hour >= 13 && hour < 18)
    {
        return "\xe4\xb8\x8b\xe5\x8d\x88\xe5\xa5\xbd"; /* 下午好 */
    }
    return "\xe6\x99\x9a\xe4\xb8\x8a\xe5\xa5\xbd";     /* 晚上好 */
}

/**
 * Build the default greeting: "{早上|中午|下午|晚上}好！今天怎么样？"
 *
 * @param buf  Output buffer
 * @param len  Buffer size
 */
static void greeting_build(char *buf, size_t len)
{
    time_t now;
    struct tm tmv;

    now = time(NULL);
    if (localtime_r(&now, &tmv) == NULL)
    {
        tmv.tm_hour = 12; /* fall back to 中午好 */
    }
    snprintf(buf, len, "%s\xef\xbc\x81\xe4\xbb\x8a\xe5\xa4\xa9\xe6\x80"
                        "\x8e\xe4\xb9\x88\xe6\xa0\xb7\xef\xbc\x9f",
             greeting_prefix(tmv.tm_hour)); /* ！今天怎么样？ */
}

/****************************************************************************
 * Public API
 ****************************************************************************/

/**
 * Create the pet display page
 *
 * @return Pet page container object, or NULL on failure
 */
lv_obj_t *pet_page_create(void)
{
    lv_obj_t *row3;      /* layer 3: cloud */
    lv_obj_t *tile_pet;    /* 第 1 屏: 桌宠 */
    lv_obj_t *tile_health; /* 第 2 屏: 健康(心率/血氧) */
    char greeting[64];

    printf("[PetPage] Creating pet display page\n");

    /* Create pet page container: full-screen, launcher wallpaper */
    pet_page_container = lv_obj_create(lv_scr_act());
    ui_page_init(pet_page_container);
    /* ★ 这个容器绝不能滚动。lv_obj 默认带 LV_OBJ_FLAG_SCROLLABLE，而
     *   s_hist_page（:.219）是它的子对象，尺寸 390x450 —— 比这里 20px
     *   padding 之后的内容区（350x410）大，于是父容器判定"内容溢出"，
     *   自己变成可滚动。结果就是：在历史列表上拖动，滚的是**整个桌宠页**
     *   （背景图、云、按钮全部跟着移），而不是列表本身。
     *
     *   子对象里那句 lv_obj_set_pos(s_hist_page, -20, -20) 想抵消 padding，
     *   但父容器是 FLEX 布局，位置由布局决定，set_pos 会被布局覆盖 ——
     *   所以那行不解决问题，必须在这里关掉滚动。 */
    lv_obj_clear_flag(pet_page_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(pet_page_container, LV_SCROLLBAR_MODE_OFF);
    /* 底部安全区 40px：原来只留 16px，按钮下沿被屏幕圆角/边框遮住
     * （2026-11-03 用户实测「开始说话」在最低下被遮）。圆角屏上下都要让，
     * 顶部由浮动按钮 UI_SAFE_Y(52) 让，底部由这里让。 */
    lv_obj_set_style_pad_bottom(pet_page_container, UI_SAFE_BOTTOM, 0);

    /* 容器自己用纵向 flex：去掉 tileview 之后，AI 文本 / 云朵 /
     * 底部主按钮直接是它的子对象，不给布局的话它们会全部叠在 (0,0)。
     * 两个顶部按钮是浮动对象（IGNORE_LAYOUT），不参与这条内容流。 */
    lv_obj_set_layout(pet_page_container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(pet_page_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(pet_page_container, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* 默认主题会给 flex 容器 10px 行距；桌宠页高度账按 0 行距算，
     * 显式清零，避免按钮被行距再往下推。 */
    lv_obj_set_style_pad_row(pet_page_container, 0, 0);

    lv_obj_add_event_cb(pet_page_container, on_container_delete,
                        LV_EVENT_DELETE, NULL);

    /* 不再用 lv_tileview。
     *
     * 原来这里是一个两屏 tileview（桌宠 | 健康），问题有两个：
     *   1. tileview 靠滚动实现翻页，底部会带一条滚动条，视觉上很脏
     *   2. 翻页阈值按整屏计算，手指要划很长一段才切过去
     * 而「健康」那一屏的两个按钮（测心率/测血氧）现在已经是独立页面，
     * 所以整个 tileview 直接去掉，页面本身就是容器。
     */
    tile_pet = pet_page_container;
    tile_health = NULL;
    (void)tile_health;

    /* 原「健康」第二屏（测心率 / 测血氧按钮）已移除：
     * 这两个功能现在各有独立页面，从应用列表进入。 */

    /* ── 第二层：AI 返回的文字（默认按时段问候） ── */
    greeting_build(greeting, sizeof(greeting));
    ai_text_label = lv_label_create(tile_pet);
    lv_label_set_text(ai_text_label, greeting);
    /* ★ 2026-11-03 审计修复：LLM 回复无长度上限（MSG_MAX_LEN=512），
     * 5 行以上会把云图挤到"开始说话"按钮上。固定 3 行高度 + DOT 截断，
     * 长回复只显示前 3 行（完整内容仍可在对话记录里看）。 */
    lv_label_set_long_mode(ai_text_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ai_text_label, lv_pct(100));
    lv_obj_set_height(ai_text_label, 66);
    /* ★ 同 hist_bubble_create：这里是 AI 返回的文字（动态、字符不可预知），
     * 必须用 cjk_18 全集，否则子集外的字显示成 □。 */
    lv_obj_set_style_text_font(ai_text_label, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(ai_text_label, lv_color_hex(UI_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_align(ai_text_label, LV_TEXT_ALIGN_CENTER, 0);
    /* 顶部那两个浮动按钮（返回/记录）占 y=52..112，文字层要让开它们：
     * 子对象坐标是相对内容区（已扣掉 padding）的，所以上边距取
     * 安全区下沿 - 页内边距 = 112 - 20 = 92。 */
    lv_obj_set_style_margin_top(ai_text_label,
                                UI_SAFE_Y + UI_BACK_H - UI_PAD_PAGE, 0);

    /* ── 第三层：云朵图片，默认宽度为全宽的 70%，保持比例 ── */
    row3 = lv_obj_create(tile_pet);
    lv_obj_set_width(row3, lv_pct(100));
    lv_obj_set_style_bg_opa(row3, LV_OPA_0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(row3, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(row3, 0, 0);
    /* 固定 140 高，不用 flex_grow：LVGL 的 grow 会把 margin 排除在轨道计算外，
     * 实测会向下溢 20px 压到「开始说话」按钮上。固定高度 + 绝对定位按钮，
     * 两者互不依赖。云图 135px 高，居中后上下各留 2~3px。 */
    lv_obj_set_height(row3, 140);
    /* 云图区上边距 20；下边距 70 是给绝对定位的「开始说话」按钮留位：
     * 按钮固定在屏幕 y=350..410（距底 40px），云图区必须在 350 之前结束，
     * 否则 135px 高的云会被按钮压住。 */
    lv_obj_set_style_margin_top(row3, 20, 0);
    lv_obj_set_layout(row3, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row3, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row3, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* ★ 2026-11-02（批次 3）：云朵改用**真实表情资产**，不再用 LVGL 图元拼。
     *
     * 之前是「圆角块 + 两个圆鼓包 + 两只眼睛」拼出来的白云：批次 1 换浅色
     * 主题后白云落在 #F8FBFD 上几乎看不见，只能加 3px 描边救回来，而且
     * 所有表情长得一模一样。现在直接画 Lumina 的云图 —— 12 款表情各自
     * 不同，笔触就是设计稿的笔触，也不用再手工描边。
     *
     * 取的是**当前选中**的表情，和表盘显示的保持一致（都在 LVGL 线程读
     * expression_current()，它是纯内存读，只有第一次调用才 open 文件）。 */
    {
        const expression_t *ex = expression_get(expression_current());
        int img_w = (int)ex->img->header.w * PET_EXPR_SCALE / 256;
        int img_h = (int)ex->img->header.h * PET_EXPR_SCALE / 256;

        cloud_img = lv_obj_create(row3);
        /* 容器比图片大 8px，给按下反馈留一点余量；点哪里都算点中云 */
        lv_obj_set_size(cloud_img, img_w + 8, img_h + 8);
        lv_obj_set_style_bg_opa(cloud_img, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(cloud_img, 0, 0);
        lv_obj_set_style_pad_all(cloud_img, 0, 0);
        lv_obj_clear_flag(cloud_img, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *expr_img = lv_image_create(cloud_img);
        lv_image_set_src(expr_img, ex->img);
        /* ★ lv_obj_set_size 对图片只裁剪不缩放，缩放必须走 set_scale */
        lv_image_set_scale(expr_img, PET_EXPR_SCALE);
        lv_obj_center(expr_img);
    }

    /* 触摸交互：点击云朵 -> 文字回应（副路径，保留） */
    lv_obj_add_flag(cloud_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cloud_img, on_cloud_clicked, LV_EVENT_CLICKED, NULL);

    /* ── 第四层：底部主按钮「开始说话」──
     *
     * 之前桌宠页**没有任何显式的说话入口**：只有"点云朵"（用户不知道）
     * 和侧键。云朵保留为副路径，主入口改成这颗 350x60 的胶囊按钮。
     *
     * 回调直接复用 on_cloud_clicked：1s 去抖、正在录音就提前结束、
     * 上一轮未说完不许开新录音、语音线程 detach —— 全部在里面，
     * 这里不再复制一份，免得两处行为漂移。
     *
     * 它在 flex 内容流里排在云朵之后，所以贴住内容区下沿（距屏幕底 16px，
     * 见上面 pad_bottom），同时顺带把云朵区的高度挤出来，云不会压到按钮。
     * 「开始说话」四个字都在 cjk_24 子集里，ui_button_create 的 24px 标签
     * 能完整渲染（见 assets/ui_font_cjk_24.c 的 cmap）。 */
    /* 绝对定位到屏幕 (20,350)-(370,410)：距底 40px，避开圆角屏下沿。
     * 之前放在 flex 流末尾，运行态实测底边到 430（离屏幕底只有 20px），
     * 被圆角遮住；绝对坐标与父容器 pad/flex 行为无关，更可靠。 */
    (void)ui_float_button_create(tile_pet, 350, 60, 20, 350, UI_ACCENT,
                                 "开始说话", on_cloud_clicked, NULL);

    /* ── 顶部安全区：返回（左）+ 记录（右），84x60 ──
     *
     * 原来这两个是 60x60 圆钮，塞在 flex 内容流第一行，算出来落在 (20,20)
     * —— 正在屏幕左上圆角里，被圆角切掉一块。现在改成浮动按钮：
     *   - 返回：ui_back_button_create()，固定 84x60 @ (24,52)；
     *   - 记录：ui_float_button_create()，84x60 @ (282,52)，
     *     282 = 390 - 24(安全边距) - 84(按钮宽)，右边距同为 24。
     * 两者都带 IGNORE_LAYOUT，不参与内容流；创建顺序放在所有内容之后，
     * 因为 ui_float_button_create 会 move_foreground —— 早建会被云朵盖住。
     * 「记录」= 打开对话记录全屏浮层（on_hist_open），语义与原来一致。 */
    (void)ui_back_button_create(tile_pet, on_back_clicked, NULL);
    (void)ui_float_button_create(tile_pet, UI_BACK_W, UI_BACK_H,
                                 UI_SCREEN_W - UI_SAFE_X - UI_BACK_W, UI_SAFE_Y,
                                 UI_SURFACE, "记录", on_hist_open, NULL);

    printf("[PetPage] Pet display page created successfully\n");

    /* 下滑返回：之前这个页面没挂手势，进去出不来 */
    ui_nav_attach_back_swipe(pet_page_container);

    return pet_page_container;
}

/**
 * Set the back button callback
 *
 * @param callback Function to call when back button is pressed.
 *                 Set to NULL to disable.
 */
void pet_page_set_back_callback(pet_back_callback_t callback)
{
    back_callback = callback;
}

/**
 * Update the AI returned text in the text layer
 *
 * @param text Response text to display (UTF-8)
 */
void pet_page_open_history(void)
{
    on_hist_open(NULL);
}

void pet_page_update_response(const char *text)
{
    if (ai_text_label != NULL && text != NULL)
    {
        lv_label_set_text(ai_text_label, text);
        printf("[PetPage] Response updated: %s\n", text);
    }
}

/**
 * Update the pet status text
 *
 * The three-layer design has no dedicated status line; kept as a no-op
 * for API compatibility.
 *
 * @param status Status text (ignored)
 */
void pet_page_update_status(const char *status)
{
    (void)status;
}

/**
 * Clean up pet page resources
 */
void pet_page_delete(void)
{
    printf("[PetPage] Deleting pet page\n");
    if (pet_page_container != NULL)
    {
        lv_obj_del(pet_page_container); /* triggers on_container_delete */
        pet_page_container = NULL;
    }
    back_callback = NULL;
}
