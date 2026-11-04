/****************************************************************************
 * weather_cache.c - 进程级共享天气缓存（实现）
 *
 * 见 weather_cache.h 的说明。这里只有一把互斥锁 + 一个 static 结构体，
 * 没有队列、没有分配、没有线程 —— 谁拉到谁写，谁要看谁拷一份走。
 *
 * 日志里**不做浮点格式化**（%.1f）：本板 syslog 的浮点路径会踩栈，天气页
 * 文件头记过这个坑。温度/风速都先乘 10 取整再打印。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <pthread.h>
#include <string.h>
#include <syslog.h>

#include "infra/weather_cache.h"

#define TAG "wx_cache"

/* 静态零初始化 = "没有数据"，所以开机不调 weather_cache_init() 也安全 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static weather_cache_s g_cache;

void weather_cache_init(void)
{
    pthread_mutex_lock(&g_lock);

    memset(&g_cache, 0, sizeof(g_cache));
    strncpy(g_cache.source, "none", sizeof(g_cache.source) - 1);

    pthread_mutex_unlock(&g_lock);

    syslog(LOG_INFO, "[%s] ready (shared weather cache)\n", TAG);
}

void weather_cache_update(float temp_c, int code, float wind_kmh,
                          const char *source)
{
    const char *src = (source != NULL && source[0] != '\0') ? source : "?";
    char src_copy[sizeof(g_cache.source)];
    int t10;
    int w10;
    int t_abs;

    pthread_mutex_lock(&g_lock);

    g_cache.valid    = true;
    g_cache.last_ok  = true;
    g_cache.temp_c   = temp_c;
    g_cache.code     = code;
    g_cache.wind_kmh = wind_kmh;
    g_cache.stamp    = time(NULL);

    memset(g_cache.source, 0, sizeof(g_cache.source));
    strncpy(g_cache.source, src, sizeof(g_cache.source) - 1);

    /* 日志用的副本在锁内取：解锁之后再读全局就成了数据竞争，
     * 而且可能打出别的线程刚写进去的 source。 */
    memcpy(src_copy, g_cache.source, sizeof(src_copy));

    pthread_mutex_unlock(&g_lock);

    /* 乘 10 取整再打印：避开本板的浮点格式化（见文件头） */
    t10   = (int)(temp_c * 10.0f + (temp_c >= 0.0f ? 0.5f : -0.5f));
    w10   = (int)(wind_kmh * 10.0f + 0.5f);
    t_abs = t10 < 0 ? -t10 : t10;

    syslog(LOG_INFO, "[%s] update from %s: %s%d.%d C, code %d, wind %d.%d km/h\n",
           TAG, src_copy, t10 < 0 ? "-" : "", t_abs / 10, t_abs % 10,
           code, w10 / 10, w10 % 10);
}

void weather_cache_mark_fail(void)
{
    bool had_data;

    pthread_mutex_lock(&g_lock);

    had_data = g_cache.valid;
    g_cache.last_ok = false;   /* valid / 数值 / stamp 一律不动 */

    pthread_mutex_unlock(&g_lock);

    syslog(LOG_WARNING, "[%s] fetch failed (has previous data: %s)\n",
           TAG, had_data ? "yes" : "no");
}

bool weather_cache_get(weather_cache_s *out)
{
    weather_cache_s snap;

    pthread_mutex_lock(&g_lock);
    snap = g_cache;            /* 整结构体拷贝：没有指针成员，安全 */
    pthread_mutex_unlock(&g_lock);

    if (out != NULL)
      {
        *out = snap;
      }

    return snap.valid;
}

int weather_cache_age_s(void)
{
    time_t stamp;
    bool   valid;

    pthread_mutex_lock(&g_lock);

    valid = g_cache.valid;
    stamp = g_cache.stamp;

    pthread_mutex_unlock(&g_lock);

    if (!valid)
      {
        return -1;
      }

    return (int)(time(NULL) - stamp);
}
