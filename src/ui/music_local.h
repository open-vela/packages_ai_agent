/****************************************************************************
 * music_local.h - 本地音乐（/data/music 下的文件）
 *
 * 为什么要有这个模块：
 *   在线音乐走的是网易云的匿名公开接口，返回的 URL 很多因版权/防盗链
 *   根本播不了 —— 「搜得到，放不出」。而 media_player 原生支持本地文件
 *   （media_player.h:55 的官方示例就是 media_player_prepare(h, "/data/1.mp3")）。
 *   所以本地文件是唯一能保证「点了就真出声」的路径，而且不需要网络。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef AI_AGENT_MUSIC_LOCAL_H
#define AI_AGENT_MUSIC_LOCAL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSIC_LOCAL_DIR   "/data/music"
#define MUSIC_LOCAL_MAX   16
#define MUSIC_LOCAL_NAMELEN 48

/**
 * 确保 /data/music 存在；若里面没有内嵌的演示曲就写一份进去。
 * 幂等：已经存在则什么都不做。开机时调用一次。
 * 返回 0 成功，负数失败（失败不影响其他功能）。
 */
int music_local_seed(void);

/**
 * 扫描 MUSIC_LOCAL_DIR 下的 .mp3，把文件名（不含目录）填进 names。
 * 返回找到的文件数（0 表示没有）。
 */
int music_local_scan(char names[][MUSIC_LOCAL_NAMELEN], int max);

/* 内置演示曲的"路径"标识（不是真文件，用来在界面列表里认它） */
#define MUSIC_LOCAL_DEMO_PATH  MUSIC_LOCAL_DIR "/demo1.pcm"

/**
 * 内置演示曲：直接从固件里的 const 数组取，**不经过文件系统**。
 *
 * 原来播放它要先把 960KB 写进 littlefs 再从 /data 读回来，实测读到
 * 1.5~2.4 秒时 read() 返回 -1，播放中断（"只有一小段卡住的声音"）。
 * 数据本来就在固件里，没必要绕这一趟。
 */
const void *music_local_demo_data(void);
size_t      music_local_demo_len(void);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_MUSIC_LOCAL_H */