/*
 * timesync_main.c - NSH builtin: sync the clock now, or show what it says.
 *
 * `timesync`         one SNTP attempt, then print the resulting local time
 * `timesync status`  print the clock without touching the network
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "infra/time_sync.h"

static void print_now(void)
{
  char buf[40];
  struct tm tmv;
  time_t now = time(NULL);

  localtime_r(&now, &tmv);
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
  printf("time: %s (%s)\n", buf,
         time_sync_is_synced() ? "synced from network" : "not synced yet");
}

int timesync_main(int argc, char *argv[])
{
  if (argc > 1 && strcmp(argv[1], "status") == 0)
    {
      print_now();
      return 0;
    }

  if (time_sync_once() != 0)
    {
      printf("timesync: all servers failed (see syslog for which step)\n");
      print_now();
      return 1;
    }

  print_now();
  return 0;
}
