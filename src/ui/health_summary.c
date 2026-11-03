/****************************************************************************
 * health_summary.c - 每日健康摘要卡片（步数 / 心率 / 休息 / 天气）
 *
 * 什么时候说话：
 *   本地时间 >= 08:00 且 summary.json 里的 last 不是今天 -> 生成一句中文
 *   摘要，用桌宠气泡展示一次（silent：不 TTS，避免大清早突然说话），然后
 *   把 last 写回今天。同一份判据同时覆盖三种场景：
 *     - 开机时已经过了 08:00（开机补卡）；
 *     - 一直开机，运行中跨过 08:00；
 *     - 跨天（00:00 不触发，08:00 之后 last != 今天 -> 新的一张卡）。
 *   展示失败也写 last（并告警），否则每 60 s 一次会刷屏；写盘失败时
 *   RAM 里还有 s_attempt_date 再守一道。
 *
 * 数据从哪来（全部读现成缓存，本模块不发任何网络请求）：
 *   昨日步数  health/pedometer.c   pedometer_get_history_json()
 *   24h 心率  health/hr_measure.c  hr_measure_history_snapshot()
 *   昨夜休息  health_store_load("rest")  -> {"last_night":{"dur_s":N},...}
 *   天气      infra/weather_cache    weather_cache_get()
 *
 * 诚实性红线：任一数据源没有数据就把整个子句省略，绝不补 0 / 编造；
 * 四个子句全空时给一句"今天还没有足够数据做健康小结"。
 *
 * 时间：本构建 CONFIG_LIBC_LOCALTIME 没开，time() 里存的就是本地时间
 * （time_sync.c 把本地时间写进 RTC），所以 localtime_r() 拿到的 tm 就是
 * 墙上时间，和计步器/表盘一致。年份不在 2024..2027 时视为未对时：跳过，
 * 并且不写 last —— 等对时成功后下一轮 tick 自己会补上。
 *
 * 线程：一条 detached 线程，12 KB 栈，每 60 s tick 一次，不做任何阻塞
 * IO 的同步等待（文件读都是小 JSON，读不到就当没有）。LVGL 是 LV_OS_NONE，
 * 但 lvgl_ui_channel_send_silent() 内部只做加锁入队，任何线程可调。
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

#include "ui/health_summary.h"

#include "health/hr_measure.h"
#include "health/pedometer.h"
#include "infra/health_store.h"
#include "infra/weather_cache.h"
#include "ui/lvgl_ui_channel.h"
#include "ui/pet_display.h"

#define TAG "health_summary"

#define HS_TRIGGER_HOUR      8      /* 08:00 之后才展示"每日小结" */
#define HS_PERIOD_S          60     /* tick 间隔 */
#define HS_FIRST_DELAY_S     5      /* 开机先让 UI/对时/天气缓存就位 */
#define HS_STACK_SIZE        (12 * 1024)
#define HS_UI_WAIT_MAX       30     /* UI 通道还没起来：最多等 30 个 tick */
#define HS_MIN_YEAR          2024   /* 时钟可信区间：明显异常就不说话 */
#define HS_MAX_YEAR          2027
#define HS_HR_WINDOW_S       (24 * 3600)   /* 心率只统计最近 24 h */
#define HS_HR_SANE_MIN       30            /* 与 hr_measure 的兜底区间同源 */
#define HS_HR_SANE_MAX       220
#define HS_REST_MAX_S        (24 * 3600)   /* 超过一天的"昨夜"必然不是昨夜 */
/* 与 ui/sleep_page.c 的 LAST_NIGHT_MAX_AGE_S 同一条纪律：超过 24 h 的记录
 * 盘上还在（工具能查），但卡片上写"昨夜"就是编数据。 */
#define HS_REST_MAX_AGE_S    (24 * 3600)
#define HS_REST_JSON_MAX     4096          /* rest.json: last_night + sessions */
#define HS_STEPS_JSON_MAX    768           /* pedometer 的 s_json_buf 上限 512 */
#define HS_STORE_JSON_MAX    384           /* summary.json: last + 200B text */
#define HS_WEATHER_STALE_S   (48 * 3600)   /* 两整天没更新过的天气不叫"今天" */
#define HS_CLOCK_WARN_S      600           /* 时钟异常日志限频 */

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static bool            s_started;

/* 只有 summary 线程读写（start() 不碰），因此不需要锁。 */
static char   s_attempt_date[11];   /* 本轮开机已经为哪一天尝试过展示 */
static int    s_ui_wait_ticks;      /* UI 通道未就绪的重试计数 */
static time_t s_last_clock_warn;    /* 时钟告警限频（MONOTONIC 秒） */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static time_t hs_mono_s(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      {
        return 0;
      }

    return ts.tv_sec;
}

/* 时钟异常时留一行日志，但 10 分钟最多一次：60 s tick 不能刷屏。 */
static void hs_clock_warn(const char *why)
{
    time_t now = hs_mono_s();

    if (now != 0 && now - s_last_clock_warn < HS_CLOCK_WARN_S)
      {
        return;
      }

    s_last_clock_warn = now;
    syslog(LOG_WARNING, "[%s] clock not usable (%s), summary skipped\n",
           TAG, why != NULL ? why : "?");
}

/* "YYYY-MM-DD" 形状校验（长度 + 数字 + 两个连字符）。 */
static bool hs_date_shape_ok(const char *date)
{
    int i;

    if (date == NULL || strlen(date) != 10)
      {
        return false;
      }

    for (i = 0; i < 10; i++)
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

/* "YYYY-MM-DD" 里的月/日，给"10月30日走了 N 步"用。 */
static void hs_month_day(const char *date, int *month, int *day)
{
    *month = (date[5] - '0') * 10 + (date[6] - '0');
    *day   = (date[8] - '0') * 10 + (date[9] - '0');
}

static const char *hs_greeting(int hour)
{
    if (hour >= 5 && hour < 11)
      {
        return "早上好！";
      }

    if (hour >= 11 && hour < 18)
      {
        return "下午好！";
      }

    return "晚上好！";
}

/* 整段追加：放不下就整句放弃，绝不留下半个汉字。 */
static void hs_append(char *out, size_t cap, const char *text)
{
    size_t used = strlen(out);
    size_t add  = strlen(text);

    if (used + add + 1 > cap)
      {
        return;
      }

    memcpy(out + used, text, add + 1);
}

/* 兜底截断到 max_bytes，并回退到 UTF-8 字符边界。 */
static void hs_clip_utf8(char *text, size_t max_bytes)
{
    size_t len = strlen(text);

    if (len <= max_bytes)
      {
        return;
      }

    len = max_bytes;
    while (len > 0 && ((unsigned char)text[len] & 0xc0) == 0x80)
      {
        len--;
      }

    text[len] = '\0';
}

/* WMO weather code -> 中文（只用 ui_font_cjk_18 里确定有的字）。 */
static const char *hs_weather_desc(int code)
{
    if (code == 0)
      {
        return "晴";
      }

    if (code == 1 || code == 2)
      {
        return "多云";
      }

    if (code == 3)
      {
        return "阴";
      }

    if (code == 45 || code == 48)
      {
        return "雾";
      }

    if (code >= 51 && code <= 67)
      {
        return "雨";
      }

    if (code >= 71 && code <= 77)
      {
        return "雪";
      }

    if (code >= 80 && code <= 82)
      {
        return "阵雨";
      }

    if (code >= 95 && code <= 99)
      {
        return "雷雨";
      }

    return "";
}

/****************************************************************************
 * 各数据源采集（缺数据就保持 ok=false，绝不补 0）
 ****************************************************************************/

static void hs_collect_steps(health_summary_data_t *d, const char *yesterday)
{
    char buf[HS_STEPS_JSON_MAX];
    cJSON *root;
    cJSON *history;
    cJSON *item;
    cJSON *jdate;
    cJSON *jsteps;
    const char *best = NULL;
    double best_steps = 0.0;
    bool exact = false;
    int count;
    int i;

    if (yesterday == NULL || yesterday[0] == '\0')
      {
        return;
      }

    if (pedometer_get_history_json(buf, sizeof(buf)) < 0)
      {
        syslog(LOG_WARNING, "[%s] step history unavailable, clause omitted\n",
               TAG);
        return;
      }

    root = cJSON_Parse(buf);
    if (root == NULL)
      {
        syslog(LOG_WARNING, "[%s] step history parse failed, clause omitted\n",
               TAG);
        return;
      }

    history = cJSON_GetObjectItemCaseSensitive(root, "history");
    count = cJSON_IsArray(history) ? cJSON_GetArraySize(history) : 0;

    /* history 是"旧的在前、新的在后"，但这里按日期取最大
     * （< 今天）而不是靠位置，避免文件被写乱时取错。 */
    for (i = 0; i < count; i++)
      {
        const char *date;

        item = cJSON_GetArrayItem(history, i);
        jdate = cJSON_GetObjectItemCaseSensitive(item, "date");
        jsteps = cJSON_GetObjectItemCaseSensitive(item, "steps");
        if (!cJSON_IsString(jdate) || !cJSON_IsNumber(jsteps))
          {
            continue;
          }

        date = cJSON_GetStringValue(jdate);
        if (!hs_date_shape_ok(date))
          {
            continue;
          }

        /* 今天/未来（时钟被往回调过）不算"昨天"。
         * 今天的实时步数在 pedometer 顶层 steps 里，不重复计。 */
        if (strcmp(date, d->date) >= 0)
          {
            continue;
          }

        if (strcmp(date, yesterday) == 0)
          {
            best = date;
            best_steps = cJSON_GetNumberValue(jsteps);
            exact = true;
            break;
          }

        if (best == NULL || strcmp(date, best) > 0)
          {
            best = date;
            best_steps = cJSON_GetNumberValue(jsteps);
          }
      }

    if (best != NULL)
      {
        if (best_steps < 0.0)
          {
            best_steps = 0.0;
          }
        else if (best_steps > 99999999.0)
          {
            best_steps = 99999999.0;
          }

        d->steps_ok = true;
        d->steps_is_yesterday = exact;
        d->steps = (int)best_steps;
        snprintf(d->steps_date, sizeof(d->steps_date), "%s", best);
      }

    if (d->steps_ok && !d->steps_is_yesterday)
      {
        /* 没有昨天那条：用最近一次旧记录，并在文案里把日期说出来。 */
        syslog(LOG_INFO, "[%s] no step record for %s, using %s\n",
               TAG, yesterday, d->steps_date);
      }

    cJSON_Delete(root);
}

static void hs_collect_hr(health_summary_data_t *d, time_t now)
{
    uint32_t ts[HR_MEASURE_HISTORY_CAP];
    int bpm[HR_MEASURE_HISTORY_CAP];
    int count = 0;
    int min_bpm = 0;
    int max_bpm = 0;
    int n;
    int i;

    n = hr_measure_history_snapshot(ts, bpm, HR_MEASURE_HISTORY_CAP);
    if (n <= 0)
      {
        return;     /* 还没有任何测量：整句省略 */
      }

    for (i = 0; i < n; i++)
      {
        int64_t age;

        if (ts[i] == 0 || bpm[i] < HS_HR_SANE_MIN || bpm[i] > HS_HR_SANE_MAX)
          {
            continue;
          }

        age = (int64_t)now - (int64_t)ts[i];
        if (age < 0 || age > (int64_t)HS_HR_WINDOW_S)
          {
            continue;
          }

        if (count == 0 || bpm[i] < min_bpm)
          {
            min_bpm = bpm[i];
          }

        if (count == 0 || bpm[i] > max_bpm)
          {
            max_bpm = bpm[i];
          }

        count++;
      }

    if (count == 0)
      {
        return;
      }

    d->hr_ok = true;
    d->hr_min = min_bpm;
    d->hr_max = max_bpm;
    d->hr_count = count;
}

static void hs_collect_rest(health_summary_data_t *d, time_t now)
{
    char *buf;
    cJSON *root;
    cJSON *night;
    cJSON *jdur;
    cJSON *jend;
    double dur;
    uint64_t end;
    int n;

    /* 用堆而不是栈：rest.json 未来可能带多条 sessions，4 KB 放栈上会把
     * 12 KB 线程的余量吃掉一大截。 */
    buf = malloc(HS_REST_JSON_MAX);
    if (buf == NULL)
      {
        return;
      }

    n = health_store_load("rest", buf, HS_REST_JSON_MAX);
    if (n < 0)
      {
        free(buf);
        return;     /* 没有 rest.json 不是错误：兄弟任务可能还没落盘 */
      }

    root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL)
      {
        syslog(LOG_WARNING, "[%s] rest.json parse failed, clause omitted\n",
               TAG);
        return;
      }

    night = cJSON_GetObjectItemCaseSensitive(root, "last_night");
    jdur = cJSON_IsObject(night)
               ? cJSON_GetObjectItemCaseSensitive(night, "dur_s")
               : NULL;
    jend = cJSON_IsObject(night)
               ? cJSON_GetObjectItemCaseSensitive(night, "end")
               : NULL;

    /* 记录太旧 / 在未来（时钟被往回拨）都不能叫"昨夜"：和睡眠页的
     * last_night_recent() 保持同一口径。end 缺失时不做时效判断（老文件）。 */
    end = (cJSON_IsNumber(jend) && cJSON_GetNumberValue(jend) > 0.0)
              ? (uint64_t)cJSON_GetNumberValue(jend)
              : 0;

    if (end != 0 && now > 0 &&
        ((uint64_t)now < end || (uint64_t)now - end > HS_REST_MAX_AGE_S))
      {
        syslog(LOG_INFO, "[%s] rest.json last_night end=%lu not last night, "
               "clause omitted\n", TAG, (unsigned long)end);
        cJSON_Delete(root);
        return;
      }

    if (cJSON_IsNumber(jdur) && cJSON_GetNumberValue(jdur) > 0.0 &&
        cJSON_GetNumberValue(jdur) <= (double)HS_REST_MAX_S)
      {
        dur = cJSON_GetNumberValue(jdur);
        d->rest_ok = true;
        d->rest_s = (int)(dur + 0.5);
      }
    else if (jdur != NULL)
      {
        syslog(LOG_WARNING, "[%s] rest.json last_night.dur_s invalid, "
               "clause omitted\n", TAG);
      }

    cJSON_Delete(root);
}

static void hs_collect_weather(health_summary_data_t *d, time_t now)
{
    weather_cache_s wc;
    const char *desc;

    if (!weather_cache_get(&wc) || !wc.valid)
      {
        return;     /* 没有过成功数据：整句省略，不在开机时抢发请求 */
      }

    /* stamp==0 表示写方没给时间（测试/旧数据）：不做时效判断。
     * 有 stamp 时，超过两整天的"今天"显然不是今天，宁可不说。 */
    if (wc.stamp > 0 && now > wc.stamp &&
        (now - wc.stamp) > HS_WEATHER_STALE_S)
      {
        syslog(LOG_INFO, "[%s] weather cache is %ld s old, clause omitted\n",
               TAG, (long)(now - wc.stamp));
        return;
      }

    d->temp_c = (int)(wc.temp_c + (wc.temp_c >= 0.0f ? 0.5f : -0.5f));
    d->weather_code = wc.code;

    desc = hs_weather_desc(wc.code);
    snprintf(d->weather_desc, sizeof(d->weather_desc), "%s",
             desc != NULL ? desc : "");

    d->weather_ok = true;
}

/****************************************************************************
 * 文案
 ****************************************************************************/

void health_summary_text(const health_summary_data_t *d, char *out, size_t cap)
{
    char clause[4][64];
    const char *clauses[4];
    int n = 0;
    int i;

    if (out == NULL || cap == 0)
      {
        return;
      }

    out[0] = '\0';

    if (d == NULL)
      {
        snprintf(out, cap, "今天还没有足够数据做健康小结。");
        return;
      }

    hs_append(out, cap, hs_greeting(d->hour));

    if (d->steps_ok)
      {
        if (d->steps_is_yesterday)
          {
            snprintf(clause[n], sizeof(clause[n]), "昨天走了 %d 步", d->steps);
          }
        else
          {
            int month = 0;
            int day = 0;

            hs_month_day(d->steps_date, &month, &day);
            snprintf(clause[n], sizeof(clause[n]), "%d月%d日走了 %d 步",
                     month, day, d->steps);
          }

        clauses[n] = clause[n];
        n++;
      }

    if (d->hr_ok)
      {
        if (d->hr_min == d->hr_max)
          {
            snprintf(clause[n], sizeof(clause[n]), "心率 %d", d->hr_min);
          }
        else
          {
            snprintf(clause[n], sizeof(clause[n]), "心率 %d~%d",
                     d->hr_min, d->hr_max);
          }

        clauses[n] = clause[n];
        n++;
      }

    if (d->rest_ok)
      {
        if (d->rest_s >= 3600)
          {
            snprintf(clause[n], sizeof(clause[n]), "昨夜休息 %d时%d分",
                     d->rest_s / 3600, (d->rest_s % 3600) / 60);
          }
        else if (d->rest_s >= 60)
          {
            snprintf(clause[n], sizeof(clause[n]), "昨夜休息 %d分",
                     d->rest_s / 60);
          }
        else
          {
            snprintf(clause[n], sizeof(clause[n]), "昨夜休息不到1分钟");
          }

        clauses[n] = clause[n];
        n++;
      }

    if (d->weather_ok)
      {
        if (d->weather_desc[0] != '\0')
          {
            snprintf(clause[n], sizeof(clause[n]), "今天 %d度 %s",
                     d->temp_c, d->weather_desc);
          }
        else
          {
            snprintf(clause[n], sizeof(clause[n]), "今天 %d度", d->temp_c);
          }

        clauses[n] = clause[n];
        n++;
      }

    if (n == 0)
      {
        hs_append(out, cap, "今天还没有足够数据做健康小结。");
        hs_clip_utf8(out, HEALTH_SUMMARY_TEXT_MAX - 1);
        return;
      }

    for (i = 0; i < n; i++)
      {
        if (i > 0)
          {
            hs_append(out, cap, "，");
          }

        hs_append(out, cap, clauses[i]);
      }

    hs_append(out, cap, "。");
    hs_clip_utf8(out, HEALTH_SUMMARY_TEXT_MAX - 1);
}

/****************************************************************************
 * 采集（公开 API）
 ****************************************************************************/

int health_summary_collect(health_summary_data_t *d)
{
    struct tm tmv;
    struct tm ymv;
    char yesterday[11];
    time_t ynow;
    time_t now;
    int year;

    if (d == NULL)
      {
        return -EINVAL;
      }

    memset(d, 0, sizeof(*d));

    now = time(NULL);
    if (now <= 0 || localtime_r(&now, &tmv) == NULL)
      {
        return -EINVAL;
      }

    year = tmv.tm_year + 1900;
    if (year < HS_MIN_YEAR || year > HS_MAX_YEAR)
      {
        return -EINVAL;
      }

    if (strftime(d->date, sizeof(d->date), "%Y-%m-%d", &tmv) == 0)
      {
        d->date[0] = '\0';
        return -EINVAL;
      }

    d->hour = tmv.tm_hour;

    /* 昨天 = 本地时间减 86400 s。本板 RTC 存的就是本地时间，中国无夏令时，
     * 直接减一天是安全的（和 pedometer 的 localtime 口径一致）。 */
    yesterday[0] = '\0';
    ynow = now - (time_t)86400;
    if (localtime_r(&ynow, &ymv) == NULL ||
        strftime(yesterday, sizeof(yesterday), "%Y-%m-%d", &ymv) == 0)
      {
        yesterday[0] = '\0';
      }

    hs_collect_steps(d, yesterday);
    hs_collect_hr(d, now);
    hs_collect_rest(d, now);
    hs_collect_weather(d, now);

    return 0;
}

/****************************************************************************
 * summary.json（{"last":...,"text":...}）
 ****************************************************************************/

/* 读 summary.json 的 last。文件不存在/坏 JSON/字段非法都返回 false。 */
static bool hs_store_last_date(char *out, size_t cap)
{
    char buf[HS_STORE_JSON_MAX];
    cJSON *root;
    cJSON *jlast;
    bool ok = false;

    if (out == NULL || cap == 0)
      {
        return false;
      }

    out[0] = '\0';

    if (health_store_load(HEALTH_SUMMARY_STORE_NAME, buf, sizeof(buf)) < 0)
      {
        return false;   /* 第一次开机：没有文件不是错误 */
      }

    root = cJSON_Parse(buf);
    if (root == NULL)
      {
        /* 半写/坏文件当"没有记录"：最多本轮开机多展示一次。 */
        syslog(LOG_WARNING, "[%s] summary.json parse failed, "
               "treat as no record\n", TAG);
        return false;
      }

    jlast = cJSON_GetObjectItemCaseSensitive(root, "last");
    if (cJSON_IsString(jlast))
      {
        const char *date = cJSON_GetStringValue(jlast);

        if (hs_date_shape_ok(date))
          {
            snprintf(out, cap, "%s", date);
            ok = true;
          }
      }

    cJSON_Delete(root);
    return ok;
}

/* 写 {"last":date} 或 {"last":date,"text":text}。 */
static int hs_store_mark(const char *date, const char *text)
{
    char raw[HS_STORE_JSON_MAX];
    cJSON *root;
    char *json;
    int ret;

    root = cJSON_CreateObject();
    if (root == NULL)
      {
        return -ENOMEM;
      }

    cJSON_AddStringToObject(root, "last", date);
    if (text != NULL)
      {
        cJSON_AddStringToObject(root, "text", text);
      }

    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json == NULL)
      {
        return -ENOMEM;
      }

    if (strlen(json) >= sizeof(raw))
      {
        syslog(LOG_WARNING, "[%s] summary.json too long (%u bytes)\n",
               TAG, (unsigned)strlen(json));
        free(json);
        return -ENOSPC;
      }

    snprintf(raw, sizeof(raw), "%s", json);
    free(json);

    ret = health_store_save(HEALTH_SUMMARY_STORE_NAME, raw);
    if (ret < 0)
      {
        syslog(LOG_WARNING, "[%s] save summary.json failed: %d\n", TAG, ret);
        return ret;
      }

    return 0;
}

/****************************************************************************
 * 上屏
 ****************************************************************************/

int health_summary_build_json(char *out, size_t cap)
{
    health_summary_data_t data;
    char text[HEALTH_SUMMARY_TEXT_MAX];
    cJSON *root;
    cJSON *hr;
    cJSON *weather;
    char *json;

    if (out == NULL || cap == 0)
      {
        return -EINVAL;
      }

    out[0] = '\0';

    if (health_summary_collect(&data) != 0)
      {
        snprintf(out, cap, "{\"error\":\"clock not set\"}");
        return -EINVAL;
      }

    health_summary_text(&data, text, sizeof(text));

    root = cJSON_CreateObject();
    if (root == NULL)
      {
        snprintf(out, cap, "{\"error\":\"out of memory\"}");
        return -ENOMEM;
      }

    cJSON_AddStringToObject(root, "date", data.date);
    cJSON_AddNumberToObject(root, "steps_yesterday", (double)data.steps);
    cJSON_AddStringToObject(root, "steps_source",
                            !data.steps_ok ? "none"
                                           : (data.steps_is_yesterday
                                                  ? "yesterday"
                                                  : "latest"));
    if (data.steps_ok)
      {
        cJSON_AddStringToObject(root, "steps_date", data.steps_date);
      }

    if (data.hr_ok)
      {
        hr = cJSON_AddObjectToObject(root, "hr");
        if (hr == NULL)
          {
            cJSON_Delete(root);
            snprintf(out, cap, "{\"error\":\"out of memory\"}");
            return -ENOMEM;
          }

        cJSON_AddNumberToObject(hr, "min", (double)data.hr_min);
        cJSON_AddNumberToObject(hr, "max", (double)data.hr_max);
        cJSON_AddNumberToObject(hr, "count", (double)data.hr_count);
      }
    else
      {
        cJSON_AddNullToObject(root, "hr");
      }

    if (data.rest_ok)
      {
        cJSON_AddNumberToObject(root, "rest_s", (double)data.rest_s);
      }
    else
      {
        cJSON_AddNullToObject(root, "rest_s");
      }

    if (data.weather_ok)
      {
        weather = cJSON_AddObjectToObject(root, "weather");
        if (weather == NULL)
          {
            cJSON_Delete(root);
            snprintf(out, cap, "{\"error\":\"out of memory\"}");
            return -ENOMEM;
          }

        cJSON_AddNumberToObject(weather, "temp_c", (double)data.temp_c);
        cJSON_AddNumberToObject(weather, "code",
                                (double)data.weather_code);
        if (data.weather_desc[0] != '\0')
          {
            cJSON_AddStringToObject(weather, "desc", data.weather_desc);
          }
      }
    else
      {
        cJSON_AddNullToObject(root, "weather");
      }

    cJSON_AddStringToObject(root, "text", text);

    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL)
      {
        snprintf(out, cap, "{\"error\":\"serialize failed\"}");
        return -ENOMEM;
      }

    if (strlen(json) >= cap)
      {
        free(json);
        snprintf(out, cap, "{\"error\":\"output too small\"}");
        return -ENOSPC;
      }

    snprintf(out, cap, "%s", json);
    free(json);
    return 0;
}

/****************************************************************************
 * tick / 线程
 ****************************************************************************/

static void hs_tick(void)
{
    health_summary_data_t data;
    char today[11];
    char last[11];
    char text[HEALTH_SUMMARY_TEXT_MAX];
    struct tm tmv;
    time_t now;
    int year;
    int rc;

    now = time(NULL);
    if (now <= 0 || localtime_r(&now, &tmv) == NULL)
      {
        hs_clock_warn("time() unavailable");
        return;
      }

    year = tmv.tm_year + 1900;
    if (year < HS_MIN_YEAR || year > HS_MAX_YEAR)
      {
        /* 未对时/时钟跑飞：跳过，并且不写 last。 */
        hs_clock_warn("year out of range");
        return;
      }

    if (strftime(today, sizeof(today), "%Y-%m-%d", &tmv) == 0)
      {
        hs_clock_warn("strftime failed");
        return;
      }

    /* 08:00 之前不打扰（这条判据同时覆盖开机补卡 / 跨 08:00 / 跨天）。 */
    if (tmv.tm_hour < HS_TRIGGER_HOUR)
      {
        return;
      }

    /* 本轮开机已经试过今天：summary.json 写失败时靠它兜底，不再刷屏，
     * 也不再每分钟读一次盘。 */
    if (strcmp(s_attempt_date, today) == 0)
      {
        return;
      }

    /* 今天已经展示过（跨重启也认）：什么都不做。 */
    if (hs_store_last_date(last, sizeof(last)) &&
        strcmp(last, today) == 0)
      {
        return;
      }

    if (health_summary_collect(&data) != 0)
      {
        hs_clock_warn("collect rejected");
        return;
      }

    health_summary_text(&data, text, sizeof(text));

    rc = lvgl_ui_channel_send_silent(text, PET_EMOTION_HAPPY);

    if (rc == -EINVAL && s_ui_wait_ticks < HS_UI_WAIT_MAX)
      {
        /* LVGL 通道还没起来（例如 UI 启动比本线程慢）：60 s 后重试，
         * 最多等 30 个 tick，绝不因为一次 -EINVAL 就把今天判死刑。 */
        s_ui_wait_ticks++;
        syslog(LOG_INFO, "[%s] UI channel not ready (rc=%d), retry %d/%d\n",
               TAG, rc, s_ui_wait_ticks, HS_UI_WAIT_MAX);
        return;
      }

    snprintf(s_attempt_date, sizeof(s_attempt_date), "%s", today);

    if (rc == 0)
      {
        /* 成功（已入队）：日志留痕 + 落盘，下一轮 tick 不再触发。 */
        syslog(LOG_INFO, "[%s] generated: %s\n", TAG, text);
        if (hs_store_mark(today, text) != 0)
          {
            syslog(LOG_WARNING, "[%s] summary.json not saved, "
                   "RAM guard keeps today quiet\n", TAG);
          }
      }
    else
      {
        /* 失败也写 last：避免每 60 s 重试刷屏。 */
        syslog(LOG_WARNING, "[%s] display failed rc=%d, text=%s "
               "(marking %s done)\n", TAG, rc, text, today);
        (void)hs_store_mark(today, NULL);
      }
}

static void *hs_task(void *arg)
{
    (void)arg;

    /* 开机风暴里先让 UI / 对时 / 天气缓存就位，晚 5 秒再开始 tick。 */
    sleep(HS_FIRST_DELAY_S);

    for (;;)
      {
        hs_tick();
        sleep(HS_PERIOD_S);
      }

    return NULL;
}

int health_summary_start(void)
{
    pthread_attr_t attr;
    pthread_t tid;
    int ret;

    pthread_mutex_lock(&s_lock);
    if (s_started)
      {
        pthread_mutex_unlock(&s_lock);
        return 0;
      }

    s_started = true;   /* 先占位：两个调用方同时进来也只建一条线程 */
    pthread_mutex_unlock(&s_lock);

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, HS_STACK_SIZE);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    ret = pthread_create(&tid, &attr, hs_task, NULL);
    pthread_attr_destroy(&attr);

    if (ret != 0)
      {
        pthread_mutex_lock(&s_lock);
        s_started = false;
        pthread_mutex_unlock(&s_lock);

        syslog(LOG_ERR, "[%s] thread create failed: %d\n", TAG, ret);
        return -ret;
      }

    syslog(LOG_INFO, "[%s] started (stack %d, tick %d s, trigger %02d:00)\n",
           TAG, HS_STACK_SIZE, HS_PERIOD_S, HS_TRIGGER_HOUR);
    return 0;
}
