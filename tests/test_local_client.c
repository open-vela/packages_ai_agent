/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <velaclaw/client.h>
#include "core/message_bus_tap.h"

static mbus_tap_cb_t tap;
static void *tap_cookie;
static int fail_push;
static int calls;
static agent_msg_t sent;
int agent_loop_ensure_started(void) { return 0; }
int mbus_tap_register(const char *channel, mbus_tap_cb_t cb, void *cookie)
{ (void)channel; tap = cb; tap_cookie = cookie; return 0; }
void mbus_tap_unregister(const char *channel) { (void)channel; }
int message_bus_push_inbound(const agent_msg_t *msg)
{
    if (fail_push) return -1;
    sent = *msg;
    free(sent.content);
    sent.content = NULL;
    return 0;
}
static void reply(int status, const char *text, void *cookie)
{ assert(!status); assert(!strcmp(text, "reply")); assert(cookie == &calls); calls++; }
static void failure(int status, const char *text, void *cookie)
{ assert(status == -EIO); assert(!strcmp(text, "reply")); assert(cookie == &calls); calls++; }

int main(void)
{
    velaclaw_client_t *c = velaclaw_client_open("test");
    velaclaw_ask_req_t req = {"test", 10};
    assert(c);
    assert(!velaclaw_ask(c, &req, reply, &calls));
    agent_msg_t old = sent;
    agent_msg_t progress = sent;
    progress.content = "working";
    progress.is_progress = true;
    tap(&progress, tap_cookie);
    assert(calls == 0);
    assert(velaclaw_ask(c, &req, reply, &calls) == -EBUSY);
    usleep(20000);
    assert(!velaclaw_ask(c, &req, reply, &calls));
    agent_msg_t current = sent;
    old.content = "reply";
    tap(&old, tap_cookie);
    assert(calls == 0);
    current.content = "reply";
    tap(&current, tap_cookie);
    assert(calls == 1);
    tap(&current, tap_cookie);
    assert(calls == 1);
    fail_push = 1;
    assert(velaclaw_ask(c, &req, reply, &calls) < 0);
    fail_push = 0;
    assert(!velaclaw_ask(c, &req, reply, &calls));
    current = sent;
    current.content = "reply";
    tap(&current, tap_cookie);
    assert(calls == 2);
    /* An actual processing request owns this cancellation flag until end.
     * Replacing a request cannot reset the old flag or clear the new one. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t now = (uint32_t)((uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    req.timeout_ms = 1000;
    assert(!velaclaw_ask_async_deadline(c, &req, now + 1000, reply, &calls));
    agent_msg_t controlled_old = sent;
    vela_req_ctl_t ctl = {0};
    volatile int cancel_old = 0;
    volatile int cancel_new = 0;
    assert(velaclaw_request_begin(sent.chat_id, &ctl, &cancel_old) == 1);
    assert(ctl.deadline_ms == now + 1000 && ctl.cancel == &cancel_old);
    velaclaw_client_abandon(c);
    assert(cancel_old == 1);
    assert(!velaclaw_ask_async_deadline(c, &req, now + 1000, reply, &calls));
    assert(velaclaw_request_begin(controlled_old.chat_id, &ctl, &cancel_old) < 0);
    assert(velaclaw_request_begin(sent.chat_id, &ctl, &cancel_new) == 1);
    velaclaw_request_end(&cancel_old);
    velaclaw_client_abandon(c);
    assert(cancel_new == 1);
    velaclaw_request_end(&cancel_new);

    /* Uncontrolled background work remains independent of a voice cancel. */
    assert(!velaclaw_ask(c, &req, reply, &calls));
    assert(velaclaw_request_begin(sent.chat_id, &ctl, &cancel_new) == 0);
    cancel_new = 0;
    velaclaw_client_abandon(c);
    assert(cancel_new == 0);
    assert(velaclaw_ask_async_deadline(c, &req, now - 1, reply, &calls) == -ETIMEDOUT);

    /* Failures retain their request identity; an older failure cannot mark
     * the replacement, and an older success cannot complete it. */
    assert(!velaclaw_ask_async_deadline(c, &req, now + 1000, reply, &calls));
    old = sent;
    velaclaw_client_abandon(c);
    assert(!velaclaw_ask_async_deadline(c, &req, now + 1000, reply, &calls));
    velaclaw_request_failed(old.chat_id, -EIO);
    current = sent;
    current.content = "reply";
    tap(&current, tap_cookie);
    assert(calls == 3);
    assert(!velaclaw_ask_async_deadline(c, &req, now + 1000, failure, &calls));
    velaclaw_request_failed(sent.chat_id, -EIO);
    current = sent;
    current.content = "reply";
    tap(&current, tap_cookie);
    assert(calls == 4);

    assert(!velaclaw_ask_async_deadline(c, &req, now + 1000, reply, &calls));
    assert(velaclaw_request_begin(sent.chat_id, &ctl, &cancel_new) == 1);
    velaclaw_client_close(c);
    assert(cancel_new == 1);
    velaclaw_request_end(&cancel_new);
    puts("test_local_client: OK (busy, stale, duplicate, control lifetime, isolated cancel, close, recovery)");
    return 0;
}
