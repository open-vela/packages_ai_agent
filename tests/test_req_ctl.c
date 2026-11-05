/*
 * Host regression test for the shared request deadline (BVO-07).
 *
 * The voice path gives one 30 s budget to recognition, the agent round and
 * the first playable audio.  Each stage has to ask how much is left; a stage
 * that starts a fresh 30 s would let the whole round run three times over.
 * These cases pin the arithmetic, including the two mistakes that are easy
 * to make: treating 0 as expired, and breaking across the monotonic wrap.
 *
 * 用法：
 *   gcc -O2 -g -Wall -Wextra -I src tests/test_req_ctl.c -o /tmp/test_req_ctl
 *   /tmp/test_req_ctl
 */

#include "infra/req_ctl.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            g_fail++;                                                   \
        }                                                               \
    } while (0)

static void test_no_deadline(void)
{
    vela_req_ctl_t ctl;

    memset(&ctl, 0, sizeof(ctl));

    /* Zero means "unbounded", not "expired".  A caller with no budget must
     * still be able to send a request. */

    CHECK(vela_req_remaining_ms(0, 1000) == -1);
    CHECK(vela_req_budget_ms(&ctl, 1000) == -1);
    CHECK(!vela_req_stop(&ctl, 1000));
    CHECK(vela_req_io_timeout_ms(&ctl, 1000, VELA_REQ_IO_SLICE_MS)
          == VELA_REQ_IO_SLICE_MS);

    /* A NULL control block behaves the same way. */

    CHECK(vela_req_budget_ms(NULL, 1000) == -1);
    CHECK(!vela_req_stop(NULL, 1000));
}

static void test_remaining(void)
{
    CHECK(vela_req_remaining_ms(31000, 1000) == 30000);
    CHECK(vela_req_remaining_ms(31000, 30000) == 1000);
    CHECK(vela_req_remaining_ms(31000, 31000) == 0);
    CHECK(vela_req_remaining_ms(31000, 31001) == 0);
    CHECK(vela_req_remaining_ms(31000, 99000) == 0);
}

static void test_wrap(void)
{
    /* A deadline 5 s past the 32-bit wrap is still 5 s away. */

    uint32_t now = 0xfffff000u;
    uint32_t deadline = now + 5000u;

    CHECK(vela_req_remaining_ms(deadline, now) == 5000);
    CHECK(vela_req_remaining_ms(deadline, now + 1000u) == 4000);
    CHECK(vela_req_remaining_ms(deadline, deadline) == 0);
    CHECK(vela_req_remaining_ms(deadline, deadline + 1u) == 0);
}

static void test_stop_and_cancel(void)
{
    vela_req_ctl_t ctl;
    volatile int cancel = 0;

    ctl.deadline_ms = 5000;
    ctl.cancel = &cancel;

    CHECK(!vela_req_stop(&ctl, 4000));
    CHECK(vela_req_stop(&ctl, 5000));

    cancel = 1;
    CHECK(vela_req_cancelled(&ctl));
    CHECK(vela_req_stop(&ctl, 0));

    /* A cancel without a deadline still stops. */

    ctl.deadline_ms = 0;
    CHECK(vela_req_stop(&ctl, 12345));

    cancel = 0;
    CHECK(!vela_req_stop(&ctl, 12345));
}

static void test_io_timeout(void)
{
    vela_req_ctl_t ctl;

    ctl.deadline_ms = 10000;
    ctl.cancel = NULL;

    /* Never longer than the slice. */

    CHECK(vela_req_io_timeout_ms(&ctl, 1000, VELA_REQ_IO_SLICE_MS)
          == VELA_REQ_IO_SLICE_MS);

    /* Never longer than the budget. */

    CHECK(vela_req_io_timeout_ms(&ctl, 9800, VELA_REQ_IO_SLICE_MS) == 200);

    /* Spent budget yields zero, so the caller stops instead of blocking. */

    CHECK(vela_req_io_timeout_ms(&ctl, 10000, VELA_REQ_IO_SLICE_MS) == 0);
    CHECK(vela_req_io_timeout_ms(&ctl, 20000, VELA_REQ_IO_SLICE_MS) == 0);
}

static void test_stage_shares_one_budget(void)
{
    /* The point of the header: three stages of 10 s each inside a 30 s
     * budget, then a fourth stage that must see zero. */

    vela_req_ctl_t ctl;
    uint32_t now = 1000;

    ctl.deadline_ms = now + 30000;
    ctl.cancel = NULL;

    now += 10000;
    CHECK(vela_req_remaining_ms(ctl.deadline_ms, now) == 20000);

    now += 10000;
    CHECK(vela_req_remaining_ms(ctl.deadline_ms, now) == 10000);

    now += 10000;
    CHECK(vela_req_remaining_ms(ctl.deadline_ms, now) == 0);
    CHECK(vela_req_stop(&ctl, now));
}

int main(void)
{
    test_no_deadline();
    test_remaining();
    test_wrap();
    test_stop_and_cancel();
    test_io_timeout();
    test_stage_shares_one_budget();

    if (g_fail == 0) {
        printf("test_req_ctl: ok\n");
        return 0;
    }

    printf("test_req_ctl: %d failure(s)\n", g_fail);
    return 1;
}
