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
#define VAD_POSTROLL_FRAMES 25           /* 500ms */
#define VAD_START_FRAMES    2            /* 连续 2 帧才算起说 */
#define VAD_ON_NUM          3            /* 超过底噪 3 倍算语音 */
#define VAD_NOISE_SHIFT     5            /* EMA: alpha = 1/32 */
#define VAD_NOISE_INIT      200          /* 标定没跑完时的临时值(几乎用不到) */
#define VAD_NOISE_MIN       8            /* 防止底噪被压到 0 后疯狂触发 */
#define VAD_CALIB_FRAMES    15           /* 标定用的帧数(300ms) */
#define VAD_CALIB_SKIP      5            /* 先跳过 100ms 启动瞬态再开始标定 */
#define VAD_NOISE_MAX       6000         /* 底噪封顶, 免得标定坏了直接废掉 VAD */
#define VAD_MIN_UTTER_FRAMES 15          /* 至少 300ms 语音才算"一句话", 防咳嗽误触发 */

/* ★ 判"说完"的静音时长必须**明显长于**裁剪用的后滚(VAD_POSTROLL_FRAMES=500ms)。
 *   两者是不同的事:
 *     · 后滚 500ms —— 修剪时尾部留多少静音, 短一点无所谓;
 *     · 收尾 1.2s  —— 多久没声音才算"这个人说完了"。
 *   真机 2026-10-14 踩到: 两件事都用 500ms, 而说话人在「小云手表」和
 *   「现在几点了」之间自然停顿了一下 -> VAD 以为说完了 -> 只放行 0.8 秒
 *   -> 识别结果为空串。 */
#define VAD_ENDPOINT_FRAMES  60          /* 1.2s 连续静音才算说完 */

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
  /* 残余不足一帧: 按静音处理, 不强行放行(否则会把半帧噪声塞进去) */
  v->pend_len = 0;

  /* 这些数字是调阈值的依据: 底噪与最大帧能量的**比值**决定 VAD_ON_NUM 该取
   * 多少。全程静音时 e_max 会贴近底噪, 一眼就能看出来。 */
  syslog(LOG_INFO, "[%s] noise=%ld e_max=%ld kept=%lu dropped=%lu\n",
         VAD_TAG, v->noise, v->e_max, v->emitted, v->dropped);

  return v->emitted == 0 ? 1 : 0;
}
