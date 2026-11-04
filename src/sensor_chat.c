/* sensor_chat.c —— 语音对话控灯 + 传感器问答（AI-VOX3 / openvela NuttX）
 *
 * 设计：
 *   1. 在 message 入队咽喉点挂钩（见 PATCH_GUIDE.md），用户说话文本进过
 *      sensor_chat_handle_text()：命中硬件指令就本地执行 + 播报，不再送 LLM；
 *      未命中原样送 LLM，普通聊天完全不受影响。
 *   2. 后台线程 50ms 轮询摇晃/触摸引脚，边沿触发时播报（带冷却），状态缓存
 *      供随时查询。
 *
 * 线程安全：LED 动作只在 handle_text（单线程入队路径）里发生；轮询线程只读
 * 传感器引脚、只写缓存。全部用 volatile + 简单临界区足够。
 */
#include <nuttx/config.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "sensor_chat.h"

#define TAG "SENSOR"

/* 与 voice_channel.c 相同的 arch GPIO 接口（libarch.a 提供）。
 * 若编译链接报符号错误，按 PATCH_GUIDE.md 核对 esp32s3_gpio.h 的原型。 */
extern int  esp32s3_gpioread(int pin);
extern int  esp32s3_configgpio(uint32_t pin, uint16_t attr);
extern void esp32s3_gpiowrite(int pin, int value);

/* voice_channel.c 的同步播报（阻塞至播完），本模块在独立线程里调用 */
extern int  voice_channel_speak(const char *text);
/* [V1042-TOUCHLISTEN] BOOT 键死亡时的等效触发（ai_vox_main.cpp 提供） */
extern void aivox_simulate_boot_click(void);

/* ── 状态 ───────────────────────────────────────────────── */
static volatile int s_led_on   = 0;
static volatile int s_shake_lv = 0;
static volatile int s_touch_lv = 0;
static volatile int s_announce = 0;          /* 传感器触发自动播报（默认关：插拔传感器不播报，说"打开播报"可开） */
static int  s_inited = 0;

/* ── 小工具 ─────────────────────────────────────────────── */
/* [V1228] 触摸片空闲电平（开机实测）。TTP223 在本板是"空闲高、按下低"，
 * 写死 active-high 会让触摸永远不触发；改成与硬件极性解耦。 */
static int s_touch_idle = -1;

static int read_active_raw(int pin, int active_high)
{
    int lv = esp32s3_gpioread(pin) ? 1 : 0;
    return active_high ? lv : !lv;
}

static int touch_pressed(void)
{
    int raw = esp32s3_gpioread(SC_PIN_TOUCH) ? 1 : 0;
    if (s_touch_idle < 0)
      {
        return read_active_raw(SC_PIN_TOUCH, SC_TOUCH_ACTIVE);
      }
    return raw != s_touch_idle;
}

static int read_active(int pin, int active_high)
{
    int lv = esp32s3_gpioread(pin) ? 1 : 0;
    return active_high ? lv : !lv;
}

static void led_set(int on)
{
    int lv = (SC_LED_ACTIVE_HIGH ? (on ? 1 : 0) : (on ? 0 : 1));
    esp32s3_gpiowrite(SC_PIN_LED, lv);
    s_led_on = on ? 1 : 0;
    syslog(LOG_INFO, "[%s] LED -> %s (gpio%d=%d)\n",
           TAG, on ? "ON" : "OFF", SC_PIN_LED, lv);
}

/* ── 异步播报兜底线程 ───────────────────────────────────── */
/* voice_channel_speak 里有 HTTP + 音频播放，栈给足（ai_agent 主线程 32K） */
#define SC_SPEAK_STACK 16384
#define SC_POLL_STACK  4096

static void *speak_task(void *arg)
{
    char *text = (char *)arg;
    if (text) {
        voice_channel_speak(text);
        free(text);
    }
    return NULL;
}

void sensor_chat_reply_async(const char *text)
{
    if (!text || !*text) {
        return;
    }
    char *copy = strdup(text);
    if (!copy) {
        return;
    }
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, SC_SPEAK_STACK);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, speak_task, copy) != 0) {
        syslog(LOG_ERR, "[%s] reply thread failed\n", TAG);
        free(copy);
    }
    pthread_attr_destroy(&attr);
}

/* ── 传感器轮询：边沿检测 + 播报冷却 ─────────────────────── */
#define SC_POLL_MS       50
#define SC_DEBOUNCE_N    3        /* 连续 3 次一致才认（150ms） */
#define SC_ANNOUNCE_COOL 8        /* 同一传感器播报冷却秒数 */

static void *poll_task(void *arg)
{
    (void)arg;
    int shake_c = 0, touch_c = 0;          /* 去抖计数 */
    int shake = read_active(SC_PIN_SHAKE, SC_SHAKE_ACTIVE);
    int touch = touch_pressed();
    time_t shake_last_say = 0, touch_last_say = 0;
    int spk_tick = 0;                      /* [SPK-FILE] 每 500ms 查一次 */

    s_shake_lv = shake;
    s_touch_lv = touch;

    for (;;) {
        usleep(SC_POLL_MS * 1000);

        /* [SPK-FILE] 文件触发的本地播报通道（不依赖网络/云端）：
         *   nsh:  echo "文本" > /data/spk.txt
         * 读到就播报并删除文件，用于确定性验证本地 TTS 与内容级禁播。 */
        if (++spk_tick >= 10) {
            spk_tick = 0;
            int sfd = open("/data/spk.txt", O_RDONLY);
            if (sfd >= 0) {
                char sb[128];
                ssize_t srn = read(sfd, sb, sizeof(sb) - 1);
                close(sfd);
                unlink("/data/spk.txt");
                if (srn > 0) {
                    sb[srn] = '\0';
                    while (srn > 0 && (sb[srn - 1] == '\n' ||
                           sb[srn - 1] == '\r' || sb[srn - 1] == ' ')) {
                        sb[--srn] = '\0';
                    }
                    if (sb[0]) {
                        syslog(LOG_INFO, "[%s] [SPK-FILE] speak '%s'\n", TAG, sb);
                        sensor_chat_reply_async(sb);
                    }
                }
            }
        }

        /* [V1230-LISTENFILE] 文件触发：往 /tmp/listen.txt 写任意内容，
         * 下一轮轮询即等效按一次 BOOT（进聆听）。BOOT 键电气死亡、触摸
         * 板极性还会漂移，这条通道让整条语音链路可以在没有任何人碰板的
         * 情况下被自动化验证。 */
        {
            static int v1230_tick = 0;

            if ((v1230_tick++ % 10) == 0 &&
                access("/tmp/listen.txt", F_OK) == 0)
            {
                unlink("/tmp/listen.txt");
                syslog(LOG_INFO, "[%s] [V1230] LISTENFILE -> boot click\n", TAG);
                aivox_simulate_boot_click();
            }
        }

        int s_raw = read_active(SC_PIN_SHAKE, SC_SHAKE_ACTIVE);
        int t_raw = touch_pressed();

        /* 摇晃：去抖后沿触发 */
        if (s_raw != shake) {
            if (++shake_c >= SC_DEBOUNCE_N) {
                shake = s_raw;
                shake_c = 0;
                s_shake_lv = shake;
                syslog(LOG_INFO, "[%s] shake -> %s\n",
                       TAG, shake ? "TRIGGERED" : "idle");
                if (shake && s_announce &&
                    time(NULL) - shake_last_say >= SC_ANNOUNCE_COOL) {
                    shake_last_say = time(NULL);
                    sensor_chat_reply_async("检测到摇晃");
                }
            }
        } else {
            shake_c = 0;
        }

        /* 触摸：去抖后沿触发 */
        if (t_raw != touch) {
            if (++touch_c >= SC_DEBOUNCE_N) {
                touch = t_raw;
                touch_c = 0;
                s_touch_lv = touch;
                syslog(LOG_INFO, "[%s] touch -> %s (raw=%d idle=%d)\n",
                       TAG, touch ? "TOUCHED" : "idle",
                       esp32s3_gpioread(SC_PIN_TOUCH) ? 1 : 0, s_touch_idle);
                /* [V1042-TOUCHLISTEN 2026-10-29] BOOT 键电气死亡：触摸板
                 * 上升沿 = 等效 BOOT 单击（待机→开始聆听；聆听中再摸→
                 * 立刻收句；播报中摸→打断）。原"触摸板被摸到了"语音播报
                 * 移除，避免与聆听会话互相打断。 */
                if (touch) {
                    touch_last_say = time(NULL);
                    syslog(LOG_INFO, "[%s] touch -> BOOT click (listen)\n", TAG);
                    aivox_simulate_boot_click();
                }
            }
        } else {
            touch_c = 0;
        }
    }
    return NULL;
}

/* ── 中文指令识别（UTF-8 子串匹配，顺序敏感：先查询后动作） ── */
static int has(const char *t, const char *kw)
{
    return strstr(t, kw) != NULL;
}

static int is_query_word(const char *t)
{
    return has(t, "吗") || has(t, "？") || has(t, "?") ||
           has(t, "状态") || has(t, "情况") || has(t, "亮不亮") ||
           has(t, "有没有") || has(t, "多少");
}

int sensor_chat_handle_text(const char *text, char *reply, size_t cap)
{
    char tmp[256];

    /* [V1042-TOUCHLISTEN] UDP 发"开始聆听" = 等效 BOOT 单击（备用触发通道） */
    if (has(text, "开始聆听")) {
        aivox_simulate_boot_click();
        snprintf(reply, cap, "好的，开始聆听");
        return 1;
    }
    const char *t = text;

    if (!t || !*t) {
        return 0;
    }
    /* 懒初始化兜底：即使 init 调用点没挂上，第一条消息也能把模块拉起来 */
    if (!s_inited) {
        sensor_chat_init();
    }
    /* 统一截断到本地缓冲，避免超长文本 */
    strncpy(tmp, t, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    t = tmp;

    #define REPLY(fmt, ...) do { \
        if (reply && cap) { snprintf(reply, cap, fmt, ##__VA_ARGS__); } \
    } while (0)

    /* ---- 0. [ECHO-TEST] 复读：<文本> ---- */
    if (strncmp(t, "复读：", 9) == 0 || strncmp(t, "复读:", 7) == 0) {
        const char *p = (t[6] == ':') ? (t + 7) : (t + 9);
        if (*p) {
            REPLY("%s", p);
            return 1;
        }
    }

    /* ---- 1. 播报开关 ---- */
    if (has(t, "打开播报") || has(t, "开启播报") || has(t, "开播报")) {
        s_announce = 1;
        REPLY("好嘞，传感器一有动静我就告诉你");
        return 1;
    }
    if (has(t, "关闭播报") || has(t, "关播报") || has(t, "停止播报")) {
        s_announce = 0;
        REPLY("行，传感器有动静我就不吵你啦");
        return 1;
    }

    /* ---- 2. 摇晃 / 触摸 查询（提到就报状态） ---- */
    if (has(t, "摇晃") || has(t, "晃动") || has(t, "震动") || has(t, "摇一摇") ||
        has(t, "摇") || has(t, "晃") || has(t, "震")) {
        REPLY("数字摇晃传感器%s（引脚电平%s）",
              s_shake_lv ? "已触发" : "未触发",
              s_shake_lv ? "高" : "低");
        return 1;
    }
    if (has(t, "触摸") || has(t, "摸一下") || has(t, "摸摸") || has(t, "摸")) {
        REPLY("触摸板%s（引脚电平%s）",
              s_touch_lv ? "被触摸" : "未触摸",
              s_touch_lv ? "高" : "低");
        return 1;
    }

    /* ---- 3. 全量状态汇报 ---- */
    if (has(t, "汇报") || has(t, "所有传感器") || has(t, "传感器") ||
        has(t, "状态") || has(t, "情况")) {
        REPLY("台灯%s；数字摇晃传感器%s；触摸板%s",
              s_led_on ? "亮着" : "熄灭",
              s_shake_lv ? "已触发" : "未触发",
              s_touch_lv ? "被触摸" : "未触摸");
        return 1;
    }

    /* ---- 4. LED 查询（"灯亮着吗/开了吗/状态" 报状态，不动作） ---- */
    if (has(t, "灯") && is_query_word(t)) {
        REPLY("台灯现在%s", s_led_on ? "亮着" : "关着");
        return 1;
    }

    /* ---- 5. LED 开 / 关（意图识别，兼容多种口语：
     *        开灯/亮灯/灯亮/点亮/亮起来/打开灯/让LED灯亮起来/点亮led灯…） ---- */
    {
        int has_led = has(t, "灯") || has(t, "LED") || has(t, "led");
        if (has_led) {
            int off = has(t, "关") || has(t, "灭") || has(t, "熄") ||
                      has(t, "关闭") || has(t, "关掉") || has(t, "断");
            int on  = has(t, "开") || has(t, "亮") || has(t, "点") ||
                      has(t, "启") || has(t, "打开") || has(t, "开启");
            if (off && !on) {
                led_set(0);
                /* [GENTLE-LAMP 2026-10-27] 用户主动下指令 = 有人在场，
                 * 立即解除开机 30 s 硬静音（g_vox_audio_mute_until_ms 定义在
                 * ai_vox_main.cpp）。否则回复会被 [BOOT-HAL-MUTE] 吞掉，
                 * 现象就是"灯关了但一声不吭"。置 0 后本次开机不再恢复。 */
                {
                    extern int64_t g_vox_audio_mute_until_ms;
                    g_vox_audio_mute_until_ms = 0;
                }
                REPLY("好的这就为您关闭台灯。");
                return 1;
            }
            if (on) {
                led_set(1);
                /* [GENTLE-LAMP 2026-10-27] 同上：主动指令 -> 解除开机硬静音 */
                {
                    extern int64_t g_vox_audio_mute_until_ms;
                    g_vox_audio_mute_until_ms = 0;
                }
                REPLY("好的这就为您点亮台灯。");
                return 1;
            }
        }
    }

    #undef REPLY
    return 0;   /* 不是硬件指令 -> 走 LLM 正常聊天 */
}

/* ── 状态查询接口 ───────────────────────────────────────── */
int sensor_chat_led_state(void)   { return s_led_on; }
int sensor_chat_shake_level(void) { return s_shake_lv; }
int sensor_chat_touch_level(void) { return s_touch_lv; }


/* ── [UDP-CTRL] 本地 UDP 3336 控制通道 ─────────────────────
 * 目的：让"让台灯亮起来/关闭台灯"这类指令不必依赖云端 STT 也能触发，
 * 便于本地验证与集成。收到文本后走的是和语音完全相同的
 * sensor_chat_handle_text() 路径，行为一致。
 * 报文：任意 UTF-8 文本（如 "让台灯亮起来" / "关闭台灯" / "灯亮着吗"）
 * 回包：命中 -> 中文回复文案；未命中 -> "not a hardware command"
 */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SC_UDP_PORT  3336
#define SC_UDP_STACK 4096   /* [STKFIX] 8KB 分配失败 -> 降 4KB */

static void *udp_task(void *arg)
{
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        syslog(LOG_ERR, "[%s] udp socket failed\n", TAG);
        return NULL;
    }
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(SC_UDP_PORT);
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        syslog(LOG_ERR, "[%s] udp bind :%d failed\n", TAG, SC_UDP_PORT);
        close(fd);
        return NULL;
    }
    syslog(LOG_INFO, "[%s] udp ctrl listening :%d\n", TAG, SC_UDP_PORT);

    char buf[256];
    char reply[160];
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&from, &flen);
        if (n <= 0) {
            continue;
        }
        buf[n] = '\0';
        while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n')) {
            buf[--n] = '\0';
        }
        syslog(LOG_INFO, "[%s] udp rx '%s' from %s\n", TAG, buf,
               inet_ntoa(from.sin_addr));

        /* [ECHO-TEST] UDP 发 "复读：<文本>" 让板子本地播报该文本。
         * 用途：确定性验证内容级禁播（NOWEATHER-TTS）与本地 TTS 后端。
         * 仅由 UDP 显式触发，语音/控灯正常路径完全不受影响。 */
        {
            const char *ep = NULL;
            if (strncmp(buf, "复读：", 9) == 0) {
                ep = buf + 9;
            } else if (strncmp(buf, "复读:", 7) == 0) {
                ep = buf + 7;
            }
            if (ep != NULL && *ep) {
                syslog(LOG_INFO, "[%s] [ECHO-TEST] speak '%s'\n", TAG, ep);
                sensor_chat_reply_async(ep);
                const char *eo = "[ECHO-TEST] sent";
                sendto(fd, eo, strlen(eo), 0, (struct sockaddr *)&from, flen);
                continue;
            }
        }

        reply[0] = '\0';
        int hit = sensor_chat_handle_text(buf, reply, sizeof(reply));
        syslog(LOG_INFO, "[%s] udp hit=%d led=%d\n", TAG, hit, s_led_on);

        /* [UDP-SPEAK] UDP 命中本地指令时也走本地播报（与语音 LAMP-STT 行为一致）。
         * 语音路径由 ai_vox stt 分支负责播报、不经过这里，无双播风险。 */
        if (hit && reply[0]) {
            sensor_chat_reply_async(reply);
        }

        const char *out = (hit && reply[0]) ? reply : "not a hardware command";
        sendto(fd, out, strlen(out), 0, (struct sockaddr *)&from, flen);
    }
    return NULL;
}

/* ── 初始化 ─────────────────────────────────────────────── */
int sensor_chat_init(void)
{
    if (s_inited) {
        return 0;
    }

    /* LED：输出，开机熄灭 */
    esp32s3_configgpio(SC_PIN_LED, SC_ATTR_OUT);
    led_set(0);

    /* 传感器：输入 + 上拉 */
    esp32s3_configgpio(SC_PIN_SHAKE, SC_ATTR_IN_PULLUP);
    esp32s3_configgpio(SC_PIN_TOUCH, SC_ATTR_IN_PULLUP);

    /* [V1228] 校准触摸片空闲电平 */
    {
        int hi = 0;
        int i;
        for (i = 0; i < 12; i++) {
            if (esp32s3_gpioread(SC_PIN_TOUCH)) {
                hi++;
            }
            usleep(25 * 1000);
        }
        s_touch_idle = (hi >= 6) ? 1 : 0;
        syslog(LOG_WARNING,
               "[%s] [V1228] touch calibrated idle=%d press=%d (hi=%d/12)\n",
               TAG, s_touch_idle, !s_touch_idle, hi);
    }

    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, SC_POLL_STACK);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, poll_task, NULL) != 0) {
        syslog(LOG_ERR, "[%s] poll thread failed\n", TAG);
        pthread_attr_destroy(&attr);
        return -1;
    }
    pthread_attr_destroy(&attr);


    /* [UDP-CTRL] 本地 UDP 控制线程 */
    {
        pthread_t uth;
        pthread_attr_t uattr;
        pthread_attr_init(&uattr);
        pthread_attr_setstacksize(&uattr, SC_UDP_STACK);
        pthread_attr_setdetachstate(&uattr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&uth, &uattr, udp_task, NULL) != 0) {
            syslog(LOG_ERR, "[%s] udp thread failed\n", TAG);
        }
        pthread_attr_destroy(&uattr);
    }
    s_inited = 1;
    syslog(LOG_INFO,
           "[%s] init OK: LED=GPIO%d shake=GPIO%d touch=GPIO%d\n",
           TAG, SC_PIN_LED, SC_PIN_SHAKE, SC_PIN_TOUCH);
    return 0;
}
