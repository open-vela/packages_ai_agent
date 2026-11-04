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

/*
 * voice_tts_cache.c - P0-CACHE: 模板 TTS 音频离线缓存（IMA ADPCM 4:1）
 *
 * 背景（docs_ble/112_voice_latency_p0.md §2C）：语音回合的正常路径 = ASR 3–5s +
 * LLM 首 token 4–45s + TTS 首包 2.5–3.6s。固定台词没必要每次走云 TTS。
 *
 * P0-CACHE 版本（本文件）：
 *   - 11 条固定台词（hello/who/joke/tired/hr/greet/alarm/water/sleep/weather/
 *     sedentary，文案 = include/voice/demo_voice_cache.h 的 DEMO_VOICE_TEXT_*）
 *     以 **IMA ADPCM 4:1** 压缩后嵌入 Flash（数据见 voice_tts_cache_data.c）：
 *     源 PCM16 1,706,466 B -> ADPCM 433,307 B（24kHz mono）。
 *   - 命中时在播放线程里逐 block 解码（512 samples/block，栈上 1 KiB 缓冲），
 *     直接 audio_playback_open(/dev/audio/pcm0p,24000,1,16) 播放：
 *     无网络 TTS、不占 TTS session 额度、不写 /data。
 *     （24kHz 依据：旧 demo_voice_worker 的 audio_playback_open 参数即 24000。）
 *   - 未命中返回 -ENOENT，调用方（voice_channel_speak）照常回落云端 TTS。
 *
 * 文本映射：
 *   1) 别名表（短口语，如 你好/嗨/hello/晚上好/讲个笑话/我有点累/你是谁/
 *      喝水/睡觉/天气/久坐；见 g_vc_aliases）——允许尾随标点/空格；
 *   2) 与 DEMO_VOICE_TEXT_* 宏精确匹配；
 *   3) 文本包含模板原句（LLM 往往原样复述）；
 *   4) 前 12 字节相同（容忍尾部语气词差异）。
 *
 * 边界/线程模型同 VOICE-C：单播放者（trylock）、static const 只读数据、
 * 无堆分配、无文件写入；PTT 用 voice_tts_cache_stop() 打断。
 *
 * ADPCM 容器格式（与 .v3_work/plan/gen_voice_tts_cache_adpcm.py 配套）：
 *   block = 4B header {int16 init_sample; uint8 step_index; uint8 rsv}
 *         + 256B nibble（512 samples/block，低 nibble 为前一个样本）
 *   满 block 实际编码 511 个后续样本（512 个 nibble 的最后 1 个为填充）。
 */

#include "voice/voice_tts_cache.h"
#include "voice/audio_playback.h"
#include "voice/demo_voice_cache.h"
#include "agent_config.h"

#include <errno.h>
#include <math.h>       /* P2A-感知增强：care 提示音正弦合成 */
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#define TAG "voice_cache"

#define VOICE_CACHE_RATE        24000u   /* 与数据文件一致（旧 demo 播放参数） */
#define VOICE_CACHE_CHANNELS    1u
#define VOICE_CACHE_BITS        16u
#define VOICE_CACHE_SLACK_MS    1500u    /* 超时 = 播放时长 + 该余量 */
#define VOICE_CACHE_LOG_CHARS   48u      /* 日志里文本最多打印多少字节 */

/* P2A-感知增强（task-62） */
#ifndef VOICE_CACHE_CARE_VOLUME_PCT
/* P2A-感知增强-FIX（task-64）：关怀音量改为 100% = 0 dB（review 要求不再
 * 比正常 TTS 轻）；上限仍由 audio_playback_set_volume 的 hw<=0dB 保护。 */
#define VOICE_CACHE_CARE_VOLUME_PCT 100  /* 100%=0dB；只减不增 */
#endif
#define VC_PROMPT_CHUNK         240u     /* 提示音分块合成：240 samples=10ms */
#define VC_PROMPT_AMP           7000.0   /* 提示音幅度（约 -13dBFS，不刺耳） */
#define VC_PROMPT_RAMP_MS       6u       /* 首尾淡入淡出，防爆音 */
#define VC_PROMPT_GAP_MS        50u      /* 两音之间静音 */

/* IMA ADPCM block（生成脚本与解码器共享的常量） */
#define VC_ADPCM_BLOCK_BYTES    260u     /* 4B header + 256B nibbles */
#define VC_ADPCM_BLOCK_SAMPLES  512u     /* 每 block 解码出的 PCM 样本数 */
#define VC_ADPCM_FULL_NIBBLES   511u     /* 满 block 实际编码的后续样本数 */

/* 由 voice_tts_cache_data.c 生成（IMA ADPCM，24kHz mono 解码后 s16le） */
extern const unsigned char voice_tts_cache_adpcm_hello[];
extern const unsigned char voice_tts_cache_adpcm_who[];
extern const unsigned char voice_tts_cache_adpcm_joke[];
extern const unsigned char voice_tts_cache_adpcm_tired[];
extern const unsigned char voice_tts_cache_adpcm_hr[];
extern const unsigned char voice_tts_cache_adpcm_greet[];
extern const unsigned char voice_tts_cache_adpcm_alarm[];
extern const unsigned char voice_tts_cache_adpcm_water[];
extern const unsigned char voice_tts_cache_adpcm_sleep[];
extern const unsigned char voice_tts_cache_adpcm_weather[];
extern const unsigned char voice_tts_cache_adpcm_sedentary[];

extern const unsigned int voice_tts_cache_adpcm_hello_len;
extern const unsigned int voice_tts_cache_adpcm_who_len;
extern const unsigned int voice_tts_cache_adpcm_joke_len;
extern const unsigned int voice_tts_cache_adpcm_tired_len;
extern const unsigned int voice_tts_cache_adpcm_hr_len;
extern const unsigned int voice_tts_cache_adpcm_greet_len;
extern const unsigned int voice_tts_cache_adpcm_alarm_len;
extern const unsigned int voice_tts_cache_adpcm_water_len;
extern const unsigned int voice_tts_cache_adpcm_sleep_len;
extern const unsigned int voice_tts_cache_adpcm_weather_len;
extern const unsigned int voice_tts_cache_adpcm_sedentary_len;

extern const unsigned int voice_tts_cache_adpcm_hello_nsamples;
extern const unsigned int voice_tts_cache_adpcm_who_nsamples;
extern const unsigned int voice_tts_cache_adpcm_joke_nsamples;
extern const unsigned int voice_tts_cache_adpcm_tired_nsamples;
extern const unsigned int voice_tts_cache_adpcm_hr_nsamples;
extern const unsigned int voice_tts_cache_adpcm_greet_nsamples;
extern const unsigned int voice_tts_cache_adpcm_alarm_nsamples;
extern const unsigned int voice_tts_cache_adpcm_water_nsamples;
extern const unsigned int voice_tts_cache_adpcm_sleep_nsamples;
extern const unsigned int voice_tts_cache_adpcm_weather_nsamples;
extern const unsigned int voice_tts_cache_adpcm_sedentary_nsamples;

typedef struct
{
  const char *text;            /* 触发文案（与 demo_voice_cache.h 宏一致） */
  const unsigned char *adpcm;  /* IMA ADPCM block 流，只读 */
  const unsigned int *clen;    /* ADPCM 字节数 */
  const unsigned int *nsamples;/* 解码后 24kHz 样本数 */
} vc_entry_t;

/* 顺序与 demo_voice_cache.h 的宏顺序一致；别名表用 index 引用 */
static const vc_entry_t g_entries[] =
{
  { DEMO_VOICE_TEXT_HELLO, voice_tts_cache_adpcm_hello,
    &voice_tts_cache_adpcm_hello_len, &voice_tts_cache_adpcm_hello_nsamples },
  { DEMO_VOICE_TEXT_WHO, voice_tts_cache_adpcm_who,
    &voice_tts_cache_adpcm_who_len, &voice_tts_cache_adpcm_who_nsamples },
  { DEMO_VOICE_TEXT_JOKE, voice_tts_cache_adpcm_joke,
    &voice_tts_cache_adpcm_joke_len, &voice_tts_cache_adpcm_joke_nsamples },
  { DEMO_VOICE_TEXT_TIRED, voice_tts_cache_adpcm_tired,
    &voice_tts_cache_adpcm_tired_len, &voice_tts_cache_adpcm_tired_nsamples },
  { DEMO_VOICE_TEXT_HR, voice_tts_cache_adpcm_hr,
    &voice_tts_cache_adpcm_hr_len, &voice_tts_cache_adpcm_hr_nsamples },
  { DEMO_VOICE_TEXT_GREET, voice_tts_cache_adpcm_greet,
    &voice_tts_cache_adpcm_greet_len, &voice_tts_cache_adpcm_greet_nsamples },
  { DEMO_VOICE_TEXT_ALARM, voice_tts_cache_adpcm_alarm,
    &voice_tts_cache_adpcm_alarm_len, &voice_tts_cache_adpcm_alarm_nsamples },
  { DEMO_VOICE_TEXT_WATER, voice_tts_cache_adpcm_water,
    &voice_tts_cache_adpcm_water_len, &voice_tts_cache_adpcm_water_nsamples },
  { DEMO_VOICE_TEXT_SLEEP, voice_tts_cache_adpcm_sleep,
    &voice_tts_cache_adpcm_sleep_len, &voice_tts_cache_adpcm_sleep_nsamples },
  { DEMO_VOICE_TEXT_WEATHER, voice_tts_cache_adpcm_weather,
    &voice_tts_cache_adpcm_weather_len, &voice_tts_cache_adpcm_weather_nsamples },
  { DEMO_VOICE_TEXT_SEDENTARY, voice_tts_cache_adpcm_sedentary,
    &voice_tts_cache_adpcm_sedentary_len, &voice_tts_cache_adpcm_sedentary_nsamples },
};

#define VC_ENTRY_COUNT (sizeof(g_entries) / sizeof(g_entries[0]))

/* 短口语别名 -> 缓存条目。只在语义一致时添加；匹配规则是"完整等值 + 允许
 * 尾随标点/空格"，避免 "你好，帮我查天气" 这类长句被误判成缓存而截断回答。 */
static const struct
{
  const char *alias;
  int entry;
} g_vc_aliases[] =
{
  /* 0 hello */
  { "你好", 0 },        { "你好啊", 0 },      { "你好呀", 0 },
  { "嗨", 0 },          { "哈喽", 0 },
  { "hello", 0 },       { "Hello", 0 },       { "hi", 0 },  { "Hi", 0 },
  /* 1 who */
  { "你是谁", 1 },      { "你叫什么", 1 },    { "你叫什么名字", 1 },
  { "介绍一下你自己", 1 }, { "你是干什么的", 1 }, { "你是干嘛的", 1 },
  /* 2 joke */
  { "讲个笑话", 2 },    { "来个笑话", 2 },    { "讲笑话", 2 },
  { "笑话", 2 },        { "再讲一个", 2 },    { "再说一个", 2 },
  /* 3 tired */
  { "我有点累", 3 },    { "好累", 3 },        { "累", 3 },
  { "累了", 3 },        { "我很累", 3 },      { "有点累", 3 },  { "我累了", 3 },
  /* 4 hr */
  { "心跳有点快", 4 },  { "心跳快", 4 },      { "心跳好快", 4 },
  { "心率有点快", 4 },  { "心跳快了点", 4 },
  /* 5 greet */
  { "晚上好", 5 },      { "晚上好呀", 5 },
  /* 6 alarm */
  { "闹钟", 6 },        { "闹钟响了", 6 },    { "起床啦", 6 },  { "叮咚", 6 },
  /* 7 water */
  { "喝水", 7 },        { "喝口水", 7 },      { "口渴", 7 },
  { "喝点水", 7 },      { "该喝水了", 7 },
  /* 8 sleep */
  { "睡觉", 8 },        { "该睡觉了", 8 },    { "晚安", 8 },
  { "该睡了", 8 },      { "早点休息", 8 },
  /* 9 weather */
  { "天气", 9 },        { "看天气", 9 },      { "查天气", 9 },  { "天气预报", 9 },
  /* 10 sedentary */
  { "久坐", 10 },       { "坐太久", 10 },     { "站起来走走", 10 },
  { "起来走两步", 10 }, { "坐太久了", 10 },
};

#define VC_ALIAS_COUNT (sizeof(g_vc_aliases) / sizeof(g_vc_aliases[0]))

/* 状态：单播放者。g_vc_playing/g_vc_abort 跨线程读，g_vc_pb 供 stop() 打断 */
static pthread_mutex_t g_vc_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool g_vc_playing;
static volatile bool g_vc_abort;
static audio_playback_t *volatile g_vc_pb;

/* 标准 IMA ADPCM 步长/索引表 */
static const int16_t g_ima_step[89] =
{
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
  34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
  157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
  598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878,
  2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
  6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
  18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int8_t g_ima_index[16] =
{
  -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};

static const char *vc_skip_ws(const char *s)
{
  if (s == NULL)
    {
      return NULL;
    }

  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
    {
      s++;
    }

  return (s[0] != '\0') ? s : NULL;
}

/* 只允许尾随标点/空格，防止长句被短别名截胡 */
static bool vc_only_tail_punct(const char *p)
{
  while (*p != '\0')
    {
      if (strchr(" \t\r\n!！.。~～?？,，、…", *p) == NULL)
        {
          return false;
        }
      p++;
    }
  return true;
}

static bool vc_alias_match(const char *text, const char *alias)
{
  size_t n = strlen(alias);

  if (strncmp(text, alias, n) != 0)
    {
      return false;
    }
  return vc_only_tail_punct(text + n);
}

/* 命中判定：别名 -> 精确 -> 包含模板原句 -> 前 12 字节相同 */
static const vc_entry_t *vc_lookup_entry(const char *raw)
{
  const char *text = vc_skip_ws(raw);
  size_t i;

  if (text == NULL)
    {
      return NULL;
    }

  for (i = 0; i < VC_ALIAS_COUNT; i++)
    {
      if (vc_alias_match(text, g_vc_aliases[i].alias))
        {
          return &g_entries[g_vc_aliases[i].entry];
        }
    }

  for (i = 0; i < VC_ENTRY_COUNT; i++)
    {
      if (strcmp(text, g_entries[i].text) == 0)
        {
          return &g_entries[i];
        }
    }

  for (i = 0; i < VC_ENTRY_COUNT; i++)
    {
      if (strstr(text, g_entries[i].text) != NULL)
        {
          return &g_entries[i];
        }
    }

  /* 12 字节 = 4 个汉字：11 句前 4 字互不相同 */
  for (i = 0; i < VC_ENTRY_COUNT; i++)
    {
      if (strncmp(text, g_entries[i].text, 12) == 0)
        {
          return &g_entries[i];
        }
    }

  return NULL;
}

int voice_tts_cache_lookup(const char *text, const void **pcm, size_t *bytes)
{
  const vc_entry_t *e = vc_lookup_entry(text);

  if (e == NULL)
    {
      return -ENOENT;
    }

  /* P0-CACHE：内存里不再有连续 PCM（ADPCM 需实时解码），pcm 返回 NULL；
   * bytes 返回解码后的 PCM 字节数，供调用方做时长/额度判断。 */
  if (pcm != NULL)
    {
      *pcm = NULL;
    }
  if (bytes != NULL)
    {
      *bytes = (size_t)*e->nsamples * 2u;
    }

  return 0;
}

bool voice_tts_cache_is_playing(void)
{
  return g_vc_playing;
}

void voice_tts_cache_stop(void)
{
  audio_playback_t *pb;

  g_vc_abort = true;

  /* audio_playback_stop() 文档保证可从任意线程调用：会 stop 底层 media_player，
   * 并让正在阻塞的 audio_playback_write() 返回 -ECANCELED。 */
  pb = g_vc_pb;
  if (pb != NULL)
    {
      audio_playback_stop(pb);
    }
}

static int vc_elapsed_ms(const struct timespec *t0, const struct timespec *t1)
{
  return (int)((t1->tv_sec - t0->tv_sec) * 1000L
               + (t1->tv_nsec - t0->tv_nsec) / 1000000L);
}

/* 解码一个 ADPCM block -> pcm[]，返回解码出的样本数（<= max_samples）。 */
static unsigned int vc_adpcm_decode_block(const unsigned char *in,
                                          unsigned int in_bytes,
                                          int16_t *out,
                                          unsigned int max_samples)
{
  int pred;
  int idx;
  unsigned int nibbles;
  unsigned int n = 0;
  unsigned int i;

  if (in == NULL || out == NULL || in_bytes < 4u || max_samples == 0u)
    {
      return 0;
    }

  pred = (int)(int16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
  idx = (int)in[2];
  if (idx > 88)
    {
      idx = 88;
    }

  out[n++] = (int16_t)pred;

  nibbles = (in_bytes >= VC_ADPCM_BLOCK_BYTES)
            ? VC_ADPCM_FULL_NIBBLES : (in_bytes - 4u) * 2u;

  for (i = 0; i < nibbles && n < max_samples; i++)
    {
      unsigned int byte = in[4u + (i >> 1)];
      unsigned int code = (i & 1u) ? (byte >> 4) : (byte & 0x0Fu);
      int step = (int)g_ima_step[idx];
      int delta = step >> 3;

      if ((code & 4u) != 0u)
        {
          delta += step;
        }
      if ((code & 2u) != 0u)
        {
          delta += step >> 1;
        }
      if ((code & 1u) != 0u)
        {
          delta += step >> 2;
        }

      if ((code & 8u) != 0u)
        {
          pred -= delta;
        }
      else
        {
          pred += delta;
        }

      if (pred > 32767)
        {
          pred = 32767;
        }
      else if (pred < -32768)
        {
          pred = -32768;
        }

      out[n++] = (int16_t)pred;

      idx += (int)g_ima_index[code];
      if (idx < 0)
        {
          idx = 0;
        }
      else if (idx > 88)
        {
          idx = 88;
        }
    }

  return n;
}

/* P2A-感知增强（task-62）：care 播放前的短提示音，写入**同一个**
 * audio_playback 会话（不二次 open）。两音上行 ding：880Hz 80ms + 1320Hz
 * 120ms，中间 50ms 静音，共约 250ms；24kHz mono s16，首尾 6ms 线性淡入
 * 淡出防爆音，分块合成控制栈占用。失败只返回错误码，由调用方决定是否
 * 阻断正文（当前策略：提示音失败不阻断，只记日志）。 */
static int vc_write_all(audio_playback_t *pb, const void *buf, size_t len)
{
  const unsigned char *p = (const unsigned char *)buf;

  while (len > 0)
    {
      int w = audio_playback_write(pb, p, len);

      if (w < 0)
        {
          return (w == -ECANCELED) ? -ECANCELED : -EIO;
        }
      if (w == 0)
        {
          return -EIO;      /* 播放器不再消费：避免死循环 */
        }
      p += w;
      len -= (size_t)w;
    }
  return 0;
}

static int vc_write_prompt_tone(audio_playback_t *pb)
{
  static const struct
    {
      unsigned int hz;
      unsigned int ms;
    } notes[] = {
      { 880u, 80u },
      { 1320u, 120u },
    };
  int16_t buf[VC_PROMPT_CHUNK];
  unsigned int k;
  unsigned int ramp = (VOICE_CACHE_RATE * VC_PROMPT_RAMP_MS) / 1000u;

  if (ramp == 0)
    {
      ramp = 1;
    }

  for (k = 0; k < sizeof(notes) / sizeof(notes[0]); k++)
    {
      unsigned int total = (VOICE_CACHE_RATE * notes[k].ms) / 1000u;
      unsigned int pos;

      for (pos = 0; pos < total; pos += VC_PROMPT_CHUNK)
        {
          unsigned int cnt = total - pos;
          unsigned int i;

          if (cnt > VC_PROMPT_CHUNK)
            {
              cnt = VC_PROMPT_CHUNK;
            }
          for (i = 0; i < cnt; i++)
            {
              unsigned int n = pos + i;
              double t = (double)n / (double)VOICE_CACHE_RATE;
              double env = 1.0;
              double s;

              if (n < ramp)
                {
                  env = (double)n / (double)ramp;
                }
              if (total - 1u - n < ramp)
                {
                  env = (double)(total - 1u - n) / (double)ramp;
                }
              if (env < 0.0)
                {
                  env = 0.0;
                }
              s = sin(2.0 * 3.14159265358979 * (double)notes[k].hz * t);
              buf[i] = (int16_t)(s * env * VC_PROMPT_AMP);
            }
          int rc = vc_write_all(pb, buf, (size_t)cnt * sizeof(int16_t));
          if (rc != 0)
            {
              return rc;
            }
        }

      if (k + 1u < sizeof(notes) / sizeof(notes[0]))
        {
          unsigned int gap = (VOICE_CACHE_RATE * VC_PROMPT_GAP_MS) / 1000u;

          memset(buf, 0, sizeof(buf));
          while (gap > 0)
            {
              unsigned int cnt = (gap > VC_PROMPT_CHUNK) ? VC_PROMPT_CHUNK : gap;
              int rc = vc_write_all(pb, buf,
                  (size_t)cnt * sizeof(int16_t));

              if (rc != 0)
                {
                  return rc;
                }
              gap -= cnt;
            }
        }
    }

  return 0;
}

/* 内部实现：care=true 时先设关怀音量 + 播短提示音（同一会话），再播正文。 */
static int vc_play_impl(const char *text, bool care)
{
  const vc_entry_t *e;
  audio_playback_t *pb = NULL;
  struct timespec t_start;
  struct timespec t_now;
  int16_t pcm_buf[VC_ADPCM_BLOCK_SAMPLES];
  unsigned int enc_len;
  unsigned int total_samples;
  unsigned int off = 0;
  unsigned int written_samples = 0;
  unsigned int limit_ms;
  int ret = 0;

  e = vc_lookup_entry(text);
  if (e == NULL)
    {
      syslog(LOG_INFO, "[%s] miss: \"%.*s\"\n", TAG, VOICE_CACHE_LOG_CHARS,
             (text != NULL) ? text : "(null)");
      return -ENOENT;
    }

  if (pthread_mutex_trylock(&g_vc_lock) != 0)
    {
      syslog(LOG_WARNING, "[%s] busy: another cache playback in progress\n",
             TAG);
      return -EBUSY;
    }

  enc_len = *e->clen;
  total_samples = *e->nsamples;
  limit_ms = (unsigned int)(((uint64_t)total_samples * 1000u) / VOICE_CACHE_RATE)
             + VOICE_CACHE_SLACK_MS;

  g_vc_abort = false;
  g_vc_playing = true;
  g_vc_pb = NULL;

  clock_gettime(CLOCK_MONOTONIC, &t_start);

  syslog(LOG_INFO,
         "[voice_cache] hit: \"%.*s\" adpcm=%u bytes (%u pcm bytes, ~%ums)\n",
         VOICE_CACHE_LOG_CHARS, text, enc_len, total_samples * 2u,
         (unsigned int)(((uint64_t)total_samples * 1000u) / VOICE_CACHE_RATE));

  pb = audio_playback_open(AGENT_AUDIO_PLAYBACK_DEV, VOICE_CACHE_RATE,
                           VOICE_CACHE_CHANNELS, VOICE_CACHE_BITS);
  if (pb == NULL)
    {
      syslog(LOG_ERR, "[%s] playback open failed (%s)\n", TAG,
             AGENT_AUDIO_PLAYBACK_DEV);
      ret = -EIO;
      goto out;
    }

  g_vc_pb = pb;

  if (care)
    {
      /* P2A-感知增强（task-62）：同一次 audio_playback 会话内先设关怀音量、
       * 再播短提示音，然后播缓存正文。绝不二次 open（会打断通路）。 */
      int prc;
      int vrc = audio_playback_set_volume(pb, VOICE_CACHE_CARE_VOLUME_PCT);

      /* task-64：日志明确打出 care earcon（提示音）诊断行，验收直接 grep。 */
      syslog(LOG_INFO,
             "[voice_cache] care earcon: prompt tone 250ms (880/1320Hz) + "
             "volume=%d%% (set_rc=%d, same audio_playback session)\n",
             (int)VOICE_CACHE_CARE_VOLUME_PCT, vrc);
      prc = vc_write_prompt_tone(pb);
      if (prc == -ECANCELED)
        {
          ret = -ECANCELED;
          goto out;
        }
      if (prc != 0)
        {
          syslog(LOG_WARNING,
                 "[voice_cache] care earcon failed: %d (continue body)\n",
                 prc);
        }
      else
        {
          syslog(LOG_INFO,
                 "[voice_cache] care earcon: done (250ms two-tone, same "
                 "session, no second open)\n");
        }
    }

  while (off < enc_len)
    {
      unsigned int blk = enc_len - off;
      unsigned int remain = total_samples - written_samples;
      unsigned int maxs = (remain > VC_ADPCM_BLOCK_SAMPLES)
                          ? VC_ADPCM_BLOCK_SAMPLES : remain;
      unsigned int got;
      unsigned int wrote = 0;
      unsigned int want;

      if (g_vc_abort)
        {
          ret = -ECANCELED;
          break;
        }

      if (blk > VC_ADPCM_BLOCK_BYTES)
        {
          blk = VC_ADPCM_BLOCK_BYTES;
        }

      got = vc_adpcm_decode_block(e->adpcm + off, blk, pcm_buf, maxs);
      if (got == 0)
        {
          syslog(LOG_ERR, "[%s] adpcm decode failed at %u/%u\n", TAG, off,
                 enc_len);
          ret = -EIO;
          break;
        }

      want = got * 2u;
      while (wrote < want)
        {
          int w = audio_playback_write(pb, ((const unsigned char *)pcm_buf) + wrote,
                                       want - wrote);

          if (w < 0)
            {
              ret = (w == -ECANCELED) ? -ECANCELED : w;
              break;
            }
          if (w == 0)
            {
              /* 播放器不再消费：视为设备错误，避免死循环 */
              ret = -EIO;
              break;
            }
          wrote += (unsigned int)w;
        }

      if (ret != 0)
        {
          break;
        }

      off += blk;
      written_samples += got;

      clock_gettime(CLOCK_MONOTONIC, &t_now);
      if ((unsigned int)vc_elapsed_ms(&t_start, &t_now) > limit_ms)
        {
          syslog(LOG_ERR,
                 "[%s] timeout: wrote %u/%u samples in %ums (limit %ums)\n",
                 TAG, written_samples, total_samples,
                 (unsigned int)vc_elapsed_ms(&t_start, &t_now), limit_ms);
          ret = -ETIMEDOUT;
          break;
        }
    }

out:
  g_vc_pb = NULL;
  g_vc_playing = false;

  if (pb != NULL)
    {
      /* close 会等余下 PCM 播完（正常路径）；abort 时已被 stop，快速返回 */
      audio_playback_close(pb);
    }

  clock_gettime(CLOCK_MONOTONIC, &t_now);
  if (ret == 0)
    {
      syslog(LOG_INFO,
             "[voice_cache] play done: %u/%u samples, %u adpcm bytes, %ums\n",
             written_samples, total_samples, off,
             (unsigned int)vc_elapsed_ms(&t_start, &t_now));
    }
  else if (ret == -ECANCELED)
    {
      syslog(LOG_INFO, "[voice_cache] play aborted at %u/%u samples\n",
             written_samples, total_samples);
    }

  pthread_mutex_unlock(&g_vc_lock);
  return ret;
}

/* 普通模板播放（dispatch/演示）：不播提示音，保持原有音量策略。 */
int voice_tts_cache_play(const char *text)
{
  return vc_play_impl(text, false);
}

/* P2A-感知增强（task-62）：主动关怀专用 —— 提示音 + 关怀音量 + 正文，
 * 全部在同一次 audio_playback 会话内完成。 */
int voice_tts_cache_play_care(const char *text)
{
  return vc_play_impl(text, true);
}
