/****************************************************************************
 * health_summary.h - 每日健康摘要卡片（步数 / 心率 / 休息 / 天气）
 *
 * 一句话：开机后（或运行中跨过 08:00）用桌宠气泡展示一次"昨天+昨夜+今天"
 * 的中文小结，同一天只展示一次；结果落盘到
 * /data/ai_agent/health/summary.json：
 *
 *   {"last":"YYYY-MM-DD","text":"..."}
 *
 * 数据全部来自现成缓存，本模块**不发任何网络请求**：
 *   昨日步数  health/pedometer.c   pedometer_get_history_json()
 *   24h 心率  health/hr_measure.c  hr_measure_history_snapshot()
 *   昨夜休息  health_store_load("rest")  (兄弟任务 D 写入的 rest.json)
 *   天气      infra/weather_cache    weather_cache_get()
 *
 * 诚实性：任一数据源没有数据 -> 整个子句省略，绝不补 0 / 编造。
 * 四个子句全空时给一句"今天还没有足够数据做健康小结"。
 *
 * 线程：health_summary_start() 起一条 detached 线程（12 KB 栈），每 60 s
 * tick 一次，不阻塞启动；任何线程都可以调 build_json/collect（内部不加锁，
 * 依赖各数据源自己的互斥）。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef AI_AGENT_HEALTH_SUMMARY_H
#define AI_AGENT_HEALTH_SUMMARY_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 摘要文本缓冲上限（含结尾 '\0'）：文本本身 <= 200 字节，
 *  保证一行桌宠气泡放得下（气泡自身在 132 字节处截断）。 */
#define HEALTH_SUMMARY_TEXT_MAX 201

/** 摘要状态在 health_store 里的名字（/data/ai_agent/health/summary.json）。 */
#define HEALTH_SUMMARY_STORE_NAME "summary"

/** 一次采集到的摘要原料。缺数据的子项 ok=false，文案生成时整句省略。 */
typedef struct
{
  char  date[11];         /* 今天（本地）"YYYY-MM-DD" */
  int   hour;             /* 本地小时，决定"早上好/下午好/晚上好" */

  bool  steps_ok;         /* 找到了可用的历史步数 */
  int   steps;            /* 步数（steps_ok=false 时恒为 0） */
  char  steps_date[11];   /* 步数对应的日期 */
  bool  steps_is_yesterday; /* true=就是昨天；false=用的是最近一条旧记录 */

  bool  hr_ok;            /* 最近 24 h 内有有效心率 */
  int   hr_min;
  int   hr_max;
  int   hr_count;

  bool  rest_ok;          /* rest.json 里有合法且不超过 24h 的 last_night */
  int   rest_s;           /* 昨夜休息秒数 */

  bool  weather_ok;       /* 天气缓存里有过成功数据 */
  int   temp_c;           /* 四舍五入到整度（本板不做浮点格式化） */
  int   weather_code;
  char  weather_desc[12]; /* "晴"/"多云"/"阴"/"雾"/"雨"/"雪"/"阵雨"/"雷雨" */
} health_summary_data_t;

/**
 * 启动每日摘要线程（幂等）。
 *
 * 触发：本地小时 >= 8 且 summary.json 的 last != 今天（开机补卡与
 * 运行中跨 08:00 / 跨天走的是同一条判据）。展示走
 * lvgl_ui_channel_send_silent(text, PET_EMOTION_HAPPY)：只上气泡 + 表情，
 * 不 TTS（避免开机突然说话）。展示成功写回 {"last","text"}；展示失败
 * 只写回 last 并告警，避免每 60 s 刷屏。时钟年份不在 2024..2027 时
 * 跳过且**不写 last**（等对时后补卡）。
 *
 * @return 0 线程已启动/已在运行；负 errno 创建线程失败。
 */
int health_summary_start(void);

/**
 * 采集一次摘要原料（实时，不读 summary.json）。
 *
 * @return 0 成功；-EINVAL 时钟不可信（time() 失败或年份超范围）。
 */
int health_summary_collect(health_summary_data_t *out);

/**
 * 把采集结果渲染成一句中文（<= HEALTH_SUMMARY_TEXT_MAX-1 字节）。
 * 纯函数，不发 IO、不写盘；out 保证以 '\0' 结尾（cap>0 时）。
 */
void health_summary_text(const health_summary_data_t *data, char *out,
                         size_t cap);

/**
 * 实时生成与上屏同源的 JSON（不要求今天已经展示过）：
 *
 *   {"date":"YYYY-MM-DD",
 *    "steps_yesterday":N,            // 没有历史时为 0（配合 steps_source）
 *    "steps_source":"yesterday|latest|none",
 *    "steps_date":"YYYY-MM-DD",      // steps_source != none 时才有
 *    "hr":{"min":..,"max":..,"count":..} | null,
 *    "rest_s":N | null,
 *    "weather":{"temp_c":..,"code":..,"desc":".."} | null,
 *    "text":"..."}
 *
 * 失败时 out 里是 {"error":"..."}，返回负 errno（工具层转 ERROR）。
 */
int health_summary_build_json(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_HEALTH_SUMMARY_H */
