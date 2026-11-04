/*
 * care_router.h - 主动关怀规则 Router（PROACTIVE-MVP P0）
 *
 * 目标（docs_ble/107_proactive_mvp_plan.md）：把散落在 pet_care 各场景里的
 * "要不要开口"判断收拢成一条可解释的规则链：
 *
 *     感知/特征快照 -> 规则评分 -> ACTION / SILENT / DEFER -> 现有 care 执行
 *
 * P0 边界（刻意不做）：
 *   - 不新增线程：care_router_tick() 由 pet_care 的 1Hz tick 驱动；
 *   - 不写 /data：所有计数/冷却都在 RAM，随 boot 清零；
 *   - 不新增静态大数组：features 一个结构体 + 3 类冷却时间戳；
 *   - 不做 TTS/L2：路由只给"能不能开口"，出声仍由 pet_care 决定
 *     （P0 默认 care L2 关、主动行为默认只气泡/历史）。
 *
 * 参数（P0 硬编码，报告里有表；后续可搬 Kconfig）：
 *   每 boot 主动开口上限 8 次；同类冷却 10min；任意两次最小间隔 60s；
 *   空闲 >5min 计入 idle；静止 >30min 记 INACTIVE；
 *   HR 高：bpm>=120 或 比基线高 20，且连续 2 次静默测量都高才 ACTION；
 *   夜间 22:00-07:00 / 免打扰：强制 SILENT（只记录）。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * P0 参数（全部可在报告里对照；改这里即可调策略）
 ****************************************************************************/
#define CARE_ROUTER_MAX_ACTIONS_PER_BOOT 8     /* 每 boot 最多主动开口次数 */
#define CARE_ROUTER_SAME_TYPE_COOLDOWN_S 600   /* 同类场景冷却 10min */
#define CARE_ROUTER_MIN_ACTION_GAP_S     60    /* 任意两次主动开口最小间隔 */
#define CARE_WALK_STEPS_60S 60  /* 60s 内 >=20 步才算走路；防单步/噪声复位 idle */
#define CARE_ROUTER_IDLE_ACTION_S        300   /* 空闲 >5min 才计入 idle */
#define CARE_ROUTER_STILL_INACTIVE_S     1800  /* 静止 >30min 记 INACTIVE */
#define CARE_ROUTER_RECENT_INTERACTION_S 60    /* 60s 内刚交互 -> SILENT */
#define CARE_ROUTER_HR_HIGH_BPM          120   /* 与 HR_WATCH_BPM_HIGH 一致 */
#define CARE_ROUTER_HR_ELEVATED_DELTA    20    /* 高于基线 20bpm 记为偏高 */
#define CARE_ROUTER_HR_CONSEC_REPORTS    2     /* 连续 2 次静默高心率才 ACTION */
#define CARE_ROUTER_HR_CONSEC_WINDOW_S   1800  /* 两次高心率报告间隔上限 30min */
#define CARE_ROUTER_HR_STALE_S           1200  /* 最近有效测量超过 20min 视为无数据 */
#define CARE_ROUTER_DEFER_S              60    /* DEFER 建议重试秒数 */
#define CARE_ROUTER_VALID_BPM_MIN        30
#define CARE_ROUTER_VALID_BPM_MAX        220

/* 规则状态（用于日志/后续策略，不是医疗结论） */
typedef enum
{
  CARE_STATE_UNKNOWN = 0,   /* 传感器都不可用 */
  CARE_STATE_ACTIVE,        /* 刚有活动（走动/交互） */
  CARE_STATE_RESTING,       /* 静止但不算久坐 */
  CARE_STATE_INACTIVE,      /* 久坐/低活动 */
  CARE_STATE_ELEVATED       /* 心率高于基线（可能连续 1 次） */
} care_state_t;

typedef enum
{
  CARE_ACTION = 0,          /* 允许开口 */
  CARE_SILENT = 1,          /* 不开口（只记录） */
  CARE_DEFER = 2            /* 现在别开口，defer_s 后再试 */
} care_decision_t;

/* 一次性特征快照（约 64B，无指针，可安全拷贝） */
typedef struct
{
  int      hr_bpm;               /* 最近一次有效测量值；无数据为 0 */
  bool     hr_ok;                /* 数据新鲜且合法（见 hr_age_s） */
  uint32_t hr_age_s;             /* 距最近一次有效测量的秒数 */
  int      hr_baseline_bpm;      /* 静息基线（EMA，只在正常值上更新） */
  int      hr_baseline_dev;      /* 当前值 - 基线 */
  uint32_t steps;                /* 累计步数 */
  uint32_t steps_delta_60s;      /* 近 60s 步数（漏桶近似） */
  uint32_t still_s;              /* IMU 静止秒数 */
  uint32_t idle_s;               /* 距最近一次用户交互秒数 */
  bool     dnd;                  /* 免打扰 */
  bool     quiet;                /* 夜间静默期 */
  uint8_t  hour;                 /* 本地小时 0-23 */
  uint32_t last_interaction_s;   /* 与 idle_s 同值（plan 字段对齐） */
  bool     in_call_or_busy;      /* 音乐/通话等占用中 */
  care_state_t state;            /* 规则状态 */
  uint8_t  confidence;           /* 特征可信度 0-100 */
} care_features_t;

/* 一次评估结果（字符串都是静态常量，可直接放日志） */
typedef struct
{
  care_decision_t decision;
  int         score;    /* 可解释评分（越高越值得开口） */
  const char *type;     /* idle / sedentary / hr_alert / none */
  const char *reason;   /* silent/defer/action 原因，日志用 */
  int         defer_s;  /* DEFER 时的建议重试秒数 */
} care_eval_t;

/****************************************************************************
 * P1b（docs_ble/110 §1/§2）：运行时可配设置（RAM only，重启回落默认）
 *
 * App 用 care_settings_get/set 同步；固件只存 RAM（/data 只读），App 本地
 * 持久化并在连接后重新下发。DND 不在本结构里（沿用 pet_care/settings 的
 * 既有 dnd，双向同步）。
 ****************************************************************************/
#define CARE_ROUTER_TYPES_HR        0x01   /* hr_alert 心率关怀 */
#define CARE_ROUTER_TYPES_IDLE      0x02   /* idle 空闲陪伴 */
#define CARE_ROUTER_TYPES_SEDENTARY 0x04   /* sedentary 久坐提醒 */
#define CARE_ROUTER_TYPES_ALL       0x07

/* P2B-1A（task-65）：主动关怀本地语音默认开（App care_settings.voice 可关）。 */
#define CARE_ROUTER_VOICE_DEFAULT   1

typedef struct
{
  uint8_t quiet_start;   /* 夜间静默开始小时 0-23（默认 22） */
  uint8_t quiet_end;     /* 夜间静默结束小时 0-23（默认 7） */
  uint8_t daily_budget;  /* 每 boot 主动开口上限 1-12（默认 8） */
  uint8_t types;         /* CARE_ROUTER_TYPES_* 位掩码（默认全开） */
  bool    voice;         /* P2B-1A：关怀本地语音 true=开(默认)/false=voice_off */
} care_router_settings_t;

/****************************************************************************
 * Public API
 *
 * 线程模型：任意线程可调；内部一把小 mutex，所有函数都很快（无阻塞 I/O、
 * 无 syslog 之外的副作用）。car_router_tick 由 pet_care 1Hz tick 调用。
 ****************************************************************************/

/* 1Hz：采样 IMU/最近心率/环境，更新 features/state，并按需打 [care_router] 日志 */
void care_router_tick(void);

/* 用户交互（触摸/按键/走路）：复位 idle；任意线程可调 */
void care_router_note_interaction(void);

/* 环境注入（pet_care 每 tick 同步） */
void care_router_set_dnd(bool on);
void care_router_set_busy(bool busy);
void care_router_set_sedentary_threshold_s(uint32_t seconds);

/* 后台静默心率测量结果（hr_watch -> pet_care_hr_report 入口调用）。
 * 只接受合法 bpm；内部维护"连续静默高心率"计数与最近样本。 */
void care_router_note_hr_report(int bpm);

/* 当前整体评估（只读，用于 tick 日志/诊断；不做冷却与预算扣减） */
care_eval_t care_router_eval(void);

/* 场景请求：type = "idle" / "sedentary" / "hr_alert"。
 * 依次检查 DND/夜间强制静默 -> busy DEFER -> 每 boot 预算 -> 同类冷却
 * -> 最小间隔 -> 场景专属门槛；返回 ACTION/SILENT/DEFER 与原因。
 * 调用方只在 ACTION 时执行现有 care 行为，并在真正执行后调
 * care_router_note_action() 记账。 */
care_eval_t care_router_request(const char *type);

/* P2B-2A（task-68，docs_ble/114）：App care_event 场景请求（type 白名单
 * eye_rest/water/sleep/weather，由 BLE 层先校验）。复用 Router 门禁：
 *   DND -> SILENT(dnd)；夜间 -> SILENT(quiet)；每 boot 预算 -> SILENT(budget)；
 *   同场景冷却 -> SILENT(cooldown)；与上次开口最小间隔 -> SILENT(min_gap)。
 * 音乐/通话(busy) 不在这里拦：场景仍出角标/历史，本地语音由 pet_care 的
 * music/recording/busy/voice_off 门禁决定（避免放音乐时连提醒角标都看不到）。
 * 返回 ACTION 时调用方真正执行后调 care_router_note_action() 记账。 */
care_eval_t care_router_request_scene(const char *type);

/* 真正开口后记账：预算 +1、记录本次时间（同类冷却/最小间隔用） */
void care_router_note_action(const char *type);

/* 拷贝当前特征快照（用于诊断/后续上报） */
void care_router_get_features(care_features_t *out);

/* 枚举 -> 静态字符串（P1a care_log/ticket/pet_care 日志复用，避免多份 switch） */
const char *care_router_state_name(care_state_t st);
const char *care_router_decision_name(care_decision_t d);

/* ── P1b 运行时设置（BLE care_settings_get/set）──────────────────────
 * 默认值：quiet 22-7（沿用 Kconfig 缺省）、daily_budget 8、types 全开。
 * 设置只存 RAM；校验失败返回 false 且不修改当前值。 */
void care_router_settings_default(care_router_settings_t *out);
void care_router_get_settings(care_router_settings_t *out);
bool care_router_set_settings(const care_router_settings_t *in);

/* P2B-1A（task-65）：关怀本地语音是否开启（care_settings.voice，RAM only，
 * 重启回落默认值 CARE_ROUTER_VOICE_DEFAULT=1）。pet_care 在本地语音播放前
 * 用它做 voice_off 门禁；只影响主动关怀本地语音，不影响用户 TTS/其它声音。 */
bool care_router_voice_enabled(void);

/* 串口诊断：打印特征 + 预算 + 冷却（NSH care_test 汇总用） */
void care_router_dump(void);

#ifdef __cplusplus
}
#endif
