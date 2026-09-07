/****************************************************************************
 * Pet Display Page
 *
 * Cloud pet page shown when the "桌宠" icon is tapped on the launcher.
 *
 * Layout (reuses the launcher wallpaper, overall padding 20):
 *   ┌──────────────────────────────┐
 *   │ (20)                        │
 *   │  [ 返回 ]                    │   layer 1: back button 60x60, circle
 *   │                              │
 *   │    中午好！今天怎么样？        │   layer 2: AI text (time greeting)
 *   │                              │
 *   │          ( ☁️ 云朵 )          │   layer 3: cloud image, 70% of full
 *   │                              │            width, aspect preserved
 *   │ (20)                        │
 *   └──────────────────────────────┘
 *
 * Cloud behaviors:
 *  - Touch: tapping the cloud shows a playful reply in the AI text layer.
 *
 * The cloud is drawn with LVGL primitives.  It used to be two 300x300
 * ARGB8888 frames blended into intermediate blink frames; those cost 720 KB
 * of flash, which is now spent on the on-device language model.
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "pet_page.h"
#include "lvgl_ui_channel.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Launcher wallpaper (390x450 RGB565, shared with launcher_page.c) */
extern const uint16_t img_background_watch_px[];

/* Fonts */
extern const lv_font_t ui_font_sans_24; /* back button glyph */
extern const lv_font_t ui_font_cjk_18;  /* greeting text (CJK subset) */

#define PET_CLOUD_SIZE   300
#define PET_CLOUD_WIDTH_PCT 70   /* cloud width = 70% of full page width */

/* Wallpaper image descriptor (same bitmap as the launcher desktop) */
static const lv_image_dsc_t s_bg_dsc = {
    .header = { .magic = LV_IMAGE_HEADER_MAGIC,
                .cf = LV_COLOR_FORMAT_RGB565,
                .flags = 0,
                .w = 390,
                .h = 450,
                .stride = 390 * 2,
                .reserved_2 = 0 },
    .data_size = 390 * 450 * 2,
    .data = (const uint8_t *)img_background_watch_px,
    .reserved = NULL,
    .reserved_2 = NULL,
};

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

/****************************************************************************
 * Cloud drawing
 ****************************************************************************/

/* One rounded bump of the cloud body */
static void cloud_bump(lv_obj_t *parent, int x, int y, int size)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, size, size);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
}

/**
 * Draw the cloud into parent: a wide rounded slab, two bumps on top and two
 * eyes.  Sized relative to w/h so the same code works at any scale.
 */
static void cloud_body_build(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *slab;
    lv_obj_t *eye;
    int i;

    slab = lv_obj_create(parent);
    lv_obj_set_size(slab, w, h / 2);
    lv_obj_set_pos(slab, 0, h / 2);
    lv_obj_set_style_radius(slab, h / 4, 0);
    lv_obj_set_style_bg_color(slab, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(slab, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(slab, 0, 0);
    lv_obj_clear_flag(slab, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(slab, LV_OBJ_FLAG_CLICKABLE);

    cloud_bump(parent, w / 8, h / 4, h / 2);
    cloud_bump(parent, w / 2 - h / 12, 0, h * 2 / 3);

    for (i = 0; i < 2; i++)
    {
        eye = lv_obj_create(parent);
        lv_obj_set_size(eye, w / 18, h / 9);
        lv_obj_set_pos(eye, i == 0 ? w * 5 / 12 : w * 7 / 12, h * 5 / 9);
        lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(eye, lv_color_hex(0x2b2b2b), 0);
        lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(eye, 0, 0);
        lv_obj_clear_flag(eye, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(eye, LV_OBJ_FLAG_CLICKABLE);
    }
}

/****************************************************************************
 * History overlay
 ****************************************************************************/

#define HIST_MSG_MAX 512

static void hist_bubble_create(lv_obj_t *parent, const char *text, bool is_user)
{
    lv_obj_t *row;
    lv_obj_t *lbl;

    row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row,
                              lv_color_hex(is_user ? 0x2f6fd0 : 0xf2f2f5), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_set_style_margin_bottom(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_obj_set_style_text_font(lbl, &ui_font_cjk_18, 0);
    lv_obj_set_style_text_color(lbl,
                                lv_color_hex(is_user ? 0xffffff : 0x2b2b2b), 0);
    lv_obj_set_style_text_line_space(lbl, 2, 0);
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
        lv_obj_t *empty = lv_label_create(s_hist_list);
        /* 还没有对话记录 */
        lv_label_set_text(empty,
                          "\xe8\xbf\x98\xe6\xb2\xa1\xe6\x9c\x89\xe5\xaf\xb9"
                          "\xe8\xaf\x9d\xe8\xae\xb0\xe5\xbd\x95");
        lv_obj_set_style_text_font(empty, &ui_font_cjk_18, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(0x9a9ab0), 0);
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
    lv_obj_t *close_btn;
    lv_obj_t *close_lbl;

    s_hist_page = lv_obj_create(pet_page_container);
    lv_obj_set_size(s_hist_page, 390, 450);
    lv_obj_set_pos(s_hist_page, -20, -20);   /* cancel the parent's padding */
    lv_obj_set_style_bg_color(s_hist_page, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_bg_opa(s_hist_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_hist_page, 0, 0);
    lv_obj_set_style_radius(s_hist_page, 0, 0);
    lv_obj_set_style_pad_all(s_hist_page, 12, 0);
    lv_obj_clear_flag(s_hist_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(s_hist_page, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_hist_page, LV_FLEX_FLOW_COLUMN);

    /* title bar: 标题 + 关闭按钮 */
    bar = lv_obj_create(s_hist_page);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(bar, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    title = lv_label_create(bar);
    /* 对话记录 */
    lv_label_set_text(title,
                      "\xe5\xaf\xb9\xe8\xaf\x9d\xe8\xae\xb0\xe5\xbd\x95");
    lv_obj_set_style_text_font(title, &ui_font_cjk_18, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x88ccff), 0);

    close_btn = lv_btn_create(bar);
    lv_obj_set_size(close_btn, 44, 44);
    lv_obj_set_style_radius(close_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_30, 0);
    lv_obj_set_style_border_width(close_btn, 0, 0);
    lv_obj_add_event_cb(close_btn, on_hist_close, LV_EVENT_CLICKED, NULL);

    close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "X");
    lv_obj_set_style_text_font(close_lbl, &ui_font_sans_24, 0);
    lv_obj_set_style_text_color(close_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_center(close_lbl);

    /* scrollable list */
    s_hist_list = lv_obj_create(s_hist_page);
    lv_obj_set_width(s_hist_list, lv_pct(100));
    lv_obj_set_flex_grow(s_hist_list, 1);
    lv_obj_set_style_bg_opa(s_hist_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_hist_list, 0, 0);
    lv_obj_set_style_pad_all(s_hist_list, 0, 0);
    lv_obj_set_style_margin_top(s_hist_list, 8, 0);
    lv_obj_set_layout(s_hist_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_hist_list, LV_FLEX_FLOW_COLUMN);
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
 * Cloud tapped: playful reply
 */
static void on_cloud_clicked(lv_event_t *e)
{
    (void)e;
    printf("[PetPage] Cloud tapped\n");

    /* Playful reply in the AI text layer, restore greeting after 3s */
    if (ai_text_label != NULL)
    {
        lv_label_set_text(ai_text_label,
                          "\xe5\x98\xbf\xef\xbc\x8c\xe6\x88\xb3\xe5\x88\xb0"
                          "\xe6\x88\x91\xe4\xba\x86\xef\xbd\x9e\xe6\x98\xaf"
                          "\xe5\xb0\x8f\xe4\xba\x91\xe5\x91\xa6\xef\xbc\x81");
        /* 嘿，戳到我了～是小云哦！ */
        if (s_greet_timer != NULL)
        {
            lv_timer_delete(s_greet_timer);
        }
        s_greet_timer = lv_timer_create(greet_restore_timer_cb, 3000, NULL);
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
    lv_obj_t *row1;      /* layer 1: back button */
    lv_obj_t *back_btn;
    lv_obj_t *back_label;
    lv_obj_t *hist_btn;
    lv_obj_t *hist_label;
    lv_obj_t *row3;      /* layer 3: cloud */
    char greeting[64];
    int full_w;
    lv_coord_t cloud_w;

    printf("[PetPage] Creating pet display page\n");

    /* Create pet page container: full-screen, launcher wallpaper */
    pet_page_container = lv_obj_create(lv_scr_act());
    lv_obj_set_size(pet_page_container, 390, 450);
    lv_obj_set_pos(pet_page_container, 0, 0);
    lv_obj_set_style_bg_image_src(pet_page_container, &s_bg_dsc, 0);
    lv_obj_set_style_bg_image_opa(pet_page_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pet_page_container, 0, 0);
    lv_obj_set_style_pad_all(pet_page_container, 20, 0); /* 整体内边距 20 */
    lv_obj_add_event_cb(pet_page_container, on_container_delete,
                        LV_EVENT_DELETE, NULL);

    /* 垂直布局，三层 */
    lv_obj_set_layout(pet_page_container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(pet_page_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(pet_page_container, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* ── 第一层：左返回按钮 + 右历史按钮，全圆角 60x60 ── */
    row1 = lv_obj_create(pet_page_container);
    lv_obj_set_width(row1, lv_pct(100));
    lv_obj_set_height(row1, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row1, LV_OPA_0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(row1, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(row1, 0, 0);
    lv_obj_set_layout(row1, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row1, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    back_btn = lv_btn_create(row1);
    lv_obj_set_size(back_btn, 60, 60); /* 60x60 圆形按钮 */
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_50, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(back_btn, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(back_btn, 0, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(back_btn, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(back_btn, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, 0); /* 全圆角 */

    back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "<");
    lv_obj_set_style_text_font(back_label, &ui_font_sans_24, 0);
    lv_obj_set_style_text_color(back_label, lv_color_hex(0x2b2b2b), 0);
    lv_obj_center(back_label);

    lv_obj_add_event_cb(back_btn, on_back_clicked, LV_EVENT_CLICKED, NULL);

    /* 历史对话按钮（右侧）：打开全页对话记录 */
    hist_btn = lv_btn_create(row1);
    lv_obj_set_size(hist_btn, 60, 60);
    lv_obj_set_style_bg_color(hist_btn, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(hist_btn, LV_OPA_50, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(hist_btn, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(hist_btn, 0, LV_STATE_PRESSED);
    lv_obj_set_style_radius(hist_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(hist_btn, on_hist_open, LV_EVENT_CLICKED, NULL);

    hist_label = lv_label_create(hist_btn);
    lv_label_set_text(hist_label, "\xe2\x98\xb0");  /* ☰ */
    lv_obj_set_style_text_font(hist_label, &ui_font_sans_24, 0);
    lv_obj_set_style_text_color(hist_label, lv_color_hex(0x2b2b2b), 0);
    lv_obj_center(hist_label);

    /* ── 第二层：AI 返回的文字（默认按时段问候） ── */
    greeting_build(greeting, sizeof(greeting));
    ai_text_label = lv_label_create(pet_page_container);
    lv_label_set_text(ai_text_label, greeting);
    lv_label_set_long_mode(ai_text_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ai_text_label, lv_pct(100));
    lv_obj_set_style_text_font(ai_text_label, &ui_font_cjk_18, 0);
    lv_obj_set_style_text_color(ai_text_label, lv_color_hex(0x2b2b2b), 0);
    lv_obj_set_style_text_align(ai_text_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(ai_text_label, 28, 0);

    /* ── 第三层：云朵图片，默认宽度为全宽的 70%，保持比例 ── */
    row3 = lv_obj_create(pet_page_container);
    lv_obj_set_width(row3, lv_pct(100));
    lv_obj_set_style_bg_opa(row3, LV_OPA_0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(row3, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(row3, 0, 0);
    lv_obj_set_flex_grow(row3, 1); /* 占满剩余高度，图片在层内居中 */
    lv_obj_set_layout(row3, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row3, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row3, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    full_w = lv_display_get_horizontal_resolution(NULL);
    cloud_w = full_w * PET_CLOUD_WIDTH_PCT / 100;   /* 70% of full width */

    /* 云朵用 LVGL 图元画：底部圆角块 + 两个圆鼓包 + 两只眼睛 */
    cloud_img = lv_obj_create(row3);
    lv_obj_set_size(cloud_img, cloud_w, cloud_w * 2 / 3);
    lv_obj_set_style_bg_opa(cloud_img, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cloud_img, 0, 0);
    lv_obj_set_style_pad_all(cloud_img, 0, 0);
    lv_obj_clear_flag(cloud_img, LV_OBJ_FLAG_SCROLLABLE);
    cloud_body_build(cloud_img, cloud_w, cloud_w * 2 / 3);

    /* 触摸交互：点击云朵 -> 文字回应 */
    lv_obj_add_flag(cloud_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cloud_img, on_cloud_clicked, LV_EVENT_CLICKED, NULL);

    printf("[PetPage] Pet display page created successfully\n");
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
