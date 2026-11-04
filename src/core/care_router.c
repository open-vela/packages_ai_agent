/*
 * care_router.c - 主动关怀规则 Router 实现（PROACTIVE-MVP P0）
 *
 * 设计见 care_router.h。这里只做三件事：
 *   1) 1Hz 采样 IMU/最近一次心率测量/环境，维护 features 快照；
 *   2) 评估规则（DND/夜间/预算/冷却/最小间隔/场景门槛）给出
 *      ACTION / SILENT / DEFER，并打 [care_router] 可解释日志；
 *   3) 提供记账接口，让 pet_care 在真正开口后更新预算与冷却。
 *
 * 诚实性约束：
 *   - 心率只用 hr_watch 后台静默测量的"最近一次有效值"，不宣称实时；
 *   - 单次高心率不告警（可能是运动/噪声）：连续 2 次静默测量都高才 ACTION；
 *   - 夜间/免打扰只记录，绝不弹气泡/出声。
 *
 * 没有新增线程、没有静态大数组、不写 /data。
 */

#include "core/care_router.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include "ui/pet_care.h"        /* 静默期/久坐阈值缺省值，与 pet_care 保持一致 */
#include "health/hr_measure.h"  /* hr_measure_last_bpm / _last_ok：最近一次真实测量 */
#include "health/pedometer.h"   /* 步数 / 静止时长 / 传感器存活 */
#include "core/care_ticket.h"   /* P1a：DEFER/ACTION ticket 的时间轮（1Hz sweep） */
#include "core/care_log.h"      /* P1a：隐式反馈 60s 超时判定（1Hz tick） */

#define TAG "care_router"

/* 单类型冷却记录（idle / sedentary / hr_alert 三类） */
typedef struct
{
  const char *type;
  time_t      last_s;
} care_type_cooldown_t;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static bool            s_inited;

static care_features_t s_feat;
static time_t          s_last_tick_s;
static time_t          s_last_interaction_s;  /* 最近一次交互（MONOTONIC 秒） */
static time_t          s_last_action_s;       /* 最近一次真正开口 */
static int             s_actions_used;        /* 本 boot 已开口次数 */

static uint32_t        s_prev_steps;
static uint32_t        s_steps_delta60;       /* 近 60s 步数（漏桶近似） */

static int             s_hr_baseline;         /* 静息基线 EMA */
static int             s_prev_hr_sample;      /* 上一次看到的测量值（检测新样本） */
static uint32_t        s_hr_age_s;            /* 距最近一次有效测量 */
static int             s_hr_consec;           /* 连续静默高心率次数 */
static time_t          s_last_hr_report_s;    /* 上一次高心率报告时刻 */

static uint32_t        s_sed_trigger_s;       /* 久坐阈值（与 pet_care 同步） */
static bool            s_dnd;
static bool            s_busy;

/* P1b（docs_ble/110）：App 可配设置（RAM only）。 */
static care_router_settings_t s_settings;

/* 周期日志节流（状态变化或 60s 一次） */
static int             s_logged_state = -1;
static time_t          s_last_log_s;

static care_type_cooldown_t s_type_last[] = {
  { "idle", 0 },
  { "sedentary", 0 },
  { "hr_alert", 0 },
  /* P2B-2A（task-68）：App care_event 场景也用同类冷却（CARE_ROUTER_SAME_TYPE_COOLDOWN_S） */
  { "eye_rest", 0 },
  { "water", 0 },
  { "sleep", 0 },
  { "weather", 0 },
};
#define CARE_TYPE_N ((int)(sizeof(s_type_last) / sizeof(s_type_last[0])))

/* ── 小工具 ─────────────────────────────────────────────────────── */

static time_t router_now_s(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec;
}

/* 与 pet_care 同一约定：板上 RTC 直接存本地时间，gmtime 即本地。 */
static int router_local_hour(void)
{
  time_t now = time(NULL);
  struct tm tm_buf;

  if (gmtime_r(&now, &tm_buf) != NULL)
    {
      return tm_buf.tm_hour;
    }

  return 12;   /* 拿不到时间就按白天处理，不误判夜间 */
}

static const char *state_name(care_state_t st)
{
  switch (st)
    {
      case CARE_STATE_ACTIVE:   return "ACTIVE";
      case CARE_STATE_RESTING:  return "RESTING";
      case CARE_STATE_INACTIVE: return "INACTIVE";
      case CARE_STATE_ELEVATED: return "ELEVATED";
      default:                  return "UNKNOWN";
    }
}

static int type_index(const char *type)
{
  int i;

  if (type == NULL)
    {
      return -1;
    }

  for (i = 0; i < CARE_TYPE_N; i++)
    {
      if (strcmp(s_type_last[i].type, type) == 0)
        {
          return i;
        }
    }

  return -1;
}

/* 场景名 -> P1b types 位。调用方持锁。 */
static uint8_t type_mask_locked(const char *type)
{
  if (type == NULL)
    {
      return 0;
    }
  if (strcmp(type, "hr_alert") == 0)
    {
      return CARE_ROUTER_TYPES_HR;
    }
  if (strcmp(type, "idle") == 0)
    {
      return CARE_ROUTER_TYPES_IDLE;
    }
  if (strcmp(type, "sedentary") == 0)
    {
      return CARE_ROUTER_TYPES_SEDENTARY;
    }
  return 0;   /* 未知场景不受 types 开关影响（由场景层自行判断） */
}

/* 调用方持锁：该场景是否被 App 打开。 */
static bool type_enabled_locked(const char *type)
{
  uint8_t mask = type_mask_locked(type);

  return (mask == 0) || ((s_settings.types & mask) != 0);
}

/* 调用方必须持锁 */
static void care_router_init_locked(void)
{
  if (s_inited)
    {
      return;
    }

  s_inited = true;
  s_last_interaction_s = router_now_s();   /* 开机视为一次交互 */
  s_last_tick_s = 0;
  s_last_action_s = 0;
  s_actions_used = 0;
  s_prev_steps = pedometer_get_steps();
  s_steps_delta60 = 0;
  s_hr_baseline = 75;                      /* 成人静息中位起点；由 EMA 收敛 */
  s_prev_hr_sample = 0;
  s_hr_age_s = CARE_ROUTER_HR_STALE_S;
  s_hr_consec = 0;
  s_last_hr_report_s = 0;
  s_sed_trigger_s = (uint32_t)PET_CARE_SEDENTARY_MIN_DEFAULT * 60u;
  s_dnd = false;
  s_busy = false;

  /* P1b 默认设置：夜间 22-7、预算 8、三类全开（App 未下发时沿用）。 */
  s_settings.quiet_start = PET_CARE_QUIET_START_DEFAULT;
  s_settings.quiet_end = PET_CARE_QUIET_END_DEFAULT;
  s_settings.daily_budget = CARE_ROUTER_MAX_ACTIONS_PER_BOOT;
  s_settings.types = CARE_ROUTER_TYPES_ALL;
  s_settings.voice = (CARE_ROUTER_VOICE_DEFAULT != 0);   /* P2B-1A */

  memset(&s_feat, 0, sizeof(s_feat));
  s_feat.state = CARE_STATE_UNKNOWN;
  s_feat.hr_baseline_bpm = s_hr_baseline;

  syslog(LOG_INFO,
         "[%s] init P0/P1b rules: quiet=%u-%u budget=%u types=0x%x "
         "voice=%d cooldown=%ds min_gap=%ds idle=%ds still=%ds hr_high=%d "
         "hr_x=%d window=%ds\n",
         TAG, (unsigned)s_settings.quiet_start,
         (unsigned)s_settings.quiet_end, (unsigned)s_settings.daily_budget,
         (unsigned)s_settings.types, (int)s_settings.voice,
         CARE_ROUTER_SAME_TYPE_COOLDOWN_S,
         CARE_ROUTER_MIN_ACTION_GAP_S, CARE_ROUTER_IDLE_ACTION_S,
         CARE_ROUTER_STILL_INACTIVE_S, CARE_ROUTER_HR_HIGH_BPM,
         CARE_ROUTER_HR_CONSEC_REPORTS, CARE_ROUTER_HR_CONSEC_WINDOW_S);
}

/* ── 评估（调用方必须持锁）─────────────────────────────────────────
 * 只做"当前状态最适合做什么"的建议，不扣预算/不写冷却（request 才做）。 */
static care_eval_t care_router_eval_locked(void)
{
  care_eval_t ev;

  memset(&ev, 0, sizeof(ev));
  ev.decision = CARE_ACTION;
  ev.score = 0;
  ev.type = "none";
  ev.reason = "not_needed";

  if (s_dnd)
    {
      ev.decision = CARE_SILENT;
      ev.reason = "dnd";
      return ev;
    }
  if (s_feat.quiet)
    {
      ev.decision = CARE_SILENT;
      ev.reason = "quiet";
      return ev;
    }
  if (s_busy)
    {
      ev.decision = CARE_DEFER;
      ev.reason = "busy";
      ev.defer_s = CARE_ROUTER_DEFER_S;
      return ev;
    }

  if (s_feat.state == CARE_STATE_ELEVATED && s_feat.hr_ok)
    {
      ev.score += 3;
      ev.type = "hr_alert";
      ev.reason = "hr_elevated";
    }
  else if (s_feat.state == CARE_STATE_INACTIVE
           && s_feat.steps_delta_60s < CARE_WALK_STEPS_60S)
    {
      ev.score += 2;
      ev.type = "sedentary";
      ev.reason = "inactive";
    }
  else if (s_feat.idle_s >= CARE_ROUTER_IDLE_ACTION_S)
    {
      ev.score += 1;
      ev.type = "idle";
      ev.reason = "idle_5min";
    }
  else
    {
      ev.decision = CARE_SILENT;
      ev.reason = "not_needed";
      return ev;
    }

  /* P1b：App 关闭的场景不参与（tick 日志也显示 type_off） */
  if (!type_enabled_locked(ev.type))
    {
      ev.decision = CARE_SILENT;
      ev.reason = "type_off";
      return ev;
    }

  /* 通用门禁（仅用于 tick 日志里的"建议"；request 会重新逐项判定） */
  if (s_actions_used >= s_settings.daily_budget)
    {
      ev.decision = CARE_SILENT;
      ev.reason = "budget";
    }
  else if (s_last_action_s != 0
           && router_now_s() - s_last_action_s
                  < CARE_ROUTER_MIN_ACTION_GAP_S)
    {
      ev.decision = CARE_SILENT;
      ev.reason = "min_gap";
    }

  return ev;
}

/* ── 1Hz tick ──────────────────────────────────────────────────── */

void care_router_tick(void)
{
  time_t now;

  pthread_mutex_lock(&s_lock);
  care_router_init_locked();

  now = router_now_s();
  if (now == s_last_tick_s)
    {
      pthread_mutex_unlock(&s_lock);
      return;
    }
  s_last_tick_s = now;

  /* ── IMU：步数/静止 ── */
  {
    uint32_t steps = pedometer_get_steps();
    uint32_t still = pedometer_get_still_seconds();
    uint32_t delta = (steps >= s_prev_steps) ? steps - s_prev_steps : 0;

    s_prev_steps = steps;
    /* 漏桶近似"近 60s 步数"：每秒衰减 1/60，避免再存 60 个样本的数组 */
    s_steps_delta60 = s_steps_delta60 - s_steps_delta60 / 60u + delta;

    s_feat.steps = steps;
    s_feat.steps_delta_60s = s_steps_delta60;
    s_feat.still_s = still;
  }

  /* ── 心率：只用"最近一次有效测量"（后台 10min 一轮，不宣称实时）── */
  {
    int bpm = hr_measure_last_bpm();
    bool ok = hr_measure_last_ok()
              && bpm >= CARE_ROUTER_VALID_BPM_MIN
              && bpm <= CARE_ROUTER_VALID_BPM_MAX;

    if (ok)
      {
        if (bpm != s_prev_hr_sample)
          {
            /* 新样本：刷新 age；只有"正常值"参与基线收敛，
             * 高心率样本不能把基线抬高（否则永远测不出偏高）。 */
            s_prev_hr_sample = bpm;
            s_hr_age_s = 0;
            if (bpm < CARE_ROUTER_HR_HIGH_BPM - 10)
              {
                s_hr_baseline += (bpm - s_hr_baseline) / 16;
                if (s_hr_baseline < 45)
                  {
                    s_hr_baseline = 45;
                  }
                if (s_hr_baseline > 120)
                  {
                    s_hr_baseline = 120;
                  }
                s_hr_consec = 0;   /* 回到正常：清空连续高心率计数 */
              }
          }
        else if (s_hr_age_s < 0x7fffffffU)
          {
            s_hr_age_s++;
          }
      }
    else if (s_hr_age_s < 0x7fffffffU)
      {
        s_hr_age_s++;
      }

    s_feat.hr_bpm = ok ? bpm : 0;
    s_feat.hr_age_s = s_hr_age_s;
    s_feat.hr_ok = ok && s_hr_age_s <= CARE_ROUTER_HR_STALE_S;
    s_feat.hr_baseline_bpm = s_hr_baseline;
    s_feat.hr_baseline_dev = ok ? (bpm - s_hr_baseline) : 0;
  }

  /* ── 环境 ── */
  s_feat.hour = (uint8_t)router_local_hour();
  /* P1b：夜间时段运行时可变；start==end 视为关闭夜间静默。 */
  if (s_settings.quiet_start == s_settings.quiet_end) {
      s_feat.quiet = false;   /* start==end: 关闭夜间静默 */
  } else if (s_settings.quiet_start < s_settings.quiet_end) {
      /* 同日窗口，例如 07-22 */
      s_feat.quiet = (s_feat.hour >= s_settings.quiet_start
                      && s_feat.hour < s_settings.quiet_end);
  } else {
      /* 跨午夜窗口，例如 22-07 */
      s_feat.quiet = (s_feat.hour >= s_settings.quiet_start
                      || s_feat.hour < s_settings.quiet_end);
  }
  s_feat.dnd = s_dnd;
  s_feat.in_call_or_busy = s_busy;
  s_feat.idle_s = (uint32_t)(now - s_last_interaction_s);
  s_feat.last_interaction_s = s_feat.idle_s;

  /* ── P1a：ticket 时间轮 + 隐式反馈超时（复用本 1Hz tick，无新线程）── */
  care_ticket_sweep((uint32_t)now);
  care_log_tick((uint32_t)now);

  /* ── 状态 + 置信度 ── */
  {
    bool imu_ok = pedometer_is_alive();
    uint8_t conf = 100;

    if (s_hr_consec >= 1 && s_feat.hr_ok)
      {
        s_feat.state = CARE_STATE_ELEVATED;
      }
    else if (s_feat.steps_delta_60s >= CARE_WALK_STEPS_60S || s_feat.idle_s < 120)
      {
        s_feat.state = CARE_STATE_ACTIVE;
      }
    else if (s_feat.still_s >= CARE_ROUTER_STILL_INACTIVE_S
             && s_feat.idle_s >= CARE_ROUTER_IDLE_ACTION_S)
      {
        s_feat.state = CARE_STATE_INACTIVE;
      }
    else if (imu_ok)
      {
        s_feat.state = CARE_STATE_RESTING;
      }
    else
      {
        s_feat.state = CARE_STATE_UNKNOWN;
      }

    if (!s_feat.hr_ok)
      {
        conf = (conf >= 40) ? (uint8_t)(conf - 40) : 0;
      }
    if (!imu_ok)
      {
        conf = (conf >= 40) ? (uint8_t)(conf - 40) : 0;
      }
    s_feat.confidence = conf;
  }

  /* ── 周期日志：状态变化或每 60s 一次 ── */
  {
    care_eval_t ev = care_router_eval_locked();

    if ((int)s_feat.state != s_logged_state
        || now - s_last_log_s >= 60)
      {
        s_logged_state = (int)s_feat.state;
        s_last_log_s = now;
        syslog(LOG_INFO,
               "[%s] state=%s score=%d decision=%s reason=%s conf=%u "
               "hr=%d dev=%d age=%us steps=%u d60=%u still=%u idle=%u "
               "dnd=%d quiet=%d hour=%u budget=%d/%d\n",
               TAG, state_name(s_feat.state), ev.score,
               ev.decision == CARE_ACTION ? "ACTION"
                 : (ev.decision == CARE_DEFER ? "DEFER" : "SILENT"),
               ev.reason, (unsigned)s_feat.confidence, s_feat.hr_bpm,
               s_feat.hr_baseline_dev, (unsigned)s_feat.hr_age_s,
               (unsigned)s_feat.steps, (unsigned)s_feat.steps_delta_60s,
               (unsigned)s_feat.still_s, (unsigned)s_feat.idle_s,
               (int)s_feat.dnd, (int)s_feat.quiet, (unsigned)s_feat.hour,
               s_actions_used, (unsigned)s_settings.daily_budget);
      }
  }

  pthread_mutex_unlock(&s_lock);
}

/* ── 环境/交互注入 ─────────────────────────────────────────────── */

void care_router_note_interaction(void)
{
  pthread_mutex_lock(&s_lock);
  care_router_init_locked();
  s_last_interaction_s = router_now_s();
  pthread_mutex_unlock(&s_lock);
}

void care_router_set_dnd(bool on)
{
  pthread_mutex_lock(&s_lock);
  care_router_init_locked();
  s_dnd = on;
  pthread_mutex_unlock(&s_lock);
}

void care_router_set_busy(bool busy)
{
  pthread_mutex_lock(&s_lock);
  care_router_init_locked();
  s_busy = busy;
  pthread_mutex_unlock(&s_lock);
}

void care_router_set_sedentary_threshold_s(uint32_t seconds)
{
  pthread_mutex_lock(&s_lock);
  care_router_init_locked();
  s_sed_trigger_s = seconds;
  pthread_mutex_unlock(&s_lock);
}

/* ── 高心率报告（来自 hr_watch 的真实静默测量）────────────────────
 * 连续计数规则：只有"合法高心率"报告才累加；两次报告间隔超过
 * CARE_ROUTER_HR_CONSEC_WINDOW_S 则重新从 1 开始（中间大概率测过正常值）。
 * 第一次报告 -> SILENT("hr_once")，第二次起 -> 可 ACTION。 */
void care_router_note_hr_report(int bpm)
{
  time_t now;

  if (bpm < CARE_ROUTER_VALID_BPM_MIN
      || bpm > CARE_ROUTER_VALID_BPM_MAX)
    {
      return;
    }

  pthread_mutex_lock(&s_lock);
  care_router_init_locked();
  now = router_now_s();

  /* 报告来自刚结束的真实测量：直接刷新 HR 特征（tick 可能还没跑到） */
  s_feat.hr_bpm = bpm;
  s_feat.hr_ok = true;
  s_feat.hr_age_s = 0;
  s_prev_hr_sample = bpm;
  s_hr_age_s = 0;
  s_feat.hr_baseline_bpm = s_hr_baseline;
  s_feat.hr_baseline_dev = bpm - s_hr_baseline;

  if (s_hr_consec == 0 || s_last_hr_report_s == 0
      || now - s_last_hr_report_s > CARE_ROUTER_HR_CONSEC_WINDOW_S)
    {
      s_hr_consec = 1;
    }
  else if (s_hr_consec < 5)
    {
      s_hr_consec++;   /* 上限 5，防长期高值把计数撑爆 */
    }

  s_last_hr_report_s = now;

  syslog(LOG_INFO,
         "[%s] hr_report bpm=%d base=%d dev=%d consec=%d/%d\n",
         TAG, bpm, s_hr_baseline, s_feat.hr_baseline_dev, s_hr_consec,
         CARE_ROUTER_HR_CONSEC_REPORTS);

  pthread_mutex_unlock(&s_lock);
}

/* ── 场景请求 ─────────────────────────────────────────────────── */

/* 心率是否"偏高"：绝对阈值或相对基线（定义在 request 之后，先声明） */
static bool bpm_high_locked(void);

care_eval_t care_router_request(const char *type)
{
  care_eval_t ev;
  time_t now;
  int idx;

  memset(&ev, 0, sizeof(ev));
  ev.decision = CARE_SILENT;
  ev.type = (type != NULL) ? type : "unknown";
  ev.reason = "unknown_type";

  pthread_mutex_lock(&s_lock);
  care_router_init_locked();
  now = router_now_s();

  /* tick 是 1Hz：request 前再读一次最便宜的实时量，避免最多 1s 的滞后
   * 让"刚好在走路"被误判成久坐。 */
  s_feat.still_s = pedometer_get_still_seconds();
  s_feat.idle_s = (uint32_t)(now - s_last_interaction_s);
  s_feat.last_interaction_s = s_feat.idle_s;

  idx = type_index(type);

  /* 1) 环境强制：DND / 夜间 / 占用中 */
  if (s_dnd)
    {
      ev.reason = "dnd";
      goto log_decision;
    }
  if (s_feat.quiet)
    {
      ev.reason = "quiet";
      goto log_decision;
    }
  if (s_busy)
    {
      ev.decision = CARE_DEFER;
      ev.reason = "busy";
      ev.defer_s = CARE_ROUTER_DEFER_S;
      goto log_decision;
    }

  /* 1.5) P1b：App 关闭的场景直接 SILENT（不建 ticket，只记录理由） */
  if (!type_enabled_locked(type))
    {
      ev.reason = "type_off";
      goto log_decision;
    }

  /* 2) 每 boot 预算 / 同类冷却 / 最小间隔 */
  if (s_actions_used >= s_settings.daily_budget)
    {
      ev.reason = "budget";
      goto log_decision;
    }
  if (idx >= 0 && s_type_last[idx].last_s != 0
      && now - s_type_last[idx].last_s < CARE_ROUTER_SAME_TYPE_COOLDOWN_S)
    {
      ev.reason = "cooldown";
      goto log_decision;
    }
  if (s_last_action_s != 0
      && now - s_last_action_s < CARE_ROUTER_MIN_ACTION_GAP_S)
    {
      ev.reason = "min_gap";
      goto log_decision;
    }

  /* 3) 场景专属门槛 */
  if (type == NULL)
    {
      ev.reason = "null_type";
    }
  else if (strcmp(type, "hr_alert") == 0)
    {
      if (!s_feat.hr_ok)
        {
          ev.reason = "hr_no_data";
        }
      else if (bpm_high_locked() == false)
        {
          ev.reason = "hr_not_elevated";
        }
      else if (s_hr_consec < CARE_ROUTER_HR_CONSEC_REPORTS)
        {
          ev.reason = "hr_once";   /* 第一次：只记录，不吓用户 */
          ev.score = 1;
        }
      else
        {
          ev.decision = CARE_ACTION;
          ev.reason = "hr_elevated_x2";
          ev.score = 3;
        }
    }
  else if (strcmp(type, "idle") == 0)
    {
      if (s_feat.idle_s < CARE_ROUTER_IDLE_ACTION_S)
        {
          ev.reason = "not_idle";
        }
      else if (s_feat.steps_delta_60s >= CARE_WALK_STEPS_60S)
        {
          ev.reason = "walking";    /* 走路 = 活动，不算空闲 */
        }
      else
        {
          ev.decision = CARE_ACTION;
          ev.reason = "idle_5min";
          ev.score = 1;
        }
    }
  else if (strcmp(type, "sedentary") == 0)
    {
      if (s_sed_trigger_s == 0)
        {
          ev.reason = "disabled";
        }
      else if (s_feat.still_s < s_sed_trigger_s)
        {
          ev.reason = "not_still";
        }
      else if (s_feat.steps_delta_60s >= CARE_WALK_STEPS_60S)
        {
          ev.reason = "walking";    /* 走路不报久坐 */
        }
      else
        {
          ev.decision = CARE_ACTION;
          ev.reason = "still_30min";
          ev.score = 2;
        }
    }
  else
    {
      ev.reason = "unknown_type";
    }

log_decision:
  syslog(LOG_INFO,
         "[%s] req type=%s decision=%s score=%d reason=%s "
         "hr=%d ok=%d dev=%d age=%us still=%u idle=%u d60=%u "
         "dnd=%d quiet=%d hour=%u budget=%d/%d\n",
         TAG, ev.type,
         ev.decision == CARE_ACTION ? "ACTION"
           : (ev.decision == CARE_DEFER ? "DEFER" : "SILENT"),
         ev.score, ev.reason, s_feat.hr_bpm, (int)s_feat.hr_ok,
         s_feat.hr_baseline_dev, (unsigned)s_feat.hr_age_s,
         (unsigned)s_feat.still_s, (unsigned)s_feat.idle_s,
         (unsigned)s_feat.steps_delta_60s, (int)s_feat.dnd,
         (int)s_feat.quiet, (unsigned)s_feat.hour, s_actions_used,
         (unsigned)s_settings.daily_budget);

  pthread_mutex_unlock(&s_lock);
  return ev;
}

/* P2B-2A（task-68）：App care_event 场景门禁（docs_ble/114）。
 * 复用 Router 环境/预算/冷却/最小间隔；**不检查 busy**（角标/历史仍应可
 * 达，语音由 pet_care 音频门禁处理）。不在这里扣预算：调用方 ACTION 后调
 * care_router_note_action()，与既有 care 场景同一套记账。 */
care_eval_t care_router_request_scene(const char *type)
{
  care_eval_t ev;
  time_t now;
  int idx;

  memset(&ev, 0, sizeof(ev));
  ev.decision = CARE_SILENT;
  ev.type = (type != NULL) ? type : "unknown";
  ev.reason = "unknown_type";

  pthread_mutex_lock(&s_lock);
  care_router_init_locked();
  now = router_now_s();

  /* 与 request 一样先刷新最便宜的实时量，避免 1s tick 滞后 */
  s_feat.still_s = pedometer_get_still_seconds();
  s_feat.idle_s = (uint32_t)(now - s_last_interaction_s);
  s_feat.last_interaction_s = s_feat.idle_s;

  idx = type_index(type);

  if (s_dnd)
    {
      ev.reason = "dnd";
      goto scene_log;
    }
  if (s_feat.quiet)
    {
      ev.reason = "quiet";
      goto scene_log;
    }
  if (s_actions_used >= s_settings.daily_budget)
    {
      ev.reason = "budget";
      goto scene_log;
    }
  if (idx >= 0 && s_type_last[idx].last_s != 0
      && now - s_type_last[idx].last_s < CARE_ROUTER_SAME_TYPE_COOLDOWN_S)
    {
      ev.reason = "cooldown";
      goto scene_log;
    }
  if (s_last_action_s != 0
      && now - s_last_action_s < CARE_ROUTER_MIN_ACTION_GAP_S)
    {
      ev.reason = "min_gap";
      goto scene_log;
    }

  if (type == NULL)
    {
      ev.reason = "null_type";
    }
  else if (strcmp(type, "eye_rest") != 0 && strcmp(type, "water") != 0
           && strcmp(type, "sleep") != 0 && strcmp(type, "weather") != 0)
    {
      ev.reason = "unknown_type";
    }
  else
    {
      ev.decision = CARE_ACTION;
      ev.reason = "app_event";
      ev.score = 1;
    }

scene_log:
  syslog(LOG_INFO,
         "[%s] scene type=%s decision=%s reason=%s dnd=%d quiet=%d "
         "budget=%d/%u\n",
         TAG, ev.type,
         ev.decision == CARE_ACTION ? "ACTION" : "SILENT",
         ev.reason, (int)s_dnd, (int)s_feat.quiet, s_actions_used,
         (unsigned)s_settings.daily_budget);

  pthread_mutex_unlock(&s_lock);
  return ev;
}

/* 是否"心率偏高"：绝对阈值或相对基线。调用方持锁。 */
static bool bpm_high_locked(void)
{
  if (s_feat.hr_bpm >= CARE_ROUTER_HR_HIGH_BPM)
    {
      return true;
    }

  return s_feat.hr_baseline_dev >= CARE_ROUTER_HR_ELEVATED_DELTA;
}

void care_router_note_action(const char *type)
{
  time_t now;
  int idx;

  pthread_mutex_lock(&s_lock);
  care_router_init_locked();
  now = router_now_s();

  if (s_actions_used < s_settings.daily_budget)
    {
      s_actions_used++;
    }
  s_last_action_s = now;

  idx = type_index(type);
  if (idx >= 0)
    {
      s_type_last[idx].last_s = now;
    }

  syslog(LOG_INFO,
         "[%s] action type=%s budget=%d/%u\n",
         TAG, (type != NULL) ? type : "?", s_actions_used,
         (unsigned)s_settings.daily_budget);

  pthread_mutex_unlock(&s_lock);
}

/* ── 只读接口 ─────────────────────────────────────────────────── */

care_eval_t care_router_eval(void)
{
  care_eval_t ev;

  pthread_mutex_lock(&s_lock);
  care_router_init_locked();
  ev = care_router_eval_locked();
  pthread_mutex_unlock(&s_lock);

  return ev;
}

void care_router_get_features(care_features_t *out)
{
  if (out == NULL)
    {
      return;
    }

  pthread_mutex_lock(&s_lock);
  care_router_init_locked();
  *out = s_feat;
  pthread_mutex_unlock(&s_lock);
}

const char *care_router_state_name(care_state_t st)
{
  return state_name(st);
}

const char *care_router_decision_name(care_decision_t d)
{
  switch (d)
    {
      case CARE_ACTION: return "ACTION";
      case CARE_DEFER:  return "DEFER";
      default:          return "SILENT";
    }
}

/* ── P1b 运行时设置 ─────────────────────────────────────────────── */

void care_router_settings_default(care_router_settings_t *out)
{
  if (out == NULL)
    {
      return;
    }

  out->quiet_start = PET_CARE_QUIET_START_DEFAULT;
  out->quiet_end = PET_CARE_QUIET_END_DEFAULT;
  out->daily_budget = CARE_ROUTER_MAX_ACTIONS_PER_BOOT;
  out->types = CARE_ROUTER_TYPES_ALL;
  out->voice = (CARE_ROUTER_VOICE_DEFAULT != 0);   /* P2B-1A：默认开 */
}

void care_router_get_settings(care_router_settings_t *out)
{
  if (out == NULL)
    {
      return;
    }

  pthread_mutex_lock(&s_lock);
  care_router_init_locked();
  *out = s_settings;
  pthread_mutex_unlock(&s_lock);
}

bool care_router_set_settings(const care_router_settings_t *in)
{
  if (in == NULL)
    {
      return false;
    }

  /* 越界校验：quiet 0-23、budget 1-12、types 只允许已知位 */
  if (in->quiet_start > 23 || in->quiet_end > 23)
    {
      return false;
    }
  if (in->daily_budget < 1 || in->daily_budget > 12)
    {
      return false;
    }
  if ((in->types & (uint8_t)~CARE_ROUTER_TYPES_ALL) != 0)
    {
      return false;
    }

  pthread_mutex_lock(&s_lock);
  care_router_init_locked();
  s_settings = *in;
  pthread_mutex_unlock(&s_lock);

  syslog(LOG_INFO,
         "[%s] settings: quiet=%u-%u budget=%u types=0x%x voice=%d\n",
         TAG, (unsigned)in->quiet_start, (unsigned)in->quiet_end,
         (unsigned)in->daily_budget, (unsigned)in->types,
         (int)in->voice);

  return true;
}

/* P2B-1A（task-65）：voice 开关读取（RAM only；App care_settings_set 原子
 * 更新，pet_care 本地语音播放前门禁用）。 */
bool care_router_voice_enabled(void)
{
  bool on;

  pthread_mutex_lock(&s_lock);
  care_router_init_locked();
  on = s_settings.voice;
  pthread_mutex_unlock(&s_lock);

  return on;
}

void care_router_dump(void)
{
  care_features_t f;
  care_router_settings_t st;
  time_t now;
  int used;
  long last_action_age = -1;
  long hr_report_age = -1;

  pthread_mutex_lock(&s_lock);
  care_router_init_locked();
  f = s_feat;
  st = s_settings;
  used = s_actions_used;
  now = router_now_s();
  if (s_last_action_s != 0)
    {
      last_action_age = (long)(now - s_last_action_s);
    }
  if (s_last_hr_report_s != 0)
    {
      hr_report_age = (long)(now - s_last_hr_report_s);
    }
  pthread_mutex_unlock(&s_lock);

  printf("[care_router] dump state=%s conf=%u hr=%d ok=%d age=%us "
         "base=%d dev=%d steps=%u d60=%u still=%u idle=%u dnd=%d quiet=%d "
         "hour=%u busyl=%d consec=%d last_action_age=%lds "
         "last_hr_report_age=%lds budget=%d/%u\n",
         state_name(f.state), (unsigned)f.confidence, f.hr_bpm, (int)f.hr_ok,
         (unsigned)f.hr_age_s, f.hr_baseline_bpm, f.hr_baseline_dev,
         (unsigned)f.steps, (unsigned)f.steps_delta_60s,
         (unsigned)f.still_s, (unsigned)f.idle_s, (int)f.dnd,
         (int)f.quiet, (unsigned)f.hour, (int)f.in_call_or_busy, s_hr_consec,
         last_action_age, hr_report_age, used,
         (unsigned)st.daily_budget);

  /* P1b：App 下发的运行时设置（care_settings_get 的固件侧真值） */
  printf("[care_router] settings quiet=%u-%u daily_budget=%u types=0x%x "
         "voice=%d (hr=%d idle=%d sedentary=%d)\n",
         (unsigned)st.quiet_start, (unsigned)st.quiet_end,
         (unsigned)st.daily_budget, (unsigned)st.types, (int)st.voice,
         (st.types & CARE_ROUTER_TYPES_HR) ? 1 : 0,
         (st.types & CARE_ROUTER_TYPES_IDLE) ? 1 : 0,
         (st.types & CARE_ROUTER_TYPES_SEDENTARY) ? 1 : 0);
}
