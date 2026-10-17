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
    velaclaw_client_close(c);
    puts("test_local_client: OK (busy, stale, duplicate, failed submit, recovery)");
    return 0;
}
