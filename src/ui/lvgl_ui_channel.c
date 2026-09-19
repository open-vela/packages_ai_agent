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
#include "channels/ws_server.h"
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
#include <unistd.h>

/* CJK font sources, in order of preference:
 *   1. uikit's font manager — needs CONFIG_UIKIT + LV_USE_FREETYPE and a .ttf
 *      deployed on the device at runtime (the original design).
 *   2. A font compiled into the firmware (CONFIG_AI_AGENT_LVGL_UI_CJK_FONT) —
 *      no runtime dependency, which is what standalone agent firmware uses.
 * Without either, the file must still build: the chat text is Chinese, so it
 * would then render blank with LVGL's built-in Montserrat. This include used
 * to be unconditional, which made the whole channel fail to compile whenever
 * uikit was absent. */
#if defined(CONFIG_UIKIT) && defined(CONFIG_UIKIT_FONT_MANAGER)
#include <uikit/uikit_font_manager.h>
#define LVGL_UI_HAVE_CJK_FONT 1
#else
#define LVGL_UI_HAVE_CJK_FONT 0
#endif

#if defined(CONFIG_AI_AGENT_LVGL_UI_CJK_FONT)
extern const lv_font_t lv_font_misans_18_cjk;
#define LVGL_UI_BUILTIN_CJK_FONT (&lv_font_misans_18_cjk)
#else
#define LVGL_UI_BUILTIN_CJK_FONT NULL
#endif

/* ── Constants ────────────────────────────────────────────────── */

static const char* TAG = "lvgl_ui";

/* Display/input devices used when this channel has to bring LVGL up by
 * itself (standalone agent firmware, i.e. no launcher owns the display). */
#define LVGL_UI_FB_PATH "/dev/lcd0"
#define LVGL_UI_INPUT_PATH "/dev/input0"

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

    /* Set when no host system owns LVGL: we bring it up (lv_init +
     * display/input) and run the event loop ourselves. */
    bool lvgl_owned;
    pthread_t lvgl_thread;

} lvgl_ui_state_t;

static lvgl_ui_state_t s_state;

/* ── Cross-thread message queue ───────────────────────────── */

/* Heap-allocated payload queued for the LVGL thread to render. */
typedef struct {
    char text[LVGL_UI_MSG_MAX_LEN];
    bool is_user;
} async_msg_t;

/* lv_async_call() must not be used to hand work to the LVGL thread.
 *
 * It is not thread safe: it mallocs a request and links it into an internal
 * LVGL list with no locking at all. One writer is fine and two are fine as
 * long as they are the same thread, but this code has several -- the agent
 * loop showing a recognised command, the outbound dispatcher mirroring a
 * reply, the event loop itself -- and once two of them interleave inside
 * that list it is corrupted. The next tick walks the wreckage and takes a
 * hard fault inside lv_async_timer_cb, on the LVGL thread, which is exactly
 * what a device dump showed after a few minutes of load.
 *
 * So nothing off the LVGL thread touches LVGL, not even to queue. Senders
 * put a message here under a plain mutex; the event loop takes them out and
 * builds the widgets itself.
 */
#define UI_QUEUE_MAX 12

static async_msg_t* s_queue[UI_QUEUE_MAX];
static int s_queue_head;
static int s_queue_count;
static bool s_show_requested;
static pthread_mutex_t s_queue_mtx = PTHREAD_MUTEX_INITIALIZER;

/* ── Forward declarations ─────────────────────────────────── */

static void chat_view_add_message(const char* text, bool is_user);
static void ui_drain(void);
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

    /* A host system UI (e.g. the miwear launcher) may already own LVGL,
     * the display and the event loop — in that case we only add widgets to
     * the existing instance and must not create a second display.
     * Standalone agent firmware has no such host, so bring LVGL up here or
     * lv_obj_create() below would assert on an uninitialized instance. */
    if (!lv_is_initialized()) {
        lv_nuttx_dsc_t dsc;
        lv_nuttx_result_t res;

        lv_init();
        lv_nuttx_dsc_init(&dsc);
        dsc.fb_path = LVGL_UI_FB_PATH;
        dsc.input_path = LVGL_UI_INPUT_PATH;
        lv_nuttx_init(&dsc, &res);

        if (res.disp == NULL) {
            syslog(LOG_ERR, "[%s] lv_nuttx_init found no display (%s)\n",
                TAG, LVGL_UI_FB_PATH);
            ret = -EIO;
            goto cleanup;
        }

        s_state.lvgl_owned = true;
        syslog(LOG_INFO, "[%s] LVGL brought up standalone: fb=%s input=%s\n",
            TAG, LVGL_UI_FB_PATH, LVGL_UI_INPUT_PATH);
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

    /* uikit/FreeType fonts must be created in the LVGL thread (see
     * show_screen_async_cb): creating one here races with the render thread
     * and fails with FT_Err_Invalid_Size_Handle (0x55).  A font compiled
     * into the firmware has no such constraint, so apply it right away;
     * otherwise Montserrat stands in until the screen is shown. */
    s_state.font = NULL;
    lv_obj_set_style_text_font(s_state.screen,
        LVGL_UI_BUILTIN_CJK_FONT ? LVGL_UI_BUILTIN_CJK_FONT
                                 : &lv_font_montserrat_14,
        0);

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
#if LVGL_UI_HAVE_CJK_FONT
    if (s_state.font) {
        vg_font_destroy(s_state.font);
        s_state.font = NULL;
    }
#endif

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

/* LVGL event loop — only used when we brought LVGL up ourselves.
 * The host system (miwear) normally drives lv_timer_handler() for us. */
static void* lvgl_event_loop(void* arg)
{
    (void)arg;

    /* Keep pumping until teardown finishes: lvgl_ui_channel_stop() clears
     * "running" first and then relies on this loop to run the queued
     * lv_async_call() cleanup, so keying on "running" would abort it. */
    while (s_state.initialized) {
        uint32_t idle;

        /* Put anything other threads have queued on screen first, then let
         * LVGL do its own work. This is the only place widgets get touched. */
        ui_drain();

        idle = lv_timer_handler();

        /* Never busy-spin, but do not sleep so long that a queued line sits
         * there visibly late either. */
        if (idle > 50) {
            idle = 50;
        }
        usleep((idle ? idle : 5) * 1000);
    }

    return NULL;
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

    /* When a host system owns LVGL, its event loop already calls
     * lv_timer_handler() and processes our widgets — nothing to do here.
     * Standalone, nobody would pump the loop, so run one ourselves. */
    if (s_state.lvgl_owned) {
        if (pthread_create(&s_state.lvgl_thread, NULL, lvgl_event_loop, NULL)
            != 0) {
            syslog(LOG_ERR, "[%s] failed to start LVGL event loop\n", TAG);
            s_state.running = false;
            return -EIO;
        }
        syslog(LOG_INFO, "[%s] LVGL event loop thread started\n", TAG);
    }

    /* Put the chat screen up immediately instead of waiting for the first
     * message.
     *
     * Lazy-showing it means the one control a person needs in order to say
     * anything -- the push-to-talk button -- does not exist until after they
     * have already said something, which is circular. It also leaves the
     * display sitting on a blank default screen, which reads as "the UI did
     * not start" rather than "waiting for input". Show() only queues an
     * lv_async_call, so this is safe to call before the event loop runs.
     */
    lvgl_ui_channel_show();

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

#if LVGL_UI_HAVE_CJK_FONT
    if (s_state.font) {
        vg_font_destroy(s_state.font);
        s_state.font = NULL;
    }
#endif

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

/* Join the event loop thread we own, if any.  Never joins from the LVGL
 * thread itself (stop_sync runs there). */
static void lvgl_ui_join_event_loop(void)
{
    if (!s_state.lvgl_owned) {
        return;
    }

    if (pthread_equal(pthread_self(), s_state.lvgl_thread)) {
        s_state.lvgl_owned = false;
        return;
    }

    pthread_join(s_state.lvgl_thread, NULL);
    s_state.lvgl_owned = false;
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

    lvgl_ui_join_event_loop();

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

    lvgl_ui_join_event_loop();

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
#if LVGL_UI_HAVE_CJK_FONT
    if (!s_state.font) {
        s_state.font = vg_font_create("MiSans-Medium", 18,
            LV_FREETYPE_FONT_STYLE_NORMAL);
        if (s_state.font) {
            lv_obj_set_style_text_font(s_state.screen, s_state.font, 0);
        } else {
            syslog(LOG_WARNING, "[%s] vg_font_create failed, keeping montserrat\n", TAG);
        }
    }
#endif

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

    /* Ask rather than schedule: the event loop picks this up on its next
     * pass. See the note on the queue -- lv_async_call() from here is what
     * corrupted LVGL's list. */
    pthread_mutex_lock(&s_queue_mtx);
    s_show_requested = true;
    pthread_mutex_unlock(&s_queue_mtx);
}

/* Common body for every way a line reaches the screen. 'speak' is separate
 * from 'is_user' because the two callers want different things: an Agent
 * reply is worth reading aloud, a mirrored or user line is not. */
static int ui_post(const char* text, bool is_user, bool speak)
{
    async_msg_t* payload;
    int slot;

    if (!text || text[0] == '\0') {
        return -EINVAL;
    }

    if (!s_state.initialized || !s_state.running) {
        return -EINVAL;
    }

    payload = malloc(sizeof(async_msg_t));
    if (!payload) {
        syslog(LOG_ERR, "[%s] queue payload alloc failed\n", TAG);
        return -ENOMEM;
    }

    strncpy(payload->text, text, LVGL_UI_MSG_MAX_LEN - 1);
    payload->text[LVGL_UI_MSG_MAX_LEN - 1] = '\0';
    payload->is_user = is_user;

    pthread_mutex_lock(&s_queue_mtx);

    if (s_queue_count == UI_QUEUE_MAX) {
        /* Full: drop the oldest so the line the user just caused still
         * gets through. Losing the tail of a burst is better than losing
         * the thing that prompted it. */
        free(s_queue[s_queue_head]);
        s_queue[s_queue_head] = NULL;
        s_queue_head = (s_queue_head + 1) % UI_QUEUE_MAX;
        s_queue_count--;
    }

    slot = (s_queue_head + s_queue_count) % UI_QUEUE_MAX;
    s_queue[slot] = payload;
    s_queue_count++;

    /* First line of any kind brings the chat screen up */
    s_show_requested = true;

    pthread_mutex_unlock(&s_queue_mtx);

    if (speak) {
        /* TTS is blocking — run after queueing the UI update */
        int ret = voice_channel_speak(text);
        if (ret != 0) {
            syslog(LOG_ERR, "[%s] voice_channel_speak failed (rc=%d)\n",
                TAG, ret);
        }
    }

    return 0;
}

/* Take everything queued and put it on screen. Runs on the LVGL thread only,
 * so it is the one place allowed to create or touch widgets. */
static void ui_drain(void)
{
    async_msg_t* batch[UI_QUEUE_MAX];
    bool show;
    int n = 0;
    int i;

    pthread_mutex_lock(&s_queue_mtx);

    show = s_show_requested;
    s_show_requested = false;

    while (s_queue_count > 0 && n < UI_QUEUE_MAX) {
        batch[n++] = s_queue[s_queue_head];
        s_queue[s_queue_head] = NULL;
        s_queue_head = (s_queue_head + 1) % UI_QUEUE_MAX;
        s_queue_count--;
    }

    pthread_mutex_unlock(&s_queue_mtx);

    if (show && !s_state.screen_visible) {
        show_screen_async_cb(NULL);
    }

    for (i = 0; i < n; i++) {
        chat_view_add_message(batch[i]->text, batch[i]->is_user);
        free(batch[i]);
        batch[i] = NULL;
    }

    /* A line landing means recognition finished -- either the recognised
     * command itself or the answer to it -- so the "识别中..." indicator has
     * done its job and goes back to idle. */
    if (n > 0 && s_state.is_processing) {
        s_state.is_processing = false;
        recording_indicator_stop();
    }
}

int lvgl_ui_channel_send(const char* text)
{
    return ui_post(text, false, true);
}

void lvgl_ui_channel_send_user(const char* text)
{
    /* The other side of the conversation. Without it the screen only ever
     * shows answers, and a command whose reply happens to be short looks
     * exactly like one that was never heard at all -- which is the hardest
     * case to debug when it is the recogniser that is wrong. */
    ui_post(text, true, false);
}

int lvgl_ui_channel_post(const char* text)
{
    /* Display only, no TTS. Used to mirror traffic that was addressed to
     * another channel; speaking it would both duplicate whatever the
     * intended channel does with it and, since voice_channel_speak() is
     * blocking, hold up the outbound dispatcher on every message. */
    return ui_post(text, false, false);
}

/* ── Async callback for LVGL thread ────────────────────────── */

/* The lv_async_call() callback that used to live here is gone with the call
 * itself; ui_drain() does the same work, from the event loop, with the queue
 * providing the thread safety that lv_async_call lacked. */

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
        s_state.is_recording = true;
        pthread_mutex_unlock(&s_state.lock);

        /* The microphone already streams to the host continuously; this
         * frame only says that whatever arrives from now on was meant as a
         * command. Recognition stays on the host because that is the only
         * place with a recogniser: this board's own voice channel wants
         * /dev/audio/pcm0c, and there is no NuttX audio driver under it, so
         * the button used to answer every press with "录音启动失败".
         * The other end is voice_bridge.py (see its "ptt" handler). */
        ws_server_broadcast_json("{\"type\":\"ptt\",\"state\":\"start\"}");
        recording_indicator_start();
    } else {
        s_state.is_recording = false;
        s_state.is_processing = true;
        pthread_mutex_unlock(&s_state.lock);

        ws_server_broadcast_json("{\"type\":\"ptt\",\"state\":\"stop\"}");
        recording_indicator_show_processing();
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
