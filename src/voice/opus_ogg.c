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
 * Ogg Opus 封装 (编码 + 容器), 支持批量与增量两种用法
 *
 * 算法先在主机上用 Python 从零实现并对着 MiniMax ASR 验证通过(返回 200 且
 * 识别文字完全正确、体积与 ffmpeg 产物几乎一致), 再原样移植到这里。
 * 容器格式本身很简单, 复杂的是"读"那一侧; 我们只"写", 所以不需要 libogg:
 *   OggS 页 = 27 字节头 + 段表 + 数据, CRC32 覆盖整页(计算时 CRC 字段置 0),
 *   段表用 255 递降、末尾一个 <255 的值标记包结束。
 ****************************************************************************/

#include <nuttx/config.h>

#include "voice/opus_ogg.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#ifdef CONFIG_LIB_OPUS

#include "opus.h"

#define TAG "opus_ogg"

#define OG_RATE           16000          /* 输入采样率 */
#define OG_FRAME          320            /* 20ms @16kHz */
#define OG_PRESKIP        312            /* 与 ffmpeg 一致, 单位是 48kHz 采样 */
#define OG_SERIAL         0x5A5A1234u
#define OG_MAX_PKT        512
#define OG_PKTS_PER_PAGE  50             /* 每页最多 50 包(段表上限 255, 安全) */

/* 编码复杂度: 真机实测(16kHz, 4.096 秒音频)
 *   complexity 10 -> 6 秒 (0.68x 实时)
 *   complexity 0  -> 2 秒 (2.05x 实时)
 * 走增量路径时这个值是硬约束: 必须 >= 1x 实时才跟得上采集, c0 有 2 倍余量。 */
#define OG_COMPLEXITY     0

/* ── Ogg 用的 CRC32 ────────────────────────────────────────────────────────
 * 多项式 0x04c11db7, 初值 0, **不反射**, 无最终异或 —— 与通用 zlib CRC32
 * 不同, 不能拿现成的 crc32() 顶替。 */
static unsigned int g_og_crc_table[256];
static int          g_og_crc_ready;

static void og_crc_init(void)
{
  unsigned int i;
  int b;

  if (g_og_crc_ready)
    {
      return;
    }

  for (i = 0; i < 256; i++)
    {
      unsigned int r = i << 24;

      for (b = 0; b < 8; b++)
        {
          r = (r & 0x80000000u) ? ((r << 1) ^ 0x04c11db7u) : (r << 1);
        }

      g_og_crc_table[i] = r;
    }

  g_og_crc_ready = 1;
}

static unsigned int og_crc(const unsigned char* buf, size_t len)
{
  unsigned int c = 0;
  size_t       i;

  for (i = 0; i < len; i++)
    {
      c = (c << 8) ^ g_og_crc_table[((c >> 24) & 0xffu) ^ buf[i]];
    }

  return c;
}

/* ── 会话状态 ──────────────────────────────────────────────────────────── */

struct opus_ogg_enc
{
  OpusEncoder*   enc;

  unsigned char* out;                  /* Ogg 累积缓冲, 按需增长 */
  size_t         cap;
  size_t         len;
  unsigned int   seq;                  /* 页序号 */
  unsigned long long granule;          /* 48kHz 采样计数(含 preskip) */

  const unsigned char* pkt[OG_PKTS_PER_PAGE];
  size_t         plen[OG_PKTS_PER_PAGE];
  int            npkt;

  /* ★ 每个包必须**拷贝**着存下来。og_add_packet() 只记录指针, 而调用方每帧
   *   都把 opus_encode() 写回同一个 pktbuf —— 直接存指针的话, 等攒够一页
   *   再落页时所有指针都指向最后一帧, 整页数据全错。
   *   真机表现极具迷惑性: 编码"成功"、体积正常、HTTP 200, 但 text="" 空串。 */
  unsigned char* stage;
  unsigned char* pktbuf;

  unsigned char  pend[OG_FRAME * 2];   /* 未凑满一帧的残余字节 */
  size_t         pend_len;
};

static void og_le32(unsigned char* p, unsigned int v)
{
  p[0] = (unsigned char)(v & 0xff);
  p[1] = (unsigned char)((v >> 8) & 0xff);
  p[2] = (unsigned char)((v >> 16) & 0xff);
  p[3] = (unsigned char)((v >> 24) & 0xff);
}

/* 把当前积攒的包封成一页追加到 e->out。hdr_type: 0x02=BOS 0x04=EOS */
static int og_flush_page(struct opus_ogg_enc* e, int hdr_type)
{
  unsigned char  seg[255];
  size_t         body = 0;
  size_t         need;
  size_t         i;
  int            ns = 0;
  unsigned char* p;

  if (e->npkt == 0)
    {
      return 0;
    }

  for (i = 0; i < (size_t)e->npkt; i++)
    {
      size_t n = e->plen[i];

      while (n >= 255)
        {
          seg[ns++] = 255;
          n -= 255;
        }

      seg[ns++] = (unsigned char)n;
      body += e->plen[i];
    }

  need = 27u + (size_t)ns + body;

  if (e->len + need > e->cap)
    {
      size_t         ncap = e->cap ? e->cap : 8192u;
      unsigned char* n;

      while (ncap < e->len + need)
        {
          ncap *= 2u;
        }

      n = (unsigned char*)realloc(e->out, ncap);
      if (n == NULL)
        {
          return -ENOMEM;
        }

      e->out = n;
      e->cap = ncap;
    }

  p = e->out + e->len;
  memcpy(p, "OggS", 4);
  p[4] = 0;                                   /* version */
  p[5] = (unsigned char)hdr_type;

  {
    unsigned long long g = e->granule;
    int k;

    for (k = 0; k < 8; k++)
      {
        p[6 + k] = (unsigned char)((g >> (8 * k)) & 0xff);
      }
  }

  og_le32(p + 14, OG_SERIAL);
  og_le32(p + 18, e->seq++);
  og_le32(p + 22, 0);                         /* CRC 先置 0 */
  p[26] = (unsigned char)ns;
  memcpy(p + 27, seg, (size_t)ns);

  {
    unsigned char* d = p + 27 + ns;

    for (i = 0; i < (size_t)e->npkt; i++)
      {
        memcpy(d, e->pkt[i], e->plen[i]);
        d += e->plen[i];
      }
  }

  og_le32(p + 22, og_crc(p, need));
  e->len += need;
  e->npkt = 0;
  return 0;
}

static int og_add_packet(struct opus_ogg_enc* e, const unsigned char* pkt,
                         size_t len, int granule_ticks)
{
  if (e->npkt >= OG_PKTS_PER_PAGE)
    {
      int ret = og_flush_page(e, 0);

      if (ret != 0)
        {
          return ret;
        }
    }

  memcpy(e->stage + (size_t)e->npkt * OG_MAX_PKT, pkt, len);
  e->pkt[e->npkt] = e->stage + (size_t)e->npkt * OG_MAX_PKT;
  e->plen[e->npkt] = len;
  e->npkt++;
  e->granule += (unsigned long long)granule_ticks;
  return 0;
}

/* 把 e->pend 里的整整一帧(320 样本)编码并入页 */
static int og_encode_frame(struct opus_ogg_enc* e)
{
  int got = opus_encode(e->enc, (const opus_int16*)(const void*)e->pend,
                        OG_FRAME, e->pktbuf, OG_MAX_PKT);

  if (got < 0)
    {
      syslog(LOG_ERR, "[%s] opus_encode failed: %d\n", TAG, got);
      return -EIO;
    }

  /* granule 以 48kHz 计, 而输入是 16kHz => x3 */
  return og_add_packet(e, e->pktbuf, (size_t)got, OG_FRAME * 3);
}

/* ── 对外接口 ──────────────────────────────────────────────────────────── */

opus_ogg_enc_t* opus_ogg_begin(int bitrate_bps)
{
  static const unsigned char head[19] =
  {
    'O', 'p', 'u', 's', 'H', 'e', 'a', 'd',
    1,                      /* version */
    1,                      /* channels */
    0x38, 0x01,             /* preskip = 312 (LE) */
    0x80, 0x3e, 0x00, 0x00, /* input rate = 16000 (LE) */
    0x00, 0x00,             /* output gain */
    0x00                    /* mapping family */
  };

  static const unsigned char tags[20] =
  {
    'O', 'p', 'u', 's', 'T', 'a', 'g', 's',
    4, 0, 0, 0,             /* vendor length = 4 */
    'v', 'e', 'l', 'a',
    0, 0, 0, 0              /* comment count = 0 */
  };

  struct opus_ogg_enc* e;
  int                  err = OPUS_OK;

  if (bitrate_bps <= 0)
    {
      return NULL;
    }

  og_crc_init();

  e = (struct opus_ogg_enc*)calloc(1, sizeof(*e));
  if (e == NULL)
    {
      return NULL;
    }

  e->enc = opus_encoder_create(OG_RATE, 1, OPUS_APPLICATION_VOIP, &err);
  if (e->enc == NULL || err != OPUS_OK)
    {
      syslog(LOG_ERR, "[%s] encoder_create failed: %d\n", TAG, err);
      opus_ogg_abort(e);
      return NULL;
    }

  (void)opus_encoder_ctl(e->enc, OPUS_SET_BITRATE(bitrate_bps));
  (void)opus_encoder_ctl(e->enc, OPUS_SET_COMPLEXITY(OG_COMPLEXITY));
  (void)opus_encoder_ctl(e->enc, OPUS_SET_VBR(1));

  e->stage = (unsigned char*)malloc((size_t)OG_PKTS_PER_PAGE * OG_MAX_PKT);
  e->pktbuf = (unsigned char*)malloc(OG_MAX_PKT);

  if (e->stage == NULL || e->pktbuf == NULL)
    {
      opus_ogg_abort(e);
      return NULL;
    }

  /* 两个头页的 granule 必须是 0 */
  e->granule = 0;

  if (og_add_packet(e, head, sizeof(head), 0) != 0 ||
      og_flush_page(e, 0x02) != 0 ||
      og_add_packet(e, tags, sizeof(tags), 0) != 0 ||
      og_flush_page(e, 0x00) != 0)
    {
      opus_ogg_abort(e);
      return NULL;
    }

  /* 音频页的 granule 从 preskip 起算 */
  e->granule = OG_PRESKIP;
  return e;
}

int opus_ogg_feed(opus_ogg_enc_t* e, const void* pcm, size_t bytes)
{
  const unsigned char* p = (const unsigned char*)pcm;
  size_t               i = 0;

  if (e == NULL || (pcm == NULL && bytes > 0))
    {
      return -EINVAL;
    }

  while (i < bytes)
    {
      size_t need = (size_t)(OG_FRAME * 2) - e->pend_len;
      size_t take = (bytes - i) < need ? (bytes - i) : need;
      int    ret;

      memcpy(e->pend + e->pend_len, p + i, take);
      e->pend_len += take;
      i += take;

      if (e->pend_len == (size_t)(OG_FRAME * 2))
        {
          ret = og_encode_frame(e);
          if (ret != 0)
            {
              return ret;
            }

          e->pend_len = 0;
        }
    }

  return 0;
}

int opus_ogg_end_input(opus_ogg_enc_t* e)
{
  if (e == NULL)
    {
      return -EINVAL;
    }

  if (e->pend_len == 0)
    {
      return 0;
    }

  /* 补到偶数字节再补零, 凑成完整一帧编掉 */
  if ((e->pend_len & 1u) != 0)
    {
      e->pend[e->pend_len++] = 0;
    }

  memset(e->pend + e->pend_len, 0, (size_t)(OG_FRAME * 2) - e->pend_len);

  {
    int ret = og_encode_frame(e);

    if (ret != 0)
      {
        return ret;
      }
  }

  e->pend_len = 0;
  return 0;
}

int opus_ogg_finish(opus_ogg_enc_t* e, unsigned char** out, size_t* out_len)
{
  int ret;

  if (e == NULL || out == NULL || out_len == NULL)
    {
      opus_ogg_abort(e);
      return -EINVAL;
    }

  *out = NULL;
  *out_len = 0;

  if (e->pend_len > 0)
    {
      /* 调用者没先调 end_input。这里**故意不编码**: 本函数可能跑在很小的栈上
       * (真机上 voice_stop 就在 16KB 的 CLI 线程里), 一编码就栈溢出。
       * 丢弃的不足 20ms, 对识别无影响。 */
      syslog(LOG_WARNING,
             "[%s] %u trailing bytes dropped (call end_input first)\n",
             TAG, (unsigned)e->pend_len);
      e->pend_len = 0;
    }

  ret = og_flush_page(e, 0x04);                 /* 末页打 EOS */
  if (ret != 0)
    {
      opus_ogg_abort(e);
      return ret;
    }

  syslog(LOG_INFO, "[%s] encoded %u Ogg Opus bytes\n", TAG,
         (unsigned)e->len);

  *out = e->out;
  *out_len = e->len;
  e->out = NULL;                                /* 所有权交给调用者 */
  opus_ogg_abort(e);
  return 0;
}

void opus_ogg_abort(opus_ogg_enc_t* e)
{
  if (e == NULL)
    {
      return;
    }

  if (e->enc != NULL)
    {
      opus_encoder_destroy(e->enc);
    }

  free(e->stage);
  free(e->pktbuf);
  free(e->out);
  free(e);
}

int opus_ogg_encode(const void* pcm, size_t pcm_bytes, int bitrate_bps,
                    unsigned char** out, size_t* out_len)
{
  opus_ogg_enc_t* e;
  int             ret;

  if (pcm == NULL || pcm_bytes == 0 || out == NULL || out_len == NULL)
    {
      return -EINVAL;
    }

  *out = NULL;
  *out_len = 0;

  e = opus_ogg_begin(bitrate_bps);
  if (e == NULL)
    {
      return -ENOMEM;
    }

  ret = opus_ogg_feed(e, pcm, pcm_bytes);
  if (ret != 0)
    {
      opus_ogg_abort(e);
      return ret;
    }

  return opus_ogg_finish(e, out, out_len);
}

#else  /* !CONFIG_LIB_OPUS */

/* 没开 libopus 时全部返回失败: 调用者(minimax_asr.c)会自动回退到
 * "原始 PCM + WAV 头"的老路径, 功能不中断, 只是上传量大得多。 */

opus_ogg_enc_t* opus_ogg_begin(int bitrate_bps)
{
  (void)bitrate_bps;
  return NULL;
}

int opus_ogg_feed(opus_ogg_enc_t* e, const void* pcm, size_t bytes)
{
  (void)e;
  (void)pcm;
  (void)bytes;
  return -ENOSYS;
}

int opus_ogg_finish(opus_ogg_enc_t* e, unsigned char** out, size_t* out_len)
{
  (void)e;
  (void)out;
  (void)out_len;
  return -ENOSYS;
}

void opus_ogg_abort(opus_ogg_enc_t* e)
{
  (void)e;
}

int opus_ogg_encode(const void* pcm, size_t pcm_bytes, int bitrate_bps,
                    unsigned char** out, size_t* out_len)
{
  (void)pcm;
  (void)pcm_bytes;
  (void)bitrate_bps;
  (void)out;
  (void)out_len;
  return -ENOSYS;
}

#endif /* CONFIG_LIB_OPUS */
