/*
 * time_sync.c - SNTP time sync over whatever uplink is available.
 *
 * Why not apps/netutils/ntpclient: its daemon gates every attempt on
 * netlib_check_ipconnectivity(NULL, 1, 1), a single ICMP echo to the DNS
 * nameserver with a one-second deadline.  Over Bluetooth PAN that check fails
 * often enough that the daemon never collects a sample -- observed here as
 * three minutes of backoff on a link that was answering `ping -c 4 223.5.5.5`
 * 4/4 at the same moment.  It also compiles its diagnostics out by default, so
 * there is nothing in the log to explain the silence.
 *
 * This does the one thing needed: a single SNTP request per attempt, with a
 * syslog line for every outcome, and a retry policy we control.
 *
 * clock_settime(CLOCK_REALTIME) writes through to the hardware RTC via
 * up_rtc_settime() when CONFIG_RTC is set, so one success survives a reboot.
 */

#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "agent_config.h"
#include "infra/time_sync.h"

#define TAG "timesync"

/* Seconds between the NTP epoch (1900-01-01) and the Unix epoch. */
#define NTP_TO_UNIX_EPOCH 2208988800u

#define SNTP_PORT       123
#define SNTP_PKT_LEN    48
#define SNTP_RECV_SEC   4

/* A reply whose year is outside this range is a decoding error, not a clock:
 * the firmware cannot predate its own build and will not outlive the board. */
#define SANE_MIN_UNIX 1735689600L   /* 2025-01-01 */
#define SANE_MAX_UNIX 4102444800L   /* 2100-01-01 */

/* Alibaba first: 223.5.5.5 (their resolver) is the one reliably reachable
 * over this link, so their NTP hosts are the best bet.  Kept as literal IPs as
 * well so a DNS outage does not block the sync. */
static const char *const g_servers[] =
{
  "ntp.aliyun.com",
  "203.107.6.88",       /* ntp.aliyun.com, in case DNS is down */
  "cn.pool.ntp.org",
};

#define NSERVERS (int)(sizeof(g_servers) / sizeof(g_servers[0]))

static pthread_t   g_thread;
static bool        g_running;
static bool        g_synced;
static time_t      g_last_sync;

/* One SNTP exchange.  Returns 0 and sets *out on success. */
static int sntp_query(const char *host, time_t *out)
{
  uint8_t pkt[SNTP_PKT_LEN];
  struct addrinfo hints;
  struct addrinfo *res = NULL;
  struct timeval tv;
  uint32_t secs;
  int sd = -1;
  int ret = -1;
  ssize_t n;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;

  if (getaddrinfo(host, "123", &hints, &res) != 0 || res == NULL)
    {
      syslog(LOG_WARNING, "[%s] resolve %s failed\n", TAG, host);
      return -1;
    }

  sd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sd < 0)
    {
      syslog(LOG_ERR, "[%s] socket: %d\n", TAG, errno);
      goto done;
    }

  tv.tv_sec = SNTP_RECV_SEC;
  tv.tv_usec = 0;
  setsockopt(sd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  /* LI=0 VN=4 Mode=3 (client); everything else zero is a valid request. */

  memset(pkt, 0, sizeof(pkt));
  pkt[0] = 0x23;

  if (sendto(sd, pkt, sizeof(pkt), 0, res->ai_addr, res->ai_addrlen) < 0)
    {
      syslog(LOG_WARNING, "[%s] send to %s: %d\n", TAG, host, errno);
      goto done;
    }

  n = recv(sd, pkt, sizeof(pkt), 0);
  if (n < SNTP_PKT_LEN)
    {
      syslog(LOG_WARNING, "[%s] no reply from %s (ret=%zd errno=%d)\n",
             TAG, host, n, errno);
      goto done;
    }

  /* Mode must be 4 (server) or 5 (broadcast). */

  if ((pkt[0] & 0x07) != 4 && (pkt[0] & 0x07) != 5)
    {
      syslog(LOG_WARNING, "[%s] %s: bad mode 0x%02x\n", TAG, host, pkt[0]);
      goto done;
    }

  /* Transmit timestamp, seconds part, at offset 40. */

  secs = ((uint32_t)pkt[40] << 24) | ((uint32_t)pkt[41] << 16) |
         ((uint32_t)pkt[42] << 8)  | (uint32_t)pkt[43];
  if (secs == 0)
    {
      syslog(LOG_WARNING, "[%s] %s: zero timestamp\n", TAG, host);
      goto done;
    }

  *out = (time_t)(secs - NTP_TO_UNIX_EPOCH);
  if (*out < SANE_MIN_UNIX || *out > SANE_MAX_UNIX)
    {
      syslog(LOG_WARNING, "[%s] %s: implausible time %ld\n",
             TAG, host, (long)*out);
      goto done;
    }

  ret = 0;

done:
  if (sd >= 0)
    {
      close(sd);
    }
  freeaddrinfo(res);
  return ret;
}

int time_sync_once(void)
{
  struct timespec ts;
  struct tm tmv;
  char buf[32];
  time_t now = 0;
  int i;

  for (i = 0; i < NSERVERS; i++)
    {
      if (sntp_query(g_servers[i], &now) != 0)
        {
          continue;
        }

      /* Store local time, not UTC: CONFIG_LIBC_LOCALTIME is off, so
       * localtime_r() is gmtime() and TZ is ignored.  See the note next to
       * AGENT_UTC_OFFSET_SEC in agent_config.h. */

      now += AGENT_UTC_OFFSET_SEC;

      ts.tv_sec = now;
      ts.tv_nsec = 0;
      if (clock_settime(CLOCK_REALTIME, &ts) != 0)
        {
          syslog(LOG_ERR, "[%s] clock_settime: %d\n", TAG, errno);
          return -1;
        }

      g_synced = true;
      g_last_sync = now;

      localtime_r(&now, &tmv);
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
      syslog(LOG_INFO, "[%s] clock set from %s: %s (UTC%+d)\n",
             TAG, g_servers[i], buf, AGENT_UTC_OFFSET_SEC / 3600);
      return 0;
    }

  syslog(LOG_WARNING, "[%s] all %d servers failed\n", TAG, NSERVERS);
  return -1;
}

bool time_sync_is_synced(void)
{
  return g_synced;
}

/* Acquire, then keep it honest.  Backoff caps at 60 s so a link that comes
 * good is picked up quickly; after success we only re-check every 6 h. */
static void *time_sync_task(void *arg)
{
  int delay = 5;

  (void)arg;

  for (; ; )
    {
      if (time_sync_once() == 0)
        {
          delay = 5;
          sleep(6 * 60 * 60);
          continue;
        }

      sleep(delay);
      delay *= 2;
      if (delay > 60)
        {
          delay = 60;
        }
    }

  return NULL;
}

int time_sync_start(void)
{
  pthread_attr_t attr;
  struct sched_param sp;
  int ret;

  if (g_running)
    {
      return 0;
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 4096);
  sp.sched_priority = 50;
  pthread_attr_setschedparam(&attr, &sp);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  ret = pthread_create(&g_thread, &attr, time_sync_task, NULL);
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      syslog(LOG_ERR, "[%s] thread create failed: %d\n", TAG, ret);
      return -1;
    }

  g_running = true;
  syslog(LOG_INFO, "[%s] started\n", TAG);
  return 0;
}
