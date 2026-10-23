/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <pthread.h>
#include <sched.h>
static int calls, policy = SCHED_FIFO, priority = 60, get_error, set_error;
static int fake_get(pthread_t thread, int *pol, struct sched_param *param)
{ (void)thread; if (get_error) return EINVAL; *pol=policy; param->sched_priority=priority; return 0; }
static int fake_set(pthread_t thread, int pol, const struct sched_param *param)
{ (void)thread; calls++; if (set_error) return EPERM; policy=pol; priority=param->sched_priority; return 0; }
#define pthread_getschedparam fake_get
#define pthread_setschedparam fake_set
#include "../src/infra/voice_sched.h"
int main(void)
{
    struct agent_voice_sched s={0};
    assert(!agent_voice_sched_enter(&s,false) && !s.changed && !calls);
    assert(!agent_voice_sched_leave(&s) && !calls);
    assert(!agent_voice_sched_enter(&s,true) && s.changed);
    assert(policy==SCHED_RR && priority==100);
    assert(!agent_voice_sched_leave(&s) && !s.changed);
    assert(policy==SCHED_FIFO && priority==60);
    assert(!agent_voice_sched_leave(&s) && calls==2);
    get_error=1; assert(agent_voice_sched_enter(&s,true)==EINVAL && !s.changed); get_error=0;
    set_error=1; assert(agent_voice_sched_enter(&s,true)==EPERM && !s.changed); set_error=0;
    assert(!agent_voice_sched_enter(&s,true)); set_error=1;
    assert(agent_voice_sched_leave(&s)==EPERM && s.changed); set_error=0;
    assert(!agent_voice_sched_leave(&s) && !s.changed && priority==60 && policy==SCHED_FIFO);
    puts("test_voice_sched: OK (foreground RR, original background policy, no-op and error restoration)");
}
