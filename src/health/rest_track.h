/****************************************************************************
 * rest_track.h - 休息时段追踪（体动估算）
 *
 * 数据源只有 MPU6050 的加速度。这里做的是「连续静止 + 时间窗」的粗判，
 * 不是多导睡眠图：没有心率、没有血氧、没有体动分期，也分不清"躺着睡"
 * 和"坐着不动"。因此 UI / 工具 / 日志统一写「休息（体动估算）」，
 * 并明确标注「非医疗级」。
 *
 * 判定（1 分钟 tick，5 秒轮询体动）：
 *   AWAKE  still <  30 分钟
 *   QUIET  still >= 30 分钟，且当前不在夜间窗口
 *   NIGHT  still >= 30 分钟，且本地小时 ∈ [22:00, 08:00)
 *
 * 体动恢复：still 重新 < 60 秒，或开机以来活动秒数增加 -> 结束当前段。
 * 夜间段结束时只有持续 >= 45 分钟才落盘；太短不记，避免把起夜/翻身
 * 记成一整夜。落盘前校验本地年份 ∈ [2024, 2027]，坏时钟（本板开机会
 * 偶读 2036）直接丢弃并 log warning。
 *
 * dur_s 的起点是"进入 NIGHT 状态"的时刻，不是最后一次体动的时刻：
 * 22:00 前已经静止的那一段不算进夜间休息，所以 dur_s 是保守下界。
 *
 * 持久化：/data/ai_agent/health/rest.json
 *   {"last_night":{"start":...,"end":...,"dur_s":...},
 *    "sessions":[{"start":...,"end":...,"dur_s":...}, ... 最多 7 条]}
 *
 * Team 181 - Contest 2026
 ****************************************************************************/
#ifndef AI_AGENT_REST_TRACK_H
#define AI_AGENT_REST_TRACK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REST_STATE_AWAKE  0
#define REST_STATE_QUIET  1
#define REST_STATE_NIGHT  2

/** 连续静止多久才算「休息」。30 分钟：坐着不动也可能达到，所以只是估算。 */
#define REST_QUIET_MIN_S        (30u * 60u)

/** 夜间段最短记录时长。45 分钟：短于这个的只是起夜/翻身，不记。 */
#define REST_NIGHT_MIN_RECORD_S (45u * 60u)

/** sessions 最多保留多少条（新的在前）。 */
#define REST_MAX_SESSIONS       7

/** 启动追踪线程（幂等）。返回 0 表示线程已跑起来（IMU 不可用时也会返回
 *  0，状态保持 awake，只是无法产生新数据）；-1 表示线程创建失败。 */
int rest_track_start(void);

/** 当前状态：REST_STATE_AWAKE / QUIET / NIGHT。
 *  状态每 60 秒推进一次，因此最多滞后 1 分钟；quiet_s 是实时的。 */
int rest_track_get_state(void);

/** 当前连续静止秒数（与 pedometer_get_still_seconds 一致）。
 *  IMU 没数据时返回 0，不把"读不到"伪装成"一直静止"。 */
uint32_t rest_track_current_quiet_s(void);

/** 最近一段已记录的夜间休息。
 *  返回 1 表示有数据（start/end 为本地 epoch 秒，dur_s 为秒）；
 *  返回 0 表示没有。参数允许传 NULL。 */
int rest_track_get_last_night(uint32_t *start, uint32_t *end, uint32_t *dur_s);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_REST_TRACK_H */
