/* SPDX-License-Identifier: Apache-2.0 */
#ifndef AGENT_VOICE_SCHED_H
#define AGENT_VOICE_SCHED_H
#include <stdbool.h>
#include <pthread.h>
#include <sched.h>

struct agent_voice_sched {
    int policy;
    struct sched_param param;
    bool changed;
};

/* HMI 文件工作线程也使用 RR 100。语音模型与其公平轮转；后台恢复原策略。 */
static inline int agent_voice_sched_enter(struct agent_voice_sched *saved, bool voice)
{
    saved->changed = false;
#ifdef CONFIG_VG_HMI
    if (voice) {
        int rc = pthread_getschedparam(pthread_self(), &saved->policy, &saved->param);
        if (rc) return rc;
        struct sched_param foreground = saved->param;
        foreground.sched_priority = 100;
        rc = pthread_setschedparam(pthread_self(), SCHED_RR, &foreground);
        if (rc) return rc;
        saved->changed = true;
    }
#else
    (void)voice;
#endif
    return 0;
}

static inline int agent_voice_sched_leave(struct agent_voice_sched *saved)
{
    if (!saved->changed) return 0;
    int rc = pthread_setschedparam(pthread_self(), saved->policy, &saved->param);
    if (!rc) saved->changed = false;
    return rc;
}
#endif
