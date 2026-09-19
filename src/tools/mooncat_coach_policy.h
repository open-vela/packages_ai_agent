/*
 * Deterministic recovery-coach policy shared by the openVela agent tool and
 * host-side tests.  Milestone 1 intentionally accepts simulated observations
 * only: Gemini S1 sensor ingestion has not yet been integrated or verified.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
enum mooncat_data_source_e {
    MOONCAT_DATA_SOURCE_UNSPECIFIED = 0,
    MOONCAT_DATA_SOURCE_SIMULATED = 1,
    MOONCAT_DATA_SOURCE_LIVE = 2,
};

enum mooncat_coach_action_e {
    MOONCAT_COACH_NONE = 0,
    MOONCAT_COACH_BREATHE,
    MOONCAT_COACH_WALK,
    MOONCAT_COACH_REST,
    MOONCAT_COACH_STRETCH,
};

enum mooncat_coach_priority_e {
    MOONCAT_PRIORITY_NONE = 0,
    MOONCAT_PRIORITY_GENTLE,
    MOONCAT_PRIORITY_NORMAL,
    MOONCAT_PRIORITY_HIGH,
};

struct mooncat_coach_observation_s {
    enum mooncat_data_source_e source;
    bool valid;
    bool workout_active;
    bool do_not_disturb;
    uint16_t inactivity_minutes;
    uint16_t sleep_debt_minutes;
    uint8_t stress_score;
    uint8_t battery_percent;
};

struct mooncat_coach_decision_s {
    enum mooncat_coach_action_e action;
    enum mooncat_coach_priority_e priority;
    uint16_t suggested_duration_seconds;
    uint32_t cooldown_seconds;
    const char *reason_id;
};

/* Pure and deterministic: no clock, storage, transport or sensor access. */
struct mooncat_coach_decision_s mooncat_coach_evaluate(
    const struct mooncat_coach_observation_s *observation);

const char *mooncat_coach_action_name(enum mooncat_coach_action_e action);

/*
 * Produce the user-facing milestone-1 notification.  Every actionable string
 * carries the simulated-data marker so screenshots cannot be mistaken for
 * physical sensor evidence.
 */
int mooncat_coach_format_notification(
    const struct mooncat_coach_observation_s *observation,
    const struct mooncat_coach_decision_s *decision,
    char *output, size_t output_size);

#ifdef __cplusplus
}
#endif
