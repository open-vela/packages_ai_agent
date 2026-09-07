/*
 * time_sync.h - SNTP time sync over the active uplink.
 *
 * The board has no trusted clock source at first boot, so the RTC starts at an
 * arbitrary date and both `date` and the model's time answers are wrong until
 * this lands.  A successful sync writes through to the hardware RTC, so it
 * survives a reboot.
 */

#ifndef AI_AGENT_TIME_SYNC_H
#define AI_AGENT_TIME_SYNC_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the background sync thread: retries with backoff until the first
 * success, then re-checks every 6 hours.  Idempotent.
 *
 * @return 0 on success, -1 if the thread could not be created.
 */
int time_sync_start(void);

/**
 * One synchronous SNTP attempt across the built-in server list.
 * Blocks for up to a few seconds per server.  Also reachable from NSH as
 * `timesync`.
 *
 * @return 0 if the clock was set, -1 if every server failed.
 */
int time_sync_once(void);

/**
 * Whether the clock has been set from the network since boot.
 */
bool time_sync_is_synced(void);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_TIME_SYNC_H */
