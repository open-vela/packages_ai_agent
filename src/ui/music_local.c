/****************************************************************************
 * music_local.c - 本地音乐（见 music_local.h 的说明）
 *
 * 注意：这里只负责「文件在不在、有哪些」，**不负责播放**。
 * 播放一律走 tools/tool_media.c 的 tool_music_play_execute()，它内部有
 * g_media 互斥锁和完整状态机；在这里另起一套播放器会破坏状态一致性。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include "music_local.h"
#include "assets/audio/demo_music.h"

#define TAG "[music_local]"

/* 演示曲是**原始 PCM**（16kHz/单声道/s16le），不是 MP3。
 *
 * 原因：本工作区的 media_player 是 src/voice/pcm_media_player.c 的兼容实现
 * （CONFIG_MEDIA 关闭），它只会把 PCM 写进 /dev/audio/pcm0p，
 * media_player_prepare() 里第一行就是 (void)url —— 根本不读文件、也不解码。
 * 所以放 MP3 只会"显示播放中但没有声音"。 */
#define DEMO_NAME  "demo1.pcm"
#define DEMO_PATH  MUSIC_LOCAL_DIR "/" DEMO_NAME

/* 只认 .pcm。MP3 在这个构建里放不出来（没有解码路径），
 * 列出来只会让用户点了发现没声音。 */
static int is_playable(const char *name)
{
    size_t n = strlen(name);

    if (n < 5)
      {
        return 0;
      }

    return (strcasecmp(name + n - 4, ".pcm") == 0);
}

const void *music_local_demo_data(void)
{
    return demo_music_data;
}

size_t music_local_demo_len(void)
{
    return demo_music_len;
}

int music_local_seed(void)
{
    struct stat st;
    int fd;
    size_t off;
    ssize_t wrote;

    if (mkdir(MUSIC_LOCAL_DIR, 0755) < 0 && errno != EEXIST)
      {
        syslog(LOG_WARNING, "%s mkdir %s failed: %d\n",
               TAG, MUSIC_LOCAL_DIR, errno);
        return -errno;
      }

    if (stat(DEMO_PATH, &st) == 0)
      {
        /* 已经有了，不重复写 —— littlefs 在 NOR 上，写一次要擦一次 */
        return 0;
      }

    syslog(LOG_INFO, "%s seeding %s (%u bytes)\n",
           TAG, DEMO_PATH, (unsigned)demo_music_len);

    fd = open(DEMO_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
      {
        syslog(LOG_WARNING, "%s open %s failed: %d\n",
               TAG, DEMO_PATH, errno);
        return -errno;
      }

    /* 分块写：一次性写 480KB 到 littlefs 上，单次 write 的返回值和
     * NOR 擦写时序都更难预测，分块能保证部分写也能继续推进。 */
    for (off = 0; off < demo_music_len; off += 4096)
      {
        size_t chunk = demo_music_len - off;

        if (chunk > 4096)
          {
            chunk = 4096;
          }

        wrote = write(fd, demo_music_data + off, chunk);
        if (wrote != (ssize_t)chunk)
          {
            syslog(LOG_ERR, "%s write %s failed at %u: %d\n",
                   TAG, DEMO_PATH, (unsigned)off, (int)wrote);
            close(fd);
            unlink(DEMO_PATH);
            return -EIO;
          }
      }

    close(fd);

    syslog(LOG_INFO, "%s seeded %s OK\n", TAG, DEMO_PATH);
    return 0;
}

int music_local_scan(char names[][MUSIC_LOCAL_NAMELEN], int max)
{
    FAR DIR *dir;
    FAR struct dirent *ent;
    int n = 0;

    if (names == NULL || max <= 0)
      {
        return 0;
      }

    dir = opendir(MUSIC_LOCAL_DIR);
    if (dir == NULL)
      {
        return 0;
      }

    while ((ent = readdir(dir)) != NULL && n < max)
      {
        if (!is_playable(ent->d_name))
          {
            continue;
          }

        snprintf(names[n], MUSIC_LOCAL_NAMELEN, "%s", ent->d_name);
        n++;
      }

    closedir(dir);
    return n;
}