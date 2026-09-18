/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * lvgl_toast.c — Top-of-screen toast popup (transient notice, not in chat history).
 *
 * Pattern follows qrcode_display.c: an async entry copies the request onto the
 * heap and lv_async_call()'s into the LVGL thread, where the real widget is
 * built on top of lv_screen_active().  A one-shot lv_timer triggers a fade-out
 * animation that deletes the object; a CLICKED handler allows early dismiss.
 *
 * Only one toast is shown at a time — a newer toast replaces an older one.
 */

#include "ui/lvgl_toast.h"
#include "agent_config.h"

#include <string.h>
#include <stdlib.h>
#include <syslog.h>

#if defined(CONFIG_AI_AGENT_LVGL_UI) && defined(CONFIG_GRAPHICS_LVGL)
#include <lvgl/lvgl.h>

/* App-owned pre-rasterized MiSans-16 CJK font (compiled from
 * src/ui/lv_font_misans_16_cjk.c, same pattern as xiaozhi_gui's
 * font_awesome_*.c) — declared here instead of patching apps_graphics_lvgl. */
LV_FONT_DECLARE(lv_font_misans_16_cjk);

static const char *TAG = "lvgl_toast";

#define TOAST_TITLE_LEN 32
#define TOAST_BODY_LEN  160
#define TOAST_DEFAULT_MS 4000  /* auto-dismiss if caller passes 0 */

/* Heap payload for the async hop into the LVGL thread. */
typedef struct {
    char title[TOAST_TITLE_LEN];
    char body[TOAST_BODY_LEN];
    uint32_t bar_color;
    uint32_t duration_ms;
} toast_payload_t;

static lv_obj_t *s_toast = NULL;        /* active toast object, NULL if none */
static lv_timer_t *s_timer = NULL;     /* auto-dismiss timer */

/* ── Cleanup (must run on LVGL thread) ─────────────────────────── */

static void toast_cleanup(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_toast) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
    }
}

/* ── Event callbacks (LVGL thread) ──────────────────────────────── */

static void toast_click_cb(lv_event_t *e)
{
    (void)e;
    syslog(LOG_INFO, "[%s] toast dismissed by tap\n", TAG);
    toast_cleanup();
}

static void toast_anim_ready_cb(lv_anim_t *a)
{
    lv_obj_t *obj = (lv_obj_t *)a->var;
    if (obj == s_toast) {
        s_toast = NULL;   /* cleared so toast_cleanup won't double-delete */
    }
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    lv_obj_delete(obj);
}

static void toast_timeout_cb(lv_timer_t *t)
{
    (void)t;
    s_timer = NULL;  /* timer auto-deletes (repeat_count=1 + auto_delete) */
    if (!s_toast) {
        return;
    }
    syslog(LOG_INFO, "[%s] toast auto-dismiss (fade out)\n", TAG);

    /* Fade out 300ms then delete */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_toast);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&a, 300);
    lv_anim_set_ready_cb(&a, toast_anim_ready_cb);
    lv_anim_start(&a);
}

/* ── Build the toast widget (LVGL thread) ──────────────────────── */

static void toast_build(const toast_payload_t *p)
{
    /* Replace any existing toast */
    toast_cleanup();

    lv_obj_t *scr = lv_screen_active();
    if (!scr) {
        syslog(LOG_ERR, "[%s] no active screen\n", TAG);
        return;
    }

    lv_display_t *disp = lv_display_get_default();
    lv_coord_t disp_w = disp ? lv_display_get_horizontal_resolution(disp) : 466;
    lv_coord_t disp_h = disp ? lv_display_get_vertical_resolution(disp) : 466;
    (void)disp_h;

    lv_obj_t *toast = lv_obj_create(scr);
    if (!toast) {
        syslog(LOG_ERR, "[%s] lv_obj_create failed\n", TAG);
        return;
    }
    s_toast = toast;

    lv_coord_t w = disp_w - 40;
    if (w < 200) w = 200;
    lv_obj_set_size(toast, w, 80);
    lv_obj_align(toast, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_style_bg_color(toast, lv_color_hex(0x2a2a3e), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(toast, lv_color_hex(p->bar_color), 0);
    lv_obj_set_style_border_width(toast, 2, 0);
    lv_obj_set_style_border_side(toast, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_radius(toast, 8, 0);
    lv_obj_set_style_pad_all(toast, 8, 0);
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(toast, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(toast, toast_click_cb, LV_EVENT_CLICKED, NULL);

    /* Left color bar */
    lv_obj_t *bar = lv_obj_create(toast);
    lv_obj_set_size(bar, 6, 64);
    lv_obj_align(bar, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(p->bar_color), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 3, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Title label */
    lv_obj_t *title = lv_label_create(toast);
    if (title) {
        lv_label_set_text(title, p->title);
        lv_obj_set_style_text_font(title, &lv_font_misans_16_cjk, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(0xe8e8e8), 0);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 14, 4);
    }

    /* Body label (ellipsis on overflow) */
    lv_obj_t *body = lv_label_create(toast);
    if (body) {
        lv_label_set_text(body, p->body);
        lv_obj_set_style_text_font(body, &lv_font_misans_16_cjk, 0);
        lv_obj_set_style_text_color(body, lv_color_hex(0xc8c8d0), 0);
        lv_label_set_long_mode(body, LV_LABEL_LONG_DOT);
        lv_obj_set_width(body, w - 28);
        lv_obj_align(body, LV_ALIGN_BOTTOM_LEFT, 14, -4);
    }

    /* Auto-dismiss timer */
    s_timer = lv_timer_create(toast_timeout_cb, p->duration_ms, NULL);
    if (s_timer) {
        lv_timer_set_repeat_count(s_timer, 1);
        lv_timer_set_auto_delete(s_timer, true);
    }

    syslog(LOG_INFO, "[%s] toast: %s | %s\n", TAG, p->title, p->body);
}

/* ── Async entry (LVGL thread) ────────────────────────────────── */

static void toast_async_cb(void *data)
{
    toast_payload_t *p = (toast_payload_t *)data;
    if (!p) {
        return;
    }
    toast_build(p);
    free(p);
}

/* ── Public API ───────────────────────────────────────────────── */

void lvgl_toast_show_async(const char *title, const char *body,
                           uint32_t bar_color, uint32_t duration_ms)
{
    if (!title || !body) {
        return;
    }

    toast_payload_t *p = (toast_payload_t *)malloc(sizeof(toast_payload_t));
    if (!p) {
        syslog(LOG_ERR, "[%s] payload alloc failed\n", TAG);
        return;
    }
    strlcpy(p->title, title, sizeof(p->title));
    strlcpy(p->body, body, sizeof(p->body));
    p->bar_color = bar_color;
    p->duration_ms = duration_ms ? duration_ms : TOAST_DEFAULT_MS;

    if (lv_async_call(toast_async_cb, p) == LV_RESULT_OK) {
        /* async scheduled; payload freed in callback */
        return;
    }

    /* lv_async_call failed — free here to avoid leak */
    syslog(LOG_ERR, "[%s] lv_async_call failed\n", TAG);
    free(p);
}

#else /* !CONFIG_AI_AGENT_LVGL_UI || !CONFIG_GRAPHICS_LVGL */

void lvgl_toast_show_async(const char *title, const char *body,
                           uint32_t bar_color, uint32_t duration_ms)
{
    (void)bar_color;
    (void)duration_ms;
    if (title && body) {
        syslog(LOG_INFO, "[lvgl_toast] (LVGL unavailable) %s: %s\n", title, body);
    }
}

#endif /* CONFIG_AI_AGENT_LVGL_UI && CONFIG_GRAPHICS_LVGL */
