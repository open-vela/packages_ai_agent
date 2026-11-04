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
 *
 * 为什么值得: 16kHz/16bit 原始 PCM 是 32KB/s, 而本板蓝牙 PAN 上行的结构性
 *   上限约 12KB/s, 实测 TLS 上传 4.6~4.9KB/s。Opus 16kbps = 2KB/s,
 *   4 秒语音从 128KB 降到约 8KB, 上传从 26 秒降到约 1.6 秒。
 *
 * 两套接口:
 *   1) 批量: opus_ogg_encode() —— 一次性把整段 PCM 编完。
 *   2) 增量: opus_ogg_begin/feed/finish —— 边采集边编码。后者是"边录边编码"
 *      的基础: complexity=0 实测 2.05x 实时, 有 2 倍余量跟得上采集,
 *      录完时只差最后收尾, 省掉整段 2 秒的编码等待。 */

typedef struct opus_ogg_enc opus_ogg_enc_t;

/* ── 增量接口 ──────────────────────────────────────────────────────────── */

/* 建编码器并写好 OpusHead/OpusTags 两个头页。失败返回 NULL。 */
opus_ogg_enc_t* opus_ogg_begin(int bitrate_bps);

/* 喂任意长度的 PCM(字节数不必对齐到帧)。内部会攒够 20ms 再编。
 * 返回 0 或负 errno。 */
int opus_ogg_feed(opus_ogg_enc_t* e, const void* pcm, size_t bytes);

/* 声明"输入到此为止": 把最后不足 20ms 的残余补零编掉, 之后 pend 清零。
 * 幂等, 可以重复调用。
 *
 * ★ 为什么要单独一个函数: 这一步会调 opus_encode(), 而它的栈开销很大。
 *   finish() 是在**调用者的线程**里跑的 —— 真机上 voice_stop 由 vela CLI
 *   线程处理, 那个栈只有 16KB(AGENT_CLI_STACK), 于是 silk_Encode 直接栈
 *   溢出硬 fault。所以约定: **由采集线程(64KB 栈)在收流前调本函数**,
 *   finish() 里就再也不做任何编码 —— 它对任何调用栈都安全。 */
int opus_ogg_end_input(opus_ogg_enc_t* e);

/* 收尾(打 EOS 页), 交出完整的 Ogg 文件。
 * 成功返回 0, *out 为 malloc 的缓冲(调用者 free), *out_len 为长度。
 * 注意: 调用后 e 即被释放, 不要再用。
 * 本函数**不做编码**(见 opus_ogg_end_input 的说明); 若还有残余帧,
 * 说明调用者没先调 end_input, 残余会被丢弃并打一条警告。 */
int opus_ogg_finish(opus_ogg_enc_t* e, unsigned char** out, size_t* out_len);

/* 放弃会话(不产出任何东西), 并释放 e。 */
void opus_ogg_abort(opus_ogg_enc_t* e);

/* ── 批量接口(内部就是 begin+feed+finish) ──────────────────────────────── */

/* 参数:
 *   pcm        PCM 数据(小端 16bit), 长度 pcm_bytes 字节
 *   bitrate_bps 目标码率(16000 = 16kbps), 实际是 VBR
 *   成功返回 0, *out 指向 malloc 出来的 Ogg 文件(调用者 free); 失败负 errno。 */
int opus_ogg_encode(const void* pcm, size_t pcm_bytes, int bitrate_bps,
                    unsigned char** out, size_t* out_len);

#ifdef __cplusplus
}
#endif
