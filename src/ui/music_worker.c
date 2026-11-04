/****************************************************************************
 * music_worker.c - 音乐操作的后台执行器（见 music_worker.h）
 *
 * 结构：一个任务 + 一个深度很小的命令队列。
 *   - UI 线程调 music_worker_post()：拿互斥锁、入队、给信号量，立刻返回。
 *   - 工作线程：等信号量、出队、执行真正的 tool_music_*_execute()。
 *   - 工作线程每轮顺带取一次播放状态，存进 g_snap；UI 只读这个快照。
 *
 * 这样即使播放器卡住 30 秒，UI 也只会在下一个命令投递时短暂等一个
 * 很短的互斥锁，画面继续刷新。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <errno.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <sched.h>
#include <semaphore.h>
#include <stdlib.h>      /* atoi: 音量字符串转数值 */
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include "music_worker.h"
#include "music_local.h"          /* MUSIC_LOCAL_DEMO_PATH / demo data */
#include "voice/pcm_file_player.h"
#include "tools/tool_media.h"
#include "cJSON.h"

#define TAG "[music_worker]"

#define QUEUE_LEN     8
#define WORKER_STACK  8192
#define WORKER_PRIO   90

typedef struct
{
  music_cmd_t cmd;
  char        arg[96];
} music_job_t;

/****************************************************************************
 * 共享状态
 ****************************************************************************/

static music_job_t    g_queue[QUEUE_LEN];
static int            g_head;
static int            g_tail;
static int            g_count;

static mutex_t        g_lock;          /* 保护队列 + 快照 */
static sem_t          g_wake;          /* 有新命令时唤醒工作线程 */
static bool           g_started;

static music_snapshot_t g_snap;

/****************************************************************************
 * 状态快照
 ****************************************************************************/

/* 只在工作线程里调用（它已经串行化了所有播放器操作）。 */
static void snap_refresh(void)
{
  char buf[512];
  cJSON *root;
  cJSON *item;
  music_snapshot_t s;

  memset(&s, 0, sizeof(s));
  buf[0] = '\0';

  /* ★ 本地 PCM 走的是 pcm_file_player，tool_music_status 根本不知道它在播。
   * 所以这里先问 PCM 播放器；它没在播才去问通用工具层。 */
  if (pcm_file_is_playing() || pcm_file_is_paused() ||
      pcm_file_duration_ms() > 0)
    {
      s.valid  = true;
      s.volume = pcm_file_get_volume();   /* 读真实音量，不写死 */

      if (pcm_file_is_paused())
        {
          /* 暂停中：任务还活着、位置保留，只是 DMA 停了 */
          s.position_ms = pcm_file_position_ms();
          s.duration_ms = pcm_file_duration_ms();
          snprintf(s.state, sizeof(s.state), "PAUSED");
        }
      else if (pcm_file_is_playing())
        {
          s.position_ms = pcm_file_position_ms();
          s.duration_ms = pcm_file_duration_ms();
          snprintf(s.state, sizeof(s.state), "PLAYING");
        }
      else
        {
          /* 播放已经结束。
           * 以前这里报 "PAUSED"，而 s_dur_ms 又永不清零，于是界面永远卡在
           * "已暂停 + 末尾位置"，点播放键发 RESUME 对本地 PCM 完全无效 ——
           * 歌放完之后播放键就永久失效了。
           * 现在如实回到 IDLE（未播放），duration 归零，播放键恢复可用。 */
          s.position_ms = 0;
          s.duration_ms = -1;
          snprintf(s.state, sizeof(s.state), "IDLE");
        }

      nxmutex_lock(&g_lock);
      g_snap = s;
      nxmutex_unlock(&g_lock);
      return;
    }

  if (tool_music_status_execute(NULL, buf, sizeof(buf)) != 0 || buf[0] == '\0')
    {
      return;               /* 取不到就保持上一次快照，不要清零 */
    }

  root = cJSON_Parse(buf);
  if (root == NULL)
    {
      return;
    }

  item = cJSON_GetObjectItem(root, "state");
  if (item != NULL && cJSON_IsString(item))
    {
      snprintf(s.state, sizeof(s.state), "%s", item->valuestring);
      s.valid = true;
    }

  item = cJSON_GetObjectItem(root, "position_ms");
  if (item != NULL && cJSON_IsNumber(item))
    {
      s.position_ms = item->valueint;
    }

  item = cJSON_GetObjectItem(root, "duration_ms");
  if (item != NULL && cJSON_IsNumber(item))
    {
      s.duration_ms = item->valueint;
    }
  else
    {
      s.duration_ms = -1;
    }

  item = cJSON_GetObjectItem(root, "volume");
  if (item != NULL && cJSON_IsNumber(item))
    {
      s.volume = item->valueint;
    }

  cJSON_Delete(root);

  if (s.valid)
    {
      nxmutex_lock(&g_lock);
      g_snap = s;
      nxmutex_unlock(&g_lock);
    }
}

/****************************************************************************
 * 命令执行
 ****************************************************************************/

static void run_job(const music_job_t *job)
{
  char out[128];

  out[0] = '\0';

  switch (job->cmd)
    {
      case MUSIC_CMD_PLAY:
        /* tool_music_play_execute 在这台设备上只会"成功返回但什么都不做"，
         * 真正出声的只有下面两条路。 */
        if (strcmp(job->arg, MUSIC_LOCAL_DEMO_PATH) == 0)
          {
            /* 内置演示曲：直接从固件里读，不经过文件系统。
             * 走 /data 会读失败（实测 1.5~2.4 秒处 read 返回 -1）。 */
            (void)pcm_mem_play(music_local_demo_data(),
                               music_local_demo_len(), "演示曲");
          }
        else
          {
            (void)pcm_file_play(job->arg);
          }
        break;

      case MUSIC_CMD_PAUSE:
        /* 真正的暂停：播放任务自己停掉 DMA，位置和句柄都保留，
         * 恢复时从原位置接着放，不是从头开始。 */
        pcm_file_pause();
        break;

      case MUSIC_CMD_RESUME:
        pcm_file_resume();
        break;

      case MUSIC_CMD_STOP:
        pcm_file_stop();
        (void)tool_music_stop_execute(NULL, out, sizeof(out));
        break;

      case MUSIC_CMD_SEEK:
        {
          char arg[160];

          snprintf(arg, sizeof(arg), "{\"position_ms\":%s}", job->arg);
          (void)tool_music_seek_execute(arg, out, sizeof(out));
        }
        break;

      case MUSIC_CMD_VOLUME:
        {
          /* ★ 必须走 pcm_file_set_volume，不能走 tool_music_set_volume。
           *
           * tool_music_set_volume 最终调用 media_policy_set_stream_volume()，
           * 而后者在本工作区是一个空桩（pcm_media_player.c:523，
           * 内容是 (void)stream; (void)volume; return 0）。
           * 也就是说旧路径"返回成功但什么都没做" —— 界面音量会变，声音不变。
           *
           * pcm_file_set_volume 直接对 /dev/audio/pcm0p 发
           * AUDIO_TYPE_FEATURE / AUDIO_FU_VOLUME，真的会改 DAC 音量寄存器。 */
          (void)pcm_file_set_volume(atoi(job->arg));
        }
        break;

      default:
        break;
    }
}

/****************************************************************************
 * 工作线程
 ****************************************************************************/

static int music_worker_thread(int argc, FAR char *argv[])
{
  music_job_t job;

  (void)argc;
  (void)argv;

  for (;;)
    {
      /* 等命令；超时 1 秒时借机刷一次状态快照。
       * 这样即使没有操作，进度条也能动起来。 */
      /* ★ 单位是 **tick**，不是毫秒/微秒。
       * 本构建 CONFIG_USEC_PER_TICK=10000，即 1 tick = 10ms，
       * 所以 1 秒 = 100 tick。原来写的 1000000 是 10000 秒（2.8 小时）——
       * 表现就是"播放 30 秒期间进度条一次都不动"。 */
      if (nxsem_tickwait(&g_wake, 100) == OK)
        {
          bool have = false;

          nxmutex_lock(&g_lock);
          if (g_count > 0)
            {
              job = g_queue[g_head];
              g_head = (g_head + 1) % QUEUE_LEN;
              g_count--;
              have = true;
            }
          nxmutex_unlock(&g_lock);

          if (have)
            {
              run_job(&job);
            }
        }

      snap_refresh();
    }

  return 0;
}

/****************************************************************************
 * 公开接口
 ****************************************************************************/

int music_worker_init(void)
{
  int pid;

  if (g_started)
    {
      return 0;
    }

  nxmutex_init(&g_lock);
  sem_init(&g_wake, 0, 0);

  g_head = 0;
  g_tail = 0;
  g_count = 0;
  memset(&g_snap, 0, sizeof(g_snap));
  g_snap.duration_ms = -1;

  pid = task_create("music_wrk", WORKER_PRIO, WORKER_STACK,
                    music_worker_thread, NULL);
  if (pid < 0)
    {
      syslog(LOG_ERR, "%s task_create failed: %d\n", TAG, errno);
      return -errno;
    }

  g_started = true;
  syslog(LOG_INFO, "%s started (pid %d)\n", TAG, pid);
  return 0;
}

int music_worker_post(music_cmd_t cmd, const char *arg)
{
  int ret = 0;

  if (!g_started)
    {
      (void)music_worker_init();
    }

  nxmutex_lock(&g_lock);

  if (g_count >= QUEUE_LEN)
    {
      ret = -EAGAIN;
    }
  else
    {
      g_queue[g_tail].cmd = cmd;
      snprintf(g_queue[g_tail].arg, sizeof(g_queue[g_tail].arg), "%s",
               arg != NULL ? arg : "");
      g_tail = (g_tail + 1) % QUEUE_LEN;
      g_count++;
    }

  nxmutex_unlock(&g_lock);

  if (ret == 0)
    {
      sem_post(&g_wake);
    }

  return ret;
}

void music_worker_snapshot(music_snapshot_t *out)
{
  if (out == NULL)
    {
      return;
    }

  if (!g_started)
    {
      memset(out, 0, sizeof(*out));
      out->duration_ms = -1;
      return;
    }

  nxmutex_lock(&g_lock);
  *out = g_snap;
  nxmutex_unlock(&g_lock);
}