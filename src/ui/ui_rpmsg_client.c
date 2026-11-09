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
 * ui_rpmsg_client.c — AP-side shim that implements the lvgl_ui_channel_* +
 * lvgl_toast_* API as RPMsg messages to the CP core (路 B).
 *
 * The AP core runs ai_agent (LLM/voice/TLS) but has no LCD/LVGL/FPU.  Instead
 * of building bubbles locally, this shim serializes each UI call into a
 * ui_rpmsg_msg and sends it over a dedicated RPMsg endpoint to the CP core,
 * where ui_rpmsg_server.c deserializes it and calls the REAL LVGL functions.
 *
 * Compile-time selection (CMakeLists): when CONFIG_AI_AGENT_UI_RPMSG is set
 * (AP), the agent links this file INSTEAD of lvgl_ui_channel.c/lvgl_toast.c.
 * The call sites in agent_main.c / nsh_commands.c / remote_ctrl_channel.c /
 * notify_service.c are guarded with
 *     #if defined(CONFIG_AI_AGENT_LVGL_UI) || defined(CONFIG_AI_AGENT_UI_RPMSG)
 * so the same source compiles on both cores unchanged.
 *
 * TTS split: the original lvgl_ui_channel_send() renders a bubble AND calls
 * voice_channel_speak() for TTS.  On the real device TTS hardware lives on
 * AP, so this shim calls voice_channel_speak() locally (real, audible) and
 * separately sends UI_RPMSG_SEND to CP for the bubble.  CP's copy of
 * lvgl_ui_channel_send() calls voice_channel_speak() too, but on CP that is
 * the cp_stubs.c no-op — so audio plays once (on AP) and the bubble renders
 * once (on CP), with no protocol round-trip for audio.
 *
 * Mirrors the RPMsg endpoint pattern in
 * chips/bk7258/mb_ipc/bk_mailbox_rpmsg.c: rpmsg_register_callback →
 * rpmsg_create_ept(name, ANY, ANY) → rpmsg_send_offchannel_raw.  Flat build
 * makes the kernel rpmsg symbols linkable from this userspace builtin.
 */

#include <nuttx/config.h>

#ifdef CONFIG_AI_AGENT_UI_RPMSG

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/rpmsg/rpmsg.h>

#include "ui_rpmsg_proto.h"
#include "ui/lvgl_ui_channel.h"
#include "ui/lvgl_toast.h"
#include "infra/network_manager.h" /* network_is_connected — gate TTS */
#include "voice/voice_channel.h"   /* voice_channel_speak — real on AP */

#define TAG "ui_rpmsg_client"

/* ── link state ─────────────────────────────────────────────────────── */

struct ui_rpmsg_client_state
{
  struct rpmsg_endpoint ept;
  bool registered;
  bool created;   /* ept created & bound to the CP peer */
};

static struct ui_rpmsg_client_state g_ui_cli;

static inline bool ui_rpmsg_ready(void)
{
  return g_ui_cli.created && is_rpmsg_ept_ready(&g_ui_cli.ept);
}

/* ── RX: CP→AP reverse-direction messages (M5: PTT on/off, ASR text) ── */
/* For now the CP server only renders; no reverse traffic is expected.
 * When PTT is wired (M5), this will forward button events to voice/agent. */

static int ui_rpmsg_rx(struct rpmsg_endpoint *ept, void *data, size_t len,
                       uint32_t source, void *priv)
{
  (void)ept;
  (void)source;
  (void)priv;
  (void)data;
  (void)len;
  /* Intentionally a no-op for M3.  CP→AP PTT/ASR forwarding lands in M5. */
  return 0;
}

/* ── RPMsg device appear/disappear callbacks ─────────────────────────── */

static void ui_rpmsg_created(struct rpmsg_device *rdev, void *priv)
{
  struct ui_rpmsg_client_state *s = priv;
  int ret;

  /* On the AP the peer we care about is the CP core. */
  if (strcmp(rpmsg_get_cpuname(rdev), UI_RPMSG_REMOTE_CPU) != 0 || s->created)
    {
      return;
    }

  ret = rpmsg_create_ept(&s->ept, rdev, UI_RPMSG_EPT_NAME,
                         RPMSG_ADDR_ANY, RPMSG_ADDR_ANY, ui_rpmsg_rx, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[%s] rpmsg_create_ept failed: %d\n", TAG, ret);
      return;
    }

  s->created = true;
  syslog(LOG_INFO, "[%s] endpoint up → CP UI renderer\n", TAG);
}

static void ui_rpmsg_destroyed(struct rpmsg_device *rdev, void *priv)
{
  struct ui_rpmsg_client_state *s = priv;

  if (s->created &&
      strcmp(rpmsg_get_cpuname(rdev), UI_RPMSG_REMOTE_CPU) == 0)
    {
      rpmsg_destroy_ept(&s->ept);
      s->created = false;
      syslog(LOG_WARNING, "[%s] endpoint down (CP UI gone)\n", TAG);
    }
}

/* ── send helper ────────────────────────────────────────────────────── */

static int ui_rpmsg_send(const struct ui_rpmsg_msg *m)
{
  size_t total = sizeof(*m) - UI_RPMSG_MAX_PAYLOAD + m->payload_len;

  if (!ui_rpmsg_ready())
    {
      return -ENOTCONN;
    }

  return rpmsg_send_offchannel_raw(&g_ui_cli.ept, g_ui_cli.ept.addr,
                                    g_ui_cli.ept.dest_addr,
                                    (void *)m, total, false);
}

/* ── public API: lvgl_ui_channel_* + lvgl_toast_* (drop-in shim) ────── */

int lvgl_ui_channel_init(void)
{
  int ret;

  if (g_ui_cli.registered)
    {
      return 0;  /* idempotent */
    }

  g_ui_cli.registered = true;
  ret = rpmsg_register_callback(&g_ui_cli, ui_rpmsg_created,
                                ui_rpmsg_destroyed, NULL, NULL);
  if (ret < 0)
    {
      g_ui_cli.registered = false;
      syslog(LOG_ERR, "[%s] rpmsg_register_callback failed: %d\n", TAG, ret);
      return ret;
    }

  /* The CP ai_agent_ui app self-starts LVGL on its own; this endpoint only
   * needs to be ready before the first bubble is dispatched.  The link comes
   * up at boot, so by the time the AP agent has network + a reply to show,
   * ui_rpmsg_ready() is true. */
  syslog(LOG_INFO, "[%s] registered; waiting for CP UI link\n", TAG);
  return 0;
}

int lvgl_ui_channel_start(void)
{
  /* CP's LVGL loop is started by cp_ui_main on boot; nothing to do here. */
  return 0;
}

void lvgl_ui_channel_stop(void)
{
  struct ui_rpmsg_msg m;
  memset(&m, 0, sizeof(m));
  m.op = UI_RPMSG_STOP;
  (void)ui_rpmsg_send(&m);
}

void lvgl_ui_channel_show(void)
{
  struct ui_rpmsg_msg m;
  memset(&m, 0, sizeof(m));
  m.op = UI_RPMSG_SHOW;
  (void)ui_rpmsg_send(&m);
}

int lvgl_ui_channel_send(const char *text)
{
  struct ui_rpmsg_msg m;
  size_t tlen;
  int ret;

  if (text == NULL)
    {
      return -EINVAL;
    }

  /* TTS plays locally on AP (real voice_channel_speak); the bubble is
   * sent to CP separately.  Original single-core send() did both in one
   * call, so we preserve both side-effects here.
   *
   * Offline, TTS can only fail — after a ~20 s network timeout — and the
   * bubble would sit behind it, leaving the user staring at a blank
   * screen.  Skip TTS when there is no network so the on-screen reason
   * (e.g. the "未联网" card) appears immediately. */
  if (network_is_connected())
    {
      (void)voice_channel_speak(text);
    }

  memset(&m, 0, sizeof(m));
  m.op = UI_RPMSG_SEND;
  tlen = strlen(text);
  if (tlen > UI_RPMSG_MAX_TEXT)
    {
      tlen = UI_RPMSG_MAX_TEXT;
    }
  m.payload_len = (uint16_t)tlen;
  memcpy(m.payload, text, tlen);

  ret = ui_rpmsg_send(&m);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[%s] send bubble to CP failed: %d\n", TAG, ret);
    }
  return ret;
}

/* Extended send: bubble with an explicit card bar color (0=auto).  Like
 * send(), TTS plays locally on AP (voice_channel_speak) and only the
 * colored bubble goes to CP. */
int lvgl_ui_channel_send_ex(const char *text, uint32_t bar_color)
{
  struct ui_rpmsg_msg m;
  size_t tlen, plen;
  int ret;

  if (text == NULL)
    {
      return -EINVAL;
    }
  /* See lvgl_ui_channel_send(): no network → skip the doomed TTS wait so
   * the colored card (usually an error explanation) shows right away. */
  if (network_is_connected())
    {
      (void)voice_channel_speak(text);
    }

  memset(&m, 0, sizeof(m));
  m.op = UI_RPMSG_SEND_EX;

  /* payload = [bar_color:u32le][text..] */
  uint32_t *hdr = (uint32_t *)m.payload;
  hdr[0] = bar_color;
  plen = sizeof(uint32_t);

  tlen = strlen(text);
  if (tlen > UI_RPMSG_MAX_TEXT - plen)
    {
      tlen = UI_RPMSG_MAX_TEXT - plen;
    }
  memcpy(m.payload + plen, text, tlen);
  m.payload_len = (uint16_t)(plen + tlen);

  ret = ui_rpmsg_send(&m);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[%s] send_ex bubble to CP failed: %d\n", TAG, ret);
    }
  return ret;
}

/* User bubble: forward the recognized PTT utterance to CP as a right-aligned
 * blue bubble.  No TTS — the user's own words are not spoken back. */
int lvgl_ui_channel_send_user(const char *text)
{
  struct ui_rpmsg_msg m;
  size_t tlen;
  int ret;

  if (text == NULL)
    {
      return -EINVAL;
    }

  memset(&m, 0, sizeof(m));
  m.op = UI_RPMSG_SEND_USER;
  tlen = strlen(text);
  if (tlen > UI_RPMSG_MAX_TEXT)
    {
      tlen = UI_RPMSG_MAX_TEXT;
    }
  m.payload_len = (uint16_t)tlen;
  memcpy(m.payload, text, tlen);

  ret = ui_rpmsg_send(&m);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[%s] send_user bubble to CP failed: %d\n", TAG, ret);
    }
  return ret;
}

/* Drive the CP PTT button state from the AP voice pipeline. */
int lvgl_ui_channel_set_ptt(int state)
{
  struct ui_rpmsg_msg m;
  int ret;

  memset(&m, 0, sizeof(m));
  m.op = UI_RPMSG_SET_PTT;
  m.payload[0] = (uint8_t)state;
  m.payload_len = 1;

  ret = ui_rpmsg_send(&m);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[%s] set_ptt to CP failed: %d\n", TAG, ret);
    }
  return ret;
}

/* Drive the CP-side pet status ring (M7 差异化页) from the AP agent's
 * lifecycle state.  payload[0] = enum pet_state (0..5). */
int lvgl_ui_channel_set_pet_error(const char *reason)
{
  /* The SET_PET_STATE op carries no text payload, so the concrete reason
   * stays local to this core; the CP side shows its generic ERROR caption. */
  (void)reason;
  return lvgl_ui_channel_set_pet_state(5);
}

int lvgl_ui_channel_set_pet_state(int state)
{
  struct ui_rpmsg_msg m;
  int ret;

  if (state < 0 || state > 5)
    {
      return -EINVAL;
    }
  memset(&m, 0, sizeof(m));
  m.op = UI_RPMSG_SET_PET_STATE;
  m.payload[0] = (uint8_t)state;
  m.payload_len = 1;

  ret = ui_rpmsg_send(&m);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[%s] set_pet_state to CP failed: %d\n", TAG, ret);
    }
  return ret;
}

/* Switch the CP round-screen page from the AP side (debug `set_page` nsh
 * command and idle-rotate logic).  payload[0] = page index (0=pet,1=particle). */
int lvgl_ui_channel_set_page(int page)
{
  struct ui_rpmsg_msg m;
  int ret;

  if (page < 0 || page > 1)
    {
      return -EINVAL;
    }
  memset(&m, 0, sizeof(m));
  m.op = UI_RPMSG_SET_PAGE;
  m.payload[0] = (uint8_t)page;
  m.payload_len = 1;

  ret = ui_rpmsg_send(&m);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[%s] set_page to CP failed: %d\n", TAG, ret);
    }
  return ret;
}

/* Show the CP approval screen for a pending remote permission request.
 * payload = [tool\0hint\0]. */
int lvgl_ui_channel_set_approval(const char *tool, const char *hint)
{
  struct ui_rpmsg_msg m;
  size_t off = 0, tlen, hlen;
  int ret;

  /* Guard against NULL — memcpy(dst, NULL, 0) is UB even at zero length. */
  const char *st = tool != NULL ? tool : "";
  const char *sh = hint != NULL ? hint : "";

  memset(&m, 0, sizeof(m));
  m.op = UI_RPMSG_SET_APPROVAL;

  tlen = strlen(st);
  hlen = strlen(sh);
  if (tlen + hlen + 2 > UI_RPMSG_MAX_PAYLOAD)
    {
      /* Truncate hint first, then tool, keeping both NUL-terminated. */
      size_t room = UI_RPMSG_MAX_PAYLOAD - 2;   /* 2 NULs */
      if (tlen > room)
        {
          tlen = room;
          hlen = 0;
        }
      else if (tlen + hlen > room)
        {
          hlen = room - tlen;
        }
    }

  memcpy(m.payload + off, st, tlen); off += tlen;
  m.payload[off++] = '\0';
  memcpy(m.payload + off, sh, hlen); off += hlen;
  m.payload[off++] = '\0';
  m.payload_len = (uint16_t)off;

  ret = ui_rpmsg_send(&m);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[%s] set_approval to CP failed: %d\n", TAG, ret);
    }
  return ret;
}

/* Dismiss the CP approval screen. */
int lvgl_ui_channel_clear_approval(void)
{
  struct ui_rpmsg_msg m;
  int ret;

  memset(&m, 0, sizeof(m));
  m.op = UI_RPMSG_CLEAR_APPROVAL;

  ret = ui_rpmsg_send(&m);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[%s] clear_approval to CP failed: %d\n", TAG, ret);
    }
  return ret;
}

/* Notification page shim.  In this build the screen is owned by the other
 * core, so this side has no page state to track: keep the existing RPMsg toast
 * opcode (it renders on the CP) instead of inventing a second notice opcode.
 * Callers that need the approval-priority rules must run on the core that owns
 * the screen (the AP-LVGL_UI build). */
int lvgl_ui_channel_show_notice(const char *title, const char *body,
                                uint32_t bar_color)
{
  lvgl_toast_show_async(title, body, bar_color, LVGL_UI_NOTICE_MS);
  return 0;
}

int lvgl_ui_channel_notice_dismiss(void)
{
  return 0;
}

int lvgl_ui_channel_notice_visible(void)
{
  return 0;
}

/* Scroll the CP chat transcript. payload[0] = dir (0=up, 1=down). */int lvgl_ui_channel_scroll(int dir)
{
  struct ui_rpmsg_msg m;
  int ret;

  if (dir < 0 || dir > 1)
    {
      return -EINVAL;
    }
  memset(&m, 0, sizeof(m));
  m.op = UI_RPMSG_SCROLL;
  m.payload[0] = (uint8_t)dir;
  m.payload_len = 1;

  ret = ui_rpmsg_send(&m);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[%s] scroll to CP failed: %d\n", TAG, ret);
    }
  return ret;
}

/* Drop the CP-side conversation and bring the empty chat screen up (a new
 * gateway session started). payload_len = 0. */
int lvgl_ui_channel_clear_history(void)
{
  struct ui_rpmsg_msg m;
  int ret;

  memset(&m, 0, sizeof(m));
  m.op = UI_RPMSG_CLEAR_HISTORY;
  m.payload_len = 0;

  ret = ui_rpmsg_send(&m);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[%s] clear history on CP failed: %d\n", TAG, ret);
    }
  return ret;
}

void lvgl_toast_show_async(const char *title, const char *body,
                           uint32_t bar_color, uint32_t duration_ms)
{
  struct ui_rpmsg_msg m;
  size_t off, tlen, blen;

  memset(&m, 0, sizeof(m));
  m.op = UI_RPMSG_TOAST;

  /* payload layout: [bar_color:u32le][duration_ms:u32le][title\0body\0] */
  uint32_t *hdr = (uint32_t *)m.payload;
  hdr[0] = bar_color;
  hdr[1] = duration_ms;
  off = 2 * sizeof(uint32_t);

  tlen = (title != NULL) ? strlen(title) + 1 : 0;
  blen = (body  != NULL) ? strlen(body)  + 1 : 0;

  if (off + tlen + blen > UI_RPMSG_MAX_PAYLOAD)
    {
      /* Truncate body first, then title, keeping both NUL-terminated. */
      size_t room = UI_RPMSG_MAX_PAYLOAD - off;
      if (tlen > room)
        {
          tlen = room;
          blen = 0;
        }
      else if (tlen + blen > room)
        {
          blen = room - tlen;
        }
      /* Ensure a NUL terminator even when truncated. */
      if (tlen > 0)
        {
          m.payload[off + tlen - 1] = '\0';
        }
      if (blen > 0)
        {
          m.payload[off + tlen + blen - 1] = '\0';
        }
    }

  if (tlen > 0)
    {
      memcpy(m.payload + off, title, tlen);
    }
  if (blen > 0)
    {
      memcpy(m.payload + off + tlen, body, blen);
    }
  m.payload_len = (uint16_t)(off + tlen + blen);

  (void)ui_rpmsg_send(&m);
}

#endif /* CONFIG_AI_AGENT_UI_RPMSG */
