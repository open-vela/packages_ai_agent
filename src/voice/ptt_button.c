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
 * Two threads, on purpose:
 *   - the watcher below owns /dev/btn0 and does NOTHING but read edges and
 *     flip flags.  It must never block: everything it does is also the only
 *     path that can observe the next press.
 *   - a worker does the slow half of a release (voice_channel_stop_with_text
 *     finalizes the ASR stream — seconds — and voice_channel_dispatch
 *     publishes to the bridge).  Both used to run on the watcher, so for the
 *     whole ASR window a press produced no event at all: the button read as
 *     dead rather than busy (BNI: "second voice round never happens").
 *
 * A busy flag still serializes sessions — one utterance at a time — but a
 * press that lands while busy is now logged and shown as a toast instead of
 * being dropped without a trace, and the worker's exit always clears it.
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
#include <semaphore.h>
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

/* The toast only exists where LVGL is drawn locally (路 A/AP).  Under
 * UI_RPMSG the CP renders and there is no AP-side toast to call. */
#if defined(CONFIG_AI_AGENT_LVGL_UI)
#include "ui/lvgl_toast.h"
#define PTT_HAVE_TOAST 1
#else
#define PTT_HAVE_TOAST 0
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

/* Watcher → worker handoff.  A counting sem keeps one post per release, and
 * the worker (unlike the watcher) is allowed to block for as long as the ASR
 * finalize and the bridge publish take. */
static sem_t g_release_sem;
static bool g_release_worker = false;

/* The worker runs the same code the watcher used to; give it room for the
 * ASR text buffer plus the voice stack's own frames. */
#ifndef PTT_WORKER_STACK
#  define PTT_WORKER_STACK 6144
#endif

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
      bool busy = g_ptt_busy;

      pthread_mutex_unlock(&g_ptt_lock);

      /* Not silent any more: a dropped press used to leave no trace at all,
       * which is indistinguishable from a dead button. */
      syslog(LOG_WARNING, "[%s] press ignored: %s\n", TAG,
             busy ? "previous utterance still processing"
                  : "already recording");
#if PTT_HAVE_TOAST
      (void)lvgl_toast_show_async("语音", "上一轮还在处理，请稍候再说",
                                  0xffa31f, 2500);
#endif
      return;  /* one utterance at a time */
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

  /* Hand the slow half to the worker and go straight back to the button.
   * Doing it here would blind the watcher for the whole ASR round. */
  sem_post(&g_release_sem);
}

/* Worker side of a release: finalize ASR, echo the utterance, dispatch. */
static void ptt_process_release(void)
{
  char asr_text[CONFIG_AI_AGENT_PTT_ASR_CAP];
  int ret;
  bool asr_failed = false;

  asr_text[0] = '\0';
  ret = voice_channel_stop_with_text(asr_text, sizeof(asr_text));

  /* -ENODATA is not a link failure: the stream finalized fine and ASR simply
   * heard no words.  volc_asr returns it when the final text is empty, so it
   * must land in the same "user miss" bucket as the empty-text branch below —
   * treating it as a red "ASR unavailable" card made a silent press look
   * identical to a broken network (log: "stream: no text recognized" followed
   * by rc=-61). */
  if (ret == -ENODATA)
    {
      syslog(LOG_WARNING, "[%s] ASR produced no text (ENODATA)\n", TAG);
      asr_text[0] = '\0';
      ret = 0;
    }

  if (ret != 0)
    {
      syslog(LOG_ERR, "[%s] voice_channel_stop_with_text failed rc=%d\n",
             TAG, ret);
      asr_failed = true;
#if PTT_HAVE_UI
      /* Distinct from "no speech": a real transport/credential failure.  Red
       * card + pet ERROR, so the user can tell "坏了" from "没说话". */
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

/* One post per release; the loop keeps the last-idle-clear invariant even if
 * a stop path returns early (its `done:` label is the only clear point). */
static void* ptt_release_thread(void* arg)
{
  (void)arg;

  for (;;)
    {
      if (sem_wait(&g_release_sem) != 0)
        {
          if (errno == EINTR)
            {
              continue;
            }
          syslog(LOG_ERR, "[%s] release worker sem_wait: %d\n", TAG, errno);
          break;
        }

      syslog(LOG_INFO, "[%s] processing release\n", TAG);
      ptt_process_release();
      syslog(LOG_INFO, "[%s] release done, ready for the next utterance\n",
             TAG);
    }

  return NULL;
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

  if (!g_release_worker)
    {
      ret = sem_init(&g_release_sem, 0, 0);
      if (ret != 0)
        {
          syslog(LOG_ERR, "[%s] sem_init: %d\n", TAG, errno);
          return -errno;
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
      (void)pthread_attr_setstacksize(&attr, PTT_WORKER_STACK);

      ret = pthread_create(&tid, &attr, ptt_release_thread, NULL);
      (void)pthread_attr_destroy(&attr);
      if (ret != 0)
        {
          syslog(LOG_ERR, "[%s] release worker create: %d\n", TAG, ret);
          return -ret;
        }

      pthread_detach(tid);
      g_release_worker = true;
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
