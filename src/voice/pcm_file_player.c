/****************************************************************************
 * pcm_file_player.c - 播放 /data 下的原始 PCM 文件（见头文件说明）
 *
 * 数据流：
 *   read() 文件 -> media_player_write_data() -> 96KB 环形缓冲
 *              -> 设备 worker -> /dev/audio/pcm0p -> 喇叭
 *
 * 限速方式：每块 3200 字节（100ms 音频），写完 usleep(100ms)。
 * 不这么做的话，文件读得比播放快，96KB 缓冲 3 秒就满，之后一直丢最旧的。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <unistd.h>

#include <nuttx/audio/audio.h>

#include "pcm_file_player.h"

/* 这两个是 media_player 兼容层的入口（src/voice/pcm_media_player.c） */
void *media_player_open(const char *url);
int   media_player_prepare(void *player, const char *url, const char *opts);
int   media_player_start(void *player);
ssize_t media_player_write_data(void *player, const void *data, size_t len);
int   media_player_stop(void *player);
int   media_player_close(void *player, int dummy);
/* 用播放器自己的 fd 设音量（pcm_media_player.c 里实现） */
int   media_player_set_volume(void *player, int hw_volume);

#define TAG "[pcm_file]"

/* 16kHz x 2 字节 x 1 声道 = 32000 字节/秒。
 *
 * 块大小必须和设备侧的 APB 对齐（sf32lb52_audio_tx.c 里 TX_APB_BYTES = 4096）。
 * 之前用 3200 字节（100ms）比 APB 小，设备经常凑不满一块就欠载 ——
 * 实测 30 秒里有 441 次欠载（441 x 128ms ≈ 56 秒静音），结果几乎听不到声音。
 * 现在按 4096 字节（128ms）写，并在开头预灌 PREBUF_CHUNKS 块把队列填满。 */
#define BYTES_PER_SEC   32000
#define CHUNK_BYTES     4096
#define CHUNK_MS        128
/* 设备 APB 队列是 8 深，全部预灌满 = 32KB = 1 秒余量。
 * 原来只预灌 4 块（512ms），稍微一抖就断粮。 */
#define PREBUF_CHUNKS   8

/* 8KB 不够：任务里有一个 4096 字节的读缓冲，再加上 media_player_* 的调用深度，
 * 实测播到第 83 块（约 10.6 秒）任务就退出了。16KB 留足余量。 */
#define PLAYER_STACK    16384
#define PLAYER_PRIO     95

static volatile bool s_stop_req;
static volatile bool s_playing;

/* ★ 单例保护（2026-10-21 崩溃修复的核心）。
 *
 * 之前 pcm_file_play() 只是"再起一个 pcm_play 任务"，旧任务可能还卡在
 * 128ms 喂数据睡眠 / 300ms 排空 / 5 秒 stop 等待里，于是连点几次就有
 * 几个播放器并行操作同一个 /dev/audio/pcm0p 和同一个 mqueue。
 * 而 NuttX 把 DEQUEUE 广播给所有客户端（audio.c:1430-1437），
 * 同一个 APB 会进两个播放器的空闲池 → 链表自环 → 双 free → 看门狗复位。
 *
 * 现在：同一时刻只允许一个播放任务；新请求必须等旧的真正退出，
 * 等不到就**拒绝**，绝不再创建第二个。 */
static volatile bool s_task_alive;

/* ★ 内存音源（2026-10-22 新增）
 *
 * 为什么：内置演示曲的 960KB PCM 本来就以 const 数组的形式编进固件了
 * （ui/assets/audio/demo_music.c），但之前的实现又把它往 littlefs 写了一份，
 * 播放时再从 /data/music/demo1.pcm 读回来。实测读到 1.5~2.4 秒时
 * read() 返回 -1，播放直接中断 —— 用户听到的就是"只有一小段卡住的声音"。
 *
 * 既然数据已经在固件里，就没必要绕一趟 flash 文件系统：
 * 直接从内存喂，既不会读失败，也不占 /data 的空间。 */
/* ★ 暂停（2026-10-22）
 *
 * 原来 PAUSE 直接调 pcm_file_stop()，等于"停止"，位置也丢了 ——
 * 用户按暂停听到的是继续播完缓存里的 3 秒然后静默，再按继续又从头开始。
 *
 * 现在的做法：暂停时由**播放任务自己**把音频设备停掉（不排空、不释放），
 * 位置和文件句柄都保留；恢复时重新 start，从原来的位置接着读。
 * 关键点是所有设备操作都在播放任务里做，UI 线程只改标志。 */
static volatile bool s_pause_req;
static volatile bool s_paused;

static const uint8_t *s_src_mem;
static size_t         s_src_len;
static size_t         s_src_off;

/* ★ 2026-11-01 暂停重做：暂停 = 停掉播放任务并记住位置；恢复 = 从该位置
 * 重新起任务。
 *
 * 旧做法是"暂停期间设备继续跑、按节奏喂静音块"，真机证明这会在暂停/恢复
 * 后让音频设备的 APB 卡死（8 个缓冲不归还、DMA 半中断停摆），表现为
 * 声音卡死、暂停按钮失效。现在暂停直接让任务退出，不再和停摆的设备纠缠。 */
static volatile bool s_pause_keep;      /* 本次 stop 是"暂停"而不是"停止" */
static size_t        s_resume_off;      /* 暂停时已消费的源字节 */
static int           s_resume_ms;       /* 暂停时的显示位置 */
static size_t        s_start_off;       /* 本任务起始源字节 */
static int           s_start_pos_ms;    /* 本任务起始显示位置 */

/* 播放任务退出时 post，供 pcm_file_play 等待 */
static sem_t s_done;

#define STOP_WAIT_MS   3000    /* 等旧任务退出的上限 */
static volatile int  s_pos_ms;
static volatile int  s_dur_ms;
static volatile int  s_last_result;

/* 音量：0~100。映射到硬件的 -36..54（0.5dB 单位）。
 * 默认 40 —— 换算下来 hw = -36 + 36 = 0，正好是驱动原本的默认 0 dB，
 * 不会因为"播放器顺手设了个音量"而突然变响或削波。 */
static volatile int  s_volume_pct = 40;
static volatile int  s_volume_applied = -1;

static char s_path[128];

/****************************************************************************
 * 音量（真正下发到硬件）
 ****************************************************************************/

/* 把 0~100 映射到 AUDCODEC 的 DAC 音量单位（0.5dB 单位，范围 -36..54）。
 *
 * ★ 上界必须是 0dB，不能到 +54。
 *
 * 原来写的是 -36 + pct*90/100，于是：
 *     40% → 0    (0 dB)
 *    100% → 54   (+27 dB 数字增益！)
 * 而本机播放的 PCM 峰值已到 14231/32767，再叠 +27dB 就是**彻底削波**，
 * 听感是又响又糊，而且削波后的方波会让功放/编解码器工作异常 ——
 * 实测调到 100% 后播放直接卡死。
 *
 * 现在 100% = 0 dB（满幅但不越界），60% ≈ -14dB，40% ≈ -22dB。 */
static int pct_to_hw(int pct)
{
  if (pct < 0)
    {
      pct = 0;
    }

  if (pct > 100)
    {
      pct = 100;
    }

  return -36 + (pct * 36) / 100;
}

/* ★ 千万别自己 open 第二个 /dev/audio/pcm0p 来设音量。
 *
 * 踩过的坑：另开一个 fd 发 AUDIO_FU_VOLUME，audio_tx 驱动会重新初始化通路，
 * 正在播的声音直接消失 —— 表现就是"点一下音量，歌没了"。
 * 必须用播放器自己持有的那个 fd，所以调用转到 pcm_media_player.c 里新加的
 * media_player_set_volume()。 */
static void *s_cur_player;     /* 当前播放器句柄，设音量要用它的 fd */

static void apply_volume(int pct)
{
  int hw = pct_to_hw(pct);

  if (s_cur_player == NULL)
    {
      return;      /* 还没开始播放：只记住值，开播时再应用 */
    }

  if (media_player_set_volume(s_cur_player, hw) < 0)
    {
      syslog(LOG_WARNING, "%s set volume %d%% (hw %d) failed: %d\n",
             TAG, pct, hw, errno);
      return;
    }

  syslog(LOG_INFO, "%s volume -> %d%% (hw %d)\n", TAG, pct, hw);
}

/****************************************************************************
 * 播放任务
 ****************************************************************************/

static int pcm_player_task(int argc, FAR char *argv[])
{
  unsigned char buf[CHUNK_BYTES];
  void *player = NULL;
  int fd = -1;
  ssize_t got;
  int ret;

  (void)argc;
  (void)argv;

#define PCM_TASK_EXIT(res)                    \
  do {                                        \
      s_last_result = (res);                  \
      s_playing = false;                      \
      s_task_alive = false;                   \
      sem_post(&s_done);                      \
      return 0;                               \
    } while (0)

  s_task_alive = true;

  if (s_src_mem != NULL)
    {
      /* 内存音源：不碰文件系统 */
      s_dur_ms = (int)((s_src_len / BYTES_PER_SEC) * 1000);
      syslog(LOG_INFO, "%s playing from memory (%u bytes, %d ms)\n",
             TAG, (unsigned)s_src_len, s_dur_ms);
    }
  else
    {
      fd = open(s_path, O_RDONLY);
      if (fd < 0)
        {
          syslog(LOG_ERR, "%s open %s failed: %d\n", TAG, s_path, errno);
          PCM_TASK_EXIT(-1);
        }

      syslog(LOG_INFO, "%s opened %s as fd=%d\n", TAG, s_path, fd);

      /* 时长由文件大小直接算：格式是固定的 32KB/s，不需要解析头部 */
      s_dur_ms = (int)((lseek(fd, 0, SEEK_END) / BYTES_PER_SEC) * 1000);
      lseek(fd, (off_t)s_start_off, SEEK_SET);
      s_src_off = s_start_off;
    }

  s_pos_ms = s_start_pos_ms;

  player = media_player_open("pcm_file");
  if (player == NULL)
    {
      syslog(LOG_ERR, "%s media_player_open failed\n", TAG);
      close(fd);
      PCM_TASK_EXIT(-1);
    }

  ret = media_player_prepare(player, s_path,
                             "format=s16le:sample_rate=16000:ch_layout=mono");
  if (ret < 0)
    {
      syslog(LOG_ERR, "%s prepare failed: %d\n", TAG, ret);
      media_player_close(player, 0);
      close(fd);
      PCM_TASK_EXIT(-1);
    }

  /* 记下句柄，之后音量 ioctl 走它自己的 fd（绝不能再开第二个 fd） */
  s_cur_player = player;

  ret = media_player_start(player);
  if (ret < 0)
    {
      syslog(LOG_ERR, "%s start failed: %d\n", TAG, ret);
      media_player_close(player, 0);
      close(fd);
      PCM_TASK_EXIT(-1);
    }

  /* 开播前先按当前音量下发一次，否则第一秒是上一次的音量 */
  apply_volume(s_volume_pct);
  s_volume_applied = s_volume_pct;

  syslog(LOG_INFO, "%s playing %s (%d ms, chunk=%d, prefill=%d)\n",
         TAG, s_path, s_dur_ms, CHUNK_BYTES, PREBUF_CHUNKS);

  int prefill = PREBUF_CHUNKS;
  int chunk_no = 0;
  struct timespec t0;

  /* ★ 绝对时间调度，不用相对 usleep。
   *
   * 为什么：CONFIG_USEC_PER_TICK = 10000（1 tick = 10ms），
   * usleep(128000) 实际会睡 13 个 tick = 130ms。而我们每块要喂的是
   * 128ms 的音频 —— 每块慢 2ms，攒 64 块就落后整整一个缓冲，
   * 之后设备持续欠载、补静音，听感就是"不成句的断续碎片"。
   * 实测 underruns=562 而 halves 只有 240，正是这个原因。
   *
   * 改成"起始时刻 + n x CHUNK_MS"计算目标时刻，误差不累积。 */
  clock_gettime(CLOCK_MONOTONIC, &t0);

  while (!s_stop_req)
    {
      /* ── 暂停 ───────────────────────────────────────────────
       *
       * ★ 关键设计：暂停**不碰音频设备**。
       *
       * 试过的做法是"暂停时 ioctl(AUDIOIOC_STOP)，恢复时 AUDIOIOC_START"，
       * 结果恢复后完全没有声音：数据照样喂进环形缓冲，但 DMA 不跑。
       * 原因有两处：
       *   - audio.c 处理 AUDIOIOC_STOP 后无条件把 priv->state 置 OPEN，
       *     再次 START 时上半部的状态机已经不是原来那条路；
       *   - 驱动 tx_start() 开头是 if (priv->running) return OK;
       *     —— 状态不一致时它**静默返回**，什么也不做。
       * 实测：paused/resumed 之后 chunk 一路跑到 230，却一个音都没有，
       * 且 audio_tx 的统计再没出现过（停止根本没传到驱动）。
       *
       * 现在的做法：设备一直跑，暂停期间按原节奏喂**静音块**。
       * 听感上立即静音，位置不前进，恢复时无缝接着放。
       * 而且静音块占用节奏槽，target_ms 继续推进 —— 不需要任何
       * 时间补偿，恢复时不会出现"憋了一大坨再爆发"的副作用。
       * 全程不改任何设备状态，结构上不可能再不一致。 */
      bool silent = false;

      if (s_pause_req)
        {
          if (!s_paused)
            {
              s_paused = true;
              syslog(LOG_INFO, "%s paused at %d/%d ms\n",
                     TAG, s_pos_ms, s_dur_ms);
            }

          memset(buf, 0, sizeof(buf));
          got = (ssize_t)sizeof(buf);
          silent = true;
        }
      else
        {
          if (s_paused)
            {
              s_paused = false;
              syslog(LOG_INFO, "%s resumed at %d/%d ms\n",
                     TAG, s_pos_ms, s_dur_ms);
            }

          if (s_src_mem != NULL)
            {
              /* 内存音源：直接拷，不涉及任何 IO */
              size_t left = s_src_len - s_src_off;

              got = (ssize_t)((left < sizeof(buf)) ? left : sizeof(buf));

              if (got > 0)
                {
                  memcpy(buf, s_src_mem + s_src_off, (size_t)got);
                  s_src_off += (size_t)got;
                }
            }
          else
            {
              got = read(fd, buf, sizeof(buf));
              if (got > 0)
                {
                  s_src_off += (size_t)got;   /* 让暂停能记住文件位置 */
                }
            }
        }

      if (got <= 0)
        {
          /* 区分 EOF 和真错误。内存音源走不到这里（got==0 就是正常播完）。 */
          if (s_src_mem == NULL && got < 0)
            {
              syslog(LOG_ERR,
                     "%s read failed (errno=%d) at chunk %d (%d ms), fd=%d\n",
                     TAG, errno, chunk_no, s_pos_ms, fd);
              break;
            }

          /* 恢复普通 EOF 收尾（2026-11-01 A 测试证明"不暂停、自然放完"
           * 这条路径是干净的；循环播放反而引入了新的边界问题）。 */
          syslog(LOG_INFO, "%s EOF at chunk %d (%d ms)\n",
                 TAG, chunk_no, s_pos_ms);
          break;              /* EOF 或出错，都当播完 */
        }

      /* 每 10 块打一行。这样"卡在哪一块"是日志直接告诉我们的，
       * 不用再靠 ps 和推断。 */
      if (!silent && chunk_no % 10 == 0)
        {
          syslog(LOG_INFO, "%s chunk %d, pos %d ms, prefill %d\n",
                 TAG, chunk_no, s_pos_ms, prefill);
        }

      ssize_t wr = media_player_write_data(player, buf, (size_t)got);
      if (wr < 0)
        {
          syslog(LOG_WARNING, "%s write_data failed at chunk %d (%d ms): %d\n",
                 TAG, chunk_no, s_pos_ms, (int)wr);
          break;
        }

      chunk_no++;

      if (!silent)
        {
          s_pos_ms += CHUNK_MS;
        }

      /* 音量变化时下发一次（放循环里是为了不用跨任务共享 fd） */
      if (s_volume_applied != s_volume_pct)
        {
          s_volume_applied = s_volume_pct;
          apply_volume(s_volume_pct);
        }

      /* 开头几块不停顿，先把 APB 队列灌满；之后按绝对时间对齐。 */
      if (prefill > 0 && !silent)
        {
          prefill--;
        }
      else
        {
          /* 目标时刻 = t0 + chunk_no * CHUNK_MS，与前面累计误差无关 */
          long target_ms = (long)chunk_no * CHUNK_MS;
          struct timespec now;
          long elapsed_ms;

          clock_gettime(CLOCK_MONOTONIC, &now);
          /* 静音块也占用节奏槽（chunk_no 照样 +1），
           * 所以这里不需要任何暂停补偿 —— 恢复时天然无缝。 */
          elapsed_ms = (now.tv_sec - t0.tv_sec) * 1000L
                     + (now.tv_nsec - t0.tv_nsec) / 1000000L;

          if (target_ms > elapsed_ms)
            {
              usleep((useconds_t)((target_ms - elapsed_ms) * 1000));
            }
        }
    }

  /* 暂停：把当前位置留给 pcm_file_resume()（此时源偏移已停止推进） */
  if (s_pause_keep)
    {
      s_resume_off = s_src_off;
      s_resume_ms  = s_pos_ms;
      syslog(LOG_INFO, "%s paused at %d/%d ms (keep for resume)\n",
             TAG, s_resume_ms, s_dur_ms);
    }

  /* 让缓冲里剩下的音频放完再停，否则尾音会被切掉 */
  if (!s_stop_req)
    {
      usleep(300 * 1000);
    }

  media_player_stop(player);

  /* ★ 必须在 close（内部会 free 掉 player）之前把手柄清掉，
   * 否则音量接口会从已释放的结构里读 fd，拿到 EBADF —— 实测日志：
   *   [pcm_file] set volume 50% (hw 9) failed: 9 */
  s_cur_player = NULL;

  media_player_close(player, 0);

  if (fd >= 0)
    {
      close(fd);
    }

  syslog(LOG_INFO, "%s finished at %d/%d ms (stop_req=%d)\n",
         TAG, s_pos_ms, s_dur_ms, (int)s_stop_req);

  PCM_TASK_EXIT(s_stop_req ? 0 : 1);

#undef PCM_TASK_EXIT
}

/****************************************************************************
 * 公开接口
 ****************************************************************************/

/* 一次性初始化（幂等） */
static void pcm_file_once(void)
{
  static bool inited;

  if (!inited)
    {
      inited = true;
      sem_init(&s_done, 0, 0);
    }
}

/* 等上一个播放任务退出。等不到就返回 -EBUSY，绝不再起第二个。 */
static int pcm_wait_prev(void)
{
  int i;

  if (!s_task_alive)
    {
      return 0;
    }

  s_stop_req = true;

  /* 清掉可能残留的 done 计数（暂停掉的任务也会 post 一次），
   * 否则下一次等待可能立刻"假成功"导致误报 -EBUSY。 */
  while (sem_trywait(&s_done) == OK)
    {
    }

  for (i = 0; i < STOP_WAIT_MS / 10 && s_task_alive; i++)
    {
      struct timespec ts;

      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 10 * 1000 * 1000;
      if (ts.tv_nsec >= 1000000000)
        {
          ts.tv_sec++;
          ts.tv_nsec -= 1000000000;
        }

      if (sem_timedwait(&s_done, &ts) == OK)
        {
          break;
        }
    }

  if (s_task_alive)
    {
      syslog(LOG_WARNING,
             "%s previous task still alive after %d ms, refusing to start\n",
             TAG, STOP_WAIT_MS);
      return -EBUSY;
    }

  return 0;
}

int pcm_mem_play(const void *data, size_t len, const char *name)
{
  int pid;

  if (data == NULL || len < CHUNK_BYTES)
    {
      return -EINVAL;
    }

  pcm_file_once();

  if (pcm_wait_prev() != 0)
    {
      return -EBUSY;
    }

  s_src_mem  = (const uint8_t *)data;
  s_src_len  = len;
  s_src_off  = 0;
  s_pause_keep = false;
  s_paused     = false;
  s_start_off  = 0;
  s_start_pos_ms = 0;

  snprintf(s_path, sizeof(s_path), "%s", name != NULL ? name : "(memory)");
  s_stop_req  = false;
  s_pause_req = false;
  s_paused    = false;
  s_pos_ms    = 0;
  s_dur_ms    = 0;
  s_last_result = 0;
  s_playing   = false;
  s_task_alive = true;

  pid = task_create("pcm_play", PLAYER_PRIO, PLAYER_STACK,
                    pcm_player_task, NULL);
  if (pid < 0)
    {
      syslog(LOG_ERR, "%s task_create failed: %d\n", TAG, errno);
      s_task_alive = false;
      s_playing = false;
      s_src_mem = NULL;
      s_last_result = -1;
      return -errno;
    }

  s_playing = true;
  return 0;
}

void pcm_file_pause(void)
{
  if (!s_task_alive || s_paused)
    {
      return;
    }

  /* 暂停 = 让播放任务退出（它会在退出前保存位置），不再喂静音块。 */
  s_paused     = true;
  s_pause_keep = true;
  s_stop_req   = true;
  syslog(LOG_INFO, "%s pause requested (task will stop & keep offset)\n",
         TAG);
}

void pcm_file_resume(void)
{
  int pid;

  if (!s_paused)
    {
      return;
    }

  pcm_file_once();

  if (pcm_wait_prev() != 0)
    {
      syslog(LOG_WARNING, "%s resume: previous task busy\n", TAG);
      return;
    }

  s_paused       = false;
  s_pause_keep   = false;
  s_start_off    = s_resume_off;
  s_start_pos_ms = s_resume_ms;
  s_stop_req     = false;
  s_pause_req    = false;
  s_volume_applied = -1;
  s_last_result  = 0;
  s_task_alive   = true;

  pid = task_create("pcm_play", PLAYER_PRIO, PLAYER_STACK,
                    pcm_player_task, NULL);
  if (pid < 0)
    {
      syslog(LOG_ERR, "%s resume task_create failed: %d\n", TAG, errno);
      s_task_alive = false;
      s_playing    = false;
      s_last_result = -1;
      return;
    }

  s_playing = true;
  syslog(LOG_INFO, "%s resumed from %d ms (off=%u)\n",
         TAG, s_start_pos_ms, (unsigned)s_start_off);
}

int pcm_file_is_paused(void)
{
  return s_paused ? 1 : 0;
}

int pcm_file_play(const char *path)
{
  int pid;
  int i;

  if (path == NULL || path[0] == '\0')
    {
      return -EINVAL;
    }

  pcm_file_once();

  /* ① 先让旧任务退出。等不到就**拒绝**，绝不再起第二个。
   *
   * 这一步是整个崩溃修复的核心：之前是"置个标志 + 睡 50ms + 无脑
   * task_create"，而旧任务可能正处于 128ms 的喂数据睡眠、300ms 的排空、
   * 或 5 秒的 stop 等待 —— 50ms 后它必然还活着。 */
  if (s_task_alive)
    {
      s_stop_req = true;

      for (i = 0; i < STOP_WAIT_MS / 10 && s_task_alive; i++)
        {
          struct timespec ts;

          clock_gettime(CLOCK_REALTIME, &ts);
          ts.tv_nsec += 10 * 1000 * 1000;
          if (ts.tv_nsec >= 1000000000)
            {
              ts.tv_sec++;
              ts.tv_nsec -= 1000000000;
            }

          if (sem_timedwait(&s_done, &ts) == OK)
            {
              break;
            }
        }

      if (s_task_alive)
        {
          syslog(LOG_WARNING,
                 "%s previous task still alive after %d ms, refusing to start\n",
                 TAG, STOP_WAIT_MS);
          return -EBUSY;
        }
    }

  /* ② 到这里旧任务一定已经退出，可以安全覆盖共享状态 */
  snprintf(s_path, sizeof(s_path), "%s", path);
  s_stop_req  = false;
  s_pause_req = false;
  s_paused    = false;
  s_src_mem   = NULL;         /* 文件音源 */
  s_pause_keep = false;
  s_paused     = false;
  s_start_off  = 0;
  s_start_pos_ms = 0;
  s_pos_ms = 0;
  s_dur_ms = 0;
  s_last_result = 0;
  s_playing = false;          /* 由任务自己置 true，避免和旧任务收尾打架 */
  s_task_alive = true;        /* 先占位，防止两次 play 竞争 */

  pid = task_create("pcm_play", PLAYER_PRIO, PLAYER_STACK,
                    pcm_player_task, NULL);
  if (pid < 0)
    {
      syslog(LOG_ERR, "%s task_create failed: %d\n", TAG, errno);
      s_task_alive = false;
      s_playing = false;
      s_last_result = -1;
      return -errno;
    }

  s_playing = true;
  return 0;
}

void pcm_file_stop(void)
{
  s_paused     = false;
  s_pause_keep = false;

  if (s_playing || s_task_alive)
    {
      s_stop_req = true;
    }
}

int pcm_file_is_playing(void)
{
  return s_playing ? 1 : 0;
}

int pcm_file_position_ms(void)
{
  return s_pos_ms;
}

int pcm_file_duration_ms(void)
{
  return s_dur_ms;
}

int pcm_file_last_result(void)
{
  return s_last_result;
}

int pcm_file_set_volume(int percent)
{
  if (percent < 0)   percent = 0;
  if (percent > 100) percent = 100;

  /* ★ 这里**只能记值，绝对不能调 ioctl**。
   *
   * 这个函数是 UI 线程调的，而 s_cur_player 由播放任务在退出时清空并释放。
   * 跨线程读它会拿到悬空指针 —— 实测表现：
   *   [pcm_file] set volume 90% (hw 45) failed: 9   (EBADF)
   *   [pcm_file] set volume 80% (hw 36) failed: 9
   * 然后把垃圾 fd 传给 ioctl，音频通路被搞坏，pcm_play 随后卡死在信号量上
   * （ps 里 PID 228 pcm_play "Waiting Semaphore"，再没有新的 chunk 日志）。
   *
   * 播放任务每轮循环会对比 s_volume_pct 和 s_volume_applied，
   * 有变化就自己下发一次 —— 那里持有有效的 player 句柄，天然安全。 */
  s_volume_pct = percent;
  return 0;
}

int pcm_file_get_volume(void)
{
  return s_volume_pct;
}