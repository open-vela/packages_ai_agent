/* sensor_chat.h
 *
 * AI-VOX3 语音对话控灯 + 传感器问答模块
 * -------------------------------------
 * 对话中说"让LED灯亮起来" -> GPIO4 输出点亮 LED 模块
 * 对话中问摇晃/触摸状态   -> 读 GPIO48(数字摇晃传感器) / GPIO42(触摸板) 并播报
 * 摇晃/触摸触发时自动播报（可关闭）
 *
 * 接线（2026-10-24 用户确认）:
 *   LED 灯模块(信号)        -> GPIO4   （输出，默认高电平点亮）
 *   数字摇晃传感器(DO)      -> GPIO48  （输入，内部上拉，默认高电平=触发）
 *   触摸板(DO)              -> GPIO42  （输入，内部上拉，默认高电平=被摸）
 *   三个模块 VCC->3V3，GND->GND
 *
 * 集成方式见 PATCH_GUIDE.md（3~6 行挂钩代码）。
 */
#ifndef SENSOR_CHAT_H
#define SENSOR_CHAT_H

#include <stddef.h>

/* ── 引脚与极性（如模块极性相反，只改这里的宏） ───────────── */
#define SC_PIN_LED          4
#define SC_PIN_SHAKE        48
#define SC_PIN_TOUCH        42

#define SC_LED_ACTIVE_HIGH  1   /* 1 = 输出高电平亮灯 */
#define SC_SHAKE_ACTIVE     1   /* 1 = 引脚高电平算"已触发"（SW-420 类模块） */
#define SC_TOUCH_ACTIVE     1   /* 1 = 引脚高电平算"被触摸"（TTP223 类模块） */

/* GPIO 属性常量，取自 voice_channel.c 现有用法：
 *   0x09 = INPUT|PULLUP （板上 BOOT 键就是这么配的）
 *   0x02 = OUTPUT
 * 若编译报错或 LED 不动，用 PATCH_GUIDE.md 里的一条 grep 核对
 * arch/xtensa/src/esp32s3/esp32s3_gpio.h 的真实定义。
 */
#define SC_ATTR_IN_PULLUP   0x09
#define SC_ATTR_OUT         0x02

/* ── 对外接口 ────────────────────────────────────────────── */

/* 初始化：配置 GPIO、LED 熄灭、启动传感器轮询线程。
 * 在 ai_agent 启动序列（llm_proxy_init 附近）调用一次。 */
int  sensor_chat_init(void);

/* 核心入口：拿一条用户文本判断是不是硬件指令。
 * 命中 -> 执行动作、把回复写进 reply，返回 1（调用方应消费掉这条消息，
 *         不要再送 LLM）；未命中 -> 返回 0（原样走 LLM 对话）。
 * reply 可为 NULL（只要动作不要文案时）。 */
int  sensor_chat_handle_text(const char *text, char *reply, size_t cap);

/* 蓝牙式异步播报：起一个线程 voice_channel_speak(text)。
 * 回复通道接好后可不用它（直接推 outbound 更好），留作兜底。 */
void sensor_chat_reply_async(const char *text);

/* 便捷查询（调试/其他模块可用） */
int  sensor_chat_led_state(void);      /* 0/1 */
int  sensor_chat_shake_level(void);    /* 原始电平 0/1 */
int  sensor_chat_touch_level(void);    /* 原始电平 0/1 */

#endif /* SENSOR_CHAT_H */
