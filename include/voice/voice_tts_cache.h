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

/*
 * voice_tts_cache.h - P0-CACHE: 模板 TTS 音频离线缓存（只读 Flash，无网络）
 *
 * 设计（docs_ble/112_voice_latency_p0.md §2C，P0-CACHE 扩展）：
 *   - 11 条固定台词（hello/who/joke/tired/hr/greet/alarm/water/sleep/
 *     weather/sedentary，文案 = include/voice/demo_voice_cache.h 的
 *     DEMO_VOICE_TEXT_* 宏）以 IMA ADPCM 4:1 压缩嵌入 Flash
 *     （voice_tts_cache_data.c）：源 PCM16 1,706,466 B -> ADPCM 433,307 B；
 *     原始/声道/采样率 = 24kHz mono s16le（旧 demo_voice_worker 即按 24000
 *     打开播放器，数据从旧构建产物 demo_voice_data.c.o 恢复）。
 *   - 命中时由播放线程逐 block 解码后直接
 *     audio_playback_open(/dev/audio/pcm0p, 24000, 1, 16) 播放：
 *     无网络 TTS、不占每 boot TTS session 额度、不写 /data。
 *   - 未命中返回 -ENOENT，调用方（VOICE-B / voice_channel_speak）照常走
 *     原云端 TTS 路径。
 *
 * 文本匹配顺序：
 *   1) 短口语别名（你好/嗨/hello/晚上好/讲个笑话/我有点累/你是谁/喝水/睡觉/
 *      天气/久坐 等，允许尾随标点/空格；完整等值，不会截断长句）；
 *   2) DEMO_VOICE_TEXT_* 宏精确匹配；
 *   3) 文本包含模板原句；
 *   4) 前 12 字节相同。
 *
 * 集成约定（给 VOICE-B）：
 *   int rc = voice_tts_cache_play(text);
 *     rc == 0         命中并已本地播放完（可当 TTS 成功收尾）
 *     rc == -ENOENT   未命中 → 走原 TTS
 *     rc == -EBUSY    已有一次缓存播放在进行 → 走原 TTS 或丢弃
 *     rc == -ECANCELED 被 voice_tts_cache_stop() 取消（PTT 打断）→ 静默收尾
 *     rc == -ETIMEDOUT 播放超时（设备异常）→ 记失败，走兜底
 *    其它 <0          设备/IO 错误
 *
 *   播放期间调用 voice_tts_cache_stop() 可从任意线程打断（PTT start 路径用）；
 *   播放线程会在 1 个 ADPCM block（512 samples）内返回 -ECANCELED。
 *   voice_tts_cache_is_playing() 可用来判断是否由缓存占着播放器。
 *   voice_tts_cache_lookup() 只查表不播放（先判断命中再决定是否占用 TTS
 *   session/日志）；P0-CACHE 后音频是 ADPCM，pcm 输出参数恒为 NULL，
 *   bytes 返回解码后的 PCM 字节数（24kHz mono s16le）。
 *
 * 线程模型：同一时刻只允许一次播放（内部 mutex trylock）；ADPCM 数据为
 * const 静态只读，可跨线程共享；播放时使用 1 KiB 栈缓冲；不分配堆、
 * 不写文件。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 尝试用模板音频缓存播放 text。
 *
 * @param text 待播报文本（UTF-8，NULL/空串视为未命中）
 * @return 0 命中并播放完成；-ENOENT 未命中；-EBUSY 已有播放；
 *         -ECANCELED 被 stop() 取消；-ETIMEDOUT 播放超时；
 *         其它负 errno 为设备/写入错误。
 */
int voice_tts_cache_play(const char *text);

/**
 * P2A-感知增强（task-62）：主动关怀专用缓存播放。
 *
 * 与 voice_tts_cache_play 相同命中/播放逻辑，但：
 *   - 在**同一次 audio_playback 会话**里先播一段短提示音（两音 ding，约
 *     250ms；不二次 audio_playback_open，不打断通路）；
 *   - 播放前用 audio_playback_set_volume() 设为关怀音量（默认 80%，
 *     100%=0dB、只减不增）；
 *   - 提示音失败不阻断正文（只记日志）。
 *
 * @return 同 voice_tts_cache_play。
 */
int voice_tts_cache_play_care(const char *text);

/**
 * 打断当前缓存播放（可从任意线程调用，PTT 开始录音/新一轮 TTS 前调用）。
 * 无播放时是安全 no-op；之后 play() 会重置取消标志。
 */
void voice_tts_cache_stop(void);

/** 是否有一次缓存播放正在进行（任意线程可读）。 */
bool voice_tts_cache_is_playing(void);

/**
 * 只查询文本是否命中缓存，不播放。
 *
 * P0-CACHE 起音频以 IMA ADPCM 存储，不再有可直接播放的连续 PCM 指针：
 * pcm 参数恒返回 NULL；bytes 返回解码后的 PCM 字节数（24kHz mono s16le，
 * 可用于估算播放时长 = bytes / 48000 秒）。
 *
 * @param text  待匹配文本
 * @param pcm   [out, optional] 恒为 NULL（保留参数以兼容旧调用方）
 * @param bytes [out, optional] 解码后 PCM 字节数
 * @return 0 命中；-ENOENT 未命中
 */
int voice_tts_cache_lookup(const char *text, const void **pcm, size_t *bytes);

#ifdef __cplusplus
}
#endif
