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

/* 流式 VAD: 只放行"有语音"的音频, 把首尾静音裁掉。
 *
 * 为什么需要(真机实测 2026-10-14): 说话人只说了 3 秒, 却录了 22 秒 ——
 * 送给识别引擎的音频里 85% 是噪声, 上传量也从约 6KB 涨到 42KB。
 *
 * 设计取舍:
 *   · **只裁首尾**, 不裁中间。中间静音一裁就把词间停顿切断了, 反而伤识别。
 *   · 起说前保留 **300ms 预滚** —— 否则第一个音素会被削掉。
 *   · 说完后保留 **500ms 后滚** —— 否则尾音会被削掉。
 *   · 底噪用慢速 EMA 自适应跟踪, 所以**与 ADC 增益无关**: 增益一改,
 *     阈值跟着底噪走, 不需要重新标定。
 *   · 起说需要**连续 2 帧**超阈值, 抗单帧脉冲噪声(敲击、爆音)。 */

/* 放行一帧时的回调。frame 是 16bit 小端 PCM, samples 通常为 320。 */
typedef void (*vad_emit_fn)(void* user, const short* frame, size_t samples);

typedef struct
{
  short          preroll[15 * 320];   /* 300ms @16kHz 的预滚环形缓冲 */
  int            head;                /* 环形写位置(帧) */
  int            count;
  long           noise;               /* 底噪估计(平均绝对幅度) */
  long           calib_min;           /* 标定期的最小帧能量(取最小而非均值) */
  int            calib_n;
  int            calib_skip;          /* 跳过的启动瞬态帧数 */
  int            calib_done;
  int            in_speech;
  int            above_run;
  int            below_run;
  unsigned long  emitted;             /* 放行的帧数(20ms/帧) */
  unsigned long  dropped;             /* 丢弃的帧数 */
  long           e_max;               /* 见过的最大帧能量(用于标定阈值) */
  unsigned long  utter_frames;        /* 本句话已放行的帧数 */
  int            silence_run;         /* 距最后一个"响"帧的连续静音帧数 */
  int            done;                /* 1 = 一句话已说完(粘滞, 见下) */

  /* ── VOICE-F3（task-61）：冷启动/短录音诊断 + 抢救 ──────────────────
   * emit_cb/emit_user 在 feed 时缓存，vad_trim_finish() 抢救短录音时要
   * 主动补发预滚帧（原 finish 只有 v, 拿不到回调）。fed_frames 记录总共
   * 喂进来的 20ms 帧数（含标定期），用于日志和"短录音"判定。 */
  vad_emit_fn    emit_cb;             /* 首次 feed 时记住的放行回调 */
  void*          emit_user;
  unsigned long  fed_frames;          /* 总输入帧数(含标定期, 20ms/帧) */

  short          pend[320];           /* 不足一帧的残余 */
  size_t         pend_len;            /* 采样数 */
} vad_trim_t;

void vad_trim_init(vad_trim_t* v);

/* 喂任意长度的 PCM(采样数), 内部按 20ms 切帧。 */
void vad_trim_feed(vad_trim_t* v, const short* pcm, size_t samples,
                   vad_emit_fn emit, void* user);

/* 收尾: 处理残余(不足一帧的按静音处理, 不强行放行)。
 * 返回 1 表示整段里**一帧都没放行**(全静音), 调用方据此告警。
 *
 * VOICE-F3: 如果录音在冷启动窗口(200ms 标定期)内就结束了(用户短按 PTT),
 * 会把预滚里已采到的帧补发出去并返回 0 —— 避免整段录音被标定段吃掉后
 * 直接变成 -ENODATA(-61)。长录音全静音仍返回 1(不上传噪声)。 */
int vad_trim_finish(vad_trim_t* v);

/* 「一句话说完了」—— 用于**自动断句**(不用手动停录)。
 * 判定: 之前确实起说过, 且已连续静音够久(后滚期满)退出语音态。
 * 要求本句放行帧数 >= VAD_MIN_UTTER_FRAMES(300ms), 免得一声咳嗽就收尾。
 * 粘滞: 返回 1 后一直返回 1, 直到开始下一句。 */
int vad_trim_utterance_done(vad_trim_t* v);

#ifdef __cplusplus
}
#endif
