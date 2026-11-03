/****************************************************************************
 * weather_cache.h - 进程级共享天气缓存（北京 Open-Meteo）
 *
 * 三条写路径、一处读路径：
 *   写  ui/weather_page.c        天气页 worker 拉到数据   source = "page"
 *   写  tools/tool_web_search.c  get_weather 的 Open-Meteo source = "tool"
 *   写  infra/weather_prefetch.c 后台定时预取（30 分钟）   source = "prefetch"
 *   读  ui/weather_page.c        进页面先看它：新鲜就直接渲染，一次请求都不发
 *
 * 为什么要有它：天气页原来只有页面自己的 static 缓存（wx_cache_t），别的
 * 模块都看不到 —— 页面关着的那段时间里没有任何人更新它，于是"进页面看到
 * 的往往是几小时前的温度"。改成进程级单例以后，谁拉到都算数：后台预取
 * 拉到的、语音问天气时 get_weather 拉到的，天气页进页面都能直接用。
 *
 * 约束（刻意保持简单）：
 *   - 无内存分配：一个 static struct + 一把 pthread_mutex；
 *   - get 在锁内做**整结构体拷贝**，调用方拿到一份快照，不会被写方撕裂；
 *   - source 只给日志/调试看，不参与任何判断；
 *   - 不做 TTL 判断（TTL 是各调用方的策略，天气页用 600 s），这里只回答
 *     "有没有过成功数据" 和 "距离上次成功多久"。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef AI_AGENT_WEATHER_CACHE_H
#define AI_AGENT_WEATHER_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
  bool   valid;       /* 有过成功数据 */
  bool   last_ok;     /* 最近一次拉取是否成功 */
  float  temp_c;
  int    code;        /* open-meteo weather code */
  float  wind_kmh;
  time_t stamp;       /* 成功时间 */
  char   source[16];  /* "page" / "tool" / "prefetch"（调试用） */
} weather_cache_s;

/** 清空缓存（只清数据，不重建锁）。开机调一次；静态零初始化本来就等于
 *  "没有数据"，所以不调也不会读到脏值。 */
void weather_cache_init(void);

/** 记一次**成功**的拉取：valid/last_ok 置位、写入数值、stamp = now。
 *  @param source 只用于日志，建议 "page" / "tool" / "prefetch"；
 *                NULL 或空串写成 "?"。
 *  数据是低速变化的，后到的覆盖先到的即可（不做新旧比较）。 */
void weather_cache_update(float temp_c, int code, float wind_kmh, const char *source);

/** 记一次**失败**的拉取：只把 last_ok 打 false，valid/数据/stamp 一律不动
 *  —— 上次成功的数据仍然可以展示（天气页的"刷新失败继续显示旧数据"靠它）。
 *  开机以来一次都没成功过时也安全（valid 本来就是 false）。 */
void weather_cache_mark_fail(void);

/** 取一份快照。
 *  @return true = 有过成功数据（out->valid），false = 没有。
 *  无论返回什么都会把当前内容写进 *out（没有数据时是零值），
 *  调用方可以借此看 last_ok / source。允许 out == NULL（只问有没有）。 */
bool weather_cache_get(weather_cache_s *out);

/** 距离上次成功多少秒。
 *  @return >= 0 = 秒数；-1 = 开机以来从没成功过。
 *  ★ 返回值也可能是负数（-1 以外）：本板时钟会被 SNTP 往回调，这时
 *    "现在 - 成功时刻" 为负。调用方按"已过期"处理（天气页就是这么做的：
 *    宁可多拉一次，也不拿错时间糊弄人）。 */
int weather_cache_age_s(void);

/****************************************************************************
 * 天气拉取（实现在 tools/tool_web_search.c）
 *
 * 后台预取线程复用它，避免第二份 Open-Meteo 网络+解析代码。
 * 放在这个头文件里是因为 tools/tool_web_search.h 不归本模块改；声明与
 * 实现同处一个编译单元，签名不一致时编译器直接就报错了。
 ****************************************************************************/

/** 拉一次北京天气（Open-Meteo，免 key），成功时写共享缓存 source="prefetch"。
 *
 *  @param desc_out 可选。成功填中文描述（"晴"/"多云"/"雷雨"…），给日志用；
 *                  失败置空串。NULL 表示不要。
 *  @param cap      desc_out 的字节容量。
 *  @return OK / ERROR。失败只 mark_fail + 返回，不重试（节奏由调用方定）。
 */
int tool_weather_fetch_now(char *desc_out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_WEATHER_CACHE_H */
