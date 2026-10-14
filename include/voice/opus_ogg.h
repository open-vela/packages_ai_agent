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

/* 把 16kHz / 单声道 / 16bit 的裸 PCM 编成 **Ogg Opus**。
 *
 * 为什么必须是 Ogg 而不是裸码流(真机实测, 2026-10-14):
 *   MiniMax ASR 的格式列表里写着支持 opus, 但**只接受带容器的**。
 *   把 Ogg 里的音频包抽出来拼成纯 Opus 包序列再上传, 会稳定返回
 *     HTTP 400 bad_request_error
 *     "invalid params, Invalid data found when processing input (2013)"
 *   (换成 .ogg 后缀也一样 —— 它看的是内容, 不是扩展名)。
 *   而带 Ogg 封装、同样码率的文件返回 200 且识别完全正确。
 *
 * 为什么值得: 16kHz/16bit 原始 PCM 是 32KB/s, 而本板蓝牙 PAN 上行的结构性
 *   上限约 12KB/s(ACL 在飞窗口 x 分片 x RTT), 实测 TLS 上传 4.6~4.9KB/s。
 *   Opus 16kbps = 2KB/s, 4 秒语音从 128KB 降到约 8KB, 上传从 26 秒降到约 1.6 秒。
 *
 * 参数:
 *   pcm        PCM 数据(小端 16bit), 长度 pcm_bytes 字节
 *   bitrate_bps 目标码率(16000 = 16kbps), 实际是 VBR
 *   成功返回 0, *out 指向 malloc 出来的 Ogg 文件(调用者 free), *out_len 为长度;
 *   失败返回负 errno。 */
int opus_ogg_encode(const void *pcm, size_t pcm_bytes, int bitrate_bps,
                    unsigned char **out, size_t *out_len);

#ifdef __cplusplus
}
#endif
