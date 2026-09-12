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
#include <lvgl/lvgl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <uikit/uikit_font_manager.h>
#include <unistd.h>

/* ── Constants ────────────────────────────────────────────────── */

static const char* TAG = "lvgl_ui";

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
#define LVGL_UI_MSG_MAX_LEN 512
#define CHAT_HISTORY_MAX 20

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

    /* Font (created via uikit font manager, supports CJK) */
    lv_font_t* font;

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
 * Add a message to the chat history ring buffer.
 *
 * When count < CHAT_HISTORY_MAX, writes at head and increments count.
 * When count == CHAT_HISTORY_MAX, overwrites the oldest message and
 * deletes its LVGL bubble widget if present.
 *
 * Text is truncated to LVGL_UI_MSG_MAX_LEN - 1 bytes with explicit
 * NUL termination (rule coding-9).
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

    /* Write message text with truncation (coding-9: strncpy + manual NUL) */
    strncpy(slot->text, text, LVGL_UI_MSG_MAX_LEN - 1);
    slot->text[LVGL_UI_MSG_MAX_LEN - 1] = '\0';

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

    /* Idempotent: already initialized */
    if (s_state.initialized) {
        return 0;
    }

    syslog(LOG_INFO, "[%s] init\n", TAG);

    /* Do NOT call lv_init() — the system (miwear) already initialized
     * LVGL and owns the display + event loop.  We only create our UI
     * widgets on the existing LVGL instance. */

    /* Do NOT create a display or allocate a display buffer — the system
     * already has one.  Creating a second display causes resource
     * conflicts and framebuffer contention. */

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

    /* Defer CJK font creation to the LVGL thread (show_screen_async_cb).
     * Creating a FreeType font here — from the main thread — causes
     * FT_Err_Invalid_Size_Handle (0x55) because the FreeType size
     * object races with the LVGL render thread.  Use Montserrat as
     * a safe placeholder until the screen is shown. */
    s_state.font = NULL;
    lv_obj_set_style_text_font(s_state.screen, &lv_font_montserrat_14, 0);

    /* Step 5: Create Chat View container */
    chat_h = LVGL_UI_SCREEN_H - LVGL_UI_PADDING_TOP - LVGL_UI_PADDING_BOTTOM
        - LVGL_UI_PTT_SIZE - 20;

    s_state.chat_list = lv_obj_create(s_state.screen);
    if (!s_state.chat_list) {
        syslog(LOG_ERR, "[%s] chat_list create failed\n", TAG);
        ret = -EIO;
        goto cleanup;
    }

    lv_obj_set_size(s_state.chat_list,
        LVGL_UI_SCREEN_W - 2 * LVGL_UI_PADDING_H, chat_h);
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
    if (s_state.font) {
        vg_font_destroy(s_state.font);
        s_state.font = NULL;
    }

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

    if (s_state.font) {
        vg_font_destroy(s_state.font);
        s_state.font = NULL;
    }

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

    /* Create CJK font here — inside the LVGL thread — so the
     * FreeType size handle is owned by the same thread that will
     * later rasterize glyphs.  Creating it from the main thread
     * (in init) causes FT_Err_Invalid_Size_Handle (0x55). */
    if (!s_state.font) {
        s_state.font = vg_font_create("MiSans-Medium", 18,
            LV_FREETYPE_FONT_STYLE_NORMAL);
        if (s_state.font) {
            lv_obj_set_style_text_font(s_state.screen, s_state.font, 0);
        } else {
            syslog(LOG_WARNING, "[%s] vg_font_create failed, keeping montserrat\n", TAG);
        }
    }

    syslog(LOG_INFO, "[%s] loading chat screen\n", TAG);

    /* Save the current screen so we can restore it on stop.
     * The launcher (miwear) owns this screen — we must not delete it. */
    s_state.prev_screen = lv_screen_active();

    lv_screen_load(s_state.screen);
    s_state.screen_visible = true;
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

/* Run TTS on a worker thread. voice_channel_speak() does blocking network
 * round-trips (and several of them for a long story), so calling it inline
 * would stall the dispatch thread and leave the UI unresponsive — frozen
 * touch, screen not refreshing. A detached thread keeps the UI alive and
 * relies on voice_channel_speak()'s internal abort logic to cancel an
 * in-flight utterance when a newer one arrives. */
static void* tts_speak_thread(void* arg)
{
    char* text = (char*)arg;

    if (text) {
        int ret = voice_channel_speak(text);
        if (ret != 0) {
            syslog(LOG_ERR, "[%s] voice_channel_speak failed (rc=%d)\n",
                TAG, ret);
        }
        free(text);
    }

    return NULL;
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

    strncpy(payload->text, text, LVGL_UI_MSG_MAX_LEN - 1);
    payload->text[LVGL_UI_MSG_MAX_LEN - 1] = '\0';
    payload->is_user = false;

    lv_async_call(chat_view_add_message_async_cb, payload);

    /* TTS is blocking — run it on a worker thread after scheduling the UI
     * update so the dispatch thread returns immediately. */
    char* tts_text = malloc(strlen(text) + 1);
    if (!tts_text) {
        syslog(LOG_ERR, "[%s] TTS text alloc failed\n", TAG);
        return 0;
    }

    strcpy(tts_text, text);

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    ret = pthread_create(&tid, &attr, tts_speak_thread, tts_text);
    pthread_attr_destroy(&attr);

    if (ret != 0) {
        free(tts_text);
        syslog(LOG_ERR, "[%s] TTS thread spawn failed (rc=%d)\n", TAG, ret);
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
        /* Agent bubble: left-aligned, semi-transparent dark bg */
        lv_obj_set_style_align(bubble, LV_ALIGN_LEFT_MID, 0);
        lv_obj_set_style_bg_color(bubble, lv_color_hex(0x2a2a40), 0);
        lv_obj_set_style_bg_opa(bubble, LV_OPA_90, 0);
        lv_obj_set_style_text_color(bubble, lv_color_hex(0xe8e8e8), 0);
    }

    /* Create label inside bubble */
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
