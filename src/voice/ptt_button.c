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

/*
 * ptt_button.c — AP-side physical Push-to-Talk button (路 B M5).
 *
 * BK7258 dual-core: the physical user buttons (S1/S2/S3 = GPIO13/12/8) and
 * the voice pipeline (voice_channel_*) both live on the AP core, as does the
 * ui_rpmsg_client shim that drives the CP UI.  So PTT is entirely an AP
 * affair — no reverse CP→AP RPMsg is needed:
 *
 *   S1 press   → voice_channel_start()          + set_ptt(RECORDING)  → CP
 *   S1 release → set_ptt(PROCESSING) → CP
 *               voice_channel_stop_with_text()   (blocks ~ASR finalize)
 *               send_user(asr_text)  → CP (right bubble echo)
 *               voice_channel_dispatch(asr_text) → agent → LLM → reply
 *               set_ptt(IDLE)         → CP
 *
 * Hold-to-talk semantics (press=record, release=ASR), as specified in
 * UI方案.md §2.5 ("PTT=按下录音→松手触发 ASR；可挂 S1").  Button index,
 * long-press threshold, and devpath are Kconfig-tunable.
 *
 * The blocking voice_channel_stop_with_text() runs in this dedicated thread
 * (not the LVGL/agent thread), so the ASR finalize (~300-800ms TLS + stream)
 * never stalls rendering or the agent loop.  A local busy flag serializes
 * sessions: a second press during processing is ignored, mirroring the
 * on-screen ptt_btn_event_cb's is_processing guard.
 *
 * Only compiled under CONFIG_AI_AGENT_PTT, which depends on a UI channel
 * (LVGL_UI or UI_RPMSG) so the lvgl_ui_channel_* symbols resolve.
 */

#include <nuttx/config.h>

#ifdef CONFIG_AI_AGENT_PTT

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>

#include <nuttx/input/buttons.h>

#include "voice/voice_channel.h"

#if defined(CONFIG_AI_AGENT_LVGL_UI) || defined(CONFIG_AI_AGENT_UI_RPMSG)
#include "ui/lvgl_ui_channel.h"
#define PTT_HAVE_UI 1
#else
#define PTT_HAVE_UI 0
#endif

#define TAG "ptt_button"

#ifndef CONFIG_AI_AGENT_PTT_DEVPATH
#  define CONFIG_AI_AGENT_PTT_DEVPATH "/dev/btn0"
#endif

#ifndef CONFIG_AI_AGENT_PTT_BUTTON_IDX
#  define CONFIG_AI_AGENT_PTT_BUTTON_IDX 0   /* S1 = KEY1 = GPIO13 */
#endif

#ifndef CONFIG_AI_AGENT_PTT_STACKSIZE
#  define CONFIG_AI_AGENT_PTT_STACKSIZE 4096
#endif

#ifndef CONFIG_AI_AGENT_PTT_PRIORITY
#  define CONFIG_AI_AGENT_PTT_PRIORITY 100
#endif

/* ASR text buffer — must match the agent's utterance cap. */
#ifndef CONFIG_AI_AGENT_PTT_ASR_CAP
#  define CONFIG_AI_AGENT_PTT_ASR_CAP 512
#endif

static bool g_ptt_running = false;
static bool g_ptt_recording = false;   /* between press and release */
static bool g_ptt_busy = false;        /* processing a release (stop+ASR) */
static pthread_mutex_t g_ptt_lock = PTHREAD_MUTEX_INITIALIZER;

static inline int ptt_button_bit(void)
{
  return (int)((btn_buttonset_t)1 << CONFIG_AI_AGENT_PTT_BUTTON_IDX);
}

static void ptt_on_press(void)
{
  int ret;

  pthread_mutex_lock(&g_ptt_lock);
  if (g_ptt_busy || g_ptt_recording)
    {
      pthread_mutex_unlock(&g_ptt_lock);
      return;  /* ignore re-press during an active session */
    }
  g_ptt_recording = true;
  pthread_mutex_unlock(&g_ptt_lock);

  ret = voice_channel_start();
  if (ret != 0)
    {
      syslog(LOG_ERR, "[%s] voice_channel_start failed rc=%d\n", TAG, ret);
      pthread_mutex_lock(&g_ptt_lock);
      g_ptt_recording = false;
      pthread_mutex_unlock(&g_ptt_lock);
      return;
    }

#if PTT_HAVE_UI
  (void)lvgl_ui_channel_set_ptt(LVGL_UI_PTT_RECORDING);
#endif
  syslog(LOG_INFO, "[%s] PTT press → recording\n", TAG);
}

static void ptt_on_release(void)
{
  char asr_text[CONFIG_AI_AGENT_PTT_ASR_CAP];
  int ret;
  bool asr_failed = false;

  pthread_mutex_lock(&g_ptt_lock);
  if (!g_ptt_recording)
    {
      /* release without a matched press (e.g. baseline edge) — ignore */
      pthread_mutex_unlock(&g_ptt_lock);
      return;
    }
  g_ptt_recording = false;
  g_ptt_busy = true;
  pthread_mutex_unlock(&g_ptt_lock);

#if PTT_HAVE_UI
  (void)lvgl_ui_channel_set_ptt(LVGL_UI_PTT_PROCESSING);
#endif
  syslog(LOG_INFO, "[%s] PTT release → ASR\n", TAG);

  /* Blocks until ASR finalizes the stream.  Fine here — dedicated thread. */
  asr_text[0] = '\0';
  ret = voice_channel_stop_with_text(asr_text, sizeof(asr_text));
  if (ret != 0)
    {
      syslog(LOG_ERR, "[%s] voice_channel_stop_with_text failed rc=%d\n",
             TAG, ret);
      asr_failed = true;
#if PTT_HAVE_UI
      /* ASR cannot run without the network (or its credentials).  Show a
       * red-bar card that says so; otherwise the screen simply drops back
       * to the pet with no clue about what went wrong. */
      (void)lvgl_ui_channel_send_ex(
          "语音识别不可用：请检查网络和语音识别配置。", 0xff5252);
#endif
      goto done;
    }

  if (asr_text[0] == '\0')
    {
      syslog(LOG_WARNING, "[%s] no speech recognized\n", TAG);
#if PTT_HAVE_UI
      /* The stream finalized cleanly but produced no text.  That is a *user*
       * miss (didn't speak / too far from the mic), not a broken link, so it
       * must look different from the red "ASR unavailable" card above.  An
       * amber hint tells them to try again rather than silently dropping back
       * to the idle pet, which was indistinguishable from success. */
      (void)lvgl_ui_channel_send_ex(
          "没听到语音，请靠近麦克风再说一次。", 0xffa31f);
#endif
      goto done;
    }

  syslog(LOG_INFO, "[%s] recognized: %s\n", TAG, asr_text);

#if PTT_HAVE_UI
  /* Echo the user utterance as a right-aligned bubble on CP. */
  (void)lvgl_ui_channel_send_user(asr_text);
#endif

  /* Route to the agent (message_bus → LLM → reply bubble + TTS).
   * voice_channel_dispatch is the single sink point; on AP it is real. */
  voice_channel_dispatch(asr_text);

done:
  pthread_mutex_lock(&g_ptt_lock);
  g_ptt_busy = false;
  pthread_mutex_unlock(&g_ptt_lock);

#if PTT_HAVE_UI
  (void)lvgl_ui_channel_set_ptt(LVGL_UI_PTT_IDLE);
  /* On ASR failure, drive the pet to ERROR (5) AFTER the PTT-IDLE mirror so
   * the RPMsg FIFO delivers ERROR last — the pet shows red+shake instead of
   * silently going idle-green.  pet_status_view.h: 5=PET_STATE_ERROR. */
  if (asr_failed)
    (void)lvgl_ui_channel_set_pet_error("语音识别不可用\n请检查网络和凭据");
#endif
}

static void* ptt_button_thread(void* arg)
{
  const char* devpath = CONFIG_AI_AGENT_PTT_DEVPATH;
  struct btn_pollevents_s pe;
  struct pollfd pfd;
  btn_buttonset_t supported;
  btn_buttonset_t last;
  bool have_baseline = false;
  int fd;
  int bit = ptt_button_bit();

  (void)arg;

  fd = open(devpath, O_RDONLY);
  if (fd < 0)
    {
      syslog(LOG_ERR, "[%s] open %s failed: %d\n", TAG, devpath, errno);
      return NULL;
    }

  supported = 0;
  if (ioctl(fd, BTNIOC_SUPPORTED, (unsigned long)&supported) < 0)
    {
      syslog(LOG_ERR, "[%s] BTNIOC_SUPPORTED failed: %d\n", TAG, errno);
      close(fd);
      return NULL;
    }

  if ((supported & (btn_buttonset_t)bit) == 0)
    {
      syslog(LOG_ERR, "[%s] button idx %d not in supported set 0x%08lx\n",
             TAG, CONFIG_AI_AGENT_PTT_BUTTON_IDX, (unsigned long)supported);
      close(fd);
      return NULL;
    }

  /* Wake on both edges of our PTT button only. */
  pe.bp_press   = (btn_buttonset_t)bit;
  pe.bp_release = (btn_buttonset_t)bit;
  (void)ioctl(fd, BTNIOC_POLLEVENTS, (unsigned long)&pe);

  pfd.fd     = fd;
  pfd.events = POLLIN;
  last       = 0;

  syslog(LOG_INFO, "[%s] watching %s button %d (hold-to-talk)\n", TAG,
         devpath, CONFIG_AI_AGENT_PTT_BUTTON_IDX);

  for (;;)
    {
      btn_buttonset_t cur;
      btn_buttonset_t change;
      ssize_t nread;

      if (poll(&pfd, 1, -1) < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }
          syslog(LOG_ERR, "[%s] poll failed: %d\n", TAG, errno);
          break;
        }

      if ((pfd.revents & POLLIN) == 0)
        {
          continue;
        }

      nread = read(fd, &cur, sizeof(cur));
      if (nread != (ssize_t)sizeof(cur))
        {
          continue;
        }

      if (!have_baseline)
        {
          last = cur;
          have_baseline = true;
          continue;
        }

      change = cur ^ last;
      if ((change & (btn_buttonset_t)bit) == 0)
        {
          last = cur;
          continue;  /* our button didn't change */
        }

      if (cur & (btn_buttonset_t)bit)
        {
          ptt_on_press();
        }
      else
        {
          ptt_on_release();
        }
      last = cur;
    }

  close(fd);
  return NULL;
}

int ptt_button_start(void)
{
  pthread_t tid;
  pthread_attr_t attr;
  struct sched_param sp;
  int ret;

  if (g_ptt_running)
    {
      return 0;
    }

  ret = pthread_attr_init(&attr);
  if (ret != 0)
    {
      syslog(LOG_ERR, "[%s] attr_init: %d\n", TAG, ret);
      return -ret;
    }
  sp.sched_priority = CONFIG_AI_AGENT_PTT_PRIORITY;
  (void)pthread_attr_setschedpolicy(&attr, SCHED_RR);
  (void)pthread_attr_setschedparam(&attr, &sp);
  (void)pthread_attr_setstacksize(&attr, CONFIG_AI_AGENT_PTT_STACKSIZE);

  ret = pthread_create(&tid, &attr, ptt_button_thread, NULL);
  (void)pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      syslog(LOG_ERR, "[%s] pthread_create: %d\n", TAG, ret);
      return -ret;
    }

  pthread_detach(tid);
  g_ptt_running = true;
  syslog(LOG_INFO, "[%s] PTT button thread started\n", TAG);
  return 0;
}

#endif /* CONFIG_AI_AGENT_PTT */
