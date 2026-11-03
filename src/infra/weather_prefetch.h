/****************************************************************************
 * weather_prefetch.h - 天气后台定时预取
 *
 * 起一个 detached 线程：先等网络可用，然后每 30 分钟拉一次北京天气写进
 * infra/weather_cache。天气页进页面时读同一份缓存，于是"打开即最新"，
 * 而且这一次拉取是**后台**做的 —— 不占用户等待时间，也不占 UI 线程。
 *
 * 线程自己等网络（每 5 s 探一次、一轮最多 120 s），所以调用方不需要先
 * network_wait_connected()；启动路径不会被它阻塞。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef AI_AGENT_WEATHER_PREFETCH_H
#define AI_AGENT_WEATHER_PREFETCH_H

#ifdef __cplusplus
extern "C" {
#endif

/** 启动预取线程。幂等：重复调用只会在第一次真正创建线程。
 *
 *  @return 0 (OK) = 线程在跑（含"本来就在跑"）；-1 (ERROR) = 创建失败。
 *          创建失败后可以再调一次重试（失败不会把"已启动"状态卡死）。
 */
int weather_prefetch_start(void);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_WEATHER_PREFETCH_H */
