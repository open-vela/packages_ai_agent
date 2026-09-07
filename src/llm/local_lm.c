/*
 * local_lm.c - On-device language model fallback for the agent loop.
 *
 * See local_lm.h.  The model itself lives in the team repo
 * (app/ai_agent/ai_lm.cxx + model/), referenced from CMakeLists.txt through
 * the packages/demos linkfile.
 */

#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "local_lm.h"

#ifdef CONFIG_TFLITEMICRO
#  include "ai_lm.h"
#endif

#define TAG "local_lm"

bool local_lm_available(void)
{
#ifdef CONFIG_TFLITEMICRO
    return true;
#else
    return false;
#endif
}

int local_lm_reply(const char *user_text, char **out)
{
#ifdef CONFIG_TFLITEMICRO
    char reply[AI_LM_REPLY_MAX];

    if (!user_text || !out) {
        return -1;
    }

    if (ai_lm_init() != 0) {
        syslog(LOG_WARNING, "[%s] init failed\n", TAG);
        return -1;
    }

    ai_lm_cancel_clear();
    reply[0] = '\0';
    if (ai_lm_agent_reply(user_text, reply, sizeof(reply)) != 0 ||
        reply[0] == '\0') {
        syslog(LOG_WARNING, "[%s] no reply for \"%s\"\n", TAG, user_text);
        return -1;
    }

    *out = strdup(reply);
    if (!*out) {
        return -1;
    }

    syslog(LOG_INFO, "[%s] answered locally: %s\n", TAG, *out);
    return 0;
#else
    (void)user_text;
    (void)out;
    return -1;
#endif
}
