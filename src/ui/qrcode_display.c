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

#include "ui/qrcode_display.h"
#include "agent_compat.h"

#include <string.h>

static const char *TAG = "qr_display";

#if defined(CONFIG_LV_USE_QRCODE) && defined(CONFIG_GRAPHICS_LVGL)
#include <lvgl/lvgl.h>
#include "ui/lvgl_ui_channel.h"  /* lvgl_ui_lock — cross-thread LVGL entry */

#define QR_DISPLAY_TIMEOUT_MS 120000 /* auto-close after 2 min */
#define QR_SIZE_PX            180  /* fixed size for watch screens */

static char s_qr_url[512];
static lv_obj_t *s_qr_screen;
static lv_timer_t *s_timeout_timer;

static void qr_cleanup(void)
{
    if (s_timeout_timer) {
        lv_timer_delete(s_timeout_timer);
        s_timeout_timer = NULL;
    }
    if (s_qr_screen) {
        lv_obj_delete(s_qr_screen);
        s_qr_screen = NULL;
    }
}

static void qr_click_cb(lv_event_t *e)
{
    (void)e;
    syslog(LOG_INFO, "[%s] QR screen dismissed by tap\n", TAG);
    qr_cleanup();
}

static void qr_timeout_cb(lv_timer_t *timer)
{
    (void)timer;
    syslog(LOG_INFO, "[%s] QR screen auto-closed (timeout)\n", TAG);
    s_timeout_timer = NULL; /* timer auto-deletes */
    if (s_qr_screen) {
        lv_obj_delete(s_qr_screen);
        s_qr_screen = NULL;
    }
}

static void qr_show_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    const char *url = s_qr_url;
    size_t url_len = strlen(url);

    if (url_len == 0) {
        syslog(LOG_WARNING, "[%s] empty QR URL\n", TAG);
        return;
    }

    /* Clean up any previous QR screen */
    qr_cleanup();

    /* Create a full-screen black background */
    s_qr_screen = lv_obj_create(lv_screen_active());
    if (!s_qr_screen) {
        syslog(LOG_ERR, "[%s] failed to create QR screen\n", TAG);
        return;
    }

    lv_obj_set_size(s_qr_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_qr_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_qr_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_qr_screen, 0, 0);
    lv_obj_set_style_pad_all(s_qr_screen, 0, 0);
    lv_obj_add_event_cb(s_qr_screen, qr_click_cb,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_qr_screen, LV_OBJ_FLAG_CLICKABLE);

    /* Create QR code widget */
    lv_obj_t *qr = lv_qrcode_create(s_qr_screen);
    if (!qr) {
        syslog(LOG_ERR, "[%s] lv_qrcode_create failed\n", TAG);
        qr_cleanup();
        return;
    }

    lv_coord_t qr_sz = QR_SIZE_PX;
    lv_qrcode_set_size(qr, qr_sz);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_obj_align(qr, LV_ALIGN_CENTER, 0, -20);

    lv_result_t rc = lv_qrcode_update(qr, url, (uint32_t)url_len);
    if (rc != LV_RESULT_OK) {
        syslog(LOG_ERR, "[%s] lv_qrcode_update failed\n", TAG);
        qr_cleanup();
        return;
    }

    /* Add white border around QR */
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 4, 0);

    /* Hint label */
    lv_obj_t *label = lv_label_create(s_qr_screen);
    if (label) {
        lv_label_set_text(label, "Tap to close");
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -20);
    }

    /* Auto-close timer */
    s_timeout_timer = lv_timer_create(qr_timeout_cb,
                                      QR_DISPLAY_TIMEOUT_MS, NULL);
    if (s_timeout_timer) {
        lv_timer_set_repeat_count(s_timeout_timer, 1);
        lv_timer_set_auto_delete(s_timeout_timer, true);
    }

    syslog(LOG_INFO, "[%s] QR displayed for: %.60s...\n", TAG, url);
}

int claw_show_qrcode(const char *url)
{
    if (!url || url[0] == '\0') {
        return ERROR;
    }

    strlcpy(s_qr_url, url, sizeof(s_qr_url));

    /* Called from the command channel thread: creating a timer mutates
     * LVGL's global timer list, which the LVGL loop thread is walking. */
    lvgl_ui_lock();
    lv_timer_t *t = lv_timer_create(qr_show_timer_cb, 0, NULL);
    if (t) {
        lv_timer_set_repeat_count(t, 1);
        lv_timer_set_auto_delete(t, true);
    }
    lvgl_ui_unlock();
    if (!t) {
        syslog(LOG_ERR, "[%s] failed to create show timer\n", TAG);
        return ERROR;
    }

    return OK;
}

#else /* !CONFIG_LV_USE_QRCODE || !CONFIG_GRAPHICS_LVGL */

int claw_show_qrcode(const char *url)
{
    if (!url || url[0] == '\0') {
        return ERROR;
    }
    syslog(LOG_INFO, "[%s] QR display not available, URL: %s\n",
           TAG, url);
    return OK;
}

#endif /* CONFIG_LV_USE_QRCODE && CONFIG_GRAPHICS_LVGL */
