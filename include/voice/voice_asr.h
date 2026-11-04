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

/* Generic ASR backend interface. Each provider implements this struct
 * and registers via voice_asr_register(). */

typedef struct voice_asr_ops {
    const char *name;

    /* Load credentials from config store. */
    int (*init)(void);

    /* Recognize PCM audio (16-bit LE, 16kHz, mono) into text. */
    int (*recognize)(const unsigned char *pcm_data,
                     size_t pcm_len,
                     char *text_out,
                     size_t text_cap);

    /* Release resources held by the backend. */
    void (*deinit)(void);

    /* ── 可选的流式接口 ────────────────────────────────────────────────
     * 后端若支持"边采集边处理", 就由采集线程逐块把 PCM 送进来(stream_send),
     * 而不是等录完再一次性交给 recognize()。MiniMax 用它做**边录边编码**
     * (见 minimax_asr.c): complexity=0 实测 2.05x 实时, 跟得上采集,
     * 于是录完时只剩上传, 省掉整段 2 秒的编码等待。
     * 四个回调要么都给、要么都不给。stream_open 返回 NULL 表示本次不可用,
     * 调用方会回退到 recognize() 的批量路径。 */
    void *(*stream_open)(void);
    int   (*stream_send)(void *s, const unsigned char *pcm, size_t len);
    int   (*stream_finish)(void *s, char *text_out, size_t text_cap);
    void  (*stream_abort)(void *s);

    /* 可选: 声明"输入到此为止", 把最后不足一帧的残余处理掉。
     * ★ 必须由**采集线程**(栈够大的那个)在收流之前调用 —— 真机上这一步会跑
     *   编码器, 而 voice_asr_stream_finish() 是在调用者的线程里执行的,
     *   那个栈可能只有 16KB(vela CLI 线程), 直接把 silk_Encode 撑爆成硬 fault。 */
    int   (*stream_end_input)(void *s);

    /* 可选: 返回 1 表示"一句话已经说完"(粘滞直到下一句开始)。
     * 采集线程据此**自动收尾**, 用户就不必手动停录。
     * 返回 0 或未实现 = 不自动收尾。 */
    int   (*stream_utterance_done)(void *s);
} voice_asr_ops_t;

/* Register an ASR backend. Multiple backends can be registered. */
int voice_asr_register(const voice_asr_ops_t *ops);

/* Select a backend by name. Returns 0 or -ENOENT. */
int voice_asr_set_backend(const char *name);

/* Get the name of the current active backend, or NULL. */
const char *voice_asr_get_backend(void);

/* ── VOICE-F3（task-61）：连续"没检测到语音"(-ENODATA / -61) 统计 ──────
 * 流式/批量 ASR 返回 -ENODATA 时在公共分发层累计；成功一次清零。
 * 供 UI/voice_channel 做操作引导与限流：>=3 时建议提示"按住说话、说完
 * 再松手"，不要在失败后自动重试（每次重试都会重新采集+走一次 ASR）。
 * 计数上限饱和在 255，日志本身已在 voice_asr.c 内限流。 */
unsigned voice_asr_consecutive_nospeech(void);

/* 手动清零（例如 PTT 按下、开始新一轮录音时由调用方重置）。 */
void voice_asr_reset_nospeech(void);

/* Recognize speech using the active backend. */
int voice_asr_recognize(const unsigned char *pcm_data,
                        size_t pcm_len,
                        char *text_out,
                        size_t text_cap);

/* ── Streaming ASR interface ─────────────────────────────────── */

/* Opaque handle for a streaming ASR session. */
typedef struct voice_asr_stream voice_asr_stream_t;

/* Open a streaming ASR session using the active backend.
 * Returns NULL if the backend does not support streaming. */
voice_asr_stream_t *voice_asr_stream_open(void);

/* Send one PCM chunk to the streaming session. */
int voice_asr_stream_send(voice_asr_stream_t *s,
                          const unsigned char *pcm, size_t len);

/* 一句话是否已经说完(用于自动断句)。未实现该能力的后端恒返回 0。 */
int voice_asr_stream_utterance_done(voice_asr_stream_t *s);

/* 声明输入结束: 处理掉最后不足一帧的残余(会跑编码器, 见 ops 里的说明)。
 * 幂等。应由采集线程在收流前调用, 不要留着让 finish 去做。 */
int voice_asr_stream_end_input(voice_asr_stream_t *s);

/* Finish streaming, get final text, and free the session. */
int voice_asr_stream_finish(voice_asr_stream_t *s,
                            char *text_out, size_t text_cap);

/* Abort streaming session without waiting for result. */
void voice_asr_stream_abort(voice_asr_stream_t *s);

#ifdef __cplusplus
}
#endif
