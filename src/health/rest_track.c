/****************************************************************************
 * rest_track.c - 休息时段追踪（体动估算）
 *
 * 只读 MPU6050 的加速度，不做医疗级睡眠判定。完整说明见 rest_track.h。
 *
 * 为什么不是「still 一到 30 分钟就记」：
 *   坐着不动、把表放在桌上，still 都会一直涨。夜间窗口 + 45 分钟最短
 *   记录只是把误报压小，不能消除。所以落盘的字段叫 dur_s，界面叫
 *   「休息（体动估算）」，工具里 estimate 固定为 "motion_only"。
 *
 * 线程模型：
 *   - 单独线程，8 KB 栈，detached，启动幂等；
 *   - 每 5 秒读一次 pedometer 的 still/active，只为了**锁存体动**，
 *     避免"体动刚好发生在 60 秒 tick 边界"被漏掉；
 *   - 每 60 秒才推进一次状态机，状态变化都打日志；
 *   - 落盘只发生在夜间段结束时（一晚最多一次），不会写坏 flash。
 *
 * 构建：和 health/pedometer.c 同条件（CONFIG_AI_AGENT_LVGL_UI）编进
 * AGENT_SRCS；非 UI 构建走文件尾的空实现，保证工具层仍可链接。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "infra/health_store.h"
#include "health/pedometer.h"
#include "health/rest_track.h"

#define TAG "[rest_track]"

#if defined(CONFIG_AI_AGENT_LVGL_UI)

/* ── 阈值 / 周期 ─────────────────────────────────────────────── */

#define REST_STORE_NAME       "rest"
#define REST_STORE_JSON_MAX   1024
#define REST_TICK_SEC         60
#define REST_POLL_SEC         5
#define REST_TASK_STACK       8192

/** still 小于这个值 = 刚刚有体动。< 60 秒，按需求写死。 */
#define REST_MOTION_STILL_S   60u

/** 夜间窗口 [22:00, 08:00)。 */
#define REST_NIGHT_START_HOUR 22
#define REST_NIGHT_END_HOUR   8

/** 单段最长 24 小时。超过就是时钟跳变/文件被改，不能写进历史。 */
#define REST_NIGHT_MAX_S      (24u * 60u * 60u)

/** 可信年份。本板开机会偶读 2036，落在窗口外的一律丢弃。 */
#define REST_CLOCK_MIN_YEAR   2024
#define REST_CLOCK_MAX_YEAR   2027

struct rest_session_s
{
  uint32_t start;
  uint32_t end;
  uint32_t dur_s;
};

/* 状态机 / 线程 */
static volatile int  s_state = REST_STATE_AWAKE;
static volatile bool s_started;
static volatile bool s_run;
static volatile bool s_motion_latch;
static volatile uint32_t s_motion_epoch;   /* 体动发生时的本地 epoch；0=未知 */
static uint32_t      s_active_prev;        /* 上次看到的累计活动秒数 */
static uint32_t      s_night_start;        /* 本次 NIGHT 段起点 epoch */
static bool          s_night_clock_ok;     /* 进入 NIGHT 时时钟是否可信 */

static pthread_mutex_t s_start_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_data_lock  = PTHREAD_MUTEX_INITIALIZER;

/* 持久化（s_data_lock 保护） */
static struct rest_session_s s_sessions[REST_MAX_SESSIONS];
static int                   s_session_count;
static struct rest_session_s s_last_night;
static bool                  s_last_night_valid;
static bool                  s_store_loaded;

/* 启动加载缓冲：只在 s_start_lock 内用一次，放静态区而不是调用者
 * （UI/工具线程）的栈上。 */
static char                  s_load_buf[REST_STORE_JSON_MAX];

/****************************************************************************
 * 小工具
 ****************************************************************************/

static uint32_t rest_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)((uint64_t)ts.tv_sec * 1000u +
                    (uint64_t)ts.tv_nsec / 1000000u);
}

static const char *rest_state_name(int state)
{
  switch (state)
    {
      case REST_STATE_QUIET: return "quiet";
      case REST_STATE_NIGHT: return "night";
      default:               return "awake";
    }
}

/* 当前本地时间 + 年份校验。失败返回 false，epoch 不写。
 * 这个树里 time() 存的就是本地时间（CONFIG_LIBC_LOCALTIME 没开），
 * localtime_r 只是把 time_t 拆成 tm；和 pedometer.c 的用法一致。 */
static bool rest_now(struct tm *tmv, uint32_t *epoch)
{
  time_t t = time(NULL);
  int    year;

  if (t <= (time_t)0 || localtime_r(&t, tmv) == NULL)
    {
      return false;
    }

  year = tmv->tm_year + 1900;
  if (year < REST_CLOCK_MIN_YEAR || year > REST_CLOCK_MAX_YEAR)
    {
      return false;
    }

  if ((uint64_t)t > 0xffffffffu)
    {
      return false;
    }

  if (epoch != NULL)
    {
      *epoch = (uint32_t)t;
    }

  return true;
}

/* 校验一个已经存下来的 epoch 的年份。不依赖"现在"。 */
static bool rest_epoch_ok(uint32_t epoch)
{
  time_t   t = (time_t)epoch;
  struct tm tmv;
  int      year;

  if (t <= (time_t)0 || localtime_r(&t, &tmv) == NULL)
    {
      return false;
    }

  year = tmv.tm_year + 1900;
  return year >= REST_CLOCK_MIN_YEAR && year <= REST_CLOCK_MAX_YEAR;
}

static bool rest_json_u32(const cJSON *obj, const char *key, uint32_t *out)
{
  const cJSON *item;
  double       v;

  if (obj == NULL || key == NULL || out == NULL)
    {
      return false;
    }

  item = cJSON_GetObjectItem(obj, key);
  if (!cJSON_IsNumber(item))
    {
      return false;
    }

  v = item->valuedouble;

  /* 顺手挡住 NaN：!(a && b) 对 NaN 成立；小数也拒绝，
   * 时间戳/时长必须是整数。 */
  if (!(v >= 0.0 && v <= 4294967295.0))
    {
      return false;
    }

  if ((double)(uint32_t)v != v)
    {
      return false;
    }

  *out = (uint32_t)v;
  return true;
}

/* 一段记录是否可信。dur_s 必须等于 end-start，范围 [45min, 24h]，
 * 两端年份都在 2024..2027。 */
static bool rest_record_valid(uint32_t start, uint32_t end, uint32_t dur_s)
{
  if (start == 0 || end <= start)
    {
      return false;
    }

  if (dur_s != (end - start))
    {
      return false;
    }

  if (dur_s < REST_NIGHT_MIN_RECORD_S || dur_s > REST_NIGHT_MAX_S)
    {
      return false;
    }

  return rest_epoch_ok(start) && rest_epoch_ok(end);
}

static bool rest_parse_record(const cJSON *obj, struct rest_session_s *out)
{
  uint32_t start;
  uint32_t end;
  uint32_t dur_s;

  if (obj == NULL || out == NULL)
    {
      return false;
    }

  if (!rest_json_u32(obj, "start", &start) ||
      !rest_json_u32(obj, "end", &end) ||
      !rest_json_u32(obj, "dur_s", &dur_s))
    {
      return false;
    }

  if (!rest_record_valid(start, end, dur_s))
    {
      return false;
    }

  out->start = start;
  out->end   = end;
  out->dur_s = dur_s;
  return true;
}

static bool rest_session_exists(uint32_t start, uint32_t end)
{
  int i;

  for (i = 0; i < s_session_count; i++)
    {
      if (s_sessions[i].start == start && s_sessions[i].end == end)
        {
          return true;
        }
    }

  return false;
}

/****************************************************************************
 * 持久化
 *
 * 文件很小，且只在夜间段结束时写一次。启动时读一次；坏条目逐条丢弃，
 * 绝不因为坏文件崩溃，也不把坏数据继续写回去。
 ****************************************************************************/

static void rest_store_load(void)
{
  cJSON *root;
  cJSON *item;
  cJSON *arr;
  struct rest_session_s rec;
  int    n;
  int    i;
  int    count;

  pthread_mutex_lock(&s_data_lock);

  s_session_count    = 0;
  s_last_night_valid = false;

  n = health_store_load(REST_STORE_NAME, s_load_buf, sizeof(s_load_buf));
  if (n < 0)
    {
      syslog(LOG_INFO, "%s store load: no usable file (%d), start empty\n",
             TAG, n);
      pthread_mutex_unlock(&s_data_lock);
      return;
    }

  root = cJSON_Parse(s_load_buf);
  if (root == NULL)
    {
      syslog(LOG_WARNING,
             "%s store load: bad json (%d bytes), start empty\n", TAG, n);
      pthread_mutex_unlock(&s_data_lock);
      return;
    }

  item = cJSON_GetObjectItem(root, "last_night");
  if (item != NULL && cJSON_IsObject(item))
    {
      if (rest_parse_record(item, &rec))
        {
          s_last_night       = rec;
          s_last_night_valid = true;
        }
      else
        {
          syslog(LOG_WARNING,
                 "%s store load: invalid last_night dropped\n", TAG);
        }
    }
  else if (item != NULL && !cJSON_IsNull(item))
    {
      syslog(LOG_WARNING, "%s store load: last_night is not an object\n",
             TAG);
    }

  arr = cJSON_GetObjectItem(root, "sessions");
  if (arr != NULL && cJSON_IsArray(arr))
    {
      count = cJSON_GetArraySize(arr);
      for (i = 0; i < count && s_session_count < REST_MAX_SESSIONS; i++)
        {
          cJSON *entry = cJSON_GetArrayItem(arr, i);

          if (!rest_parse_record(entry, &rec))
            {
              syslog(LOG_WARNING,
                     "%s store load: session[%d] invalid, dropped\n", TAG, i);
              continue;
            }

          if (rest_session_exists(rec.start, rec.end))
            {
              continue;
            }

          s_sessions[s_session_count++] = rec;
        }
    }

  /* 保持 last_night == sessions[0]。文件被手改成不一致时以 sessions 为准；
   * 只有 last_night、没有 sessions 的旧文件则补一条。 */
  if (s_session_count > 0)
    {
      s_last_night       = s_sessions[0];
      s_last_night_valid = true;
    }
  else if (s_last_night_valid)
    {
      s_sessions[0]  = s_last_night;
      s_session_count = 1;
    }

  syslog(LOG_INFO, "%s store loaded: last_night=%d sessions=%d\n",
         TAG, s_last_night_valid ? 1 : 0, s_session_count);

  cJSON_Delete(root);
  pthread_mutex_unlock(&s_data_lock);
}

/* 写一段夜间休息。RAM 一定先更新；序列化/落盘失败只 warning，
 * 下一段成功时会把 RAM 里的全部 sessions 一起写下去。返回 0。 */
static int rest_store_record(uint32_t start, uint32_t end, uint32_t dur_s)
{
  struct rest_session_s rec;
  cJSON *root;
  cJSON *last;
  cJSON *arr;
  char  *txt;
  int    ret = 0;
  int    i;

  rec.start = start;
  rec.end   = end;
  rec.dur_s = dur_s;

  pthread_mutex_lock(&s_data_lock);

  if (s_session_count >= REST_MAX_SESSIONS)
    {
      s_session_count = REST_MAX_SESSIONS - 1;   /* 挤掉最旧的一条 */
    }

  memmove(&s_sessions[1], &s_sessions[0],
          (size_t)s_session_count * sizeof(s_sessions[0]));

  s_sessions[0] = rec;
  s_session_count++;
  s_last_night       = rec;
  s_last_night_valid = true;

  root = cJSON_CreateObject();
  if (root == NULL)
    {
      pthread_mutex_unlock(&s_data_lock);
      syslog(LOG_WARNING,
             "%s store json alloc failed (segment kept in RAM)\n", TAG);
      return 0;
    }

  last = cJSON_CreateObject();
  if (last != NULL)
    {
      cJSON_AddNumberToObject(last, "start", (double)start);
      cJSON_AddNumberToObject(last, "end",   (double)end);
      cJSON_AddNumberToObject(last, "dur_s", (double)dur_s);
      cJSON_AddItemToObject(root, "last_night", last);
    }

  arr = cJSON_AddArrayToObject(root, "sessions");
  for (i = 0; arr != NULL && i < s_session_count; i++)
    {
      cJSON *entry = cJSON_CreateObject();

      if (entry == NULL)
        {
          break;
        }

      cJSON_AddNumberToObject(entry, "start", (double)s_sessions[i].start);
      cJSON_AddNumberToObject(entry, "end",   (double)s_sessions[i].end);
      cJSON_AddNumberToObject(entry, "dur_s", (double)s_sessions[i].dur_s);
      cJSON_AddItemToArray(arr, entry);
    }

  txt = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  /* 先把数据锁放掉再写 flash：UI 每秒读 last_night，不能被一次
   * littlefs 擦写卡住。txt 已经和 sessions 无关，写盘期间不会再变。 */
  pthread_mutex_unlock(&s_data_lock);

  if (txt == NULL)
    {
      syslog(LOG_WARNING,
             "%s store json print failed (segment kept in RAM)\n", TAG);
      return 0;
    }

  ret = health_store_save(REST_STORE_NAME, txt);
  free(txt);

  if (ret < 0)
    {
      syslog(LOG_WARNING,
             "%s store save failed: %d (segment kept in RAM)\n", TAG, ret);
    }

  return 0;
}

/****************************************************************************
 * 状态机
 ****************************************************************************/

/* 收到一段夜间数据。end==0 表示"不知道什么时候结束"（IMU 掉了、时钟坏了），
 * 这种段宁可丢，也不能拿 now 硬凑一个结束时间。 */
static void rest_finish_night(uint32_t end, const char *why)
{
  uint32_t start = s_night_start;
  uint32_t dur;

  s_night_start = 0;

  if (start == 0)
    {
      syslog(LOG_WARNING, "%s night segment (%s): no start, dropped\n",
             TAG, why);
      return;
    }

  if (!s_night_clock_ok || end == 0)
    {
      syslog(LOG_WARNING,
             "%s night segment dropped (%s): bad clock start=%u end=%u\n",
             TAG, why, start, end);
      return;
    }

  if (end <= start)
    {
      syslog(LOG_WARNING,
             "%s night segment dropped (%s): end<=start start=%u end=%u\n",
             TAG, why, start, end);
      return;
    }

  dur = end - start;

  if (dur < REST_NIGHT_MIN_RECORD_S)
    {
      syslog(LOG_INFO,
             "%s night segment too short dur=%us (%s), not recorded\n",
             TAG, dur, why);
      return;
    }

  if (dur > REST_NIGHT_MAX_S || !rest_epoch_ok(start) || !rest_epoch_ok(end))
    {
      syslog(LOG_WARNING,
             "%s night segment dropped (%s): bad range/clock "
             "start=%u end=%u dur=%us\n",
             TAG, why, start, end, dur);
      return;
    }

  (void)rest_store_record(start, end, dur);

  syslog(LOG_INFO,
         "%s night rest recorded dur=%us start=%u end=%u "
         "(motion-only estimate)\n",
         TAG, dur, start, end);
}

static void rest_enter(int new_state, uint32_t now, uint32_t still,
                       bool clock_ok, int hour, int minute)
{
  int old = s_state;

  if (new_state == old)
    {
      return;
    }

  if (new_state == REST_STATE_NIGHT)
    {
      s_night_start    = now;
      s_night_clock_ok = clock_ok;
    }

  s_state = new_state;

  if (new_state == REST_STATE_QUIET)
    {
      syslog(LOG_INFO, "%s %s->quiet begin still=%us\n",
             TAG, rest_state_name(old), still);
    }
  else if (new_state == REST_STATE_NIGHT)
    {
      syslog(LOG_INFO, "%s %s->night begin at %02d:%02d still=%us\n",
             TAG, rest_state_name(old), hour, minute, still);
    }
  else
    {
      syslog(LOG_INFO, "%s %s->awake still=%us\n",
             TAG, rest_state_name(old), still);
    }
}

/* 一轮 60 秒状态推进。 */
static void rest_tick(void)
{
  struct tm tmv;
  uint32_t  now = 0;
  uint32_t  still;
  bool      alive = pedometer_is_alive();
  bool      clock_ok;
  bool      night_window;
  int       want;

  memset(&tmv, 0, sizeof(tmv));
  clock_ok = rest_now(&tmv, &now);
  still    = alive ? pedometer_get_still_seconds() : 0;

  /* ① 体动锁存优先：结束当前段。结束时间用锁存到的体动时刻，
   *    不是 tick 时刻，这样 dur_s 不会多算最多 1 分钟。 */
  if (s_motion_latch)
    {
      uint32_t end = s_motion_epoch;
      int      old = s_state;

      if (end == 0 && now != 0)
        {
          end = now;
        }

      if (old == REST_STATE_NIGHT)
        {
          rest_finish_night(end, "motion");
        }

      if (old != REST_STATE_AWAKE)
        {
          s_state = REST_STATE_AWAKE;
          syslog(LOG_INFO, "%s %s->awake (motion) still=%us\n",
                 TAG, rest_state_name(old), still);
        }

      s_motion_latch = false;
      s_motion_epoch = 0;

      /* 这一 tick 已经确认有体动，不再用同一份 still 去尝试进入休息；
       * 下一 tick 重新评估。 */
      return;
    }

  /* ② IMU 掉了：不能拿 now 当结束时间，直接丢段。 */
  if (!alive)
    {
      if (s_state == REST_STATE_NIGHT)
        {
          rest_finish_night(0, "imu_lost");
          s_state = REST_STATE_AWAKE;
          syslog(LOG_WARNING,
                 "%s night->awake (imu lost), segment not recorded\n", TAG);
        }
      else if (s_state == REST_STATE_QUIET)
        {
          s_state = REST_STATE_AWAKE;
          syslog(LOG_WARNING, "%s quiet->awake (imu lost)\n", TAG);
        }

      s_motion_latch = false;
      s_motion_epoch = 0;
      return;
    }

  /* ③ still 被重置但没锁存到体动：只可能是数据源重启。能确定的结束
   *    时刻是"最后一次体动"（now - still）；拿不到就丢。 */
  if (still < REST_QUIET_MIN_S)
    {
      if (s_state != REST_STATE_AWAKE)
        {
          int      old = s_state;
          uint32_t end = (now > still) ? (now - still) : 0;

          if (old == REST_STATE_NIGHT)
            {
              rest_finish_night(end, "still_reset");
            }

          s_state = REST_STATE_AWAKE;
          syslog(LOG_WARNING, "%s %s->awake (still reset to %us)\n",
                 TAG, rest_state_name(old), still);
        }

      s_motion_latch = false;
      s_motion_epoch = 0;
      return;
    }

  /* ④ 连续静止够久：按本地小时决定 QUIET / NIGHT。时钟不可信时只敢
   *    进 QUIET，绝不凭 2036 或 1970 的小时数声称"夜间"。 */
  night_window = clock_ok &&
                 (tmv.tm_hour >= REST_NIGHT_START_HOUR ||
                  tmv.tm_hour < REST_NIGHT_END_HOUR);
  want = night_window ? REST_STATE_NIGHT : REST_STATE_QUIET;

  if (s_state == REST_STATE_AWAKE)
    {
      rest_enter(want, now, still, clock_ok, tmv.tm_hour, tmv.tm_min);
    }
  else if (s_state == REST_STATE_QUIET && want == REST_STATE_NIGHT)
    {
      rest_enter(REST_STATE_NIGHT, now, still, clock_ok,
                 tmv.tm_hour, tmv.tm_min);
    }
  else if (s_state == REST_STATE_NIGHT && want == REST_STATE_QUIET)
    {
      /* 08:00 到了但人还没动：夜间窗口结束，这一段按窗口末结束。 */
      rest_finish_night(now, "window_end");
      rest_enter(REST_STATE_QUIET, now, still, clock_ok,
                 tmv.tm_hour, tmv.tm_min);
    }
}

/****************************************************************************
 * 体动轮询 / 线程
 ****************************************************************************/

/* 每 5 秒读一次，只做体动锁存。pedometer_get_still_seconds() 是单调时钟
 * 算出来的，所以 now-still 就是最后一次体动的本地 epoch（±1 秒）。 */
static void rest_poll_motion(void)
{
  uint32_t still;
  uint32_t active;

  if (!pedometer_is_alive())
    {
      return;
    }

  still  = pedometer_get_still_seconds();
  active = pedometer_get_active_seconds();

  if (active > s_active_prev)
    {
      struct tm tmv;
      uint32_t  now = 0;

      s_motion_latch = true;
      s_motion_epoch = rest_now(&tmv, &now) ? now : 0;
    }

  s_active_prev = active;

  if (still < REST_MOTION_STILL_S)
    {
      struct tm tmv;
      uint32_t  now = 0;

      s_motion_latch = true;

      if (rest_now(&tmv, &now) && now > still)
        {
          s_motion_epoch = now - still;
        }
      else
        {
          s_motion_epoch = 0;
        }
    }
}

static void *rest_worker(void *arg)
{
  uint32_t last_tick;
  uint32_t now;

  (void)arg;

  s_active_prev = pedometer_get_active_seconds();
  last_tick     = rest_now_ms();

  syslog(LOG_INFO, "%s worker started: tick=%ds poll=%ds\n",
         TAG, REST_TICK_SEC, REST_POLL_SEC);

  rest_tick();

  while (s_run)
    {
      sleep(REST_POLL_SEC);

      if (!s_run)
        {
          break;
        }

      rest_poll_motion();

      now = rest_now_ms();
      if ((now - last_tick) >= (uint32_t)(REST_TICK_SEC * 1000))
        {
          last_tick = now;
          rest_tick();
        }
    }

  syslog(LOG_INFO, "%s worker stopped\n", TAG);
  return NULL;
}

/****************************************************************************
 * 公开接口
 ****************************************************************************/

int rest_track_start(void)
{
  pthread_attr_t attr;
  pthread_t      tid;
  int            ret;

  pthread_mutex_lock(&s_start_lock);

  if (s_started)
    {
      pthread_mutex_unlock(&s_start_lock);
      return 0;
    }

  if (!s_store_loaded)
    {
      rest_store_load();
      s_store_loaded = true;
    }

  /* pedometer_start 是幂等的；这里再调一次，保证工具/页面单独先跑时
   * 也有数据源。打不开 /dev/mpu0 不阻止追踪线程（状态保持 awake）。 */
  if (pedometer_start() != 0)
    {
      syslog(LOG_WARNING,
             "%s pedometer unavailable, tracking stays awake\n", TAG);
    }

  s_run = true;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, REST_TASK_STACK);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  ret = pthread_create(&tid, &attr, rest_worker, NULL);
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      s_run = false;
      syslog(LOG_ERR, "%s pthread_create failed: %d\n", TAG, ret);
      pthread_mutex_unlock(&s_start_lock);
      return -1;
    }

  s_started = true;

  syslog(LOG_INFO,
         "%s started: quiet_min=%us night_min=%us window=%02d:00-%02d:00\n",
         TAG, REST_QUIET_MIN_S, REST_NIGHT_MIN_RECORD_S,
         REST_NIGHT_START_HOUR, REST_NIGHT_END_HOUR);

  pthread_mutex_unlock(&s_start_lock);
  return 0;
}

int rest_track_get_state(void)
{
  return (int)s_state;
}

uint32_t rest_track_current_quiet_s(void)
{
  if (!pedometer_is_alive())
    {
      return 0;
    }

  return pedometer_get_still_seconds();
}

int rest_track_get_last_night(uint32_t *start, uint32_t *end, uint32_t *dur_s)
{
  int ret = 0;

  pthread_mutex_lock(&s_data_lock);

  if (s_last_night_valid)
    {
      if (start != NULL)
        {
          *start = s_last_night.start;
        }

      if (end != NULL)
        {
          *end = s_last_night.end;
        }

      if (dur_s != NULL)
        {
          *dur_s = s_last_night.dur_s;
        }

      ret = 1;
    }

  pthread_mutex_unlock(&s_data_lock);
  return ret;
}

#else /* !CONFIG_AI_AGENT_LVGL_UI */

/* 这个包的 CMake 把 pedometer.c 放在 UI 构建里。没有 UI 时给出可链接的
 * 空实现，工具层会返回 "rest tracker not enabled"。 */

int rest_track_start(void)
{
  return 0;
}

int rest_track_get_state(void)
{
  return REST_STATE_AWAKE;
}

uint32_t rest_track_current_quiet_s(void)
{
  return 0;
}

int rest_track_get_last_night(uint32_t *start, uint32_t *end, uint32_t *dur_s)
{
  (void)start;
  (void)end;
  (void)dur_s;
  return 0;
}

#endif /* CONFIG_AI_AGENT_LVGL_UI */
