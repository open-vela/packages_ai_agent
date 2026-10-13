/****************************************************************************
 * hangwatch.c - 冻屏/死锁取证看门狗 (接口与背景见 hangwatch.h)
 *
 * 设计约束(踩过的坑, 改之前先读):
 *   1. **不许调 mallinfo()** —— core/agent_mem.h:49 已记录: 它会遍历整个堆,
 *      堆空闲链表一旦被越界写坏就 DEBUGASSERT, 反而把 agent 线程打下去。
 *      看门狗本身必须是无副作用的观察者。
 *   2. **不许碰网络/文件系统/任何锁** —— 卡住现场往往就卡在这些锁上,
 *      看门狗再去拿一次会把"能取证"变成"一起卡"。
 *      只用 clock_gettime + syslog + nxsched_foreach。
 *   3. nxsched_foreach 内部持调度器锁遍历全局任务表, 单核 flat build 下安全。
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/sched.h>

#include "infra/hangwatch.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define HW_POLL_MS      1000    /* 采样周期 */
#define HW_ALIVE_MS     5000    /* [alive] 打印周期 */
#define HW_STALL_MS     8000    /* UI 心跳停多久判为卡死 */
#define HW_STALL_RELOG  10000   /* [stall] 重复打印的限流间隔 */
#define HW_STACK        3072
#define HW_NSTATES      16      /* tstate_t 的桶数(只用低 4 位, 足够) */

/****************************************************************************
 * Private Data
 ****************************************************************************/

static volatile uint32_t g_ui_tick;      /* LVGL 任务心跳(1Hz) */
static volatile bool     g_hw_running;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void hangwatch_ui_tick(void)
{
  g_ui_tick++;
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t hw_now_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
      return 0;
    }

  return (uint32_t)((uint64_t)ts.tv_sec * 1000ULL +
                    (uint64_t)ts.tv_nsec / 1000000ULL);
}

/* 任务直方图 + 全量 dump 共用的收集结构 */

struct hw_scan_s
{
  int total;
  int by_state[HW_NSTATES];
  int dumped;               /* dump 模式下已打印条数 */
};

static void hw_count_one(FAR struct tcb_s *tcb, FAR void *arg)
{
  FAR struct hw_scan_s *s = (FAR struct hw_scan_s *)arg;
  unsigned st = (unsigned)tcb->task_state & (HW_NSTATES - 1);

  s->total++;
  s->by_state[st]++;
}

static void hw_dump_one(FAR struct tcb_s *tcb, FAR void *arg)
{
  FAR struct hw_scan_s *s = (FAR struct hw_scan_s *)arg;
#if CONFIG_TASK_NAME_SIZE > 0
  FAR const char *name = tcb->name;
#else
  FAR const char *name = "?";
#endif

  s->dumped++;
  syslog(LOG_INFO, "[stall] #%-2d pid=%-4d state=%-2d prio=%-3d stk=%-6u %s\n",
         s->dumped, (int)tcb->pid, (int)tcb->task_state,
         (int)tcb->sched_priority, (unsigned)tcb->adj_stack_size,
         name != NULL ? name : "?");
}

static void hw_dump_tasks(FAR const char *why)
{
  struct hw_scan_s s;

  memset(&s, 0, sizeof(s));
  syslog(LOG_INFO, "[stall] ---- task dump (%s) ----\n", why);
  nxsched_foreach(hw_dump_one, &s);
  syslog(LOG_INFO, "[stall] ---- %d tasks ----\n", s.dumped);
}

static void *hw_thread(FAR void *arg)
{
  struct hw_scan_s s;
  uint32_t t0;
  uint32_t last_alive = 0;
  uint32_t last_ui_change;
  uint32_t last_stall_log = 0;
  uint32_t ui_seen = 0;
  uint32_t ui_stuck_ms = 0;
  unsigned n;

  (void)arg;

  t0 = hw_now_ms();
  last_ui_change = t0;
  ui_seen = g_ui_tick;

  syslog(LOG_INFO, "[alive] watchdog started (ui_stall>%dms)\n", HW_STALL_MS);

  while (g_hw_running)
    {
      uint32_t now;
      uint32_t ui_now;
      bool stall;

      usleep(HW_POLL_MS * 1000);

      now   = hw_now_ms();
      ui_now = g_ui_tick;

      if (ui_now != ui_seen)
        {
          ui_seen = ui_now;
          last_ui_change = now;
        }

      ui_stuck_ms = now - last_ui_change;
      stall = (ui_stuck_ms >= HW_STALL_MS);

      /* ---- [alive]: 周期性心跳 + 任务状态直方图 ----
       * 直方图的 key 是 tstate_t 的数值, 映射见 nuttx/include/nuttx/sched.h
       * 的 enum tstate_e (0=INVALID 1=PENDING 2=READYTORUN 3=RUNNING
       * 4=INACTIVE 5=WAIT_SEM ...) —— 打印数值是为了不依赖枚举名。 */

      if ((uint32_t)(now - last_alive) >= HW_ALIVE_MS || last_alive == 0)
        {
          char hist[96];
          int  off = 0;
          int  i;

          last_alive = now;

          memset(&s, 0, sizeof(s));
          nxsched_foreach(hw_count_one, &s);

          hist[0] = '\0';
          for (i = 0; i < HW_NSTATES; i++)
            {
              if (s.by_state[i] == 0)
                {
                  continue;
                }

              off += snprintf(hist + off, sizeof(hist) - (size_t)off,
                              "%s%d:%d", off > 0 ? " " : "", i, s.by_state[i]);
              if (off >= (int)sizeof(hist) - 8)
                {
                  break;
                }
            }

          syslog(LOG_INFO,
                 "[alive] up=%us ui=%u stuck=%ums tasks=%d st=[%s]%s\n",
                 (unsigned)((now - t0) / 1000u), (unsigned)ui_now,
                 (unsigned)ui_stuck_ms, s.total, hist,
                 stall ? " UI-FROZEN" : "");
        }

      /* ---- [stall]: UI 心跳停了 -> 全量 dump, 限流重打 ---- */

      if (stall && (uint32_t)(now - last_stall_log) >= HW_STALL_RELOG)
        {
          last_stall_log = now;
          syslog(LOG_INFO,
                 "[stall] UI heartbeat frozen for %ums (up=%us)\n",
                 (unsigned)ui_stuck_ms, (unsigned)((now - t0) / 1000u));
          hw_dump_tasks("ui-frozen");
        }
    }

  for (n = 0; n < 1; n++)
    {
      (void)n;
    }

  return NULL;
}

int hangwatch_start(void)
{
  pthread_attr_t attr;
  pthread_t      tid;
  int            ret;

  if (g_hw_running)
    {
      return 0;
    }

  g_hw_running = true;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, HW_STACK);

  ret = pthread_create(&tid, &attr, hw_thread, NULL);
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      g_hw_running = false;
      syslog(LOG_ERR, "[alive] watchdog pthread_create failed: %d\n", ret);
      return ret;
    }

  ret = pthread_detach(tid);
  if (ret != 0)
    {
      syslog(LOG_WARNING, "[alive] watchdog pthread_detach failed: %d\n", ret);
    }

  return 0;
}
