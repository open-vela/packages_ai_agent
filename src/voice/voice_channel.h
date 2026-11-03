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

/* Initialize voice channel (load config keys). */
int voice_channel_init(void);

/* Start voice recording, ASR, then push text to agent via message_bus. */
int voice_channel_start(void);

/* Stop an active voice session. */
int voice_channel_stop(void);

/* 2026-11-03: 停止当前 TTS 播放并清空排队文本（新用户消息优先）。 */
void voice_channel_tts_abort(void);

/* 是否正在录音 (1=是)。给物理按键做"按一下开始/再按一下结束"用。 */
int voice_channel_is_recording(void);

/* Stop recording and return ASR text to caller (does NOT push inbound).
 * text_out: buffer to receive ASR text, text_cap: buffer capacity.
 * Returns 0 on success, negative errno on failure.
 * If ASR returns empty text, text_out[0] is set to '\0' (not an error). */
int voice_channel_stop_with_text(char *text_out, size_t text_cap);

/* Synthesize text and play back (called from outbound dispatcher). */
int voice_channel_speak(const char *text);

/* Same, but on a persistent background worker: never blocks the caller (UI or
 * the outbound dispatcher).  The text is queued into a depth-2 drop-oldest
 * queue (a newer answer always wins), the worker thread is created once with
 * an explicit >=24KB stack and reused.  Returns 0 once the text is queued --
 * 0 does NOT mean the utterance was spoken. */
int voice_channel_speak_async(const char *text);

/* 2026-11-03 分句 TTS：把最终回答按句拆开，首句先合成/播放（省首声等待）。 */
int voice_channel_speak_sentences(const char *text);

/* 2026-11-01 v102c：是否还有 TTS 在拉流/播放（agent 收到下一条消息前要等它）。 */
int voice_channel_is_busy(void);

/* v111：上一轮语音提问是否仍在处理/播报（用于拒绝新的录音）。 */
int voice_channel_turn_active(void);
/* v112：显式结束当前回合（语音通道同步播报路径用）。 */
void voice_channel_turn_clear(void);

/* 2026-11-01 v102d：LLM 请求与 TTS 会话互斥（同一时刻只允许一条网络+播报流）。
 * llm_begin/end 包住一次 LLM HTTP 请求；tts_begin/end 包住一次完整 TTS 会话。 */
void voice_channel_llm_begin(void);
void voice_channel_llm_end(void);
void voice_channel_tts_begin(void);
void voice_channel_tts_end(void);

/* Test TTS: synthesize text, save PCM to file. */
int voice_channel_test_tts(const char *text, const char *out_path);

/* Test ASR: read PCM file, recognize, print result. */
int voice_channel_test_asr(const char *pcm_path);

#ifdef __cplusplus
}
#endif
