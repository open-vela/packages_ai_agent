/****************************************************************************
 * weather_page.h - 天气页（北京，Open-Meteo）
 *
 * 数据来自 Open-Meteo 的 current_weather（免 API key，返回 JSON），
 * 城市固定北京 —— 手表没有定位，坐标是写死的。
 *
 * 取数是异步的：页面自己起一个 worker 线程做 HTTPS + JSON 解析，
 * 结果经互斥锁交给 UI，lv_timer 每 200ms 取走一次。
 * 绝不能在 LVGL 渲染线程里同步取数（一次请求最坏几十秒）。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef AI_AGENT_WEATHER_PAGE_H
#define AI_AGENT_WEATHER_PAGE_H

#include <lvgl/lvgl.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 创建天气页并返回页面对象。重复调用返回同一实例。
 *  缓存未过期（10 分钟内）直接用缓存渲染；过期或无缓存才发起请求
 *  （后台线程，不阻塞界面）。「刷新」按钮无条件重取。 */
lv_obj_t *weather_page_create(void);

/** 销毁页面、刷新定时器，并停掉 worker 线程。导航切页时调用。 */
void weather_page_delete(void);

/** 只读访问天气的模块级缓存：拿"曾经成功拉到过"的温度/天气码。
 *
 *  这份缓存跨页面生命周期存在（页面销毁不清），表盘天气卡这类
 *  外部页面用它显示一个"至少有过"的值。只回答有没有成功数据，
 *  不做 10 分钟 TTL 判断 —— 卡片显示旧温度好过 "--"，要不要
 *  重新取数是天气页自己进页时的事。
 *
 *  返回 true  = 命中，*temp_c / *code 已填（两个出参都允许传
 *               NULL，表示只要其中一个）；
 *  返回 false = 开机以来一次都没成功过，出参保持不动。 */
bool weather_page_get_cached(float *temp_c, int *code);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_WEATHER_PAGE_H */
