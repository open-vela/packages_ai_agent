/*
 * hr_monitor.c - Simulated heart-rate random walk + threshold check.
 *
 * 随机游走：基线 ±1 bpm/秒的小扰动，夹在基线 ±6 的走廊里；
 * 注入事件后先向上游走到峰值再指数回落——曲线像一次「爬楼后休息」，
 * 演示时视觉上可信。全部状态是几个 static 标量：无堆、无线程。
 *
 * Team 181 - Contest 2026
 */

#include <stdlib.h>
#include <syslog.h>

#include "hr_monitor.h"
#include "ui/pet_care.h"

#define TAG "hr"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static int  s_base = 78;          /* 模拟基线（hr_set 可改） */
static int  s_current = 78;       /* 当前值 */
static bool s_event_active;        /* 注入事件进行中 */
static int  s_event_elapsed;       /* 事件进行秒数 */
static bool s_alerted_this_event;  /* 本事件（或本段高值）已告警过 */

/* 简易夹取 */
static int clampi(int v, int lo, int hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void hr_monitor_tick(void)
{
  int next = s_current;
  int peak = 0;

  /* 随机游走一步：±1 bpm */
  next += (rand() % 3) - 1;

  if (s_event_active)
    {
      s_event_elapsed++;

      if (s_event_elapsed <= HR_MONITOR_EVENT_RISE_S)
        {
          /* 上升段：向峰值线性爬升 + 扰动 */
          peak = HR_MONITOR_EVENT_PEAK_MIN +
                 rand() % (HR_MONITOR_EVENT_PEAK_MAX -
                           HR_MONITOR_EVENT_PEAK_MIN + 1);
          next = s_current + (peak - s_current + 2) / 3;  /* 每秒收 1/3 差距 */
        }
      else if (s_event_elapsed <= HR_MONITOR_EVENT_RISE_S +
                                   HR_MONITOR_EVENT_FALL_S)
        {
          /* 回落段：向基线收 1/4 差距（比上升慢，像喘匀气） */
          next = s_current - (s_current - s_base + 3) / 4;
        }
      else
        {
          s_event_active = false;   /* 事件结束，回到日常游走 */
        }
    }
  else
    {
      /* 日常：把值拉回基线走廊，防止游走出界 */
      if (next > s_base + 6)
        {
          next = s_base + 6;
        }
      if (next < s_base - 6)
        {
          next = s_base - 6;
        }
    }

  s_current = clampi(next, 40, 200);

  /* 阈值判定：超过 HR_MONITOR_HIGH_DEFAULT 且本段没报过 → pet_care 告警。
   * 冷却窗在 pet_care_hr_report 内部（10 分钟），这里的 s_alerted 只是
   * 保证「同一事件不重复报」，跨事件的高值靠 pet_care 冷却兜底。 */
  if (s_current > HR_MONITOR_HIGH_DEFAULT)
    {
      if (!s_alerted_this_event)
        {
          s_alerted_this_event = true;
          pet_care_hr_report(s_current);
        }
    }
  else if (s_current < HR_MONITOR_HIGH_DEFAULT - 15)
    {
      /* 回到安全区间足够深才重置告警标志，避免在阈值附近抖动重报 */
      s_alerted_this_event = false;
    }
}

int hr_monitor_set(int base_bpm)
{
  s_base = clampi(base_bpm, HR_MONITOR_BASE_MIN, HR_MONITOR_BASE_MAX);
  syslog(LOG_INFO, "[%s] baseline set to %d bpm\n", TAG, s_base);
  return s_base;
}

void hr_monitor_inject_event(void)
{
  s_event_active = true;
  s_event_elapsed = 0;
  s_alerted_this_event = false;
  syslog(LOG_INFO, "[%s] event injected (peak in %ds)\n",
         TAG, HR_MONITOR_EVENT_RISE_S);
}

int hr_monitor_current(void)
{
  return s_current;
}
