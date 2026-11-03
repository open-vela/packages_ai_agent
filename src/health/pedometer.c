/****************************************************************************
 * pedometer.c - 计步器（读 /dev/mpu0）
 *
 * ── 算法 ──────────────────────────────────────────────────────
 *
 * 用的是最常见的「模长 + 高通 + 迟滞峰值检测」，不需要 FFT、不需要 ML：
 *
 *   mag   = sqrt(x^2 + y^2 + z^2)          原始 LSB，1g = 8192
 *   grav  = EMA(mag, 0.02)                 重力基线，时间常数约 2 秒
 *   motion= mag - grav                     高通：去掉重力，只留运动分量
 *   smooth= EMA(motion, 0.35)              低通：去掉高频噪声
 *
 * 迟滞状态机（在**下降沿**计数，避免一次冲击被数成两步）：
 *
 *   IDLE  --(smooth > +TH)-->  PEAK   记录峰值
 *   PEAK  --(smooth < -TH/2)--> IDLE  计一步（若距上一步 > 250ms）
 *
 *   TH = 600 LSB。依据：这是**实测**出来的 ——
 *     静止时 1 秒内模长波动 span 只有 61~112 LSB（见 25Hz 采样阶段的日志），
 *     而晃动板子时 span 达到 356~11570。600 是静止噪声的 5 倍以上，
 *     同时远小于真实步态的幅度，两边都有余量。
 *
 *   AFS_SEL=1（±4g）也是为这里选的：走路时腕部峰值常超过 2g，
 *   用 ±2g 会削顶，±4g 的 8192 LSB/g 分辨率做计步足够。
 *
 * ── 数据结构来自驱动 ──────────────────────────────────────────
 *
 * mpu60x0 驱动的 read() 返回 struct sensor_data_s（packed，14 字节）：
 *   int16 x_accel, y_accel, z_accel, temp, x_gyro, y_gyro, z_gyro;
 * 驱动已经在里面做过字节交换，出来就是主机序。
 *
 * ── 持久化 ────────────────────────────────────────────────────
 *
 * 步数原来只在 RAM，重启/关机就归零，而表盘和运动页都把它当"今天走了
 * 多少"。现在每天的总步数 + 最近 7 天历史落在
 * /data/ai_agent/health/steps.json（littlefs，掉电保持）：
 *
 *   {"date":"2026-10-31","steps":1234,
 *    "history":[{"date":"2026-10-30","steps":4321}]}
 *
 * 写 flash 有限频（见 SAVE_MIN_INTERVAL_MS），跨天和停止时各补一次。
 * 细节和"为什么"在下面的「步数持久化」一节。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#if defined(CONFIG_UORB) && defined(CONFIG_FITNESS_ALGO)
#  include <uORB/uORB.h>
#  include <fitness/algo_pedometer.h>
#  define PEDO_HAVE_UORB 1
#endif

#include "health/pedometer.h"

/* 落盘用：JSON 走 cJSON（和 tool_health.c / config_store.c 一致），
 * 文件读写走 infra/health_store.c（littlefs + tmp/rename 原子替换）。 */
#include "cJSON.h"
#include "infra/health_store.h"

#define TAG "[pedometer]"

#define IMU_DEV        "/dev/mpu0"

/** 采样周期（毫秒）。25 Hz。 */
#define SAMPLE_PERIOD_MS   40

/* 波形取证日志开关：0=关（默认，行走时 25 行/秒刷屏的噪音主体）；
 * 1=开（取证时用，波形触发条件与旧版一致）。 */
#define PEDO_WAVE_LOG_EN    0

/* ── 诊断汇总间隔（秒）：可调常量，只影响日志频率，不影响算法 ──
 * 旧版每秒打一条 mag= 明细，行走时 20 条/秒刷屏（文档里挂了多日的
 * 待办），粒度也不够分析 -O2 后的采样间隔漂移。现改为每 N 秒一条
 * dt 统计汇总（见 pedo_worker 尾部的诊断段）。 */
#define STATS_INTERVAL_SEC  10

/** 每多少个样本出一条汇总：25Hz × STATS_INTERVAL_SEC。 */
#define REPORT_SAMPLES      (25 * STATS_INTERVAL_SEC)

/** 采样率（Hz）。和 SAMPLE_PERIOD_MS 必须一致，滤波器系数按它算。 */
#define SAMPLE_HZ          25.0f

/* ── 带通滤波 0.7 - 3.5 Hz ────────────────────────────────────
 *
 * 为什么必须要带通，而不是像之前那样只做「减基线」：
 *
 *   实测波形（40ms 间隔，逐样本）：
 *       mag = 10581  7409  10295  7114  8491  9074  6917  10146 ...
 *   相邻样本之间 ±3000 LSB 反复跳变 —— 那是 12 Hz 级别的振动，
 *   也就是 25 Hz 采样下的奈奎斯特频率附近。人走路是 1-2 Hz，
 *   这些高频成分和步态毫无关系，却会被峰值检测当成"步"。
 *   之前 20 下只数到 8~11 步、间隔乱跳 1.4~2.9 秒，根因就在这。
 *
 * 所以先滤到步态频段再判峰：
 *
 *   ① 二阶 Butterworth 低通 fc=3.5Hz —— 抗混叠，压掉振动
 *   ② 二阶 Butterworth 高通 fc=0.7Hz —— 去重力，压掉慢漂移
 *
 * 系数由 RBJ Audio EQ Cookbook 公式在 fs=25Hz 下算得，Q=1/sqrt(2)。
 * 采样率变了必须重算这些系数。 */

/* 低通 3.5 Hz @25Hz */
#define LPF_B0  0.1174f
#define LPF_B1  0.2347f
#define LPF_B2  0.1174f
#define LPF_A1 (-0.8252f)
#define LPF_A2  0.2946f

/* 高通 0.7 Hz @25Hz */
#define HPF_B0  0.8830f
#define HPF_B1 (-1.7660f)
#define HPF_B2  0.8830f
#define HPF_A1 (-1.7524f)
#define HPF_A2  0.7798f

/** 计步阈值：相对于包络的比例，外加绝对下限。 */
#define STEP_TH_ENV_RATIO  0.50f
#define STEP_TH_MIN        120.0f

/** 包络的 EMA 系数（约 0.8 秒）。 */
#define ENV_ALPHA          0.05f

/** 两步最小间隔（毫秒）-> 最快 240 步/分。 */
#define STEP_MIN_MS        250

#define TASK_STACK      4096
#define TASK_PRIO       110      /* 低于 UI 渲染（50-60），不抢帧 */

/* 驱动返回的原始结构，必须和 mpu60x0.c 的 sensor_data_s 一致且 packed */
struct pedo_raw_s
{
  int16_t x_accel;
  int16_t y_accel;
  int16_t z_accel;
  int16_t temp;
  int16_t x_gyro;
  int16_t y_gyro;
  int16_t z_gyro;
} __attribute__((packed));

static volatile bool s_run;
static volatile bool s_started;
static volatile bool s_alive;

static volatile uint32_t s_steps;
static volatile uint32_t s_cadence;

/* 静止时长追踪（睡眠页用）。
 * last_motion = 最后一次明显运动的时刻（毫秒）。
 * 判定阈值比计步低得多：计步要 400ms 去抖，这里只判断有没有动。 */
static volatile uint32_t s_last_motion_ms;
static volatile uint32_t s_active_ms;
static volatile uint32_t s_started_ms;

#ifdef PEDO_HAVE_UORB
/* 本树的 orb_advertise() 返回 int fd（不是 orb_advert_t 句柄），
 * 失败时返回负数。参考 sensors/max30102.c 的用法。 */
static int s_pub = -1;
#endif

static pthread_t s_thread;
static int       s_fd = -1;

/* 最近一帧，给 pedometer_dump() 用 */
static volatile int16_t s_last_x, s_last_y, s_last_z;
static volatile int32_t s_last_mag;
static volatile int32_t s_last_motion;
static volatile int32_t s_last_smooth;

/* ── 步数持久化 ────────────────────────────────────────────────
 *
 * 状态分三块：s_steps（当天的步数，采样线程写）、s_today（这一天的
 * 日期）、s_history（最近 7 天，新的在前）。后两块 + 落盘都归
 * s_store_lock 保护，s_steps 保持 volatile 单写者，读方容忍"差一步"。
 */

#define STORE_NAME            "steps"
#define STORE_HISTORY_MAX     7
#define DATE_LEN              11        /* "YYYY-MM-DD" + '\0' */
#define STORE_JSON_MAX        512       /* 7 天历史 + 今天，最长约 340 字节 */

/* 两次写 flash 的最小间隔。每步都写会把 littlefs 写坏、也拖慢采样线程；
 * 60 秒最多丢一分钟的步数，重启后由下一次保存补上。 */
#define SAVE_MIN_INTERVAL_MS  60000u

/* 跨天检查频率：一天只变一次，没必要跟着 25Hz 采样算时间。 */
#define DAY_CHECK_INTERVAL_MS 1000u

/* 早于这一天的日期视为"RTC 还没对时"。本构建 time() 存的就是本地时间
 * （见 agent_config.h 的 AGENT_UTC_OFFSET_SEC 注释），没对过时的 RTC 是
 * 1970-01-01，这种日期写进历史只会顶掉真实数据。 */
#define DATE_VALID_SINCE      "2020-01-01"

/* 晚于这一天的日期同样视为"RTC 不可信"。本板开机偶发读到 2036 年
 * （SNTP 落地前的 RTC 值），那时"今天"比真实日期大 10 年，跨天逻辑会
 * 把真实的 2026-10-31 当成"过去的一天"压进历史 —— 加个上限直接挡住。 */
#define DATE_VALID_UNTIL      "2027-12-31"

struct pedo_day_s
{
  char     date[DATE_LEN];
  uint32_t steps;
};

static pthread_mutex_t s_store_lock = PTHREAD_MUTEX_INITIALIZER;

static struct pedo_day_s s_history[STORE_HISTORY_MAX];
static int               s_history_len;

/* "今天"的日期。s_steps 就是这一天的步数。 */
static char              s_today[DATE_LEN] = "1970-01-01";
static bool              s_store_loaded;

static uint32_t          s_last_saved_steps;   /* 上次**成功**落盘的 s_steps */
static uint32_t          s_last_save_ms;       /* 上次**尝试**落盘的时刻（限频） */
static uint32_t          s_last_daycheck_ms;
static uint32_t          s_day_key;            /* 跨天判据，见 pedo_day_key() */

/* JSON 拼装/解析的公共缓冲：只在持 s_store_lock 时用。放静态区而不是
 * 栈上 —— worker 线程只有 4 KB 栈。 */
static char              s_json_buf[STORE_JSON_MAX];

/****************************************************************************
 * 工具
 ****************************************************************************/

static uint32_t now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/****************************************************************************
 * 步数持久化
 *
 * 规则（对着需求写的，改之前先想清楚 flash 寿命）：
 *   ① 启动：读 steps.json，date==今天就把 s_steps 接回来；日期变了就把
 *      旧的 {date,steps} 压进 history、清零、立刻保存一次；
 *      没有文件/文件坏了 -> 当 0 处理并写一份新的，绝不因为坏文件崩。
 *   ② 运行：pedo_worker 每秒判一次"日"是否变了（(年,yday) 组合），
 *      跨天走和 ① 完全相同的 rollover。
 *   ③ 落盘：步数有变化 + 距上次尝试 >= 60s 才写；停止时把没来得及写的
 *      那一截补上。失败只 warning —— 记步是主功能，盘坏了也不能停采样。
 *
 * 全部在 s_store_lock 下操作（s_steps 除外，它只有一个写者）。
 ****************************************************************************/

/* 当前本地时间。CONFIG_LIBC_LOCALTIME 没开，这个构建里 time() 存的就
 * 是本地时间（time_sync.c 把本地时间写进 RTC），所以 localtime_r() 拿到
 * 的 tm 就是墙上时间，和表盘/运动页一致。 */
static bool pedo_localtime(struct tm *tmv)
{
  time_t now = time(NULL);

  return localtime_r(&now, tmv) != NULL;
}

/* "YYYY-MM-DD"。用 strftime 是本仓既有的写法（memory_store.c /
 * timesync_main.c 同款），格式符只用到 %Y-%m-%d，这个 libc 支持。 */
static void pedo_date_str(const struct tm *tmv, char out[DATE_LEN])
{
  if (strftime(out, DATE_LEN, "%Y-%m-%d", tmv) == 0)
    {
      /* 年份跑到 4 位以外（RTC 被写成垃圾）：当"未对时"处理，
       * 免得一串残缺日期流进 history。 */
      snprintf(out, DATE_LEN, "1970-01-01");
    }
}

/* 跨天判据：年 × 1000 + yday（和 ui/launcher_page.c:189 同款）。
 * 只看 tm_yday 不够：时钟对齐后年可能变而 yday 不变。 */
static uint32_t pedo_day_key(const struct tm *tmv)
{
  return (uint32_t)(tmv->tm_year * 1000 + tmv->tm_yday);
}

/* 只校验形状（YYYY-MM-DD、10 个字符），不判断年代。 */
static bool pedo_date_shape_ok(const char *date)
{
  int i;

  if (date == NULL || strlen(date) != DATE_LEN - 1)
    {
      return false;
    }

  for (i = 0; i < DATE_LEN - 1; i++)
    {
      if (i == 4 || i == 7)
        {
          if (date[i] != '-')
            {
              return false;
            }
        }
      else if (date[i] < '0' || date[i] > '9')
        {
          return false;
        }
    }

  return true;
}

/* 形状对 + 年代可信（ISO 日期补零，字符串比较等价于时间比较）。 */
static bool pedo_date_ok(const char *date)
{
  return pedo_date_shape_ok(date) &&
         strcmp(date, DATE_VALID_SINCE) >= 0 &&
         strcmp(date, DATE_VALID_UNTIL) <= 0;
}

/* 从 JSON 里取一个步数，带钳位。
 *
 * 坏文件里可能出现 "steps": 1e30 这种能通过 cJSON_Parse、但直接
 * (uint32_t) 转换在 C 里是未定义行为的值；钳到 0/上限最安全。 */
static uint32_t pedo_steps_from_json(const cJSON *item)
{
  double v;

  if (item == NULL || !cJSON_IsNumber(item))
    {
      return 0;
    }

  v = item->valuedouble;
  if (v <= 0.0)
    {
      return 0;
    }

  return (v >= 4294967295.0) ? 0xffffffffu : (uint32_t)v;
}

/* 把一天的记录插到 history 头部（新的在前）。
 * 同日期先删掉旧的：保存失败后重启、同一天被压入两次时不会出现两条一样
 * 的日期。超过 7 条丢最旧的一条。 */
static void pedo_history_push_locked(const char *date, uint32_t steps)
{
  int n = s_history_len;
  int i;

  for (i = 0; i < n; i++)
    {
      if (strcmp(s_history[i].date, date) == 0)
        {
          memmove(&s_history[i], &s_history[i + 1],
                  (size_t)(n - i - 1) * sizeof(s_history[0]));
          n--;
          break;
        }
    }

  if (n >= STORE_HISTORY_MAX)
    {
      n = STORE_HISTORY_MAX - 1;          /* 挤掉最旧的一条 */
    }

  memmove(&s_history[1], &s_history[0], (size_t)n * sizeof(s_history[0]));

  snprintf(s_history[0].date, sizeof(s_history[0].date), "%s", date);
  s_history[0].steps = steps;
  s_history_len      = n + 1;
}

/* 拼出完整的 store JSON（假定已持锁）。成功返回 0，内容在 s_json_buf。 */
static int pedo_build_json_locked(void)
{
  cJSON *root;
  cJSON *hist;
  char  *txt;
  int    i;

  root = cJSON_CreateObject();
  if (root == NULL)
    {
      return -ENOMEM;
    }

  cJSON_AddStringToObject(root, "date", s_today);
  cJSON_AddNumberToObject(root, "steps", (double)s_steps);

  hist = cJSON_AddArrayToObject(root, "history");
  for (i = 0; hist != NULL && i < s_history_len; i++)
    {
      cJSON *day = cJSON_CreateObject();

      if (day == NULL)
        {
          break;                          /* 内存不够就少写几天，不整体失败 */
        }

      cJSON_AddStringToObject(day, "date", s_history[i].date);
      cJSON_AddNumberToObject(day, "steps", (double)s_history[i].steps);
      cJSON_AddItemToArray(hist, day);
    }

  txt = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  if (txt == NULL)
    {
      return -ENOMEM;
    }

  snprintf(s_json_buf, sizeof(s_json_buf), "%s", txt);
  free(txt);

  return 0;
}

/* 落盘（假定已持锁）。失败只 log warning，不影响计步。 */
static void pedo_save_locked(const char *why)
{
  int ret;

  /* 限频按"尝试"计时：盘坏了不能每 40ms 重试一次，那就成了日志 + flash 风暴。 */
  s_last_save_ms = now_ms();

  ret = pedo_build_json_locked();
  if (ret == 0)
    {
      ret = health_store_save(STORE_NAME, s_json_buf);
    }

  if (ret < 0)
    {
      syslog(LOG_WARNING, "%s store save failed (%s): %d\n", TAG, why, ret);
      return;
    }

  s_last_saved_steps = s_steps;
}

/* 跨天（或时钟跳变）：旧的 {日期,步数} 进 history、清零、立刻落盘。
 * 假定已持锁。 */
static void pedo_rollover_locked(const char *today)
{
  uint32_t old_steps = s_steps;
  bool     keep;

  /* 只有"真实走过的过去一天"才写进历史。以下三种情况都丢弃：
   *   1. 日期不合法（RTC 没对时，1970 之类）；
   *   2. 当天一步没走（0 步记录没有信息量，还会把真实的 7 天数据顶掉）；
   *   3. s_today 比今天还新 —— 时间同步把 RTC 从"错误的未来"拨回来，
   *      这条记录不是真实的过去。 */
  keep = pedo_date_ok(s_today) && old_steps > 0 &&
         strcmp(s_today, today) < 0;

  if (keep)
    {
      pedo_history_push_locked(s_today, old_steps);
    }
  else
    {
      syslog(LOG_WARNING,
             "%s rollover: drop %s (%u steps, date_ok=%d future=%d)\n",
             TAG, s_today, (unsigned)old_steps,
             pedo_date_ok(s_today) ? 1 : 0,
             strcmp(s_today, today) > 0 ? 1 : 0);
    }

  syslog(LOG_INFO, "%s daily rollover: date=%s steps=%u -> today=%s steps=0\n",
         TAG, s_today, (unsigned)old_steps, today);

  snprintf(s_today, sizeof(s_today), "%s", today);
  s_steps = 0;

  pedo_save_locked("rollover");
}

/* 启动时加载（假定已持锁）。五种路径：无文件 / 坏 JSON / 同一天 /
 * 跨天 / 时钟未对时，都必须能继续往下走。 */
static void pedo_store_load_locked(void)
{
  struct tm tmv;
  char      today[DATE_LEN];
  char      stored[DATE_LEN];
  cJSON    *root;
  cJSON    *item;
  cJSON    *hist;
  uint32_t  stored_steps = 0;
  uint32_t  hist_steps;
  int       n;
  int       i;

  if (pedo_localtime(&tmv))
    {
      pedo_date_str(&tmv, today);
      s_day_key = pedo_day_key(&tmv);
    }
  else
    {
      snprintf(today, sizeof(today), "1970-01-01");
      s_day_key = 0;
    }

  snprintf(s_today, sizeof(s_today), "%s", today);
  snprintf(stored, sizeof(stored), "1970-01-01");

  n = health_store_load(STORE_NAME, s_json_buf, sizeof(s_json_buf));
  if (n < 0)
    {
      syslog(LOG_INFO, "%s store load: no usable file (%d), start from 0\n",
             TAG, n);
      s_steps            = 0;
      s_last_saved_steps = 0;
      pedo_save_locked("fresh");
      return;
    }

  root = cJSON_Parse(s_json_buf);
  if (root == NULL)
    {
      syslog(LOG_WARNING,
             "%s store load: bad json (%d bytes), start from 0 and rewrite\n",
             TAG, n);
      s_steps            = 0;
      s_last_saved_steps = 0;
      pedo_save_locked("repair");
      return;
    }

  item = cJSON_GetObjectItem(root, "date");
  if (item == NULL || !cJSON_IsString(item) ||
      !pedo_date_shape_ok(item->valuestring))
    {
      syslog(LOG_WARNING, "%s store load: missing/invalid date\n", TAG);
      cJSON_Delete(root);
      s_steps            = 0;
      s_last_saved_steps = 0;
      pedo_save_locked("repair");
      return;
    }

  snprintf(stored, sizeof(stored), "%s", item->valuestring);

  stored_steps = pedo_steps_from_json(cJSON_GetObjectItem(root, "steps"));

  /* history 逐条读：单条坏掉只跳过它，不影响其它天。 */
  hist = cJSON_GetObjectItem(root, "history");
  if (hist != NULL && cJSON_IsArray(hist))
    {
      int cnt = cJSON_GetArraySize(hist);

      for (i = 0; i < cnt && s_history_len < STORE_HISTORY_MAX; i++)
        {
          cJSON *e = cJSON_GetArrayItem(hist, i);
          cJSON *ed;

          if (e == NULL)
            {
              continue;
            }

          ed = cJSON_GetObjectItem(e, "date");
          if (ed == NULL || !cJSON_IsString(ed) ||
              !pedo_date_shape_ok(ed->valuestring))
            {
              continue;
            }

          /* 读入时也做同样的清洗（历史文件可能由旧版本写出）：
           * 0 步的记录没有信息量；比今天还新的日期是时钟跳变残留。 */
          hist_steps = pedo_steps_from_json(cJSON_GetObjectItem(e, "steps"));
          if (hist_steps == 0 || strcmp(ed->valuestring, today) > 0)
            {
              syslog(LOG_INFO,
                     "%s store load: drop history %s (%u steps)\n",
                     TAG, ed->valuestring, (unsigned)hist_steps);
              continue;
            }

          snprintf(s_history[s_history_len].date,
                   sizeof(s_history[s_history_len].date), "%s",
                   ed->valuestring);
          s_history[s_history_len].steps = hist_steps;
          s_history_len++;
        }
    }

  cJSON_Delete(root);

  s_steps = stored_steps;

  syslog(LOG_INFO, "%s store loaded: date=%s steps=%u history=%d\n",
         TAG, stored, (unsigned)stored_steps, s_history_len);

  if (strcmp(stored, today) == 0)
    {
      /* 同一天：接着今天更早的步数往下数。
       * s_last_saved_steps 对齐盘上的值 —— 不用马上重写一遍。 */
      syslog(LOG_INFO, "%s store load date=%s steps=%u\n",
             TAG, stored, (unsigned)stored_steps);
      s_last_saved_steps = stored_steps;
      return;
    }

  /* 今天还没对时（RTC 停在 1970）：先别动数据。等时钟同步后由
   * pedo_check_day() 判跨天，否则现在就会把真实日期压成一条 1970。 */
  if (!pedo_date_ok(today))
    {
      snprintf(s_today, sizeof(s_today), "%s", stored);
      s_last_saved_steps = stored_steps;
      syslog(LOG_WARNING,
             "%s store: clock unsynced (%s), keep date=%s steps=%u\n",
             TAG, today, stored, (unsigned)stored_steps);
      return;
    }

  /* 日期变了（跨天，或 RTC 被往回拨）：旧记录进 history，从 0 开始。
   * 先把 s_today 拨回旧日期 —— rollover 要压进 history 的是它。 */
  snprintf(s_today, sizeof(s_today), "%s", stored);
  pedo_rollover_locked(today);
}

/* 保证 store 已加载过一次（假定已持锁）。先置位再加载，避免重入。 */
static void pedo_store_ensure_loaded_locked(void)
{
  if (s_store_loaded)
    {
      return;
    }

  s_store_loaded = true;
  pedo_store_load_locked();
}

/****************************************************************************
 * 跨天检测 / 限频保存（都从 pedo_worker 里调）
 ****************************************************************************/

/* 每秒最多判一次。判定为跨天时走和启动时同一个 rollover。 */
static void pedo_check_day(void)
{
  struct tm tmv;
  char      today[DATE_LEN];
  uint32_t  now = now_ms();

  if ((now - s_last_daycheck_ms) < DAY_CHECK_INTERVAL_MS)
    {
      return;
    }

  s_last_daycheck_ms = now;

  if (!pedo_localtime(&tmv))
    {
      return;
    }

  pthread_mutex_lock(&s_store_lock);

  pedo_store_ensure_loaded_locked();

  if (pedo_day_key(&tmv) == s_day_key)
    {
      pthread_mutex_unlock(&s_store_lock);
      return;
    }

  s_day_key = pedo_day_key(&tmv);
  pedo_date_str(&tmv, today);

  if (strcmp(today, s_today) == 0)
    {
      pthread_mutex_unlock(&s_store_lock);
      return;
    }

  if (!pedo_date_ok(today))
    {
      /* RTC 被拨回未对时的状态：保留现有数据，等下次同步。 */
      syslog(LOG_WARNING, "%s day check: clock unsynced (%s), keep %s\n",
             TAG, today, s_today);
      pthread_mutex_unlock(&s_store_lock);
      return;
    }

  pedo_rollover_locked(today);

  pthread_mutex_unlock(&s_store_lock);
}

/* 步数变了且距上次尝试 >= 60s 才写一次。 */
static void pedo_maybe_save(void)
{
  uint32_t now = now_ms();

  /* 快路径不加锁：s_last_save_ms 只在采样线程（以及启动/停止这类单线程
   * 时刻）写，32 位对齐读写不会撕裂，省掉 25 次/秒的加锁。 */
  if ((now - s_last_save_ms) < SAVE_MIN_INTERVAL_MS)
    {
      return;
    }

  pthread_mutex_lock(&s_store_lock);

  if (s_steps != s_last_saved_steps)
    {
      pedo_save_locked("periodic");
    }

  pthread_mutex_unlock(&s_store_lock);
}

/****************************************************************************
 * 发布到 uORB
 *
 * 表盘的步数卡读的是 algo_pedometer 话题（watchface_page.c 的 read_steps
 * 用 orb_read_once）。没有发布者时它返回 -1，界面就显示 "--"。
 * 这个函数就是那个一直缺失的发布者。
 ****************************************************************************/

static void pedo_publish(uint16_t delta)
{
#ifdef PEDO_HAVE_UORB
  struct algo_pedometer pd;

  memset(&pd, 0, sizeof(pd));

  pd.timestamp_us      = (uint64_t)now_ms() * 1000u;
  pd.count             = s_steps;
  pd.count_rt          = s_steps;
  pd.delta             = delta;
  pd.delta_rt          = delta;
  pd.step_frequency    = s_cadence;
  pd.step_frequency_rt = s_cadence;

  if (s_pub < 0)
    {
      int fd = orb_advertise(ORB_ID(algo_pedometer), &pd);

      if (fd < 0)
        {
          syslog(LOG_WARNING, "%s orb_advertise failed: %d\n", TAG, fd);
          return;
        }

      s_pub = fd;
      syslog(LOG_INFO, "%s published algo_pedometer (fd=%d)\n", TAG, fd);
    }
  else
    {
      int ret = orb_publish(ORB_ID(algo_pedometer), s_pub, &pd);

      if (ret < 0)
        {
          syslog(LOG_WARNING, "%s orb_publish failed: %d\n", TAG, ret);
        }
    }
#else
  (void)delta;
#endif
}

/****************************************************************************
 * 采样线程
 ****************************************************************************/

static void *pedo_worker(void *arg)
{
  struct pedo_raw_s raw;

  /* ── 滤波器状态（Direct Form II biquad）── */
  float lpf_x1 = 0.0f, lpf_x2 = 0.0f;
  float lpf_y1 = 0.0f, lpf_y2 = 0.0f;
  float hpf_x1 = 0.0f, hpf_x2 = 0.0f;
  float hpf_y1 = 0.0f, hpf_y2 = 0.0f;

  bool  primed = false;         /* 前几个样本用于预热，不参与判步 */

  /* ── 峰值状态机 ── */
  float    env        = 0.0f;   /* 带通信号的包络 */
  bool     in_peak    = false;
  float    peak_val   = 0.0f;
  uint32_t last_step  = 0;
  bool     have_step  = false;
  uint32_t n_samples  = 0;

  /* 步频估计 */
  float    interval_ms = 0.0f;

  /* 诊断（每 STATS_INTERVAL_SEC 秒一条汇总，纯观测） */
  uint32_t n = 0;
  uint32_t n_dt = 0;               /* 已计入 dt 统计的样本数 */
  uint32_t t_prev = 0;
  uint32_t dt_min = 0xffffffff;
  uint32_t dt_max = 0;
  uint32_t dt_sum = 0;

  /* 波形记录（取证用） */
  int32_t  wave_left = 0;
  uint32_t wave_seq  = 0;

  (void)arg;

  syslog(LOG_INFO,
         "%s worker started: 25Hz, bandpass 0.7-3.5Hz, "
         "th=max(%d, env*%.2f)\n",
         TAG, (int)STEP_TH_MIN, (double)STEP_TH_ENV_RATIO);

  while (s_run)
    {
      ssize_t  got;
      int32_t  mag;
      float    lpf_out;
      float    bp;
      float    th;
      uint32_t t;

      got = read(s_fd, &raw, sizeof(raw));
      if (got != (ssize_t)sizeof(raw))
        {
          s_alive = false;
          syslog(LOG_WARNING, "%s read failed: %d (errno %d)\n",
                 TAG, (int)got, errno);
          usleep(SAMPLE_PERIOD_MS * 1000);
          continue;
        }

      s_alive = true;
      s_last_x = raw.x_accel;
      s_last_y = raw.y_accel;
      s_last_z = raw.z_accel;

      mag = (int32_t)sqrt((double)raw.x_accel * raw.x_accel +
                          (double)raw.y_accel * raw.y_accel +
                          (double)raw.z_accel * raw.z_accel);
      s_last_mag = mag;

      /* ── ① 低通 3.5Hz：抗混叠，压掉 12Hz 级别的振动 ── */
      lpf_out = LPF_B0 * (float)mag + LPF_B1 * lpf_x1 + LPF_B2 * lpf_x2
                - LPF_A1 * lpf_y1 - LPF_A2 * lpf_y2;
      lpf_x2 = lpf_x1;  lpf_x1 = (float)mag;
      lpf_y2 = lpf_y1;  lpf_y1 = lpf_out;

      /* ── ② 高通 0.7Hz：去掉重力分量和慢漂移 ── */
      bp = HPF_B0 * lpf_out + HPF_B1 * hpf_x1 + HPF_B2 * hpf_x2
           - HPF_A1 * hpf_y1 - HPF_A2 * hpf_y2;
      hpf_x2 = hpf_x1;  hpf_x1 = lpf_out;
      hpf_y2 = hpf_y1;  hpf_y1 = bp;

      s_last_motion = (int32_t)bp;
      s_last_smooth = (int32_t)lpf_out;

      /* ── 静止/活动统计 ──
       * 带通信号绝对值超过 150 LSB（约 0.018g）就算"在动"。
       * 远低于计步阈值，因为这里只判断有没有动、不判断是不是一步。 */
      if (bp > 150.0f || bp < -150.0f)
        {
          s_last_motion_ms = now_ms();
        }

      /* ── ③ 包络 ── */
      {
        float a = (bp >= 0.0f) ? bp : -bp;

        env += ENV_ALPHA * (a - env);
      }

      /* 预热：滤波器需要约 2 秒才稳定，这段时间不判步 */
      if (!primed)
        {
          if (++n_samples >= 50)      /* 2 秒 @25Hz */
            {
              primed = true;
            }
        }

      /* ── ④ 波形记录（取证用） ──
       * 行走时会以 25 行/秒刷屏（即文档里挂着的'行走 20 条/秒'噪音主体），
       * 默认关闭；需要取证波形时把 PEDO_WAVE_LOG_EN 改为 1。纯日志开关。 */
#if PEDO_WAVE_LOG_EN
      if (wave_left <= 0 && primed && (bp > 100.0f || bp < -100.0f))
        {
          wave_left = 125;            /* 5 秒 @25Hz */
          wave_seq++;
          syslog(LOG_INFO, "%s WAVE#%u begin\n", TAG, (unsigned)wave_seq);
        }

      if (wave_left > 0)
        {
          syslog(LOG_INFO, "%s W %d %d %d\n",
                 TAG, (int)mag, (int)bp, (int)env);
          wave_left--;
        }
#else
      (void)wave_left;
      (void)wave_seq;
#endif

      /* ── ⑤ 峰值检测（自适应阈值 + 过零回位） ── */
      t = now_ms();

      th = env * STEP_TH_ENV_RATIO;
      if (th < STEP_TH_MIN)
        {
          th = STEP_TH_MIN;
        }

      if (primed)
        {
          if (!in_peak)
            {
              if (bp > th)
                {
                  in_peak = true;
                  peak_val = bp;
                }
            }
          else
            {
              if (bp > peak_val)
                {
                  peak_val = bp;
                }

              /* 回到零以下 = 这一步的上升沿结束 */
              if (bp < 0.0f)
                {
                  if (!have_step || (t - last_step) >= STEP_MIN_MS)
                    {
                      if (have_step)
                        {
                          float dt = (float)(t - last_step);

                          interval_ms = (interval_ms <= 0.0f)
                                        ? dt
                                        : interval_ms * 0.7f + dt * 0.3f;
                        }

                      s_steps = s_steps + 1;

                      syslog(LOG_INFO,
                             "%s STEP #%u peak=%d env=%d th=%d gap=%ums\n",
                             TAG, (unsigned)s_steps, (int)peak_val,
                             (int)env, (int)th,
                             have_step ? (unsigned)(t - last_step) : 0u);

                      last_step = t;
                      have_step = true;

                      pedo_publish(1);
                    }

                  in_peak = false;
                  peak_val = 0.0f;
                }
            }
        }

      /* 步频 */
      if (interval_ms > 1.0f && have_step && (t - last_step) < 3000)
        {
          float cad = 60000.0f / interval_ms;

          s_cadence = (cad > 250.0f) ? 250u : (uint32_t)cad;
        }
      else if (have_step && (t - last_step) >= 3000)
        {
          s_cadence = 0;
        }

      /* ── ⑥ 持久化：跨天检查 + 限频落盘 ──
       * 两者都自带限频（跨天 1 秒判一次、保存 60 秒一次），不给 25Hz
       * 的采样循环增加 I/O；这里也就不把 t 传进去，免得和落盘时更新的
       * 时刻混用。顺序先判天再保存：跨天那次 rollover 会立刻写盘。 */
      pedo_check_day();
      pedo_maybe_save();

      /* ── 诊断：实测采样间隔统计（只加观测，判步/滤波逻辑未动）── */
      if (t_prev != 0)
        {
          uint32_t dt = t - t_prev;

          if (dt < dt_min) dt_min = dt;
          if (dt > dt_max) dt_max = dt;
          dt_sum += dt;
          n_dt++;
        }
      t_prev = t;

      if (++n >= REPORT_SAMPLES)
        {
          syslog(LOG_INFO,
                 "%s samples=%u min_dt=%ums max_dt=%ums avg_dt=%ums "
                 "steps=%u\n",
                 TAG, (unsigned)n,
                 (unsigned)(n_dt > 0 ? dt_min : 0),
                 (unsigned)(n_dt > 0 ? dt_max : 0),
                 (unsigned)(n_dt > 0 ? dt_sum / n_dt : 0),
                 (unsigned)s_steps);

          n = 0;
          n_dt = 0;
          t_prev = 0;
          dt_min = 0xffffffff;
          dt_max = 0;
          dt_sum = 0;
        }

      usleep(SAMPLE_PERIOD_MS * 1000);
    }

  syslog(LOG_INFO, "%s worker stopped (steps=%u)\n",
         TAG, (unsigned)s_steps);
  return NULL;
}

/****************************************************************************
 * 公开接口
 ****************************************************************************/

int pedometer_start(void)
{
  if (s_started)
    {
      return 0;
    }

  /* 先接回盘上的步数/历史，再碰 IMU：这样即使 /dev/mpu0 打不开，
   * 运动和表盘页仍能显示今天的数，工具也能给出历史。 */
  pthread_mutex_lock(&s_store_lock);
  pedo_store_ensure_loaded_locked();
  pthread_mutex_unlock(&s_store_lock);

  syslog(LOG_INFO, "%s start: step 1 open %s\n", TAG, IMU_DEV);

  s_fd = open(IMU_DEV, O_RDONLY);
  if (s_fd < 0)
    {
      syslog(LOG_ERR, "%s open %s failed: %d\n", TAG, IMU_DEV, errno);
      return -1;
    }

  syslog(LOG_INFO, "%s start: step 2 opened fd=%d\n", TAG, s_fd);

  s_run = true;
  s_started = true;

  s_started_ms     = now_ms();
  s_last_motion_ms = s_started_ms;
  s_active_ms      = 0;

  syslog(LOG_INFO, "%s start: step 3 creating thread\n", TAG);

  if (pthread_create(&s_thread, NULL, pedo_worker, NULL) != 0)
    {
      syslog(LOG_ERR, "%s pthread_create failed\n", TAG);
      s_started = false;
      s_run = false;
      close(s_fd);
      s_fd = -1;
      return -1;
    }

  syslog(LOG_INFO, "%s start: step 4 thread created OK\n", TAG);

  return 0;
}

void pedometer_stop(void)
{
  if (!s_started)
    {
      return;
    }

  s_run = false;
  pthread_join(s_thread, NULL);

  /* 线程已停，这里独占状态：把"上次保存之后新增的步数"补写一次
   * （60 秒限频没轮到的那部分，否则掉电就丢了）。没有变化就不写，
   * 省一次 flash 擦写 —— 也不违背"停止时再保存一次"的要求。 */
  pthread_mutex_lock(&s_store_lock);
  if (s_steps != s_last_saved_steps)
    {
      pedo_save_locked("stop");
    }
  pthread_mutex_unlock(&s_store_lock);

  if (s_fd >= 0)
    {
      close(s_fd);
      s_fd = -1;
    }

  s_started = false;
}

uint32_t pedometer_get_steps(void)
{
  return s_steps;
}

uint32_t pedometer_get_cadence(void)
{
  return s_cadence;
}

bool pedometer_is_alive(void)
{
  return s_alive;
}

uint32_t pedometer_get_still_seconds(void)
{
  if (!s_started)
    {
      return 0;
    }

  return (now_ms() - s_last_motion_ms) / 1000u;
}

uint32_t pedometer_get_active_seconds(void)
{
  return s_active_ms / 1000u;
}

void pedometer_dump(void)
{
  printf("%s: x=%d y=%d z=%d mag=%d motion=%d smooth=%d | "
         "steps=%u cad=%u alive=%d\n",
         TAG, (int)s_last_x, (int)s_last_y, (int)s_last_z,
         (int)s_last_mag, (int)s_last_motion, (int)s_last_smooth,
         (unsigned)s_steps, (unsigned)s_cadence, (int)s_alive);
}

/****************************************************************************
 * 步数历史（给工具层用）
 *
 * 输出的是"当前完整状态"：今天的日期 + 今天的步数（实时，不是上次落盘
 * 的值）+ 最近 7 天。没有文件、没启动、甚至没插 IMU 都返回合法 JSON，
 * 工具层不用再判空。
 ****************************************************************************/

int pedometer_get_history_json(char *out, size_t cap)
{
  int ret = 0;

  if (out == NULL || cap == 0)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&s_store_lock);

  /* 惰性加载：没调过 pedometer_start()（或无 /dev/mpu0）时也能报历史。 */
  pedo_store_ensure_loaded_locked();

  if (pedo_build_json_locked() < 0)
    {
      ret = -ENOMEM;
      snprintf(out, cap, "{\"error\":\"out of memory\"}");
    }
  else if (strlen(s_json_buf) >= cap)
    {
      /* 截断的 JSON 不合法，不如直接报错让调用方换大缓冲。 */
      ret = -ENOSPC;
      snprintf(out, cap, "{\"error\":\"buffer too small\"}");
    }
  else
    {
      snprintf(out, cap, "%s", s_json_buf);
    }

  pthread_mutex_unlock(&s_store_lock);

  return ret;
}

