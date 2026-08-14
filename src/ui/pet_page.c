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
 *  - Blink: middle frames are COMPUTED FROM THE PIXEL ARRAYS by linearly
 *    blending pet_cloud_open_px / pet_cloud_blink_px (see blend_frames()).
 *    The blink plays open -> 1/3 -> 2/3 -> closed -> 2/3 -> 1/3 -> open.
 *  - Touch: tapping the cloud bounces it (scale anim), triggers a quick
 *    blink and shows a playful reply in the AI text layer.
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "pet_page.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Launcher wallpaper (390x450 ARGB8888, shared with launcher_page.c) */
extern const uint32_t img_background_watch_px[];

/* Cloud pet bitmaps (300x300 ARGB8888, shared with pet_display.c) */
extern const uint32_t pet_cloud_open_px[];
extern const uint32_t pet_cloud_blink_px[];

/* Fonts */
extern const lv_font_t ui_font_sans_24; /* back button glyph */
extern const lv_font_t ui_font_cjk_18;  /* greeting text (CJK subset) */

#define PET_CLOUD_SIZE   300
#define PET_CLOUD_STRIDE (PET_CLOUD_SIZE * 4)
#define PET_CLOUD_WIDTH_PCT 70   /* cloud width = 70% of full page width */

/* Blink timing */
#define BLINK_PERIOD_MS 3200     /* idle time between blinks */
#define BLINK_STEP_MS   70       /* per-frame time while blinking */
#define BLINK_MID_FRAMES 2       /* computed intermediate frames */
#define BLEND_1_3 85             /* 1/3 toward the blink frame */
#define BLEND_2_3 171            /* 2/3 toward the blink frame */

/* Wallpaper image descriptor (same bitmap as the launcher desktop) */
static const lv_image_dsc_t s_bg_dsc = {
    .header = { .magic = LV_IMAGE_HEADER_MAGIC,
                .cf = LV_COLOR_FORMAT_ARGB8888,
                .flags = 0,
                .w = 390,
                .h = 450,
                .stride = 390 * 4,
                .reserved_2 = 0 },
    .data_size = 390 * 450 * 4,
    .data = (const uint8_t *)img_background_watch_px,
    .reserved = NULL,
    .reserved_2 = NULL,
};

/* Cloud image descriptors: open / closed / computed middle frames */
static const lv_image_dsc_t s_cloud_open_dsc = {
    .header = { .magic = LV_IMAGE_HEADER_MAGIC,
                .cf = LV_COLOR_FORMAT_ARGB8888,
                .flags = 0,
                .w = PET_CLOUD_SIZE,
                .h = PET_CLOUD_SIZE,
                .stride = PET_CLOUD_STRIDE,
                .reserved_2 = 0 },
    .data_size = PET_CLOUD_SIZE * PET_CLOUD_STRIDE,
    .data = (const uint8_t *)pet_cloud_open_px,
    .reserved = NULL,
    .reserved_2 = NULL,
};

static const lv_image_dsc_t s_cloud_blink_dsc = {
    .header = { .magic = LV_IMAGE_HEADER_MAGIC,
                .cf = LV_COLOR_FORMAT_ARGB8888,
                .flags = 0,
                .w = PET_CLOUD_SIZE,
                .h = PET_CLOUD_SIZE,
                .stride = PET_CLOUD_STRIDE,
                .reserved_2 = 0 },
    .data_size = PET_CLOUD_SIZE * PET_CLOUD_STRIDE,
    .data = (const uint8_t *)pet_cloud_blink_px,
    .reserved = NULL,
    .reserved_2 = NULL,
};

static lv_obj_t *pet_page_container;
static lv_obj_t *cloud_img;      /* layer 3: the cloud */
static lv_obj_t *ai_text_label;  /* layer 2: AI returned text */
static pet_back_callback_t back_callback = NULL;

/* Blink state */
static uint32_t *s_mid_frames[BLINK_MID_FRAMES];  /* computed pixel arrays */
static lv_image_dsc_t s_mid_dsc[BLINK_MID_FRAMES];
static lv_timer_t *s_blink_timer;  /* idle -> start a blink */
static lv_timer_t *s_blink_step;   /* advances the blink frames */
static int s_blink_phase;          /* 0 idle, 1..5 blinking, 6 back to open */
static int s_cloud_scale;          /* current scale (256 = 100%) */

/* Touch reply auto-restore */
static lv_timer_t *s_greet_timer;  /* restores the greeting after tap */

/* Forward declarations */
static void greeting_build(char *buf, size_t len);

/****************************************************************************
 * Array computation: pixel blending
 ****************************************************************************/

/**
 * Linearly blend two ARGB8888 pixel arrays into dst.
 *
 * This is the "array computation" that synthesizes the in-between blink
 * frames: for every pixel of the 300x300 arrays,
 *   dst = a * (256 - f)/256 + b * f/256   (per channel, incl. alpha)
 *
 * @param dst  Output array (PET_CLOUD_SIZE^2 pixels)
 * @param a    First frame (open eyes)
 * @param b    Second frame (closed eyes)
 * @param f    Blend factor: 0 -> a, 256 -> b
 */
static void blend_frames(uint32_t *dst, const uint32_t *a,
                         const uint32_t *b, unsigned f)
{
    unsigned i;
    unsigned n = PET_CLOUD_SIZE * PET_CLOUD_SIZE;
    unsigned inv = 256 - f;

    for (i = 0; i < n; i++)
    {
        uint32_t pa = a[i];
        uint32_t pb = b[i];
        unsigned ao = (((pa >> 24) & 0xff) * inv + ((pb >> 24) & 0xff) * f) >> 8;
        unsigned ro = (((pa >> 16) & 0xff) * inv + ((pb >> 16) & 0xff) * f) >> 8;
        unsigned go = (((pa >> 8) & 0xff) * inv + ((pb >> 8) & 0xff) * f) >> 8;
        unsigned bo = ((pa & 0xff) * inv + (pb & 0xff) * f) >> 8;
        dst[i] = (ao << 24) | (ro << 16) | (go << 8) | bo;
    }
}

/****************************************************************************
 * Blink animation
 ****************************************************************************/

/**
 * Switch the cloud bitmap to the given blink phase:
 *   0 -> open, 1 -> 1/3 closed, 2 -> 2/3 closed, 3 -> fully closed
 */
static void cloud_frame_set(int phase)
{
    const lv_image_dsc_t *dsc;

    switch (phase)
    {
    case 1:
        dsc = &s_mid_dsc[0];
        break;
    case 2:
        dsc = &s_mid_dsc[1];
        break;
    case 3:
        dsc = &s_cloud_blink_dsc;
        break;
    default:
        dsc = &s_cloud_open_dsc;
        break;
    }
    lv_image_set_src(cloud_img, dsc);
}

/**
 * Advance the blink sequence: open -> 1/3 -> 2/3 -> closed -> 2/3 -> 1/3 -> open
 */
static void blink_step_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    s_blink_phase++;
    if (s_blink_phase > 5)
    {
        s_blink_phase = 0;
        cloud_frame_set(0);
        lv_timer_pause(s_blink_step);
        return;
    }
    cloud_frame_set(s_blink_phase);
}

/**
 * Start one blink sequence
 */
static void blink_start(void)
{
    if (cloud_img == NULL)
    {
        return;
    }
    s_blink_phase = 1;
    cloud_frame_set(1);
    lv_timer_reset(s_blink_step);
    lv_timer_resume(s_blink_step);
}

/**
 * Idle timer: blink every BLINK_PERIOD_MS
 */
static void blink_idle_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    blink_start();
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
 * Cloud tapped: bounce + quick blink + playful reply
 */
static void on_cloud_clicked(lv_event_t *e)
{
    lv_anim_t a;
    lv_anim_t a2;

    (void)e;
    printf("[PetPage] Cloud tapped\n");

    /* Bounce: scale up then back (squash & stretch) */
    lv_anim_init(&a);
    lv_anim_set_var(&a, cloud_img);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_image_set_scale);
    lv_anim_set_values(&a, s_cloud_scale, s_cloud_scale + 44);
    lv_anim_set_time(&a, 160);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lv_anim_init(&a2);
    lv_anim_set_var(&a2, cloud_img);
    lv_anim_set_exec_cb(&a2, (lv_anim_exec_xcb_t)lv_image_set_scale);
    lv_anim_set_values(&a2, s_cloud_scale + 44, s_cloud_scale);
    lv_anim_set_time(&a2, 280);
    lv_anim_set_delay(&a2, 160);
    lv_anim_set_path_cb(&a2, lv_anim_path_ease_in);
    lv_anim_start(&a2);

    /* Quick blink on tap */
    blink_start();

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
 * Release all pet page resources (timers, computed frames)
 */
static void pet_cleanup(void)
{
    int i;

    if (s_blink_timer != NULL)
    {
        lv_timer_delete(s_blink_timer);
        s_blink_timer = NULL;
    }
    if (s_blink_step != NULL)
    {
        lv_timer_delete(s_blink_step);
        s_blink_step = NULL;
    }
    if (s_greet_timer != NULL)
    {
        lv_timer_delete(s_greet_timer);
        s_greet_timer = NULL;
    }
    for (i = 0; i < BLINK_MID_FRAMES; i++)
    {
        if (s_mid_frames[i] != NULL)
        {
            free(s_mid_frames[i]);
            s_mid_frames[i] = NULL;
        }
    }
    cloud_img = NULL;
    ai_text_label = NULL;
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
    lv_obj_t *row3;      /* layer 3: cloud image */
    char greeting[64];
    int full_w;
    lv_coord_t cloud_w;
    int i;

    printf("[PetPage] Creating pet display page\n");

    /* Compute the middle blink frames from the pixel arrays (once) */
    for (i = 0; i < BLINK_MID_FRAMES; i++)
    {
        if (s_mid_frames[i] == NULL)
        {
            s_mid_frames[i] = malloc(PET_CLOUD_SIZE * PET_CLOUD_STRIDE);
            if (s_mid_frames[i] != NULL)
            {
                blend_frames(s_mid_frames[i], pet_cloud_open_px,
                             pet_cloud_blink_px,
                             i == 0 ? BLEND_1_3 : BLEND_2_3);
                s_mid_dsc[i] = s_cloud_open_dsc;
                s_mid_dsc[i].data = (const uint8_t *)s_mid_frames[i];
            }
        }
    }

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

    /* ── 第一层：返回按钮，全圆角 60x60 ── */
    row1 = lv_obj_create(pet_page_container);
    lv_obj_set_width(row1, lv_pct(100));
    lv_obj_set_height(row1, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row1, LV_OPA_0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(row1, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(row1, 0, 0);
    lv_obj_set_layout(row1, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row1, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

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
    s_cloud_scale = (int)cloud_w * 256 / PET_CLOUD_SIZE; /* keep ratio */

    cloud_img = lv_image_create(row3);
    lv_image_set_src(cloud_img, &s_cloud_open_dsc);
    lv_image_set_scale(cloud_img, s_cloud_scale);
    lv_obj_set_size(cloud_img, cloud_w, cloud_w); /* square bitmap: h = w */

    /* 触摸交互：点击云朵 -> 弹跳 + 眨眼 + 文字回应 */
    lv_obj_add_flag(cloud_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cloud_img, on_cloud_clicked, LV_EVENT_CLICKED, NULL);

    /* 眨眼：idle timer 触发，step timer 推进中间帧 */
    s_blink_step = lv_timer_create(blink_step_timer_cb, BLINK_STEP_MS, NULL);
    lv_timer_pause(s_blink_step);
    s_blink_timer = lv_timer_create(blink_idle_timer_cb, BLINK_PERIOD_MS, NULL);

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
