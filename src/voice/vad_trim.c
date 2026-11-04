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

/****************************************************************************
 * vad_trim.c —— 流式 VAD, 只放行有语音的音频(裁首尾静音)
 *
 * 算法很朴素, 但每一步的取舍都是冲着真机数据去的:
 *
 * 1. 每 20ms 一帧算**平均绝对幅度**(比 RMS 便宜, 对本用途够用)。
 * 2. 底噪 noise 用慢速 EMA 跟踪: 只在"当前判为静音"时更新(α=1/32),
 *    判为语音时冻结 —— 否则说着话底噪会被拉高, 后段就再也触发不了。
 * 3. 判定: 幅度 > noise * 3 算"这一帧有语音"。
 *    α 慢 + 倍数粗, 换来的是**与 ADC 增益无关**(增益一改, noise 跟着变)。
 * 4. 起说要连续 2 帧超阈值(抗敲击/爆音这种单帧脉冲);
 *    停说也要连续 25 帧(500ms)静音才认为说完, 期间照常放行(后滚)。
 * 5. 未起说时把帧写进 300ms 的环形预滚; 一旦起说, 先把预滚整段补发出去,
 *    保证第一个音素不被削掉。
 *
 * 只裁**首尾**, 中间一律放行 —— 中间一裁会切断词间停顿, 反而伤识别。
 ****************************************************************************/

#include "voice/vad_trim.h"

#include <string.h>
#include <syslog.h>

#define VAD_TAG "vad"

#define VAD_FRAME_SAMPLES   320          /* 20ms @16kHz */
#define VAD_PREROLL_FRAMES  15           /* 300ms */
#define VAD_POSTROLL_FRAMES 10           /* v114b10：500ms→200ms，省 ~0.3s 等待（B 报告 P8） */
#define VAD_START_FRAMES    2            /* 连续 2 帧才算起说 */
/* 超过底噪多少倍算"这一帧有语音"。
 *
 * ★ 这个值**与增益无关** —— 底噪是自适应跟踪的, 数字增益把信号和底噪等比
 *   放大, 比值不变。所以"说话轻就识别不到"**不能靠加大增益解决**,
 *   只能把这里的倍数调小。
 *
 * 真机 2026-10-16 的数据(增益已修正为 x1):
 *   正常音量: e_max=12363, noise=470 -> 26 倍, 3x 阈值轻松通过 ✅
 *   轻声/偏远: e_max=2596,  noise=349 -> 7.4 倍, 但只放行 41 帧(0.8s),
 *             整句约 1.5s -> 语音被截断 -> 识别结果空串 ❌
 * 3 -> 2 之后, 轻声那句的阈值从 1047 降到 698, 能放行更完整的语音。
 *
 * 误触发风险可控: 起说要求**连续 2 帧**超阈值, 收尾要求 1.2 秒静音,
 * 偶发噪声毛刺不足以凑成一次"说话"。 */
#define VAD_ON_NUM          2
#define VAD_NOISE_SHIFT     5            /* EMA: alpha = 1/32 */
#define VAD_NOISE_INIT      200          /* 标定没跑完时的临时值(几乎用不到) */
#define VAD_NOISE_MIN       8            /* 防止底噪被压到 0 后疯狂触发 */
/* ── VOICE-F3（task-61, audio_rca2 §2.3/§3）：冷启动窗口 600ms -> 200ms ──
 * 旧值 15+15 帧（300ms 跳过 + 300ms 标定）会让短按 PTT（<600ms）的整段录音
 * 全被冷启动段吃掉：RCA2 里 attempt 3/4 的日志特征 e_max=0 / kept=0 /
 * dropped=0，随后后端返回 -ENODATA(-61)。
 * 新值 4+6 帧（80ms 跳过 + 120ms 标定 = 200ms）：
 *   - 标定仍取窗口内**最小**帧能量，启动满量程爆音被 min 机制自然滤掉；
 *   - 标定没跑完就松手的短录音，由 vad_trim_finish() 的 short-recording
 *     rescue 把预滚帧补发出去（见 finish）。
 *   - 正常录音的判定阈值/预滚/后滚/收尾时长全部不变。
 * 回滚：编译加 -DVOICE_VAD_FAST_START=0 恢复旧的 15+15 帧。 */
#ifndef VOICE_VAD_FAST_START
#  define VOICE_VAD_FAST_START 1
#endif
#if VOICE_VAD_FAST_START
#  define VAD_CALIB_SKIP    4            /* 80ms：跳过录音启动满量程爆音 */
#  define VAD_CALIB_FRAMES  6            /* 120ms：标定底噪（取窗口内最小帧能量） */
#else
#  define VAD_CALIB_SKIP    15
#  define VAD_CALIB_FRAMES  15
#endif
#define VAD_RESCUE_MIN_FRAMES 3          /* VOICE-F3：短录音抢救最少 3 帧(60ms) */
#define VAD_NOISE_MAX       6000         /* 底噪封顶, 免得标定坏了直接废掉 VAD */
/* VOICE-F3：标定窗口只有 120ms（短按用户常在标定窗内就开始说话）。若窗口内
 * 都是语音/爆音，calib_min 会把底噪抬到语音级别，noise*2 阈值再也过不去。
 * 超过该值就认为标定不可信，退回初始底噪（VAD 至少能检测到语音，后续静音
 * 段的 EMA 会重新收敛）。正常安静环境标定值 300~1000，远低于此。 */
#define VAD_CALIB_MAX_TRUST 3000
#define VAD_MIN_UTTER_FRAMES 15          /* 至少 300ms 语音才算"一句话", 防咳嗽误触发 */

/* ★ 判"说完"的静音时长必须**明显长于**裁剪用的后滚(VAD_POSTROLL_FRAMES=500ms)。
 *   两者是不同的事:
 *     · 后滚 500ms —— 修剪时尾部留多少静音, 短一点无所谓;
 *     · 收尾 1.2s  —— 多久没声音才算"这个人说完了"。
 *   真机 2026-10-14 踩到: 两件事都用 500ms, 而说话人在「小云手表」和
 *   「现在几点了」之间自然停顿了一下 -> VAD 以为说完了 -> 只放行 0.8 秒
 *   -> 识别结果为空串。 */
#define VAD_ENDPOINT_FRAMES  80          /* 1.6s 连续静音才算说完（1.2s 会把带停顿的句子截断） */

/* ★ 教训(真机 2026-10-14): 底噪**不能拍脑袋给初值**。
 *   第一版把初值写成 200, 而实际环境帧能量远高于 3x200, 于是第一帧就被判成
 *   语音 -> in_speech=1 -> 底噪估计被冻结 -> 再也退不出语音态 ->
 *   整段 40.7 秒一帧没裁(日志 "VAD: kept 2035 frames, dropped 0")。
 *   正确做法: 开头留 300ms 专门标定, 拿标定期的**均值**当初值。 */

static long vad_frame_energy(const short* f, size_t n)
{
  long sum = 0;
  size_t i;

  for (i = 0; i < n; i++)
    {
      sum += (f[i] < 0) ? -(long)f[i] : (long)f[i];
    }

  return n ? sum / (long)n : 0;
}

void vad_trim_init(vad_trim_t* v)
{
  memset(v, 0, sizeof(*v));
  v->noise = VAD_NOISE_INIT;
}

/* 放行一帧(顺带统计) */
static void vad_emit_frame(vad_trim_t* v, const short* f, vad_emit_fn emit,
                           void* user)
{
  v->emitted++;
  emit(user, f, VAD_FRAME_SAMPLES);
}

/* 预滚环形缓冲: 存下这一帧; 起说时按时间顺序补发 */
static void vad_preroll_push(vad_trim_t* v, const short* f)
{
  memcpy(v->preroll + (size_t)v->head * VAD_FRAME_SAMPLES, f,
         VAD_FRAME_SAMPLES * sizeof(short));
  v->head = (v->head + 1) % VAD_PREROLL_FRAMES;
  if (v->count < VAD_PREROLL_FRAMES)
    {
      v->count++;
    }
}

static void vad_preroll_flush(vad_trim_t* v, vad_emit_fn emit, void* user)
{
  int i;
  int start = (v->head - v->count + VAD_PREROLL_FRAMES) % VAD_PREROLL_FRAMES;

  for (i = 0; i < v->count; i++)
    {
      int idx = (start + i) % VAD_PREROLL_FRAMES;

      vad_emit_frame(v, v->preroll + (size_t)idx * VAD_FRAME_SAMPLES,
                     emit, user);
    }

  v->count = 0;
}

static void vad_process_frame(vad_trim_t* v, const short* f, vad_emit_fn emit,
                              void* user)
{
  long e = vad_frame_energy(f, VAD_FRAME_SAMPLES);
  long thr;
  int  loud;

  v->fed_frames++;   /* VOICE-F3：诊断/短录音判定，含标定期与跳过帧 */

  /* ── 标定期: 只攒底噪, 不判语音 ─────────────────────────────────────
   * 用均值而不是最小值: 最小值容易被一个安静的下陷带偏。
   * 同时做一次粗野值剔除, 免得标定期正好在说话把底噪抬上去。 */
  if (!v->calib_done)
    {
      /* 先丢掉开头若干帧: 真机实测录音刚启动时音频通路会出现**满量程**
       * (peak=32768) 的爆音。拿它去标定, 底噪会被抬到 14068, 阈值乘 3 之后
       * 再也没有任何一帧能触发 —— VAD 直接失效, 而且失效得毫无声息。 */
      if (v->calib_skip < VAD_CALIB_SKIP)
        {
          v->calib_skip++;
          vad_preroll_push(v, f);
          return;
        }

      /* 取**最小**而非均值: 底噪应该代表"最安静时的背景", 均值会被夹杂的
       * 说话/噪声抬高。 */
      if (v->calib_n == 0 || e < v->calib_min)
        {
          v->calib_min = e;
        }

      v->calib_n++;
      vad_preroll_push(v, f);

      if (v->calib_n >= VAD_CALIB_FRAMES)
        {
          v->noise = v->calib_min;

          if (v->noise < VAD_NOISE_MIN)
            {
              v->noise = VAD_NOISE_MIN;
            }

          if (v->noise > VAD_NOISE_MAX)
            {
              v->noise = VAD_NOISE_MAX;
            }

          /* VOICE-F3：标定值高得离谱 = 窗口里大概率是说话/爆音，别信 */
          if (v->noise > VAD_CALIB_MAX_TRUST)
            {
              syslog(LOG_WARNING,
                     "[%s] calib suspicious: %ld -> fallback %d\n",
                     VAD_TAG, v->noise, VAD_NOISE_INIT);
              v->noise = VAD_NOISE_INIT;
            }

          v->calib_done = 1;
          syslog(LOG_INFO,
                 "[%s] noise floor calibrated: %ld (min of %d frames)\n",
                 VAD_TAG, v->noise, v->calib_n);
        }

      return;
    }

  if (e > v->e_max)
    {
      v->e_max = e;
    }

  thr = v->noise * VAD_ON_NUM;
  loud = (e > thr);

  /* 连续静音计数: 与 in_speech 无关地跟踪, 因为"说完"的判据要在
   * 退出语音态之后仍然继续累计(见 VAD_ENDPOINT_FRAMES 的说明)。 */
  if (loud)
    {
      v->silence_run = 0;
    }
  else
    {
      v->silence_run++;

      if (v->silence_run >= VAD_ENDPOINT_FRAMES && !v->done &&
          v->utter_frames >= VAD_MIN_UTTER_FRAMES)
        {
          v->done = 1;
          syslog(LOG_INFO,
                 "[%s] utterance done: %lu frames (%.1fs speech, %.1fs silence)\n",
                 VAD_TAG, v->utter_frames,
                 (double)v->utter_frames * 0.02,
                 (double)v->silence_run * 0.02);
        }
    }

  if (!v->in_speech)
    {
      /* 静音期: 底噪慢速跟踪 */
      v->noise += (e - v->noise) >> VAD_NOISE_SHIFT;
      if (v->noise < VAD_NOISE_MIN)
        {
          v->noise = VAD_NOISE_MIN;
        }

      if (loud)
        {
          v->above_run++;

          if (v->above_run >= VAD_START_FRAMES)
            {
              /* 起说: 把预滚补发出去, 第一个音素不能丢 */
              v->in_speech = 1;
              v->below_run = 0;
              v->done = 0;                    /* 新的一句开始, 清掉粘滞 */
              v->silence_run = 0;
              /* ★ 必须在 flush 之前取 count —— flush 内部会把它清零。
               *   本句帧数 = 即将补发的预滚帧 + 当前这一帧。 */
              v->utter_frames = (unsigned long)v->count + 1;
              vad_preroll_flush(v, emit, user);
              vad_emit_frame(v, f, emit, user);
            }
          else
            {
              vad_preroll_push(v, f);
            }
        }
      else
        {
          v->above_run = 0;
          vad_preroll_push(v, f);
          v->dropped++;
        }

      return;
    }

  /* 语音中: 一律放行; 连续静音够久才认为说完 */
  vad_emit_frame(v, f, emit, user);
  v->utter_frames++;

  if (loud)
    {
      v->below_run = 0;
    }
  else
    {
      v->below_run++;

      if (v->below_run >= VAD_POSTROLL_FRAMES)
        {
          /* 后滚期满: 退出语音态(只影响"是否继续放行"), 但**不清零
           * utter_frames** —— "说完"的判据还要用它, 见 VAD_ENDPOINT_FRAMES。 */
          v->in_speech = 0;
          v->above_run = 0;
          v->below_run = 0;
          v->count = 0;         /* 预滚重新积累 */
        }
    }
}

void vad_trim_feed(vad_trim_t* v, const short* pcm, size_t samples,
                   vad_emit_fn emit, void* user)
{
  size_t i = 0;

  if (v == NULL || pcm == NULL || samples == 0)
    {
      return;
    }

  /* VOICE-F3：finish 抢救短录音时要主动补发预滚帧，这里缓存回调。 */
  v->emit_cb = emit;
  v->emit_user = user;

  while (i < samples)
    {
      size_t need = VAD_FRAME_SAMPLES - v->pend_len;
      size_t take = (samples - i) < need ? (samples - i) : need;

      memcpy(v->pend + v->pend_len, pcm + i, take * sizeof(short));
      v->pend_len += take;
      i += take;

      if (v->pend_len == VAD_FRAME_SAMPLES)
        {
          vad_process_frame(v, v->pend, emit, user);
          v->pend_len = 0;
        }
    }
}

int vad_trim_utterance_done(vad_trim_t* v)
{
  return (v != NULL) ? v->done : 0;
}

int vad_trim_finish(vad_trim_t* v)
{
  if (v == NULL)
    {
      return 1;
    }

  /* 残余不足一帧: 按静音处理, 不强行放行(否则会把半帧噪声塞进去) */
  v->pend_len = 0;

#if VOICE_VAD_FAST_START
  /* ── VOICE-F3: short-recording rescue ─────────────────────────────
   * 录音在冷启动窗口(200ms)还没跑完就结束了(用户短按 PTT)，旧逻辑
   * emitted==0 -> 后端直接 -ENODATA(-61)，整段录音白采。这里把预滚里
   * 已采到的帧按时间顺序补发，让后端至少分析真实音频（可能识别出短词）。
   * 只抢救"连标定都没做完"的短录音；长录音全静音仍返回 1 -> 不上传
   * 几十秒噪声，正常 VAD 判定不变。 */
  if (v->emitted == 0 && v->count > 0 && v->emit_cb != NULL
      && v->calib_done == 0 && v->fed_frames >= VAD_RESCUE_MIN_FRAMES)
    {
      unsigned long before = v->emitted;

      vad_preroll_flush(v, v->emit_cb, v->emit_user);
      syslog(LOG_WARNING,
             "[%s] short-recording rescue: emitted %lu frames (%lums, "
             "calib_done=0, fed=%lums)\n",
             VAD_TAG, v->emitted - before,
             (unsigned long)((v->emitted - before) * 20),
             (unsigned long)(v->fed_frames * 20));
    }
  else if (v->emitted == 0 && v->fed_frames > 0 && v->fed_frames < 15)
    {
      /* 少于 300ms 又没得抢救(帧数不足/无回调): 明确提示，方便现场判断 */
      syslog(LOG_INFO,
             "[%s] short recording: fed=%lums, calib_done=%d, no speech\n",
             VAD_TAG, (unsigned long)(v->fed_frames * 20), v->calib_done);
    }
#endif

  /* 这些数字是调阈值的依据: 底噪与最大帧能量的**比值**决定 VAD_ON_NUM 该取
   * 多少。全程静音时 e_max 会贴近底噪, 一眼就能看出来。
   * F3 增加 fed/calib_done/silence_run，便于真机对比冷启动窗口修复效果。 */
  syslog(LOG_INFO,
         "[%s] noise=%ld e_max=%ld kept=%lu dropped=%lu fed=%lu "
         "calib_done=%d silence_run=%d\n",
         VAD_TAG, v->noise, v->e_max, v->emitted, v->dropped,
         v->fed_frames, v->calib_done, v->silence_run);

  return v->emitted == 0 ? 1 : 0;
}
