/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * BLE GATT Command Handler
 *
 * Processes JSON commands received from the companion Android App
 * via BLE GATT NUS RX characteristic.
 *
 * 旧命令（保持不变）：
 *   {"cmd":"health"}   {"cmd":"wifi_config",...}   {"cmd":"ping"}   {"cmd":"status"}
 * v2 命令（docs_ble/98_ble_extended_protocol.md FW-A）：
 *   {"cmd":"chat","text":"..."}                     -> 入 agent 队列，回答异步回 evt=chat
 *   {"cmd":"chat_history"}                          -> evt=chat_history
 *   {"cmd":"alarm_list"}                            -> evt=alarm_list
 *   {"cmd":"alarm_add","name":"...","at":<epoch>}   -> evt=alarm_add
 *   {"cmd":"alarm_del","id":"..."}                  -> evt=alarm_del
 *   {"cmd":"expression_get"} / {"cmd":"expression_set","index":N}
 *                                                   -> evt=expression
 *   {"cmd":"settings_get"} / {"cmd":"settings_set","brightness":N|"dnd":1}
 *                                                   -> evt=settings
 *   {"cmd":"care_list","limit":8}                    -> evt=care_list（RAM care_log）
 *   {"cmd":"care_ack","seq":N,"feedback":"ACCEPTED|IGNORED|REJECTED"}
 *                                                   -> evt=care_ack
 *   {"cmd":"care_settings_get"} / {"cmd":"care_settings_set",
 *     "dnd":0,"voice":1,"quiet_start":22,"quiet_end":7,"daily_budget":8,
 *     "types":{"hr":1,"idle":1,"sedentary":1}}      -> evt=care_settings
 *     （P2B-1A：voice=关怀本地语音开关，RAM only，默认 1；0=voice_off）
 *   {"cmd":"care_event","type":"eye_rest|water|sleep|weather"}
 *     -> {"evt":"care_event","ok":1,"type":"...","suppressed":0}
 *     （P2B-2A/docs114：未知 type ok=0；suppressed=1 表示被 DND/夜间/预算/
 *       冷却/最小间隔静默；eye_rest 只角标/历史不出声，water/sleep/weather
 *       走本地缓存语音，仍受 voice/PTT/音乐门禁）
 *   {"cmd":"demo","action":"hr_alert|greet|water|sleep|weather|sedentary|
 *                           milestone|alarm","seconds":N}
 *                                                   -> evt=demo（DEMO-FW/PHONE 演示用）
 *
 * 线程模型：本文件全部运行在 BLE GATT 的 RX 回调上下文（BT 栈线程），命令
 * 串行到达。这里只做内存操作与短临界区；凡是要碰 LVGL 的活一律投递到
 * LVGL 线程执行（lvgl_ui_post / lvgl_ui_history_export，等待上限 200ms）。
 * cron 的读写自带内部锁，但 add/remove 会落盘（/data 上的 NOR 写），比纯
 * 内存操作慢 —— 这是这条命令本身的代价，与 tool_cron 的做法一致。
 */

#include "ble_cmd_handler.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include "agent_config.h" /* AGENT_CRON_MAX_JOBS：alarm_list 的 cron 快照容量 */
#include "core/message_bus.h"
#include "infra/ble_gatt.h"
#include "infra/cron_service.h"
#include "infra/network_manager.h"

#if defined(CONFIG_AI_AGENT_LVGL_UI)
#include "ui/expression.h"
#include "ui/lvgl_ui_channel.h"
#include "ui/pet_care.h"
#include "ui/settings_page.h"
#include "core/care_log.h"     /* P1b care_list / care_ack */
#include "core/care_router.h"  /* P1b care_settings 运行时设置 */
#endif

/* 2026-11-02 health：把设备侧已有的健康数据打包成一条 JSON 回传手机。
 * 数据源全部是现有函数/话题，不新增采集逻辑：
 *   - 心率：hr_measure_last_bpm()（最近一次测量值，未测为 0）
 *   - 步数：uORB algo_pedometer.count（仅 CONFIG_UORB && CONFIG_FITNESS_ALGO）
 *   - 血氧：max30102.c 的 g_max30102_spo2_{pct,ok} 全局量（单次测量结果）
 *   - 电量：演示固定值 100（本板无电量计，见 handle_health）
 *   - 时间戳：time(NULL)
 * 读取发生在 BLE RX 回调线程（非 LVGL），毫秒级，不做任何阻塞操作。 */
#if defined(CONFIG_UORB) && defined(CONFIG_FITNESS_ALGO)
#include <poll.h>
#include <uORB/uORB.h>
#include <fitness/algo_pedometer.h>
#endif
#if defined(CONFIG_AI_AGENT_LVGL_UI)
#include "health/hr_measure.h"
#endif

#if defined(CONFIG_EXAMPLES_SENSOR_MAX30102)
extern float g_max30102_spo2_pct;
extern int   g_max30102_spo2_ok;
#endif

#define TAG "ble_cmd"

/* ★ 时间基准（2026-11-03 实测踩到）：
 * 手表系统时钟存的是「本地墙钟」= 真 UTC + AGENT_UTC_OFFSET_SEC（+8h），
 * 见 infra/time_sync.c 在 clock_settime 前加的偏移；而手机 App 用的是真
 * UTC epoch。BLE 协议统一用真 UTC：进来的 at 换成手表时钟，出去的
 * at/next/ts 换回真 UTC，否则 App 下发的闹钟会被手表判成"已过期"或晚 8h。 */
static long long app_from_watch_time(long long t)
{
    return t - (long long)AGENT_UTC_OFFSET_SEC;
}

static long long watch_from_app_time(long long t)
{
    return t + (long long)AGENT_UTC_OFFSET_SEC;
}

/* BLE 单帧上限：与 infra/ble_gatt.c 的 TX_SLOT_SIZE(1600) 对齐 —— 超过它的
 * 一帧会被直接 -EMSGSIZE 丢掉。NUS 的 (MTU-3) 分片由协议栈负责，这里只需
 * 保证每次 ble_gatt_send() 的长度不超过它。 */
#define BLE_TX_FRAME_MAX 1600

/* 回包里 items 数组的额度：留出外层对象的几十字节，整帧仍在 1600 以内 */
#define BLE_ITEMS_MAX 1400

/* P1b：care_list 回包可到 ~1.5KB。复用 BLE RX 缓冲做输出（BLE RX 线程
 * 串行，dispatch 返回前不会再收新包），避免为回包新增静态大缓冲。
 * 定义提前到 handlers 之前，recv 侧仍用同一块。 */
#define BLE_RX_BUF_SIZE 1600
static char     s_rx_buf[BLE_RX_BUF_SIZE];

/* chat 单条提问上限。RX 侧整包只收 512 字节，扣掉 JSON 外壳后单条文本最多
 * 也就 460 上下；取 384，既能装下长句，也不至于让 BT 栈线程的栈帧变胖。 */
#define BLE_TEXT_MAX 384

/* ── 回包出口 ─────────────────────────────────────────────────────
 * 所有回包（旧命令的 status 回包、v2 的 evt 事件、agent 的 chat 回答）都从
 * send_response() 出去，所以测试 sink 只需要改这一处。 */
static ble_test_sink_t s_test_sink;

void ble_cmd_handler_set_test_sink(ble_test_sink_t sink)
{
    s_test_sink = sink;
}

/* 返回送出结果：>0 = 交给 BLE 栈的字节数，0 = 已交给测试 sink，
 * <0 = 没送出去（未连接 / TX 队列满）。多数调用方不关心，chat 回答会记日志。 */
static int send_response(const char* json)
{
    if (json == NULL) {
        return -EINVAL;
    }

    /* 串口自测（NSH "ui ble ..."）时把回包交给 sink，不要求有手机连着 */
    if (s_test_sink != NULL) {
        s_test_sink(json);
        return 0;
    }

    return ble_gatt_send((const uint8_t*)json, (uint16_t)strlen(json));
}

/* ── JSON 工具 ──────────────────────────────────────────────────── */

/* 找到 "key" 对应值的起始位置（跳过冒号与空白），没有返回 NULL。
 * 不能只 strstr 一次：值的正文里也可能出现 "at" 这种字样（比如闹钟名就叫
 * "at"），所以每个命中点都要求后面（跳过空白）是 ':'，否则继续往后找。 */
static const char* json_find_value(const char* json, const char* key)
{
    char pattern[48];
    const char* p;
    size_t plen;

    if (json == NULL || key == NULL) {
        return NULL;
    }

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    plen = strlen(pattern);

    for (p = strstr(json, pattern); p != NULL; p = strstr(p + 1, pattern)) {
        const char* q = p + plen;

        while (*q == ' ' || *q == '\t') {
            q++;
        }
        if (*q != ':') {
            continue;
        }
        q++;
        while (*q == ' ' || *q == '\t') {
            q++;
        }
        return q;
    }

    return NULL;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* 把一个 Unicode 码点写成 UTF-8。返回字节数；代理区码点落成 U+FFFD
 * （成对的代理对不合并 —— App 的 Kotlin JSON 库对 CJK 直接吐 UTF-8） */
static int utf8_encode(unsigned int cp, char* out, size_t cap)
{
    if (cp >= 0xD800 && cp <= 0xDFFF) {
        cp = 0xFFFD;
    }

    if (cp < 0x80) {
        if (cap < 1) {
            return 0;
        }
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        if (cap < 2) {
            return 0;
        }
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        if (cap < 3) {
            return 0;
        }
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        if (cap < 4) {
            return 0;
        }
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }

    return 0;
}

/* 取字符串字段并做 JSON 反转义（引号/反斜杠/\/ \b \f \n \r \t \uXXXX）。
 * App 侧是标准 JSON 序列化：用户输入里的引号、换行会被转义，某些库连中文
 * 都写成 \uXXXX。不解回来，"他说\"你好\"" 会在第一个 \" 处被截断。 */
static int json_get_string(const char* json, const char* key,
    char* out, size_t out_size)
{
    const char* p = json_find_value(json, key);
    size_t n = 0;

    if (p == NULL) {
        return -ENOENT;
    }
    if (*p != '"') {
        return -EINVAL;
    }
    p++;

    while (*p != '\0' && *p != '"') {
        char ch = *p;

        if (ch == '\\') {
            char esc;

            p++;
            if (*p == '\0') {
                return -EINVAL;
            }
            esc = *p++;
            switch (esc) {
            case '"':
                ch = '"';
                break;
            case '\\':
                ch = '\\';
                break;
            case '/':
                ch = '/';
                break;
            case 'b':
                ch = '\b';
                break;
            case 'f':
                ch = '\f';
                break;
            case 'n':
                ch = '\n';
                break;
            case 'r':
                ch = '\r';
                break;
            case 't':
                ch = '\t';
                break;
            case 'u': {
                unsigned int cp = 0;
                char enc[4];
                int k;
                int len;

                for (k = 0; k < 4; k++) {
                    int hv = hex_val(*p);

                    if (hv < 0) {
                        return -EINVAL;
                    }
                    cp = (cp << 4) | (unsigned int)hv;
                    p++;
                }
                len = utf8_encode(cp, enc, sizeof(enc));
                if (len <= 0 || n + (size_t)len >= out_size) {
                    return -ENOSPC;
                }
                memcpy(out + n, enc, (size_t)len);
                n += (size_t)len;
                continue;
            }
            default:
                /* 未知转义：保留转义符后的那个字符，宁可原样也别丢内容 */
                ch = esc;
                break;
            }
        } else {
            p++;
        }

        if (n + 1 >= out_size) {
            return -ENOSPC;
        }
        out[n++] = ch;
    }

    if (*p != '"') {
        return -EINVAL;
    }

    out[n] = '\0';
    return 0;
}

/* 取整数字段。epoch 秒用 int64，不用 long：32 位目标上 long 到 2038 就溢出。
 * 手写十进制解析而不用 strtoll —— NuttX 的 strtoll 是否编进来依配置。 */
static int json_get_int64(const char* json, const char* key, int64_t* out)
{
    const char* p = json_find_value(json, key);
    int64_t v = 0;
    bool neg = false;
    int digits = 0;

    if (p == NULL) {
        return -ENOENT;
    }
    if (*p == '"') {
        p++; /* 容忍 "at":"1793682000" 这种带引号的写法 */
    }
    if (*p == '-') {
        neg = true;
        p++;
    }
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (int64_t)(*p - '0');
        p++;
        digits++;
    }
    if (digits == 0) {
        return -EINVAL;
    }

    *out = neg ? -v : v;
    return 0;
}

/* 取布尔字段：接受 1/0 与 true/false（App 侧两种都可能发） */
static int json_get_bool(const char* json, const char* key, bool* out)
{
    const char* p = json_find_value(json, key);

    if (p == NULL) {
        return -ENOENT;
    }
    if (*p == '1' || *p == 't' || *p == 'T') {
        *out = true;
        return 0;
    }
    if (*p == '0' || *p == 'f' || *p == 'F') {
        *out = false;
        return 0;
    }

    return -EINVAL;
}

/* JSON 字符串转义：引号/反斜杠/换行/回车/制表/其它控制字符转义，中文等
 * 非 ASCII 原样按 UTF-8 输出（98 号文要求）。按**整字符**截断，避免把
 * UTF-8 撕成半个。返回写入（dst 为 NULL 时为需要）的字节数，不写 NUL。
 *
 * 与 ui/lvgl_ui_channel.c 里的同名实现故意各留一份：两边各管一个方向的
 * 文本（一个给 App 回包、一个给历史导出），谁也不用把对方拉成依赖。 */
static size_t json_escape(char* dst, size_t cap, const char* src)
{
    const unsigned char* p = (const unsigned char*)src;
    size_t n = 0;

    if (src == NULL) {
        return 0;
    }

    while (*p != '\0') {
        unsigned char c = *p;
        char tmp[8];
        size_t need;
        size_t clen;

        /* 只为整字符截断，不校验 UTF-8 合法性 */
        if (c < 0x80) {
            clen = 1;
        } else if ((c & 0xE0) == 0xC0) {
            clen = 2;
        } else if ((c & 0xF0) == 0xE0) {
            clen = 3;
        } else if ((c & 0xF8) == 0xF0) {
            clen = 4;
        } else {
            clen = 1;
        }

        switch (c) {
        case '"':
            memcpy(tmp, "\\\"", 2);
            need = 2;
            break;
        case '\\':
            memcpy(tmp, "\\\\", 2);
            need = 2;
            break;
        case '\n':
            memcpy(tmp, "\\n", 2);
            need = 2;
            break;
        case '\r':
            memcpy(tmp, "\\r", 2);
            need = 2;
            break;
        case '\t':
            memcpy(tmp, "\\t", 2);
            need = 2;
            break;
        default:
            if (c < 0x20) {
                snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned int)c);
                need = 6;
            } else {
                need = clen; /* 原样（含多字节 UTF-8） */
            }
            break;
        }

        if (need > cap - n) { /* 按**剩余**空间判断，不能用原始 cap */
            break;
        }
        if (dst != NULL) {
            memcpy(dst + n, (need == clen) ? (const char*)p : tmp, need);
        }
        n += need;
        p += clen;
    }

    return n;
}

/* 转义并补 NUL；返回转义后长度（放不下时按整字符截断） */
static size_t json_escape_to(char* dst, size_t cap, const char* src)
{
    size_t n;

    if (dst == NULL || cap == 0) {
        return 0;
    }

    n = json_escape(dst, cap - 1, src);
    dst[n] = '\0';
    return n;
}

/* ── Agent 回答 -> App（evt=chat） ───────────────────────────────── */

void ble_send_chat_reply(const char* text, bool from_local)
{
    char* buf;
    size_t n;
    int prefix;

    if (text == NULL) {
        text = "";
    }

    /* 一次 ble_gatt_send() 就是一帧（≤1600B），NUS 的分片由协议栈处理。
     * 转义会让个别字符膨胀到 6 字节，所以不预分配"刚好"，而是按剩余空间
     * 边转义边截断 —— 发出去的永远是一条合法 JSON。 */
    buf = malloc(BLE_TX_FRAME_MAX + 1);
    if (buf == NULL) {
        syslog(LOG_ERR, "[%s] chat reply: out of memory\n", TAG);
        return;
    }

    prefix = snprintf(buf, BLE_TX_FRAME_MAX + 1,
        "{\"evt\":\"chat\",\"ok\":1,\"text\":\"");
    n = (size_t)prefix;

    /* 留 16 字节给 "\",\"local\":0}" 和结尾 NUL */
    n += json_escape(buf + n, (BLE_TX_FRAME_MAX + 1) - n - 16, text);
    snprintf(buf + n, (BLE_TX_FRAME_MAX + 1) - n, "\",\"local\":%d}",
        from_local ? 1 : 0);

    syslog(LOG_INFO, "[%s] chat reply (%u B, local=%d)\n", TAG,
        (unsigned)strlen(buf), from_local ? 1 : 0);

    /* 手机可能已经断开：这时回答发不出去，日志里要看得见（App 那边只是超时） */
    if (send_response(buf) < 0) {
        syslog(LOG_WARNING, "[%s] chat reply dropped: not connected\n", TAG);
    }
    free(buf);
}

/* ── health ─────────────────────────────────────────────────────── */

/* uORB 读一次（非阻塞优先）：订阅 → poll ≤100ms → copy → 退订。
 * 与 tools/tool_health.c 的 orb_read_once 同语义，但本地实现，避免跨文件
 * 依赖 static 符号。BLE 回调线程里最多阻塞 100ms，可接受。 */
#if defined(CONFIG_UORB)
static int health_orb_read_once(const struct orb_metadata* meta, void* buf)
{
    struct pollfd pfd;
    int fd;
    int ret = -1;
    int i;

    fd = orb_subscribe(meta);
    if (fd < 0) {
        return -1;
    }

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    for (i = 0; i < 10; i++) {
        ret = poll(&pfd, 1, 10);
        if (ret > 0) {
            ret = orb_copy(meta, fd, buf);
            if (ret == 0) {
                break;
            }
        }
    }

    orb_unsubscribe(fd);
    return (ret == 0) ? 0 : -1;
}
#endif

static void handle_wifi_config(const char* json)
{
    char ssid[64];
    char password[128];

    if (json_get_string(json, "ssid", ssid, sizeof(ssid)) != 0) {
        syslog(LOG_ERR, "[%s] wifi_config: missing ssid\n", TAG);
        send_response("{\"status\":\"error\",\"msg\":\"missing ssid\"}");
        return;
    }

    if (json_get_string(json, "password", password, sizeof(password)) != 0) {
        /* Allow empty password for open networks */
        password[0] = '\0';
    }

    syslog(LOG_INFO, "[%s] WiFi config: ssid=%s\n", TAG, ssid);

#ifdef CONFIG_AI_AGENT_WIFI
    int ret = network_wifi_connect(NULL, ssid, password);
    if (ret == 0) {
        syslog(LOG_INFO, "[%s] WiFi connected successfully\n", TAG);
        send_response("{\"status\":\"ok\",\"msg\":\"wifi connected\"}");
    } else {
        syslog(LOG_ERR, "[%s] WiFi connect failed: %d\n", TAG, ret);
        char resp[128];
        snprintf(resp, sizeof(resp),
            "{\"status\":\"error\",\"msg\":\"wifi failed: %d\"}", ret);
        send_response(resp);
    }
#else
    syslog(LOG_WARNING, "[%s] WiFi not supported on this build\n", TAG);
    send_response("{\"status\":\"error\",\"msg\":\"wifi not supported\"}");
#endif
}

static void handle_ping(void)
{
    send_response("{\"status\":\"ok\",\"msg\":\"pong\"}");
}

static void handle_status(void)
{
    char resp[256];
    bool net = network_is_connected();
    const char* ip = net ? network_get_ip() : "none";

    snprintf(resp, sizeof(resp),
        "{\"status\":\"ok\",\"network\":%s,\"ip\":\"%s\"}",
        net ? "true" : "false", ip ? ip : "none");
    send_response(resp);
}

/* 2026-11-02 health：一次返回手机 App 需要的全部实时健康数据。 */
static void handle_health(void)
{
    char resp[320];
    int hr = -1;
    int steps = -1;
    int spo2 = -1;
    int spo2_ok = 0;
    /* 电量：演示固定值 100 / 未充电（98 号文）。
     * 本板没有电量计，battery_state 话题没有发布者，读出来只会是 -1，
     * 手机上显示 "--" 反而像设备故障。读话题的那段代码随之删掉。 */
    int bat = 100;
    int chg = 0;
    /* 协议用真 UTC；手表系统时钟是 UTC+8 墙钟，回包前换回去 */
    long now = (long)app_from_watch_time((long long)time(NULL));

#if defined(CONFIG_AI_AGENT_LVGL_UI)
    hr = hr_measure_last_bpm();
#endif

#if defined(CONFIG_UORB) && defined(CONFIG_FITNESS_ALGO)
    {
        struct algo_pedometer pd;
        memset(&pd, 0, sizeof(pd));
        if (health_orb_read_once(ORB_ID(algo_pedometer), &pd) == 0) {
            steps = (int)pd.count;
        }
    }
#endif

#if defined(CONFIG_EXAMPLES_SENSOR_MAX30102)
    spo2_ok = g_max30102_spo2_ok ? 1 : 0;
    if (spo2_ok) {
        spo2 = (int)(g_max30102_spo2_pct + 0.5f);
    }
#endif

    snprintf(resp, sizeof(resp),
        "{\"ok\":1,\"hr\":%d,\"steps\":%d,\"spo2\":%d,\"spo2_ok\":%d,"
        "\"bat\":%d,\"chg\":%d,\"ts\":%ld}",
        hr, steps, spo2, spo2_ok, bat, chg, now);
    send_response(resp);
}

/* ── chat ───────────────────────────────────────────────────────── */

/* App 发来一句 -> 进 agent 入站队列 -> agent_loop -> outbound 派发到
 * channel=="ble" -> ble_send_chat_reply() 回 evt=chat。
 * 这条命令**当场不回包**：回答要等模型，可能几十秒（98 号文允许）。
 * 只有入队失败（agent 正忙）才立刻回 ok:0。 */
static void handle_chat(const char* json)
{
    char text[BLE_TEXT_MAX];
    agent_msg_t msg;
    int rc;

    rc = json_get_string(json, "text", text, sizeof(text));
    if (rc != 0 || text[0] == '\0') {
        syslog(LOG_WARNING, "[%s] chat: bad text (%d)\n", TAG, rc);
        send_response(rc == -ENOSPC
            ? "{\"evt\":\"chat\",\"ok\":0,\"msg\":\"text too long\"}"
            : "{\"evt\":\"chat\",\"ok\":0,\"msg\":\"missing text\"}");
        return;
    }

    memset(&msg, 0, sizeof(msg));
    strncpy(msg.channel, "ble", sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, "app", sizeof(msg.chat_id) - 1);
    msg.content = strdup(text);
    if (msg.content == NULL) {
        send_response("{\"evt\":\"chat\",\"ok\":0,\"msg\":\"out of memory\"}");
        return;
    }

    /* try：BLE 回调线程绝不等待。agent 队列满 = 上一轮还在跑，立刻告诉 App
     * "agent busy"，别让它干等 120 秒超时。 */
    rc = message_bus_try_push_inbound(&msg);
    if (rc != 0) {
        free(msg.content);
        syslog(LOG_WARNING, "[%s] chat: inbound queue busy (%d)\n", TAG, rc);
        send_response("{\"evt\":\"chat\",\"ok\":0,\"msg\":\"agent busy\"}");
        return;
    }

    /* 入队成功后 bus 接管 content。手表上同步显示用户气泡并记进历史 ——
     * App 与屏幕看到的是同一段对话。 */
#if defined(CONFIG_AI_AGENT_LVGL_UI)
    if (lvgl_ui_channel_send_user(text) != 0) {
        syslog(LOG_WARNING, "[%s] chat: ui bubble failed\n", TAG);
    }
#endif

    syslog(LOG_INFO, "[%s] chat queued (%u B)\n", TAG, (unsigned)strlen(text));
}

/* ── chat_history ───────────────────────────────────────────────── */

static void handle_chat_history(void)
{
#if defined(CONFIG_AI_AGENT_LVGL_UI)
    char* items;
    char* resp;
    int count;
    int written;

    items = malloc(BLE_ITEMS_MAX + 1);
    resp = malloc(BLE_ITEMS_MAX + 96);
    if (items == NULL || resp == NULL) {
        free(items);
        free(resp);
        send_response("{\"evt\":\"chat_history\",\"ok\":0,"
                      "\"msg\":\"out of memory\"}");
        return;
    }

    /* 最多 20 条；超出一帧额度时内部保留最新的若干条（最旧的先丢） */
    count = lvgl_ui_history_export(items, BLE_ITEMS_MAX + 1, 20);
    if (count < 0) {
        free(items);
        free(resp);
        syslog(LOG_WARNING, "[%s] chat_history: export failed (%d)\n",
            TAG, count);
        send_response("{\"evt\":\"chat_history\",\"ok\":0,"
                      "\"msg\":\"history unavailable\"}");
        return;
    }

    written = snprintf(resp, BLE_ITEMS_MAX + 96,
        "{\"evt\":\"chat_history\",\"ok\":1,\"items\":[%s]}", items);
    free(items);
    if (written > 0 && written < BLE_ITEMS_MAX + 96) {
        send_response(resp);
    }
    free(resp);
#else
    send_response("{\"evt\":\"chat_history\",\"ok\":0,\"msg\":\"ui unavailable\"}");
#endif
}

/* ── alarm_* ────────────────────────────────────────────────────── */

/* cron 任务 -> alarm_list 的一条。at 对一次性任务就是 at_epoch；对周期任务
 * 取下一次触发时间（没有"某一次"的概念，别编一个数出来）。 */
static void handle_alarm_list(void)
{
    cron_job_t* jobs;
    char* items;
    char* resp;
    size_t used = 0;
    int count;
    int i;
    int written;
    bool truncated = false;

    jobs = malloc(sizeof(cron_job_t) * AGENT_CRON_MAX_JOBS);
    items = malloc(BLE_ITEMS_MAX + 1);
    resp = malloc(BLE_ITEMS_MAX + 96);
    if (jobs == NULL || items == NULL || resp == NULL) {
        free(jobs);
        free(items);
        free(resp);
        send_response("{\"evt\":\"alarm_list\",\"ok\":0,\"msg\":\"out of memory\"}");
        return;
    }

    count = cron_list_jobs(jobs, AGENT_CRON_MAX_JOBS);
    items[0] = '\0';

    for (i = 0; i < count; i++) {
        cron_job_t* job = &jobs[i];
        char name[256];
        char one[352];
        int64_t at;
        int len;

        json_escape_to(name, sizeof(name), job->name);
        at = app_from_watch_time((job->kind == CRON_KIND_AT) ? job->at_epoch
                                                             : job->next_run);

        len = snprintf(one, sizeof(one),
            "%s{\"id\":\"%s\",\"name\":\"%s\",\"at\":%lld,\"next\":%lld,"
            "\"enabled\":%d,\"kind\":\"%s\"}",
            (used > 0) ? "," : "", job->id, name,
            (long long)at, (long long)app_from_watch_time(job->next_run),
            job->enabled ? 1 : 0,
            (job->kind == CRON_KIND_AT) ? "at" : "every");
        if (len <= 0 || (size_t)len >= sizeof(one)) {
            continue; /* 单条异常长：跳过，别把整条回包弄成非法 JSON */
        }
        if (used + (size_t)len + 1 > BLE_ITEMS_MAX) {
            /* 一帧装不下全部任务。宁可少列几条，也不能让 App 以为这就是
             * 完整的闹钟表 —— 回包里带 truncated 标记，由 App 决定怎么提示。 */
            syslog(LOG_WARNING, "[%s] alarm_list: %d/%d items fit in one frame\n",
                TAG, i, count);
            truncated = true;
            break;
        }

        memcpy(items + used, one, (size_t)len + 1);
        used += (size_t)len;
    }

    free(jobs);

    written = snprintf(resp, BLE_ITEMS_MAX + 96,
        "{\"evt\":\"alarm_list\",\"ok\":1,\"items\":[%s]%s}",
        items, truncated ? ",\"truncated\":1" : "");
    free(items);
    if (written > 0 && written < BLE_ITEMS_MAX + 96) {
        send_response(resp);
    }
    free(resp);
}

static void handle_alarm_add(const char* json)
{
    char name[32];
    char resp[192];
    cron_job_t* job;
    int64_t at = 0;
    long long now;
    int rc;
    int written;

    if (json_get_string(json, "name", name, sizeof(name)) != 0 ||
        name[0] == '\0') {
        send_response("{\"evt\":\"alarm_add\",\"ok\":0,\"msg\":\"missing name\"}");
        return;
    }

    if (json_get_int64(json, "at", &at) != 0 || at <= 0) {
        send_response("{\"evt\":\"alarm_add\",\"ok\":0,\"msg\":\"missing at\"}");
        return;
    }

    now = (long long)time(NULL);
    /* 只在校过时的前提下判"过去"：开机没网时 RTC 可能是 1970，拿它跟手机的
     * epoch 比较会把所有闹钟都误杀。真过期了也别静默建一个永不触发的任务，
     * 直接把手表当前时间回给 App，让它按手表时钟重算。 */
    if (now > 1600000000LL && watch_from_app_time(at) <= now) {
        written = snprintf(resp, sizeof(resp),
            "{\"evt\":\"alarm_add\",\"ok\":0,\"msg\":\"at in the past\","
            "\"now\":%lld}", app_from_watch_time(now));
        if (written > 0) {
            send_response(resp);
        }
        return;
    }

    job = malloc(sizeof(*job));
    if (job == NULL) {
        send_response("{\"evt\":\"alarm_add\",\"ok\":0,\"msg\":\"out of memory\"}");
        return;
    }

    memset(job, 0, sizeof(*job));
    strncpy(job->name, name, sizeof(job->name) - 1);
    /* 到点念出来的就是闹钟名；voice 通道 = 会念 + 上屏 + 进聊天历史 */
    strncpy(job->message, name, sizeof(job->message) - 1);
    strncpy(job->channel, "voice", sizeof(job->channel) - 1);
    strncpy(job->chat_id, "alarm", sizeof(job->chat_id) - 1);
    job->kind = CRON_KIND_AT;
    /* at 是 App 的真 UTC epoch；cron 用系统时钟（UTC+8 墙钟）比较 */
    job->at_epoch = watch_from_app_time(at);
    job->delete_after_run = true;

    rc = cron_add_job(job);
    if (rc != 0) {
        syslog(LOG_WARNING, "[%s] alarm_add: cron full (%d)\n", TAG, rc);
        free(job);
        send_response("{\"evt\":\"alarm_add\",\"ok\":0,\"msg\":\"alarm full\"}");
        return;
    }

    /* cron_add_job() 会把生成的 8 位 id 与 next_run 写回这份拷贝 */
    written = snprintf(resp, sizeof(resp),
        "{\"evt\":\"alarm_add\",\"ok\":1,\"id\":\"%s\",\"at\":%lld}",
        job->id, (long long)app_from_watch_time(job->at_epoch));
    free(job);

    if (written > 0 && (size_t)written < sizeof(resp)) {
        send_response(resp);
    }
}

static void handle_alarm_del(const char* json)
{
    char id[32];
    char resp[128];
    int written;

    if (json_get_string(json, "id", id, sizeof(id)) != 0 || id[0] == '\0') {
        send_response("{\"evt\":\"alarm_del\",\"ok\":0,\"msg\":\"missing id\"}");
        return;
    }

    if (cron_remove_job(id) != 0) {
        syslog(LOG_WARNING, "[%s] alarm_del: no such job %s\n", TAG, id);
        send_response("{\"evt\":\"alarm_del\",\"ok\":0,\"msg\":\"not found\"}");
        return;
    }

    written = snprintf(resp, sizeof(resp),
        "{\"evt\":\"alarm_del\",\"ok\":1,\"id\":\"%s\"}", id);
    if (written > 0 && (size_t)written < sizeof(resp)) {
        send_response(resp);
    }
}

/* ── expression_* ───────────────────────────────────────────────── */

static void send_expression_event(int index)
{
#if defined(CONFIG_AI_AGENT_LVGL_UI)
    char labels[256];
    char label[64];
    char resp[512];
    const expression_t* cur;
    int count;
    int i;
    int written;
    size_t used = 0;

    count = expression_count();
    if (count <= 0) {
        send_response("{\"evt\":\"expression\",\"ok\":0,"
                      "\"msg\":\"no expression\"}");
        return;
    }
    if (index < 0 || index >= count) {
        index = expression_current();
    }

    labels[0] = '\0';
    for (i = 0; i < count; i++) {
        char one[64];
        size_t need;

        json_escape_to(one, sizeof(one), expression_get(i)->label);
        /* 先算再写：宁可少给几个 label，也不能让 snprintf 截断出半截 JSON */
        need = strlen(one) + ((used > 0) ? 3 : 2);   /* 引号 + 可能的逗号 */
        if (used + need + 1 > sizeof(labels)) {
            break;
        }
        used += (size_t)snprintf(labels + used, sizeof(labels) - used,
            "%s\"%s\"", (used > 0) ? "," : "", one);
    }

    cur = expression_get(index);
    json_escape_to(label, sizeof(label), cur->label);

    written = snprintf(resp, sizeof(resp),
        "{\"evt\":\"expression\",\"ok\":1,\"index\":%d,\"count\":%d,"
        "\"label\":\"%s\",\"labels\":[%s]}",
        index, count, label, labels);
    if (written > 0 && (size_t)written < sizeof(resp)) {
        send_response(resp);
    }
#else
    (void)index;
    send_response("{\"evt\":\"expression\",\"ok\":0,\"msg\":\"ui unavailable\"}");
#endif
}

static void handle_expression_get(void)
{
#if defined(CONFIG_AI_AGENT_LVGL_UI)
    send_expression_event(expression_current());
#else
    send_expression_event(-1);
#endif
}

static void handle_expression_set(const char* json)
{
#if defined(CONFIG_AI_AGENT_LVGL_UI)
    char resp[160];
    int64_t index = -1;
    int count = expression_count();
    int written;

    if (json_get_int64(json, "index", &index) != 0) {
        send_response("{\"evt\":\"expression\",\"ok\":0,"
                      "\"msg\":\"missing index\"}");
        return;
    }
    if (count <= 0 || index < 0 || index >= (int64_t)count) {
        written = snprintf(resp, sizeof(resp),
            "{\"evt\":\"expression\",\"ok\":0,\"msg\":\"index out of range\","
            "\"count\":%d}", count);
        if (written > 0) {
            send_response(resp);
        }
        return;
    }

    /* expression_set() 内部有锁；索引没变时不碰 flash，变了才写 1 字节
     * /data/expression。表情的显示在页面重绘时读 expression_current()，
     * 与 NSH 的 "ui expression set" 行为一致。 */
    if (expression_set((int)index) != 0) {
        send_response("{\"evt\":\"expression\",\"ok\":0,\"msg\":\"set failed\"}");
        return;
    }

    send_expression_event((int)index);
#else
    (void)json;
    send_expression_event(-1);
#endif
}

/* ── settings_* ─────────────────────────────────────────────────── */

/* 回包一律以手表**真实状态**为准，不回显请求值：回显会让"设失败"看起来
 * 像成功。 */
static void send_settings_event(void)
{
    char resp[192];
    int written;

#if defined(CONFIG_AI_AGENT_LVGL_UI)
    written = snprintf(resp, sizeof(resp),
        "{\"evt\":\"settings\",\"ok\":1,\"brightness\":%d,"
        "\"brightness_max\":%d,\"dnd\":%d}",
        settings_page_brightness_get(),
        settings_page_brightness_max(),
        pet_care_get_dnd() ? 1 : 0);
#else
    written = snprintf(resp, sizeof(resp),
        "{\"evt\":\"settings\",\"ok\":0,\"msg\":\"ui unavailable\"}");
#endif

    if (written > 0 && (size_t)written < sizeof(resp)) {
        send_response(resp);
    }
}

static void handle_settings_get(void)
{
    send_settings_event();
}

static void handle_settings_set(const char* json)
{
#if defined(CONFIG_AI_AGENT_LVGL_UI)
    char resp[160];
    int64_t level = 0;
    bool dnd = false;
    bool did = false;
    int max;
    int applied;
    int written;

    if (json_get_int64(json, "brightness", &level) == 0) {
        max = settings_page_brightness_max();
        if (level < 0 || level > (int64_t)max) {
            written = snprintf(resp, sizeof(resp),
                "{\"evt\":\"settings\",\"ok\":0,"
                "\"msg\":\"brightness out of range\",\"brightness_max\":%d}",
                max);
            if (written > 0) {
                send_response(resp);
            }
            return;
        }

        /* FW-B 的 setter 自己把"刷 label"投递回 render 线程（LV_OS_NONE 下
         * 不能从 BLE 线程碰 LVGL），硬件亮度当场生效，所以在 BLE 回调线程
         * 直接调是安全的 —— 它内部就是 design 成给这里用的。 */
        applied = settings_page_brightness_set((int)level);
        if (applied < 0) {
            syslog(LOG_WARNING, "[%s] settings_set: brightness apply failed\n",
                TAG);
        }
        did = true;
    }

    if (json_get_bool(json, "dnd", &dnd) == 0) {
        /* 免打扰是纯内存 bool（pet_care.c），任意线程改都安全 */
        pet_care_set_dnd(dnd);
        did = true;
    }

    if (!did) {
        send_response("{\"evt\":\"settings\",\"ok\":0,"
                      "\"msg\":\"missing brightness/dnd\"}");
        return;
    }

    send_settings_event();
#else
    (void)json;
    send_settings_event();
#endif
}

/* ── care_*（PROACTIVE-P1B，docs_ble/110 §1）────────────────────
 * 协议：
 *   care_list   {"cmd":"care_list","limit":8}
 *     -> {"evt":"care_list","count":N,"items":[{"seq","ts","type","state",
 *         "decision","reason","action","feedback","score","hr","dev",
 *         "still_min","idle_s","hour","dnd","quiet"},...]}（按 seq 倒序）
 *   care_ack    {"cmd":"care_ack","seq":N,"feedback":"ACCEPTED|IGNORED|REJECTED"}
 *     -> {"evt":"care_ack","ok":1,"seq":N,"feedback":"..."}（不存在 ok=0）
 *   care_settings_get / care_settings_set（见下面 send/handle）
 *
 * 线程：BLE RX 回调线程，命令串行；care_log/care_router 内部只有短临界区/
 * 内存锁，不阻塞、不写盘。care_list 回包复用 s_rx_buf（1600B），单帧上限
 * 与 ble_gatt TX_SLOT 对齐；字段一样不少，超出时由 care_log 按字节预算截断
 * items（count 如实反映实际条数）。 */
#if defined(CONFIG_AI_AGENT_LVGL_UI)
static void handle_care_list(const char* json)
{
    int64_t v = 0;
    int limit = 6;
    int written;

    if (json_get_int64(json, "limit", &v) == 0) {
        if (v < 1) {
            v = 1;
        }
        /* P1c：docs110/112 验收按 6 条，且 BLE 单帧 1600B 典型只放得下
         * ~6 条；即使 App 仍传 8 也压到 6，保证 count≤6。 */
        if (v > 6) {
            v = 6;
        }
        limit = (int)v;
    }

    /* 复用 RX 缓冲作为回包输出：dispatch 期间不会再有 RX 写入。 */
    written = care_log_export_json(s_rx_buf, sizeof(s_rx_buf), limit,
        -(int32_t)AGENT_UTC_OFFSET_SEC);
    if (written > 0 && (size_t)written < sizeof(s_rx_buf)) {
        send_response(s_rx_buf);
    } else {
        syslog(LOG_WARNING, "[%s] care_list: export failed (%d)\n",
            TAG, written);
        send_response("{\"evt\":\"care_list\",\"ok\":0,"
                      "\"msg\":\"export failed\"}");
    }
}

static void handle_care_ack(const char* json)
{
    char resp[224];
    char feedback[16];
    int64_t seq = 0;
    int written;

    if (json_get_int64(json, "seq", &seq) != 0 || seq < 1
        || seq > 0xFFFFFFFFLL) {
        send_response("{\"evt\":\"care_ack\",\"ok\":0,"
                      "\"msg\":\"invalid seq\"}");
        return;
    }
    if (json_get_string(json, "feedback", feedback, sizeof(feedback)) != 0) {
        send_response("{\"evt\":\"care_ack\",\"ok\":0,"
                      "\"msg\":\"missing feedback\"}");
        return;
    }

    if (care_log_ack((uint32_t)seq, feedback)) {
        written = snprintf(resp, sizeof(resp),
            "{\"evt\":\"care_ack\",\"ok\":1,\"seq\":%lld,"
            "\"feedback\":\"%s\"}",
            (long long)seq, feedback);
    } else {
        written = snprintf(resp, sizeof(resp),
            "{\"evt\":\"care_ack\",\"ok\":0,\"seq\":%lld,"
            "\"msg\":\"seq not found or invalid feedback\"}",
            (long long)seq);
    }
    if (written > 0 && (size_t)written < sizeof(resp)) {
        send_response(resp);
    }
}

/* 回包一律以固件真实状态为准（不回声请求值）。 */
static void send_care_settings_event(void)
{
    char resp[224];
    int written;
    care_router_settings_t st;
    int dnd;

    care_router_get_settings(&st);
    dnd = pet_care_get_dnd() ? 1 : 0;

    written = snprintf(resp, sizeof(resp),
        "{\"evt\":\"care_settings\",\"ok\":1,\"dnd\":%d,\"voice\":%d,"
        "\"quiet_start\":%u,\"quiet_end\":%u,\"daily_budget\":%u,"
        "\"types\":{\"hr\":%d,\"idle\":%d,\"sedentary\":%d}}",
        dnd, st.voice ? 1 : 0,
        (unsigned)st.quiet_start, (unsigned)st.quiet_end,
        (unsigned)st.daily_budget,
        (st.types & CARE_ROUTER_TYPES_HR) ? 1 : 0,
        (st.types & CARE_ROUTER_TYPES_IDLE) ? 1 : 0,
        (st.types & CARE_ROUTER_TYPES_SEDENTARY) ? 1 : 0);

    if (written > 0 && (size_t)written < sizeof(resp)) {
        send_response(resp);
    }
}

static void handle_care_settings_set(const char* json)
{
    int64_t v = 0;
    int64_t tv = 0;
    bool b = false;
    bool dirty = false;
    bool router_dirty = false;
    bool dnd_dirty = false;
    bool dnd_val = false;
    care_router_settings_t cur;
    care_router_settings_t st;
    const char* pv;
    const char* types;

    care_router_get_settings(&cur);
    st = cur;

    /* ── 1) 解析 + 校验全部字段（此阶段**不产生任何副作用**）──
     * P1c：任一字段非法 -> ok=0 且不改任何值（含 DND）；
     * 全部合法后才在下面一次性应用（原子）。 */

    pv = json_find_value(json, "dnd");
    if (pv != NULL) {
        if (json_get_bool(json, "dnd", &b) != 0) {
            send_response("{\"evt\":\"care_settings\",\"ok\":0,"
                          "\"msg\":\"dnd must be 0/1 or true/false\"}");
            return;
        }
        dnd_dirty = true;
        dnd_val = b;
        dirty = true;
    }

    /* P2B-1A（task-65）：voice 开关，与其它字段一起"先校验后原子应用"；
     * 只接受 0/1 或 true/false，非法 -> 整包 ok=0 且不改任何值。 */
    pv = json_find_value(json, "voice");
    if (pv != NULL) {
        if (json_get_bool(json, "voice", &b) != 0) {
            send_response("{\"evt\":\"care_settings\",\"ok\":0,"
                          "\"msg\":\"voice must be 0/1 or true/false\"}");
            return;
        }
        st.voice = b;
        router_dirty = true;
        dirty = true;
    }

    pv = json_find_value(json, "quiet_start");
    if (pv != NULL) {
        if (json_get_int64(json, "quiet_start", &v) != 0
            || v < 0 || v > 23) {
            send_response("{\"evt\":\"care_settings\",\"ok\":0,"
                          "\"msg\":\"quiet_start out of range (0-23)\"}");
            return;
        }
        st.quiet_start = (uint8_t)v;
        router_dirty = true;
        dirty = true;
    }

    pv = json_find_value(json, "quiet_end");
    if (pv != NULL) {
        if (json_get_int64(json, "quiet_end", &v) != 0
            || v < 0 || v > 23) {
            send_response("{\"evt\":\"care_settings\",\"ok\":0,"
                          "\"msg\":\"quiet_end out of range (0-23)\"}");
            return;
        }
        st.quiet_end = (uint8_t)v;
        router_dirty = true;
        dirty = true;
    }

    pv = json_find_value(json, "daily_budget");
    if (pv != NULL) {
        if (json_get_int64(json, "daily_budget", &v) != 0
            || v < 1 || v > 12) {
            send_response("{\"evt\":\"care_settings\",\"ok\":0,"
                          "\"msg\":\"daily_budget out of range (1-12)\"}");
            return;
        }
        st.daily_budget = (uint8_t)v;
        router_dirty = true;
        dirty = true;
    }

    /* types 是嵌套对象：只接受 0/1；任一非法 -> 整包 ok=0（不改任何值） */
    types = json_find_value(json, "types");
    if (types != NULL) {
        uint8_t mask = st.types;
        bool any = false;

        if (*types != '{') {
            send_response("{\"evt\":\"care_settings\",\"ok\":0,"
                          "\"msg\":\"types must be an object\"}");
            return;
        }

        pv = json_find_value(types, "hr");
        if (pv != NULL) {
            if (json_get_int64(types, "hr", &tv) != 0
                || (tv != 0 && tv != 1)) {
                send_response("{\"evt\":\"care_settings\",\"ok\":0,"
                              "\"msg\":\"types.hr must be 0/1\"}");
                return;
            }
            if (tv != 0) {
                mask |= CARE_ROUTER_TYPES_HR;
            } else {
                mask &= (uint8_t)~CARE_ROUTER_TYPES_HR;
            }
            any = true;
        }

        pv = json_find_value(types, "idle");
        if (pv != NULL) {
            if (json_get_int64(types, "idle", &tv) != 0
                || (tv != 0 && tv != 1)) {
                send_response("{\"evt\":\"care_settings\",\"ok\":0,"
                              "\"msg\":\"types.idle must be 0/1\"}");
                return;
            }
            if (tv != 0) {
                mask |= CARE_ROUTER_TYPES_IDLE;
            } else {
                mask &= (uint8_t)~CARE_ROUTER_TYPES_IDLE;
            }
            any = true;
        }

        pv = json_find_value(types, "sedentary");
        if (pv != NULL) {
            if (json_get_int64(types, "sedentary", &tv) != 0
                || (tv != 0 && tv != 1)) {
                send_response("{\"evt\":\"care_settings\",\"ok\":0,"
                              "\"msg\":\"types.sedentary must be 0/1\"}");
                return;
            }
            if (tv != 0) {
                mask |= CARE_ROUTER_TYPES_SEDENTARY;
            } else {
                mask &= (uint8_t)~CARE_ROUTER_TYPES_SEDENTARY;
            }
            any = true;
        }

        if (any) {
            st.types = mask;
            router_dirty = true;
            dirty = true;
        }
    }

    if (!dirty) {
        send_response("{\"evt\":\"care_settings\",\"ok\":0,"
                      "\"msg\":\"missing care settings fields\"}");
        return;
    }

    /* ── 2) 全部字段合法：一次性原子应用（先 settings，再 DND）── */
    if (router_dirty && !care_router_set_settings(&st)) {
        send_response("{\"evt\":\"care_settings\",\"ok\":0,"
                      "\"msg\":\"settings rejected\"}");
        return;
    }

    if (dnd_dirty) {
        pet_care_set_dnd(dnd_val);   /* 与 settings_set 同一入口，双向同步 */
    }

    syslog(LOG_INFO, "[%s] care_settings_set atomic: dnd=%d voice=%d "
        "quiet=%u-%u budget=%u types=0x%x\n", TAG, (int)pet_care_get_dnd(),
        (int)st.voice, (unsigned)st.quiet_start, (unsigned)st.quiet_end,
        (unsigned)st.daily_budget, (unsigned)st.types);
    send_care_settings_event();
}

/* ── P2B-2A（task-68，docs_ble/114）：App care_event 场景 ────────────
 * 白名单：eye_rest/water/sleep/weather；未知 type -> ok=0。
 * BLE RX 线程只做门禁/记账/投递（pet_care_care_event 内部），UI/语音在
 * LVGL render 线程执行；suppressed=1 表示被 Router 门禁静默（无角标/历史/声）。 */
extern int pet_care_care_event(const char *type);

static void handle_care_event(const char* json)
{
    char type[16];
    char resp[128];
    int written;
    int rc;

    if (json_get_string(json, "type", type, sizeof(type)) != 0) {
        send_response("{\"evt\":\"care_event\",\"ok\":0,"
                      "\"msg\":\"missing type\"}");
        return;
    }

    if (strcmp(type, "eye_rest") != 0 && strcmp(type, "water") != 0
        && strcmp(type, "sleep") != 0 && strcmp(type, "weather") != 0) {
        send_response("{\"evt\":\"care_event\",\"ok\":0,"
                      "\"msg\":\"unknown type\"}");
        return;
    }

    rc = pet_care_care_event(type);
    if (rc < 0) {
        send_response("{\"evt\":\"care_event\",\"ok\":0,"
                      "\"msg\":\"internal error\"}");
        return;
    }

    written = snprintf(resp, sizeof(resp),
        "{\"evt\":\"care_event\",\"ok\":1,\"type\":\"%s\","
        "\"suppressed\":%d}", type, (rc == 1) ? 1 : 0);
    if (written > 0 && (size_t)written < sizeof(resp)) {
        send_response(resp);
    }
}
#else  /* !CONFIG_AI_AGENT_LVGL_UI */
static void handle_care_list(const char* json)
{
    (void)json;
    send_response("{\"evt\":\"care_list\",\"ok\":0,"
                  "\"msg\":\"ui unavailable\"}");
}

static void handle_care_ack(const char* json)
{
    (void)json;
    send_response("{\"evt\":\"care_ack\",\"ok\":0,"
                  "\"msg\":\"ui unavailable\"}");
}

static void send_care_settings_event(void)
{
    send_response("{\"evt\":\"care_settings\",\"ok\":0,"
                  "\"msg\":\"ui unavailable\"}");
}

static void handle_care_settings_set(const char* json)
{
    (void)json;
    send_care_settings_event();
}

static void handle_care_event(const char* json)
{
    (void)json;
    send_response("{\"evt\":\"care_event\",\"ok\":0,"
                  "\"msg\":\"ui unavailable\"}");
}
#endif /* CONFIG_AI_AGENT_LVGL_UI */

/* ── demo（DEMO-FW / DEMO-PHONE 2026-11-03）：手机 App 触发演示动作 ──
 * 演示视频里所有主动关怀都由手机点按触发，手表只负责响应：
 *   切到小云页（lvgl_ui_channel_navigate_pet）→ 气泡/历史 → 本地预合成音频。
 * 动作表：
 *   {"cmd":"demo","action":"hr_alert"}              心率过高告警
 *   {"cmd":"demo","action":"greet"}                 主动打招呼（晚间问候）
 *   {"cmd":"demo","action":"water"}                 喝水提醒
 *   {"cmd":"demo","action":"sleep"}                 该睡觉了
 *   {"cmd":"demo","action":"weather"}               天气变化提醒
 *   {"cmd":"demo","action":"sedentary"}             久坐提醒
 *   {"cmd":"demo","action":"milestone"}             步数里程碑（真实步数，无预合成音频）
 *   {"cmd":"demo","action":"alarm","seconds":10}    10s 后一次性 cron voice 闹钟
 * 线程：本文件在 BLE RX 回调线程跑；care API 只做投递/内存锁操作，cron
 * 落盘与现有 alarm_add 同一条路径（已在真机用过），不阻塞 BT 栈线程。
 * 切页由 pet_care demo 钩子内部异步投递，不在这里碰 LVGL。
 * 需要 LVGL UI 构建（pet_care demo 钩子只在该配置下编进来）。 */
static void handle_demo(const char* json)
{
#if defined(CONFIG_AI_AGENT_LVGL_UI)
    char action[24];
    char resp[160];
    int written;

    if (json_get_string(json, "action", action, sizeof(action)) != 0) {
        send_response("{\"evt\":\"demo\",\"ok\":0,"
                      "\"msg\":\"missing action\"}");
        return;
    }

    /* 2026-11-03 紧急修复：不在 BLE RX 回调线程直接跑 care/audio，
     * 统一投递给 pet_care 的 ai_agent worker；回包只表示已受理。 */
    if (strcmp(action, "hr_alert") == 0) {
        pet_care_demo_async(PET_CARE_DEMO_HR, 0);
    } else if (strcmp(action, "greet") == 0) {
        pet_care_demo_async(PET_CARE_DEMO_GREET, 0);
    } else if (strcmp(action, "water") == 0) {
        pet_care_demo_async(PET_CARE_DEMO_WATER, 0);
    } else if (strcmp(action, "sleep") == 0) {
        pet_care_demo_async(PET_CARE_DEMO_SLEEP, 0);
    } else if (strcmp(action, "weather") == 0) {
        pet_care_demo_async(PET_CARE_DEMO_WEATHER, 0);
    } else if (strcmp(action, "sedentary") == 0) {
        pet_care_demo_async(PET_CARE_DEMO_SEDENTARY, 0);
    } else if (strcmp(action, "milestone") == 0) {
        pet_care_demo_async(PET_CARE_DEMO_MILESTONE, 0);
    } else if (strcmp(action, "alarm") == 0) {
        int64_t seconds = 10;

        (void)json_get_int64(json, "seconds", &seconds);
        if (seconds <= 0 || seconds > 3600) {
            send_response("{\"evt\":\"demo\",\"ok\":0,"
                          "\"msg\":\"bad seconds\"}");
            return;
        }
        pet_care_demo_async(PET_CARE_DEMO_ALARM, (int)seconds);
    } else {
        send_response("{\"evt\":\"demo\",\"ok\":0,"
                      "\"msg\":\"unknown action\"}");
        return;
    }

    written = snprintf(resp, sizeof(resp),
        "{\"evt\":\"demo\",\"ok\":1,\"action\":\"%s\"}", action);
    if (written > 0 && (size_t)written < sizeof(resp)) {
        send_response(resp);
    }
#else
    (void)json;
    send_response("{\"evt\":\"demo\",\"ok\":0,\"msg\":\"ui unavailable\"}");
#endif
}

/* -- Public API ---------------------------------------------- */

static void dispatch_json(char *buf)
{
    /* Extract command */
    char cmd[32];
    if (json_get_string(buf, "cmd", cmd, sizeof(cmd)) != 0) {
        syslog(LOG_WARNING, "[%s] No 'cmd' field in message\n", TAG);
        send_response("{\"status\":\"error\",\"msg\":\"missing cmd\"}");
        return;
    }

    if (strcmp(cmd, "wifi_config") == 0) {
        handle_wifi_config(buf);
    } else if (strcmp(cmd, "ping") == 0) {
        handle_ping();
    } else if (strcmp(cmd, "status") == 0) {
        handle_status();
    } else if (strcmp(cmd, "health") == 0) {
        handle_health();
    } else if (strcmp(cmd, "chat") == 0) {
        handle_chat(buf);
    } else if (strcmp(cmd, "chat_history") == 0) {
        handle_chat_history();
    } else if (strcmp(cmd, "alarm_list") == 0) {
        handle_alarm_list();
    } else if (strcmp(cmd, "alarm_add") == 0) {
        handle_alarm_add(buf);
    } else if (strcmp(cmd, "alarm_del") == 0) {
        handle_alarm_del(buf);
    } else if (strcmp(cmd, "expression_get") == 0) {
        handle_expression_get();
    } else if (strcmp(cmd, "expression_set") == 0) {
        handle_expression_set(buf);
    } else if (strcmp(cmd, "settings_get") == 0) {
        handle_settings_get();
    } else if (strcmp(cmd, "settings_set") == 0) {
        handle_settings_set(buf);
    } else if (strcmp(cmd, "care_list") == 0) {
        handle_care_list(buf);              /* PROACTIVE-P1B */
    } else if (strcmp(cmd, "care_ack") == 0) {
        handle_care_ack(buf);               /* PROACTIVE-P1B */
    } else if (strcmp(cmd, "care_settings_get") == 0) {
        send_care_settings_event();         /* PROACTIVE-P1B */
    } else if (strcmp(cmd, "care_settings_set") == 0) {
        handle_care_settings_set(buf);      /* PROACTIVE-P1B */
    } else if (strcmp(cmd, "care_event") == 0) {
        handle_care_event(buf);             /* P2B-2A docs114 场景事件 */
    } else if (strcmp(cmd, "demo") == 0) {
        handle_demo(buf);   /* DEMO-FW：演示触发（hr_alert/greet/alarm） */
    } else {
        syslog(LOG_WARNING, "[%s] Unknown command: %s\n", TAG, cmd);
        char resp[128];
        snprintf(resp, sizeof(resp),
            "{\"status\":\"error\",\"msg\":\"unknown cmd: %s\"}", cmd);
        send_response(resp);
    }
}

/****************************************************************************
 * BLE RX 拼包
 *
 * Android GATT write 单帧最大 (MTU-3) 字节，长命令（chat/alarm_add 的 JSON
 * 超过 20~200 字节）由 App 切片发送。原 recv 把每次写当一条完整 JSON，
 * 切片后会变成多条坏命令；这里按花括号配对（忽略字符串内的括号）拼回完整
 * JSON 再 dispatch，同时兼容旧的"整条一次写"。
 ****************************************************************************/

static uint16_t s_rx_len;
static int      s_rx_depth;
static bool     s_rx_in_str;
static bool     s_rx_esc;

void ble_cmd_handler_recv(const uint8_t* data, uint16_t len, void* user_data)
{
    uint16_t i;

    (void)user_data;

    if (data == NULL || len == 0) {
        return;
    }

    for (i = 0; i < len; i++) {
        char ch = (char)data[i];

        if (s_rx_len == 0) {
            if (ch != '{') {
                continue;
            }
            s_rx_buf[s_rx_len++] = ch;
            s_rx_depth = 1;
            s_rx_in_str = false;
            s_rx_esc = false;
            continue;
        }

        if (s_rx_len >= BLE_RX_BUF_SIZE - 1) {
            syslog(LOG_WARNING, "[%s] RX JSON too long (>%d), dropped\n",
                TAG, BLE_RX_BUF_SIZE - 1);
            s_rx_len = 0;
            s_rx_depth = 0;
            s_rx_in_str = false;
            s_rx_esc = false;
            continue;
        }

        s_rx_buf[s_rx_len++] = ch;

        if (s_rx_in_str) {
            if (s_rx_esc) {
                s_rx_esc = false;
            } else if (ch == '\\') {
                s_rx_esc = true;
            } else if (ch == '"') {
                s_rx_in_str = false;
            }
            continue;
        }

        if (ch == '"') {
            s_rx_in_str = true;
        } else if (ch == '{') {
            s_rx_depth++;
        } else if (ch == '}') {
            s_rx_depth--;
            if (s_rx_depth == 0) {
                s_rx_buf[s_rx_len] = '\0';
                syslog(LOG_INFO, "[%s] Received(%u): %s\n", TAG,
                    (unsigned)s_rx_len, s_rx_buf);
                dispatch_json(s_rx_buf);
                s_rx_len = 0;
                s_rx_in_str = false;
                s_rx_esc = false;
            }
        }
    }
}
