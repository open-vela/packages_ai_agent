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

/* P2A-COLLISION F4（task-57）：主动关怀本地语音专用入队。
 * origin=care 且 cache-only：speak worker 只查 voice_tts_cache 模板缓存直出
 * 本地 PCM —— 不调 tts_begin（因此不会被 LLM 回合阻塞）、不占每 boot TTS
 * 会话额度、未命中/-EBUSY 直接丢弃，**绝不回退网络 TTS**；录音态直接丢弃。
 * 优先级低于用户回答：队列满时只淘汰最旧 care（care 上限 2 条），没有 care
 * 则丢弃本条，**绝不淘汰用户回答**；worker 也不会因 care 条目清 turn_active。
 * 返回 0 已入队（不代表已播出），负 errno 表示被拒。 */
int voice_channel_speak_care_async(const char *text);

/* 2026-11-03 分句 TTS：把最终回答按句拆开，首句先合成/播放（省首声等待）。 */
int voice_channel_speak_sentences(const char *text);

/* 2026-11-04 VOICE-B（docs_ble/112 §2B）：LLM 流式首句 TTS。
 * agent_loop 在 voice 通道的流式 delta 上调用：
 *   - 首个 delta 前 stream_begin()；
 *   - 每个 delta 调 stream_feed(delta)：攒到第一个完整句子（。！？；换行）
 *     就异步入队 speak worker（不阻塞网络回调），后续句子继续入队；
 *   - 回答结束调 stream_end(full_text)：flush 尾句，并登记 takeover，随后
 *     outbound dispatch 对同一段文本调用 speak_async 时会被跳过（不重复整段）；
 *   - 失败/工具轮/被 PTT 打断调 stream_abort()：清掉状态，dispatch 回退原
 *     整段 TTS（原有行为）。
 * 线程模型：begin/feed/end 都在 LLM 网络回调线程；内部只有短内存锁 +
 * speak 队列入队（strdup+cond signal），不做网络/播放；播放器所有权仍归
 * 既有 speak worker（PTT abort 会清队列+stream 状态）。 */
void voice_channel_stream_begin(void);
void voice_channel_stream_feed(const char *delta);
void voice_channel_stream_end(const char *full_text);
void voice_channel_stream_abort(void);

/* P0-NET（perf-fw 复核 #3）：最近一次 TTS 首块延迟（ms）与序号。
 * 由 speak worker 的 tts_stream_cb 在首块 PCM 时写入；agent_loop 在每轮
 * [lat] 聚合日志里用“序号是否变化”判断本轮的 tts_first_ms。
 * 只读 getter，不改变 TTS/播放逻辑。 */
void voice_channel_get_last_tts_first_ms(long *ms, unsigned *seq);

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
