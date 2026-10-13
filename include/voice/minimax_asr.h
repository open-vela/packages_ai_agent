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

#ifdef __cplusplus
extern "C" {
#endif

/* 注册 MiniMax 语音识别(Speech-to-Text)后端, 名字 "minimax"。
 *
 *   POST https://api.minimaxi.com/v1/speech_to_text
 *   Authorization: Bearer <api_key>
 *   Content-Type: multipart/form-data
 *     model = asr-1.0
 *     file  = <wav>
 *   返回 JSON: {"text": "...", "duration": ..., "trace_id": "..."}
 *   文档: https://platform.minimax.cn/docs/api-reference/speech-to-text
 *
 * key 与 TTS 走同一条来源链(tts_api_key → api_key → llm_backend_0 →
 * 编译进镜像的本地 key), 见 minimax_get_api_key()。
 *
 * 后端只接受 16bit/16kHz/单声道裸 PCM(voice_asr_ops_t 的约定), 内部会给它
 * 套一个 44 字节 RIFF/WAV 头 —— 官方明确"不支持无容器的裸 PCM"。
 * 不做任何音频编码, 不需要额外的编解码器。 */
int minimax_asr_register(void);

#ifdef __cplusplus
}
#endif
