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
 * ui_rpmsg_server.c — CP-side RPMsg receiver for the AP→CP UI channel (路 B).
 *
 * Lives on the CP core, which owns the round LCD + LVGL.  The CP app
 * (cp_ui_main) self-starts LVGL and the chat screen, then calls
 * ui_rpmsg_server_init() to open a dedicated RPMsg endpoint.  When the AP
 * agent (ui_rpmsg_client.c) sends a ui_rpmsg_msg, this server's rx callback
 * deserializes it and calls the REAL lvgl_ui_channel_* / lvgl_toast_* on
 * this core, where LVGL actually runs.
 *
 * TTS: lvgl_ui_channel_send() on CP internally calls voice_channel_speak(),
 * which is the cp_stubs.c no-op here — so the bubble renders on CP and audio
 * is handled separately by the AP shim.  No double audio.
 *
 * Mirrors the RPMsg endpoint pattern in
 * chips/bk7258/mb_ipc/bk_mailbox_rpmsg.c.  Flat build exposes the kernel
 * rpmsg symbols to this userspace builtin.
 *
 * Only compiled into CONFIG_EXAMPLES_AI_AGENT_UI_CP.
 */

#include <nuttx/config.h>

#ifdef CONFIG_EXAMPLES_AI_AGENT_UI_CP

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/rpmsg/rpmsg.h>

#include "ui/ui_rpmsg_proto.h"
#include "ui/lvgl_ui_channel.h"
#include "ui/lvgl_toast.h"

#define TAG "ui_rpmsg_server"

struct ui_rpmsg_server_state
{
  struct rpmsg_endpoint ept;
  bool registered;
  bool created;
};

static struct ui_rpmsg_server_state g_ui_srv;

/* On the CP the peer we care about is the AP core. */
#define UI_RPMSG_PEER_CPU "ap"

/* ── dispatch a received message to the real LVGL functions ──────────── */

static int ui_rpmsg_server_rx(struct rpmsg_endpoint *ept, void *data,
                               size_t len, uint32_t source, void *priv)
{
  const struct ui_rpmsg_msg *m = data;
  size_t need;

  (void)ept;
  (void)source;
  (void)priv;

  if (len < 4)  /* header = op + reserved + payload_len */
    {
      return -EINVAL;
    }

  need = 4 + (size_t)m->payload_len;
  if (len < need)
    {
      syslog(LOG_WARNING, "[%s] short msg op=%u len=%zu need=%zu\n",
             TAG, m->op, len, need);
      return -EINVAL;
    }

  switch (m->op)
    {
      case UI_RPMSG_INIT:
        (void)lvgl_ui_channel_init();
        break;

      case UI_RPMSG_START:
        (void)lvgl_ui_channel_start();
        break;

      case UI_RPMSG_STOP:
        lvgl_ui_channel_stop();
        break;

      case UI_RPMSG_SHOW:
        lvgl_ui_channel_show();
        break;

      case UI_RPMSG_SEND:
        {
          /* Wire text is len-prefixed, not NUL-terminated. */
          char text[UI_RPMSG_MAX_TEXT + 1];
          size_t tl = m->payload_len;
          if (tl > UI_RPMSG_MAX_TEXT)
            {
              tl = UI_RPMSG_MAX_TEXT;
            }
          memcpy(text, m->payload, tl);
          text[tl] = '\0';
          /* Real CP send(): builds the bubble + voice_channel_speak()
             (no-op stub here).  Audio is handled on the AP side. */
          (void)lvgl_ui_channel_send(text);
        }
        break;

      case UI_RPMSG_SEND_EX:
        {
          /* payload = [bar_color:u32le][text..] (text not NUL-terminated). */
          if (m->payload_len < sizeof(uint32_t))
            {
              break;
            }
          uint32_t bar_color;
          memcpy(&bar_color, m->payload, sizeof(uint32_t));
          size_t tl = m->payload_len - sizeof(uint32_t);
          char text[UI_RPMSG_MAX_TEXT + 1];
          if (tl > UI_RPMSG_MAX_TEXT)
            {
              tl = UI_RPMSG_MAX_TEXT;
            }
          memcpy(text, m->payload + sizeof(uint32_t), tl);
          text[tl] = '\0';
          (void)lvgl_ui_channel_send_ex(text, bar_color);
        }
        break;

      case UI_RPMSG_SET_PTT:
        {
          if (m->payload_len < 1)
            {
              break;
            }
          (void)lvgl_ui_channel_set_ptt((int)m->payload[0]);
        }
        break;

      case UI_RPMSG_SEND_USER:
        {
          /* Echo the recognized user utterance as a right-aligned bubble. */
          char text[UI_RPMSG_MAX_TEXT + 1];
          size_t tl = m->payload_len;
          if (tl > UI_RPMSG_MAX_TEXT)
            {
              tl = UI_RPMSG_MAX_TEXT;
            }
          memcpy(text, m->payload, tl);
          text[tl] = '\0';
          (void)lvgl_ui_channel_send_user(text);
        }
        break;

      case UI_RPMSG_SET_PET_STATE:
        {
          /* Drive the pet status ring home screen (M7 差异化页) from the AP
           * agent's lifecycle state.  No-op if PET_STATUS is off. */
          if (m->payload_len < 1)
            {
              break;
            }
          (void)lvgl_ui_channel_set_pet_state((int)m->payload[0]);
        }
        break;

      case UI_RPMSG_SET_PAGE:
        {
          /* Switch the round-screen page (0=pet home, 1=particle flow).
           * Delegates to the CP-side lvgl_ui_channel_set_page, which gates
           * each page behind its CONFIG guard. */
          if (m->payload_len < 1)
            {
              break;
            }
          (void)lvgl_ui_channel_set_page((int)m->payload[0]);
        }
        break;

      case UI_RPMSG_SET_APPROVAL:
        {
          /* payload = [tool\0hint\0] (both NUL-terminated). */
          char tool[UI_RPMSG_MAX_TEXT + 1];
          char hint[UI_RPMSG_MAX_TEXT + 1];
          const char *p = (const char *)m->payload;
          size_t avail = m->payload_len;
          size_t tl = strnlen(p, avail);
          const char *h = "";
          if (tl < avail)
            {
              h = p + tl + 1;
            }
          size_t copy = tl > UI_RPMSG_MAX_TEXT ? UI_RPMSG_MAX_TEXT : tl;
          memcpy(tool, p, copy);
          tool[copy] = '\0';
          /* hint is bounded by the remaining bytes after tool's NUL. */
          size_t ha = (tl < avail) ? (avail - tl - 1) : 0;
          size_t hc = strnlen(h, ha);
          if (hc > UI_RPMSG_MAX_TEXT)
            {
              hc = UI_RPMSG_MAX_TEXT;
            }
          memcpy(hint, h, hc);
          hint[hc] = '\0';
          (void)lvgl_ui_channel_set_approval(tool, hint);
        }
        break;

      case UI_RPMSG_CLEAR_APPROVAL:
        (void)lvgl_ui_channel_clear_approval();
        break;

      case UI_RPMSG_SCROLL:
        {
          /* payload[0] = dir (0=up/older, 1=down/newer). */
          if (m->payload_len < 1)
            {
              break;
            }
          (void)lvgl_ui_channel_scroll((int)m->payload[0]);
        }
        break;

      case UI_RPMSG_CLEAR_HISTORY:
        (void)lvgl_ui_channel_clear_history();
        break;

      case UI_RPMSG_TOAST:
        {
          if (m->payload_len < 2 * sizeof(uint32_t))
            {
              break;
            }
          uint32_t bar_color;
          uint32_t duration_ms;
          memcpy(&bar_color,  m->payload + 0, sizeof(uint32_t));
          memcpy(&duration_ms, m->payload + 4, sizeof(uint32_t));

          const char *title = (const char *)(m->payload + 8);
          size_t avail = m->payload_len - 8;
          /* title is NUL-terminated within the wire payload. */
          size_t tlen = strnlen(title, avail);
          const char *body = "";
          if (tlen < avail)
            {
              body = title + tlen + 1;  /* skip the NUL after title */
            }
          lvgl_toast_show_async(title, body, bar_color, duration_ms);
        }
        break;

      default:
        syslog(LOG_WARNING, "[%s] unknown op %u\n", TAG, m->op);
        break;
    }

  return 0;
}

/* ── RPMsg device appear/disappear callbacks ─────────────────────────── */

static void ui_rpmsg_server_created(struct rpmsg_device *rdev, void *priv)
{
  struct ui_rpmsg_server_state *s = priv;
  int ret;

  if (strcmp(rpmsg_get_cpuname(rdev), UI_RPMSG_PEER_CPU) != 0 || s->created)
    {
      return;
    }

  ret = rpmsg_create_ept(&s->ept, rdev, UI_RPMSG_EPT_NAME,
                         RPMSG_ADDR_ANY, RPMSG_ADDR_ANY,
                         ui_rpmsg_server_rx, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[%s] rpmsg_create_ept failed: %d\n", TAG, ret);
      return;
    }

  s->created = true;
  syslog(LOG_INFO, "[%s] endpoint up ← AP agent UI commands\n", TAG);
}

static void ui_rpmsg_server_destroyed(struct rpmsg_device *rdev, void *priv)
{
  struct ui_rpmsg_server_state *s = priv;

  if (s->created &&
      strcmp(rpmsg_get_cpuname(rdev), UI_RPMSG_PEER_CPU) == 0)
    {
      rpmsg_destroy_ept(&s->ept);
      s->created = false;
      syslog(LOG_WARNING, "[%s] endpoint down (AP agent gone)\n", TAG);
    }
}

/* ── public init (called from cp_ui_main) ────────────────────────────── */

int ui_rpmsg_server_init(void)
{
  int ret;

  if (g_ui_srv.registered)
    {
      return 0;
    }

  g_ui_srv.registered = true;
  ret = rpmsg_register_callback(&g_ui_srv, ui_rpmsg_server_created,
                                ui_rpmsg_server_destroyed, NULL, NULL);
  if (ret < 0)
    {
      g_ui_srv.registered = false;
      syslog(LOG_ERR, "[%s] rpmsg_register_callback failed: %d\n", TAG, ret);
      return ret;
    }

  syslog(LOG_INFO, "[%s] registered; waiting for AP UI link\n", TAG);
  return 0;
}

#endif /* CONFIG_EXAMPLES_AI_AGENT_UI_CP */
