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

#include "voice/voice_tts.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register the MiniMax T2A v2 TTS backend (name "minimax").
 *
 * HTTPS POST https://api.minimaxi.com/v1/t2a_v2 with
 * "Authorization: Bearer <key>"; the key comes from the "tts_api_key" config
 * key, or from "api_key" when that is absent (the MiniMax key the LLM uses).
 * Docs: https://platform.minimax.cn/docs/api-reference/speech-t2a-http
 *
 * Unlike the credential-less Edge backend this one costs money, so it is
 * registered after it and never becomes the default backend. */
int minimax_tts_register(void);

/* Streaming synthesis: the response audio (hex or base64 encoded MP3) is
 * decoded as it arrives and handed to the minimp3 wrapper chunk by chunk;
 * cb is finally called with is_last == 1.  Raw 24 kHz/16-bit/mono PCM, the
 * same contract as edge_tts_synthesize_stream().
 *
 * Returns 0 as soon as at least one PCM chunk was delivered (partial audio is
 * better than re-speaking the sentence through the next fallback), negative
 * errno when nothing could be synthesised at all:
 *   -EACCES  no API key configured, or the service rejected it
 *   -ENOMEM  decoder/request allocation failed
 *   -ETIMEDOUT / -EIO / -EPROTO  transport or protocol failure
 * Never asserts, never retries in a loop. */
int minimax_tts_synthesize_stream(const char *text,
                                  voice_tts_chunk_cb cb,
                                  void *user_data);

/* ── 供 voice/minimax_asr.c 复用（同一个账号同一个 key，同一套 TLS 通道） ──
 *
 * minimax_get_api_key(): 按 TTS 完全相同的顺序取 key
 *   ("tts_api_key" → "api_key" → llm_backend_0 → 编译进镜像的本地 key)，
 *   四道都不是空才返回 0。ASR 与 TTS 必须用同一条链，否则会出现
 *   "能说话但不能识别"这种最难查的分裂状态。
 *
 * minimax_https_post(): 对 AGENT_MINIMAX_TTS_HOST 发一个 POST，复用本文件
 *   已经真机验证过的 mm_tls_*(代理隧道 + mbedtls + 超时) 与
 *   mm_read_response_head/mm_reader_read(自动处理 Content-Length 与 chunked)。
 *   path / content_type 由调用者给（ASR 是 /v1/speech_to_text +
 *   multipart/form-data）。响应体写入 resp 并 NUL 结尾。
 *   返回 0 表示 HTTP 2xx 且响应体已读入；否则负 errno
 *   (-EACCES 401/403、-EIO 其它状态码、-ETIMEDOUT/-EPROTO 传输层)。 */
int minimax_get_api_key(char *out, size_t cap);

int minimax_https_post(const char *path, const char *content_type,
                       const char *api_key,
                       const unsigned char *body, size_t body_len,
                       char *resp, size_t resp_cap, int timeout_ms);

#ifdef __cplusplus
}
#endif
