/****************************************************************************
 * key_input.c - Physical key handling for the pet UI.
 *
 *   Key1 -> open the pet page (no-op when it is already open)
 *   Key2 -> send one of the demo questions to the on-device model
 *
 * The board driver reports both keys through /dev/buttons as a bitset; this
 * thread polls that device and turns press edges into UI actions.  Widget
 * work is bounced onto the LVGL thread with lv_async_call(); the question
 * itself is pushed onto the agent bus exactly like the NSH `ask` builtin, so
 * it takes the normal route (agent loop -> cloud LLM -> on-device model) and
 * the reply comes back through outbound dispatch on the "cli" channel.
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <lvgl/lvgl.h>
#include <nuttx/input/buttons.h>

#include "key_input.h"
#include "ui_nav.h"
#include "voice/voice_channel.h"
#include "ui_nav.h"
#include "pet_page.h"
#include "lvgl_ui_channel.h"
#include "core/message_bus.h"
#include "pet_care.h"
#include "settings_page.h"   /* settings_page_wake_screen: 息屏时的按键逃生口(P17) */

#define TAG "key"

#define KEY_DEV_PATH   "/dev/buttons"
#define KEY_STACK      6144
#define KEY_POLL_MS    1000

/* Bit positions come from the board header (BUTTON_KEY2 = 0, KEY1 = 1).
 * Repeated here so this file does not have to pull in board internals. */

#define KEY_BIT_KEY2   (1 << 0)   /* PA11 - 短按说话, 长按问模型 */
#define KEY_BIT_KEY1   (1 << 1)   /* PA34 - open the pet page */

/* KEY2 按住的时长达到这里算**长按**(问模型演示); 短按则是语音。
 * 1.5s 足够区分, 又不会让"长按"等太久。 */
#define KEY_LONG_MS    1500

/* Two presses closer together than this are treated as contact bounce. */

#define KEY_DEBOUNCE_MS 300

/* Demo questions.  Each one hits a different intent of the on-device agent
 * model (ag_detect_intent in ai_lm.cxx): light, curtain, AC set/mode, device
 * on-off, device status, clock, weather, temperature, door, timer.  Keeping
 * them inside the trained domain is what makes the answers meaningful -- the
 * model is a 2.3M-parameter smart-home LSTM, not a general chatbot. */

static const char *const g_demo_questions[] =
{
    "\xe6\x89\x93\xe5\xbc\x80\xe5\xae\xa2\xe5\x8e\x85\xe7\x9a\x84\xe7\x81\xaf",
    /* 打开客厅的灯 */
    "\xe5\x85\xb3\xe6\x8e\x89\xe5\x8d\xa7\xe5\xae\xa4\xe7\x9a\x84\xe7\x81\xaf",
    /* 关掉卧室的灯 */
    "\xe6\x8a\x8a\xe4\xb9\xa6\xe6\x88\xbf\xe7\xa9\xba\xe8\xb0\x83\xe8\xae\xbe"
    "\xe6\x88\x9022\xe5\xba\xa6",
    /* 把书房空调设成22度 */
    "\xe5\xae\xa2\xe5\x8e\x85\xe7\xa9\xba\xe8\xb0\x83\xe5\x88\x87\xe6\x8d\xa2"
    "\xe5\x88\xb0\xe9\x99\xa4\xe6\xb9\xbf\xe6\xa8\xa1\xe5\xbc\x8f",
    /* 客厅空调切换到除湿模式 */
    "\xe6\x89\x93\xe5\xbc\x80\xe9\x98\xb3\xe5\x8f\xb0\xe7\x9a\x84\xe7\xaa\x97"
    "\xe5\xb8\x98",
    /* 打开阳台的窗帘 */
    "\xe5\x8d\xa7\xe5\xae\xa4\xe7\x9a\x84\xe5\x8a\xa0\xe6\xb9\xbf\xe5\x99\xa8"
    "\xe7\x8e\xb0\xe5\x9c\xa8\xe6\x80\x8e\xe4\xb9\x88\xe6\xa0\xb7",
    /* 卧室的加湿器现在怎么样 */
    "\xe6\x89\x93\xe5\xbc\x80\xe5\x8e\xa8\xe6\x88\xbf\xe7\x9a\x84\xe7\x83\xad"
    "\xe6\xb0\xb4\xe5\x99\xa8",
    /* 打开厨房的热水器 */
    "\xe7\x8e\xb0\xe5\x9c\xa8\xe5\x87\xa0\xe7\x82\xb9\xe4\xba\x86",
    /* 现在几点了 */
    "\xe5\x8c\x97\xe4\xba\xac\xe4\xbb\x8a\xe5\xa4\xa9\xe5\xa4\xa9\xe6\xb0\x94"
    "\xe6\x80\x8e\xe4\xb9\x88\xe6\xa0\xb7",
    /* 北京今天天气怎么样 */
    "\xe5\xae\xa2\xe5\x8e\x85\xe7\x8e\xb0\xe5\x9c\xa8\xe5\xa4\x9a\xe5\xb0\x91"
    "\xe5\xba\xa6",
    /* 客厅现在多少度 */
    "\xe9\x94\x81\xe4\xb8\x8a\xe5\xa4\xa7\xe9\x97\xa8",
    /* 锁上大门 */
    "20\xe5\x88\x86\xe9\x92\x9f\xe5\x90\x8e\xe6\x8f\x90\xe9\x86\x92\xe6\x88"
    "\x91\xe5\x96\x9d\xe6\xb0\xb4",
    /* 20分钟后提醒我喝水 */
};

#define KEY_N_QUESTIONS (int)(sizeof(g_demo_questions) / \
                              sizeof(g_demo_questions[0]))

/* "……\n本地模型思考中…" suffix shown while the model runs */
#define KEY_THINKING "\n\xe6\x9c\xac\xe5\x9c\xb0\xe6\xa8\xa1\xe5\x9e\x8b" \
                     "\xe6\x80\x9d\xe8\x80\x83\xe4\xb8\xad\xe2\x80\xa6"

static bool s_started;
static int  s_last_question = -1;

/****************************************************************************
 * LVGL thread callbacks
 ****************************************************************************/

static void key_open_pet_cb(void *arg)
{
    (void)arg;

    if ((ui_nav_current() != UI_PAGE_AI)) {
        ui_nav_goto(UI_PAGE_AI);
    }
}

/* arg: malloc'ed question text, freed here */
static void key_ask_shown_cb(void *arg)
{
    char *question = (char *)arg;
    char line[256];

    if ((ui_nav_current() != UI_PAGE_AI)) {
        ui_nav_goto(UI_PAGE_AI);
    }

    snprintf(line, sizeof(line), "%s" KEY_THINKING, question);
    pet_page_update_response(line);
    free(question);
}

/****************************************************************************
 * Key actions
 ****************************************************************************/

static void key_action_open_pet(void)
{
    syslog(LOG_INFO, "[%s] KEY1: open pet page\n", TAG);
    /* key monitor thread -> LVGL: post queue, not lv_async_call (Round 27) */
    lvgl_ui_post(key_open_pet_cb, NULL);
}

/* KEY2 短按: 开始/结束语音。
 *
 * 按下不必一直按住 —— 开始之后由 VAD **自动断句**收尾(见 voice/vad_trim.c),
 * 所以正常用法就是"按一下, 说一句, 什么都不用管"。
 * 再按一下是提前结束(想中止当前这句时用)。
 * 之前 KEY2 独占的"问模型"演示挪到了**长按**。 */
static void key_action_voice_toggle(void)
{
    if (voice_channel_is_recording()) {
        syslog(LOG_INFO, "[%s] KEY2: stop voice\n", TAG);
        voice_channel_stop();
    } else {
        syslog(LOG_INFO, "[%s] KEY2: start voice\n", TAG);
        voice_channel_start();
    }
}

static void key_action_ask_model(void)
{
    agent_msg_t msg = { 0 };
    const char *question;
    char *shown;
    int idx;

    /* Avoid asking the same thing twice in a row -- with 12 entries a plain
     * rand() repeat is common enough to look broken during a demo. */

    idx = rand() % KEY_N_QUESTIONS;
    if (idx == s_last_question) {
        idx = (idx + 1) % KEY_N_QUESTIONS;
    }
    s_last_question = idx;
    question = g_demo_questions[idx];

    syslog(LOG_INFO, "[%s] KEY2: ask #%d \"%s\"\n", TAG, idx, question);

    /* User side of the exchange goes into the history ring right away. */
    lvgl_ui_channel_log(question, true);

    shown = strdup(question);
    if (shown != NULL) {
        lvgl_ui_post(key_ask_shown_cb, shown);
    }

    strncpy(msg.channel, "cli", sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, "key2", sizeof(msg.chat_id) - 1);
    msg.content = strdup(question);
    if (msg.content == NULL) {
        return;
    }

    if (message_bus_push_inbound(&msg) != 0) {
        syslog(LOG_WARNING, "[%s] inbound queue rejected the question\n", TAG);
        free(msg.content);
    }
}

/****************************************************************************
 * Monitor thread
 ****************************************************************************/

static uint32_t key_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000U + ts.tv_nsec / 1000000U);
}

/* KEY2 的长按判定状态 */
static bool     s_key2_down;
static bool     s_key2_long_fired;
static uint32_t s_key2_press_ms;

static void *key_monitor_thread(void *arg)
{
    btn_buttonset_t supported = 0;
    btn_buttonset_t sample = 0;
    btn_buttonset_t last = 0;
    btn_buttonset_t pressed;
    uint32_t last_event_ms = 0;
    ssize_t nbytes;
    int fd;

    (void)arg;

    fd = open(KEY_DEV_PATH, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        syslog(LOG_WARNING, "[%s] %s unavailable (%d), keys disabled\n",
               TAG, KEY_DEV_PATH, errno);
        s_started = false;
        return NULL;
    }

    if (ioctl(fd, BTNIOC_SUPPORTED, (unsigned long)&supported) == 0) {
        syslog(LOG_INFO, "[%s] monitor started, supported=0x%02x\n",
               TAG, (unsigned)supported);
    }

    /* Baseline, so a key held at boot does not read as a press. */
    if (read(fd, &sample, sizeof(sample)) == (ssize_t)sizeof(sample)) {
        last = sample;
    }

    srand((unsigned)time(NULL) ^ key_now_ms());

    for (; ; ) {
        /* 2026-11-03 稳定性修复：不再用 poll()。
         * 真机复现：TTS 播放期间 /dev/buttons 的 poll 拆除路径触发
         * NuttX button_upper.c:760 断言（spin_unlock/sched_unlock 状态异常），
         * ai_agent 被打崩 -> 看门狗 20s 后复位 —— 用户看到"黑屏重启"。
         * 改成每 50ms 非阻塞 read 当前按键位图：功能等价、延迟 50ms、
         * 长按逻辑不变，同时完全绕开 button_upper 的 poll 路径。 */
        usleep(50 * 1000);
        nbytes = read(fd, &sample, sizeof(sample));

        if (nbytes == (ssize_t)sizeof(sample)) {
            btn_buttonset_t released;

            pressed  = sample & ~last;
            released = ~sample & last;
            last = sample;

            /* KEY2: 按下只记时间, **动作等松开时再定** —— 因为要先分清
             * 短按(语音)还是长按(问模型)。 */
            if (pressed & KEY_BIT_KEY2) {
                s_key2_down       = true;
                s_key2_long_fired = false;
                s_key2_press_ms   = key_now_ms();
            }

            if (released & KEY_BIT_KEY2) {
                s_key2_down = false;

                if (!s_key2_long_fired &&
                    key_now_ms() - last_event_ms >= KEY_DEBOUNCE_MS) {
                    last_event_ms = key_now_ms();

                    /* P17：黑屏时语音键同样先点亮屏幕（与 KEY1 一致） */
                    if (settings_page_wake_screen()) {
                        pet_care_note_activity();
                        continue;
                    }

                    pet_care_note_activity();
                    key_action_voice_toggle();
                }
            }

            if (pressed & KEY_BIT_KEY1) {
                if (key_now_ms() - last_event_ms >= KEY_DEBOUNCE_MS) {
                    last_event_ms = key_now_ms();

                    /* ★ P17 逃生口：息屏状态下按键先点亮屏幕、吞掉本次
                     * 动作。触摸 I2C 锁死时（reset/普通插拔都救不了），
                     * 这是从黑屏恢复的唯一软件路径。 */
                    if (settings_page_wake_screen()) {
                        pet_care_note_activity();
                        continue;
                    }

                    /* 任何按键都算用户活动：复位空闲关怀计时（闹钟 pending
                     * 时 note_activity 内部按唤醒确认处理） */
                    pet_care_note_activity();
                    key_action_open_pet();
                }
            }
        }

        /* 长按判定：50ms 循环里检查一次，按满 KEY_LONG_MS 就触发，不必等松开。 */
        if (s_key2_down && !s_key2_long_fired &&
            key_now_ms() - s_key2_press_ms >= KEY_LONG_MS) {
            s_key2_long_fired = true;
            last_event_ms = key_now_ms();
            pet_care_note_activity();
            syslog(LOG_INFO, "[%s] KEY2: long press -> ask model\n", TAG);
            key_action_ask_model();
        }
    }

    close(fd);
    s_started = false;
    return NULL;
}

/****************************************************************************
 * Public API
 ****************************************************************************/

int key_input_start(void)
{
    pthread_attr_t attr;
    pthread_t tid;
    int ret;

    if (s_started) {
        return 0;
    }

    s_started = true;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, KEY_STACK);
    ret = pthread_create(&tid, &attr, key_monitor_thread, NULL);
    pthread_attr_destroy(&attr);

    if (ret != 0) {
        s_started = false;
        syslog(LOG_ERR, "[%s] thread create failed (%d)\n", TAG, ret);
        return -ret;
    }

    pthread_detach(tid);
    return 0;
}
