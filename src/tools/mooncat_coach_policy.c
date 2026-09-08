#include "mooncat_coach_policy.h"

#include <stdio.h>

static struct mooncat_coach_decision_s mooncat_none(const char *reason_id)
{
    struct mooncat_coach_decision_s decision = {
        .action = MOONCAT_COACH_NONE,
        .priority = MOONCAT_PRIORITY_NONE,
        .suggested_duration_seconds = 0,
        .cooldown_seconds = 0,
        .reason_id = reason_id,
    };

    return decision;
}
struct mooncat_coach_decision_s mooncat_coach_evaluate(
    const struct mooncat_coach_observation_s *observation)
{
    struct mooncat_coach_decision_s decision;

    if (!observation || !observation->valid) {
        return mooncat_none("invalid_observation");
    }

    /* The first milestone is deliberately not physical-sensor evidence. */
    if (observation->source != MOONCAT_DATA_SOURCE_SIMULATED) {
        return mooncat_none("source_not_verified");
    }

    if (observation->do_not_disturb) {
        return mooncat_none("do_not_disturb");
    }

    if (observation->workout_active) {
        return mooncat_none("workout_active");
    }

    if (observation->battery_percent <= 5) {
        return mooncat_none("battery_critical");
    }

    if (observation->stress_score >= 80) {
        decision.action = MOONCAT_COACH_BREATHE;
        decision.priority = MOONCAT_PRIORITY_HIGH;
        decision.suggested_duration_seconds = 90;
        decision.cooldown_seconds = 30 * 60;
        decision.reason_id = "stress_high";
        return decision;
    }

    if (observation->inactivity_minutes >= 90) {
        decision.action = MOONCAT_COACH_WALK;
        decision.priority = MOONCAT_PRIORITY_NORMAL;
        decision.suggested_duration_seconds = 3 * 60;
        decision.cooldown_seconds = 60 * 60;
        decision.reason_id = "inactive_90m";
        return decision;
    }

    if (observation->sleep_debt_minutes >= 120) {
        decision.action = MOONCAT_COACH_REST;
        decision.priority = MOONCAT_PRIORITY_NORMAL;
        decision.suggested_duration_seconds = 5 * 60;
        decision.cooldown_seconds = 2 * 60 * 60;
        decision.reason_id = "sleep_debt_120m";
        return decision;
    }

    if (observation->inactivity_minutes >= 45) {
        decision.action = MOONCAT_COACH_STRETCH;
        decision.priority = MOONCAT_PRIORITY_GENTLE;
        decision.suggested_duration_seconds = 60;
        decision.cooldown_seconds = 45 * 60;
        decision.reason_id = "inactive_45m";
        return decision;
    }

    return mooncat_none("no_intervention");
}

const char *mooncat_coach_action_name(enum mooncat_coach_action_e action)
{
    switch (action) {
    case MOONCAT_COACH_BREATHE:
        return "breathe";
    case MOONCAT_COACH_WALK:
        return "walk";
    case MOONCAT_COACH_REST:
        return "rest";
    case MOONCAT_COACH_STRETCH:
        return "stretch";
    case MOONCAT_COACH_NONE:
    default:
        return "none";
    }
}

int mooncat_coach_format_notification(
    const struct mooncat_coach_observation_s *observation,
    const struct mooncat_coach_decision_s *decision,
    char *output, size_t output_size)
{
    int written;

    if (!observation || !decision || !output || output_size == 0 ||
        decision->action == MOONCAT_COACH_NONE ||
        observation->source != MOONCAT_DATA_SOURCE_SIMULATED) {
        return -1;
    }

    switch (decision->action) {
    case MOONCAT_COACH_BREATHE:
        written = snprintf(output, output_size,
                           "[DEMO] Stress %u/100. Breathe with Mooncat for 90 seconds.",
                           (unsigned int)observation->stress_score);
        break;
    case MOONCAT_COACH_WALK:
        written = snprintf(output, output_size,
                           "[DEMO] Inactive for %u minutes. Take a 3-minute walk.",
                           (unsigned int)observation->inactivity_minutes);
        break;
    case MOONCAT_COACH_REST:
        written = snprintf(output, output_size,
                           "[DEMO] Sleep debt is %u minutes. Schedule a 5-minute recovery break.",
                           (unsigned int)observation->sleep_debt_minutes);
        break;
    case MOONCAT_COACH_STRETCH:
        written = snprintf(output, output_size,
                           "[DEMO] Inactive for %u minutes. Stretch for 60 seconds.",
                           (unsigned int)observation->inactivity_minutes);
        break;
    case MOONCAT_COACH_NONE:
    default:
        return -1;
    }

    return written >= 0 && (size_t)written < output_size ? 0 : -1;
}
