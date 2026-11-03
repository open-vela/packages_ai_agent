/****************************************************************************
 * pcm_file_player.h - 播放 /data 下的原始 PCM 文件
 *
 * 为什么需要这个模块：
 *   本工作区的 media_player 是 src/voice/pcm_media_player.c 里的兼容实现
 *   （CONFIG_MEDIA 关闭），它只把原始 PCM 写进 /dev/audio/pcm0p ——
 *   media_player_prepare() 的第一行就是 `(void)url;`，**根本不读文件**。
 *   所以「放歌」这件事必须由我们自己：打开文件 -> 读块 -> write_data。
 *
 * 为什么是 PCM 而不是 MP3：
 *   设备固定 16kHz / 单声道 / 16bit，且没有任何 MP3 解码路径接在播放器上。
 *   要放 MP3 得自己接 edge_mp3 解码流水线（见 docs_ble/41 方案的 C）。
 *
 * 速率约束（关键）：
 *   media_player_write_data() 只是把数据推进 96KB 环形缓冲，满了**丢最旧的**。
 *   如果按文件读多快就写多快，3 秒后开始持续丢帧，听感是跳音。
 *   所以播放器按实时速度喂：每 100ms 写 3200 字节（= 16000Hz x 2B x 0.1s）。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef AI_AGENT_PCM_FILE_PLAYER_H
#define AI_AGENT_PCM_FILE_PLAYER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PCM_FILE_RATE      16000
#define PCM_FILE_CHANNELS  1

/**
 * 开始播放一个 PCM 文件（16kHz/单声道/s16le）。**立即返回**，实际播放
 * 在独立任务里进行。重复调用会先停掉上一首。
 * 返回 0 表示已开始，负数表示失败。
 */
int pcm_file_play(const char *path);

/**
 * 从**内存**播放原始 PCM（16kHz/单声道/s16le）。**立即返回**。
 *
 * 为什么要有这个：内置演示曲的 960KB PCM 本来就用 const 数组编在固件里
 * （ui/assets/audio/demo_music.c），但早期实现又把它往 littlefs 写了一份，
 * 播放时从 /data 读回来。实测读到 1.5~2.4 秒时 read() 返回 -1（文件系统层
 * 的读失败），播放直接中断 —— 现象是"30 秒里只有一小段卡住的声音"。
 *
 * 数据既然已经在固件里，就没必要绕一趟 flash 文件系统。
 * 内存播放不会读失败，也不占 /data 空间。
 */
int pcm_mem_play(const void *data, size_t len, const char *name);

/** 停止当前播放（若没有在播则什么都不做）。 */
void pcm_file_stop(void);

/**
 * 暂停 / 恢复。
 *
 * 暂停会在**播放任务内部**把 DMA 停掉（不排空、不释放），位置、文件句柄、
 * 环形缓冲全部保留；恢复时从原位置接着放。UI 线程只改标志，不碰任何
 * 音频句柄 —— 跨线程碰句柄曾经导致 EBADF 和播放卡死。
 */
void pcm_file_pause(void);
void pcm_file_resume(void);
int  pcm_file_is_paused(void);

/** 当前是否在播放。 */
int pcm_file_is_playing(void);

/** 当前播放位置（毫秒）。没在播放时返回最后位置。 */
int pcm_file_position_ms(void);

/** 当前文件总时长（毫秒）。没在播放时返回 0。 */
int pcm_file_duration_ms(void);

/** 最近一次播放结束的原因：0=还在放 1=正常结束 -1=出错（用于界面提示） */
int pcm_file_last_result(void);

/**
 * 设置音量（0~100）。**真的会改硬件**。
 *
 * 为什么要单独做：通用的 tool_music_set_volume 走的是
 * media_policy_set_stream_volume()，而那个函数在本工作区是个空桩
 * （src/voice/pcm_media_player.c:523 里就是 (void)stream;(void)volume; return 0）。
 * 也就是说界面上的音量按钮看起来生效、实际一个寄存器都没动。
 *
 * 这里直接对 /dev/audio/pcm0p 发 AUDIO_TYPE_FEATURE / AUDIO_FU_VOLUME，
 * 由 sf32lb52_audio_tx.c 落到 AUDCODEC 的 DAC 音量寄存器。
 */
int pcm_file_set_volume(int percent);

/** 当前音量（0~100）。 */
int pcm_file_get_volume(void);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_PCM_FILE_PLAYER_H */