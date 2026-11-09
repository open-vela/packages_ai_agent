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

/* Remote-control channel: lets this device drive a remote ACP agent
 * (mimocode / kiro-cli) that runs on a PC.
 *
 * Unlike the other channels, which bring a user's messages *into* the local
 * agent, this one reaches *out*: the device is a client of a remote agent. A
 * PC-side bridge speaks ACP to the agent and the frozen link contract to us, so
 * the device never sees ACP session IDs, raw tool arguments, or credentials.
 *
 * Permission decisions are deliberately not part of the tool surface. See
 * remote_ctrl_channel_decide().
 */

#pragma once

#include "agent_compat.h"
#include "remote/remote_link.h"
#include "remote/remote_session.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_AI_AGENT_REMOTE_CTRL

/* ── Lifecycle ─────────────────────────────────────────────────────── */

/* Reads configuration and prepares the session. Returns OK even when no broker
 * is configured; the channel then stays idle. */
int remote_ctrl_channel_init(void);

/* Starts the transport thread. No-op when unconfigured. */
int remote_ctrl_channel_start(void);

void remote_ctrl_channel_stop(void);

/* True once a broker is configured, whether or not the link is up. */
bool remote_ctrl_channel_enabled(void);

/* ── Operations ────────────────────────────────────────────────────── */

/* Consistent snapshot for display. Returns ERROR when unconfigured. */
int remote_ctrl_channel_status(struct remote_status_s* out);

/* Submits one prompt to the remote agent, then waits up to timeout_ms for the
 * turn to finish, copying the agent's text into `out`.
 *
 * A turn that stalls on a remote permission request will not finish until a
 * human answers on this device, so a timeout here is a normal outcome and is
 * reported as such rather than treated as an error.
 *
 * Only for callers running on a thread a human is not typing on — see
 * remote_ctrl_channel_submit(). */
int remote_ctrl_channel_prompt(const char* text, uint32_t timeout_ms,
    char* out, size_t out_size);

/* Submits a prompt and returns immediately. Agent output arrives through the
 * event handler and accumulates in the transcript. On failure `out` receives
 * the reason.
 *
 * This is what the console uses. Blocking the console thread on a turn would
 * put the permission commands out of reach, so a turn that stopped for approval
 * could never be approved: the wait would deadlock on the input it waits for. */
int remote_ctrl_channel_submit(const char* text, char* out, size_t out_size);

/* Copies the bounded transcript of the remote agent's output so far. */
int remote_ctrl_channel_transcript(char* out, size_t out_size);

/* Read-only billing refresh. */
int remote_ctrl_channel_query_usage(void);

/* Copies the remote agent's model list for display. */
int remote_ctrl_channel_model_view(struct remote_model_view_s* out);

/* Requests a model for the rest of the remote session. Returns OK when the
 * request was sent, which is not the same as applied: the bridge validates the
 * name and reports the outcome in a `models` message. */
int remote_ctrl_channel_select_model(const char* model);

/* Requests an approval-mode switch for the session. `mode` is the wire
 * spelling ("interactive" | "trust_reads" | "trust"). yolo is not accepted
 * here or in the codec: the dashboard is the only surface allowed to open it.
 * OK means the request left the device. */
int remote_ctrl_channel_set_mode(const char* mode);

/* Session chain (rnew / ruse): request the listing, open a fresh generation,
 * or switch to a generation number. All three are request-only — the gateway
 * answers with a `session_list` frame, delivered as REMOTE_EVENT_SESSIONS. */
int remote_ctrl_channel_request_sessions(void);
int remote_ctrl_channel_new_session(void);
int remote_ctrl_channel_switch_session(const char* target);
/* Copies the cached listing + the current generation for console display. */
int remote_ctrl_channel_session_view(char* list_out, size_t list_size,
    char* current_out, size_t current_size);

/* Answers a pending permission request from the remote agent.
 *
 * HUMAN INPUT ONLY. Reached from the NSH commands and, later, a GPIO key. This
 * is intentionally absent from the tool registry: approving a remote agent's
 * tool call is a human act, and a model that could both request and approve
 * would make the whole permission model decorative. */
int remote_ctrl_channel_decide(enum remote_decision_e decision);

/* Input-adapter plug point.
 *
 * What reaches the callbacks is an application intent, not a button press:
 * a physical key map, a touch-screen widget and a voice intent parser are
 * all adapters over the same vocabulary.  Each registers here and receives
 * the decide / pending / scroll / model-next / new-session entry points, so
 * an adapter knows the intents and nothing else about remote_ctrl -- and
 * remote_ctrl never learns what hardware the product has.  A product with a
 * touch screen registers a different adapter and leaves the channel
 * untouched.
 */
typedef void (*remote_decide_fn)(enum remote_decision_e decision, void *ctx);
typedef int  (*remote_pending_fn)(void *ctx);      /* 1 when an approval is up */
typedef void (*remote_scroll_fn)(int dir, void *ctx); /* 0=older, 1=newer */
typedef void (*remote_action_fn)(void *ctx);          /* model-next / new-session */
typedef int  (*remote_wake_fn)(void *ctx);            /* 1 when it ate the press as a wake */
typedef int  (*remote_dismiss_fn)(void *ctx);         /* 1 when it ate the press as a page dismiss */

struct remote_input_ops_s
{
  const char *name;                       /* "keymap" / "touch" / "voice" */

  /* The adapter receives the *application* surface, not raw button indices:
   * it maps its gesture vocabulary onto decide / scroll / model / session and
   * the channel owns the semantics.  Every callback except `decide` is
   * optional -- a product with no transcript leaves `scroll` NULL and the
   * channel simply never offers it.  `pending` lets the adapter pick a key
   * context (idle keys scroll, approval keys decide) without reading session
   * state itself.  `dismiss` and `wake` are "consume or pass" gates the
   * adapter must try BEFORE the keymap: a notice page (5 s) or the standby
   * clock owns the press and swallows it. */
  int (*attach)(remote_decide_fn decide,
                remote_pending_fn pending,
                remote_scroll_fn scroll,
                remote_action_fn model_next,
                remote_action_fn session_new,
                remote_dismiss_fn dismiss,
                remote_wake_fn wake,
                void *ctx);
  void (*detach)(void);
};

int remote_ctrl_register_input(const struct remote_input_ops_s *ops);

/* Debug-only: simulate a pending permission request so the on-device
 * approve/deny path can be exercised without a live broker.  Shows the
 * approval page and arms the next decide() against it. */
void remote_ctrl_channel_debug_pending(const char *tool, const char *hint);

/* Output-observer plug point, the mirror of the input one.
 *
 * A product shows progress with LEDs, a screen, a voice prompt or all
 * three.  Each registers here and receives the same state vocabulary;
 * remote_ctrl never learns which one the product has. */
struct remote_observer_ops_s
{
  const char *name;                       /* "led" / "lvgl" / "voice" */
  void (*on_state)(const char *text, void *ctx);
  void (*on_reply)(const char *text, void *ctx);
  void *ctx;
};

int remote_ctrl_register_observer(const struct remote_observer_ops_s *ops);

#else /* stubs */

static inline int remote_ctrl_channel_init(void) { return OK; }
static inline int remote_ctrl_channel_start(void) { return OK; }
static inline void remote_ctrl_channel_stop(void) { }
static inline bool remote_ctrl_channel_enabled(void) { return false; }

#endif /* CONFIG_AI_AGENT_REMOTE_CTRL */

#ifdef __cplusplus
}
#endif
