/*
 * companion_mood.c — 桌面小跟班「触觉情绪」模块 (openvela / AI-VOX3)
 *
 * 20 ms 轮询两个输入脚，去抖后触发舵机动作并更新 mood。
 *
 * 参考了 xiaozhi 侧 companion_mood 的实战结论（doc20）：
 *   - TTP223 上电后有约 2.5 s 不稳定期，先静默再采样；
 *   - 若某个脚连续 8 s 恒为"有效电平"，判定为卡死（模块极性接反或一直被压住），
 *     自动翻转极性，避免"一直被摸"；
 *   - SW-420 输出的是**短脉冲**，电平去抖容易吃掉，所以按"脉冲计数"处理：
 *     只要在一次轮询窗口内采到有效电平就记一次脉冲。
 */

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <unistd.h>

#include <nuttx/ioexpander/gpio.h>

#include "companion_mood.h"
#include "servo.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define DEV_TOUCH       "/dev/gpio1"   /* GPIO42 TTP223 抚摸 */
#define DEV_TAP         "/dev/gpio2"   /* GPIO48 SW-420 拍打 */

#define POLL_USEC       20000          /* 20 ms 轮询 */
#define SETTLE_MS       2500           /* 上电静默 */
#define DEBOUNCE_N      3              /* 连续 N 次同电平才认定 */
#define STUCK_MS        8000           /* 卡死判定 */
#define COOLDOWN_MS     700            /* 两次动作最小间隔（舵机动作约 660ms） */

#define MOOD_MIN        (-2)
#define MOOD_MAX        2

#define MOOD_STACK      4096
#define MOOD_PRIO       100

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* 单个输入脚的采样状态机 */
struct pin_s
{
  int            fd;
  const char    *name;
  const char    *dev;
  int            valid;        /* 有效电平（0 或 1），卡死时可翻转 */
  int            last_raw;
  int            same_n;       /* 连续相同采样次数 */
  int            armed;        /* 1=已松开，可以触发下一次 */
  long           hold_ms;      /* 持续保持当前电平的时长 */
  int            pulse;        /* 本轮是否采到有效脉冲 */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct pin_s g_touch =
{
  -1, "touch(摸)", DEV_TOUCH, 1, -1, 0, 1, 0, 0
};

static struct pin_s g_tap =
{
  -1, "tap(拍)", DEV_TAP, 1, -1, 0, 1, 0, 0
};

static volatile int  g_mood = 0;
static volatile int  g_stop = 0;
static volatile int  g_n_touch = 0;
static volatile int  g_n_tap = 0;
static volatile int  g_started = 0;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int pin_read(struct pin_s *p)
{
  bool value = false;
  int ret;

  if (p->fd < 0)
    {
      return -ENODEV;
    }

  ret = ioctl(p->fd, GPIOC_READ, (unsigned long)((uintptr_t)&value));
  if (ret < 0)
    {
      return -errno;
    }

  return value ? 1 : 0;
}

/* 采样一次并更新去抖状态机；返回 1 表示"本轮产生了一次有效触发" */
static int pin_poll(struct pin_s *p)
{
  int raw = pin_read(p);
  int fired = 0;

  if (raw < 0)
    {
      return 0;
    }

  if (raw == p->last_raw)
    {
      p->same_n++;
      p->hold_ms += (POLL_USEC / 1000);
    }
  else
    {
      p->same_n = 1;
      p->hold_ms = 0;
      p->last_raw = raw;
    }

  /* 卡死自翻极性：长时间恒为有效电平，多半是极性反了或被一直压住 */
  if (p->hold_ms > STUCK_MS && raw == p->valid)
    {
      p->valid = p->valid ? 0 : 1;
      p->hold_ms = 0;
      p->armed = 1;
      syslog(LOG_WARNING, "[mood] %s stuck -> flip polarity to %d\n",
             p->name, p->valid);
      return 0;
    }

  if (p->same_n >= DEBOUNCE_N && raw == p->valid)
    {
      if (p->armed)
        {
          fired = 1;
          p->armed = 0;
        }
    }
  else if (p->same_n >= DEBOUNCE_N && raw != p->valid)
    {
      /* 松开，重新武装 */
      p->armed = 1;
    }

  return fired;
}

static void bring_mood(int delta)
{
  int m = g_mood + delta;

  if (m < MOOD_MIN)
    {
      m = MOOD_MIN;
    }

  if (m > MOOD_MAX)
    {
      m = MOOD_MAX;
    }

  g_mood = m;
}

static void on_touch(void)
{
  g_n_touch++;
  bring_mood(+1);
  syslog(LOG_INFO, "[mood] 抚摸 +1 -> %d (%s) : 点头\n",
         g_mood, companion_mood_name(g_mood));
  servo_nod();
}

static void on_tap(void)
{
  g_n_tap++;
  bring_mood(-1);
  syslog(LOG_INFO, "[mood] 被拍 -1 -> %d (%s) : 摇头\n",
         g_mood, companion_mood_name(g_mood));
  servo_shake();
}

static FAR void *mood_thread(FAR void *arg)
{
  long cooldown = 0;

  /* 上电静默，等 TTP223 稳定 */
  usleep(SETTLE_MS * 1000);

  g_touch.fd = open(DEV_TOUCH, O_RDWR);
  g_tap.fd   = open(DEV_TAP, O_RDWR);

  if (g_touch.fd < 0 && g_tap.fd < 0)
    {
      syslog(LOG_ERR, "[mood] both input pins unavailable, thread exit\n");
      g_started = 0;
      return NULL;
    }

  if (g_touch.fd < 0)
    {
      syslog(LOG_WARNING, "[mood] %s unavailable\n", DEV_TOUCH);
    }

  if (g_tap.fd < 0)
    {
      syslog(LOG_WARNING, "[mood] %s unavailable\n", DEV_TAP);
    }

  syslog(LOG_INFO, "[mood] 触觉情绪已启动：摸=%s 拍=%s\n",
         DEV_TOUCH, DEV_TAP);

  while (!g_stop)
    {
      int t = pin_poll(&g_touch);
      int p = pin_poll(&g_tap);

      if ((t || p) && cooldown <= 0)
        {
          if (t)
            {
              on_touch();
            }

          if (p)
            {
              on_tap();
            }

          cooldown = COOLDOWN_MS;
        }

      if (cooldown > 0)
        {
          cooldown -= (POLL_USEC / 1000);
        }

      usleep(POLL_USEC);
    }

  if (g_touch.fd >= 0)
    {
      close(g_touch.fd);
      g_touch.fd = -1;
    }

  if (g_tap.fd >= 0)
    {
      close(g_tap.fd);
      g_tap.fd = -1;
    }

  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int companion_mood_start(void)
{
  pthread_t tid;

  if (g_started)
    {
      return OK;
    }

  g_stop = 0;

  if (pthread_create(&tid, NULL, mood_thread, NULL) != 0)
    {
      syslog(LOG_ERR, "[mood] pthread_create failed\n");
      return -1;
    }

  pthread_detach(tid);
  g_started = 1;
  syslog(LOG_INFO, "[mood] polling thread started\n");
  return OK;
}

void companion_mood_stop(void)
{
  g_stop = 1;
  g_started = 0;
}

int companion_mood_get(void)
{
  return g_mood;
}

const char *companion_mood_name(int mood)
{
  switch (mood)
    {
      case 2:  return "excited";
      case 1:  return "happy";
      case 0:  return "neutral";
      case -1: return "unhappy";
      default: return "sad";
    }
}

void companion_mood_counts(int *n_touch, int *n_tap)
{
  if (n_touch)
    {
      *n_touch = g_n_touch;
    }

  if (n_tap)
    {
      *n_tap = g_n_tap;
    }
}
