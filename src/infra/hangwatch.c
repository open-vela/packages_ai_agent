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
#include <fcntl.h>
#include <syslog.h>
#include <sys/ioctl.h>

#include <nuttx/sched.h>
#include <nuttx/timers/watchdog.h>

#include "infra/hangwatch.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define HW_POLL_MS      1000    /* 采样周期 */
#define HW_ALIVE_MS     5000    /* [alive] 打印周期 */
/* 2026-11-01 v102a：实测硬件 IWDT 空窗仅 ~5.5–7s（配置写 20s，时钟口径存疑），
 * HW_STALL_MS=8000 比它还长 → [stall] 任务转储永远来不及打印（现场 0 条 [stall]）。
 * 先降到 3000ms，保证下次冻死能在复位前留下栈。 */
#define HW_STALL_MS     3000    /* UI 心跳停多久判为卡死 */
#define HW_STALL_RELOG  10000   /* [stall] 重复打印的限流间隔 */
#define HW_STACK        3072
#define HW_NSTATES      16      /* tstate_t 的桶数(只用低 4 位, 足够) */

/* 看门狗线程优先级: **必须高于系统里所有任务**, 否则真机那种"某个高优先级任务
 * 一直自旋"的冻屏会把看门狗一起饿死, 现场永远拿不到。
 * 真机 ps 实测的优先级分布: hpwork=224, audcodec_rx=120, sysworkq=110,
 * bt_service_ 与 bt_client_ 都是 103, bluetoothd/ai_agent/nsh=100, 其余 3~60。
 * 取 240(上限 255)压过全部; 本线程只 sleep+syslog+nxsched_foreach, 不会霸占 CPU。 */
#define HW_PRIO        240

/* ── 硬件看门狗(硬复位兜底) ────────────────────────────────────────────────
 * 真机出现过"整个调度器停摆"的冻屏([alive] 只打出 watchdog started 一行就
 * 再无输出, 说明连定时器唤醒都不再发生) —— 那种情况下软件无自救可能, 只能
 * 靠独立看门狗把 SoC 复位。这里喂狗的是本线程(1s 一次), 超时给 20s:
 * 系统正常时余量 20 倍; 真冻死时 20s 后自动重启, 不必人工拔插。
 *
 * 注意: 必须**先 SETTIMEOUT 再 START** —— 驱动的 sf32lb_start() 在
 * priv->timeout==0 时会用 CONFIG_SF32LB_IWDG_DEFTIMOUT(=2.5s) 兜底,
 * 顺序反了就会以 2.5s 超时启动, 正常启动过程都可能被复位。 */
#define HW_WDT_ENABLE      1
#define HW_WDT_TIMEOUT_MS  20000
#define HW_WDT_DEVPATH     "/dev/watchdog0"

/* ── V142-FIX: 复位后不清零的心跳计数（RTC 备份寄存器，AON 域）──────────
 * 板上 ld.script 没有 .noinit 段（不能改 ld），改用 RTC BKP 寄存器
 * idx5(heartbeat)/idx6(boot)：看门狗复位后仍然保持，只有掉电/换电池才丢。
 * 用途：复位后日志打印 hb=/boot=；与复位前最后一条 [alive] 的 hb 对比，
 * 可区分两种停摆：
 *   - 重启后读到的 hb ≈ 复位前最后打印的 hb → UI 心跳先停（真冻屏/关中断）
 *   - 重启后读到的 hb 明显更大               → UI 还在跑，是 hangwatch/
 *     日志线程自己死了（本线程优先级 240，能跑但没打日志）
 * 数据格式：高字节 magic 区分"我方数据"与冷启动/其它使用者。
 * 风险：HAL 的 RTC_BACKUP 枚举只用到 idx 0/1/2/3/4/7/8/9/11；idx5/6 在
 * 当前树内无人使用（全仓 grep 确认）。若后续被第三方占用需更换 idx。
 * 写入频率 1Hz（AON 寄存器，不进 flash），开销可忽略。
 */
#define HW_BKP_HB_IDX     5u
#define HW_BKP_BOOT_IDX   6u
#define HW_BKP_HB_MAGIC   0x48000000u   /* 'H' */
#define HW_BKP_BOOT_MAGIC 0xB1000000u
#define HW_BKP_VAL_MASK   0x00FFFFFFu

extern void HAL_Set_backup(uint8_t idx, uint32_t value);
extern uint32_t HAL_Get_backup(uint8_t idx);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static volatile uint32_t g_ui_tick;      /* LVGL 任务心跳(1Hz)，本 boot 内 */
static volatile uint32_t g_hb_total;     /* 跨复位累计心跳（RTC BKP 镜像） */
static volatile uint32_t g_boot_no;      /* 跨复位 boot 序号 */
static bool              g_hb_inited;

/* 懒初始化：UI 可能先于 hangwatch_start 开始 tick，不能等 start 才读 */
static void hw_hb_lazy_init(void)
{
  uint32_t hb;
  uint32_t boot;

  if (g_hb_inited)
    {
      return;
    }
  g_hb_inited = true;

  hb = HAL_Get_backup(HW_BKP_HB_IDX);
  boot = HAL_Get_backup(HW_BKP_BOOT_IDX);

  g_hb_total = ((hb & 0xFF000000u) == HW_BKP_HB_MAGIC)
               ? (hb & HW_BKP_VAL_MASK) : 0;
  g_boot_no = ((boot & 0xFF000000u) == HW_BKP_BOOT_MAGIC)
              ? (boot & HW_BKP_VAL_MASK) : 0;
}

static void hw_hb_persist(void)
{
  HAL_Set_backup(HW_BKP_HB_IDX,
                 HW_BKP_HB_MAGIC | (g_hb_total & HW_BKP_VAL_MASK));
}

static void hw_boot_persist(void)
{
  HAL_Set_backup(HW_BKP_BOOT_IDX,
                 HW_BKP_BOOT_MAGIC | (g_boot_no & HW_BKP_VAL_MASK));
}

static volatile bool     g_hw_running;
static int               g_wdt_fd = -1;  /* /dev/watchdog0, <0 = 未启用 */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void hangwatch_ui_tick(void)
{
  hw_hb_lazy_init();
  g_ui_tick++;
  g_hb_total++;
  hw_hb_persist();   /* 1Hz 写 RTC 备份寄存器：复位后还能读回 */
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

/* 打开并启动硬件看门狗。失败只告警不致命(冻屏取证仍可用)。 */

static void hw_wdt_arm(void)
{
#if HW_WDT_ENABLE
  g_wdt_fd = open(HW_WDT_DEVPATH, O_RDWR);
  if (g_wdt_fd < 0)
    {
      syslog(LOG_WARNING, "[alive] %s open failed: %d (no auto-reboot)\n",
             HW_WDT_DEVPATH, errno);
      return;
    }

  if (ioctl(g_wdt_fd, WDIOC_SETTIMEOUT, (unsigned long)HW_WDT_TIMEOUT_MS) < 0)
    {
      syslog(LOG_WARNING, "[alive] WDIOC_SETTIMEOUT(%d) failed: %d\n",
             HW_WDT_TIMEOUT_MS, errno);
      close(g_wdt_fd);
      g_wdt_fd = -1;
      return;
    }

  if (ioctl(g_wdt_fd, WDIOC_START, 0) < 0)
    {
      syslog(LOG_WARNING, "[alive] WDIOC_START failed: %d\n", errno);
      close(g_wdt_fd);
      g_wdt_fd = -1;
      return;
    }

  syslog(LOG_INFO, "[alive] hardware watchdog armed (%dms, fed 1Hz)\n",
         HW_WDT_TIMEOUT_MS);
#endif
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
  hw_wdt_arm();

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

      /* ★ 启动宽限(2026-10-14 真机踩到, 必须保留):
       * UI 心跳**一次都还没来过**时不算卡死 —— 板子正在启动, launcher 的
       * 1Hz lv_timer 还没建起来, 此时 ui_stuck_ms 是从"看门狗线程启动"那刻
       * 开始涨的。不加这个条件, 开机稍慢(>HW_STALL_MS)就会:
       *   判 stall -> 停止喂狗 -> 约 20s 后把**正在正常启动**的板子复位。
       * 真机现象: 三次启动 00:54:20 / 00:54:43 / 00:55:03, 间隔正好 ~20s,
       * 且**没有任何崩溃 dump**(不是 assert, 是看门狗自己干的)。
       * UI 跑起来过之后再停(真正的冻屏), 判定照旧生效。 */
      stall = (ui_seen != 0) && (ui_stuck_ms >= HW_STALL_MS);

      /* ---- 喂狗: **只在系统被证明还活着时喂** ----
       * 本线程优先级(240)高于系统里所有任务, 所以"我还能跑"并不等于"系统还活着"
       * —— 恰恰在最想救的场景(某个高优先级任务自旋/整机停摆)里, 只有我还能跑。
       * 因此判据用 UI 心跳而不是"我自己被调度到": UI 心跳停 > HW_STALL_MS 就
       * **停止喂狗**, 让硬件看门狗在 HW_WDT_TIMEOUT_MS 后把 SoC 复位。
       * (若这里改成无条件喂, 高优先级反而会让硬件看门狗永远不触发。) */
      if (g_wdt_fd >= 0 && !stall)
        {
          (void)ioctl(g_wdt_fd, WDIOC_KEEPALIVE, 0);
        }

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
                 "[alive] up=%us ui=%u hb=%lu boot=%lu stuck=%ums tasks=%d st=[%s]%s\n",
                 (unsigned)((now - t0) / 1000u), (unsigned)ui_now,
                 (unsigned long)g_hb_total, (unsigned long)g_boot_no,
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
  pthread_attr_t     attr;
  struct sched_param param;
  pthread_t          tid;
  int                ret;

  if (g_hw_running)
    {
      return 0;
    }

  hw_hb_lazy_init();
  g_boot_no = (g_boot_no + 1u) & HW_BKP_VAL_MASK;
  if (g_boot_no == 0)
    {
      g_boot_no = 1;
    }
  hw_boot_persist();

  syslog(LOG_INFO,
         "[alive] boot #%lu (retained heartbeat=%lu, +%lums poll, WDT=%dms)\n",
         (unsigned long)g_boot_no, (unsigned long)g_hb_total,
         (unsigned long)HW_POLL_MS, HW_WDT_TIMEOUT_MS);

  g_hw_running = true;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, HW_STACK);

  /* 显式调度: 不吃父线程(ai_agent, 优先级 100)的继承值。 */
  memset(&param, 0, sizeof(param));
  param.sched_priority = HW_PRIO;
  pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
  pthread_attr_setschedparam(&attr, &param);

  ret = pthread_create(&tid, &attr, hw_thread, NULL);
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      g_hw_running = false;
      syslog(LOG_ERR, "[alive] watchdog pthread_create failed: %d\n", ret);
      return ret;
    }

  /* 名字要在 detach 之前设: ps 里能一眼认出是哪个线程(否则全是 "ai_agent") */
  (void)pthread_setname_np(tid, "hangwatch");

  ret = pthread_detach(tid);
  if (ret != 0)
    {
      syslog(LOG_WARNING, "[alive] watchdog pthread_detach failed: %d\n", ret);
    }

  return 0;
}
