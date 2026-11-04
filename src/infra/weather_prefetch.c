/****************************************************************************
 * weather_prefetch.c - 天气后台定时预取（30 分钟一轮）
 *
 * 为什么要有它：
 *   天气页的 10 分钟缓存只有在页面活着的时候才有机会更新。页面一关，
 *   数据就冻在离开那一刻 —— 早起第一次进天气页看到的经常是昨晚的温度，
 *   还得站在那儿等一次 TLS 往返。这里让后台每 30 分钟拉一次，写进
 *   infra/weather_cache（天气页 / get_weather 工具看同一份），于是：
 *     - 进页面多数情况直接命中缓存，一次 HTTPS 都不发（"打开即最新"）；
 *     - 语音问天气时 get_weather 的 Open-Meteo 路径也能先吃到这份数据。
 *
 * 三条纪律：
 *   1. **绝不阻塞启动**：agent_main 只调 weather_prefetch_start()，真正的
 *      网络等待全在线程里；建线程失败也只是少一个优化，不影响其他功能。
 *   2. **没网不算失败**：每 5 s 探一次、一轮最多等 120 s，等不到就下一轮
 *      接着等 —— 不拉取、不 mark_fail（否则 last_ok 会被"还没开机连上"
 *      这种东西打成 false，调试时全是假信号）。
 *   3. **失败不重试、不刷屏**：一次拉取失败只打一行 log，30 分钟后再来。
 *      天气是低速数据，抢重试只会和语音/LLM 的 TLS 抢 IOB。
 *
 * 不设线程优先级（继承创建者的），也不改调度策略：它 30 分钟才干一次活，
 * 每次几秒，没有理由和 UI/音频抢 CPU。栈 16 KB，与天气页 worker 同量
 * （TLS + cJSON）。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <pthread.h>
#include <stdbool.h>
#include <syslog.h>
#include <unistd.h>

#include "infra/network_manager.h"
#include "infra/weather_cache.h"
#include "infra/weather_prefetch.h"

#define TAG "weather_prefetch"

#define PF_PERIOD_S   (30 * 60)    /* 拉取间隔：30 分钟 */
#define PF_NET_POLL_S 5            /* 等网络时的轮询间隔（秒） */
#define PF_NET_WAIT_S 120          /* 单轮最多等网络 120 s，超时就下一轮 */
#define PF_STACK      (16 * 1024)  /* TLS + cJSON */
#define PF_FIRST_DELAY_S 10        /* 开机首个预取延后 10 s（避开联网风暴） */
#define PF_FAIL_RETRY_S  300       /* 拉取失败后 5 分钟重试（而不是等 30 分钟） */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool            g_started;

/** 现在有没有可用上行。
 *
 *  CONFIG_AI_AGENT_NET_RPMSG 构建（本板就是）里 network_get_state() 是
 *  "真·上行"判据：network_manager.c 的 iface_poll 线程每 5 s 刷新一次，
 *  综合 4G(ppp0) > bt-pan > BLE GATT 三条通道。没有这个配置（QEMU 等）
 *  时只有通用的 network_is_connected()。
 *
 *  ★ 两个判据取"或"，不是取"与"：状态机的刷新依赖 network_wifi_reconnect()
 *    → network_rpmsg_init() 起的那条轮询线程（agent_main 的 network_watch
 *    线程里调），它比本线程晚一步；某些通道也可能已经能上网而状态机还没
 *    刷新。预取线程一旦永远等不到 CONNECTED 就变成了"存在但什么也不做"，
 *    那是最难查的失败方式 —— 宁可多试一次拉取（失败只 log + mark_fail，
 *    30 分钟后再来），也不要静默不干活。 */
static bool pf_net_ready(void)
{
#ifdef CONFIG_AI_AGENT_NET_RPMSG
    if (network_get_state() == NET_STATE_CONNECTED)
      {
        return true;
      }
#endif

    return network_is_connected();
}

/** 等网络可用：每 PF_NET_POLL_S 秒探一次，最多 wait_s 秒。
 *  @return true = 现在有网。等满仍没有网不是错误，只是"这轮跳过"。 */
static bool pf_wait_network(int wait_s)
{
    int waited = 0;

    while (waited < wait_s)
      {
        if (pf_net_ready())
          {
            return true;
          }

        sleep(PF_NET_POLL_S);
        waited += PF_NET_POLL_S;
      }

    /* 睡满以后再看一眼：最后这一觉睡完可能刚好连上 */
    return pf_net_ready();
}

/** 预取线程主体：等网络 → 拉一次 → 睡 30 分钟 → 循环。 */
static void *pf_task(void *arg)
{
    bool first = true;

    (void)arg;

    for (;;)
      {
        char            desc[32];
        weather_cache_s wc;
        int             rc;

        if (!pf_wait_network(PF_NET_WAIT_S))
          {
            /* 一轮等满还没网：下一轮接着等。pf_wait_network() 自己就会睡，
             * 所以这里 continue 不会变成空转。 */
            syslog(LOG_INFO, "[%s] network not ready after %d s, retry\n",
                   TAG, PF_NET_WAIT_S);
            continue;
          }

        if (first)
          {
            /* 12:40 真机首个版本：网络状态刚变 CONNECTED 就立刻发 TLS，
             * 与 time_sync / feishu / ws 的建连撞在一起，首次预取直接失败。
             * 先等 10 s 让开机联网风暴过去，失败后再 5 分钟重试。 */
            syslog(LOG_INFO, "[%s] network ready, first fetch in 10 s\n", TAG);
            first = false;
            sleep(PF_FIRST_DELAY_S);
          }

        desc[0] = '\0';
        rc = tool_weather_fetch_now(desc, sizeof(desc));

        if (rc == OK)
          {
            /* 成功时 tool_weather_fetch_now() 已经写过共享缓存了，
             * 这里把结果读回来只是为了让日志带上一行温度/天气码。
             * 温度用整数打印：本板不做浮点格式化。 */
            if (weather_cache_get(&wc))
              {
                int t10   = (int)(wc.temp_c * 10.0f +
                                  (wc.temp_c >= 0.0f ? 0.5f : -0.5f));
                int t_abs = t10 < 0 ? -t10 : t10;

                syslog(LOG_INFO,
                       "[%s] fetch ok temp=%s%d.%d C, code %d, %s\n",
                       TAG, t10 < 0 ? "-" : "", t_abs / 10, t_abs % 10,
                       wc.code, desc[0] != '\0' ? desc : "?");
              }
            else
              {
                syslog(LOG_INFO, "[%s] fetch ok\n", TAG);
              }
          }
        else
          {
            /* 失败只 log：tool_weather_fetch_now() 已经 mark_fail() 过了，
             * 这里不做退避、不重试，30 分钟后照常再来。 */
            syslog(LOG_WARNING, "[%s] fetch failed, retry in %d s\n",
                   TAG, PF_FAIL_RETRY_S);
          }

        sleep(rc == OK ? PF_PERIOD_S : PF_FAIL_RETRY_S);
      }

    return NULL;
}

int weather_prefetch_start(void)
{
    pthread_attr_t attr;
    pthread_t      tid;
    int            ret;

    pthread_mutex_lock(&g_lock);

    if (g_started)
      {
        pthread_mutex_unlock(&g_lock);
        syslog(LOG_INFO, "[%s] already started\n", TAG);
        return OK;
      }

    g_started = true;   /* 先占位：两个调用方同时进来也只会建一个线程 */

    pthread_mutex_unlock(&g_lock);

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, PF_STACK);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    ret = pthread_create(&tid, &attr, pf_task, NULL);
    pthread_attr_destroy(&attr);

    if (ret != 0)
      {
        /* 建线程失败不是致命错误：天气页自己还有 wx_worker，功能退化但不残。
         * 清掉占位，允许以后再试一次。 */
        pthread_mutex_lock(&g_lock);
        g_started = false;
        pthread_mutex_unlock(&g_lock);

        syslog(LOG_ERR, "[%s] thread create failed: %d\n", TAG, ret);
        return ERROR;
      }

    syslog(LOG_INFO, "[%s] started (every %d s, stack %d)\n",
           TAG, PF_PERIOD_S, PF_STACK);
    return OK;
}
