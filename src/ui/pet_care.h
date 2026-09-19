/****************************************************************************
 * pet_care.h - Proactive companion scheduler (contest "proactive x3").
 *
 * 场景②（空闲关怀，本模块）：LVGL render 线程每 5ms 喂一次 tick，
 *   内部 1Hz 分频；静默期外、无操作超过 idle 分钟数 → 桌宠主动开口
 *   （L1 模板气泡保底）。
 * L2 增强：把「主动关怀」上下文作为一条 inbound 消息推给 agent loop
 *   （channel="care"），复用云端 LLM → 本地模型兜底的整条既有链路，
 *   回复从 outbound "care" 分支渲染成第二条气泡。L1 已保底，L2 失败
 *   静默。
 * 场景①（闹钟叫醒）与场景③（心率告警）复用本模块的对外接口
 *   （alarm_set / hr_report），见 ai_lm.cxx 与 health/hr_monitor.c。
 *
 * 设计约束（docs/superpowers/plans/2026-09-06 Task 1.1）：
 *   - 不新增常驻线程：tick 由既有 render 线程驱动（1Hz 分频零成本）；
 *     agent_loop / LLM 调用全部在既有线程里跑。
 *   - 触摸/按键活动是空闲判定的唯一复位源。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef AI_AGENT_PET_CARE_H
#define AI_AGENT_PET_CARE_H

#include <nuttx/config.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 常量（编译期兜底；做成 Kconfig int 项属于后续调优，不阻塞交付） */
#ifdef CONFIG_AI_AGENT_CARE_IDLE_MIN
#  define PET_CARE_IDLE_MIN_DEFAULT CONFIG_AI_AGENT_CARE_IDLE_MIN
#else
#  define PET_CARE_IDLE_MIN_DEFAULT 5
#endif
#ifdef CONFIG_AI_AGENT_CARE_COOLDOWN_MIN
#  define PET_CARE_COOLDOWN_MIN_DEFAULT CONFIG_AI_AGENT_CARE_COOLDOWN_MIN
#else
#  define PET_CARE_COOLDOWN_MIN_DEFAULT 10
#endif
#ifdef CONFIG_AI_AGENT_CARE_QUIET_START
#  define PET_CARE_QUIET_START_DEFAULT CONFIG_AI_AGENT_CARE_QUIET_START
#else
#  define PET_CARE_QUIET_START_DEFAULT 22
#endif
#ifdef CONFIG_AI_AGENT_CARE_QUIET_END
#  define PET_CARE_QUIET_END_DEFAULT CONFIG_AI_AGENT_CARE_QUIET_END
#else
#  define PET_CARE_QUIET_END_DEFAULT 7
#endif
#define PET_CARE_ALARM_ESC_SEC       120   /* 闹钟到点后无响应的升级窗口 */
/* 心率告警冷却独立于关怀冷却：演示中连续注入两次事件很常见，
 * 10 分钟窗口会把第二次演示全部吞掉。60 秒足以防连环轰炸。 */
#ifdef CONFIG_AI_AGENT_HR_ALERT_COOLDOWN_S
#  define PET_CARE_HR_ALERT_COOLDOWN_S CONFIG_AI_AGENT_HR_ALERT_COOLDOWN_S
#else
#  define PET_CARE_HR_ALERT_COOLDOWN_S 60
#endif

/* 情绪场景，决定模板库取哪一组 */
typedef enum
{
  PET_CARE_SCENE_IDLE = 0,    /* 空闲关怀（场景②） */
  PET_CARE_SCENE_ALARM_PRE,  /* 闹钟前 1 分钟预告 */
  PET_CARE_SCENE_ALARM_FIRE, /* 闹钟到点晨间简报 */
  PET_CARE_SCENE_ALARM_ESC,  /* 闹钟到点后无响应升级 */
  PET_CARE_SCENE_HR_ALERT,  /* 心率阈值告警（场景③） */
  PET_CARE_SCENE_COUNT
} pet_care_scene_t;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* daemon 启动时调用一次（幂等） */
int  pet_care_init(void);

/* LVGL render 线程 5ms 周期调用；内部 200 次 tick 分频为 1Hz 逻辑 */
void pet_care_tick(void);

/* 触摸/按键活动复位：空闲计时清零；闹钟 pending 时视为唤醒确认 */
void pet_care_note_activity(void);

/* 场景①闹钟（Task 1.2 由 ai_lm.cxx 的 set_timer 分支调用） */
int  pet_care_alarm_set(int minutes);
void pet_care_alarm_cancel(void);

/* 场景③心率（Task 1.3 由 health/hr_monitor.c 超阈值时调用） */
void pet_care_hr_report(int bpm);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_PET_CARE_H */
