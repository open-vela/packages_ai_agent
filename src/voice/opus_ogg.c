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
 * Ogg Opus 封装 (编码 + 容器)
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
/* 20ms @16kbps 的包平均约 40 字节, VBR 尖峰也就两三倍; 512 足够,
 * 真超了 opus_encode 会返回 OPUS_BUFFER_TOO_SMALL 而不是溢出。 */
#define OG_MAX_PKT        512
#define OG_PKTS_PER_PAGE  50             /* 每页最多 50 包(远小于段表 255 上限) */

/* 编码复杂度: 真机实测(16kHz, 4.096 秒音频)
 *   complexity 10 -> 6 秒 (0.68x 实时)
 *   complexity 0  -> 2 秒 (2.05x 实时)
 * 当前这条路径是"录完再编", 不要求实时, 但 c0 已经够用且余量最大;
 * 若以后要"边录边编", c0 的 2 倍余量正是前提。想提质量可以往上调。 */
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

/* ── 页组装 ────────────────────────────────────────────────────────────── */

struct og_ctx
{
  unsigned char* out;
  size_t         cap;
  size_t         len;
  unsigned int   seq;
  unsigned long long granule;                 /* 48kHz 采样累计 */
  const unsigned char* pkt[OG_PKTS_PER_PAGE];
  size_t         plen[OG_PKTS_PER_PAGE];
  int            npkt;

  /* ★ 每个包必须**拷贝**着存下来。og_add() 只记录指针, 而调用方每帧都把
   *   opus_encode() 写回同一个 pkt 缓冲 —— 直接存指针的话, 等攒够一页再落页
   *   时所有指针都指向最后一帧, 整页数据全错(真机表现: 上传成功、HTTP 200,
   *   但识别结果为空字符串)。 */
  unsigned char* stage;
};

static void og_le32(unsigned char* p, unsigned int v)
{
  p[0] = (unsigned char)(v & 0xff);
  p[1] = (unsigned char)((v >> 8) & 0xff);
  p[2] = (unsigned char)((v >> 16) & 0xff);
  p[3] = (unsigned char)((v >> 24) & 0xff);
}

/* 把当前积攒的包封成一页写进 ctx->out, hdr_type: 0x02=BOS 0x04=EOS */
static int og_flush(struct og_ctx* c, int hdr_type)
{
  unsigned char seg[255];
  size_t        body = 0;
  size_t        need;
  size_t        i;
  int           ns = 0;
  unsigned char* p;

  if (c->npkt == 0)
    {
      return 0;
    }

  for (i = 0; i < (size_t)c->npkt; i++)
    {
      size_t n = c->plen[i];

      while (n >= 255)
        {
          seg[ns++] = 255;
          n -= 255;
        }

      seg[ns++] = (unsigned char)n;
      body += c->plen[i];
    }

  need = 27u + (size_t)ns + body;
  if (c->len + need > c->cap)
    {
      return -ENOSPC;
    }

  p = c->out + c->len;
  memcpy(p, "OggS", 4);
  p[4] = 0;                                   /* version */
  p[5] = (unsigned char)hdr_type;
  {
    unsigned long long g = c->granule;
    int k;

    for (k = 0; k < 8; k++)
      {
        p[6 + k] = (unsigned char)((g >> (8 * k)) & 0xff);
      }
  }

  og_le32(p + 14, OG_SERIAL);
  og_le32(p + 18, c->seq++);
  og_le32(p + 22, 0);                         /* CRC 先置 0 */
  p[26] = (unsigned char)ns;
  memcpy(p + 27, seg, (size_t)ns);

  {
    unsigned char* d = p + 27 + ns;

    for (i = 0; i < (size_t)c->npkt; i++)
      {
        memcpy(d, c->pkt[i], c->plen[i]);
        d += c->plen[i];
      }
  }

  og_le32(p + 22, og_crc(p, need));
  c->len += need;
  c->npkt = 0;
  return 0;
}

static int og_add(struct og_ctx* c, const unsigned char* pkt, size_t len,
                  int granule_ticks)
{
  if (c->npkt >= OG_PKTS_PER_PAGE)
    {
      int ret = og_flush(c, 0);

      if (ret != 0)
        {
          return ret;
        }
    }

  /* 拷进 staging 区, 不能只存指针(见 struct og_ctx 里的说明) */
  memcpy(c->stage + (size_t)c->npkt * OG_MAX_PKT, pkt, len);
  c->pkt[c->npkt] = c->stage + (size_t)c->npkt * OG_MAX_PKT;
  c->plen[c->npkt] = len;
  c->npkt++;
  c->granule += (unsigned long long)granule_ticks;
  return 0;
}

/* ── 对外接口 ──────────────────────────────────────────────────────────── */

int opus_ogg_encode(const void* pcm, size_t pcm_bytes, int bitrate_bps,
                    unsigned char** out, size_t* out_len)
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

  /* OpusTags: magic + vendor 长度 + vendor + comment 数 */
  static const unsigned char tags[20] =
  {
    'O', 'p', 'u', 's', 'T', 'a', 'g', 's',
    4, 0, 0, 0,             /* vendor length = 4 */
    'v', 'e', 'l', 'a',
    0, 0, 0, 0              /* comment count = 0 */
  };

  OpusEncoder*          enc = NULL;
  struct og_ctx         ctx;
  unsigned char*        buf = NULL;
  unsigned char*        pkt = NULL;
  const unsigned char*  s = (const unsigned char*)pcm;
  size_t                samples;
  size_t                off;
  size_t                nominal;
  int                   err = OPUS_OK;
  int                   ret;

  if (pcm == NULL || pcm_bytes == 0 || out == NULL || out_len == NULL ||
      bitrate_bps <= 0)
    {
      return -EINVAL;
    }

  *out = NULL;
  *out_len = 0;

  samples = pcm_bytes / 2u;
  if (samples == 0)
    {
      return -EINVAL;
    }

  og_crc_init();

  /* 名义码率 x2 再加容器与 VBR 尖峰的余量 */
  nominal = (size_t)((double)samples / (double)OG_RATE
                     * (double)bitrate_bps / 8.0);
  ctx.cap = nominal * 2u + 8192u;
  ctx.out = (unsigned char*)malloc(ctx.cap);
  pkt = (unsigned char*)malloc(OG_MAX_PKT);
  ctx.stage = (unsigned char*)malloc((size_t)OG_PKTS_PER_PAGE * OG_MAX_PKT);

  if (ctx.out == NULL || pkt == NULL || ctx.stage == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  ctx.len = 0;
  ctx.seq = 0;
  ctx.npkt = 0;

  /* Opus 的 granule 是"含 preskip 的 48kHz 采样计数", 所以从 preskip 起算;
   * 中间页与末页都按 原型(Python, 已对着 MiniMax 验证过) 的算法给值。 */
  ctx.granule = OG_PRESKIP;

  enc = opus_encoder_create(OG_RATE, 1, OPUS_APPLICATION_VOIP, &err);
  if (enc == NULL || err != OPUS_OK)
    {
      syslog(LOG_ERR, "[%s] encoder_create failed: %d\n", TAG, err);
      ret = -EIO;
      goto out;
    }

  (void)opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate_bps));
  (void)opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(OG_COMPLEXITY));
  (void)opus_encoder_ctl(enc, OPUS_SET_VBR(1));

  /* BOS 页(OpusHead) 与 OpusTags 页: granule 都是 0 */
  {
    struct og_ctx h;

    memset(&h, 0, sizeof(h));
    h.out = ctx.out;
    h.cap = ctx.cap;
    h.pkt[0] = head;
    h.plen[0] = sizeof(head);
    h.npkt = 1;
    ret = og_flush(&h, 0x02);
    if (ret != 0)
      {
        goto out;
      }

    ctx.len = h.len;
    ctx.seq = h.seq;

    memset(&h, 0, sizeof(h));
    h.out = ctx.out;
    h.cap = ctx.cap;
    h.len = ctx.len;
    h.seq = ctx.seq;
    h.pkt[0] = tags;
    h.plen[0] = sizeof(tags);
    h.npkt = 1;
    ret = og_flush(&h, 0x00);
    if (ret != 0)
      {
        goto out;
      }

    ctx.len = h.len;
    ctx.seq = h.seq;
  }

  /* 逐帧编码。最后一帧不足 320 点时补零。 */
  {
    short frame[OG_FRAME];

    for (off = 0; off < samples; off += OG_FRAME)
      {
        size_t n = samples - off;
        int    got;

        if (n > OG_FRAME)
          {
            n = OG_FRAME;
          }

        memcpy(frame, s + off * 2u, n * 2u);
        if (n < OG_FRAME)
          {
            memset(frame + n, 0, (OG_FRAME - n) * 2u);
          }

        got = opus_encode(enc, frame, OG_FRAME, pkt, OG_MAX_PKT);
        if (got < 0)
          {
            syslog(LOG_ERR, "[%s] opus_encode failed: %d\n", TAG, got);
            ret = -EIO;
            goto out;
          }

        /* granule 以 48kHz 计, 而输入是 16kHz => x3 */
        ret = og_add(&ctx, pkt, (size_t)got, OG_FRAME * 3);
        if (ret != 0)
          {
            goto out;
          }
      }
  }

  ret = og_flush(&ctx, 0x04);                 /* 末页打 EOS */
  if (ret != 0)
    {
      goto out;
    }

  syslog(LOG_INFO, "[%s] %u PCM bytes -> %u Ogg Opus bytes (%d bps)\n",
         TAG, (unsigned)pcm_bytes, (unsigned)ctx.len, bitrate_bps);

  *out = ctx.out;
  *out_len = ctx.len;
  ctx.out = NULL;
  ret = 0;

out:
  if (enc != NULL)
    {
      opus_encoder_destroy(enc);
    }

  free(pkt);
  free(ctx.stage);
  free(ctx.out);
  return ret;
}

#else  /* !CONFIG_LIB_OPUS */

/* 没开 libopus 时返回 -ENOSYS: 调用者(minimax_asr.c)会自动回退到
 * "原始 PCM + WAV 头"的老路径, 功能不中断, 只是上传量大得多。 */
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
