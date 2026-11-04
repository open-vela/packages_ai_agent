/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * This file contains code derived from MimiClaw (https://github.com/memovai/mimiclaw)
 * Copyright (c) 2026 Ziboyan Wang, licensed under the MIT License.
 * See NOTICE file for the original MIT License terms.
 */

#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <unistd.h>

#include "agent_compat.h"
#include "servo.h"
#include "voice/audio_playback.h"
#include "agent_config.h"

#include "core/agent_loop.h"
#include "core/message_bus.h"
#include "core/message_bus_tap.h"
#include "channels/nsh_commands.h"
#include "infra/config_store.h"
#include "infra/cron_service.h"
#ifdef CONFIG_AI_AGENT_FEISHU
#include "channels/feishu_bot.h"
#endif
#include "channels/ws_server.h"
#include "infra/heartbeat.h"
#include "llm/llm_proxy.h"
#include "llm/llm_router.h"
#include "core/memory_store.h"
#include "core/session_mgr.h"
#ifdef CONFIG_AI_AGENT_MQTT
#include "channels/mqtt_channel.h"
#endif
#include "infra/network_manager.h"
#ifdef CONFIG_AI_AGENT_NODE
#include "node/node_client.h"
#include "node/node_manager.h"
#endif
#include "infra/http_proxy.h"
#include "tools/tool_guard.h"
#include "tools/skill_loader.h"
#include "tools/tool_media.h"
#include "tools/tool_registry.h"
#ifdef CONFIG_AI_AGENT_MCP
#include "tools/mcp_bridge.h"
#endif
#include "llm/llm_cache.h"
#include "infra/vela_tls.h"
#if AGENT_SKILL_SYNC_ENABLED
#include "tools/skill_sync.h"
#endif
#include "voice/voice_channel.h"
#ifdef CONFIG_AI_AGENT_WEIXIN
#include "channels/weixin_channel.h"
#endif
#ifdef CONFIG_AI_AGENT_LVGL_UI
#include "ui/lvgl_ui_channel.h"
#endif
#ifdef CONFIG_AI_AGENT_BLE_GATT
#include "infra/ble_cmd_handler.h"
#include "infra/ble_gatt.h"
#include <bluetooth.h>
#include <bt_adapter.h>
#endif

static const char* TAG = "agent";

/* ── stdout mutex — shared with nsh_commands.c ──────────────── */
/* Prevents concurrent printf from outbound_dispatch_task and cli_thread
 * which causes adbd shell_service_uv assert (wait_ack != 0). */
pthread_mutex_t g_stdout_lock = PTHREAD_MUTEX_INITIALIZER;

/* ── Global shutdown flag ─────────────────────────────────────── */
/* Set by cmd_quit via agent_request_shutdown(); checked by main loop
 * to trigger graceful teardown instead of calling exit(). */
static volatile bool g_shutdown_requested = false;

void agent_request_shutdown(void)
{
    g_shutdown_requested = true;
}

bool agent_shutdown_requested(void)
{
    return g_shutdown_requested;
}

/* [V510] 开机问候（异步）。
 *
 * 原来这里是三次同步重试：
 *
 *     for (int i = 0; i < 3; i++) {
 *       ret = voice_channel_speak("桌面小跟班语音就绪");
 *       if (ret == 0) break;
 *       sleep(3);
 *     }
 *
 * 而 voice_channel_speak() 是**同步阻塞**的：它把整段合成流式取回并驱动
 * 播放，走完才返回。上面那段注释（v73/v77）早就记了这个坑——"greeting first
 * 会让板子变哑"，因为它挡住了后面的 voice_channel_listen_start()。
 *
 * 以前 MiMo 流式路径从未真正执行过（speak_stream 优先连那个不存在的自建
 * 端点，每次秒失败），所以这个隐患一直没爆。v509 把 MiMo 打通后，它第一次
 * 真跑：192KB malloc + 完整 TLS + 阻塞播放，还重试三次，全卡在启动主路径上，
 * 板子直接进不了 shell。
 *
 * 改为：独立低优先级线程，延迟几秒，只播一次，失败就记日志放弃。
 * 问候本来就是装饰性的，聆听才是功能。
 */
static void *boot_greeting_task(void *arg)
{
  /* [V876] This is the last ai_agent path that still opened audio:
   * voice_channel_speak() -> /dev/audio/pcm0p.  aivox already owns pcm0p,
   * so this collided with it (same EXCCAUSE=001c panic as H1).
   *
   * The greeting is decorative -- listening is the feature -- and there is
   * only meant to be one voice assistant.  Bail out before touching any
   * audio device.  Delete these two lines to bring the greeting back. */
  (void)arg;
  return NULL;
  const int delay_s = 12;

  syslog(LOG_INFO, "[greeting] waiting %d s before speaking\n", delay_s);
  sleep(delay_s);

  syslog(LOG_INFO, "[greeting] speaking (one attempt, async)\n");
  if (voice_channel_speak("桌面小跟班语音就绪") == 0)
    {
      syslog(LOG_INFO, "[greeting] done\n");
    }
  else
    {
      syslog(LOG_WARNING,
             "[greeting] failed, ignored - listening is unaffected\n");
    }

  return NULL;
}


/* ── Network watcher (async) ──────────────────────────────────── */

#ifdef CONFIG_AI_AGENT_NET_RPMSG
static volatile bool g_net_services_started = false;

static void net_state_change_cb(net_state_t state, void* arg)
{
    (void)arg;
    if (state == NET_STATE_CONNECTED && !g_net_services_started) {
        syslog(LOG_INFO, "[%s] Network recovered, starting services\n", TAG);
#ifdef CONFIG_AI_AGENT_FEISHU
        feishu_bot_start();
#endif
        agent_loop_start();
        ws_server_start();
#ifdef CONFIG_AI_AGENT_NODE
        node_client_start();
#endif
#ifdef CONFIG_AI_AGENT_MQTT
        mqtt_channel_start();
#endif
#ifdef CONFIG_AI_AGENT_WEIXIN
        weixin_channel_start();
#endif
        g_net_services_started = true;
    } else if (state == NET_STATE_DISCONNECTED) {
        syslog(LOG_WARNING,
            "[%s] Network lost, services may be affected\n", TAG);
        /* Services will handle disconnection via their own error paths.
         * We just log the event here. g_net_services_started stays true
         * so we don't re-start services that are already running. */
    }
}
#endif

/**
 * Standalone live publisher: refreshes /tmp/net.txt every 5s with the
 * last netinit association diagnostic from kmsg. Runs in its own
 * thread, independent of network manager locks.
 */
static void* net_publish_task(void* arg)
{
    (void)arg;
    static char kbuf[4096];
    static char tmp[512];

    for (;;)
      {
        size_t kn = 0;
        ssize_t n;
        char diag[48] = "";
        int kfd = open("/dev/ramlog", O_RDONLY | O_NONBLOCK);

        if (kfd < 0)
          {
            kfd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
          }

        if (kfd >= 0)
          {
            while ((n = read(kfd, tmp, sizeof(tmp))) > 0)
              {
                if (kn + n > sizeof(kbuf) - 1)
                  {
                    size_t drop = kn + n - (sizeof(kbuf) - 1);
                    memmove(kbuf, kbuf + drop, kn - drop);
                    kn -= drop;
                  }
                memcpy(kbuf + kn, tmp, n);
                kn += n;
              }
            close(kfd);
          }

        if (kn > 0)
          {
            char *p = kbuf;
            char *last = NULL;
            kbuf[kn] = '\0';
            while ((p = strstr(p, "netinit_associate ret=")) != NULL)
              {
                last = p;
                p += 8;
              }
            if (last != NULL)
              {
                int retv = 0;
                int errv = 0;
                if (sscanf(last, "netinit_associate ret=%d errno=%d",
                           &retv, &errv) == 2)
                  {
                    snprintf(diag, sizeof(diag), " a%d e%d", retv, errv);
                  }
              }
          }

        /* net.txt is owned by the netinit diag thread now */

        sleep(5);
      }

    return NULL;
}

/**
 * Runs in a background thread: waits for network, then starts all
 * network-dependent services. Main thread is NOT blocked.
 */
static void* network_watch_task(void* arg)
{
    (void)arg;
    syslog(LOG_INFO, "[%s] Network watcher started\n", TAG);

    /* Kick off the independent diagnostics publisher first */
    {
        pthread_t pub_tid;
        if (pthread_create(&pub_tid, NULL, net_publish_task, NULL) == 0)
          {
            pthread_detach(pub_tid);
          }
    }

    /* [WIFI-FIX-R26] The NuttX-native netinit owns the association here.
     *
     * This call made the agent run its own wapi/ifup association attempt from
     * a background thread while netinit was doing the same thing, and it
     * also rewrote /tmp/net.txt in a different format than the panel's
     * diagnostic.  Measured: with the agent stopped the station associates
     * every time (reason=0), with it running it never does.  Just wait for
     * the link netinit brings up. */

    /* [V1257] Re-enabled as the boot fallback.  network_wifi_reconnect()
     * first waits up to 8 s for netinit's link and returns without
     * touching the interface when one is already up, and it bails out
     * harmlessly when config_store holds no saved credentials - so the
     * R26 race (an unconditional second association fighting netinit) no
     * longer applies.  The manual wapi path only fires when netinit
     * failed to bring the link up on its own. */

    network_wifi_reconnect();

    /* Publish the network state for the info page (/tmp is tmpfs). */
    {
        FILE *nf = fopen("/tmp/net.txt", "w");
        if (nf) { fputs("NO NET", nf); fclose(nf); }
    }

    if (network_wait_connected(30000) == OK) {
        syslog(LOG_INFO, "[%s] Network connected: %s\n", TAG, network_get_ip());
        {
            FILE *nf = fopen("/tmp/net.txt", "w");
            if (nf) { fprintf(nf, "%s", network_get_ip()); fclose(nf); }
        }

#if AGENT_SKILL_SYNC_ENABLED
        /* Sync skills from Bitable before starting agent loop */
        if (skill_sync_from_bitable() != OK) {
            syslog(LOG_WARNING, "[%s] Bitable skill sync failed, using local skills\n", TAG);
        }
#endif

#ifdef CONFIG_AI_AGENT_FEISHU
        if (feishu_bot_start() != OK)
            syslog(LOG_WARNING, "[%s] feishu_bot_start failed\n", TAG);
#endif
        if (agent_loop_start() != OK)
            syslog(LOG_WARNING, "[%s] agent_loop_start failed\n", TAG);
        if (ws_server_start() != OK)
            syslog(LOG_WARNING, "[%s] ws_server_start failed\n", TAG);
#ifdef CONFIG_AI_AGENT_NODE
        if (node_client_start() != OK)
            syslog(LOG_WARNING, "[%s] node_client_start failed\n", TAG);
#endif
#ifdef CONFIG_AI_AGENT_MQTT
        if (mqtt_channel_start() != OK)
            syslog(LOG_WARNING, "[%s] mqtt_channel_start failed\n", TAG);
#endif
#ifdef CONFIG_AI_AGENT_WEIXIN
        if (weixin_channel_start() != OK)
            syslog(LOG_WARNING, "[%s] weixin_channel_start failed\n", TAG);
#endif

        syslog(LOG_INFO, "[%s] All network services started!\n", TAG);

#ifdef CONFIG_AI_AGENT_NET_RPMSG
        g_net_services_started = true;
#endif
    } else {
        syslog(LOG_WARNING, "[%s] Network timeout - net services not started.\n", TAG);

        /* Live net-state publisher: refresh /tmp/net.txt every 5s so the
         * info page (30s staleness limit) keeps showing the state, and
         * surface the last netinit association diagnostic from kmsg.
         * Also snapshot kmsg to /tmp/kmsg.txt for console-less debug. */
        {
            static char kbuf[4096];   /* too big for thread stack */
            static char tmp[512];
            size_t kn = 0;
            int retry_ticks = 0;

            while (1) {
                char diag[48] = "";
                size_t n;
                int kfd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
                if (kfd >= 0) {
                    kn = 0;
                    while ((n = read(kfd, tmp, sizeof(tmp))) > 0) {
                        if (kn + n > sizeof(kbuf) - 1) {
                            size_t drop = kn + n - (sizeof(kbuf) - 1);
                            memmove(kbuf, kbuf + drop, kn - drop);
                            kn -= drop;
                        }
                        memcpy(kbuf + kn, tmp, n);
                        kn += n;
                    }
                    close(kfd);
                }

                if (kn > 0) {
                    char *p = kbuf;
                    char *last = NULL;
                    kbuf[kn] = '\0';
                    while ((p = strstr(p, "netinit_associate ret=")) != NULL) {
                        last = p;
                        p += 8;
                    }
                    if (last != NULL) {
                        int retv = 0;
                        int errv = 0;
                        if (sscanf(last, "netinit_associate ret=%d errno=%d",
                                   &retv, &errv) == 2) {
                            snprintf(diag, sizeof(diag), " a%d e%d",
                                     retv, errv);
                        }
                    }
                }

                /* [UI-FIX-R35] Not /tmp/net.txt.
                 *
                 * That file is the panel's contract - the address, or
                 * "NO NET" - and netinit owns it.  This association counter
                 * is what the panel ended up showing instead ("a0"/"L0"), so
                 * it goes to a file of its own. */

                FILE *nf = fopen("/tmp/netdiag.txt", "w");
                if (nf) {
                    if (diag[0] != '\0') {
                        fprintf(nf, "%s", (diag[0] == 32) ? diag + 1 : diag);
                    } else {
                        fputs("WAIT", nf);
                    }
                    fclose(nf);
                }

                if (kn > 0) {
                    FILE *kf2 = fopen("/tmp/kmsg.txt", "w");
                    if (kf2) {
                        fwrite(kbuf, 1, kn, kf2);
                        fclose(kf2);
                    }
                }

                /* [WIFI-FIX-R26] Do not re-drive wlan0 from here: netinit
                 * owns the association on this board. */

                if (++retry_ticks >= 12) {
                    retry_ticks = 0;
                    syslog(LOG_INFO,
                           "[%s] waiting for netinit's association\n", TAG);
                }

                sleep(5);
            }
        }
    }

#ifdef CONFIG_AI_AGENT_NET_RPMSG
    /* Register listener for network state changes — auto-restart services on reconnect */
    network_register_listener(net_state_change_cb, NULL);

    /* Keep thread alive to handle reconnection events */
    while (!g_shutdown_requested) {
        sleep(1);
    }
#endif

    return NULL;
}

#ifdef CONFIG_FEATURE_SYSTEM_VELACLAW
/* -- Quickapp mqueue IPC (cross-process bridge) --------------- */

#include <mqueue.h>
#include <fcntl.h>
#include <errno.h>

#define VELACLAW_MQ_QAPP_IN   "/velaclaw_qapp_in"
#define VELACLAW_MQ_QAPP_OUT  "/velaclaw_qapp_out"
#define VELACLAW_MQ_MSG_SIZE  4096
#define VELACLAW_MQ_MAX_MSGS  4

/**
 * Reads requests from the quickapp process via POSIX mqueue and feeds
 * them into the local message_bus inbound queue.
 */
static void *quickapp_mq_listener_task(void *arg)
{
    (void)arg;
    syslog(LOG_INFO, "[%s] Quickapp mqueue listener started\n", TAG);

    struct mq_attr attr = {
        .mq_maxmsg  = VELACLAW_MQ_MAX_MSGS,
        .mq_msgsize = VELACLAW_MQ_MSG_SIZE,
    };
    mqd_t mq = mq_open(VELACLAW_MQ_QAPP_IN, O_RDONLY | O_CREAT, 0666, &attr);
    if (mq == (mqd_t)-1) {
        syslog(LOG_ERR, "[%s] Failed to open quickapp inbound mqueue: %d\n", TAG, errno);
        return NULL;
    }

    char *buf = (char *)malloc(VELACLAW_MQ_MSG_SIZE);
    if (!buf) {
        syslog(LOG_ERR, "[%s] Failed to allocate mqueue recv buffer\n", TAG);
        mq_close(mq);
        return NULL;
    }

    while (1) {
        ssize_t n = mq_receive(mq, buf, VELACLAW_MQ_MSG_SIZE, NULL);
        if (n < 0) {
            if (errno == EINTR) continue;
            syslog(LOG_ERR, "[%s] mq_receive failed: %d\n", TAG, errno);
            break;
        }
        if (n >= (ssize_t)VELACLAW_MQ_MSG_SIZE) {
            n = VELACLAW_MQ_MSG_SIZE - 1;
        }
        buf[n] = '\0';

        /* Message format: "chat_id\nquery" */
        char *sep = strchr(buf, '\n');
        if (!sep) {
            syslog(LOG_WARNING, "[%s] Malformed quickapp message\n", TAG);
            continue;
        }
        *sep = '\0';
        const char *chat_id = buf;
        const char *query = sep + 1;

        syslog(LOG_INFO, "[%s] Quickapp request: chat_id=%s query=%.40s\n",
               TAG, chat_id, query);

        agent_msg_t msg = {0};
        strncpy(msg.channel, AGENT_CHAN_QUICKAPP, sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, chat_id, sizeof(msg.chat_id) - 1);
        msg.content = strdup(query);
        if (msg.content) {
            message_bus_push_inbound(&msg);
        }
    }

    free(buf);
    mq_close(mq);
    return NULL;
}

/**
 * Send a reply to the quickapp process via the outbound mqueue.
 * Called from the outbound dispatch thread in the agent process.
 */
static void quickapp_mq_dispatch(const char *chat_id, const char *content)
{
    struct mq_attr attr = {
        .mq_maxmsg  = VELACLAW_MQ_MAX_MSGS,
        .mq_msgsize = VELACLAW_MQ_MSG_SIZE,
    };
    mqd_t mq = mq_open(VELACLAW_MQ_QAPP_OUT, O_WRONLY | O_CREAT, 0666, &attr);
    if (mq == (mqd_t)-1) {
        syslog(LOG_ERR, "[%s] quickapp dispatch: mq_open failed: %d\n", TAG, errno);
        return;
    }

    char *buf = (char *)malloc(VELACLAW_MQ_MSG_SIZE);
    if (!buf) {
        mq_close(mq);
        return;
    }
    int len = snprintf(buf, VELACLAW_MQ_MSG_SIZE, "%s\n%s", chat_id, content);
    if (len >= (int)VELACLAW_MQ_MSG_SIZE) {
        len = VELACLAW_MQ_MSG_SIZE - 1;
        buf[len] = '\0';
    }

    if (mq_send(mq, buf, len + 1, 0) != 0) {
        syslog(LOG_ERR, "[%s] quickapp dispatch: mq_send failed: %d\n", TAG, errno);
    } else {
        syslog(LOG_INFO, "[%s] quickapp dispatch: reply sent chat_id=%s\n", TAG, chat_id);
    }
    free(buf);
    mq_close(mq);
}
#endif /* CONFIG_FEATURE_SYSTEM_VELACLAW */

/* ── Outbound dispatch ────────────────────────────────────────── */

/* ── Voice failure cooldown — prevent error-message cascade ──── */

static volatile bool s_voice_cooldown;
static time_t s_voice_cooldown_until;

/**
 * Reads messages from the outbound queue and dispatches them to the
 * appropriate channel (FeiShu, WebSocket, CLI, etc.).
 */
static void* outbound_dispatch_task(void* arg)
{
    (void)arg;
    syslog(LOG_INFO, "[%s] Outbound dispatch started\n", TAG);

    while (!g_shutdown_requested) {
        agent_msg_t msg;
        if (message_bus_pop_outbound(&msg, 1000) != OK)
            continue;
        msg.channel[sizeof(msg.channel) - 1] = '\0';
        msg.chat_id[sizeof(msg.chat_id) - 1] = '\0';

        if (!msg.content) {
            syslog(LOG_WARNING, "[%s] Skip outbound message with NULL content\n", TAG);
            continue;
        }

        syslog(LOG_INFO, "[%s] Dispatching response → %s:%s\n", TAG, msg.channel, msg.chat_id);

        /* Let registered taps intercept before normal dispatch */
        if (mbus_tap_try_deliver(&msg)) {
            free(msg.content);
            continue;
        }

        if (strcmp(msg.channel, AGENT_CHAN_FEISHU) == 0) {
            bool delivered = false;
#ifdef CONFIG_AI_AGENT_FEISHU
            const char* app_id = feishu_get_app_id();
            if (app_id && app_id[0] != '\0') {
                feishu_send_message(msg.chat_id, msg.content);
                delivered = true;
            }
#endif
#ifdef CONFIG_AI_AGENT_NODE
            if (!delivered) {
                delivered = node_client_send_chat_message(
                    msg.channel, msg.chat_id, msg.content) == OK;
            }
#endif
            if (!delivered) {
                syslog(LOG_WARNING,
                    "[%s] Feishu message dropped: no local config "
                    "and no gateway\n",
                    TAG);
            }
        } else if (strcmp(msg.channel, AGENT_CHAN_WEBSOCKET) == 0) {
            ws_server_send(msg.chat_id, msg.content);
#ifdef CONFIG_AI_AGENT_MQTT
        } else if (strcmp(msg.channel, AGENT_CHAN_MQTT) == 0) {
            mqtt_channel_send(msg.chat_id, msg.content);
#endif
        } else if (strcmp(msg.channel, AGENT_CHAN_VOICE) == 0) {
            /* Check voice cooldown — skip voice dispatch if a recent
             * speak call failed, to prevent error-message cascade. */
            if (s_voice_cooldown && time(NULL) < s_voice_cooldown_until) {
                syslog(LOG_WARNING,
                    "[%s] voice cooldown active, dropping message\n", TAG);
            } else {
                s_voice_cooldown = false;
                int vret = voice_channel_speak(msg.content);
                if (vret != 0) {
                    syslog(LOG_ERR, "[%s] voice_channel_speak failed: %d\n", TAG, vret);
                    s_voice_cooldown = true;
                    s_voice_cooldown_until = time(NULL) + 5;
                }
            }
#ifdef CONFIG_AI_AGENT_LVGL_UI
        } else if (strcmp(msg.channel, AGENT_CHAN_LVGL_UI) == 0) {
            int uret = lvgl_ui_channel_send(msg.content);
            if (uret != 0) {
                syslog(LOG_ERR, "[%s] lvgl_ui_channel_send failed: %d\n", TAG, uret);
            }
#endif
#ifdef CONFIG_AI_AGENT_WEIXIN
        } else if (strcmp(msg.channel, AGENT_CHAN_WEIXIN) == 0) {
            /* chat_id format: "from_user_id|context_token" */
            char uid[64] = "";
            const char* ctx = "";
            char* sep = strchr(msg.chat_id, '|');
            if (sep) {
                size_t ulen = (size_t)(sep - msg.chat_id);
                if (ulen >= sizeof(uid))
                    ulen = sizeof(uid) - 1;
                memcpy(uid, msg.chat_id, ulen);
                uid[ulen] = '\0';
                ctx = sep + 1;
            } else {
                strncpy(uid, msg.chat_id, sizeof(uid) - 1);
            }
            weixin_channel_send(uid, ctx, msg.content);
#endif
        } else if (strcmp(msg.channel, AGENT_CHAN_SYSTEM) == 0) {
            syslog(LOG_INFO, "[%s] System message [%s]: %.128s\n", TAG, msg.chat_id, msg.content);
#ifdef CONFIG_FEATURE_SYSTEM_VELACLAW
        } else if (strcmp(msg.channel, AGENT_CHAN_QUICKAPP) == 0) {
            quickapp_mq_dispatch(msg.chat_id, msg.content);
#endif
        } else if (strcmp(msg.channel, "cli") == 0) {
            pthread_mutex_lock(&g_stdout_lock);
            printf("\n[Agent]: %s\nvela> ", msg.content);
            fflush(stdout);
            pthread_mutex_unlock(&g_stdout_lock);
            syslog(LOG_INFO, "[agent] [Agent]: %s\n", msg.content);
        } else {
            syslog(LOG_WARNING, "[%s] Unknown channel: %s\n", TAG, msg.channel);
        }

        free(msg.content);
    }

    syslog(LOG_INFO, "[%s] Outbound dispatch exiting\n", TAG);
    return NULL;
}

/* ── Entry point ──────────────────────────────────────────────── */

/* ── Startup timing helpers ───────────────────────────────────── */

static inline long boot_ms(struct timespec* t0)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)((now.tv_sec - t0->tv_sec) * 1000L + (now.tv_nsec - t0->tv_nsec) / 1000000L);
}

#define BOOT_LOG(t0, phase, msg) \
    syslog(LOG_INFO, "[%s] [boot +%ldms] " phase ": " msg "\n", TAG, boot_ms(t0))

#define BOOT_LOG_RC(t0, phase, msg, rc) \
    syslog((rc) == OK ? LOG_INFO : LOG_WARNING, \
        "[%s] [boot +%ldms] " phase ": " msg " (rc=%d)\n", TAG, boot_ms(t0), (rc))

#if 1  /* [V398] 原为 #ifdef CONFIG_AI_AGENT_VOICE_BOOT_TEST；该宏是外部包符号，nuttx olddefconfig 每次都会丢弃，导致整个自检代码从未编译 */
/* One-shot audio bring-up self-test (task #32).
 *
 * The PTT entry points (voice_channel_start/stop) are only referenced
 * from the opt-in stdin CLI (SHELL_STDIN, default off: a daemon
 * blocking on console stdin steals NSH RX bytes) and from the LVGL UI
 * (not enabled on this board).  With both triggers compiled out,
 * --gc-sections strips the whole voice-capture chain (recording_thread
 * -> audio_capture_* -> native_capture_*) from the final ELF and the
 * microphone can never be exercised.  This task gives the chain a live
 * caller at boot:
 *
 *   TTS greeting via voice_channel_speak()   -> /dev/audio/pcm0p
 *   short PTT via voice_channel_start()/stop_with_text()
 *        -> recording_thread -> native_capture_read -> pcm0c
 *
 * ASR transcripts are logged to syslog.  Disable this option once
 * audio bring-up has been verified on the target. */
#define VOICE_BOOT_TEST_DELAY_SEC  10
#define VOICE_BOOT_TEST_PTT_SEC    8

/* [DIAG] forward declaration so voice_boot_test_task can emit server-visible
 * probe beacons before the definition below. */
static void diag_beacon(const char *tag);

/* [V207] LOCAL speaker bring-up test -- no network, no TTS, no agent loop.
 *
 * Writes a square-wave beep straight into the ES8311/I2S0 backend.  This is
 * the only test on the board that isolates the speaker chain
 *     I2S0 TX -> ES8311 DAC -> NS4150B PA -> speaker
 * from WiFi / DNS / the TTS bridge.  A square wave is used on purpose: no
 * libm, no tables, and it is unmistakable even through a small speaker.
 *
 * If the board is silent here, the fault is hardware or codec configuration
 * and no amount of network fixing will produce sound. */
static void spk_local_test(void)
{
    static const unsigned int hz[3] = { 440, 660, 880 };
    /* [V234] Sweep the codec DAC volume so the correct direction of register
     * 0x32 can be identified by ear: some ES8311 revisions treat it as an
     * attenuation (0x00 = 0 dB / loudest) instead of a gain. */
    static const unsigned int vol[4] = { 0x00, 0x80, 0xBF, 0xFF };
    int round;
    const unsigned int rate = 16000;
    audio_playback_t *pb;
    unsigned char buf[4096];
    int16_t *s = (int16_t *)buf;
    unsigned int b, i, done, total, per;

    pb = audio_playback_open(AGENT_AUDIO_PLAYBACK_DEV, rate, 1, 16);
    if (pb == NULL) {
        syslog(LOG_ERR, "[SPKTEST] playback open FAILED - speaker chain dead\n");
        return;
    }

    syslog(LOG_INFO, "[SPKTEST] volume sweep: 4 beeps, reg32 = "
        "0x00 0x80 0xBF 0xFF\n");

    for (round = 0; round < 1; round++)
    for (b = 0; b < 4; b++) {
        audio_direct_set_dac_volume(vol[b]);

        per   = rate / (300 + b * 100) / 2;      /* 300/400/500/600 Hz steps */
        total = (rate * 500) / 1000;             /* 500 ms per beep */
        if (per == 0) {
            per = 1;
        }

        for (done = 0; done < total; ) {
            unsigned int n = total - done;
            if (n > sizeof(buf) / 2) {
                n = sizeof(buf) / 2;
            }
            for (i = 0; i < n; i++) {
                /* [V232] Full-scale square wave.  +-9000 (27 % of full scale,
                 * about -11 dBFS) was audible but too quiet to be sure about,
                 * and this register write is the only gain the beep controls. */
                s[i] = (((done + i) / per) & 1) ? (int16_t)32000
                                                : (int16_t)-32000;
            }
            if (audio_playback_write(pb, buf, n * 2) < 0) {
                syslog(LOG_ERR, "[SPKTEST] write failed on beep %u\n", b + 1);
                goto out;
            }
            done += n;
        }

        /* 150 ms of silence between the notes */
        memset(buf, 0, sizeof(buf));
        total = (rate * 250) / 1000;      /* 250 ms gap */
        for (done = 0; done < total; ) {
            unsigned int n = total - done;
            if (n > sizeof(buf) / 2) {
                n = sizeof(buf) / 2;
            }
            if (audio_playback_write(pb, buf, n * 2) < 0) {
                break;
            }
            done += n;
        }
    }

    syslog(LOG_INFO, "[SPKTEST] beeps done (audible => speaker chain OK)\n");

    audio_direct_dump_codec();

out:
    audio_playback_close(pb);
}

extern void audio_direct_set_dac_volume(unsigned int v);
extern void audio_direct_dump_codec(void);

/* [V213] Speaker smoke task -- runs BEFORE the 90 s network gate.
 *
 * voice_boot_test_task (which also calls spk_local_test) is only created after
 * network_wait_connected(90000) returns, so on any boot where DHCP never
 * completes, the local beep test gets pushed past that gate and is effectively
 * invisible -- which is exactly what happened when the speaker fix was first
 * flashed.  The speaker chain has nothing to do with WiFi, so the beeps are
 * emitted here, one second into the agent's life, and tagged SPKSOK so they
 * cannot be confused with anything else. */
static void* spk_smoke_task(void* arg)
{
    (void)arg;

    /* [V333] 停用启动自检。它在联网门之后运行，与开机问候语的播报同时
     * 抢同一条 I2S TX 通道，两者互相打断（表现为 TX completion TIMEOUT
     * 与 i2s_send -12）。要恢复原行为，删掉下面这两行即可。 */

    if (0)
      {
        spk_local_test();   /* 保留引用，避免 unused-function 告警 */
      }

    
    return NULL;

    /* [V315] Wait out the boot-time WiFi association storm: while the
     * association is flapping (reason=2/205 for the first ~10 s) the WiFi
     * blob starves the I2S GDMA completion path and every beep times out,
     * which looked exactly like a dead speaker on v313/v314 -- even though
     * the very same binary beeps fine once the link is up (v306).  Beep
     * only after the link; if there is no link after 30 s, beep anyway so
     * the offline audio chain still gets exercised. */
    if (network_wait_connected(30000) != OK) {
        syslog(LOG_ERR, "[SPKSOK] no link after 30s - beeping anyway\n");
    }

    syslog(LOG_INFO,
        "[SPKSOK] early speaker smoke test BEGIN (network not required)\n");
    spk_local_test();
    syslog(LOG_INFO, "[SPKSOK] early speaker smoke test END\n");

    return NULL;
}

static void* voice_boot_test_task(void* arg)
{
    int ret;

    (void)arg;

    syslog(LOG_INFO, "[%s] [DIAG] voice_boot_test_task entered\n", TAG);

    /* [V374] The speaker smoke test is gone.
     *
     * It opens /dev/audio/pcm0p and drives the I2S TX path for four beeps.
     * On v373 that happened while a BOOT-key PTT capture was already
     * streaming, and the two fought over the same transmitter:
     *   [audio_direct] I2S TX completion TIMEOUT after 1.5 s (n=1)
     *   [SPKTEST] write failed on beep 1
     *   [voice] capture EIO, retry 1/5
     * The chain is already proven ("[SPKTEST] beeps done (audible =>
     * speaker chain OK)" every boot), and the greeting below is a better
     * audible proof anyway because it exercises TTS + playback + network. */
    

    sleep(VOICE_BOOT_TEST_DELAY_SEC);

    /* [V400] 等链路稳定后再开采集。
     * v398 的现场：RX 首帧刚到（[RXEOF] first SUC_EOF!）就被 WiFi 的
     * 闭源 FIQ 打断，调用链是 irq_dispatch -> wDev_ProcessFiq ->
     * xtensa_user -> xtensa_user_panic -> _assert -> up_saveusercontext，
     * EXCCAUSE=001c 且 VADDR=0x00000000（向空指针保存现场）。
     * WiFi 在开机关联风暴期（STA_DISCONNECT=12）FIQ 最密集，
     * 所以把采集探针挪到链路就绪之后；拿不到链路也照样跑，不隐藏问题。 */
    if (network_wait_connected(60000) != OK)
      {
        
      }

    /* [V384] Capture probe FIRST, network gate second.
     *
     * See native_capture_selftest() in audio_capture_native.c.  It needs
     * no network, but it used to sit after network_wait_connected(60000)
     * and three greeting attempts -- on a boot where the link flaps
     * (v383: greeting failed twice, STA_DISCONNECT=12) it never ran at
     * all and the boot log contained no probe output whatsoever.  Running
     * it here guarantees the pin sampling and the loopback measurement
     * happen on every boot, and it also leaves the board in phase 2 for
     * the next real BOOT press.
     */

    /* [V401] CAPTURE_PROBE 开关。
     * =1 开机跑采集自检；=0 跳过。
     *
     * 现状（V398/V400 实测）：探针一旦跑起来，RX 第一帧确实收到了
     * （[RXEOF] first SUC_EOF!、[RXBOUNCE] 回灌到 PSRAM 缓冲、
     * [AUDIO-R42] xfer ret=0），但紧接着系统必崩：
     *   EXCCAUSE=001c，VADDR=0x00000000，
     *   调用链 irq_dispatch -> wDev_ProcessFiq -> xtensa_user ->
     *   xtensa_user_panic -> _assert -> up_saveusercontext
     * 即崩溃转储时向空指针保存现场，说明 WiFi 闭源 FIQ 在 RX 中断期间
     * 触发了一次异常。中断栈实测只用了 336 字节（4096/6144/8192 都一样），
     * 所以不是栈容量问题，是 RX 中断与 WiFi FIQ 的并发/内存冲突。
     * 为了让设备今晚可用，默认置 0；置 1 即可复现并继续排查。 */
#define CAPTURE_PROBE 0

#if CAPTURE_PROBE
    {
        extern void native_capture_selftest(void);

        native_capture_selftest();
    }
#else
#endif

    /* [V81] Wait for the link before opening the microphone.
     *
     * Every probe around the capture path is an HTTP beacon.  Fired before the
     * association completes they are silently lost, and a hang inside
     * audio_capture_start() then looks exactly like "the boot test never ran" -
     * which is what v79 and v80 both looked like from the server side. */

    diag_beacon("B7_BT_ENTER");

    if (network_wait_connected(60000) != OK) {
        syslog(LOG_ERR,
            "[voice_btest] no link after 60s - probes below will be lost\n");
        diag_beacon("B7_BT_NOLINK");
    } else {
        diag_beacon("B7_BT_LINKED");
    }

    /* [V77] Microphone FIRST, greeting second.
     *
     * v73 tried the greeting first and the board went mute: voice_channel_speak()
     * streams the TTS body and drives playback, and it does not return, so
     * voice_channel_listen_start() was never reached (no B7_LS_OK, no
     * B7_LST_TASK, no sessions at all).  Listening is the feature; the greeting
     * is cosmetic and is allowed to fail. */

    /* [SP-FIRST] Greeting BEFORE the microphone.
     *
     * V77 ordered the mic first because voice_channel_speak() can block for a
     * long time on the cloud TTS.  But voice_channel_listen_start() can block
     * too -- it fires diag_report("LISTEN_START") / vc_beacon() right after
     * creating the task, and when that call hangs the greeting below is never
     * reached at all.  Captured live on v272: "[voice_btest] 1/2 mic: starting
     * always-on listening" with no following line, and not a single
     * "[voice] speak:" for the whole 188 s boot -- so the board stayed silent
     * apart from the SPKTEST beeps.  The greeting is the audible proof that the
     * whole voice chain works, so run it first. */

    /* [V510] The greeting now runs on its own thread. See the comment on
     * boot_greeting_task(): a synchronous speak() here blocked the start of
     * listening for the whole synthesis, and the three retry loop multiplied
     * the stall. Nothing below this point may depend on it. */
    {
        pthread_attr_t attr;
        pthread_t th;
        /* Sizes to try, largest first.  boot_greeting_task() runs the FULL
         * TTS playback chain (speak -> mimo https -> tts_stream_cb ->
         * audio_playback_write), which needs a real audio stack: the 2 KB
         * it used to get overflowed the moment PCM arrived and killed the
         * board with EXCCAUSE=0014.  Never go below 4096 (the NuttX
         * default).  8192 is the smallest size observed to survive the
         * chain, but we ask for more first and settle for what we get. */
        static const size_t stacks[] = { 16384, 12288, 8192, 4096 };
        size_t i;
        int rc = -1;
        size_t chosen = 0;

        for (i = 0; i < sizeof(stacks) / sizeof(stacks[0]); i++)
          {
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, stacks[i]);
            rc = pthread_create(&th, &attr, boot_greeting_task, NULL);
            pthread_attr_destroy(&attr);
            if (rc == 0)
              {
                chosen = stacks[i];
                break;
              }
            syslog(LOG_WARNING,
                   "[greeting] thread create with %u B failed (%d), trying "
                   "smaller\n", (unsigned)stacks[i], rc);
          }

        if (rc == 0)
          {
            syslog(LOG_INFO, "[greeting] thread started (stack %u B)\n",
                   (unsigned)chosen);
          }
        else
          {
            syslog(LOG_WARNING,
                   "[greeting] thread create failed at every size, "
                   "skipping\n");
          }
    }

    /* [V381] Automatic capture probe: see native_capture_selftest() in
     * audio_capture_native.c.  Runs while the board is otherwise idle, so
     * the loopback measurement does not depend on a key press, and leaves
     * the board in phase 2 for the next real BOOT press.
     */

    /* [V401] CAPTURE_PROBE 开关。
     * =1 开机跑采集自检；=0 跳过。
     *
     * 现状（V398/V400 实测）：探针一旦跑起来，RX 第一帧确实收到了
     * （[RXEOF] first SUC_EOF!、[RXBOUNCE] 回灌到 PSRAM 缓冲、
     * [AUDIO-R42] xfer ret=0），但紧接着系统必崩：
     *   EXCCAUSE=001c，VADDR=0x00000000，
     *   调用链 irq_dispatch -> wDev_ProcessFiq -> xtensa_user ->
     *   xtensa_user_panic -> _assert -> up_saveusercontext
     * 即崩溃转储时向空指针保存现场，说明 WiFi 闭源 FIQ 在 RX 中断期间
     * 触发了一次异常。中断栈实测只用了 336 字节（4096/6144/8192 都一样），
     * 所以不是栈容量问题，是 RX 中断与 WiFi FIQ 的并发/内存冲突。
     * 为了让设备今晚可用，默认置 0；置 1 即可复现并继续排查。 */
#define CAPTURE_PROBE 0

#if CAPTURE_PROBE
    {
        extern void native_capture_selftest(void);

        native_capture_selftest();
    }
#else
#endif

    /* 2/2 Microphone.  v69 releases the channel right after the recording
     * thread joins and v73 stops the listen loop from re-opening a session
     * while a speaker is waiting. */

    /* [V374] No always-on listening.
     *
     * It holds the capture open for the whole session, which in turn keeps
     * the [V373] clock-keeper thread streaming silence into the I2S TX
     * forever.  v373 shows what that costs: a few seconds after
     * "[voice] always-on listening active" the link fell apart --
     *   [DIAG] evt STA_DISCONNECT reason=8
     *   [DIAG] evt STA_DISCONNECT reason=2 / 205
     *   [V98C] connect failed: 101
     *   [V58] probe gw:80 / srv:2301 / srv:9009 / llm:2302 = -110
     * i.e. no TCP at all, so TTS and ASR could never succeed.  This is the
     * same failure mode as the old faced animation: a permanently running
     * DMA task starves the WiFi blob.
     * Wake is the BOOT key (button PTT); it needs no background capture. */
    /* [V712] *** Always-on listening re-enabled. ***
     *
     * V374 turned it off because a permanently open capture kept the I2S TX
     * clock-keeper streaming silence into the transmitter, which starved the
     * WiFi blob (STA_DISCONNECT reason=8 then 101, no TCP at all).  Every part
     * of that chain is fixed now: v692 cleared I2S_TX_STOP_EN so the
     * transmitter stops when idle, v694 re-arms the I2S clock only when a
     * capture really starts, v695 fixed the shared upper-half gate that
     * silently skipped audio_start(), and v711 fixed the playback pipeline
     * that was killing the board outright.
     *
     * The listener is not a free-running DMA task either -- it opens a
     * session, waits at most VOICE_LISTEN_NO_SPEECH_MS for speech, closes it
     * and settles, so the radio sees a duty cycle rather than a saturated bus.
     *
     * Kill switch, so this can be turned off without a reflash:
     *     touch /tmp/wakeoff.txt     -> listening stays off for this boot */

    {
      FILE *woff = fopen("/tmp/wakeoff.txt", "r");
      /* [V876] aivox owns the microphone.
       *
       * This gate used to be controlled only by /tmp/wakeoff.txt, which
       * does not exist, so wake_on was 1 and voice_channel_listen_start()
       * happily returned 0 -- starting ai_agent's own always-on listening
       * even though [V872] had already disabled voice_channel_init().
       * The result was two owners of one I2S port: aivox holds pcm0c (and
       * pcm0p as its capture clock, audio_capture_native.c:122
       * CAP_CLOCK_DEV) while ai_agent tried to do the same.  That is what
       * produces the EXCCAUSE=001c LOAD_PROHIBITED panic inside vsyslog.
       *
       * The user asked for exactly one voice assistant, so force the gate
       * shut rather than depending on a file. */
      int   wake_on = 0;

      if (woff != NULL)
        {
          fclose(woff);
        }

      if (wake_on && voice_channel_listen_start() == 0)
        {
          syslog(LOG_INFO,
                 "[V712] always-on listening ENABLED - say the wake word\n");
        }
      else
        {
          syslog(LOG_INFO,
                 "[V712] always-on listening DISABLED - press BOOT to talk\n");
        }
    }

    (void)ret;

    return NULL;
}
#endif

/* [R48] One-shot SNTP time sync.  Triggered right after the network gate
 * passes (WiFi + DNS are up) so the system CLOCK_REALTIME is set even if the
 * autostarted ntpclient daemon missed its boot-time window.  The info_screen
 * app already redraws the clock every second, so once this succeeds the panel
 * clock starts ticking.  Falls back to hard-coded NTP server IPs if DNS is not
 * yet ready. */
#ifndef CONFIG_NETUTILS_NTPCLIENT_SERVER
#  define CONFIG_NETUTILS_NTPCLIENT_SERVER "cn.pool.ntp.org"
#endif

/* [V59] Set the clock over the TCP path that is known to work.
 *
 * SNTP needs UDP/123 to four different servers and on this network all of
 * them time out, which leaves the RTC at 1970 and makes info_screen paint
 * "--:--" (it only draws HH:MM once tm_year + 1900 >= 2020).  The TTS bridge
 * on 172.16.88.1:2301 answers GET /time with unix seconds and TCP 2301 is
 * the one path the board has proven it can reach.
 */

#ifndef AGENT_TIME_HTTP_HOST
#  define AGENT_TIME_HTTP_HOST "172.16.88.1"
#endif

#ifndef AGENT_TIME_HTTP_PORT
#  define AGENT_TIME_HTTP_PORT 2301
#endif

static int agent_sync_time_http(void)
{
  struct sockaddr_in sa;
  struct timeval tv;
  struct timespec ts;
  fd_set wf;
  char req[128];
  char buf[192];
  long long sec;
  char *p;
  ssize_t got;
  socklen_t elen = sizeof(int);
  int err = 0;
  int fd;
  int ret;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    {
      return -1;
    }

  ret = fcntl(fd, F_GETFL, 0);
  if (ret >= 0)
    {
      fcntl(fd, F_SETFL, ret | O_NONBLOCK);
    }

  memset(&sa, 0, sizeof(sa));
  sa.sin_family      = AF_INET;
  sa.sin_port        = htons(AGENT_TIME_HTTP_PORT);
  sa.sin_addr.s_addr = inet_addr(AGENT_TIME_HTTP_HOST);

  ret = connect(fd, (FAR struct sockaddr *)&sa, sizeof(sa));
  if (ret < 0 && errno != EINPROGRESS)
    {
      close(fd);
      return -1;
    }

  if (ret < 0)
    {
      FD_ZERO(&wf);
      FD_SET(fd, &wf);
      tv.tv_sec  = 6;
      tv.tv_usec = 0;

      ret = select(fd + 1, NULL, &wf, NULL, &tv);
      if (ret <= 0)
        {
          close(fd);
          return -1;
        }

      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0)
        {
          close(fd);
          return -1;
        }
    }

  ret = fcntl(fd, F_GETFL, 0);
  if (ret >= 0)
    {
      fcntl(fd, F_SETFL, ret & ~O_NONBLOCK);
    }

  tv.tv_sec  = 6;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  snprintf(req, sizeof(req), "GET /time HTTP/1.0\r\nHost: %s\r\n\r\n",
           AGENT_TIME_HTTP_HOST);

  if (send(fd, req, strlen(req), 0) <= 0)
    {
      close(fd);
      return -1;
    }

  got = recv(fd, buf, sizeof(buf) - 1, 0);
  close(fd);

  if (got <= 0)
    {
      return -1;
    }

  buf[got] = '\0';
  p = strstr(buf, "\r\n\r\n");
  p = (p != NULL) ? p + 4 : buf;

  sec = atoll(p);
  if (sec < 1600000000LL)
    {
      return -1;
    }

  ts.tv_sec  = (time_t)sec;
  ts.tv_nsec = 0;

  if (clock_settime(CLOCK_REALTIME, &ts) != 0)
    {
      syslog(LOG_ERR, "[time] http clock_settime failed: %d\n", errno);
      return -1;
    }

  syslog(LOG_INFO, "[time] clock set from bridge /time (unix=%ld)\n",
         (long)sec);
  return 0;
}

static void agent_sync_time_sntp(void)
{
  static const char *fallback_ips[] = {
    "203.107.6.88",     /* Aliyun NTP (CN) */
    "162.159.200.123",  /* Cloudflare NTP */
    "120.25.108.11",    /* Aliyun NTP (CN) */
    NULL
  };
  struct sockaddr_in candidates[8];
  int ncand = 0;

  /* [V67] TCP first.  UDP/123 is blocked on this network, so the four SNTP
   * candidates below each burn their timeout before the HTTP fallback is
   * even tried - which is why the panel clock stayed at "--:--" for minutes
   * after the link came up.  The bridge's GET /time answers on the one port
   * this board has proven it can reach, so ask it before spending ~20 s on
   * SNTP. */

  if (agent_sync_time_http() == 0)
    {
      return;
    }

  /* 1) DNS-resolved primary server from Kconfig */
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  if (getaddrinfo(CONFIG_NETUTILS_NTPCLIENT_SERVER, "123", &hints, &res) == 0 && res) {
    struct sockaddr_in *a = (struct sockaddr_in *)res->ai_addr;
    candidates[ncand] = *a;
    freeaddrinfo(res);
    ncand++;
  }

  /* 2) Hard-coded IP fallbacks */
  for (int i = 0; fallback_ips[i] && ncand < 8; i++) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    if (inet_pton(AF_INET, fallback_ips[i], &a.sin_addr) == 1)
      candidates[ncand++] = a;
  }

  if (ncand == 0) {
    syslog(LOG_ERR, "[time] no NTP candidate address available\n");
    return;
  }

  for (int c = 0; c < ncand; c++) {
    candidates[c].sin_port = htons(123);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) continue;
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t req[48];
    memset(req, 0, sizeof(req));
    req[0] = 0x1B;  /* LI=0, VN=3, Mode=3 (client) */
    if (sendto(fd, req, sizeof(req), 0,
               (struct sockaddr *)&candidates[c], sizeof(candidates[c])) == sizeof(req)) {
      uint8_t resp[48];
      ssize_t n = recvfrom(fd, resp, sizeof(resp), 0, NULL, NULL);
      if (n == 48) {
        uint32_t sec1900 = ((uint32_t)resp[40] << 24) | ((uint32_t)resp[41] << 16) |
                           ((uint32_t)resp[42] << 8) | resp[43];
        uint32_t sec1970 = sec1900 - 2208988800u;
        struct timespec ts = { (time_t)sec1970, 0 };
        if (clock_settime(CLOCK_REALTIME, &ts) == 0) {
          syslog(LOG_INFO, "[time] SNTP synced OK via candidate %d (unix=%u)\n", c, sec1970);
          close(fd);
          return;
        } else {
          syslog(LOG_ERR, "[time] clock_settime failed (errno=%d)\n", errno);
        }
      }
    }
    close(fd);
  }
  /* Every NTP candidate timed out: fall back to the bridge. */

  if (agent_sync_time_http() == 0)
    {
      return;
    }

  syslog(LOG_ERR, "[time] SNTP sync failed after %d candidates\n", ncand);
}

/* [R48b] Periodic NTP re-sync so the panel clock stays correct WITHOUT a
 * reboot.  The one-shot at boot (agent_sync_time_sntp, called below) sets
 * CLOCK_REALTIME once WiFi+DNS are up; this worker repeats it every 5 minutes
 * to absorb drift and any boot-window miss (the autostarted ntpclient daemon
 * runs before WiFi is associated, so it alone never keeps the clock right). */
static void* agent_time_keeper(void* arg)
{
  (void)arg;
  while (!g_shutdown_requested) {
    time_t before = time(NULL);

    /* [V64] While the clock is still unset (1970) retry every 30 s: the panel
     * paints "--:--" until tm_year + 1900 >= 2020, and the boot-window
     * attempt often lands before the link has settled (measured: the probe to
     * 172.16.88.1:2301 timed out at t=16 s and succeeded at t=17 s).  Once
     * the clock is set, drift correction every 5 minutes is plenty. */

    sleep((before < 1600000000L) ? 30 : 300);
    agent_sync_time_sntp();

    if (before < 1600000000L && time(NULL) >= 1600000000L) {
      syslog(LOG_INFO, "[time] clock is set now (unix=%ld)\n",
             (long)time(NULL));
    }
  }
  return NULL;
}

/* [BOOT-BEACON] Proof-of-boot ping: fires a unique TTS request right after
 * the network gate (before any audio/voice task).  The TTS service logs every
 * request, so seeing BOOTBEACON_V7 in tts-pcm proves THIS firmware booted.
 * Uses a no-op chunk callback so it never touches the (possibly broken) audio
 * path.  This is diagnostic only and can be removed once audio is confirmed. */
#include "voice/http_tts.h"
#include "sensor_chat.h"

/* [V207] Boot probes must NOT synthesise speech.
 *
 * diag_beacon() used to be http_tts_speak_stream(tag, ...): every boot probe
 * asked the TTS server to render the tag text to ~30 kB of PCM and blocked
 * the boot self-test task for the duration (4 connect attempts x 15 s + 8 s
 * recv).  The first probe fired at t+19 s and never returned, so the greeting
 * and the microphone bring-up behind it never ran -- which is why no
 * [voice_btest] line was ever observed.
 *
 * Probes now use the lightweight /diag endpoint and are mirrored to syslog. */
static void diag_beacon(const char *tag)
{
  syslog(LOG_INFO, "[agent] [BEACON] %s\n", tag);
  diag_report(tag);
}

static void agent_boot_beacon(void)
{
  syslog(LOG_INFO, "[BOOT-BEACON] firing v7 boot ping\n");
  /* [RECOVERY] disabled: playing TTS this early hangs the board.
   * Capture showed the board stops responding ~10s after receiving PCM.
   * Re-enable only once audio playback is proven non-blocking. */
  return;
}

/* ── Init watchdog ──────────────────────────────────────────────────────
 * Some initialisers hang forever on this board (SMART FS ops, tool registry).
 * Running each one in its own thread with a hard timeout keeps a single stuck
 * call from blocking the whole agent, which is what killed voice dialogue.
 * The thread is deliberately abandoned on timeout (its context is heap
 * allocated and leaked) so the caller can safely continue.
 */
typedef struct {
    int (*fn)(void);
    volatile int done;
    volatile int rc;
} wd_ctx_t;

static void* wd_thread(void* arg)
{
    wd_ctx_t* c = (wd_ctx_t*)arg;
    c->rc = c->fn();
    c->done = 1;
    return NULL;
}

static int run_init_to(const char* name, int (*fn)(void), int timeout_ms)
{
    pthread_attr_t attr;
    pthread_t tid;
    wd_ctx_t* ctx;
    int waited = 0;

    ctx = (wd_ctx_t*)malloc(sizeof(wd_ctx_t));
    if (ctx == NULL) {
        return fn();                 /* no heap: run inline */
    }

    ctx->fn = fn;
    ctx->done = 0;
    ctx->rc = 0;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16384);

    if (pthread_create(&tid, &attr, wd_thread, ctx) != 0) {
        pthread_attr_destroy(&attr);
        free(ctx);
        return fn();
    }
    pthread_attr_destroy(&attr);

    while (!ctx->done && waited < timeout_ms) {
        usleep(100000);
        waited += 100;
    }

    if (!ctx->done) {
        syslog(LOG_WARNING, "[WD] %s TIMED OUT after %dms - continuing\n",
               name, timeout_ms);
        return -ETIMEDOUT;           /* ctx intentionally leaked */
    }

    pthread_join(tid, NULL);
    {
        int rc = ctx->rc;
        free(ctx);
        return rc;
    }
}

/* [EARLY-VOICE] set once the voice channel has been brought up during
 * Phase 3, so the later phases do not initialise it a second time. */
static int s_voice_brought_up_early;
int ai_agent_main(int argc, char* argv[])
{
    /* Product build: discard stdout/stderr (USB CDC blocks when no host
     * reader is attached).  Diagnostics go to syslog (/dev/kmsg). */
    freopen("/dev/null", "w", stdout);
    freopen("/dev/null", "w", stderr);

    (void)argc;
    (void)argv;

    g_shutdown_requested = false;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    syslog(LOG_INFO, "[%s] AI Agent - Vela AI Agent starting (build: %s)\n",
        TAG, AGENT_BUILD_VERSION);

#if 1  /* [V398] 原为 #ifdef CONFIG_AI_AGENT_VOICE_BOOT_TEST；该宏是外部包符号，nuttx olddefconfig 每次都会丢弃，导致整个自检代码从未编译 */
    /* [V213] Prove the speaker NOW.  Anything placed after the network gate
     * below is invisible until DHCP succeeds (up to 90 s), and the speaker
     * does not depend on the network at all. */
    if (agent_task_create(spk_smoke_task, "spk_smoke",
            AGENT_OUTBOUND_STACK, NULL, AGENT_OUTBOUND_PRIO) != OK) {
        syslog(LOG_WARNING, "[%s] spk_smoke task creation failed\n", TAG);
    } else {
        syslog(LOG_INFO, "[%s] spk_smoke task started (early beep test)\n",
            TAG);
    }
#endif

    /* [OFFLINE-FIRST] 离线外设初始化，必须放在网络门之前。
     *
     * network_wait_connected(90000) 在网络不通时（实测：拿得到 IP 但 TCP 全
     * -110，板子反复重关联）永远等不到"已连接"，P0~P3 全部阶段都不会执行，
     * 舵机等外设就永远不会被初始化。舵机/台灯/触摸/震动都不依赖网络，
     * 所以按 [EARLY-VOICE] 同样的思路提到这里，先于 boot storm 起来。 */
    {
        extern int servo_init(void);
        extern int servo_self_test(void);

        syslog(LOG_INFO, "[OFFLINE-FIRST] servo init before network gate\n");
        servo_init();
        servo_self_test();

        /* [V502] 舵机保活：周期性重发当前角度，防止被负载推偏或
         *   通道被别人 STOP。必须在 servo_init() 之后。 */
        {
            extern int servo_keepalive_start(void);

            servo_keepalive_start();
        }

        /* [V503] K230 人脸跟踪云台：bind udp/3333 收 F,<cx>,<cy>,<w>,<h>,<conf>，
         *   按绝对映射律驱动 PAN/TILT。K230 侧脚本与小智固件通用，零改动。
         *   舵机没起来时它会自己报错退出，不影响语音。 */
        {
            extern int companion_face_start(void);

            if (companion_face_start() < 0)
                {
                    syslog(LOG_ERR,
                            "[OFFLINE-FIRST] face tracker not started\n");
                }
        }


        /* 触觉情绪：摸它点头(mood+1) / 拍它摇头(mood-1)。
         * 纯离线，不依赖网络，直接在这里起轮询线程。 */
        {
            extern int companion_mood_start(void);

            companion_mood_start();
        }
    }

    /* [WIFI-FIX-R27] Wait for netinit to finish associating before this app's
     * boot storm starts.
     *
     * The association is a single-frame exchange with a millisecond window,
     * and it only ever succeeds when this app is not running: with the agent
     * stopped the station associates every time, with it running it never
     * does.  The agent's own boot is heavy - ~10 threads, 37 tool
     * registrations, config/skill files written to a /data that fell back to
     * tmpfs, and a lot of syslog traffic into the shared RAMLOG ring - and it
     * lands exactly in the seconds netinit needs.  Waiting for the address
     * makes the ordering deterministic instead of hopeful;
     * /tmp/net.txt is only a hint for the panel, netinit keeps retrying
     * underneath either way. */

    syslog(LOG_INFO, "[%s] waiting for wlan0 before boot (max 90s)\n", TAG);
    (void)network_wait_connected(90000);
    syslog(LOG_INFO, "[%s] network gate passed: %s\n", TAG, network_get_ip());

    /* [BOOT-BEACON] fire v7 proof-of-boot ping (logged server-side) */
    agent_boot_beacon();

    /* [R48] Set the system clock now that WiFi + DNS are up.  The info_screen
     * panel already redraws the clock every second, so this makes it tick. */
    agent_sync_time_sntp();

    /* [R48b] Keep it synced: spawn a background re-sync worker (every 5 min)
     * so the clock never needs a reboot to become correct again. */
    {
      pthread_t tid;
      if (pthread_create(&tid, NULL, agent_time_keeper, NULL) == 0) {
        pthread_detach(tid);
      } else {
        syslog(LOG_ERR, "[time] failed to spawn NTP time keeper\n");
      }
    }

    /* ── Phase 0: Timezone ──────────────────────────────────── */
    /* [V758] TZ is INERT in this build.  It is still exported because
     * something else may read the environment, but it does NOT make
     * localtime_r() return CST+8 -- the old comment below this block
     * claimed it did, and that claim is wrong:
     *
     *   nuttx/libs/libc/time/Make.defs compiles lib_localtime.c (the only
     *   TZ-aware implementation; its line 2792 is the tree's sole
     *   getenv("TZ")) only under CONFIG_LIBC_LOCALTIME, and nuttx/.config
     *   has "# CONFIG_LIBC_LOCALTIME is not set".  The localtime_r() we
     *   link is lib_gmtimer.c:345, whose body is just
     *       return gmtime_r(timep, result);
     *   -- plain UTC, and no CONFIG_LIBC_TZ_OFFSET fallback exists.
     *
     * So never rely on localtime_r()/setenv("TZ") for wall-clock text.
     * Use the house convention instead (tool_get_time.c:93,
     * memory_store.c:42, agent_loop.c:612, context_builder.c:153):
     *
     *     time_t local_epoch = now + 8 * 3600;
     *     gmtime_r(&local_epoch, &tm_now);
     *
     * xiaozhi_ui_main.c used to be the sole localtime_r() caller, which
     * is why the on-screen clock alone sat eight hours behind. */
    setenv("TZ", AGENT_TIMEZONE, 1);
#ifdef CONFIG_LIBC_LOCALTIME
    tzset();
#endif
    BOOT_LOG(&t0, "P0", "timezone set");

    /* ── Phase 0: Storage Bootstrapping (Auto-mount & Mkdir) ── */

    struct stat st;
    if (stat("/data", &st) != 0) {
        syslog(LOG_INFO, "[%s] Mounting /data as tmpfs for simulation...\n", TAG);
        mount(NULL, "/data", "tmpfs", 0, NULL);
    }

    /* Ensure directory structure exists (No more manual mkdir needed!) */
    mkdir("/data/agent", 0755);
    mkdir("/data/agent/config", 0755);
    mkdir("/data/agent/memory", 0755);
    mkdir("/data/agent/sessions", 0755);
    mkdir("/data/agent/skills", 0755);
    BOOT_LOG(&t0, "P0", "storage ready");

    /* Memory info */
    {
        struct mallinfo mi = mallinfo();
        syslog(LOG_INFO, "[%s] [boot +%ldms] heap: arena=%d free=%d used=%d\n",
            TAG, boot_ms(&t0), mi.arena, mi.fordblks, mi.uordblks);
    }

    /* ── Phase 1: Core infrastructure ──────────────────────── */
    {
        int rc;
        rc = run_init_to("config_store_init", config_store_init, 12000);
        BOOT_LOG_RC(&t0, "P1", "config_store_init", rc);

        rc = run_init_to("message_bus_init", message_bus_init, 12000);
        BOOT_LOG_RC(&t0, "P1", "message_bus_init", rc);
        if (rc != OK)
            return -1;

        /* [MINIMAL] memory_store_init skipped: not needed for voice dialogue and a known
         * hang/failure candidate on this board. */
        rc = OK;
        BOOT_LOG_RC(&t0, "P1", "memory_store_init", rc);

        /* [MINIMAL] session_mgr_init skipped: not needed for voice dialogue and a known
         * hang/failure candidate on this board. */
        rc = OK;
        BOOT_LOG_RC(&t0, "P1", "session_mgr_init", rc);
    }

    /* ── Phase 2: Proxy / networking ───────────────────────── */
    {
        int /* [MINIMAL] http_proxy_init skipped: not needed for voice dialogue and a known
         * hang/failure candidate on this board. */
        rc = OK;
        BOOT_LOG_RC(&t0, "P2", "http_proxy_init", rc);
    }

    /* ── Phase 3: Application services ─────────────────────── */
    {
        int rc;
#ifdef CONFIG_AI_AGENT_FEISHU
        /* [MINIMAL] feishu_bot_init skipped: not needed for voice dialogue and a known
         * hang/failure candidate on this board. */
        rc = OK;
        BOOT_LOG_RC(&t0, "P3", "feishu_bot_init", rc);
#endif

        sensor_chat_init();   /* [SENSOR-CHAT] 对话控灯模块 */
        rc = run_init_to("llm_proxy_init", llm_proxy_init, 12000);
        BOOT_LOG_RC(&t0, "P3", "llm_proxy_init", rc);

        rc = run_init_to("llm_router_init", llm_router_init, 12000);
        BOOT_LOG_RC(&t0, "P3", "llm_router_init", rc);

        /* [V853] Tool registry BEFORE agent_loop: agent_loop reads
         * tool_registry_get_tools_json() once, before its message loop, and
         * reuses that string for every request (agent_loop.c:1463).  With
         * the registry initialised later the JSON was captured mid-
         * registration -- the log showed "Tools JSON built (5 builtin + 1
         * providers)" / "Tools JSON loaded: 949 bytes" although 41 tools
         * were registered -- so the LLM only ever saw a handful of tools.
         * run_init_to() bounds both at 12 s, so a hang delays voice by at
         * most that much and cannot wedge the chip.
         */
        rc = run_init_to("tool_registry_init", tool_registry_init, 12000);
        BOOT_LOG_RC(&t0, "P3", "tool_registry_init", rc);

        rc = run_init_to("tool_guard_init", tool_guard_init, 12000);
        BOOT_LOG_RC(&t0, "P3", "tool_guard_init", rc);

        /* ── [EARLY-VOICE] ────────────────────────────────────────────
         * Everything voice needs (config store, message bus, LLM router)
         * is initialised at this point.  Later initialisers on this board
         * hang (tool registry, skill loader, cron, BLE, LVGL), so start
         * the voice channel HERE: wake-word + dialogue then work no matter
         * what fails further down the boot sequence. */
        rc = run_init_to("agent_loop_init", agent_loop_init, 12000);
        BOOT_LOG_RC(&t0, "P3", "agent_loop_init(early)", rc);

        rc = run_init_to("voice_channel_init", voice_channel_init, 12000);
        BOOT_LOG_RC(&t0, "P3", "voice_channel_init(early)", rc);

        /* [OFFLINE-FIRST] 舵机初始化已移到网络门之前的 [OFFLINE-FIRST] 块，
         * 此处不再重复执行（避免每次 boot storm 都甩一次舵机）。 */

        if (agent_task_create(outbound_dispatch_task, "outbound",
                AGENT_OUTBOUND_STACK, NULL, AGENT_OUTBOUND_PRIO) == OK) {
            BOOT_LOG(&t0, "P3", "outbound dispatch started (early)");
        } else {
            syslog(LOG_ERR, "[%s] early outbound dispatch failed\n", TAG);
        }

#if 1  /* [V398] 无条件开机自检。
     * CONFIG_AI_AGENT_VOICE_BOOT_TEST 是外部包(packages/ai_agent)的符号，
     * nuttx 的 olddefconfig 每次都会把它从 .config 里丢掉，导致自检任务
     * 从未被创建（v396/v397 日志里 [V380] selftest 计数恒为 0 就是这个原因）。
     * 这里不再依赖该宏，并且在创建失败时补一条 syslog（原来失败是静默的）。 */
        if (agent_task_create(voice_boot_test_task, "voice_btest",
                AGENT_OUTBOUND_STACK, NULL, AGENT_OUTBOUND_PRIO) == OK) {
            BOOT_LOG(&t0, "P3", "voice boot self-test started (early)");
        } else {
            
        }
#endif

        s_voice_brought_up_early = 1;

        /* [V42] Start the dialogue worker before anything else can wedge.
         *
         * agent_loop_init() only logs; the thread that pops ASR transcripts
         * off the message bus and calls the LLM is created by
         * agent_loop_start(), whose only other call site is far down the
         * init chain below -- which hard-hangs this chip. */

        if (agent_loop_start() != OK) {
            syslog(LOG_ERR,
                "[%s] [V42] early agent_loop_start failed\n", TAG);
        } else {
            BOOT_LOG(&t0, "V42", "agent loop worker started (early)");
        }

        /* [V42 VOICE-ONLY]
         *
         * Everything wake-up + dialogue needs is now running:
         *   config store, message bus, LLM proxy + router, voice channel,
         *   agent_loop worker, outbound dispatch, and (from
         *   voice_boot_test_task) the always-on listen loop.
         *
         * The remaining initialisers (tool registry, tool guard, skills,
         * cron, heartbeat, nsh/web/quickapp, ...) have repeatedly wedged the
         * whole chip within seconds.  A hard wedge takes Wi-Fi down with it:
         * the board drops off the LAN entirely and the panel freezes on
         * NO NET.
         *
         * Until that chain is repaired, stop here and idle.  The assistant
         * stays online and answerable, which is what the demo needs. */

        syslog(LOG_WARNING,
            "[%s] [V42] voice-only mode: skipping the remaining init chain "
            "(it hard-hangs this board)\n", TAG);
        BOOT_LOG(&t0, "V42", "voice-only mode: remaining init skipped");

        for (;;)
          {
            sleep(30);
          }

        /* node_manager must init BEFORE tool_registry so it can
         * register itself as a tool provider. */
#ifdef CONFIG_AI_AGENT_NODE
        /* [MINIMAL] node_manager_init skipped: not needed for voice dialogue and a known
         * hang/failure candidate on this board. */
        rc = OK;
        BOOT_LOG_RC(&t0, "P3", "node_manager_init", rc);
#endif

        /* Skills must init AFTER tool_registry so executable skills
         * can register themselves as tools. */
        /* [MINIMAL] skill_loader_init skipped: not needed for voice dialogue and a known
         * hang/failure candidate on this board. */
        rc = OK;
        BOOT_LOG_RC(&t0, "P3", "skill_loader_init", rc);

        if (!s_voice_brought_up_early) {
            rc = run_init_to("agent_loop_init", agent_loop_init, 12000);
            BOOT_LOG_RC(&t0, "P3", "agent_loop_init", rc);
        }

        /* [MINIMAL] cron_service_init skipped: not needed for voice dialogue and a known
         * hang/failure candidate on this board. */
        rc = OK;
        BOOT_LOG_RC(&t0, "P3", "cron_service_init", rc);

        /* [MINIMAL] heartbeat_init skipped: not needed for voice dialogue and a known
         * hang/failure candidate on this board. */
        rc = OK;
        BOOT_LOG_RC(&t0, "P3", "heartbeat_init", rc);

#ifdef CONFIG_AI_AGENT_NODE
        /* [MINIMAL] node_client_init skipped: not needed for voice dialogue and a known
         * hang/failure candidate on this board. */
        rc = OK;
        BOOT_LOG_RC(&t0, "P3", "node_client_init", rc);
#endif

#ifdef CONFIG_AI_AGENT_MQTT
        /* [MINIMAL] mqtt_channel_init skipped: not needed for voice dialogue and a known
         * hang/failure candidate on this board. */
        rc = OK;
        BOOT_LOG_RC(&t0, "P3", "mqtt_channel_init", rc);
#endif

        if (!s_voice_brought_up_early) {
            rc = run_init_to("voice_channel_init", voice_channel_init, 12000);
            BOOT_LOG_RC(&t0, "P3", "voice_channel_init", rc);
        }

#ifdef CONFIG_AI_AGENT_WEIXIN
        rc = weixin_channel_init();
        BOOT_LOG_RC(&t0, "P3", "weixin_channel_init", rc);
#endif

#ifdef CONFIG_AI_AGENT_LVGL_UI
        rc = lvgl_ui_channel_init();
        BOOT_LOG_RC(&t0, "P3", "lvgl_ui_channel_init", rc);
#endif
    }

    /* ── Phase 4: CLI — register commands only, no thread yet ── */
    {
        int rc = nsh_commands_init();
        BOOT_LOG_RC(&t0, "P4", "nsh_commands_init", rc);
    }

    /* ── Phase 5: Network — async, does NOT block ready ────── */

    /* Kick off WiFi reconnect and start outbound dispatch immediately.
     * Network-dependent services (feishu, agent, ws, node) are started
     * inside the network_watch thread once the link comes up. */

    /* Outbound dispatch thread — start before network so queued messages
     * are drained even during the connection window. */
    if (!s_voice_brought_up_early) {
        if (agent_task_create(outbound_dispatch_task, "outbound",
                AGENT_OUTBOUND_STACK, NULL,
                AGENT_OUTBOUND_PRIO)
            != OK) {
            syslog(LOG_ERR, "[%s] Failed to start outbound dispatch thread\n", TAG);
            return -1;
        }
        BOOT_LOG(&t0, "P5", "outbound dispatch thread started");
    }

#ifdef CONFIG_FEATURE_SYSTEM_VELACLAW
    /* Quickapp mqueue listener - receives requests from quickapp process */
    if (agent_task_create(quickapp_mq_listener_task, "qapp_mq",
                         AGENT_OUTBOUND_STACK, NULL,
                         AGENT_OUTBOUND_PRIO) != OK) {
        syslog(LOG_WARNING, "[%s] Failed to start quickapp mqueue listener\n", TAG);
    }
    BOOT_LOG(&t0, "P5", "quickapp mqueue listener started");
#endif

    /* Cron + heartbeat don't need network */
    cron_service_start();
    heartbeat_start();
    BOOT_LOG(&t0, "P5", "cron + heartbeat started");

#ifdef CONFIG_AI_AGENT_LVGL_UI
    if (lvgl_ui_channel_start() != OK)
        syslog(LOG_WARNING, "[%s] lvgl_ui_channel_start failed\n", TAG);
    BOOT_LOG(&t0, "P5", "lvgl_ui_channel started");
#endif

#ifdef CONFIG_AI_AGENT_BLE_GATT
    /* BLE GATT: ensure adapter enabled, then init with retries */
    {
        extern void ble_cmd_handler_recv(const uint8_t* data, uint16_t len,
            void* user_data);
        bt_instance_t* bt_ins = bluetooth_get_instance();
        if (bt_ins) {
            bt_adapter_state_t state = bt_adapter_get_state(bt_ins);
            if (state < BT_ADAPTER_STATE_BLE_ON) {
                syslog(LOG_INFO, "[%s] BT not ready (state=%d), enabling LE...\n",
                    TAG, (int)state);
                bt_adapter_enable_le(bt_ins);
                sleep(2);
            }
        }

        ble_gatt_config_t ble_cfg = {
            .device_name = "VelaClaw",
            .recv_cb = ble_cmd_handler_recv,
        };
        int rc = -1;
        int attempts = 0;
        while (rc < 0 && attempts < 5) {
            rc = ble_gatt_init(&ble_cfg);
            if (rc < 0) {
                syslog(LOG_WARNING, "[%s] ble_gatt_init attempt %d failed: %d\n",
                    TAG, attempts + 1, rc);
                sleep(3);
            }
            attempts++;
        }
        if (rc == 0) {
            BOOT_LOG(&t0, "P5", "ble_gatt init OK");
        } else {
            BOOT_LOG(&t0, "P5", "ble_gatt FAILED after retries");
        }
    }
#endif

    /* Network watcher thread: reconnect + wait + start net services */
    if (agent_task_create(network_watch_task, "net_watch",
            AGENT_OUTBOUND_STACK, NULL,
            AGENT_OUTBOUND_PRIO)
        != OK) {
        syslog(LOG_WARNING, "[%s] Failed to start network_watch thread\n", TAG);
    }
    BOOT_LOG(&t0, "P5", "network_watch thread started (async)");

    /* ── Phase 6: CLI thread — all services now in known state ── */
    {
        int rc = nsh_commands_start();
        BOOT_LOG_RC(&t0, "P6", "nsh_commands_start", rc);
    }

#if 1  /* [V398] 原为 #ifdef CONFIG_AI_AGENT_VOICE_BOOT_TEST；该宏是外部包符号，nuttx olddefconfig 每次都会丢弃，导致整个自检代码从未编译 */
    if (!s_voice_brought_up_early) {
        if (agent_task_create(voice_boot_test_task, "voice_btest",
                AGENT_OUTBOUND_STACK, NULL, AGENT_OUTBOUND_PRIO) != OK) {
            syslog(LOG_WARNING,
                "[%s] voice boot self-test task creation failed\n", TAG);
        } else {
            BOOT_LOG(&t0, "P6", "voice boot self-test scheduled");
        }
    }
#endif

    syslog(LOG_INFO, "[%s] [boot +%ldms] AI Agent ready. Type 'help' in NSH for commands.\n",
        TAG, boot_ms(&t0));

    /* Block main thread until shutdown is requested */
    while (!g_shutdown_requested) {
        sleep(1);
    }

    syslog(LOG_INFO, "[%s] Shutdown requested — stopping services...\n", TAG);

    /* Wake all threads blocked on message bus before stopping services */
    message_bus_wakeup();

    /* ── Graceful teardown (reverse of startup order) ──────── */

    /* Phase 5 services (network-dependent) */
#ifdef CONFIG_AI_AGENT_WEIXIN
    weixin_channel_stop();
#endif
#ifdef CONFIG_AI_AGENT_NODE
    node_client_stop();
#endif
#ifdef CONFIG_AI_AGENT_MQTT
    mqtt_channel_stop();
#endif
    ws_server_stop();
    /* feishu_bot and agent_loop have no _stop(); they will exit
     * naturally once their blocking I/O returns an error after
     * the network sockets are closed below, or they can check
     * agent_shutdown_requested() in their loops. */

#ifdef CONFIG_AI_AGENT_LVGL_UI
    lvgl_ui_channel_stop();
#endif

    /* Phase 5 services (non-network) */
    cron_service_stop();
    heartbeat_stop();

    /* Give threads a moment to notice and exit */
    usleep(500 * 1000);

    /* Cleanup — reverse order of init */
    tool_media_cleanup();
#ifdef CONFIG_AI_AGENT_MCP
    mcp_bridge_cleanup();
#endif
    tool_guard_cleanup();
    tool_registry_cleanup();
    llm_cache_cleanup();
    vela_tls_pool_cleanup();
    message_bus_destroy();

    syslog(LOG_INFO, "[%s] Shutdown complete.\n", TAG);

    return 0;
}
