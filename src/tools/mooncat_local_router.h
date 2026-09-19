/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOONCAT_ROUTER_FEATURE_COUNT 2048

enum mooncat_router_intent_e {
    MOONCAT_INTENT_COACH_NOW = 0,
    MOONCAT_INTENT_COACH_PREVIEW = 1,
    MOONCAT_INTENT_COACH_SCHEDULE = 2,
    MOONCAT_INTENT_COACH_LIST = 3,
    MOONCAT_INTENT_FALLBACK = 4,
};

struct mooncat_router_prediction_s {
    enum mooncat_router_intent_e intent;
    float confidence;
    float margin;
};

void mooncat_router_featurize(const char *text, uint8_t *features,
                              size_t feature_count);

int mooncat_intent_model_predict(
    const char *text, struct mooncat_router_prediction_s *prediction);

/* Returns a heap reply for a locally executed intent, or NULL for LLM fallback. */
char *mooncat_local_router_handle(const char *text);

#ifdef __cplusplus
}
#endif
