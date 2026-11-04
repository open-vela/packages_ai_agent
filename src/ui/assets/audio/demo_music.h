#ifndef AI_AGENT_DEMO_MUSIC_H
#define AI_AGENT_DEMO_MUSIC_H

#include <stddef.h>

/* 内嵌的 30 秒演示曲（16kHz / 单声道 / s16le 原始 PCM）。
 *
 * 为什么是 PCM 而不是 MP3：本工作区的 media_player 是
 * src/voice/pcm_media_player.c 里的兼容实现（CONFIG_MEDIA 关闭），
 * 它只把原始 PCM 写进 /dev/audio/pcm0p，**不认识任何压缩格式**。
 * 设备固定 16kHz/单声道/16bit，所以这里直接按这个格式烘焙。 */
extern const unsigned char demo_music_data[];
extern const size_t demo_music_len;
#define DEMO_MUSIC_RATE 16000
#define DEMO_MUSIC_CHANNELS 1

#endif /* AI_AGENT_DEMO_MUSIC_H */
