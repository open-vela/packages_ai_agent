/*
 * local_lm.h - On-device language model fallback for the agent loop.
 *
 * Wraps the velaAI TFLite Micro model (ai_lm.cxx in the team repo) behind a
 * small C API so agent_loop.c does not need to know whether TFLM is compiled
 * in.  With CONFIG_TFLITEMICRO off every call reports "unavailable" and the
 * caller keeps its previous behaviour.
 */

#ifndef AI_AGENT_LOCAL_LM_H
#define AI_AGENT_LOCAL_LM_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Whether an on-device model is compiled into this image.
 */
bool local_lm_available(void);

/**
 * Answer user_text with the on-device model.
 *
 * Initialises the interpreter on first use (that allocates the tensor arena,
 * which lands in PSRAM).  Inference takes seconds, so call it from a worker
 * thread -- never from the LVGL render thread.
 *
 * @param user_text  The user's message.
 * @param out        On success, receives a malloc'd reply the caller frees.
 * @return 0 on success, -1 if unavailable or the model produced nothing.
 */
int local_lm_reply(const char *user_text, char **out);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_LOCAL_LM_H */
