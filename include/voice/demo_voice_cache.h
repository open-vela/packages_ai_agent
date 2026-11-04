/* V142 非演示构建桩：demo_voice_cache 源文件已不在树中。
 * DEMO_VOICE_CACHE=OFF 时只需文案宏 + 空实现，保证 pet_care/agent_main/voice_channel 编译通过；
 * 缓存播放恒返回 -ENOENT → 所有演示/关怀回落到原 TTS/L2，镜像不含 1.7MB PCM。 */
#pragma once

#include <stdbool.h>

#define DEMO_VOICE_TEXT_HELLO     "你好呀！我是小云，很高兴见到你～"
#define DEMO_VOICE_TEXT_WHO       "我是小云，你的随身 AI 小助手，可以陪聊天、报时间天气、记提醒。"
#define DEMO_VOICE_TEXT_JOKE      "为什么程序员总分不清万圣节和圣诞节？因为 Oct 31 等于 Dec 25。"
#define DEMO_VOICE_TEXT_TIRED     "累的话就先停下来歇一会儿吧，我陪着你。"
#define DEMO_VOICE_TEXT_HR        "刚刚测到心跳有点快，先坐下歇会儿吧"
#define DEMO_VOICE_TEXT_GREET     "晚上好～今天过得怎么样？"
#define DEMO_VOICE_TEXT_ALARM     "叮咚！闹钟时间到啦，该起来啦～"
#define DEMO_VOICE_TEXT_WATER     "喝口水吧，别等口渴才想起～"
#define DEMO_VOICE_TEXT_SLEEP     "该睡觉啦，放下手机，早点休息吧～"
#define DEMO_VOICE_TEXT_WEATHER   "出门前记得看一眼天气，别着凉啦～"
#define DEMO_VOICE_TEXT_SEDENTARY "坐太久啦，起来走两步吧～"

static inline int demo_voice_init(void) { return -1; }
static inline int demo_voice_try_play(const char *text) { (void)text; return -1; }
static inline void demo_voice_stop(void) { }
static inline bool demo_voice_is_playing(void) { return false; }
