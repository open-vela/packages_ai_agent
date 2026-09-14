/*
 * hr_cmd.c - NSH builtin: drive the simulated heart-rate monitor.
 *
 * `hr_set <bpm>`    set the simulated baseline (clamped 65-95), show it
 * `hr_set event`    inject a heart-rate spike event (rises ~30s, falls ~30s)
 * `hr_set`          show the current value and threshold
 *
 * Team 181 - Contest 2026
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hr_monitor.h"

int hr_set_main(int argc, char *argv[])
{
  if (argc < 2)
    {
      printf("hr: current %d bpm, threshold %d\n",
             hr_monitor_current(), HR_MONITOR_HIGH_DEFAULT);
      return 0;
    }

  if (strcmp(argv[1], "event") == 0)
    {
      hr_monitor_inject_event();
      printf("hr: event injected - value will climb past %d within %ds\n",
             HR_MONITOR_HIGH_DEFAULT, HR_MONITOR_EVENT_RISE_S);
      return 0;
    }

  {
    int bpm = atoi(argv[1]);
    if (bpm <= 0)
      {
        printf("hr_set: invalid bpm '%s'\n", argv[1]);
        return 1;
      }
    printf("hr: baseline %d bpm\n", hr_monitor_set(bpm));
    return 0;
  }
}
