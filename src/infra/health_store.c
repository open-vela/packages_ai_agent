/****************************************************************************
 * health_store.c - 健康数据小文件持久化
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include "infra/health_store.h"

#define TAG "health_store"
#define HEALTH_DIR "/data/ai_agent/health"

static bool s_inited;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static void path_of(const char *name, char *out, size_t cap, bool tmp)
{
  snprintf(out, cap, HEALTH_DIR "/%s.json%s", name, tmp ? ".tmp" : "");
}

int health_store_init(void)
{
  pthread_mutex_lock(&s_lock);
  if (!s_inited)
    {
      (void)mkdir("/data/ai_agent", 0777);
      if (mkdir(HEALTH_DIR, 0777) != 0 && errno != EEXIST)
        {
          syslog(LOG_WARNING, "[%s] mkdir %s failed: %d\n", TAG, HEALTH_DIR,
                 errno);
        }
      else
        {
          s_inited = true;
          syslog(LOG_INFO, "[%s] ready at %s\n", TAG, HEALTH_DIR);
        }
    }
  pthread_mutex_unlock(&s_lock);
  return s_inited ? 0 : -errno;
}

int health_store_load(const char *name, char *buf, size_t cap)
{
  char path[96];
  int fd;
  int n;

  if (name == NULL || buf == NULL || cap == 0)
    {
      return -EINVAL;
    }

  health_store_init();
  path_of(name, path, sizeof(path), false);

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return -errno;
    }

  n = (int)read(fd, buf, cap - 1);
  close(fd);

  if (n < 0)
    {
      return -errno;
    }

  buf[n] = '\0';
  return n;
}

int health_store_save(const char *name, const char *json)
{
  char path[96];
  char tmp[100];
  int fd;
  int len;
  int ret = 0;

  if (name == NULL || json == NULL)
    {
      return -EINVAL;
    }

  health_store_init();
  path_of(name, path, sizeof(path), false);
  path_of(name, tmp, sizeof(tmp), true);

  len = (int)strlen(json);
  fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
    {
      syslog(LOG_WARNING, "[%s] open %s failed: %d\n", TAG, tmp, errno);
      return -errno;
    }

  if (write(fd, json, (size_t)len) != len)
    {
      ret = -errno;
      syslog(LOG_WARNING, "[%s] write %s failed: %d\n", TAG, tmp, errno);
    }

  close(fd);

  if (ret == 0 && rename(tmp, path) != 0)
    {
      ret = -errno;
      syslog(LOG_WARNING, "[%s] rename %s -> %s failed: %d\n", TAG, tmp, path,
             errno);
    }

  return ret;
}
