/****************************************************************************
 * HTTP ASR backend for the Vela/NuttX AI Agent (self-hosted whisper).
 *
 * Wire contract (self-hosted faster-whisper webservice, port 9009):
 *
 *   POST <base_url>?task=transcribe&language=zh&output=text
 *   Content-Type: multipart/form-data; boundary=<b>
 *   part "audio_file": WAV (16kHz / mono / signed 16-bit LE)
 *
 * Plain HTTP/1.1, no TLS, Connection: close. The response body is the
 * transcription as plain text (output=text). If the server ignores the
 * output param and answers JSON, {"text": "..."} is extracted instead.
 ****************************************************************************/

#include "voice/http_asr.h"

#include "agent_compat.h"
#include "agent_config.h"
#include "infra/config_store.h"
#include "voice/voice_asr.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <syslog.h>
#include <unistd.h>      /* [V323] usleep */
#include <unistd.h>

#define HTTP_ASR_TAG "http_asr"

#ifndef CONFIG_AI_AGENT_ASR_HTTP_URL
#define CONFIG_AI_AGENT_ASR_HTTP_URL "http://172.16.88.1:9009/asr"
#endif

/* config.json override key */
#define HTTP_ASR_CFG_KEY "asr_http_url"

#define HTTP_ASR_URL_MAX   256
#define HTTP_ASR_HOST_MAX  96
#define HTTP_ASR_PATH_MAX  192
#define HTTP_ASR_QUERY_MAX 96
#define HTTP_ASR_CHUNK     3072   /* body streaming chunk */
#define HTTP_ASR_CONN_MS   5000   /* connect timeout */
#define HTTP_ASR_IO_MS     30000  /* send/recv timeout (ASR needs time) */
#define HTTP_ASR_RESP_MAX  (16 * 1024)
#define HTTP_ASR_BOUNDARY  "----vox3asr9f2c8a"
#define HTTP_ASR_SRATE     16000

static const char *TAG = HTTP_ASR_TAG;

/* Base URL, resolved at init() time. */
static char s_url[HTTP_ASR_URL_MAX] = CONFIG_AI_AGENT_ASR_HTTP_URL;

/* ── Small helpers ──────────────────────────────────────────── */

/* Parse "http://host[:port]/path". Only plain http is supported. */
static int url_split(const char *url, char *host, size_t hostcap,
                     int *port, char *path, size_t pathcap)
{
  const char *p;

  if (strncmp(url, "http://", 7) != 0)
    {
      syslog(LOG_ERR, "[%s] only http:// supported, got %.8s\n",
             TAG, url);
      return ERROR;
    }

  p = url + 7;
  const char *slash = strchr(p, '/');
  const char *colon = strchr(p, ':');

  if (colon && (!slash || colon < slash))
    {
      size_t hn = (size_t)(colon - p);
      if (hn >= hostcap)
        return ERROR;
      memcpy(host, p, hn);
      host[hn] = '\0';
      *port = atoi(colon + 1);
    }
  else
    {
      size_t hn = slash ? (size_t)(slash - p) : strlen(p);
      if (hn == 0 || hn >= hostcap)
        return ERROR;
      memcpy(host, p, hn);
      host[hn] = '\0';
      *port = 80;
    }

  if (slash)
    {
      if (strlen(slash) >= pathcap)
        return ERROR;
      strcpy(path, slash);
    }
  else
    {
      strcpy(path, "/");
    }

  return OK;
}

/* [V323] 有界连接等待 —— 完全不经过 select()/poll()。
 *
 * 移植自小智（ESP-IDF + esp_http_client）的连接范式：连接等待用定步长轮询 +
 * 硬性 deadline，不依赖 select() 的"可写"通知。
 *
 * 本项目已多次记录 select() 在本 NuttX 上不可靠："[V98C] connect select=0"
 * 在串口反复出现，而所有 "connect ... failed: 110(ETIMEDOUT)" 的报告都出自
 * select 版等待。
 *
 * 做法：非阻塞 connect 发出后只轮询 getsockopt(SO_ERROR)——
 *   err==0            -> 已连接
 *   err==EINPROGRESS  -> sleep 50ms 继续，直到 timeout_ms 用尽
 *   其它              -> 硬错误，立即返回
 * 返回 0 = 已连接；负数 = -errno（-ETIMEDOUT 表示用尽 deadline）。
 */
static int vc_bounded_wait(int fd, int timeout_ms)
{
    int waited = 0;
    const int step_ms = 50;

    while (waited < timeout_ms)
    {
        int err = 0;
        socklen_t elen = sizeof(err);
        unsigned char probe;

        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0)
        {
            int e = errno;

            syslog(LOG_ERR, "[VCONN] fd=%d getsockopt failed errno=%d\n", fd, e);
            return -e;
        }

        /* 只有"非 0 且非 EINPROGRESS"才是硬错误。
         * [V328] 注意：SO_ERROR==0 只表示"目前无挂起错误"，
         * **不代表已连接** —— 连接仍在进行时它同样返回 0。 */

        if (err != 0 && err != EINPROGRESS)
        {
            syslog(LOG_ERR,
                   "[VCONN] fd=%d hard error errno=%d after %dms (no select)\n",
                   fd, err, waited);
            return -err;
        }

        /* [V361] 真正的“已连接”判定：用 recv(MSG_PEEK|MSG_DONTWAIT) 探 1 字节。
         *
         * 为什么不再用 getpeername()：NuttX 在 connect() 发出 SYN 后就把对端
         * 地址写进了 conn，getpeername() 立刻返回 0 —— 三次握手还没完成就报
         * “连接成功”。这就是 [V360] 日志里 connect 假成功、随后 recv 头部空等
         * 41 x 500ms = 20.5s 的根源。
         *
         * 为什么不用重复 connect() 判定：本 NuttX 下 inet_connect() 对已连接
         * 套接字返回 -EISCONN，而 tcp_connect() 对“握手中”
         * (tcpstateflags != TCP_ALLOCATED) 同样返回 -EISCONN —— 两种情况
         * 无法区分。
         *
         * tcp_recvfrom() 的语义刚好可用：
         *   未连接        -> -ENOTCONN          （继续等）
         *   已连接、无数据 -> -EAGAIN/-EWOULDBLOCK（判定成功）
         *   有数据/对端关闭 -> >= 0              （判定成功）
         */

        probe = 0;
        if (recv(fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT) >= 0)
        {
            syslog(LOG_ERR,
                   "[VCONN] fd=%d CONNECTED after %dms (probe: data/EOF)\n",
                   fd, waited);
            return 0;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT)
        {
            syslog(LOG_ERR,
                   "[VCONN] fd=%d CONNECTED after %dms (probe: EAGAIN)\n",
                   fd, waited);
            return 0;
        }

        if (errno != ENOTCONN && errno != EINPROGRESS && errno != EALREADY)
        {
            int e = errno;

            syslog(LOG_ERR,
                   "[VCONN] fd=%d probe hard error errno=%d after %dms\n",
                   fd, e, waited);
            return -e;
        }

        usleep(step_ms * 1000);
        waited += step_ms;
    }

    syslog(LOG_ERR, "[VCONN] fd=%d still connecting after %dms\n",
           fd, timeout_ms);
    return -ETIMEDOUT;
}

/* Blocking connect with timeout. Returns fd or negative errno. */
static int connect_host(const char *host, int port)
{
  char portstr[8];
  struct addrinfo hints;
  struct addrinfo *res = NULL;
  struct timeval tv;
  int fd;
  int flags;
  int rc;

  snprintf(portstr, sizeof(portstr), "%d", port);
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  if (getaddrinfo(host, portstr, &hints, &res) != 0 || res == NULL)
    {
      syslog(LOG_ERR, "[%s] dns failed: %s\n", TAG, host);
      return -EHOSTUNREACH;
    }

  fd = socket(res->ai_family, res->ai_socktype, 0);
  if (fd < 0)
    {
      freeaddrinfo(res);
      return -errno;
    }

  flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  rc = connect(fd, res->ai_addr, res->ai_addrlen);
  freeaddrinfo(res);
  if (rc < 0)
    {
      /* [V323] 移植小智的连接范式：不依赖 select() 的有界等待 */

      if (errno != EINPROGRESS)
        {
          int e = errno;

          syslog(LOG_ERR, "[%s] connect %s:%d immediate fail errno=%d\n",
                 TAG, host, port, e);
          close(fd);
          return -e;
        }

      {
        int w = vc_bounded_wait(fd, HTTP_ASR_CONN_MS);

        if (w < 0)
          {
            syslog(LOG_ERR, "[%s] connect %s:%d failed errno=%d (no select)\n",
                   TAG, host, port, -w);
            close(fd);
            return w;
          }
      }
    }

  fcntl(fd, F_SETFL, flags);

  tv.tv_sec = HTTP_ASR_IO_MS / 1000;
  tv.tv_usec = (HTTP_ASR_IO_MS % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  return fd;
}

static int send_all(int fd, const void *buf, size_t len)
{
  const char *p = buf;

  while (len > 0)
    {
      ssize_t n = send(fd, p, len, 0);
      if (n <= 0)
        {
          syslog(LOG_ERR, "[%s] send failed errno=%d\n", TAG, errno);
          return ERROR;
        }
      p += n;
      len -= n;
    }

  return OK;
}

/* 44-byte canonical PCM WAV header. */
static void wav_header(unsigned char h[44], size_t pcm_len,
                       int rate, int ch, int bits)
{
  uint32_t byte_rate = (uint32_t)rate * ch * bits / 8;
  uint16_t block_align = (uint16_t)(ch * bits / 8);
  uint32_t data_len = (uint32_t)pcm_len;
  uint32_t riff_len = 36 + data_len;
  int i = 0;

  memcpy(h + i, "RIFF", 4); i += 4;
  memcpy(h + i, &riff_len, 4); i += 4;
  memcpy(h + i, "WAVE", 4); i += 4;
  memcpy(h + i, "fmt ", 4); i += 4;
  h[i++] = 16; h[i++] = 0;                       /* fmt chunk size */
  h[i++] = 1; h[i++] = 0;                        /* PCM */
  h[i++] = (unsigned char)ch; h[i++] = 0;
  memcpy(h + i, &rate, 4); i += 4;
  memcpy(h + i, &byte_rate, 4); i += 4;
  h[i++] = (unsigned char)block_align; h[i++] = 0;
  h[i++] = (unsigned char)bits; h[i++] = 0;
  memcpy(h + i, "data", 4); i += 4;
  memcpy(h + i, &data_len, 4);
}

/* Case-insensitive substring search (NuttX-safe, no strcasestr). */
static const char *ci_strnstr(const char *hay, size_t haylen,
                              const char *needle)
{
  size_t nlen = strlen(needle);

  if (nlen == 0 || haylen < nlen)
    return NULL;

  for (size_t i = 0; i <= haylen - nlen; i++)
    {
      size_t j;
      for (j = 0; j < nlen; j++)
        {
          if (tolower((unsigned char)hay[i + j]) !=
              tolower((unsigned char)needle[j]))
            break;
        }
      if (j == nlen)
        return hay + i;
    }

  return NULL;
}

/* Minimal HTTP chunked-transfer decoder, in place. Returns new len. */
static size_t dechunk(char *buf, size_t len)
{
  char *out = buf;
  char *p = buf;
  char *end = buf + len;

  while (p < end)
    {
      char *le = memchr(p, '\n', end - p);
      long sz;

      if (le == NULL)
        break;
      sz = strtol(p, NULL, 16);
      if (sz <= 0)
        break;
      p = le + 1;
      if (p + sz > end)
        sz = end - p;
      memmove(out, p, sz);
      out += sz;
      p += sz;
      if (p < end && *p == '\r')
        p++;
      if (p < end && *p == '\n')
        p++;
    }

  return out - buf;
}

/* Trim whitespace/CRLF in place into text_out. */
static void copy_trimmed(const char *src, size_t len,
                         char *text_out, size_t text_cap)
{
  while (len > 0 && isspace((unsigned char)*src))
    {
      src++;
      len--;
    }
  while (len > 0 && isspace((unsigned char)src[len - 1]))
    len--;

  if (len >= text_cap)
    len = text_cap - 1;
  memcpy(text_out, src, len);
  text_out[len] = '\0';
}

/* Extract "text" value from a {"text": "..."} JSON body. */
static bool extract_json_text(const char *body, size_t len,
                              char *text_out, size_t text_cap)
{
  const char *key = "\"text\"";
  const char *k = ci_strnstr(body, len, key);
  const char *p;
  const char *e;
  char *o = text_out;
  size_t left = text_cap - 1;

  if (k == NULL)
    return false;

  p = k + strlen(key);
  while (p < body + len && *p != '"' && *p != ':')
    p++;
  if (p >= body + len || *p != ':')
    return false;
  p++;
  while (p < body + len && isspace((unsigned char)*p))
    p++;
  if (p >= body + len || *p != '"')
    return false;
  p++;

  for (e = p; e < body + len && left > 0; e++)
    {
      if (*e == '"')
        break;
      if (*e == '\\' && e + 1 < body + len)
        {
          e++;
          switch (*e)
            {
            case 'n': *o++ = '\n'; left--; break;
            case 't': *o++ = '\t'; left--; break;
            case 'r': break;
            default:  *o++ = *e;   left--; break;
            }
        }
      else
        {
          *o++ = *e;
          left--;
        }
    }

  *o = '\0';
  return true;
}

/* ── Backend ops ────────────────────────────────────────────── */

int http_asr_init(void)
{
  char buf[HTTP_ASR_URL_MAX];

  if (claw_config_get(HTTP_ASR_CFG_KEY, buf, sizeof(buf)) == OK
      && buf[0] != '\0')
    {
      strncpy(s_url, buf, sizeof(s_url) - 1);
      s_url[sizeof(s_url) - 1] = '\0';
    }

  syslog(LOG_INFO, "[%s] endpoint: %s\n", TAG, s_url);
  return OK;
}

void http_asr_set_url(const char *url)
{
  if (url == NULL || url[0] == '\0')
    return;
  strncpy(s_url, url, sizeof(s_url) - 1);
  s_url[sizeof(s_url) - 1] = '\0';
  syslog(LOG_INFO, "[%s] url overridden: %s\n", TAG, s_url);
}

int http_asr_recognize(const unsigned char *pcm_data,
                              size_t pcm_len,
                              char *text_out, size_t text_cap)
{
  char host[HTTP_ASR_HOST_MAX];
  char path[HTTP_ASR_PATH_MAX];
  char fullpath[HTTP_ASR_PATH_MAX + HTTP_ASR_QUERY_MAX];
  char req[512];
  unsigned char wavhdr[44];
  int port;
  int fd;
  int ret;
  char *resp = NULL;
  size_t resp_len = 0;

  if (text_cap == 0)
    return -EINVAL;
  text_out[0] = '\0';

  if (pcm_data == NULL || pcm_len < 320)
    {
      syslog(LOG_ERR, "[%s] pcm too short (%zu)\n", TAG, pcm_len);
      return -EINVAL;
    }
  if (pcm_len > 4u * 1024 * 1024)
    {
      syslog(LOG_ERR, "[%s] pcm too long (%zu)\n", TAG, pcm_len);
      return -EINVAL;
    }
  if (url_split(s_url, host, sizeof(host), &port,
                path, sizeof(path)) != OK)
    return -EINVAL;

  if (strchr(path, '?'))
    snprintf(fullpath, sizeof(fullpath),
             "%s&task=transcribe&language=zh&output=text", path);
  else
    snprintf(fullpath, sizeof(fullpath),
             "%s?task=transcribe&language=zh&output=text", path);

  {
    static const char part1[] =
      "--" HTTP_ASR_BOUNDARY "\r\n"
      "Content-Disposition: form-data; name=\"audio_file\";"
      " filename=\"audio.wav\"\r\n"
      "Content-Type: audio/wav\r\n"
      "\r\n";
    static const char part3[] =
      "\r\n--" HTTP_ASR_BOUNDARY "--\r\n";
    size_t clen = strlen(part1) + 44 + pcm_len + strlen(part3);

    snprintf(req, sizeof(req),
             "POST %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Content-Type: multipart/form-data; boundary="
             HTTP_ASR_BOUNDARY "\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n",
             fullpath, host, port, clen);

    syslog(LOG_INFO, "[%s] POST %s (%zu bytes pcm, %.1fs)\n",
           TAG, fullpath, pcm_len, (double)pcm_len / 32000.0);

    fd = connect_host(host, port);
    if (fd < 0)
      return fd;

    wav_header(wavhdr, pcm_len, HTTP_ASR_SRATE, 1, 16);

    ret = -EIO;
    do
      {
        bool sendok;
        size_t off;

        if (send_all(fd, req, strlen(req)) != OK ||
            send_all(fd, part1, strlen(part1)) != OK ||
            send_all(fd, wavhdr, 44) != OK)
          break;

        sendok = true;
        for (off = 0; off < pcm_len; off += HTTP_ASR_CHUNK)
          {
            size_t k = pcm_len - off;
            if (k > HTTP_ASR_CHUNK)
              k = HTTP_ASR_CHUNK;
            if (send_all(fd, pcm_data + off, k) != OK)
              {
                sendok = false;
                break;
              }
          }
        if (!sendok || send_all(fd, part3, strlen(part3)) != OK)
          break;

        resp = malloc(HTTP_ASR_RESP_MAX);
        if (resp == NULL)
          {
            ret = -ENOMEM;
            break;
          }

        for (;;)
          {
            ssize_t r;

            if (resp_len >= HTTP_ASR_RESP_MAX - 1)
              break;
            r = recv(fd, resp + resp_len,
                     HTTP_ASR_RESP_MAX - 1 - resp_len, 0);
            if (r <= 0)
              break;
            resp_len += (size_t)r;
          }
        resp[resp_len] = '\0';
      }
    while (0);

    close(fd);

    if (ret != -EIO)
      {
        free(resp);
        return ret;
      }
  }

  /* Parse response. */
  if (resp_len < 12 || strncmp(resp, "HTTP/", 5) != 0)
    {
      syslog(LOG_ERR, "[%s] bad response\n", TAG);
      free(resp);
      return -EPROTO;
    }

  {
    int status = atoi(resp + 9);
    char *body = strstr(resp, "\r\n\r\n");
    const char *text;
    size_t tlen;

    if (status != 200)
      {
        syslog(LOG_ERR, "[%s] HTTP %d\n", TAG, status);
        free(resp);
        return -EPROTO;
      }
    if (body == NULL)
      {
        free(resp);
        return -EPROTO;
      }

    *body = '\0';
    body += 4;

    if (ci_strnstr(resp, body - resp, "chunked") != NULL)
      resp_len = dechunk(body, resp + resp_len - body) + (body - resp);

    if (resp_len <= (size_t)(body - resp))
      {
        syslog(LOG_ERR, "[%s] empty body\n", TAG);
        free(resp);
        return -EIO;
      }

    tlen = resp_len - (size_t)(body - resp);

    if (tlen > 0 && body[0] == '{'
        && extract_json_text(body, tlen, text_out, text_cap))
      {
        text = text_out;  /* json fallback path */
      }
    else
      {
        copy_trimmed(body, tlen, text_out, text_cap);
        text = text_out;
      }

    if (text[0] == '\0')
      {
        syslog(LOG_WARNING, "[%s] empty transcription\n", TAG);
        free(resp);
        return -EIO;
      }

    syslog(LOG_INFO, "[%s] recognized: %s\n", TAG, text_out);
    free(resp);
    return OK;
  }
}

static const voice_asr_ops_t s_http_asr_ops =
{
  .name      = "http",
  .init      = http_asr_init,
  .recognize = http_asr_recognize,
  .deinit    = NULL,
};

int http_asr_register(void)
{
  return voice_asr_register(&s_http_asr_ops);
}
