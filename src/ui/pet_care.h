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

/* 场景④ 久坐提醒（真数据驱动：pedometer_get_still_seconds() 的 IMU
 * 静止时长，不是"多久没碰表"）。默认 45 分钟，0 = 关闭。
 * 演示时不用等 45 分钟：NSH `care_test trigger 120` 把阈值临时改成
 * 2 分钟，就能完整看到"静止跨过阈值 -> 主动开口"的真实路径。 */
#ifdef CONFIG_AI_AGENT_CARE_SEDENTARY_MIN
#  define PET_CARE_SEDENTARY_MIN_DEFAULT CONFIG_AI_AGENT_CARE_SEDENTARY_MIN
#else
#  define PET_CARE_SEDENTARY_MIN_DEFAULT 45
#endif
#define PET_CARE_SEDENTARY_COOLDOWN_MIN 60  /* 两次久坐提醒的最小间隔 */
#define PET_CARE_SEDENTARY_RESET_SEC    60  /* 静止时长跌破这个值 = 动过了 */

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

/* LVGL render 线程 5ms 周期调用；内部按墙钟秒分频为 1Hz 逻辑 */
void pet_care_tick(void);

/**
 * 免打扰开关。
 *
 * 开启后**只压制主动关怀**（空闲时的关心/提醒）：
 *   - 不产生关怀文本
 *   - 也不会触发它背后的 L2 云端调用（否则断网时会白跑一次 LLM）
 *
 * **用户自己设的闹钟照常响** —— 这是"勿扰"和"关机"的区别，
 * 和手机上的行为一致。
 */
void pet_care_set_dnd(bool on);
bool pet_care_get_dnd(void);

/* 触摸/按键活动复位：空闲计时清零；闹钟 pending 时视为唤醒确认 */
void pet_care_note_activity(void);

/* 场景①闹钟（Task 1.2 由 ai_lm.cxx 的 set_timer 分支调用） */
int  pet_care_alarm_set(int minutes);
void pet_care_alarm_cancel(void);

/* 场景③心率（Task 1.3 由 health/hr_monitor.c 超阈值时调用） */
void pet_care_hr_report(int bpm);

/* 场景④久坐提醒：运行时设置静止阈值（秒）。0 = 关闭该提醒。
 * 只影响"多久算久坐"，冷却/静默期/免打扰判据不变。 */
void     pet_care_set_sedentary_trigger(uint32_t trigger_s);
uint32_t pet_care_get_sedentary_trigger(void);

/* 验收/诊断接口（NSH `care_test` 用；正常运行不需要调）：
 *   dump             打印静止时长/步数/阈值/免打扰/静默期
 *   force_sedentary  立刻弹一次久坐提醒（不看阈值/冷却，仍受免打扰约束；
 *                    不起 L2，便于现场验收）
 *   force_milestone  让下一次 1Hz tick 触发一次步数里程碑庆祝 */
void pet_care_debug_dump(void);
int  pet_care_debug_force_sedentary(void);
int  pet_care_debug_force_milestone(void);

/* ── DEMO-FW（2026-11-03）：演示触发钩子 ──────────────────────────
 * 只给 NSH 命令 `ui demo ...` 与 BLE 命令 {"cmd":"demo"} 用；正式运行
 * 路径不会调用。与 decide 路径不同：**不看冷却 / 免打扰**，演示时按一下
 * 立刻出现；连续触发也生效。实现复用 care_fire()/care_start_l2()/cron。 */
void pet_care_demo_greet(void);          /* 按时段的主动打招呼（L1 气泡+L2 接话） */
void pet_care_demo_hr_alert(int bpm);

/* 2026-11-03 紧急修复：BLE RX 回调线程直接跑 care/cache 会整机 freeze；
 * 手机 demo 命令改为投递到 ai_agent 上下文的 worker 执行。 */
enum {
    PET_CARE_DEMO_HR = 0,
    PET_CARE_DEMO_GREET,
    PET_CARE_DEMO_WATER,
    PET_CARE_DEMO_SLEEP,
    PET_CARE_DEMO_WEATHER,
    PET_CARE_DEMO_SEDENTARY,
    PET_CARE_DEMO_MILESTONE,
    PET_CARE_DEMO_ALARM,
};
void pet_care_demo_async(int action, int seconds);    /* 强制一次心率告警气泡（bpm<=0 用 155） */
int  pet_care_demo_alarm_after(int seconds); /* N 秒后一次性 cron voice 闹钟 */

/* ── DEMO-UI（2026-11-03）：设置页「演示与测试」用的固定文案关怀 ──────
 * 走 care_fire（气泡+历史）+ 本地预合成音频（<1s）；缓存未命中才回落 L2。 */
void pet_care_demo_water_reminder(void);   /* 喝水提醒 */
void pet_care_demo_sleep_reminder(void);   /* 该睡觉了 */
void pet_care_demo_weather_change(void);   /* 天气变化提醒（只提醒看天气，不谎报数据） */
void pet_care_demo_sedentary(void);        /* 久坐提醒演示（固定文案，不走 decide 冷却） */
void pet_care_demo_milestone(void);        /* 步数里程碑演示（真实步数气泡，无预合成音频） */

/* N 秒闹钟的异步版：cron 落盘写 /data，禁止在 LVGL 渲染线程里同步做。
 * 内部起一次性 detached 线程（8KB 栈）调 pet_care_demo_alarm_after()。 */
void pet_care_demo_alarm_after_async(int seconds);


#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_PET_CARE_H */
