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
 * lvgl_ui_channel.c — LVGL UI channel for AI Agent
 *
 * Provides a chat-bubble interface on a 466x466 round watch screen.
 * User input via PTT (Push-to-Talk) button triggers ASR through
 * voice_channel; Agent replies are displayed as chat bubbles and
 * spoken via TTS.
 */

#include "ui/lvgl_ui_channel.h"
#include "core/message_bus.h"
#include "agent_compat.h"
#include "agent_config.h"
#include "voice/voice_channel.h"

#include <errno.h>
#include <fcntl.h>
#include <lvgl/lvgl.h>

/* App-owned pre-rasterized MiSans-16 CJK font (compiled from
 * src/ui/lv_font_misans_16_cjk.c, same pattern as xiaozhi_gui's
 * font_awesome_*.c) — declared here instead of patching apps_graphics_lvgl. */
LV_FONT_DECLARE(lv_font_misans_16_cjk);
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

/* ── Constants ────────────────────────────────────────────────── */

static const char* TAG = "lvgl_ui";

/* Self-test toggle: define to inject one card per reply category on
 * show_chat, for visual verification of all four color-bar variants.
 * Leave undefined in normal builds. */
/* #define LVGL_UI_CARD_SELFTEST */
/* Verified 2026-08-17: static lv_font_misans_16_cjk renders full CJK (no
 * tofu) and does NOT crash the heap — self-test confirmed via screendump.
 * Also verified: 1024-byte msg buffer + UTF-8-aware truncation shows full
 * ~568-byte Agent summaries without mid-char cut. */

/* Layout constants for 466x466 round screen */
#define LVGL_UI_SCREEN_W 466
#define LVGL_UI_SCREEN_H 466
#define LVGL_UI_PADDING_H 50
#define LVGL_UI_PADDING_TOP 60
#define LVGL_UI_PADDING_BOTTOM 30
#define LVGL_UI_PTT_SIZE 112
#define LVGL_UI_CHAT_GAP 10
#define LVGL_UI_BUBBLE_RADIUS 14
#define LVGL_UI_BUBBLE_PAD 10
/* Max message text stored per bubble.  1024 bytes ≈ 340 simplified-Chinese
 * chars (UTF-8, 3 bytes/char) — enough for typical Agent LLM summaries.
 * Truncation is UTF-8-aware (chat_view_copy_text) so a multi-byte char is
 * never split in half (which would render a replacement glyph at the cut). */
#define LVGL_UI_MSG_MAX_LEN 1024
#define CHAT_HISTORY_MAX 20

/* ── Enhanced Agent card styling ─────────────────────────────── */
/* Agent replies render as a richer card: title bar + divider +
 * left status color bar + body.  The color bar reflects a coarse
 * classification of the reply (conclusion / structured / jira /
 * plain) so the user gets a glanceable cue on the round screen. */
#define LVGL_UI_CARD_TITLE "Agent"
#define LVGL_UI_CARD_TITLEBAR_H 18
#define LVGL_UI_CARD_COLOR_BAR_W 3
#define LVGL_UI_CARD_GAP 4

/* Status palette (RGB 0xRRGGBB) */
#define LVGL_UI_CARD_COLOR_CONCLUSION 0x4caf50 /* green  — yes/no/建议 */
#define LVGL_UI_CARD_COLOR_STRUCTURED 0x3a7bd5 /* blue   — MERGED/状态: */
#define LVGL_UI_CARD_COLOR_JIRA        0xff9800 /* orange — More Info/经办人 */
#define LVGL_UI_CARD_COLOR_PLAIN       0x9e9e9e /* grey   — default */

/* PTT button label — use CJK text that MiSans definitely supports.
 * Emoji glyphs (U+1F399/U+1F3A4) are NOT in MiSans and cause
 * FT_Load_Glyph error 0x14 (glyph not found). */
#define PTT_ICON_MIC "\xe8\xaf\xb7\xe8\xaf\xb4" /* "请说" in UTF-8 */
#define CLOSE_BTN_TEXT "\xe5\x85\xb3\xe9\x97\xad" /* "关闭" in UTF-8 */

/* ── Data structures ──────────────────────────────────────────── */

/* Single chat message */
typedef struct {
    char text[LVGL_UI_MSG_MAX_LEN]; /* Message text, truncated to 512 bytes */
    bool is_user; /* true = user message, false = Agent reply */
    lv_obj_t* bubble; /* Corresponding LVGL bubble widget */
} chat_msg_t;

/* Chat history ring buffer */
typedef struct {
    chat_msg_t msgs[CHAT_HISTORY_MAX]; /* Ring buffer slots */
    int head; /* Next write position */
    int count; /* Current message count */
} chat_history_t;

/* LVGL UI channel internal state */
typedef struct {
    bool initialized;
    bool running;
    pthread_mutex_t lock;

    /* UI widget references */
    lv_obj_t* screen;
    lv_obj_t* chat_list;
    lv_obj_t* ptt_btn;
    lv_obj_t* ptt_label;
    lv_obj_t* rec_indicator;
    lv_obj_t* close_btn;
    lv_anim_t rec_anim;

    /* Chat history */
    chat_history_t history;

    /* PTT state */
    bool is_recording;
    bool is_processing;

    /* Screen visibility */
    bool screen_visible;

    /* Previous screen to restore on stop */
    lv_obj_t* prev_screen;

} lvgl_ui_state_t;

static lvgl_ui_state_t s_state;

/* ── Self-managed LVGL init (qemu: no miwear/launcher owns LVGL) ──
 *
 * On real hardware the miwear/launcher process initializes LVGL and
 * owns the display + event loop; this channel only creates widgets.
 * On qemu-arm64-v8a-ap there is no such process, so the channel must
 * bring up LVGL itself: lv_init() + lv_nuttx_init(/dev/fb0) + a
 * lv_timer_handler() loop thread.  s_lv_owned tracks whether WE did
 * the init so stop() can deinit symmetrically. */
static bool s_lv_owned = false;
static pthread_t s_lv_loop_tid;
static volatile bool s_lv_loop_running = false;
static lv_nuttx_result_t s_lv_result;

static void* lv_loop_thread(void* arg)
{
    (void)arg;
    while (s_lv_loop_running) {
        uint32_t idle = lv_timer_handler();
        if (idle == 0)
            idle = 1;
        usleep(idle * 1000);
    }
    return NULL;
}

/* ── Async message payload for lv_async_call ──────────────── */

/* Heap-allocated payload passed to lv_async_call so the LVGL thread
 * can safely create widgets without cross-thread invalidation.
 * Freed by the async callback after use. */
typedef struct {
    char text[LVGL_UI_MSG_MAX_LEN];
    bool is_user;
} async_msg_t;

/* ── Forward declarations ─────────────────────────────────── */

static void chat_view_add_message(const char* text, bool is_user);
static void chat_view_add_message_async_cb(void* data);
static void chat_view_scroll_to_bottom(void);
static void chat_view_trim_history(void);
static void show_screen_async_cb(void* data);
static void ptt_btn_event_cb(lv_event_t* e);
static void close_btn_event_cb(lv_event_t* e);
static void recording_indicator_start(void);
static void recording_indicator_stop(void);
static void recording_indicator_show_processing(void);

/* ── Ring buffer ──────────────────────────────────────────────── */

/**
 * Copy text into a fixed buffer with UTF-8-aware truncation.  Copies at most
 * dst_size-1 bytes, then if the cut lands inside a multi-byte UTF-8 sequence
 * (leading byte 0xC0..0xF4 not followed by enough continuation bytes 0x80..0xBF),
 * trims back to the last complete character.  Always NUL-terminates.
 *
 * This prevents a half-cut Chinese char from rendering as a replacement glyph
 * at the end of a truncated Agent reply.
 */
static void chat_view_copy_text(char* dst, const char* src, size_t dst_size)
{
    if (dst_size == 0) return;
    size_t n = strnlen(src, dst_size - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';

    /* If we filled the buffer, the last char may be incomplete.  Walk back
     * from the end to find a UTF-8 leading byte and check its expected length
     * against the remaining bytes. */
    if (n < dst_size - 1) return;          /* src fit fully, no truncation */
    size_t i = n;
    while (i > 0) {
        unsigned char c = (unsigned char)dst[i - 1];
        if ((c & 0xC0) != 0x80) break;     /* not a continuation byte */
        i--;
    }
    if (i == 0) return;                     /* all continuation bytes? give up */
    unsigned char lead = (unsigned char)dst[i - 1];
    size_t need;
    if      ((lead & 0x80) == 0x00) need = 1;   /* ASCII */
    else if ((lead & 0xE0) == 0xC0) need = 2;
    else if ((lead & 0xF0) == 0xE0) need = 3;
    else if ((lead & 0xF8) == 0xF0) need = 4;
    else { dst[i - 1] = '\0'; return; }         /* invalid lead, cut it */
    if (n - (i - 1) < need) {
        dst[i - 1] = '\0';                      /* incomplete seq, trim it */
    }
}

/**
 * Add a message to the chat history ring buffer.
 *
 * When count < CHAT_HISTORY_MAX, writes at head and increments count.
 * When count == CHAT_HISTORY_MAX, overwrites the oldest message and
 * deletes its LVGL bubble widget if present.
 *
 * Text is truncated to LVGL_UI_MSG_MAX_LEN - 1 bytes with explicit
 * NUL termination (rule coding-9), UTF-8-aware so a multi-byte char is
 * never split in half.
 *
 * Returns pointer to the newly written slot (inside the ring buffer,
 * no stack allocation of chat_msg_t — rule coding-6).
 */
static chat_msg_t* chat_history_add(chat_history_t* h, const char* text, bool is_user)
{
    chat_msg_t* slot = &h->msgs[h->head];

    /* When buffer is full, recycle the oldest slot */
    if (h->count == CHAT_HISTORY_MAX) {
        if (slot->bubble) {
            lv_obj_del(slot->bubble);
            slot->bubble = NULL;
        }
    } else {
        h->count++;
    }

    /* Write message text with UTF-8-aware truncation */
    chat_view_copy_text(slot->text, text, LVGL_UI_MSG_MAX_LEN);

    slot->is_user = is_user;
    slot->bubble = NULL;

    h->head = (h->head + 1) % CHAT_HISTORY_MAX;

    return slot;
}

/* ── Public API stubs (filled in by later tasks) ──────────────── */

int lvgl_ui_channel_init(void)
{
    int chat_h;
    int ret = 0;
    /* Display geometry — queried from the active display so the layout
     * adapts to both the 466×466 round watch (real hardware) and the
     * 1280×800 rectangle (qemu). Falls back to the 466 constants when
     * no display is registered yet. */
    lv_coord_t disp_w = LVGL_UI_SCREEN_W;
    lv_coord_t disp_h = LVGL_UI_SCREEN_H;

    /* Idempotent: already initialized */
    if (s_state.initialized) {
        return 0;
    }

    syslog(LOG_INFO, "[%s] init\n", TAG);

    /* If LVGL is not yet initialized (qemu: no miwear/launcher owns it),
     * bring it up ourselves: lv_init + lv_nuttx_init(/dev/fb0) + a
     * lv_timer_handler loop thread.  On real hardware where miwear already
     * did this, lv_is_initialized() is true and we skip — only creating
     * widgets on the existing display, as before. */
    if (!lv_is_initialized()) {
        lv_nuttx_dsc_t dsc;

        syslog(LOG_INFO, "[%s] LVGL not initialized; self-init on /dev/fb0\n", TAG);

        lv_init();
        lv_nuttx_dsc_init(&dsc);
        /* fb_path defaults to /dev/fb0 in lv_nuttx_dsc_init; GOLDFISH_GPU_FB
         * provides it on qemu-arm64-v8a-ap. */
        lv_nuttx_init(&dsc, &s_lv_result);

        if (s_lv_result.disp == NULL) {
            syslog(LOG_ERR, "[%s] lv_nuttx_init failed (no display)\n", TAG);
            ret = -EIO;
            goto cleanup;
        }

        /* Start the LVGL event loop thread (miwear would own this on real
         * hardware; on qemu we run it ourselves). */
        s_lv_loop_running = true;
        /* LVGL rendering + CJK (SIMSUN/FreeType) glyph rasterization needs a
         * deep stack; the default pthread stack overflows -> recursive
         * assert. Use an explicitly sized stack. */
        pthread_attr_t lv_attr;
        pthread_attr_init(&lv_attr);
        pthread_attr_setstacksize(&lv_attr, AGENT_LVGL_UI_STACK);
        if (pthread_create(&s_lv_loop_tid, &lv_attr, lv_loop_thread, NULL) != 0) {
            syslog(LOG_ERR, "[%s] LVGL loop thread create failed\n", TAG);
            lv_nuttx_deinit(&s_lv_result);
            s_lv_loop_running = false;
            ret = -EIO;
            goto cleanup;
        }
        s_lv_owned = true;
        syslog(LOG_INFO, "[%s] LVGL self-init OK, loop thread started\n", TAG);
    } else {
        syslog(LOG_INFO, "[%s] LVGL already initialized (system owns it)\n", TAG);
    }

    /* Query the real display resolution so the layout adapts to the
     * actual screen (466 round watch on hardware, 1280×800 on qemu)
     * instead of always using the 466 constants. */
    lv_display_t* disp = lv_display_get_default();
    if (disp) {
        lv_coord_t rw = lv_display_get_horizontal_resolution(disp);
        lv_coord_t rh = lv_display_get_vertical_resolution(disp);
        if (rw > 0 && rh > 0) {
            disp_w = rw;
            disp_h = rh;
            syslog(LOG_INFO, "[%s] display %dx%d\n", TAG, disp_w, disp_h);
        }
    }

    /* Create main screen with dark background */
    s_state.screen = lv_obj_create(NULL);
    if (!s_state.screen) {
        syslog(LOG_ERR, "[%s] screen create failed\n", TAG);
        ret = -EIO;
        goto cleanup;
    }

    lv_obj_set_style_bg_color(s_state.screen, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_bg_opa(s_state.screen, LV_OPA_COVER, 0);
    /* Do NOT call lv_screen_load() here — we load the screen on
     * demand when the user activates the chat UI, to avoid
     * hijacking the system's current screen at boot. */

    /* Default text font: the app-owned statically-compiled MiSans-16 CJK font
     * (3755 GB2312-1 common chars + ASCII + fullwidth punctuation, baked into
     * the binary via lv_font_conv — zero runtime malloc/rasterization) so
     * Agent reply Chinese text renders on qemu with full coverage and cannot
     * corrupt the heap.  Falls back to SimSun-16 (1000 chars) then Montserrat. */
    lv_obj_set_style_text_font(s_state.screen, &lv_font_misans_16_cjk, 0);

    /* Step 5: Create Chat View container */
    chat_h = disp_h - LVGL_UI_PADDING_TOP - LVGL_UI_PADDING_BOTTOM
        - LVGL_UI_PTT_SIZE - 20;

    s_state.chat_list = lv_obj_create(s_state.screen);
    if (!s_state.chat_list) {
        syslog(LOG_ERR, "[%s] chat_list create failed\n", TAG);
        ret = -EIO;
        goto cleanup;
    }

    lv_obj_set_size(s_state.chat_list,
        disp_w - 2 * LVGL_UI_PADDING_H, chat_h);
    lv_obj_align(s_state.chat_list, LV_ALIGN_TOP_MID, 0, LVGL_UI_PADDING_TOP);
    lv_obj_set_scroll_dir(s_state.chat_list, LV_DIR_VER);
    lv_obj_set_flex_flow(s_state.chat_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_state.chat_list, LVGL_UI_CHAT_GAP, 0);
    lv_obj_set_style_bg_opa(s_state.chat_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_state.chat_list, 0, 0);
    lv_obj_set_style_pad_left(s_state.chat_list, 4, 0);
    lv_obj_set_style_pad_right(s_state.chat_list, 4, 0);
    lv_obj_set_style_pad_top(s_state.chat_list, 4, 0);
    lv_obj_set_style_pad_bottom(s_state.chat_list, 4, 0);
    lv_obj_set_scrollbar_mode(s_state.chat_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_state.chat_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    /* Step 6: Create PTT Button */
    s_state.ptt_btn = lv_btn_create(s_state.screen);
    if (!s_state.ptt_btn) {
        syslog(LOG_ERR, "[%s] ptt_btn create failed\n", TAG);
        ret = -EIO;
        goto cleanup;
    }

    lv_obj_set_size(s_state.ptt_btn, LVGL_UI_PTT_SIZE, LVGL_UI_PTT_SIZE);
    lv_obj_align(s_state.ptt_btn, LV_ALIGN_BOTTOM_MID, 0, -LVGL_UI_PADDING_BOTTOM);
    lv_obj_set_style_radius(s_state.ptt_btn, LVGL_UI_PTT_SIZE / 2, 0);
    lv_obj_set_style_bg_color(s_state.ptt_btn, lv_color_hex(0x4a90d9), 0);
    lv_obj_set_style_shadow_width(s_state.ptt_btn, 12, 0);
    lv_obj_set_style_shadow_color(s_state.ptt_btn, lv_color_hex(0x4a90d9), 0);
    lv_obj_set_style_shadow_opa(s_state.ptt_btn, LV_OPA_40, 0);

    s_state.ptt_label = lv_label_create(s_state.ptt_btn);
    if (!s_state.ptt_label) {
        syslog(LOG_ERR, "[%s] ptt_label create failed\n", TAG);
        ret = -EIO;
        goto cleanup;
    }

    /* Use Unicode microphone emoji — MiSans includes it, unlike
     * LV_SYMBOL_AUDIO which needs FontAwesome glyphs not in MiSans
     * and renders as a rectangle. */
    lv_label_set_text(s_state.ptt_label, PTT_ICON_MIC);
    lv_obj_center(s_state.ptt_label);

    lv_obj_add_event_cb(s_state.ptt_btn, ptt_btn_event_cb, LV_EVENT_ALL, NULL);

    /* Step 7: Create Recording Indicator (initially hidden) */
    s_state.rec_indicator = lv_label_create(s_state.screen);
    if (!s_state.rec_indicator) {
        syslog(LOG_ERR, "[%s] rec_indicator create failed\n", TAG);
        ret = -EIO;
        goto cleanup;
    }

    lv_label_set_text(s_state.rec_indicator, "");
    lv_obj_align_to(s_state.rec_indicator, s_state.ptt_btn, LV_ALIGN_OUT_TOP_MID, 0, -8);
    lv_obj_add_flag(s_state.rec_indicator, LV_OBJ_FLAG_HIDDEN);

    /* Step 7b: Create Close Button (top-right corner) */
    s_state.close_btn = lv_btn_create(s_state.screen);
    if (!s_state.close_btn) {
        syslog(LOG_ERR, "[%s] close_btn create failed\n", TAG);
        ret = -EIO;
        goto cleanup;
    }

    lv_obj_set_size(s_state.close_btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    /* Position close button inside the visible circular area.
     * On a 466x466 round screen (radius 233), the top-right corner
     * is clipped by the bezel.  Place the button at a position that
     * is safely inside the circle: top-center with a rightward offset.
     * At y=30 from top, the visible horizontal range is roughly
     * center ± sqrt(233²-203²) ≈ ±115px, so x offset of +80 is safe. */
    lv_obj_align(s_state.close_btn, LV_ALIGN_TOP_MID, 80, LVGL_UI_PADDING_TOP - 30);
    lv_obj_set_style_bg_opa(s_state.close_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_state.close_btn, 0, 0);
    lv_obj_set_style_shadow_opa(s_state.close_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_state.close_btn, 8, 0);

    lv_obj_t* close_label = lv_label_create(s_state.close_btn);
    if (close_label) {
        lv_label_set_text(close_label, CLOSE_BTN_TEXT);
        lv_obj_set_style_text_color(close_label, lv_color_hex(0x999999), 0);
        lv_obj_center(close_label);
    }

    lv_obj_add_event_cb(s_state.close_btn, close_btn_event_cb, LV_EVENT_CLICKED, NULL);

    /* Step 8: Initialize chat history and mutex */
    memset(&s_state.history, 0, sizeof(chat_history_t));
    pthread_mutex_init(&s_state.lock, NULL);

    s_state.initialized = true;
    syslog(LOG_INFO, "[%s] init OK\n", TAG);
    return 0;

cleanup:
    /* Release resources in reverse order (coding-2) */

    if (s_state.screen) {
        lv_obj_del(s_state.screen);
        s_state.screen = NULL;
        /* Child widgets are deleted with the screen */
        s_state.chat_list = NULL;
        s_state.ptt_btn = NULL;
        s_state.ptt_label = NULL;
        s_state.rec_indicator = NULL;
        s_state.close_btn = NULL;
    }

    syslog(LOG_ERR, "[%s] init failed (rc=%d)\n", TAG, ret);
    return ret;
}

int lvgl_ui_channel_start(void)
{
    if (!s_state.initialized) {
        syslog(LOG_ERR, "[%s] start: not initialized\n", TAG);
        return -EINVAL;
    }

    /* Idempotent: already running */
    if (s_state.running) {
        return 0;
    }

    s_state.running = true;

    /* No dedicated UI thread needed — the system's miwear LVGL event
     * loop already calls lv_timer_handler() which processes our widgets.
     * We only need to mark ourselves as running so send/PTT callbacks
     * know the channel is active. */

    syslog(LOG_INFO, "[%s] started\n", TAG);
    return 0;
}

/* Semaphore for synchronizing async stop with LVGL thread */
static sem_t s_stop_sem;

/* Internal cleanup — must be called from LVGL thread context */
static void lvgl_ui_do_cleanup(void)
{
    int i;

    if (s_state.screen) {
        /* If our screen is the active screen, restore the previous
         * (launcher) screen first.  Deleting the active screen without
         * loading another one leaves LVGL with no valid screen — the
         * next lv_timer_handler() call crashes with a MemManage fault
         * (NULL pointer dereference at offset 0x32 in lv_obj_t). */
        if (s_state.screen_visible && s_state.prev_screen) {
            lv_screen_load(s_state.prev_screen);
        }

        lv_obj_del(s_state.screen);
        s_state.screen = NULL;
    }

    s_state.screen_visible = false;
    s_state.prev_screen = NULL;

    s_state.chat_list = NULL;
    s_state.ptt_btn = NULL;
    s_state.ptt_label = NULL;
    s_state.rec_indicator = NULL;
    s_state.close_btn = NULL;

    for (i = 0; i < CHAT_HISTORY_MAX; i++) {
        s_state.history.msgs[i].bubble = NULL;
    }

    s_state.history.head = 0;
    s_state.history.count = 0;

    /* Note: when we self-initialized LVGL (s_lv_owned, qemu case), the
     * lv_timer_handler loop thread is intentionally left running — it is
     * torn down only when the ai_agent process exits.  Stopping it from
     * within cleanup (which itself runs in that loop thread via
     * lv_async_call) would self-deadlock.  Widget deletion above is safe
     * because the loop is still alive to process it. */
}

static void stop_screen_async_cb(void* data)
{
    (void)data;

    lvgl_ui_do_cleanup();
    sem_post(&s_stop_sem);
}

/* Stop from within the LVGL thread (e.g. close button callback).
 * Performs cleanup directly — no async scheduling needed. */
void lvgl_ui_channel_stop_sync(void)
{
    if (!s_state.initialized || !s_state.running) {
        return;
    }

    syslog(LOG_INFO, "[%s] stopping (sync)\n", TAG);

    s_state.running = false;
    lvgl_ui_do_cleanup();
    pthread_mutex_destroy(&s_state.lock);
    s_state.initialized = false;

    syslog(LOG_INFO, "[%s] stopped\n", TAG);
}

/* Stop from an external thread (e.g. velaquit shutdown).
 * Schedules cleanup in the LVGL thread to avoid racing with the
 * miwear/launcher event loop. */
void lvgl_ui_channel_stop(void)
{
    /* No-op if not initialized or not running */
    if (!s_state.initialized || !s_state.running) {
        return;
    }

    syslog(LOG_INFO, "[%s] stopping\n", TAG);

    /* Mark as not running first — prevents callbacks (PTT, close_btn,
     * chat_view_add_message) from touching widgets during teardown. */
    s_state.running = false;

    /* Schedule cleanup in the LVGL thread to avoid racing with the
     * miwear/launcher event loop.  Direct lv_obj_del from the
     * shutdown thread causes a MemManage fault because the LVGL
     * thread may be mid-render on the same objects. */
    sem_init(&s_stop_sem, 0, 0);
    lv_async_call(stop_screen_async_cb, NULL);

    /* Wait for LVGL thread to complete the deletion (timeout 2s) */
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 2;

    if (sem_timedwait(&s_stop_sem, &ts) != 0) {
        syslog(LOG_WARNING,
            "[%s] async stop timed out, forcing cleanup\n", TAG);
        /* Fallback: NULL out pointers without deleting (leak is
         * acceptable during shutdown to avoid crash). */
        s_state.screen = NULL;
        s_state.chat_list = NULL;
        s_state.ptt_btn = NULL;
        s_state.ptt_label = NULL;
        s_state.rec_indicator = NULL;
        s_state.close_btn = NULL;
    }

    sem_destroy(&s_stop_sem);

    /* Destroy the mutex */
    pthread_mutex_destroy(&s_state.lock);

    s_state.initialized = false;

    syslog(LOG_INFO, "[%s] stopped\n", TAG);
}

/* Async callback: load screen inside LVGL thread context */
static void show_screen_async_cb(void* data)
{
    (void)data;
    if (!s_state.initialized || !s_state.running || !s_state.screen) {
        return;
    }
    if (s_state.screen_visible) {
        return;
    }

    /* CJK font selection happens at init() time via the statically-compiled
     * lv_font_misans_16_cjk (preferred: 3755 GB2312-1 common chars, baked into
     * the binary by lv_font_conv — zero runtime malloc, zero rasterization,
     * cannot corrupt the heap).  No runtime TTF loading here.
     *
     * History: we previously used lv_tiny_ttf_create_data() with a runtime
     * MiSans TTF, but stb_truetype's MakeGlyphBitmap write path overran its
     * output buffer on certain CJK glyphs and corrupted adjacent NuttX mm heap
     * chunk metadata, leading to recursive-assert reboots (mm_foreach /
     * nxsched_add_readytorun / nxsem_post_slow in the timer IRQ).  Subsetting
     * to 857 KB only delayed the crash; the root cause is the rasterizer, so
     * we moved to a pre-rasterized static font.  See lv_font_misans_16_cjk.c
     * and the fontgen notes in docs. */
    /* Already set in init(); nothing to do at show time. */
    syslog(LOG_INFO, "[%s] using static MiSans-16 CJK font\n", TAG);

    syslog(LOG_INFO, "[%s] loading chat screen\n", TAG);

    /* Save the current screen so we can restore it on stop.
     * The launcher (miwear) owns this screen — we must not delete it. */
    s_state.prev_screen = lv_screen_active();

    lv_screen_load(s_state.screen);
    s_state.screen_visible = true;

#ifdef LVGL_UI_CARD_SELFTEST
    /* Debug self-test: inject one card per reply category so all four
     * color-bar variants render on a single show_chat.  Remove the
     * -DLVGL_UI_CARD_SELFTEST define to disable. */
    syslog(LOG_INFO, "[%s] CARD_SELFTEST: injecting sample cards\n", TAG);
    chat_view_add_message("这个提交已 MERGED,状态: 正常合入", false);
    chat_view_add_message("Jira issue FEEDBACK-149182,经办人: 张文海,More Info", false);
    chat_view_add_message("值得合入,建议采纳,yes", false);
    chat_view_add_message("这是一条普通回复,用于展示默认灰色色条", false);
    chat_view_add_message("你好,这是一条用户消息", true);
#endif
}

void lvgl_ui_channel_show(void)
{
    if (!s_state.initialized || !s_state.running) {
        syslog(LOG_ERR, "[%s] show: channel not ready\n", TAG);
        return;
    }

    if (s_state.screen_visible) {
        return;
    }

    /* Schedule screen load in LVGL thread to avoid cross-thread
     * invalidation assertion. */
    lv_async_call(show_screen_async_cb, NULL);
}

int lvgl_ui_channel_send(const char* text)
{
    int ret;
    async_msg_t* payload;

    /* Parameter validation: NULL or empty string */
    if (!text || text[0] == '\0') {
        syslog(LOG_ERR, "[%s] send: invalid text (NULL or empty)\n", TAG);
        return -EINVAL;
    }

    /* Check channel readiness */
    if (!s_state.initialized || !s_state.running) {
        syslog(LOG_ERR, "[%s] send: channel not ready (init=%d run=%d)\n",
            TAG, s_state.initialized, s_state.running);
        return -EINVAL;
    }

    /* Auto-show chat screen on first Agent reply */
    if (!s_state.screen_visible) {
        lvgl_ui_channel_show();
    }

    /* Schedule bubble creation in LVGL thread via lv_async_call.
     * This avoids the "Invalidate area is not allowed during rendering"
     * assertion crash that occurs when we create LVGL widgets from
     * the agent dispatch thread while miwear's LVGL thread is
     * mid-render. */
    payload = malloc(sizeof(async_msg_t));
    if (!payload) {
        syslog(LOG_ERR, "[%s] send: async payload alloc failed\n", TAG);
        return -ENOMEM;
    }

    chat_view_copy_text(payload->text, text, LVGL_UI_MSG_MAX_LEN);
    payload->is_user = false;

    lv_async_call(chat_view_add_message_async_cb, payload);

    /* TTS is blocking — run after scheduling the UI update */
    ret = voice_channel_speak(text);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] voice_channel_speak failed (rc=%d)\n", TAG, ret);
    }

    return 0;
}

/* ── Async callback for LVGL thread ────────────────────────── */

/* Called by lv_timer_handler() inside the LVGL thread.
 * Safe to create/modify LVGL objects here. */
static void chat_view_add_message_async_cb(void* data)
{
    async_msg_t* payload = (async_msg_t*)data;

    if (!payload) {
        return;
    }

    if (!s_state.initialized || !s_state.running) {
        free(payload);
        return;
    }

    chat_view_add_message(payload->text, payload->is_user);
    free(payload);
    payload = NULL;
}

/* ── Internal functions ───────────────────────────────────── */

/* Classify an Agent reply text into a coarse card category so the
 * left color bar gives a glanceable cue.  Matching is intentionally
 * simple (case-insensitive substring) — this is a visual hint, not
 * a parser.  Returns the palette color for the matched category. */
static uint32_t chat_view_classify_reply(const char* text)
{
    if (!text || !*text) {
        return LVGL_UI_CARD_COLOR_PLAIN;
    }

    /* Build a lowercase copy (ASCII only; CJK passes through untouched)
     * so substring checks are case-insensitive for the latin keywords. */
    char buf[LVGL_UI_MSG_MAX_LEN];
    size_t i;
    for (i = 0; i < sizeof(buf) - 1 && text[i]; i++) {
        buf[i] = (text[i] >= 'A' && text[i] <= 'Z') ? (text[i] + 32) : text[i];
    }
    buf[i] = '\0';

    /* Conclusion: explicit yes/no or recommendation phrasing */
    if (strstr(buf, "yes") || strstr(buf, " no") || strstr(buf, "no.") ||
        strstr(buf, "no,") || strstr(buf, "\xe5\x80\xbc\xe5\xbe\x97") /* 值得 */ ||
        strstr(buf, "\xe4\xb8\x8d\xe5\xbb\xba\xe8\xae\xae") /* 不建议 */ ||
        strstr(buf, "\xe5\xbb\xba\xe8\xae\xae") /* 建议 */ ) {
        return LVGL_UI_CARD_COLOR_CONCLUSION;
    }

    /* Structured: Gerrit change status keywords (latin) or 状态: */
    if (strstr(buf, "merged") || strstr(buf, "abandoned") ||
        strstr(buf, "status:") || strstr(buf, "\xe7\x8a\xb6\xe6\x80\x81") /* 状态 */ ) {
        return LVGL_UI_CARD_COLOR_STRUCTURED;
    }

    /* Jira: workflow status or assignee fields */
    if (strstr(buf, "more info") || strstr(buf, "assignee") ||
        strstr(buf, "\xe7\xbb\x8f\xe5\x8a\x9e\xe4\xba\xba") /* 经办人 */ ||
        strstr(buf, "jira") ) {
        return LVGL_UI_CARD_COLOR_JIRA;
    }

    return LVGL_UI_CARD_COLOR_PLAIN;
}

/* Build the enhanced Agent card inside `bubble`: title bar (status
 * dot + "Agent") + divider + left color bar + body label.  Replaces
 * the plain label used before. */
static void chat_view_build_agent_card(lv_obj_t* bubble, const char* text,
                                       uint32_t bar_color)
{
    lv_obj_t* title_bar;
    lv_obj_t* status_dot;
    lv_obj_t* title_lbl;
    lv_obj_t* divider;
    lv_obj_t* body_row;
    lv_obj_t* color_bar;
    lv_obj_t* body_lbl;

    /* Title bar: a thin row holding the status dot + title text */
    title_bar = lv_obj_create(bubble);
    lv_obj_remove_flag(title_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(title_bar, lv_pct(100));
    lv_obj_set_height(title_bar, LVGL_UI_CARD_TITLEBAR_H);
    lv_obj_set_style_pad_all(title_bar, 0, 0);
    lv_obj_set_style_border_width(title_bar, 0, 0);
    lv_obj_set_style_bg_opa(title_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(title_bar, 0, 0);
    lv_obj_set_flex_flow(title_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(title_bar, LVGL_UI_CARD_GAP, 0);

    /* Status dot — colored circle reflecting the reply category */
    status_dot = lv_obj_create(title_bar);
    lv_obj_remove_flag(status_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(status_dot, 8, 8);
    lv_obj_set_style_radius(status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(status_dot, lv_color_hex(bar_color), 0);
    lv_obj_set_style_bg_opa(status_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(status_dot, 0, 0);
    lv_obj_set_style_pad_all(status_dot, 0, 0);

    /* Title label */
    title_lbl = lv_label_create(title_bar);
    lv_label_set_text(title_lbl, LVGL_UI_CARD_TITLE);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(0x9e9e9e), 0);

    /* Divider line under the title */
    divider = lv_obj_create(bubble);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(divider, lv_pct(100));
    lv_obj_set_height(divider, 1);
    lv_obj_set_style_pad_all(divider, 0, 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_bg_color(divider, lv_color_hex(0x3a3a4a), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(divider, 0, 0);
    lv_obj_set_style_pad_ver(divider, 0, 0);
    lv_obj_set_style_margin_top(divider, LVGL_UI_CARD_GAP, 0);
    lv_obj_set_style_margin_bottom(divider, LVGL_UI_CARD_GAP, 0);

    /* Body row: left color bar + body label */
    body_row = lv_obj_create(bubble);
    lv_obj_remove_flag(body_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(body_row, lv_pct(100));
    lv_obj_set_height(body_row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(body_row, 0, 0);
    lv_obj_set_style_border_width(body_row, 0, 0);
    lv_obj_set_style_bg_opa(body_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(body_row, 0, 0);
    lv_obj_set_flex_flow(body_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(body_row, LVGL_UI_CARD_GAP, 0);

    /* Left color bar (the category indicator) */
    color_bar = lv_obj_create(body_row);
    lv_obj_remove_flag(color_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(color_bar, LVGL_UI_CARD_COLOR_BAR_W);
    lv_obj_set_height(color_bar, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(color_bar, 0, 0);
    lv_obj_set_style_border_width(color_bar, 0, 0);
    lv_obj_set_style_bg_color(color_bar, lv_color_hex(bar_color), 0);
    lv_obj_set_style_bg_opa(color_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(color_bar, 2, 0);

    /* Body label — the actual reply text */
    body_lbl = lv_label_create(body_row);
    lv_label_set_text(body_lbl, text);
    lv_label_set_long_mode(body_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body_lbl, lv_pct(85));
    lv_obj_set_style_text_color(body_lbl, lv_color_hex(0xe8e8e8), 0);
    lv_obj_set_flex_grow(body_lbl, 1);
}

static void chat_view_add_message(const char* text, bool is_user)
{
    chat_history_t* h = &s_state.history;
    chat_msg_t* slot;
    lv_obj_t* bubble;
    lv_obj_t* lbl;

    /* If history is already full, trim before overwriting */
    if (h->count == CHAT_HISTORY_MAX) {
        chat_view_trim_history();
    }

    /* Store message in ring buffer (handles truncation + old bubble deletion) */
    slot = chat_history_add(h, text, is_user);

    /* Create bubble container inside chat_list */
    bubble = lv_obj_create(s_state.chat_list);
    if (!bubble) {
        syslog(LOG_ERR, "[%s] bubble create failed\n", TAG);
        return;
    }

    lv_obj_set_width(bubble, lv_pct(85));
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bubble, LVGL_UI_BUBBLE_RADIUS, 0);
    lv_obj_set_style_pad_all(bubble, LVGL_UI_BUBBLE_PAD, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);

    if (is_user) {
        /* User bubble: right-aligned, blue background, white text */
        lv_obj_set_style_align(bubble, LV_ALIGN_RIGHT_MID, 0);
        lv_obj_set_style_bg_color(bubble, lv_color_hex(0x3a7bd5), 0);
        lv_obj_set_style_text_color(bubble, lv_color_white(), 0);
    } else {
        /* Agent card: left-aligned, semi-transparent dark bg, vertical
         * flex layout so title bar / divider / body stack top-to-bottom. */
        lv_obj_set_style_align(bubble, LV_ALIGN_LEFT_MID, 0);
        /* Card bg lighter than the screen bg (0x1a1a2e) + thin border so
         * the card reads as a distinct rounded box instead of blending
         * invisibly into the dark background. */
        lv_obj_set_style_bg_color(bubble, lv_color_hex(0x363650), 0);
        lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(bubble, lv_color_hex(0x4a4a66), 0);
        lv_obj_set_style_border_width(bubble, 1, 0);
        lv_obj_set_style_text_color(bubble, lv_color_hex(0xe8e8e8), 0);
        lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(bubble, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(bubble, LVGL_UI_CARD_GAP, 0);
    }

    if (is_user) {
        /* User bubble: plain label */
        lbl = lv_label_create(bubble);
        if (!lbl) {
            syslog(LOG_ERR, "[%s] bubble label create failed\n", TAG);
            lv_obj_del(bubble);
            slot->bubble = NULL;
            return;
        }

        lv_label_set_text(lbl, slot->text);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lbl, lv_pct(100));
    } else {
        /* Agent bubble: enhanced card (title bar + divider + color bar + body) */
        uint32_t bar_color = chat_view_classify_reply(slot->text);
        chat_view_build_agent_card(bubble, slot->text, bar_color);
    }

    slot->bubble = bubble;

    chat_view_scroll_to_bottom();
}

static void chat_view_scroll_to_bottom(void)
{
    lv_obj_scroll_to_y(s_state.chat_list, LV_COORD_MAX, LV_ANIM_ON);
}

static void chat_view_trim_history(void)
{
    /* Ring buffer handles old bubble deletion in chat_history_add().
     * This function is a hook for any additional cleanup if needed
     * in the future (e.g. scroll position adjustment). */
}

static void ptt_btn_event_cb(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);

    /* Single-click toggle: first click starts recording, second stops */
    if (code != LV_EVENT_CLICKED) {
        return;
    }

    pthread_mutex_lock(&s_state.lock);

    if (s_state.is_processing) {
        pthread_mutex_unlock(&s_state.lock);
        return;
    }

    if (!s_state.is_recording) {
        /* First click: start recording */
        int ret = voice_channel_start();
        if (ret != 0) {
            syslog(LOG_ERR, "[%s] voice_channel_start failed (rc=%d)\n", TAG, ret);
            pthread_mutex_unlock(&s_state.lock);
            chat_view_add_message("录音启动失败", false);
            return;
        }

        s_state.is_recording = true;
        pthread_mutex_unlock(&s_state.lock);

        recording_indicator_start();
    } else {
        /* Second click: stop recording and process */
        s_state.is_recording = false;
        s_state.is_processing = true;
        pthread_mutex_unlock(&s_state.lock);

        recording_indicator_show_processing();

        /* Stop recording and get ASR text synchronously */
        char asr_text[LVGL_UI_MSG_MAX_LEN];
        int ret = voice_channel_stop_with_text(asr_text,
            sizeof(asr_text));

        if (ret != 0) {
            syslog(LOG_ERR,
                "[%s] voice_channel_stop_with_text failed (rc=%d)\n",
                TAG, ret);
            goto done;
        }

        if (asr_text[0] == '\0') {
            chat_view_add_message("未识别到语音", false);
            goto done;
        }

        /* Construct inbound message with channel="lvgl_ui" */
        agent_msg_t msg;

        memset(&msg, 0, sizeof(msg));
        strncpy(msg.channel, AGENT_CHAN_LVGL_UI,
            sizeof(msg.channel) - 1);
        msg.channel[sizeof(msg.channel) - 1] = '\0';
        strncpy(msg.chat_id, "lvgl_ui",
            sizeof(msg.chat_id) - 1);
        msg.chat_id[sizeof(msg.chat_id) - 1] = '\0';

        msg.content = strdup(asr_text);
        if (!msg.content) {
            syslog(LOG_ERR, "[%s] strdup failed\n", TAG);
            goto done;
        }

        if (message_bus_push_inbound(&msg) != 0) {
            syslog(LOG_ERR,
                "[%s] message_bus_push_inbound failed\n", TAG);
            free(msg.content);
            msg.content = NULL;
            chat_view_add_message("发送失败", false);
            goto done;
        }

        chat_view_add_message(asr_text, true);

    done:
        pthread_mutex_lock(&s_state.lock);
        s_state.is_processing = false;
        pthread_mutex_unlock(&s_state.lock);
        recording_indicator_stop();
    }
}

/* Close button callback — destroy chat screen and stop audio */
static void close_btn_event_cb(lv_event_t* e)
{
    (void)e;
    if (!s_state.initialized || !s_state.running) {
        return;
    }

    syslog(LOG_INFO, "[%s] chat screen closed by user\n", TAG);

    /* Stop any active recording first */
    if (s_state.is_recording) {
        voice_channel_stop();
        s_state.is_recording = false;
    }

    /* Full teardown: destroy screen, free font, reset state.
     * Use sync version — we are already in the LVGL thread. */
    lvgl_ui_channel_stop_sync();
}

/* Animation callback wrapper — lv_obj_set_style_opa requires the
 * selector argument, so we cannot cast it directly as exec_cb. */
static void anim_opa_cb(void* obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t*)obj, (lv_opa_t)v, 0);
}

static void recording_indicator_start(void)
{
    /* PTT button: red background + slight scale-up */
    lv_obj_set_style_bg_color(s_state.ptt_btn, lv_color_hex(0xe74c3c), 0);
    lv_obj_set_style_transform_scale(s_state.ptt_btn, 280, 0);

    /* Show recording indicator label */
    lv_label_set_text(s_state.rec_indicator, "录音中...");
    lv_obj_set_style_text_color(s_state.rec_indicator, lv_color_hex(0xe74c3c), 0);
    lv_obj_clear_flag(s_state.rec_indicator, LV_OBJ_FLAG_HIDDEN);

    /* Pulse animation on PTT button opacity */
    lv_anim_init(&s_state.rec_anim);
    lv_anim_set_var(&s_state.rec_anim, s_state.ptt_btn);
    lv_anim_set_values(&s_state.rec_anim, LV_OPA_COVER, LV_OPA_70);
    lv_anim_set_time(&s_state.rec_anim, 600);
    lv_anim_set_repeat_count(&s_state.rec_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&s_state.rec_anim, 600);
    lv_anim_set_exec_cb(&s_state.rec_anim, anim_opa_cb);
    lv_anim_start(&s_state.rec_anim);
}

static void recording_indicator_stop(void)
{
    /* Restore PTT button default style */
    lv_obj_set_style_bg_color(s_state.ptt_btn, lv_color_hex(0x4a90d9), 0);
    lv_obj_set_style_transform_scale(s_state.ptt_btn, 256, 0);
    lv_obj_set_style_opa(s_state.ptt_btn, LV_OPA_COVER, 0);

    /* Stop pulse animation */
    lv_anim_del(s_state.ptt_btn, anim_opa_cb);

    /* Hide indicator */
    lv_obj_add_flag(s_state.rec_indicator, LV_OBJ_FLAG_HIDDEN);
}

static void recording_indicator_show_processing(void)
{
    /* Stop pulse animation and restore opacity */
    lv_anim_del(s_state.ptt_btn, anim_opa_cb);
    lv_obj_set_style_opa(s_state.ptt_btn, LV_OPA_COVER, 0);

    /* Switch indicator text to processing state */
    lv_label_set_text(s_state.rec_indicator, "识别中...");
    lv_obj_set_style_text_color(s_state.rec_indicator, lv_color_hex(0xf39c12), 0);
}

/* lvgl_ui_thread removed — we no longer run our own LVGL event loop.
 * The system's miwear event loop handles lv_timer_handler(). */
