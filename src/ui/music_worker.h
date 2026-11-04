/****************************************************************************
 * music_worker.h - 音乐操作的后台执行器
 *
 * 为什么必须单独一个任务：
 *   tool_music_*_execute() 内部是同步阻塞的 —— media_player_open 之后要
 *   等 PREPARED，start 之后要等 STARTED，每一步都有超时。如果直接在
 *   LVGL 的事件回调里调用，卡住的不是音乐，是**整个 UI 线程**：画面不再
 *   刷新，最后被看门狗复位（2026-10-21 真机就是这么冻死的）。
 *
 *   同理，music_status 要拿 g_media 锁；播放进行中锁被占着，1 秒一次的
 *   刷新定时器也会把 UI 堵死。
 *
 *   所以：UI 只投递命令 + 读一份快照，**永远不直接碰播放器**。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef AI_AGENT_MUSIC_WORKER_H
#define AI_AGENT_MUSIC_WORKER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
  MUSIC_CMD_PLAY = 0,   /* arg = 完整路径 */
  MUSIC_CMD_PAUSE,
  MUSIC_CMD_RESUME,
  MUSIC_CMD_STOP,
  MUSIC_CMD_SEEK,       /* arg = 目标毫秒（字符串）*/
  MUSIC_CMD_VOLUME      /* arg = 0~100（字符串）*/
} music_cmd_t;

typedef struct
{
  char state[16];       /* IDLE / PREPARED / PLAYING / PAUSED / ERROR */
  int  position_ms;
  int  duration_ms;     /* -1 表示未知 */
  int  volume;
  bool valid;           /* 还没取到过状态时为 false */
} music_snapshot_t;

/** 启动后台执行器（幂等）。返回 0 成功。 */
int music_worker_init(void);

/** 投递一条命令。**不阻塞**，投完立刻返回。队列满时返回负数。 */
int music_worker_post(music_cmd_t cmd, const char *arg);

/** 取一份状态快照（纯内存拷贝，不碰播放器、不拿播放器的锁）。 */
void music_worker_snapshot(music_snapshot_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_MUSIC_WORKER_H */