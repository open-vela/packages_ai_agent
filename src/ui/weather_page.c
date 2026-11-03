/****************************************************************************
 * weather_page.c - 天气页（北京，Open-Meteo）
 *
 *   ┌──────────────────────────┐
 *   │ 天气            85% [▮]  │  statusbar
 *   │        18.4 °C          │  88px 数字 + 手绘度符号
 *   │          多云            │  天气描述
 *   │           ☁             │  96px 天气图标（染 UI_COLOR_WEATHER）
 *   │  ┌────────┐ ┌────────┐  │  两张 168x80 小卡
 *   │  │ 风速    │ │ 更新    │  │
 *   │  │12.6km/h│ │ 14:30  │  │
 *   │  └────────┘ └────────┘  │
 *   │  更新于 14:30            │  新鲜度状态行（缓存过期改警示色）
 *   │        [ 刷新 ]          │  胶囊按钮
 *   └──────────────────────────┘
 *
 * 数据源：
 *   https://api.open-meteo.com/v1/forecast
 *     ?latitude=39.9042&longitude=116.4074&current_weather=true
 *   取 current_weather.temperature / .weathercode / .windspeed。
 *   ★ 这个接口的 windspeed 单位是 **km/h**（Open-Meteo 文档），
 *     所以界面照实标 km/h，别写成 m/s。
 *
 * 为什么必须用后台线程：
 *   vela_https_get 最坏要 15 秒连接超时 + 30 秒读超时
 *   （infra/network_manager.c 的 network_get_*_timeout）。
 *   LVGL 只有一条渲染线程，在里面同步取数 = 整块屏幕卡死几十秒。
 *   所以 HTTP 和解析都在 worker 线程里做，结果写进带互斥锁的共享结构；
 *   worker **从不碰任何 lv_obj**，UI 侧 200ms 的 lv_timer 只负责
 *   "取走结果 → 改标签"。
 *
 * 三种状态不能糊在一起：
 *   获取中 —— 大字 "--"，描述行 "获取中…"
 *   成功   —— 温度 + 描述 + 天气图标 + 风速/更新两张小卡
 *   失败   —— 描述行 "获取失败"，下一行给具体原因（HTTP 码 / 超时 / 解析失败）
 *
 * 失败还要再分两种（加了缓存以后）：
 *   有旧缓存 —— 保留旧温度/风速继续显示，状态行写"离线缓存 · N 小时前
 *              · 刷新失败"（年龄 >= 10 分钟时）。整页不变成错误页。
 *   没旧缓存 —— 就是上面的错误页。
 *
 * ★ 缓存年龄（2026-11-03）：只写"更新于 HH:MM"不够 —— 用户看不出
 *   这份数据是 3 分钟前还是 3 小时前。现在按 age = time(NULL) - s_cache.stamp
 *   分档：刚刚 / N 分钟前 / N 小时前；年龄 >= WX_CACHE_TTL(10 分钟) 时
 *   状态行改成"离线缓存 · 3 小时前"并用警示色，绝不谎称"刚刚更新"。
 *
 * 缓存（用户反馈"天气获取比较慢，而且每次都刷新"）：
 *   取到的数据除了交给 UI，还写进文件作用域的 static 缓存，**页面销毁不清**。
 *   10 分钟内（WX_CACHE_TTL）再进页面直接渲染缓存，一次 HTTPS 都不发；
 *   只有缓存过期或压根没有，才起 worker 去拉。
 *   底部「刷新」是用户主动行为，无条件重拉，不看 TTL。
 *   这样反复进出 20 次页面，最多只会有 1 次请求（10 分钟内）。
 *
 * 共享缓存（2026-10-31：让"打开即最新"）：
 *   上面那份 s_cache 只有"本页拉成功"才会更新 —— 页面关着的几个小时里
 *   没人动它，所以早起第一次进页面看到的常是昨晚的温度。现在多了一份
 *   进程级缓存（infra/weather_cache），写它的有三条路径：
 *     本页 worker 成功（"page"）、get_weather 工具的 Open-Meteo（"tool"）、
 *     后台预取线程每 30 分钟一次（"prefetch"）。
 *   进页面先看它：
 *     valid 且 age < 600s → 直接渲染，一次请求都不发（多数情况走这条）
 *     valid 但已过期     → 先摆出旧数据（"离线缓存 · N 前"），同时后台刷新
 *     没有数据           → 照旧 wx_request()
 *   "刷新"按钮仍然无条件重拉。
 *
 * 字体上的两个坑（都实测过，别踩）：
 *   1. ui_font_cjk_24（UI_FONT_BODY/TITLE）是按"当前 UI 源码里出现过的字"
 *      烘焙的，总共 347 个字形，没有「北京多云晴雨雪雷获取失败刷新」这些字；
 *      本构建又开着 CONFIG_LV_USE_FONT_PLACEHOLDER，缺字会画成方块 □。
 *      所以本页中文一律用 UI_FONT_SMALL（= ui_font_cjk_18，GB2312 全集，
 *      已验证上面这些字都在）。要换成 24px 必须先重烘：
 *          python3 tools/gen_font.py --size 24 --bpp 4 \
 *                  --out src/ui/assets/ui_font_cjk_24.c
 *   2. 两个字体都没有 U+00B0（度）。写 "18.4°C" 那个 ° 会变成方块，
 *      所以度符号用一个 6x6 的小圆环 LV_OBJ 画出来，不依赖字形。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <lvgl/lvgl.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "weather_page.h"
#include "ui_theme.h"
#include "ui_icon_runtime.h"   /* ui_icon("weather") + ui_icon_is_baked */
#include "ui_nav.h"            /* ui_nav_attach_back_swipe: 下滑返回 */
#include "infra/vela_tls.h"    /* vela_https_get + VELA_TLS_ERR_* */
#include "infra/weather_cache.h" /* 进程级共享天气缓存：预取/天气工具/本页共用 */
#include "cJSON.h"

#define TAG "[WeatherPage]"

/* 城市固定北京：手表没有定位，坐标写死 */
#define WX_HOST       "api.open-meteo.com"
#define WX_PORT       "443"
#define WX_PATH       "/v1/forecast?latitude=39.9042&longitude=116.4074" \
                      "&current_weather=true"

#define WX_RESP_CAP   2048          /* 这个响应实测不到 1KB，2KB 够 */
#define WX_STACK      (16 * 1024)   /* TLS + cJSON，和 node_client 取一样的量 */
#define WX_POLL_US    (50 * 1000)   /* worker 空闲时的轮询间隔（50ms） */
#define WX_JOIN_TRIES 8             /* 退出最多等 8 × 50ms = 400ms */
#define WX_TICK_MS    200           /* UI 取结果的周期 */
#define WX_CACHE_TTL  (10 * 60)     /* 模块级缓存有效期：10 分钟（秒） */

/****************************************************************************
 * 状态与数据
 ****************************************************************************/

typedef enum
{
  WX_BUSY = 0,   /* 获取中；同时也兼作"worker 有一件事要做"的请求标志 */
  WX_OK,         /* 成功 */
  WX_FAIL        /* 失败 */
} wx_state_t;

static lv_obj_t   *s_page;
static lv_obj_t   *s_temp_label;     /* 大字温度（88px 数字字体） */
static lv_obj_t   *s_desc_label;     /* 天气描述 / 获取中 / 获取失败 */
static lv_obj_t   *s_wind_value;     /* "风速"小卡里的数值 */
static lv_obj_t   *s_update_value;   /* "更新"小卡里的时刻 */
static lv_obj_t   *s_time_label;     /* 状态行：更新于 HH:MM / 离线缓存 · N 前 / 失败原因 */
static lv_obj_t   *s_btn;
static lv_obj_t   *s_btn_label;
static lv_timer_t *s_tick;

/* ── 取数结果：worker 写、UI 读，两个方向都要拿 s_lock ── */

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static wx_state_t      s_state = WX_BUSY;
static bool            s_dirty = true;   /* 有新结果没被 UI 取走 */
static bool            s_has_data;       /* 共享结构里有没有可显示的数据 */
static bool            s_from_cache;     /* 这份数据是缓存回放，不是本轮刚拉的 */
static double          s_temp_c;
static int             s_code;
static double          s_wind;
static int             s_hh = -1;        /* 更新时间；-1 = 还没有成功过 */
static int             s_mm;
static char            s_reason[48];     /* 失败原因 */

/* ── 模块级缓存（页面销毁不清）──
 *
 * 用户反馈"天气获取比较慢，而且每次都刷新"，所以把最后一次成功的数据
 * 留在文件作用域里，跨页面生命周期复用：
 *   有效 + 未过期 → 进页面直接渲染，不碰网络
 *   有效 + 已过期 → 照常 fetch；失败时退回这份旧数据继续显示
 *   无 效         → 必须 fetch
 *
 * 读写在 s_lock 下：worker 成功时写，UI 进页面时读。
 * 时间戳用 time(NULL)（本构建里它已经是本地墙钟，不要再减 8 小时）。 */

typedef struct
{
  bool   valid;       /* 缓存可用：至少成功取到过一次 */
  bool   last_ok;     /* 最近一次取数结果：true = 成功 / false = 失败 */
  double temp_c;      /* 温度 */
  int    code;        /* WMO 天气码 */
  double wind;        /* 风速 km/h */
  time_t stamp;       /* 成功取到数据的时刻（秒） */
} wx_cache_t;

static wx_cache_t s_cache;

/* ── worker 线程 ── */

static pthread_t         s_tid;
static volatile bool     s_thread_alive;
static volatile bool     s_stop;
static volatile uint32_t s_epoch;  /* 页面每重建/退出一次 +1，旧线程看到不等就退出 */

/****************************************************************************
 * 数字格式化
 *
 * ★ 不能用 snprintf("%.1f")：NuttX 的浮点格式化在本板会踩栈
 *   （lib_stackchk.c:57，channels/weixin_channel.c 里记过这个坑）。
 *   这里只做乘加，走硬件 FPU，不经过 libc 的浮点路径。
 ****************************************************************************/

static void fmt_temp(double t, char *buf, size_t cap)
{
    int t10 = (int)(t * 10.0 + (t >= 0.0 ? 0.5 : -0.5));
    int a   = t10 < 0 ? -t10 : t10;

    snprintf(buf, cap, "%s%d.%d", t10 < 0 ? "-" : "", a / 10, a % 10);
}

static void fmt_wind(double w, char *buf, size_t cap)
{
    int w10 = (int)(w * 10.0 + 0.5);

    snprintf(buf, cap, "%d.%d", w10 / 10, w10 % 10);
}

/* 缓存年龄：刚刚 / N 分钟前 / N 小时前。
 * 只在 age >= WX_CACHE_TTL 时才用；负数（时钟往回调过）也会走到这里，
 * 统一按"刚刚"处理，不显示负分钟数。 */
static void fmt_age(int age_s, char *buf, size_t cap)
{
    if (age_s < 60)
      {
        snprintf(buf, cap, "刚刚");
      }
    else if (age_s < 3600)
      {
        snprintf(buf, cap, "%d 分钟前", age_s / 60);
      }
    else
      {
        snprintf(buf, cap, "%d 小时前", age_s / 3600);
      }
}

/****************************************************************************
 * WMO 天气码 → 中文
 *
 * 只用 UI_FONT_SMALL（GB2312 全集）里确定有的字。
 ****************************************************************************/

static const char *wx_desc(int code)
{
    if (code == 0)
      {
        return "晴";
      }

    if (code >= 1 && code <= 3)
      {
        return "多云";
      }

    if (code == 45 || code == 48)
      {
        return "雾";
      }

    if (code >= 51 && code <= 57)
      {
        return "毛毛雨";
      }

    if (code >= 61 && code <= 67)
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

    if (code == 85 || code == 86)
      {
        return "阵雪";
      }

    if (code == 95)
      {
        return "雷阵雨";
      }

    if (code == 96 || code == 99)
      {
        return "雷暴冰雹";
      }

    return "未知";
}

/****************************************************************************
 * 结果交接（worker → UI）
 ****************************************************************************/

/** worker 交结果。只写共享结构 + 模块级缓存，不碰 lv_obj。
 *
 *  epoch 已经变了（页面被关掉/重建）或已经要求退出：结果没人要，直接丢。
 *  —— 这样即使线程被 detach 后还在跑，也不会污染下一次进页面的状态。
 *    缓存也不更新：只认"还属于当前这次请求"的结果，避免旧线程覆盖新数据。 */
static void wx_publish(uint32_t my_epoch, wx_state_t st, double temp,
                       int code, double wind, const char *reason)
{
    time_t now;
    struct tm tmv;

    pthread_mutex_lock(&s_lock);

    if (s_stop || s_epoch != my_epoch)
      {
        pthread_mutex_unlock(&s_lock);
        return;
      }

    now = time(NULL);

    /* 最近一次取数的成功/失败，记在缓存里备查（不影响显示） */
    s_cache.last_ok = (st == WX_OK);

    if (st == WX_OK)
      {
        /* 先写模块级缓存：页面关掉以后它还在，下次进来可以省一次请求 */
        s_cache.valid  = true;
        s_cache.temp_c = temp;
        s_cache.code   = code;
        s_cache.wind   = wind;
        s_cache.stamp  = now;

        s_temp_c = temp;
        s_code   = code;
        s_wind   = wind;
        s_reason[0] = '\0';

        localtime_r(&now, &tmv);
        s_hh = tmv.tm_hour;
        s_mm = tmv.tm_min;

        s_has_data   = true;
        s_from_cache = false;
      }
    else
      {
        snprintf(s_reason, sizeof(s_reason), "%s",
                 reason != NULL ? reason : "");

        /* 失败但有旧缓存：把旧数据摆回共享结构，让渲染继续显示它；
         * 连旧缓存都没有，才是真正的错误页。 */
        if (s_cache.valid)
          {
            s_temp_c = s_cache.temp_c;
            s_code   = s_cache.code;
            s_wind   = s_cache.wind;

            localtime_r(&s_cache.stamp, &tmv);
            s_hh = tmv.tm_hour;
            s_mm = tmv.tm_min;
          }

        s_has_data   = s_cache.valid;
        s_from_cache = s_cache.valid;
      }

    s_state = st;
    s_dirty = true;

    pthread_mutex_unlock(&s_lock);
}

/** UI 线程自己登记一次失败（例如线程都没起来）。 */
static void wx_fail_now(const char *reason)
{
    pthread_mutex_lock(&s_lock);
    s_state = WX_FAIL;
    snprintf(s_reason, sizeof(s_reason), "%s", reason != NULL ? reason : "");

    /* 和 wx_publish 的失败分支一样：有旧缓存就继续用旧数据 */
    if (s_cache.valid)
      {
        struct tm tmv;

        s_temp_c = s_cache.temp_c;
        s_code   = s_cache.code;
        s_wind   = s_cache.wind;

        localtime_r(&s_cache.stamp, &tmv);
        s_hh = tmv.tm_hour;
        s_mm = tmv.tm_min;
      }

    s_has_data   = s_cache.valid;
    s_from_cache = s_cache.valid;
    s_dirty      = true;
    pthread_mutex_unlock(&s_lock);
}

/** 进页面时先看**进程级**共享缓存（infra/weather_cache）。
 *
 *  这份缓存的写方有三条：本页 worker 成功（"page"）、get_weather 工具的
 *  Open-Meteo 兜底（"tool"）、后台预取线程每 30 分钟一次（"prefetch"）。
 *  所以进页面通常能直接命中 —— 而不再是"只有本页上次成功过才有数据"。
 *
 *  返回 true  = 有数据且 age < WX_CACHE_TTL：调用方直接渲染，一次请求都不发；
 *  返回 false = 没数据 / 已过期：调用方照旧 wx_request()。
 *               过期时也把数据摆进共享结构（s_has_data = true），
 *               于是 wx_request() 置 BUSY 以后渲染的是"旧数据 + 更新中"，
 *               不会先闪一下 "--"。
 *
 *  顺手把共享缓存同步进页面自己的 s_cache：这样"刷新失败但手里有旧数据"
 *  那条分支（wx_publish 的失败路径）也能用上预取拉到的数据，而不是在
 *  预取刚成功过的情况下还给用户弹错误页。 */
static bool wx_shared_restore(void)
{
    weather_cache_s wc;
    struct tm       tmv;
    int             age;
    bool            fresh;

    if (!weather_cache_get(&wc) || !wc.valid)
      {
        syslog(LOG_INFO, "%s shared cache miss\n", TAG);
        return false;
      }

    /* age < 0：时钟被 SNTP 往回调过（stamp 在未来）。按过期处理：
     * 宁可多拉一次，也不拿错时间糊弄人 —— 与 wx_cache_restore 一致。 */
    age   = weather_cache_age_s();
    fresh = (age >= 0 && age < WX_CACHE_TTL);

    pthread_mutex_lock(&s_lock);

    /* 共享缓存 → 页面缓存：字段一一对应，stamp 用共享缓存的成功时间
     * （"更新于 HH:MM"显示的就是数据真正拉到的时刻）。 */
    s_cache.valid  = true;
    s_cache.temp_c = (double)wc.temp_c;
    s_cache.code   = wc.code;
    s_cache.wind   = (double)wc.wind_kmh;
    s_cache.stamp  = wc.stamp;
    s_cache.last_ok = wc.last_ok;

    s_temp_c = s_cache.temp_c;
    s_code   = s_cache.code;
    s_wind   = s_cache.wind;

    localtime_r(&wc.stamp, &tmv);
    s_hh = tmv.tm_hour;
    s_mm = tmv.tm_min;

    s_has_data   = true;
    s_from_cache = true;

    if (fresh)
      {
        s_reason[0] = '\0';
        s_state     = WX_OK;
        s_dirty     = true;
      }
    /* 过期时不动 s_state：由调用方的 wx_request() 置 BUSY。 */

    pthread_mutex_unlock(&s_lock);

    syslog(LOG_INFO, "%s shared cache %s (age %d s, ttl %d s, src %s, last_ok %d)\n",
           TAG, fresh ? "hit" : "stale", age, WX_CACHE_TTL, wc.source,
           wc.last_ok ? 1 : 0);

    return fresh;
}

/** 进页面时再看**页面自己**的缓存：有效且未过期就直接用，一次请求都不发。
 *
 *  这是 wx_shared_restore() 的兜底。两者是同步写的（本页成功一次两边都写），
 *  正常情况下第二条不会命中；留着它是因为这段判断包含了"时钟被往回调"
 *  的处理，而且共享缓存整个模块没数据时还能靠页面自己的内存态撑一次。
 *
 *  返回 true  = 共享结构已经填成"成功（来自缓存）"，UI 直接渲染即可；
 *  返回 false = 过期/没有缓存，调用方照常 wx_request()。
 *  时钟被往回调过（age < 0）也按过期处理：宁可多拉一次，不拿错时间糊弄人。 */
static bool wx_cache_restore(void)
{
    time_t    now = time(NULL);
    time_t    age = 0;
    bool      fresh = false;
    bool      valid = false;
    bool      last_ok = false;
    struct tm tmv;

    pthread_mutex_lock(&s_lock);

    last_ok = s_cache.last_ok;
    valid   = s_cache.valid;

    if (valid)
      {
        age   = now - s_cache.stamp;
        fresh = (age >= 0 && age < WX_CACHE_TTL);
      }

    if (fresh)
      {
        s_temp_c = s_cache.temp_c;
        s_code   = s_cache.code;
        s_wind   = s_cache.wind;

        localtime_r(&s_cache.stamp, &tmv);
        s_hh = tmv.tm_hour;
        s_mm = tmv.tm_min;

        s_reason[0]  = '\0';
        s_state      = WX_OK;
        s_has_data   = true;
        s_from_cache = true;
        s_dirty      = true;
      }

    pthread_mutex_unlock(&s_lock);

    /* age 只在命中时才有意义（此时一定 < TTL，转 int 安全）；
     * 没命中就只报有没有缓存，别把可能离谱的秒数塞进日志。 */
    if (fresh)
      {
        syslog(LOG_INFO, "%s cache hit (age %d s, ttl %d s, last_ok %d)\n",
               TAG, (int)age, WX_CACHE_TTL, last_ok ? 1 : 0);
      }
    else
      {
        syslog(LOG_INFO, "%s cache miss (valid %d, ttl %d s, last_ok %d)\n",
               TAG, valid ? 1 : 0, WX_CACHE_TTL, last_ok ? 1 : 0);
      }

    return fresh;
}

/****************************************************************************
 * 一次完整的取数（只在 worker 线程里跑）
 ****************************************************************************/

static void wx_fetch_once(char *resp, size_t cap, uint32_t my_epoch)
{
    char reason[48] = "";
    int  status;
    cJSON *root;
    cJSON *cw;
    cJSON *item_t;
    cJSON *item_c;
    cJSON *item_w;

    memset(resp, 0, cap);

    status = vela_https_get(WX_HOST, WX_PORT, WX_PATH, resp, cap);

    if (status != 200)
      {
        /* 负值是 vela_tls 的错误码，正数是 HTTP 状态码 */
        switch (status)
          {
            case VELA_TLS_ERR_CONNECT:
              snprintf(reason, sizeof(reason), "连接失败");
              break;

            case VELA_TLS_ERR_HANDSHAKE:
              snprintf(reason, sizeof(reason), "TLS 握手失败");
              break;

            case VELA_TLS_ERR_WRITE:
              snprintf(reason, sizeof(reason), "发送失败");
              break;

            case VELA_TLS_ERR_READ:
              /* 读超时和"响应读坏了"在这一层是同一个返回码
               * （vela_tls.c 的 ssl_read 失败 / 状态行解析失败都返回 -4），
               * 所以两种可能都写出来，不假装能分清。 */
              snprintf(reason, sizeof(reason), "读取失败/超时");
              break;

            case VELA_TLS_ERR_OVERFLOW:
              snprintf(reason, sizeof(reason), "响应超过 %d 字节",
                       (int)cap);
              break;

            default:
              if (status > 0)
                {
                  snprintf(reason, sizeof(reason), "HTTP %d", status);
                }
              else
                {
                  snprintf(reason, sizeof(reason), "网络错误 %d", status);
                }
              break;
          }

        syslog(LOG_WARNING, "%s fetch failed: %d\n", TAG, status);

        /* 共享缓存也记一次失败：只把 last_ok 打 false，旧数据留着
         * （别的页面/工具还可能拿它显示）。 */
        weather_cache_mark_fail();

        wx_publish(my_epoch, WX_FAIL, 0.0, 0, 0.0, reason);
        return;
      }

    root = cJSON_Parse(resp);
    if (root == NULL)
      {
        syslog(LOG_WARNING, "%s JSON parse failed (%d bytes)\n",
               TAG, (int)strlen(resp));
        weather_cache_mark_fail();
        wx_publish(my_epoch, WX_FAIL, 0.0, 0, 0.0, "解析失败");
        return;
      }

    cw     = cJSON_GetObjectItem(root, "current_weather");
    item_t = cJSON_GetObjectItem(cw, "temperature");
    item_c = cJSON_GetObjectItem(cw, "weathercode");
    item_w = cJSON_GetObjectItem(cw, "windspeed");

    if (cJSON_IsNumber(item_t) && cJSON_IsNumber(item_c) &&
        cJSON_IsNumber(item_w))
      {
        double temp = item_t->valuedouble;
        int    code = (int)item_c->valuedouble;
        double wind = item_w->valuedouble;
        int    t10  = (int)(temp * 10.0 + (temp >= 0.0 ? 0.5 : -0.5));
        int    w10  = (int)(wind * 10.0 + 0.5);
        int    ta   = t10 < 0 ? -t10 : t10;

        cJSON_Delete(root);

        /* 日志也走整数格式化：syslog 里的 %.1f 同样会踩浮点格式化的坑 */
        syslog(LOG_INFO, "%s %s%d.%d degC, code %d, wind %d.%d km/h\n",
               TAG, t10 < 0 ? "-" : "", ta / 10, ta % 10,
               code, w10 / 10, w10 % 10);

        /* 写进程级共享缓存（source="page"）：预取线程和 get_weather 工具
         * 看的是同一份，于是"本页刚好拉过"也能让别的入口直接受益。
         * 注意这里不受下面 wx_publish 的 epoch 判断影响：页面已经关了
         * 不代表这份刚拉到的数据不新鲜，写进去下次进页面就能省一次请求。 */
        weather_cache_update((float)temp, code, (float)wind, "page");

        wx_publish(my_epoch, WX_OK, temp, code, wind, NULL);
        return;
      }

    cJSON_Delete(root);
    weather_cache_mark_fail();
    wx_publish(my_epoch, WX_FAIL, 0.0, 0, 0.0, "数据缺字段");
}

/****************************************************************************
 * worker 线程
 ****************************************************************************/

static void *wx_worker(void *arg)
{
    uint32_t my_epoch = (uint32_t)(uintptr_t)arg;
    char     resp[WX_RESP_CAP];   /* 放栈上：detach 后可能有两个线程同时活 */

    for (;;)
      {
        bool want;

        if (s_stop || s_epoch != my_epoch)
          {
            break;
          }

        pthread_mutex_lock(&s_lock);
        want = (s_state == WX_BUSY);
        pthread_mutex_unlock(&s_lock);

        if (!want)
          {
            usleep(WX_POLL_US);
            continue;
          }

        wx_fetch_once(resp, sizeof(resp), my_epoch);
      }

    pthread_mutex_lock(&s_lock);
    if (s_epoch == my_epoch)
      {
        s_thread_alive = false;
      }
    pthread_mutex_unlock(&s_lock);

    return NULL;
}

static bool wx_worker_start(void)
{
    pthread_attr_t attr;
    uint32_t       epoch;
    int            rc;

    if (s_thread_alive)
      {
        return true;
      }

    s_stop  = false;
    s_epoch = s_epoch + 1;
    epoch   = s_epoch;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, WX_STACK);

    /* epoch 从 1 开始，顺便保证实参非 NULL */
    rc = pthread_create(&s_tid, &attr, wx_worker, (void *)(uintptr_t)epoch);
    pthread_attr_destroy(&attr);

    if (rc != 0)
      {
        syslog(LOG_ERR, "%s pthread_create failed: %d\n", TAG, rc);
        return false;
      }

    s_thread_alive = true;
    syslog(LOG_INFO, "%s worker started (epoch %u)\n", TAG, (unsigned)epoch);
    return true;
}

static void wx_worker_stop(void)
{
    int i;

    if (!s_thread_alive)
      {
        return;
      }

    s_stop = true;

    /* 最多等 400ms：线程空闲时 50ms 内就会看到 s_stop 退出，
     * 这点等待足够覆盖调度抖动，又不会让"切页"明显卡顿。
     * 不能直接 pthread_join：vela_https_get 最坏 15s 连接 + 30s 读，
     * 同步等就把"切页"卡死几十秒 —— 和同步取数是同一个毛病。
     * 等不到就 detach：线程会在当前这次请求返回后自己退出，
     * 它只碰本文件的静态结构（不碰 lv_obj），不会野指针。 */
    for (i = 0; i < WX_JOIN_TRIES && s_thread_alive; i++)
      {
        usleep(WX_POLL_US);
      }

    if (!s_thread_alive)
      {
        pthread_join(s_tid, NULL);
        syslog(LOG_INFO, "%s worker joined\n", TAG);
      }
    else
      {
        /* 换代：在途结果会被 wx_publish 丢掉，
         * 退出时也不会去动下一次进页面新建线程的标志 */
        s_epoch = s_epoch + 1;
        pthread_detach(s_tid);
        syslog(LOG_WARNING, "%s worker still busy, detached\n", TAG);
      }

    s_thread_alive = false;
}

/****************************************************************************
 * 界面刷新（只在 UI 线程里跑）
 ****************************************************************************/

/** 画一份有效数据：温度 + 描述 + 风向小卡 + 状态行。
 *
 *  age_s = time(NULL) - s_cache.stamp（-1 = 没有缓存时间戳）：
 *    age < WX_CACHE_TTL → 状态行 "更新于 HH:MM"（本轮刚拉到的也走这条）
 *    age >= WX_CACHE_TTL → "离线缓存 · N 小时前" + 警示色：
 *                          显示的是数据真正取到的时刻，不谎称刚刚更新。
 *  note 非空时在状态行后面追加（"更新中…" / "刷新失败"）；
 *  warn = true 表示这一行是坏消息，用警示色。
 */
static void wx_show_data(double temp, int code, double wind,
                         int hh, int mm, int age_s,
                         const char *note, bool warn)
{
    char buf[32];
    char line[96];
    char age[24];
    bool stale;

    fmt_temp(temp, buf, sizeof(buf));
    lv_label_set_text(s_temp_label, buf);

    lv_obj_set_style_text_color(s_desc_label,
                                lv_color_hex(UI_TEXT_PRIMARY), 0);
    lv_label_set_text(s_desc_label, wx_desc(code));

    fmt_wind(wind, buf, sizeof(buf));
    snprintf(line, sizeof(line), "%s km/h", buf);
    lv_label_set_text(s_wind_value, line);

    if (hh >= 0)
      {
        snprintf(line, sizeof(line), "%02d:%02d", hh, mm);
      }
    else
      {
        snprintf(line, sizeof(line), "--");
      }

    lv_label_set_text(s_update_value, line);

    /* 状态行：先写新鲜度，再按需追加后缀。
     * age_s < 0 = 时钟被 SNTP 往回调过（stamp 在未来），算不出真实年龄；
     * 这时写"更新于"会假装数据是新的，按过期处理并明说时间未知。 */
    stale = (age_s < 0 || age_s >= WX_CACHE_TTL);
    line[0] = '\0';

    if (age_s < 0)
      {
        snprintf(line, sizeof(line), "离线缓存 · 时间未知");
      }
    else if (stale)
      {
        fmt_age(age_s, age, sizeof(age));
        snprintf(line, sizeof(line), "离线缓存 · %s", age);
      }
    else if (hh >= 0)
      {
        snprintf(line, sizeof(line), "更新于 %02d:%02d", hh, mm);
      }

    if (note != NULL && note[0] != '\0')
      {
        size_t used = strlen(line);

        snprintf(line + used, sizeof(line) - used, "%s%s",
                 used > 0 ? " · " : "", note);
      }

    lv_label_set_text(s_time_label, line);
    lv_obj_set_style_text_color(
        s_time_label,
        lv_color_hex((stale || warn) ? UI_COLOR_WARNING : UI_TEXT_TERTIARY),
        0);
}

static void wx_render(void)
{
    wx_state_t st;
    double     temp;
    double     wind;
    int        code;
    int        hh;
    int        mm;
    int        age_s = -1;
    bool       has_data;
    char       reason[48];

    pthread_mutex_lock(&s_lock);

    if (!s_dirty)
      {
        pthread_mutex_unlock(&s_lock);
        return;
      }

    st         = s_state;
    temp       = s_temp_c;
    wind       = s_wind;
    code       = s_code;
    hh         = s_hh;
    mm         = s_mm;
    has_data   = s_has_data;
    snprintf(reason, sizeof(reason), "%s", s_reason);

    /* 缓存年龄在锁里读：s_cache.stamp 由 worker 的 wx_publish 在锁内写 */
    if (s_cache.valid)
      {
        age_s = (int)(time(NULL) - s_cache.stamp);
      }

    s_dirty = false;

    pthread_mutex_unlock(&s_lock);

    switch (st)
      {
        case WX_OK:
          /* 刚拉到的 / 缓存回放：数据一样，状态行按 age 自动选写法 */
          wx_show_data(temp, code, wind, hh, mm, age_s, NULL, false);
          break;

        case WX_FAIL:
          if (has_data)
            {
              /* 刷新失败但手里还有旧数据：别把整页变成错误页。
               * 旧数据照常显示，状态行会自己标"离线缓存 · N 前"，
               * 后缀补一句刷新失败；具体原因在 wx_fetch_once 里打了日志。 */
              wx_show_data(temp, code, wind, hh, mm, age_s,
                           "刷新失败", true);
            }
          else
            {
              lv_label_set_text(s_temp_label, "--");

              lv_obj_set_style_text_color(s_desc_label,
                                          lv_color_hex(UI_COLOR_WARNING), 0);
              lv_label_set_text(s_desc_label, "获取失败");
              lv_label_set_text(s_wind_value, "--");
              lv_label_set_text(s_update_value, "--");
              lv_label_set_text(s_time_label,
                                reason[0] != '\0' ? reason : "原因未知");
              lv_obj_set_style_text_color(s_time_label,
                                          lv_color_hex(UI_COLOR_WARNING), 0);
            }
          break;

        case WX_BUSY:
        default:
          if (has_data)
            {
              /* 手里已经有数据（共享缓存过期 / 用户点了刷新）时不要清屏：
               * 继续显示旧数据 + "离线缓存 · N 前 · 更新中…"。
               * 这是"打开即最新"的可见的一半 —— 另一半是后台预取：
               * 预取拉到过的数据进页面就直接命中，走不到这个分支。 */
              wx_show_data(temp, code, wind, hh, mm, age_s, "更新中…", false);
            }
          else
            {
              lv_label_set_text(s_temp_label, "--");

              lv_obj_set_style_text_color(s_desc_label,
                                          lv_color_hex(UI_TEXT_SECONDARY), 0);
              lv_label_set_text(s_desc_label, "获取中…");
              lv_label_set_text(s_wind_value, "--");
              lv_label_set_text(s_update_value, "--");
              lv_label_set_text(s_time_label, "");
            }
          break;
      }
}

static void wx_tick_cb(lv_timer_t *timer)
{
    static int last_min = -1;
    int        now_min;

    (void)timer;

    /* "离线缓存 · N 分钟前"是相对时间：页面停在屏上也要它往前爬。
     * 每分钟把 dirty 置一次，让下面的 wx_render() 用新 age 重画；
     * 200ms 那一拍照旧只做"取结果 -> 改标签"。 */
    now_min = (int)(time(NULL) / 60);
    if (now_min != last_min)
      {
        last_min = now_min;
        pthread_mutex_lock(&s_lock);
        s_dirty = true;
        pthread_mutex_unlock(&s_lock);
      }

    wx_render();
}

/****************************************************************************
 * 交互
 ****************************************************************************/

/** 发一次请求：置 BUSY、确保 worker 活着、立刻画成"获取中…"。 */
static void wx_request(void)
{
    pthread_mutex_lock(&s_lock);
    s_state = WX_BUSY;
    s_dirty = true;
    pthread_mutex_unlock(&s_lock);

    if (!wx_worker_start())
      {
        wx_fail_now("线程创建失败");
      }

    /* 不等 200ms 那一拍，点下去立刻有反馈 */
    wx_render();
}

static void on_refresh_clicked(lv_event_t *e)
{
    (void)e;

    /* 用户主动刷新：无条件重拉，**不看 TTL**（wx_request 从不查缓存）。
     * 唯一例外是已经有一次在拉：worker 一次只做一件事，别叠加请求。 */

    pthread_mutex_lock(&s_lock);
    if (s_state == WX_BUSY)
      {
        pthread_mutex_unlock(&s_lock);
        return;
      }
    pthread_mutex_unlock(&s_lock);

    wx_request();
}

/****************************************************************************
 * 布局小工具
 ****************************************************************************/

/** 一个透明的容器：不画底、不画边、不滚动。 */
static void make_plain(lv_obj_t *obj)
{
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

/****************************************************************************
 * 公开接口
 ****************************************************************************/

lv_obj_t *weather_page_create(void)
{
    lv_obj_t *row;
    lv_obj_t *unit;
    lv_obj_t *deg;
    lv_obj_t *c;
    lv_obj_t *icon_box;
    lv_obj_t *icon;
    lv_obj_t *cards;
    lv_obj_t *card;
    lv_obj_t *label;
    lv_obj_t *spacer;
    const lv_image_dsc_t *icon_dsc;

    if (s_page != NULL)
      {
        return s_page;
      }

    s_page = lv_obj_create(lv_screen_active());
    ui_page_init(s_page);
    lv_obj_set_flex_flow(s_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_page, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* 竖向间距改用各对象自己的 margin（见下方高度预算），
     * 统一 pad_row=8 会多出 40px，刷新按钮会被挤出屏幕。
     * 原来这里还有一行"北京"副标题，为给 96px 图标 + 两张 80px 小卡
     * 腾高度去掉了 —— 城市写死北京，本页数据不会变。 */
    lv_obj_set_style_pad_row(s_page, 0, 0);

    (void)ui_statusbar_create(s_page, "天气");

    /* 高度预算（内容区 410 = 450 - 上下各 20）：
     *   状态栏 32 + 温度 80 + (2+21) 描述 + (2+96) 图标 +
     *   (4+80) 小卡 + (4+21) 状态行 + 8 空档 + (4+56) 刷新 = 410 */

    /* 大字温度 + "°C"
     * 88px 字体只有数字和 ".-/:"，没有度符号，所以度用小圆环手绘：
     * 一个 6x6 的空心圆，靠 margin_top 抬到数字顶部，看起来就是上标 °。 */

    row = lv_obj_create(s_page);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    make_plain(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    s_temp_label = lv_label_create(row);
    lv_label_set_text(s_temp_label, "--");
    lv_obj_set_style_text_font(s_temp_label, UI_FONT_TIME, 0);
    lv_obj_set_style_text_color(s_temp_label, lv_color_hex(UI_TEXT_PRIMARY), 0);
    /* ★ 行框 88px，但 baseline 在 72（font 的 base_line=16），底部 16px
     * 是空白。把标签高度收到 80 不切字形，却给下面 96px 的图标 + 两张
     * 80px 小卡腾出 8px —— 410 的高度预算就靠这点余量。 */
    lv_obj_set_height(s_temp_label, 80);

    unit = lv_obj_create(row);
    lv_obj_set_size(unit, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    make_plain(unit);
    lv_obj_set_style_margin_top(unit, 14, 0);
    lv_obj_set_style_margin_left(unit, 4, 0);
    lv_obj_set_flex_flow(unit, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(unit, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    deg = lv_obj_create(unit);
    lv_obj_set_size(deg, 6, 6);
    lv_obj_set_style_radius(deg, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(deg, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(deg, 2, 0);
    lv_obj_set_style_border_color(deg, lv_color_hex(UI_TEXT_SECONDARY), 0);
    lv_obj_set_style_pad_all(deg, 0, 0);
    lv_obj_clear_flag(deg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(deg, LV_OBJ_FLAG_CLICKABLE);

    c = lv_label_create(unit);
    lv_label_set_text(c, "C");
    lv_obj_set_style_text_font(c, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(c, lv_color_hex(UI_TEXT_SECONDARY), 0);

    /* 天气描述（成功）/ 获取中 / 获取失败 */

    s_desc_label = lv_label_create(s_page);
    lv_label_set_text(s_desc_label, "获取中…");
    lv_obj_set_style_text_font(s_desc_label, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_desc_label,
                                lv_color_hex(UI_TEXT_SECONDARY), 0);
    lv_obj_set_style_margin_top(s_desc_label, 2, 0);

    /* 96px 天气图标。
     * ★ 资产只有 28x28（ui_icons.c 的 ui_icon_weather），而
     *   lv_image_set_scale() 只缩放**绘制**、不改对象的布局尺寸
     *   （GET_SELF_SIZE 仍报 28x28），直接放 flex 里只占 28px 高。
     *   所以套一个 96x96 的透明盒、图在盒里居中（同 pet_page.c 的云朵图）。 */

    icon_box = lv_obj_create(s_page);
    lv_obj_set_size(icon_box, 96, 96);
    make_plain(icon_box);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_margin_top(icon_box, 2, 0);

    icon_dsc = ui_icon("weather");
    icon     = lv_image_create(icon_box);

    if (icon_dsc != NULL)
      {
        lv_image_set_src(icon, icon_dsc);
        lv_image_set_scale(icon, 878);   /* 28 * 878 / 256 ≈ 96 */

        /* 内置图标是白色线条画，染成天气语义色；设计师放进 icons/ 的
         * 彩色图不能染色（ui_icon_is_baked 的用途，见 ui_icon_runtime.h） */
        if (ui_icon_is_baked(icon_dsc))
          {
            lv_obj_set_style_image_recolor(icon,
                                           lv_color_hex(UI_COLOR_WEATHER), 0);
            lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
          }

        lv_obj_center(icon);
      }
    else
      {
        lv_obj_add_flag(icon_box, LV_OBJ_FLAG_HIDDEN);
      }

    /* 两张 168x80 小卡：风速 / 更新时刻。
     * 原来这两行是贴左边的纯文字，下半屏全被 spacer 吃掉；
     * 改成卡片后信息有边界，视觉上也填满了下半屏。 */

    cards = lv_obj_create(s_page);
    lv_obj_set_size(cards, lv_pct(100), 68);
    make_plain(cards);
    lv_obj_set_style_margin_top(cards, 4, 0);
    lv_obj_set_flex_flow(cards, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cards, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    card = ui_card_create(cards, 168, 68);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_style_pad_row(card, 2, 0);

    label = lv_label_create(card);
    lv_label_set_text(label, "风速");
    lv_obj_set_style_text_font(label, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_TEXT_TERTIARY), 0);

    s_wind_value = lv_label_create(card);
    lv_label_set_text(s_wind_value, "--");
    /* 数值是纯 ASCII（数字 + km/h），可以用 24px 的 BODY 当视觉锚点 */
    lv_obj_set_style_text_font(s_wind_value, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_wind_value,
                                lv_color_hex(UI_TEXT_PRIMARY), 0);

    card = ui_card_create(cards, 168, 68);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_style_pad_row(card, 2, 0);

    label = lv_label_create(card);
    lv_label_set_text(label, "更新");
    lv_obj_set_style_text_font(label, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_TEXT_TERTIARY), 0);

    s_update_value = lv_label_create(card);
    lv_label_set_text(s_update_value, "--");
    lv_obj_set_style_text_font(s_update_value, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_update_value,
                                lv_color_hex(UI_TEXT_PRIMARY), 0);

    /* 状态行：更新于 HH:MM / 离线缓存 · N 前 / 失败原因。
     * 空字符串时高度 0，不占版面；超宽时用 DOT 截断，保证永远只占一行。 */

    s_time_label = lv_label_create(s_page);
    lv_obj_set_width(s_time_label, lv_pct(100));
    lv_label_set_long_mode(s_time_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_time_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_time_label, "");
    lv_obj_set_style_text_font(s_time_label, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_time_label,
                                lv_color_hex(UI_TEXT_TERTIARY), 0);
    lv_obj_set_style_margin_top(s_time_label, 4, 0);

    /* 弹性空档：把刷新按钮顶到内容底部。
     * 温度/描述行长度会变，固定间距容易在"获取中"时留一条大缝，
     * 用 flex_grow 让多出来的高度都变成这一块留白。 */

    spacer = lv_obj_create(s_page);
    make_plain(spacer);
    lv_obj_set_width(spacer, lv_pct(100));
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_CLICKABLE);

    /* 刷新按钮（自己拼 LV_BUTTON + LV_LABEL：
     *  ui_button_create 会把文字设成 UI_FONT_BODY，24px 字体里没有"刷"字） */

    s_btn = lv_button_create(s_page);
    lv_obj_set_size(s_btn, 168, 56);
    lv_obj_set_style_radius(s_btn, UI_RADIUS_PILL, 0);
    lv_obj_set_style_bg_color(s_btn, lv_color_hex(UI_COLOR_WEATHER), 0);
    lv_obj_set_style_bg_opa(s_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_btn, 0, 0);
    /* 4 而不是 UI_GAP_M(12)：410 的预算里空档只剩 8px，见函数开头 */
    lv_obj_set_style_margin_top(s_btn, 4, 0);
    lv_obj_add_event_cb(s_btn, on_refresh_clicked, LV_EVENT_CLICKED, NULL);

    /* 圆角屏底部安全区：加 20px 透明垫片，grow 空档会相应缩短，
     * 按钮整体上移到 354..410（原来 374..430 离屏底只有 20px）。 */
    {
        lv_obj_t *bottom_pad = lv_obj_create(s_page);
        lv_obj_set_size(bottom_pad, lv_pct(100), 20);
        lv_obj_set_style_bg_opa(bottom_pad, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(bottom_pad, 0, 0);
        lv_obj_set_style_pad_all(bottom_pad, 0, 0);
        lv_obj_clear_flag(bottom_pad, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(bottom_pad, LV_OBJ_FLAG_CLICKABLE);
    }

    s_btn_label = lv_label_create(s_btn);
    lv_label_set_text(s_btn_label, "刷新");
    lv_obj_set_style_text_font(s_btn_label, UI_FONT_SMALL, 0);
    /* 深色语义底（UI_COLOR_WEATHER）必须白字，否则 2.85:1（2026-11-02） */
    lv_obj_set_style_text_color(s_btn_label,
                                lv_color_hex(ui_text_on_bg(UI_COLOR_WEATHER)),
                                0);
    lv_obj_center(s_btn_label);

    /* 必须挂手势：不调这句页面就滑不出去（本仓踩过：
     * ui_gesture_attach(page, NULL) 只维护状态、没有回调）。 */

    ui_nav_attach_back_swipe(s_page);

    /* 进页面先看**进程级**共享缓存（后台预取 / get_weather 工具 / 本页上次
     * 成功这三条路径都会写它）：
     *   有数据且未过期 → 直接渲染，一次请求都不发（"打开即最新"）；
     *   有数据但已过期 → 先把旧数据摆上屏（"离线缓存 · N 前"），再起 worker
     *                    后台刷新 —— 用户不会看到空白页等十几秒；
     *   没有数据       → 照旧 wx_request()。
     * 第二条 wx_cache_restore() 是兜底（两者同步写，正常不会命中）。 */

    if (wx_shared_restore())
      {
        wx_render();
      }
    else if (wx_cache_restore())
      {
        wx_render();
      }
    else
      {
        wx_request();
      }

    /* 200ms 取一次结果：worker 从不碰 lv_obj，这里只改标签 */

    s_tick = lv_timer_create(wx_tick_cb, WX_TICK_MS, NULL);

    syslog(LOG_INFO, "%s created\n", TAG);
    return s_page;
}

void weather_page_delete(void)
{
    /* 先停 worker：它只写共享结构、不碰 lv_obj，
     * 但仍然要"先停线程、再删对象"，避免留下还在跑的线程。 */

    wx_worker_stop();

    if (s_tick != NULL)
      {
        lv_timer_delete(s_tick);
        s_tick = NULL;
      }

    if (s_page != NULL)
      {
        lv_obj_delete(s_page);
        s_page = NULL;
      }

    s_temp_label   = NULL;
    s_desc_label   = NULL;
    s_wind_value   = NULL;
    s_update_value = NULL;
    s_time_label   = NULL;
    s_btn          = NULL;
    s_btn_label    = NULL;

    /* 下一次进页面从"获取中"重新开始，不残留上一次的数字。
     * ★ s_cache 故意不动：缓存要跨页面生命周期活着，下次进页面才能直接复用。 */

    pthread_mutex_lock(&s_lock);
    s_state      = WX_BUSY;
    s_dirty      = true;
    s_hh         = -1;
    s_mm         = 0;
    s_has_data   = false;
    s_from_cache = false;
    s_reason[0]  = '\0';
    pthread_mutex_unlock(&s_lock);

    syslog(LOG_INFO, "%s deleted\n", TAG);
}

/* 给表盘等外部页面读缓存：只要成功过一次就算命中（语义见 .h）。
 *
 * 必须拿 s_lock：s_cache 的写方（wx_publish 的成功分支）在锁里
 * 连续写 valid/temp_c/code 等多个字段，不锁的话调用方可能读到
 * "valid 已置位、温度还是旧值"的半程组合。
 * 有意不看 last_ok / TTL：最近一次刷新失败不代表旧数据不能显示 ——
 * 天气页自己的失败分支也是拿旧缓存继续渲染的，表盘卡片保持一致。 */
bool weather_page_get_cached(float *temp_c, int *code)
{
    bool hit;

    pthread_mutex_lock(&s_lock);

    hit = s_cache.valid;
    if (hit)
      {
        if (temp_c != NULL)
          {
            *temp_c = (float)s_cache.temp_c;
          }

        if (code != NULL)
          {
            *code = s_cache.code;
          }
      }

    pthread_mutex_unlock(&s_lock);

    /* 页面还没打开过时页面缓存是空的（s_cache 只在本页拉成功时写）。但后台
     * 预取（30 分钟一轮）或 get_weather 工具可能早就拉到过数据了 —— 表盘
     * 天气卡走的就是本函数，让它也能吃到那份数据，否则"开机后先看表盘"
     * 永远显示 "--"。语义与上面一致：只看有没有成功数据，不做 TTL。 */
    if (!hit)
      {
        weather_cache_s wc;

        if (weather_cache_get(&wc) && wc.valid)
          {
            if (temp_c != NULL)
              {
                *temp_c = wc.temp_c;
              }

            if (code != NULL)
              {
                *code = wc.code;
              }

            hit = true;
          }
      }

    return hit;
}
