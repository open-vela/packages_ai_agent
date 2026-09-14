/*
 * hr_monitor.h - Simulated heart-rate monitor with threshold alert.
 *
 * 场景③（心率关怀）：真实传感器是赛后演进项（SIM/传感器在用户的
 * 演进路线里），大赛阶段用随机游走模拟器 + 阈值告警验证整条链路：
 * hr_monitor_tick（1Hz，由 pet_care 转发）→ 超阈值 →
 * pet_care_hr_report → 桌宠担心表情 + 告警气泡 + 历史记录。
 *
 * NSH 命令（hr_cmd.c）：
 *   hr_set <bpm>    设定模拟基线（演示不同人群）
 *   hr_set event    注入一次心率飙升事件（60s 内游走到高值再回落）
 *
 * 措辞约束：全部文案只用「提醒/记录」，不用「诊断/监测健康状态」。
 *
 * Team 181 - Contest 2026
 */

#ifndef AI_AGENT_HR_MONITOR_H
#define AI_AGENT_HR_MONITOR_H

#include <nuttx/config.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 阈值（bpm）：超过即触发 pet_care_hr_report。
 * Kconfig 项 AI_AGENT_HR_HIGH 的编译期兜底。 */
#ifdef CONFIG_AI_AGENT_HR_HIGH
#  define HR_MONITOR_HIGH_DEFAULT CONFIG_AI_AGENT_HR_HIGH
#else
#  define HR_MONITOR_HIGH_DEFAULT 120
#endif

/* 模拟基线范围 */
#define HR_MONITOR_BASE_MIN      65
#define HR_MONITOR_BASE_MAX      95

/* 注入事件的目标区间与持续时间 */
#define HR_MONITOR_EVENT_PEAK_MIN   130
#define HR_MONITOR_EVENT_PEAK_MAX   150
#define HR_MONITOR_EVENT_RISE_S     30   /* 游走到峰值用时（秒） */
#define HR_MONITOR_EVENT_FALL_S     30   /* 峰值回落到基线用时（秒） */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* 1Hz 心率模拟步进（由 pet_care_tick 的 1Hz 分频转发调用） */
void hr_monitor_tick(void);

/* 设定模拟基线（clamp 到 65-95）。返回实际生效值。 */
int  hr_monitor_set(int base_bpm);

/* 注入一次心率飙升事件（同刻已在事件中则刷新为新的 60s 窗口）。 */
void hr_monitor_inject_event(void);

/* 当前值（只读，供 hr_set 状态打印） */
int  hr_monitor_current(void);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_HR_MONITOR_H */
