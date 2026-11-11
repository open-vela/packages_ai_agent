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
#include "ui/ui_palette.h"
#include "core/message_bus.h"
#include "infra/network_manager.h"  /* network_is_connected — gate TTS */
#include "agent_compat.h"
#include "agent_config.h"
#include "remote/remote_link.h"      /* REMOTE_TOOL_MAX / REMOTE_HINT_MAX */
#include "voice/voice_channel.h"

#include <errno.h>
#include <fcntl.h>
#include <lvgl/lvgl.h>
#include <stdint.h>   /* uintptr_t — printing pthread_t as a pointer on 64-bit */

/* App-owned pre-rasterized MiSans-16 CJK font (compiled from
 * src/ui/lv_font_misans_12_cjk.c, same pattern as xiaozhi_gui's
 * font_awesome_*.c) — declared here instead of patching apps_graphics_lvgl. */
LV_FONT_DECLARE(lv_font_misans_12_cjk);
#ifdef CONFIG_STACK_COLORATION
#include <nuttx/arch.h>   /* up_check_tcbstack — LVGL thread stack high-water */
#include <nuttx/sched.h>  /* nxsched_self / nxsched_get_stackinfo */
#endif
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#ifdef CONFIG_AI_AGENT_PET_STATUS
#include "ui/pet_status_view.h"
#ifdef CONFIG_AI_AGENT_STANDBY_UI
#include "ui/standby_view.h"
#endif
#endif

/* ── Constants ────────────────────────────────────────────────── */

static const char* TAG = "lvgl_ui";

/* Separate tag for the notification page so a serial capture can tell its
 * lines apart from the rest of the channel's chatter. */
static const char* NOTICE_TAG = "lvgl_notice";

/* ── Reply-card hang probe (BUG-20261021-01) ────────────────────
 * Step markers written from the LVGL thread around every call in
 * chat_view_add_message()/chat_view_build_agent_card().  A log whose
 * last line is "tr<N>" (with no "tr<N+1>") names the exact LVGL entry
 * point that never returned.
 *
 * These go out through a raw write(1) to the console on purpose: syslog
 * takes a process-wide mutex, so if one thread wedges inside syslog the
 * probes stop too and a syslog deadlock looks exactly like a widget call
 * that never returns.  Bypassing it separates the two.
 * Drop this once the freeze is fixed. */
static void ui_raw_log(const char* what, long a, long b)
{
    char bfr[96];
    int n = snprintf(bfr, sizeof(bfr), "\r\n[lvgl_ui] %s %ld %ld\r\n",
                     what, a, b);

    if (n > 0)
      {
        ssize_t ignored = write(1, bfr, (size_t)n);
        (void)ignored;
      }
}

/* Free-form variant of ui_raw_log() for reports with more than two numbers
 * (lock-wait reports need the owner, the probe step and the LVGL progress
 * counters at once).  Same raw write(1) rationale. */
static void ui_raw_line(const char* fmt, ...)
{
    char bfr[192];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(bfr, sizeof(bfr), fmt, ap);
    va_end(ap);

    if (n > (int)sizeof(bfr) - 1)
      {
        n = (int)sizeof(bfr) - 1;
      }

    if (n > 0)
      {
        ssize_t ignored = write(1, bfr, (size_t)n);
        (void)ignored;
      }
}

/* Last probe step reached by the UI thread.  The watchdog prints it, so a
 * stall report still names the call the UI thread is stuck in even when the
 * UI thread's own probe line never made it out (blocked console write). */
static volatile int s_ui_trace_step;

/* AP boot-record debug marker (SWAP word 40).  The CP's ap-monitor thread
 * prints every change to this word directly on UART0 (ttyUSB0), so a freeze
 * that wedges the AP's RPMsg console can still be attributed to the last
 * trace step reached.  Declared here rather than including the chip header
 * (the ai_agent app include path does not provide it) — the symbol is linked
 * into the same flat AP image.  Other boards (qemu goldfish) have no such
 * marker, so it compiles away there. */
#if defined(CONFIG_ARCH_CHIP_BK7258)
extern void bk7258_ap_dbg_mark(uint32_t mark);
#  define BK_MARK(step) bk7258_ap_dbg_mark((uint32_t)(step))
#else
#  define BK_MARK(step) ((void)0)
#endif

#define UITRACE(step)                                        \
  do {                                                       \
      s_ui_trace_step = (step);                              \
      BK_MARK(step);                                         \
      ui_raw_log("tr", (long)(step), 0);                     \
    } while (0)

/* LVGL-side progress counters, defined in core/lv_obj_pos.c.  They tell a
 * spin inside one layout pass (counters climb) from a block inside a single
 * call (counters frozen); lv_dbg_class names the widget being processed. */
extern volatile uint32_t lv_dbg_layout_outer;
extern volatile uint32_t lv_dbg_layout_calls;
extern volatile uint32_t lv_dbg_refr_calls;
extern volatile const char* lv_dbg_class;

/* ── LVGL thread-safety lock ─────────────────────────────────────
 *
 * This build has CONFIG_LV_USE_OS=0, so LVGL has no internal locking:
 * its timer list, object tree and lv_malloc heap are plain
 * unsynchronized globals.  lv_loop_thread() calls lv_timer_handler(),
 * but every other thread (agent loop, outbound dispatch, PTT button,
 * cmd channel, notify) reaches LVGL through lv_async_call() /
 * lv_timer_create().  Without a lock those calls race the handler:
 * lv_timer_create() links the new timer into the global list *before*
 * filling in its fields, so the handler can run a half-built timer and
 * read a garbage user_data pointer.  Observed on BK7258 hardware:
 * lv_async_timer_cb's `ldmia` on an unaligned user_data -> UsageFault
 * (CFSR=0x01000000 UNALIGNED) in the ai_agent process, which RPTUN
 * escalates to "Panic by remote core: ap" and resets AP.
 *
 * Hold this lock around any LVGL entry point called from a non-LVGL
 * thread, and around lv_timer_handler() itself.
 *
 * Recursion is required: LVGL-thread callbacks (standby_timer_cb)
 * themselves queue async calls.  CONFIG_PTHREAD_MUTEX_TYPES
 * is disabled in this build, so a recursive pthread mutex is unavailable;
 * emulate it with owner + depth instead.
 */
static pthread_mutex_t s_lvgl_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile pthread_t s_lvgl_lock_owner;  /* 0 = not held */
static volatile int s_lvgl_lock_depth;

void lvgl_ui_lock(void)
{
    pthread_t self = pthread_self();
    struct timespec t0;
    struct timespec t1;
    bool wait_logged = false;
    long next_raw = 5000;   /* periodic raw report while waiting */

    /* Fast path: this thread already owns the lock.  Only the owner ever
     * stores its tid here, and unlock() clears it before releasing, so a
     * stale value from a previous owner can never match a non-owner. */
    if (s_lvgl_lock_owner != (pthread_t)0 &&
        pthread_equal(s_lvgl_lock_owner, self))
      {
        s_lvgl_lock_depth++;
        return;
      }

    /* Try-then-sleep instead of a plain blocking lock so a stuck LVGL
     * thread can be identified: whoever holds the lock too long is named
     * in the log before the caller gives up waiting. */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (pthread_mutex_trylock(&s_lvgl_lock) != 0)
      {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ms = (t1.tv_sec - t0.tv_sec) * 1000
                + (t1.tv_nsec - t0.tv_nsec) / 1000000;

        if (!wait_logged && ms >= 500)
          {
            syslog(LOG_WARNING,
                   "[%s] ui lock wait %ldms (owner tid=%p)\n",
                   TAG, ms, (void *)(uintptr_t)s_lvgl_lock_owner);
            wait_logged = true;
          }

        /* Backup channel for BUG-20261021-01: this thread is alive and
         * waiting, so it can report the state of the thread that holds the
         * lock even if that thread (and the watchdog) never prints again.
         * Raw write(1), same reason as the probes: no syslog lock. */
        if (ms >= next_raw)
          {
            ui_raw_line("\r\n[lvgl_ui] lw %ld owner %p tr %d lvl %lu/%lu/%lu %s\r\n",
                        ms, (void *)(uintptr_t)s_lvgl_lock_owner, s_ui_trace_step,
                        (unsigned long)lv_dbg_layout_outer,
                        (unsigned long)lv_dbg_layout_calls,
                        (unsigned long)lv_dbg_refr_calls,
                        lv_dbg_class ? lv_dbg_class : "-");
            next_raw += 5000;
          }

        usleep(1000);
      }

    s_lvgl_lock_owner = self;
    s_lvgl_lock_depth = 1;
}

void lvgl_ui_unlock(void)
{
    if (s_lvgl_lock_depth > 1)
      {
        s_lvgl_lock_depth--;
        return;
      }

    s_lvgl_lock_depth = 0;
    s_lvgl_lock_owner = (pthread_t)0;  /* before release: no stale-owner fast path */
    pthread_mutex_unlock(&s_lvgl_lock);
}

/* Locked lv_async_call(): returns 0 on success, -1 on failure. */
int lvgl_ui_async_call(void (*cb)(void*), void* data)
{
    int ret;

    lvgl_ui_lock();
    ret = lv_async_call((lv_async_cb_t)cb, data);
    lvgl_ui_unlock();

    return ret == LV_RESULT_OK ? 0 : -1;
}

/* Self-test toggle: define to inject one card per reply category on
 * show_chat, for visual verification of all four color-bar variants.
 * Leave undefined in normal builds. */
/* #define LVGL_UI_CARD_SELFTEST */
/* Verified 2026-08-17: static lv_font_misans_12_cjk renders full CJK (no
 * tofu) and does NOT crash the heap — self-test confirmed via screendump.
 * Also verified: 1024-byte msg buffer + UTF-8-aware truncation shows full
 * ~568-byte Agent summaries without mid-char cut. */

/* Layout constants for 466x466 round screen */
#define LVGL_UI_SCREEN_W 466
#define LVGL_UI_SCREEN_H 466
#define LVGL_UI_CHAT_GAP 8
#define LVGL_UI_BUBBLE_RADIUS 12
#define LVGL_UI_BUBBLE_PAD 8
/* Max message text stored per bubble.  1024 bytes ≈ 340 simplified-Chinese
 * chars (UTF-8, 3 bytes/char) — enough for typical Agent LLM summaries.
 * Truncation is UTF-8-aware (chat_view_copy_text) so a multi-byte char is
 * never split in half (which would render a replacement glyph at the cut). */
#define LVGL_UI_MSG_MAX_LEN 1024
#define CHAT_HISTORY_MAX 20

/* 屏幕切换淡入时长(ms)。硬 lv_screen_load() 的整帧 SPI flush 会与面板 60Hz
 * GRAM 扫描竞争,表现为从上到下的白底刷屏;改用淡入交叉渐变分散到多帧,
 * 把刷屏变成优雅的渐隐渐显。cleanup 路径(删屏前)必须保留硬切换,不能淡入。 */
#define SCREEN_FADE_MS 500

/* ── Enhanced Agent card styling ─────────────────────────────── */
/* Agent replies render as a richer card: title bar + divider +
 * left status color bar + body.  The color bar reflects a coarse
 * classification of the reply (conclusion / structured / jira /
 * plain) so the user gets a glanceable cue on the round screen. */
#define LVGL_UI_CARD_TITLE "Agent"
#define LVGL_UI_CARD_TITLEBAR_H 18
#define LVGL_UI_CARD_COLOR_BAR_W 4
#define LVGL_UI_CARD_GAP 4

/* Status palette (HachimoDock 柔和低饱和, see ui_palette.h) */
#define LVGL_UI_CARD_COLOR_CONCLUSION UI_PAL_DONE  /* green — yes/no/建议 */
#define LVGL_UI_CARD_COLOR_STRUCTURED UI_PAL_THINK /* blue  — MERGED/状态: */
#define LVGL_UI_CARD_COLOR_JIRA        UI_PAL_WAIT  /* amber — More Info/经办人 */
#define LVGL_UI_CARD_COLOR_PLAIN       UI_PAL_IDLE  /* grey  — default */

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

    /* Approval screen (remote permission request pending) */
    lv_obj_t* approval_screen;
    lv_obj_t* approval_tool;    /* tool name label */
    lv_obj_t* approval_hint;    /* hint text label */
    bool approval_visible;
    lv_obj_t* approval_prev_screen;
    bool approval_was_standby;  /* clock was up when the prompt arrived */

    /* Notification page (Gerrit / Jira notices).  The round panel is too small
     * for a top toast (the body was clipped), so a notice gets its own screen,
     * held for LVGL_UI_NOTICE_MS and exiting on any user key.  It yields to the
     * approval page: while an approval is pending, notices are stashed here and
     * the newest one is shown when the approval clears (later replaces older).
     * Deliberately NOT dropped: the user wants it replayed after the approval
     * is answered, not lost. */
    lv_obj_t* notice_screen;
    lv_obj_t* notice_title;
    lv_obj_t* notice_body;
    lv_obj_t* notice_bar;
    lv_timer_t* notice_timer;
    volatile bool notice_visible;
    lv_obj_t* notice_prev_screen;
    bool notice_was_standby;   /* clock was up when the notice arrived */
    bool notice_pending;        /* a stashed notice waits for the approval */
    char notice_pending_title[64];
    char notice_pending_body[512];
    lv_color_t notice_pending_color;
    lv_color_t notice_last_color; /* bar colour of the notice on screen */

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
static pthread_t s_lv_wd_tid;
static volatile bool s_lv_loop_running = false;
static lv_nuttx_result_t s_lv_result;

/* LVGL log sink (see the registration in lv_loop_thread). */
static void lvgl_log_cb(lv_log_level_t level, const char* buf)
{
    (void)level;
    syslog(LOG_INFO, "[lvgl] %s", buf);
}

/* Frame counter polled by the UI watchdog.  Bumped by the LVGL loop. */
static volatile uint32_t s_lv_frames;

/* Watchdog ticks.  Printed by the LVGL thread's heartbeat as well as by the
 * watchdog itself: if the watchdog's own console write is ever lost, a
 * rising tick count in the heartbeat still proves the thread is scheduled. */
static volatile uint32_t s_wd_ticks;

/* How far the watchdog thread got.  1 = entered, 2 = priority set,
 * 3 = raw console write returned, 4 = syslog returned.  Frozen at 1 means
 * the thread never ran at all; frozen at 2/3 means it blocks in console
 * output.  Read from the heartbeat, so it needs no output of its own. */
static volatile int s_wd_stage;
static volatile int s_wd_prio = -1;
static volatile int s_wd_setrc = -2;

/* UI watchdog (BUG-20261021-01).  Two very different failures look the
 * same in the log: the LVGL thread wedging inside a widget call, and the
 * whole AP dying.  The watchdog runs above the LVGL thread's priority, so
 * if it keeps printing while the frame counter is frozen, the scheduler
 * and the console are alive and the LVGL thread is the stuck one; if it
 * goes silent too, the AP or the console path itself is gone. */
static void* ui_watchdog_thread(void* arg)
{
    struct sched_param got;
    char bfr[192];
    int n;
    uint32_t last = 0;
    int stall = 0;

    (void)arg;

    /* First statement on purpose: a store, no syscall, no console.  If the
     * heartbeat reports stage 1 forever, this thread never got scheduled at
     * all — which is a different bug from one that blocks in console output
     * (stage 2/3). */
    s_wd_stage = 1;

    /* Priority is set at pthread_create() time via the attribute (see
     * lv_loop_thread below), NOT here.  A self sched_setparam(0) to the
     * highest priority hangs on this config (observed stage 1 forever),
     * so read the effective priority back instead of raising it. */
    memset(&got, 0, sizeof(got));
    int grc = sched_getparam(0, &got);
    s_wd_prio = (grc == 0) ? got.sched_priority : -1;
    s_wd_setrc = 0;
    s_wd_stage = 2;

    /* Unconditional beacon through BOTH channels: as long as these keep
     * coming, the scheduler and the serial output are alive and any silence
     * elsewhere is a stuck thread, not a dead AP.  Sending it twice keeps
     * the answer readable even if one of the two paths is wedged.  The
     * priority read-back says whether the thread really runs at 20. */
    n = snprintf(bfr, sizeof(bfr),
                 "\r\n[lvgl_ui] wd up setrc=%d prio=%d getrc=%d\r\n",
                 s_wd_setrc, s_wd_prio, grc);
    if (n > 0)
      {
        ssize_t ign = write(1, bfr, (size_t)n);
        (void)ign;
      }
    s_wd_stage = 3;

    syslog(LOG_INFO, "[%s] wd up setrc=%d prio=%d getrc=%d\n",
           TAG, s_wd_setrc, s_wd_prio, grc);
    s_wd_stage = 4;

    while (s_lv_loop_running)
      {
        uint32_t cur;

        usleep(5000 * 1000);
        if (!s_lv_loop_running)
          {
            break;
          }

        s_wd_ticks++;

        cur = s_lv_frames;
        if (cur == last)
          {
            stall += 5;
          }
        else
          {
            last = cur;
            stall = 0;
          }

        /* `tr` is the last probe step the UI thread reached: a frozen frame
         * count plus a frozen tr names the widget call that never returned.
         * `lvl` is the patched-LVGL layout progress: outer passes / object
         * visits / refr_size calls, and the class being worked on.  While the
         * frames are frozen, growing lvl = spinning inside layout; static lvl
         * = blocked inside one call. */
        n = snprintf(bfr, sizeof(bfr),
                     "\r\n[lvgl_ui] wd frames %lu stall %d tr %d lvl %lu/%lu/%lu %s\r\n",
                     (unsigned long)cur, stall, s_ui_trace_step,
                     (unsigned long)lv_dbg_layout_outer,
                     (unsigned long)lv_dbg_layout_calls,
                     (unsigned long)lv_dbg_refr_calls,
                     lv_dbg_class ? lv_dbg_class : "-");
        if (n > 0)
          {
            ssize_t ign = write(1, bfr, (size_t)n);
            (void)ign;
          }

        if (stall > 0)
          {
            syslog(LOG_WARNING,
                   "[%s] wd: LVGL stalled %ds (frames=%lu tr=%d)\n",
                   TAG, stall, (unsigned long)cur, s_ui_trace_step);
          }
      }

    return NULL;
}

static void* lv_loop_thread(void* arg)
{
    struct sched_param sp;
    uint32_t frames = 0;

    (void)arg;

    /* LV_USE_LOG is on but LV_LOG_PRINTF is off, so every LVGL log line —
     * including "Asserted at expression: ... (Out of memory)" — was being
     * discarded.  Route them to syslog so an assert is visible before
     * lv_assert_handler() takes the system down. */
    lv_log_register_print_cb(lvgl_log_cb);

    /* The loop thread was created with the default pthread priority
     * (inherited), so TLS / LLM dispatch / TTS work preempts it.  A frame
     * that is preempted mid-flush holds lvgl_ui_lock() for that whole
     * time, and every cross-thread LVGL caller (agent reply path, pet
     * state, PTT) blocks behind it.  Run at the UI priority instead. */
    sp.sched_priority = AGENT_LVGL_UI_PRIO;
    int prc = sched_setparam(0, &sp);
    syslog(LOG_INFO, "[%s] LVGL loop prio -> %d (rc=%d)\n",
           TAG, AGENT_LVGL_UI_PRIO, prc);

    /* Stall detector — see ui_watchdog_thread().  Give it an explicit 8 KB
     * stack: CONFIG_PTHREAD_STACK_DEFAULT is 2048 here, which is marginal for
     * one snprintf, and the previous run's watchdog never printed a byte while
     * every other thread kept logging — a too-small stack is the likeliest
     * reason.  The creation result is logged so a silent watchdog can no
     * longer be confused with a wedged scheduler. */
    {
      pthread_attr_t wattr;
      int crc = -1;

      if (pthread_attr_init(&wattr) == 0)
        {
          struct sched_param wsp;

          (void)pthread_attr_setstacksize(&wattr, 8192);
          wsp.sched_priority = 20;   /* above the LVGL loop (45) */
          (void)pthread_attr_setschedparam(&wattr, &wsp);
          (void)pthread_attr_setschedpolicy(&wattr, SCHED_FIFO);
          (void)pthread_attr_setinheritsched(&wattr, PTHREAD_EXPLICIT_SCHED);
          crc = pthread_create(&s_lv_wd_tid, &wattr, ui_watchdog_thread, NULL);
          (void)pthread_attr_destroy(&wattr);
        }

      syslog(LOG_INFO, "[%s] watchdog create rc=%d\n", TAG, crc);
    }

    while (s_lv_loop_running) {
        uint32_t idle;
        struct timespec t0;
        struct timespec t1;
        long ms;

        /* Serialize with cross-thread lv_async_call()/lv_timer_create()
         * users.  CONFIG_LV_USE_OS=0 means LVGL does no locking itself. */
        clock_gettime(CLOCK_MONOTONIC, &t0);
        lvgl_ui_lock();
        idle = lv_timer_handler();
        lvgl_ui_unlock();
        s_lv_frames++;
        clock_gettime(CLOCK_MONOTONIC, &t1);

        ms = (t1.tv_sec - t0.tv_sec) * 1000
           + (t1.tv_nsec - t0.tv_nsec) / 1000000;

        /* One slow frame is the first symptom of a wedged render/flush;
         * log it while the board is still talking. */
        if (ms >= 300)
          {
            syslog(LOG_WARNING, "[%s] handler %ldms idle=%u\n",
                   TAG, ms, (unsigned)idle);
          }

        /* Heartbeat: ~every 5 s at the normal cadence.  Silence after a
         * hang means the LVGL thread itself stopped, not just the caller. */
        if (++frames % 20 == 0)
          {
            char hb[192];
            int hn;

#if defined(CONFIG_STACK_COLORATION)
            /* Stack high-water for this thread: if the reply-card build
             * overflows the 64 KB LVGL stack, the log simply stops with no
             * message, so surface the headroom while the board still talks. */
            struct stackinfo_s si = {0};
            size_t used = 0;

            if (nxsched_get_stackinfo(0, &si) == 0 && si.adj_stack_size > 0)
              {
                used = up_check_tcbstack(nxsched_self(), si.adj_stack_size);
              }

            hn = snprintf(hb, sizeof(hb),
                          "\r\n[lvgl_ui] hb frames stack %lu %lu wd %lu st %d pr %d "
                          "lvl %lu/%lu/%lu\r\n",
                          (unsigned long)frames, (unsigned long)used,
                          (unsigned long)s_wd_ticks, s_wd_stage, s_wd_prio,
                          (unsigned long)lv_dbg_layout_outer,
                          (unsigned long)lv_dbg_layout_calls,
                          (unsigned long)lv_dbg_refr_calls);
#else
            hn = snprintf(hb, sizeof(hb),
                          "\r\n[lvgl_ui] hb frames %lu 0 wd %lu st %d pr %d\r\n",
                          (unsigned long)frames, (unsigned long)s_wd_ticks,
                          s_wd_stage, s_wd_prio);
#endif
            if (hn > 0)
              {
                ssize_t ign = write(1, hb, (size_t)hn);
                (void)ign;
              }
          }

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
    uint32_t bar_color;   /* 0 = auto-classify via chat_view_classify_reply */
} async_msg_t;

/* ── Forward declarations ─────────────────────────────────── */

static void chat_view_add_message(const char* text, bool is_user,
                                  uint32_t bar_color);
static void chat_view_add_message_async_cb(void* data);
static void chat_view_scroll_to_bottom(void);
static void chat_view_trim_history(void);
static void show_screen_async_cb(void* data);
static void approval_screen_build(void);
static void set_approval_async_cb(void* data);
static void clear_approval_async_cb(void* data);
static void scroll_async_cb(void* data);
static void notice_screen_build(void);
static void notice_hide(const char* why);
static void notice_show_locked(const char* title, const char* body,
                               lv_color_t bar_color);
static void notice_stash_visible(void);
static void notice_maybe_show_pending(void);

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

/* Screen dwell policy (settled 2026-11-09):
 *
 *   Any page — chat or pet home — stays up until the user stops interacting
 *   for STANDBY_TIMEOUT_MS, then the dim clock takes over.  There is no
 *   chat → pet auto-yield any more: an answer used to disappear 8 s after it
 *   arrived, which read as the screen yanking itself away mid-read.
 *
 *   Pet home → chat is a deliberate gesture: an S2/S3 tap (lvgl_ui_channel_
 *   scroll) when the transcript has something in it.  An empty conversation
 *   stays on the pet page instead of showing a blank dark page.
 */

/* Agent lifecycle state, mirrored by set_pet_state().  The standby timer
 * reads it to postpone standby while the agent is busy (non-idle). */
static int s_last_pet_state = 0;

/* WAITING (4) is an overlay, not a lifecycle state: the approval page is shown
 * while the agent waits for a decision, then the pet must go back to whatever
 * it was doing (EXECUTING while a tool blocks on the prompt, IDLE for a
 * debug-only rpending).  Save that state on the first set_approval and restore
 * it once, when the prompt clears. */
static int s_approval_prev_pet_state = 0;
static int s_approval_pet_saved = 0;

#ifdef CONFIG_AI_AGENT_STANDBY_UI
/* ── Standby dim clock (M7 待机界面) ────────────────────────────── */
/* After STANDBY_TIMEOUT_MS with no real activity — no chat bubble, no PTT,
 * no pet-state change, no page switch, no approval, no scroll — and no busy
 * agent state (only IDLE qualifies; WAITING is a pending approval the device
 * is waiting on and stays awake), fade the foreground over to a dim digital
 * clock (standby_view).  Any of those activity points calls ui_activity_reset(),
 * which both restarts the countdown and (if the clock is up) fades it away,
 * restoring the screen that was up before standby. */
#define STANDBY_TIMEOUT_MS 60000
static lv_timer_t *s_standby_timer = NULL;
static uint32_t    s_last_activity_tick = 0;

static void ui_activity_reset(void)
{
    s_last_activity_tick = lv_tick_get();
    if (standby_view_is_visible())
        standby_view_hide();   /* async fade back to the previous screen */
}

static void standby_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_state.initialized || !s_state.running) return;
    if (standby_view_is_visible()) return;
    /* An alert page owns the foreground: never fade the clock over it, or the
     * first key press is eaten by the wake gate instead of answering. */
    if (s_state.approval_visible || s_state.notice_visible) return;
    /* A busy agent (listening / thinking / executing, an error being shown, or
     * a permission prompt awaiting a decision) keeps postponing standby so the
     * animation is never interrupted mid-task.  A pending approval is a live
     * question for the human: dimming it to the clock here swallowed the first
     * key (wake only) and made the approval page feel unresponsive, so WAITING
     * now postpones standby like every other non-idle state. */
    if (s_last_pet_state != PET_STATE_IDLE)
      {
        s_last_activity_tick = lv_tick_get();
        return;
      }
    if ((lv_tick_get() - s_last_activity_tick) >= STANDBY_TIMEOUT_MS)
      {
        syslog(LOG_INFO, "[%s] idle %ums → standby clock\n", TAG,
               STANDBY_TIMEOUT_MS);
        standby_view_show();
      }
}
#else
static void ui_activity_reset(void) {}
#endif

int lvgl_ui_channel_wake_from_standby(void)
{
#ifdef CONFIG_AI_AGENT_STANDBY_UI
    if (!standby_view_is_visible())
        return 0;
    ui_activity_reset();   /* hides the clock and restarts the idle countdown */
    return 1;
#else
    return 0;
#endif
}

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

        /* The event loop thread is started at the END of this function:
         * widget creation below runs on this thread, and the loop thread
         * would otherwise touch the same unsynchronized LVGL globals
         * (timer list, object tree, lv_malloc heap) concurrently. */
        s_lv_owned = true;
        syslog(LOG_INFO, "[%s] LVGL self-init OK\n", TAG);
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
            syslog(LOG_INFO, "[%s] display %ldx%ld\n", TAG,
                   (long)disp_w, (long)disp_h);
        }
    }

    /* Round-face geometry.  The BK7258 JD9853 panel is a round watch face
     * (Ø224) inscribed in a 240×296 rectangle centered at (120,148), leaving
     * 8px side bezels and 36px top/bottom bezels.  The 466 reference is a full
     * square face (Ø466, no bezel).  We derive the inscribed circle from the
     * panel and lay out every widget inside it, so nothing spills into the
     * bezel or off screen on either board. */
    bool round_panel = (disp_w == 240 && disp_h == 296);
    int32_t round_d  = round_panel ? 224 : ((disp_w < disp_h) ? disp_w : disp_h);
    int32_t cx       = disp_w / 2;
    int32_t cy       = disp_h / 2;
    int32_t face_r   = round_d / 2;

    /* Inner gutters.  The old 6%/14% split (13/31 px) was derived from the
     * inscribed Ø224 circle and left a visible black frame around every
     * bubble, plus a 31 px dead band above the first message.  The panel is a
     * 240x296 rectangle whose corners are what the round case clips (the list
     * scrolls to the bottom, where an inscribed circle would be only ~90 px
     * wide, and bubbles render fine there today) — so lay the column out on
     * the rectangle with a hairline gutter instead. */
    int32_t side_m   = round_panel ? 4  : (round_d * 6)  / 100;  /* side gutter */
    int32_t top_m    = round_panel ? 8  : (round_d * 14) / 100;  /* top gutter  */
    int32_t bot_m    = round_panel ? 2  : 0;                     /* bottom gutter */

    /* Create main screen with dark background */
    s_state.screen = lv_obj_create(NULL);
    if (!s_state.screen) {
        syslog(LOG_ERR, "[%s] screen create failed\n", TAG);
        ret = -EIO;
        goto cleanup;
    }

    lv_obj_set_style_bg_color(s_state.screen, lv_color_hex(UI_PAL_BG), 0);
    lv_obj_set_style_bg_opa(s_state.screen, LV_OPA_COVER, 0);
    /* Do NOT call lv_screen_load() here — we load the screen on
     * demand when the user activates the chat UI, to avoid
     * hijacking the system's current screen at boot. */

    /* Default text font: the app-owned statically-compiled MiSans-14 CJK font
     * (3755 GB2312-1 common chars + ASCII + fullwidth punctuation, baked into
     * the binary via lv_font_conv — zero runtime malloc/rasterization) so
     * Agent reply Chinese text renders on qemu with full coverage and cannot
     * corrupt the heap.  Falls back to SimSun-16 (1000 chars) then Montserrat. */
    lv_obj_set_style_text_font(s_state.screen, &lv_font_misans_12_cjk, 0);

    /* Step 5: Create Chat View container.  On the 240x296 panel the column
     * occupies the whole rectangle (hairline gutter) — see the gutter comment
     * above for why the inscribed-circle inset was wrong there.  On other
     * panels it stays inside the inscribed circle. */
    int32_t chat_list_w;
    int32_t chat_list_x;
    int32_t chat_list_y;
    if (round_panel) {
        chat_list_w = disp_w - 2 * side_m;
        chat_h = disp_h - top_m - bot_m;
        chat_list_x = side_m;
        chat_list_y = top_m;
    } else {
        chat_list_w = round_d - 2 * side_m;
        chat_h = 2 * face_r - top_m - bot_m;
        chat_list_x = cx - chat_list_w / 2;
        chat_list_y = cy - face_r + top_m;
    }

    s_state.chat_list = lv_obj_create(s_state.screen);
    if (!s_state.chat_list) {
        syslog(LOG_ERR, "[%s] chat_list create failed\n", TAG);
        ret = -EIO;
        goto cleanup;
    }

    lv_obj_set_size(s_state.chat_list, chat_list_w, chat_h);
    lv_obj_set_pos(s_state.chat_list, chat_list_x, chat_list_y);
    lv_obj_set_scroll_dir(s_state.chat_list, LV_DIR_VER);
    lv_obj_set_flex_flow(s_state.chat_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_state.chat_list, LVGL_UI_CHAT_GAP, 0);
    lv_obj_set_style_bg_opa(s_state.chat_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_state.chat_list, 0, 0);
    lv_obj_set_style_pad_left(s_state.chat_list, 2, 0);
    lv_obj_set_style_pad_right(s_state.chat_list, 2, 0);
    lv_obj_set_style_pad_top(s_state.chat_list, 2, 0);
    lv_obj_set_style_pad_bottom(s_state.chat_list, 2, 0);
    lv_obj_set_scrollbar_mode(s_state.chat_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_state.chat_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    /* Step 6: Initialize chat history and mutex */
    memset(&s_state.history, 0, sizeof(chat_history_t));
    pthread_mutex_init(&s_state.lock, NULL);

    s_state.initialized = true;
    syslog(LOG_INFO, "[%s] init OK\n", TAG);

    /* Build the approval screen (remote permission request) — a hidden,
     * independent screen reached via lvgl_ui_channel_set_approval. */
    approval_screen_build();

    /* Build the notification page (Gerrit/Jira notices) — hidden until
     * lvgl_ui_channel_show_notice; approval outranks it. */
    notice_screen_build();

#ifdef CONFIG_AI_AGENT_PET_STATUS
    /* Build the pet status ring home screen (M7 差异化页).  It is NOT loaded
     * here — cp_ui_main (or the AP via RPMsg) decides when to show it. */
    (void)pet_status_view_init();
    /* Surface the pet as the boot/home screen immediately.  On CP this is
     * idempotent with cp_ui_main's explicit show(); on qemu (no cp_ui_main,
     * no AP→CP RPMsg) it is the only thing that brings the pet up at boot. */
    pet_status_view_show();
#endif
#ifdef CONFIG_AI_AGENT_STANDBY_UI
    /* M7 待机界面: build the dim digital clock screen (NOT loaded — the pet is
     * the cold-start home screen; the standby timer fades it in after
     * STANDBY_TIMEOUT_MS of idle).  Init here so it is ready to show without a
     * per-call create latency.  1 s period polls the idle condition. */
    (void)standby_view_init();
    s_standby_timer = lv_timer_create(standby_timer_cb, 1000, NULL);
    if (s_standby_timer)
      {
        lv_timer_set_repeat_count(s_standby_timer, -1);
      }
    ui_activity_reset();
#endif

    /* All widgets/timers exist now; start the LVGL event loop.  LVGL
     * rendering + CJK glyph rasterization needs a deep stack — the default
     * pthread stack overflows (recursive assert), so size it explicitly. */
    if (s_lv_owned && !s_lv_loop_running) {
        pthread_attr_t lv_attr;

        s_lv_loop_running = true;
        pthread_attr_init(&lv_attr);
        pthread_attr_setstacksize(&lv_attr, AGENT_LVGL_UI_STACK);
        if (pthread_create(&s_lv_loop_tid, &lv_attr, lv_loop_thread, NULL) != 0) {
            syslog(LOG_ERR, "[%s] LVGL loop thread create failed\n", TAG);
            lv_nuttx_deinit(&s_lv_result);
            s_lv_loop_running = false;
            s_lv_owned = false;
            ret = -EIO;
            goto cleanup;
        }
        syslog(LOG_INFO, "[%s] LVGL loop thread started\n", TAG);
    }

    return 0;

cleanup:
    /* Release resources in reverse order (coding-2) */

    if (s_state.screen) {
        lv_obj_del(s_state.screen);
        s_state.screen = NULL;
        /* Child widgets are deleted with the screen */
        s_state.chat_list = NULL;
    }
    if (s_state.approval_screen) {
        lv_obj_del(s_state.approval_screen);
        s_state.approval_screen = NULL;
        s_state.approval_tool = NULL;
        s_state.approval_hint = NULL;
    }
    if (s_state.notice_screen) {
        lv_obj_del(s_state.notice_screen);
        s_state.notice_screen = NULL;
        s_state.notice_title = NULL;
        s_state.notice_body = NULL;
        s_state.notice_bar = NULL;
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

    /* Mark as not running first — prevents callbacks (PTT,
     * chat_view_add_message) from touching widgets during teardown. */
    s_state.running = false;

    /* Schedule cleanup in the LVGL thread to avoid racing with the
     * miwear/launcher event loop.  Direct lv_obj_del from the
     * shutdown thread causes a MemManage fault because the LVGL
     * thread may be mid-render on the same objects. */
    sem_init(&s_stop_sem, 0, 0);
    lvgl_ui_async_call(stop_screen_async_cb, NULL);

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
     * lv_font_misans_12_cjk (preferred: 3755 GB2312-1 common chars, baked into
     * the binary by lv_font_conv — zero runtime malloc, zero rasterization,
     * cannot corrupt the heap).  No runtime TTF loading here.
     *
     * History: we previously used lv_tiny_ttf_create_data() with a runtime
     * MiSans TTF, but stb_truetype's MakeGlyphBitmap write path overran its
     * output buffer on certain CJK glyphs and corrupted adjacent NuttX mm heap
     * chunk metadata, leading to recursive-assert reboots (mm_foreach /
     * nxsched_add_readytorun / nxsem_post_slow in the timer IRQ).  Subsetting
     * to 857 KB only delayed the crash; the root cause is the rasterizer, so
     * we moved to a pre-rasterized static font.  See lv_font_misans_12_cjk.c
     * and the fontgen notes in docs. */
    /* Already set in init(); nothing to do at show time. */
    syslog(LOG_INFO, "[%s] using static MiSans-16 CJK font\n", TAG);

    syslog(LOG_INFO, "[%s] loading chat screen\n", TAG);

    /* Save the current screen so we can restore it on stop.
     * The launcher (miwear) owns this screen — we must not delete it. */
    s_state.prev_screen = lv_screen_active();

    lv_screen_load_anim(s_state.screen, LV_SCR_LOAD_ANIM_FADE_IN,
                        SCREEN_FADE_MS, 0, false);
    s_state.screen_visible = true;

#ifdef LVGL_UI_CARD_SELFTEST
    /* Debug self-test: inject one card per reply category so all four
     * color-bar variants render on a single show_chat.  Remove the
     * -DLVGL_UI_CARD_SELFTEST define to disable. */
    syslog(LOG_INFO, "[%s] CARD_SELFTEST: injecting sample cards\n", TAG);
    chat_view_add_message("这个提交已 MERGED,状态: 正常合入", false, 0);
    chat_view_add_message("Jira issue FEEDBACK-149182,经办人: 张文海,More Info", false, 0);
    chat_view_add_message("值得合入,建议采纳,yes", false, 0);
    chat_view_add_message("这是一条普通回复,用于展示默认灰色色条", false, 0);
    chat_view_add_message("你好,这是一条用户消息", true, 0);
#endif
}

void lvgl_ui_channel_show(void)
{
    if (!s_state.initialized || !s_state.running) {
        syslog(LOG_ERR, "[%s] show: channel not ready\n", TAG);
        return;
    }

    ui_activity_reset();

    if (s_state.screen_visible) {
        return;
    }

    /* Schedule screen load in LVGL thread to avoid cross-thread
     * invalidation assertion. */
    lvgl_ui_async_call(show_screen_async_cb, NULL);
}

/* Async: drop the whole conversation, then bring the (now empty) chat screen
 * up. Used when the gateway starts a new session — the turns on screen belong
 * to the previous generation, and leaving them there would present old answers
 * as part of the new one. Every bubble lives under chat_list and a history slot
 * only caches a pointer into it, so one lv_obj_clean plus a reset is complete. */
static void chat_view_clear_async_cb(void* data)
{
    (void)data;
    chat_history_t* h = &s_state.history;
    int i;

    if (!s_state.initialized || !s_state.running) {
        return;
    }

    if (s_state.chat_list != NULL) {
        lv_obj_clean(s_state.chat_list);
    }
    for (i = 0; i < CHAT_HISTORY_MAX; i++) {
        h->msgs[i].bubble = NULL;
        h->msgs[i].text[0] = '\0';
    }
    h->head = 0;
    h->count = 0;

    /* "Jump to the new session": the empty chat page is the visible result. */
    ui_activity_reset();
    if (!s_state.screen_visible) {
        show_screen_async_cb(NULL);
    }
}

int lvgl_ui_channel_clear_history(void)
{
    if (!s_state.initialized || !s_state.running) {
        syslog(LOG_ERR, "[%s] clear_history: channel not ready\n", TAG);
        return -EINVAL;
    }

    return lvgl_ui_async_call(chat_view_clear_async_cb, NULL);
}

int lvgl_ui_channel_send(const char* text)
{
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

    ui_activity_reset();

    /* Auto-show chat screen on first Agent reply */
    if (!s_state.screen_visible) {
        lvgl_ui_channel_show();
    }

    /* Reply-path probes (BUG-20261021-01): tr40..tr43 are emitted by the
     * agent/CLI thread, not the LVGL thread, so they show whether a silent
     * console is this thread blocking on the UI lock or something earlier. */
    UITRACE(40);

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

    UITRACE(41);
    lvgl_ui_async_call(chat_view_add_message_async_cb, payload);
    UITRACE(42);

    /* TTS is blocking — run after scheduling the UI update.  Offline it can
     * only fail, but only after a ~20 s network timeout that stalls the
     * outbound dispatch thread (and the "no network" explanation that is
     * already on screen).  Skip it when there is no network. */
    if (network_is_connected()) {
        int ret = voice_channel_speak(text);
        if (ret != 0) {
            syslog(LOG_ERR, "[%s] voice_channel_speak failed (rc=%d)\n", TAG, ret);
        }
    }

    return 0;
}

/* Extended send: same as send() but lets the caller pin the card left-bar
 * color (0 = keep the heuristic classifier).  Used by the AP→CP UI RPMsg
 * SEND_EX path when the agent already knows the reply category. */
int lvgl_ui_channel_send_ex(const char* text, uint32_t bar_color)
{
    async_msg_t* payload;

    if (!text || text[0] == '\0') {
        syslog(LOG_ERR, "[%s] send_ex: invalid text\n", TAG);
        return -EINVAL;
    }
    if (!s_state.initialized || !s_state.running) {
        syslog(LOG_ERR, "[%s] send_ex: channel not ready\n", TAG);
        return -EINVAL;
    }

    ui_activity_reset();

    if (!s_state.screen_visible) {
        lvgl_ui_channel_show();
    }

    payload = malloc(sizeof(async_msg_t));
    if (!payload) {
        syslog(LOG_ERR, "[%s] send_ex: payload alloc failed\n", TAG);
        return -ENOMEM;
    }
    chat_view_copy_text(payload->text, text, LVGL_UI_MSG_MAX_LEN);
    payload->is_user = false;
    payload->bar_color = bar_color;

    lvgl_ui_async_call(chat_view_add_message_async_cb, payload);
    return 0;
}

/* User bubble: echo the recognized PTT utterance as a right-aligned blue
 * bubble.  No TTS (we don't speak the user's own words back).  bar_color is
 * unused for user bubbles (they have no card color bar). */
int lvgl_ui_channel_send_user(const char* text)
{
    async_msg_t* payload;

    if (!text || text[0] == '\0') {
        syslog(LOG_ERR, "[%s] send_user: invalid text\n", TAG);
        return -EINVAL;
    }
    if (!s_state.initialized || !s_state.running) {
        syslog(LOG_ERR, "[%s] send_user: channel not ready\n", TAG);
        return -EINVAL;
    }

    ui_activity_reset();

    if (!s_state.screen_visible) {
        lvgl_ui_channel_show();
    }

    payload = malloc(sizeof(async_msg_t));
    if (!payload) {
        syslog(LOG_ERR, "[%s] send_user: payload alloc failed\n", TAG);
        return -ENOMEM;
    }
    chat_view_copy_text(payload->text, text, LVGL_UI_MSG_MAX_LEN);
    payload->is_user = true;
    payload->bar_color = 0;

    lvgl_ui_async_call(chat_view_add_message_async_cb, payload);
    return 0;
}

/* ── External PTT state (AP→CP RPMsg SET_PTT) ─────────────────── */

/* Heap-allocated state for async set_ptt (single int fits in a void* but
 * keep a small struct for clarity/future fields). */
typedef struct {
    int state;
} async_ptt_t;

static void set_ptt_async_cb(void* data)
{
    async_ptt_t* p = (async_ptt_t*)data;
    if (!p) {
        return;
    }
    if (!s_state.initialized || !s_state.running) {
        free(p);
        return;
    }

    switch (p->state) {
    case LVGL_UI_PTT_RECORDING:
        s_state.is_recording = true;
        s_state.is_processing = false;
#ifdef CONFIG_AI_AGENT_PET_STATUS
        /* Mirror PTT onto the pet: a recording session reads as "listening". */
        pet_status_view_set_state(PET_STATE_LISTENING);
#endif
        break;
    case LVGL_UI_PTT_PROCESSING:
        s_state.is_recording = false;
        s_state.is_processing = true;
#ifdef CONFIG_AI_AGENT_PET_STATUS
        pet_status_view_set_state(PET_STATE_THINKING);
#endif
        break;
    case LVGL_UI_PTT_IDLE:
    default:
        s_state.is_recording = false;
        s_state.is_processing = false;
#ifdef CONFIG_AI_AGENT_PET_STATUS
        pet_status_view_set_state(PET_STATE_IDLE);
#endif
        break;
    }
    free(p);
}

int lvgl_ui_channel_set_ptt(int state)
{
    async_ptt_t* payload;

    if (!s_state.initialized || !s_state.running) {
        return -EINVAL;
    }
    ui_activity_reset();
    /* Coerce unknown values to IDLE. */
    if (state != LVGL_UI_PTT_IDLE && state != LVGL_UI_PTT_RECORDING &&
        state != LVGL_UI_PTT_PROCESSING) {
        state = LVGL_UI_PTT_IDLE;
    }

    payload = malloc(sizeof(async_ptt_t));
    if (!payload) {
        syslog(LOG_ERR, "[%s] set_ptt: payload alloc failed\n", TAG);
        return -ENOMEM;
    }
    payload->state = state;
    lvgl_ui_async_call(set_ptt_async_cb, payload);
    return 0;
}

/* Drive the pet status ring from the AP agent's lifecycle state (M7 差异化页).
 * On the CP/qemu core this delegates to pet_status_view; on the AP UI_RPMSG
 * shim (ui_rpmsg_client.c) the same symbol sends UI_RPMSG_SET_PET_STATE. */
int lvgl_ui_channel_set_pet_error(const char *reason)
{
    if (reason && reason[0] != '\0') {
#ifdef CONFIG_AI_AGENT_PET_STATUS
        /* Queued first so the caption is ready when the state applies. */
        pet_status_view_set_error_text(reason);
#endif
    }
    return lvgl_ui_channel_set_pet_state(PET_STATE_ERROR);
}

int lvgl_ui_channel_set_pet_state(int state)
{
    int rc = -ENOSYS;
    if (state < 0 || state > PET_STATE_ERROR) {
        return -EINVAL;
    }
#ifdef CONFIG_AI_AGENT_PET_STATUS
    pet_status_view_set_state((enum pet_state)state);
    rc = 0;
#endif
    (void)rc;
    if (state != s_last_pet_state)
      {
        s_last_pet_state = state;
        ui_activity_reset();   /* a genuine lifecycle change is activity */
      }
    return rc;
}

/* Switch the round-screen page foreground: page 0 is the pet home screen (the
 * only page; the former particle-flow page was removed).  Kept as a command so
 * `set_page 0` still works as an explicit "back to the pet / wake" action.  The
 * chat screen is intentionally excluded — it auto-preempts on send/send_user
 * and yields back to the pet on idle.  Dispatched async on the LVGL thread. */
int lvgl_ui_channel_set_page(int page)
{
    if (page != 0) {
        return -EINVAL;
    }
    ui_activity_reset();
    /* Switching to the pet means the chat screen no longer owns the
     * foreground; keep the flag honest so the next scroll or Agent reply can
     * bring the transcript back. */
    s_state.screen_visible = false;
#ifdef CONFIG_AI_AGENT_PET_STATUS
    pet_status_view_show();
    return 0;
#else
    return -ENOSYS;
#endif
}



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

    /* Entry/exit markers: if a hang reports "add msg" without "add msg
     * done", the fault is inside widget construction/rendering, not in
     * the caller or the lock. */
    syslog(LOG_INFO, "[%s] async: add msg (%u bytes)\n",
           TAG, (unsigned)strlen(payload->text));

    chat_view_add_message(payload->text, payload->is_user, payload->bar_color);

    syslog(LOG_INFO, "[%s] async: add msg done\n", TAG);

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

    UITRACE(20);
    /* Title bar: a thin row holding the status dot + title text */
    title_bar = lv_obj_create(bubble);
    UITRACE(21);
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
    UITRACE(22);

    /* Status dot — colored circle reflecting the reply category */
    status_dot = lv_obj_create(title_bar);
    lv_obj_remove_flag(status_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(status_dot, 8, 8);
    lv_obj_set_style_radius(status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(status_dot, lv_color_hex(bar_color), 0);
    lv_obj_set_style_bg_opa(status_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(status_dot, 0, 0);
    lv_obj_set_style_pad_all(status_dot, 0, 0);
    UITRACE(23);

    /* Title label */
    title_lbl = lv_label_create(title_bar);
    UITRACE(24);
    lv_label_set_text(title_lbl, LVGL_UI_CARD_TITLE);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(UI_PAL_MUTED), 0);
    UITRACE(25);

    /* Divider line under the title */
    divider = lv_obj_create(bubble);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(divider, lv_pct(100));
    lv_obj_set_height(divider, 1);
    lv_obj_set_style_pad_all(divider, 0, 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_bg_color(divider, lv_color_hex(UI_PAL_LINE), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(divider, 0, 0);
    lv_obj_set_style_pad_ver(divider, 0, 0);
    lv_obj_set_style_margin_top(divider, LVGL_UI_CARD_GAP, 0);
    lv_obj_set_style_margin_bottom(divider, LVGL_UI_CARD_GAP, 0);
    UITRACE(26);

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
    UITRACE(27);

    /* Left color bar (the category indicator).  The 0x27xx markers bisect a
     * freeze inside this block: LVGL object ops cannot block (no console/IPC),
     * so a stop here is a spin and the last marker names the call. */
    color_bar = lv_obj_create(body_row);
    BK_MARK(0x2701);
    lv_obj_remove_flag(color_bar, LV_OBJ_FLAG_SCROLLABLE);
    BK_MARK(0x2702);
    lv_obj_set_width(color_bar, LVGL_UI_CARD_COLOR_BAR_W);
    BK_MARK(0x2703);
    lv_obj_set_height(color_bar, LV_SIZE_CONTENT);
    BK_MARK(0x2704);
    lv_obj_set_style_pad_all(color_bar, 0, 0);
    lv_obj_set_style_border_width(color_bar, 0, 0);
    lv_obj_set_style_bg_color(color_bar, lv_color_hex(bar_color), 0);
    lv_obj_set_style_bg_opa(color_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(color_bar, 2, 0);
    BK_MARK(0x2705);
    UITRACE(28);

    /* Body label — the actual reply text */
    body_lbl = lv_label_create(body_row);
    UITRACE(29);
    lv_label_set_text(body_lbl, text);
    UITRACE(30);
    lv_label_set_long_mode(body_lbl, LV_LABEL_LONG_WRAP);
    UITRACE(31);
    /* The label fills the row's remaining width via flex_grow below; an
     * additional lv_pct(85) width is a conflicting second hint that makes
     * LVGL oscillate between the two (grow-share vs percentage) and never
     * converge inside lv_obj_update_layout().  Let flex_grow own the width. */
    UITRACE(32);
    lv_obj_set_style_text_color(body_lbl, lv_color_hex(UI_PAL_INK), 0);
    lv_obj_set_flex_grow(body_lbl, 1);
    UITRACE(33);
}

static void chat_view_add_message(const char* text, bool is_user,
                                  uint32_t bar_color)
{
    chat_history_t* h = &s_state.history;
    chat_msg_t* slot;
    lv_obj_t* bubble;
    lv_obj_t* lbl;

    /* If history is already full, trim before overwriting */
    if (h->count == CHAT_HISTORY_MAX) {
        chat_view_trim_history();
    }

    UITRACE(is_user ? 10 : 11);

    /* Store message in ring buffer (handles truncation + old bubble deletion) */
    slot = chat_history_add(h, text, is_user);

    /* Create a full-width flex-row wrapper so the bubble can right-align
     * (user) or left-align (agent) inside the vertical chat column.  LVGL
     * flex ignores a child's own `align` style, so alignment is carried by
     * the wrapper's justify axis instead of a per-bubble align property. */
    UITRACE(12);
    lv_obj_t* wrap = lv_obj_create(s_state.chat_list);
    if (!wrap) {
        syslog(LOG_ERR, "[%s] bubble wrapper create failed\n", TAG);
        return;
    }
    lv_obj_remove_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(wrap, lv_pct(100));
    lv_obj_set_height(wrap, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(wrap, 0, 0);
    lv_obj_set_style_border_width(wrap, 0, 0);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wrap,
                          is_user ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    UITRACE(13);
    /* Create bubble container inside the alignment wrapper */
    bubble = lv_obj_create(wrap);
    if (!bubble) {
        syslog(LOG_ERR, "[%s] bubble create failed\n", TAG);
        lv_obj_del(wrap);
        return;
    }

    /* Both bubbles hug their text: a short message reads as a compact chip
     * instead of a full-width bar.  Long text wraps at 80% of the *screen*
     * width (the user asked the bubble to only wrap once it exceeds that cap).
     * The agent card needs a fixed pixel width so its inner lv_pct(100)
     * children (title bar / divider / body row) resolve deterministically
     * without the content-size-vs-percentage oscillation documented in
     * chat_view_build_agent_card(); so we measure the reply once and set an
     * explicit width, clamped to the cap. */
    int32_t disp_w = LVGL_UI_SCREEN_W;
    lv_display_t* mdisp = lv_display_get_default();
    if (mdisp) {
        lv_coord_t rw = lv_display_get_horizontal_resolution(mdisp);
        if (rw > 0) disp_w = rw;
    }
    int32_t bubble_max = (disp_w * 80) / 100;   /* 80% of screen width */

    if (is_user) {
        lv_obj_set_width(bubble, LV_SIZE_CONTENT);
    } else {
        int32_t text_w;
        if (strchr(slot->text, '\n') != NULL) {
            text_w = bubble_max;                 /* multi-line: fill to the cap */
        } else {
            text_w = lv_text_get_width(slot->text, strlen(slot->text),
                                       &lv_font_misans_12_cjk, 0);
        }
        int32_t bw = text_w + 2 * LVGL_UI_BUBBLE_PAD
                   + LVGL_UI_CARD_COLOR_BAR_W + LVGL_UI_CARD_GAP;
        if (bw > bubble_max) bw = bubble_max;
        /* Never narrower than the title bar ("Agent" + status dot). */
        int32_t title_w = lv_text_get_width(LVGL_UI_CARD_TITLE,
                                            strlen(LVGL_UI_CARD_TITLE),
                                            &lv_font_misans_12_cjk, 0)
                        + 8 /* dot */ + LVGL_UI_CARD_GAP
                        + 2 * LVGL_UI_BUBBLE_PAD;
        if (bw < title_w) bw = title_w;
        lv_obj_set_width(bubble, bw);
    }
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bubble, LVGL_UI_BUBBLE_RADIUS, 0);
    lv_obj_set_style_pad_all(bubble, LVGL_UI_BUBBLE_PAD, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);

    if (is_user) {
        /* User bubble: brand-orange background, white text (the wrapper's
         * justify axis right-aligns it in the chat column). */
        lv_obj_set_style_bg_color(bubble, lv_color_hex(UI_PAL_ACCENT), 0);
        lv_obj_set_style_text_color(bubble, lv_color_white(), 0);
        lv_obj_set_style_shadow_width(bubble, 4, 0);
        lv_obj_set_style_shadow_offset_x(bubble, 2, 0);
        lv_obj_set_style_shadow_offset_y(bubble, 4, 0);
        lv_obj_set_style_shadow_color(bubble, lv_color_hex(UI_PAL_CARD_SHADOW), 0);
        lv_obj_set_style_shadow_opa(bubble, LV_OPA_40, 0);
    } else {
        /* Agent card: left-aligned white card (HachimoDock panel) with a
         * dark drop shadow + light border, so it reads as a raised card on
         * the near-black background. */
        lv_obj_set_style_bg_color(bubble, lv_color_hex(UI_PAL_CARD), 0);
        lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(bubble, lv_color_hex(UI_PAL_LINE), 0);
        lv_obj_set_style_border_width(bubble, 1, 0);
        lv_obj_set_style_text_color(bubble, lv_color_hex(UI_PAL_INK), 0);
        lv_obj_set_style_shadow_width(bubble, 4, 0);
        lv_obj_set_style_shadow_offset_x(bubble, 2, 0);
        lv_obj_set_style_shadow_offset_y(bubble, 4, 0);
        lv_obj_set_style_shadow_color(bubble, lv_color_hex(UI_PAL_CARD_SHADOW), 0);
        lv_obj_set_style_shadow_opa(bubble, LV_OPA_40, 0);
        lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(bubble, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(bubble, LVGL_UI_CARD_GAP, 0);
    }

    UITRACE(14);

    if (is_user) {
        /* User bubble: plain label */
        lbl = lv_label_create(bubble);
        if (!lbl) {
            syslog(LOG_ERR, "[%s] bubble label create failed\n", TAG);
            lv_obj_del(wrap);
            slot->bubble = NULL;
            return;
        }

        UITRACE(15);
        lv_label_set_text(lbl, slot->text);
        UITRACE(16);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        /* Cap the label so a long user line wraps at 80% of the screen width
         * (the content-sized bubble then hugs the wrapped text) while a short
         * message stays a compact chip. */
        int32_t lbl_max = bubble_max - 2 * LVGL_UI_BUBBLE_PAD;
        if (lbl_max < 40) lbl_max = 40;
        lv_obj_set_style_max_width(lbl, lbl_max, 0);
        UITRACE(17);
    } else {
        /* Agent bubble: enhanced card (title bar + divider + color bar + body) */
        uint32_t color = bar_color ? bar_color : chat_view_classify_reply(slot->text);
        UITRACE(18);
        chat_view_build_agent_card(bubble, slot->text, color);
        UITRACE(19);
    }

    /* slot->bubble stores the wrapper so history trim deletes bubble+wrapper
     * in one lv_obj_del (deleting a parent removes its children). */
    slot->bubble = wrap;
    UITRACE(34);

    chat_view_scroll_to_bottom();
    UITRACE(35);
}

static void chat_view_scroll_to_bottom(void)
{
    /* Split into the two halves LVGL actually runs, so a hang here is named:
     * tr36 -> forced layout pass (lv_obj_update_layout walks the whole screen
     *         and re-runs it while the screen keeps reporting layout
     *         invalidation — an oscillating content-size/percentage pair
     *         never converges and spins there forever), tr37 -> the scroll
     *         itself. */
    UITRACE(36);
    lv_obj_update_layout(s_state.chat_list);
    UITRACE(37);

    lv_obj_scroll_to_y(s_state.chat_list, LV_COORD_MAX, LV_ANIM_ON);
    UITRACE(38);
}

static void chat_view_trim_history(void)
{
    /* Ring buffer handles old bubble deletion in chat_history_add().
     * This function is a hook for any additional cleanup if needed
     * in the future (e.g. scroll position adjustment). */
}

/* lvgl_ui_thread removed — we no longer run our own LVGL event loop.
 * The system's miwear event loop handles lv_timer_handler(). */

/* ── Approval screen (remote permission request) + chat scroll ─────── */

#define LVGL_UI_SCROLL_STEP 120   /* px per short-press scroll tick */

/* Sized to the wire contract in remote/remote_link.h (REMOTE_TOOL_MAX /
 * REMOTE_HINT_MAX) so an approval that arrives over the link is shown in full
 * instead of being cut twice: once by the RPMSG shim and again here. */
typedef struct {
    char tool[REMOTE_TOOL_MAX + 1];
    char hint[REMOTE_HINT_MAX + 1];
} async_approval_t;

typedef struct {
    int dir;   /* 0 = up/older, 1 = down/newer */
} async_scroll_t;

/* Build the standalone approval screen once at init() time.  It is NOT
 * loaded here — lvgl_ui_channel_set_approval() loads it when a remote
 * permission request is pending and lvgl_ui_channel_clear_approval()
 * restores the previous page.  Content is a centered white card inside the
 * inscribed round face so nothing spills into the panel bezel. */
static void approval_screen_build(void)
{
    lv_coord_t disp_w = LVGL_UI_SCREEN_W;
    lv_coord_t disp_h = LVGL_UI_SCREEN_H;
    lv_display_t* disp = lv_display_get_default();

    if (disp) {
        lv_coord_t rw = lv_display_get_horizontal_resolution(disp);
        lv_coord_t rh = lv_display_get_vertical_resolution(disp);
        if (rw > 0 && rh > 0) {
            disp_w = rw;
            disp_h = rh;
        }
    }

    bool round_panel = (disp_w == 240 && disp_h == 296);
    int32_t round_d = round_panel ? 224 : ((disp_w < disp_h) ? disp_w : disp_h);
    /* 90% of the panel width on the 240x296 rectangle (was 90% of the Ø224
     * inscribed circle = 201 px, which left a 20 px black frame each side). */
    int32_t card_w = round_panel ? (disp_w * 90) / 100 : (round_d * 90) / 100;
    if (card_w < 160) card_w = 160;

    s_state.approval_screen = lv_obj_create(NULL);
    if (!s_state.approval_screen) {
        syslog(LOG_ERR, "[%s] approval screen create failed\n", TAG);
        return;
    }
    lv_obj_set_style_bg_color(s_state.approval_screen, lv_color_hex(UI_PAL_BG), 0);
    lv_obj_set_style_bg_opa(s_state.approval_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(s_state.approval_screen, &lv_font_misans_12_cjk, 0);

    /* White card centered on the panel (the round face is concentric with
     * the full panel, so lv_obj_center lands it on the face center). */
    lv_obj_t* card = lv_obj_create(s_state.approval_screen);
    /* Scrollable on purpose: max_height below caps the card, so a very long
     * hint must be reachable by scrolling instead of being clipped. */
    lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(card, card_w);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    /* Same bounded-card rule as the notice page: the hint can be up to
     * REMOTE_HINT_MAX bytes, so cap the card inside the panel and let the
     * hint scroll instead of pushing the button row off screen. */
    lv_obj_set_style_max_height(card, disp_h - 12, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_PAL_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(UI_PAL_LINE), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_shadow_width(card, 4, 0);
    lv_obj_set_style_shadow_offset_x(card, 2, 0);
    lv_obj_set_style_shadow_offset_y(card, 4, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(UI_PAL_CARD_SHADOW), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_40, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, 8, 0);

    /* Title — amber, the "permission request" cue.  Font-safe subset: 等待确认. */
    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, "等待确认");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_PAL_WAIT), 0);

    /* Tool name — ink.  Hides itself when the bridge sends an empty tool. */
    s_state.approval_tool = lv_label_create(card);
    lv_label_set_text(s_state.approval_tool, "");
    lv_obj_set_style_text_color(s_state.approval_tool, lv_color_hex(UI_PAL_INK), 0);

    /* Hint — muted grey, wraps within the card. */
    s_state.approval_hint = lv_label_create(card);
    lv_label_set_text(s_state.approval_hint, "");
    lv_label_set_long_mode(s_state.approval_hint, LV_LABEL_LONG_WRAP);
    /* Card inner width: card_w minus pad_all(10) on both sides minus the 1 px
     * border.  The old width ignored the border and overflowed by 2 px. */
    lv_obj_set_width(s_state.approval_hint, card_w - 2 * 10 - 2);
    /* Height is content-derived.  It used to carry flex_grow(1) +
     * min_height(0) inside a LV_SIZE_CONTENT card: LVGL cannot resolve a
     * growing child against an auto-sized main axis, so the label collapsed
     * to zero height and the command/path was invisible on the panel (the
     * tool name above it rendered fine).  Sizing from the wrapped text keeps
     * it visible; the card's max_height still bounds the worst case. */
    lv_obj_set_height(s_state.approval_hint, LV_SIZE_CONTENT);
    lv_obj_set_style_text_color(s_state.approval_hint, lv_color_hex(UI_PAL_MUTED), 0);

    /* Button row: two capsules mapping the two non-PTT keys to their approval
     * meaning (S2 → approve, S3 → cancel).  Purely visual — the actual key
     * handling lives in the board's button input layer, which registers as a
     * remote_ctrl input adapter; this page only tells the user what a short
     * press means right now. */
    lv_obj_t* btns = lv_obj_create(card);
    lv_obj_remove_flag(btns, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(btns, lv_pct(100));
    lv_obj_set_height(btns, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(btns, 0, 0);
    lv_obj_set_style_border_width(btns, 0, 0);
    lv_obj_set_style_bg_opa(btns, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(btns, 0, 0);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t* ok = lv_obj_create(btns);
    lv_obj_remove_flag(ok, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_height(ok, 32);
    lv_obj_set_width(ok, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(ok, 14, 0);
    lv_obj_set_style_radius(ok, 16, 0);
    lv_obj_set_style_bg_color(ok, lv_color_hex(UI_PAL_DONE), 0);
    lv_obj_set_style_bg_opa(ok, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ok, 0, 0);
    lv_obj_set_flex_flow(ok, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ok, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_t* ok_lbl = lv_label_create(ok);
    lv_label_set_text(ok_lbl, "S2 通过");
    lv_obj_set_style_text_color(ok_lbl, lv_color_white(), 0);

    lv_obj_t* no = lv_obj_create(btns);
    lv_obj_remove_flag(no, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_height(no, 32);
    lv_obj_set_width(no, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(no, 14, 0);
    lv_obj_set_style_radius(no, 16, 0);
    lv_obj_set_style_bg_color(no, lv_color_hex(UI_PAL_ERROR), 0);
    lv_obj_set_style_bg_opa(no, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(no, 0, 0);
    lv_obj_set_flex_flow(no, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(no, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_t* no_lbl = lv_label_create(no);
    lv_label_set_text(no_lbl, "S3 取消");
    lv_obj_set_style_text_color(no_lbl, lv_color_white(), 0);
}

static void set_approval_async_cb(void* data)
{
    async_approval_t* p = (async_approval_t*)data;

    if (!p) {
        return;
    }
    if (!s_state.initialized || !s_state.running || !s_state.approval_screen) {
        free(p);
        return;
    }

    lv_label_set_text(s_state.approval_tool, p->tool);
    lv_label_set_text(s_state.approval_hint, p->hint);
    if (p->tool[0] == '\0') {
        lv_obj_add_flag(s_state.approval_tool, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_state.approval_tool, LV_OBJ_FLAG_HIDDEN);
    }

    /* Approval outranks a notice, but only for as long as the prompt is open:
     * a notice that is already on screen is stashed first and re-shown when
     * the approval clears.  Dropping it would lose a Gerrit/Jira event the user
     * never got to read; without the stash the page just flashed
     * notice -> approval and the notice never came back.
     *
     * Carry the "we came from the clock" memory over so the clock can come back
     * once the approval is resolved. */
    if (s_state.notice_visible) {
#ifdef CONFIG_AI_AGENT_STANDBY_UI
        if (s_state.notice_was_standby) s_state.approval_was_standby = true;
        s_state.notice_was_standby = false;
#endif
        notice_stash_visible();
        notice_hide("dismissed (approval page up)");
    }

    /* A prompt that arrives on the dim clock must wake it: otherwise the first
     * key press is eaten by the wake gate and never reaches approve/cancel. */
#ifdef CONFIG_AI_AGENT_STANDBY_UI
    if (!s_state.approval_visible && standby_view_is_visible()) {
        s_state.approval_was_standby = true;
        standby_view_hide_now();
    }
#endif

    /* Save the page we were on (pet/chat) so clear can restore it.  If a
     * previous clear was dropped, the active screen can already BE the
     * approval page; saving it would make every later clear "restore" the
     * approval page and strand the user on it, so store NULL instead and let
     * clear fall back to a known page. */
    if (!s_state.approval_visible) {
        lv_obj_t* active = lv_screen_active();
        s_state.approval_prev_screen =
            (active == s_state.approval_screen) ? NULL : active;
        lv_screen_load_anim(s_state.approval_screen, LV_SCR_LOAD_ANIM_FADE_IN,
                            SCREEN_FADE_MS, 0, false);
        s_state.approval_visible = true;
    }
    free(p);
}

static void clear_approval_async_cb(void* data)
{
    (void)data;
    if (!s_state.initialized || !s_state.approval_screen) {
        return;
    }
    if (!s_state.approval_visible) {
        return;
    }
#ifdef CONFIG_AI_AGENT_STANDBY_UI
    /* The prompt woke the clock; put the user back on the lock screen once it
     * is resolved.  Restore the underlying screen first and immediately (no
     * fade) so the clock remembers the right screen to return to on wake. */
    bool back_to_clock = s_state.approval_was_standby;
    s_state.approval_was_standby = false;
    if (back_to_clock && !standby_view_is_visible())
      {
        if (s_state.approval_prev_screen &&
            s_state.approval_prev_screen != s_state.approval_screen)
            lv_screen_load(s_state.approval_prev_screen);
        standby_view_show_now();
        s_state.approval_visible = false;
        s_state.approval_prev_screen = NULL;
        notice_maybe_show_pending();
        return;
      }
#endif

    {
        /* Never "restore" the approval page itself: that happens when a set
         * arrived after a dropped clear and saved the page as prev.  Fall
         * back to the chat transcript or the pet home instead of stranding
         * the user on a page whose prompt is long gone. */
        lv_obj_t* prev = s_state.approval_prev_screen;

        if (prev == s_state.approval_screen) {
            prev = NULL;
        }
        if (prev != NULL) {
            lv_screen_load_anim(prev, LV_SCR_LOAD_ANIM_FADE_IN,
                                SCREEN_FADE_MS, 0, false);
        } else if (s_state.screen_visible && s_state.screen != NULL) {
            lv_screen_load_anim(s_state.screen, LV_SCR_LOAD_ANIM_FADE_IN,
                                SCREEN_FADE_MS, 0, false);
        } else {
#ifdef CONFIG_AI_AGENT_PET_STATUS
            pet_status_view_show();
#endif
        }
    }
    s_state.approval_visible = false;
    s_state.approval_prev_screen = NULL;

    /* A notice that arrived while the approval was pending takes the screen
     * now (only the newest one was kept). */
    notice_maybe_show_pending();
}

/* ── Notification page ────────────────────────────────────────────────────
 *
 * The round panel is too small for a top-of-screen toast — a Gerrit/Jira body
 * was clipped to a few characters.  A notice therefore gets the same treatment
 * as the approval page: its own screen with a white card, a colored left bar
 * and the full text, held for LVGL_UI_NOTICE_MS and exiting on any user key.
 *
 * Precedence: approval > notice > chat/pet.  A notice that arrives while an
 * approval is pending is remembered (newest wins) and shown once the approval
 * clears -- never discarded, because the event it reports is news the user
 * still needs after answering.  A notice that arrives while the standby clock
 * is up wakes the page and hands the screen back to the clock when it closes.
 */

typedef struct {
    char title[96];
    char body[512];
    uint32_t bar_color;
} async_notice_t;

static void notice_screen_build(void)
{
    lv_coord_t disp_w = LVGL_UI_SCREEN_W;
    lv_coord_t disp_h = LVGL_UI_SCREEN_H;
    lv_display_t* disp = lv_display_get_default();
    lv_coord_t card_w;
    lv_obj_t* card;
    lv_obj_t* title;
    lv_obj_t* body;
    lv_obj_t* hint;

    if (disp) {
        lv_coord_t rw = lv_display_get_horizontal_resolution(disp);
        lv_coord_t rh = lv_display_get_vertical_resolution(disp);
        if (rw > 0 && rh > 0) { disp_w = rw; disp_h = rh; }
    }

    bool round_panel = (disp_w == 240 && disp_h == 296);
    int32_t round_d = round_panel ? 224 : ((disp_w < disp_h) ? disp_w : disp_h);
    card_w = round_panel ? (disp_w * 90) / 100 : (round_d * 90) / 100;
    if (card_w < 160) card_w = 160;

    s_state.notice_screen = lv_obj_create(NULL);
    if (!s_state.notice_screen) {
        syslog(LOG_ERR, "[%s] notice screen create failed\n", TAG);
        return;
    }
    lv_obj_set_style_bg_color(s_state.notice_screen, lv_color_hex(UI_PAL_BG), 0);
    lv_obj_set_style_bg_opa(s_state.notice_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(s_state.notice_screen, &lv_font_misans_12_cjk, 0);

    /* The card itself carries the colored left bar (4 px left border), the same
     * status-bar cue the chat cards use.  The color is swapped per notice. */
    card = lv_obj_create(s_state.notice_screen);
    /* Scrollable on purpose: max_height below caps the card, so a long body
     * must be reachable by scrolling instead of being clipped. */
    lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(card, card_w);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_PAL_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 4, 0);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(UI_PAL_THINK), 0);
    lv_obj_set_style_shadow_width(card, 4, 0);
    lv_obj_set_style_shadow_offset_x(card, 2, 0);
    lv_obj_set_style_shadow_offset_y(card, 4, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(UI_PAL_CARD_SHADOW), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_40, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, 6, 0);
    /* Cap the card inside the panel with a 6 px gutter top and bottom.  The
     * old `disp_h - 16` (=280) is fine on the 240x296 rectangle; the earlier
     * round-face cap (224 - 16) was reverted together with the chat column —
     * see the gutter comment in lvgl_ui_channel_init(). */
    lv_obj_set_style_max_height(card, disp_h - 12, 0);
    s_state.notice_bar = card;

    title = lv_label_create(card);
    lv_label_set_text(title, "");
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(title, card_w - 2 * 10 - 4);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_PAL_INK), 0);
    s_state.notice_title = title;

    body = lv_label_create(card);
    lv_label_set_text(body, "");
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, card_w - 2 * 10 - 4);
    /* The body is the one element that can be arbitrarily long.  It used to
     * carry flex_grow(1) + min_height(0) + SCROLLABLE inside a
     * LV_SIZE_CONTENT card, and LVGL cannot resolve a growing child against
     * an auto-sized main axis: the label collapsed to zero height and the
     * notice body never appeared (only the title did).  Derive the height
     * from the wrapped text instead; the card's max_height still bounds the
     * worst case and the card itself scrolls if it ever overflows. */
    lv_obj_set_height(body, LV_SIZE_CONTENT);
    lv_obj_set_style_text_color(body, lv_color_hex(UI_PAL_MUTED), 0);
    s_state.notice_body = body;

    /* Font subset check: 任意键返回 all exist in both the AP font and the CP
     * subset.  Purely informational — the key handling is in the board's
     * button input layer, which asks the channel to dismiss first. */
    hint = lv_label_create(card);
    lv_label_set_text(hint, "任意键返回");
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_PAL_IDLE), 0);
}

static void notice_timer_cancel(void)
{
    if (s_state.notice_timer) {
        lv_timer_delete(s_state.notice_timer);
        s_state.notice_timer = NULL;
    }
}

/* Must run on the LVGL thread. */
static void notice_hide(const char* why)
{
    lv_obj_t* prev;

    notice_timer_cancel();
    if (!s_state.notice_visible) {
        return;
    }

    prev = s_state.notice_prev_screen;
    s_state.notice_visible = false;
    s_state.notice_prev_screen = NULL;

#ifdef CONFIG_AI_AGENT_STANDBY_UI
    /* The notice woke us from the dim clock: hand the screen back to the lock
     * screen.  Restore the saved screen first and *immediately* (no fade) so
     * the clock records the right screen to fade back to on wake — with a fade
     * the switch happens later in the anim timer and races that capture. */
    bool back_to_clock = s_state.notice_was_standby;
    s_state.notice_was_standby = false;
    if (back_to_clock && !standby_view_is_visible())
      {
        if (prev) lv_screen_load(prev);
        standby_view_show_now();
        syslog(LOG_INFO, "[%s] hidden (%s)\n", NOTICE_TAG, why);
        return;
      }
#endif

    /* If the clock faded in behind the notice, leave it alone: restoring the
     * saved screen would silently end standby. */
    if (prev) {
#ifdef CONFIG_AI_AGENT_STANDBY_UI
        if (!standby_view_is_visible())
#endif
            lv_screen_load_anim(prev, LV_SCR_LOAD_ANIM_FADE_IN,
                                SCREEN_FADE_MS, 0, false);
    }
    syslog(LOG_INFO, "[%s] hidden (%s)\n", NOTICE_TAG, why);
}

static void notice_timeout_cb(lv_timer_t* t)
{
    (void)t;
    s_state.notice_timer = NULL;   /* one-shot + auto-delete */
    notice_hide("auto-dismiss");
    notice_maybe_show_pending();
}

/* Fixed hold time.  It used to scale with the text (~60ms per byte, floored at
 * 15 s, capped at 45 s) so a long Jira summary would not be yanked mid-sentence.
 * The acceptance criterion for the notification page is a flat "5 s 自退" with
 * any key dismissing it early, so the window is now exactly LVGL_UI_NOTICE_MS
 * and reading more is the user's call (press a key, or the item comes back on
 * the next poll).  Kept as a function so both notice paths share one value. */
static uint32_t notice_duration_ms(const char* title, const char* body)
{
    (void)title;
    (void)body;
    return LVGL_UI_NOTICE_MS;
}

/* Must run on the LVGL thread. */
static void notice_show_locked(const char* title, const char* body,
                               lv_color_t bar_color)
{
    if (!s_state.notice_screen) {
        return;
    }

    /* The dim clock must never swallow a notice: wake the page (a key has to
     * reach the notice, not just the wake gate) and remember to hand the screen
     * back to the clock once the notice closes.  Hide it synchronously — the
     * queued async hide would run after our screen load and yank the notice. */
#ifdef CONFIG_AI_AGENT_STANDBY_UI
    if (!s_state.notice_visible) {
        s_state.notice_was_standby = standby_view_is_visible();
        if (s_state.notice_was_standby) standby_view_hide_now();
    }
#endif

    ui_activity_reset();   /* a notice is user-relevant activity */

    if (s_state.notice_title) lv_label_set_text(s_state.notice_title, title ? title : "");
    if (s_state.notice_body)  lv_label_set_text(s_state.notice_body, body ? body : "");
    if (s_state.notice_bar)   lv_obj_set_style_border_color(s_state.notice_bar, bar_color, 0);
    s_state.notice_last_color = bar_color;   /* remembered for a preempted-notice replay */

    if (!s_state.notice_visible) {
        s_state.notice_prev_screen = lv_screen_active();
        lv_screen_load_anim(s_state.notice_screen, LV_SCR_LOAD_ANIM_FADE_IN,
                            SCREEN_FADE_MS, 0, false);
        s_state.notice_visible = true;
    }

    /* Later notice replaces the current one and restarts the window. */
    notice_timer_cancel();
    s_state.notice_timer = lv_timer_create(notice_timeout_cb,
                                           notice_duration_ms(title, body), NULL);
    if (s_state.notice_timer) {
        lv_timer_set_repeat_count(s_state.notice_timer, 1);
        lv_timer_set_auto_delete(s_state.notice_timer, true);
    }

    syslog(LOG_INFO, "[%s] show: %s | %s\n", NOTICE_TAG,
           title ? title : "", body ? body : "");
}

/* Move the notice currently on screen into the stash so the approval page that
 * is about to take over does not swallow it.  Newest wins — the same rule
 * notice_show_async_cb() uses for notices that arrive while an approval is
 * already up.  Must run on the LVGL thread, before notice_hide(). */
static void notice_stash_visible(void)
{
    if (!s_state.notice_visible || !s_state.notice_title || !s_state.notice_body) {
        return;
    }

    const char* title = lv_label_get_text(s_state.notice_title);
    const char* body = lv_label_get_text(s_state.notice_body);

    strlcpy(s_state.notice_pending_title, title ? title : "",
            sizeof(s_state.notice_pending_title));
    strlcpy(s_state.notice_pending_body, body ? body : "",
            sizeof(s_state.notice_pending_body));
    s_state.notice_pending_color = s_state.notice_last_color;
    s_state.notice_pending = true;
    syslog(LOG_INFO, "[%s] stashed for re-show (approval page up): %s\n",
           NOTICE_TAG, s_state.notice_pending_title);
}

static void notice_maybe_show_pending(void)
{
    if (!s_state.notice_pending || s_state.approval_visible) {
        return;
    }
    if (!s_state.initialized || !s_state.running) {
        return;
    }

    s_state.notice_pending = false;
    /* No standby gate here: notice_show_locked() wakes the page and hands the
     * screen back to the clock when it closes. */
    notice_show_locked(s_state.notice_pending_title, s_state.notice_pending_body,
                       s_state.notice_pending_color);
}

static void notice_show_async_cb(void* data)
{
    async_notice_t* p = (async_notice_t*)data;

    if (!p) {
        return;
    }
    if (!s_state.initialized || !s_state.running || !s_state.notice_screen) {
        free(p);
        return;
    }

    if (s_state.approval_visible) {
        /* Approval wins the screen; keep only the newest notice for later. */
        strlcpy(s_state.notice_pending_title, p->title,
                sizeof(s_state.notice_pending_title));
        strlcpy(s_state.notice_pending_body, p->body,
                sizeof(s_state.notice_pending_body));
        s_state.notice_pending_color = lv_color_hex(p->bar_color);
        s_state.notice_pending = true;
        syslog(LOG_INFO, "[%s] deferred (approval page up): %s\n",
               NOTICE_TAG, p->title);
        free(p);
        return;
    }

    /* A notice always wins over the dim clock now: it wakes the page and the
     * clock comes back when the notice closes (see notice_show_locked()). */
    notice_show_locked(p->title, p->body, lv_color_hex(p->bar_color));
    free(p);
}

static void notice_dismiss_async_cb(void* data)
{
    (void)data;
    if (!s_state.notice_visible) {
        return;
    }
    notice_hide("dismissed by key");
    notice_maybe_show_pending();
}

int lvgl_ui_channel_show_notice(const char* title, const char* body,
                                uint32_t bar_color)
{
    async_notice_t* p;

    if (!s_state.initialized || !s_state.running) {
        return -EINVAL;
    }
    if (!s_state.notice_screen) {
        return -ENOSYS;
    }

    p = malloc(sizeof(*p));
    if (!p) {
        syslog(LOG_ERR, "[%s] notice: payload alloc failed\n", TAG);
        return -ENOMEM;
    }
    chat_view_copy_text(p->title, title ? title : "", sizeof(p->title));
    chat_view_copy_text(p->body, body ? body : "", sizeof(p->body));
    p->bar_color = bar_color ? bar_color : UI_PAL_THINK;

    return lvgl_ui_async_call(notice_show_async_cb, p);
}

int lvgl_ui_channel_notice_dismiss(void)
{
    if (!s_state.initialized || !s_state.running) {
        return 0;
    }
    if (!s_state.notice_visible) {
        return 0;
    }
    lvgl_ui_async_call(notice_dismiss_async_cb, NULL);
    return 1;
}

int lvgl_ui_channel_notice_visible(void)
{
    return s_state.notice_visible ? 1 : 0;
}

static void scroll_async_cb(void* data)
{
    async_scroll_t* p = (async_scroll_t*)data;

    if (!p) {
        return;
    }
    if (!s_state.initialized || !s_state.running || !s_state.chat_list) {
        free(p);
        return;
    }

    int32_t delta = (p->dir == 0) ? -LVGL_UI_SCROLL_STEP : LVGL_UI_SCROLL_STEP;
    lv_obj_scroll_by(s_state.chat_list, 0, delta, LV_ANIM_ON);
    free(p);
}

int lvgl_ui_channel_set_approval(const char* tool, const char* hint)
{
    async_approval_t* p;

    if (!s_state.initialized || !s_state.running) {
        return -EINVAL;
    }
    ui_activity_reset();

    if (!s_approval_pet_saved)
      {
        s_approval_prev_pet_state = s_last_pet_state;
        s_approval_pet_saved = 1;
        (void)lvgl_ui_channel_set_pet_state(PET_STATE_WAITING);
      }

    p = malloc(sizeof(async_approval_t));
    if (!p) {
        syslog(LOG_ERR, "[%s] set_approval: payload alloc failed\n", TAG);
        return -ENOMEM;
    }
    chat_view_copy_text(p->tool, tool ? tool : "", sizeof(p->tool));
    chat_view_copy_text(p->hint, hint ? hint : "", sizeof(p->hint));
    lvgl_ui_async_call(set_approval_async_cb, p);
    return 0;
}

int lvgl_ui_channel_clear_approval(void)
{
    if (!s_state.initialized || !s_state.running) {
        return -EINVAL;
    }
    ui_activity_reset();
    if (s_approval_pet_saved)
      {
        s_approval_pet_saved = 0;
        (void)lvgl_ui_channel_set_pet_state(s_approval_prev_pet_state);
      }
    lvgl_ui_async_call(clear_approval_async_cb, NULL);
    return 0;
}

int lvgl_ui_channel_approval_pending(void)
{
    return s_approval_pet_saved;
}

int lvgl_ui_channel_approval_visible(void)
{
    /* approval_pending() is the request latch and clears even when the async
     * clear is dropped; this one answers "is the page actually on screen",
     * which is what the key map needs to stay on S2=通过 / S3=拒绝. */
    return s_state.approval_visible ? 1 : 0;
}

int lvgl_ui_channel_scroll(int dir)
{
    async_scroll_t* p;

    if (dir != 0 && dir != 1) {
        return -EINVAL;
    }
    if (!s_state.initialized || !s_state.running) {
        return -EINVAL;
    }
    ui_activity_reset();

    /* First scroll brings the chat screen to the foreground (async, queued
     * before the scroll cb so the transcript is visible when it moves) — but
     * only when there is a transcript.  With an empty conversation, e.g. an
     * S3 tap waking from standby, switching to the chat screen would show a
     * blank near-black page with no bubbles and no hint; the tap should just
     * wake the pet home screen instead. */
    if (!s_state.screen_visible) {
        if (s_state.history.count == 0) {
            syslog(LOG_INFO, "[%s] scroll: empty transcript, staying on pet\n",
                   TAG);
            return 0;
        }
        syslog(LOG_INFO, "[%s] scroll: pet → chat (%d msgs)\n", TAG,
               s_state.history.count);
        lvgl_ui_channel_show();
    }

    p = malloc(sizeof(async_scroll_t));
    if (!p) {
        syslog(LOG_ERR, "[%s] scroll: payload alloc failed\n", TAG);
        return -ENOMEM;
    }
    p->dir = dir;
    lvgl_ui_async_call(scroll_async_cb, p);
    return 0;
}
