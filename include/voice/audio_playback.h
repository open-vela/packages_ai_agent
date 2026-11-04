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

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Audio playback session handle (opaque). */
typedef struct audio_playback audio_playback_t;

/* Open the playback device and configure for PCM output.
 * Returns NULL on failure. */
audio_playback_t* audio_playback_open(const char* dev_path,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample);

/* Write PCM data to the playback device (blocking).
 * Returns bytes written, or negative errno on error. */
int audio_playback_write(audio_playback_t* pb,
    const void* buf, size_t len);

/* P2A-感知增强（task-62）：设置本次播放会话的音量。
 * percent: 0..100；100 = 0 dB 基准（最大），只做衰减、**绝不产生 >0 dB 增益**。
 * 实现走当前会话自己的 player fd（media_player_set_volume -> 既有 ioctl），
 * 绝不另开 /dev/audio/pcm0p 改音量（实测会重新初始化音频通路、打断正在播的声音）。
 * 返回 0 成功；-EINVAL 参数/会话无效；其它负值为底层 ioctl 失败。 */
int audio_playback_set_volume(audio_playback_t* pb, int percent);

/* Force-stop an active playback from another thread.
 * Stops the underlying media_player and marks the handle so that
 * subsequent write() calls return -ECANCELED immediately.
 * Safe to call from any thread; safe if pb is NULL. */
void audio_playback_stop(audio_playback_t* pb);

/* Stop playback and release all resources. */
void audio_playback_close(audio_playback_t* pb);

#ifdef __cplusplus
}
#endif
