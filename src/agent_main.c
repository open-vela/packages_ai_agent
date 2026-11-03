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
#include <unistd.h>

#include "agent_compat.h"
#include "agent_config.h"

#include "core/agent_loop.h"
#include "core/message_bus.h"
#include "core/message_bus_tap.h"
#include "channels/nsh_commands.h"
#include "infra/config_store.h"
#include "infra/time_sync.h"
#include "infra/cron_service.h"
#ifdef CONFIG_AI_AGENT_FEISHU
#include "channels/feishu_bot.h"
#endif
#include "channels/ws_server.h"
#include "infra/heartbeat.h"
#include "infra/hangwatch.h"
#include "llm/llm_proxy.h"
#include "llm/llm_router.h"
#include "core/memory_store.h"
#include "core/session_mgr.h"
#include "health/mic_selfcheck.h" /* 开机麦克风自检探针 */
#include "ui/persona.h"           /* 人格状态：开机只读一次 /data/persona */
#include "voice/demo_voice_cache.h" /* DEMO-VOICE-A：固定演示台词预合成音频缓存 */
#ifdef CONFIG_AI_AGENT_MQTT
#include "channels/mqtt_channel.h"
#endif
#include "infra/network_manager.h"
#ifdef CONFIG_AI_AGENT_NODE
#include "node/node_client.h"
#include "node/node_manager.h"
#endif
#include "infra/http_proxy.h"
#include "infra/weather_cache.h"    /* 进程级共享天气缓存（预取/天气页/天气工具共用） */
#include "infra/weather_prefetch.h" /* 天气后台预取：30 分钟一轮，不阻塞启动 */
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
#include "ui/health_summary.h"
#include "health/rest_track.h"
#ifdef CONFIG_INPUT_BUTTONS
#include "ui/key_input.h"
#endif
#endif
#ifdef CONFIG_AI_AGENT_BLE_GATT
#include "infra/ble_cmd_handler.h"
#include "infra/ble_gatt.h"
#endif
#include <bluetooth.h>
#include <bt_adapter.h>

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

/* ── Network watcher (async) ──────────────────────────────────── */

#ifdef CONFIG_AI_AGENT_NET_RPMSG
static volatile bool g_net_services_started = false;

static void net_state_change_cb(net_state_t state, void* arg)
{
    (void)arg;
    if (state == NET_STATE_CONNECTED && !g_net_services_started) {
        syslog(LOG_INFO, "[%s] Network recovered, starting services\n", TAG);
        time_sync_start();
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

/* ── Classic-BT local name override ──────────────────────────────
 * The PAN service names the adapter "<prefix>-<MAC>" on every adapter-on
 * event (panu_service.c: pan_set_local_name_with_mac), which shows up in
 * the phone's pairing list as a long pseudo-address. Override it app-side
 * with a product name: wait for the adapter to reach ON, give the PAN
 * handler a moment to finish, then write our name and read it back.
 * One-shot task - it exits once done, so there is no resident cost.
 * If BT is restarted, PAN re-applies its name; re-running this task
 * (or a reboot) restores ours. */
#define AGENT_BT_LOCAL_NAME  "小云手表"

static void* bt_name_task(void* arg)
{
    bt_instance_t* ins;
    char cur[64] = { 0 };
    int i;

    (void)arg;

    ins = bluetooth_get_instance();
    if (ins == NULL)
    {
        syslog(LOG_WARNING, "[%s] bt_name: no bt instance, skipped\n", TAG);
        return NULL;
    }

    for (i = 0; i < 60; i++)
    {
        if (bt_adapter_get_state(ins) == BT_ADAPTER_STATE_ON)
        {
            break;
        }
        sleep(1);
    }
    if (i >= 60)
    {
        syslog(LOG_WARNING, "[%s] bt_name: adapter never reached ON\n", TAG);
        return NULL;
    }

    /* Let PAN's own adapter-on handler land first, otherwise it would
     * overwrite us a moment later. */
    sleep(3);

    if (bt_adapter_set_name(ins, AGENT_BT_LOCAL_NAME) != BT_STATUS_SUCCESS)
    {
        syslog(LOG_WARNING, "[%s] bt_name: set_name failed\n", TAG);
        return NULL;
    }

    bt_adapter_get_name(ins, cur, sizeof(cur));
    syslog(LOG_INFO, "[%s] bt_name: local name now \"%s\"\n", TAG, cur);
    return NULL;
}

/**
 * 2026-11-02 BLE GATT 数据通道启动任务。
 *
 * 不能在 P5 里同步调 ble_gatt_init：开机时 bluetoothd 可能还没把
 * adapter 拉到 BLE_ON，同步调用会直接失败、且没有第二次机会。
 * 这里照抄 bt_name_task 的做法：先等 adapter 到 BLE_ON（最多 60s），
 * 再重试 ble_gatt_init（最多 5 次，间隔 3s）。
 *
 * 只在 CONFIG_AI_AGENT_BLE_GATT && !CONFIG_AI_AGENT_BLE_NET 时编译/启动，
 * 失败只打日志，不影响语音/bt-pan 主链路。
 */
#if defined(CONFIG_AI_AGENT_BLE_GATT) && !defined(CONFIG_AI_AGENT_BLE_NET)
static void* ble_gatt_boot_task(void* arg)
{
    ble_gatt_config_t cfg;
    bt_instance_t* ins;
    int i;
    int rc = -1;

    (void)arg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.device_name = "Agent-Watch";
    cfg.recv_cb = ble_cmd_handler_recv;
    cfg.conn_cb = NULL;
    cfg.user_data = NULL;

    /* 等适配器就绪（bluetoothd 是异步起来的） */
    for (i = 0; i < 60; i++) {
        ins = bluetooth_get_instance();
        if (ins != NULL && bt_adapter_get_state(ins) >= BT_ADAPTER_STATE_BLE_ON) {
            break;
        }
        sleep(1);
    }

    ins = bluetooth_get_instance();
    if (ins == NULL) {
        syslog(LOG_WARNING, "[%s] ble_gatt: no bt instance after wait, skipped\n", TAG);
        return NULL;
    }

    if (bt_adapter_get_state(ins) < BT_ADAPTER_STATE_BLE_ON) {
        syslog(LOG_WARNING, "[%s] ble_gatt: enabling LE adapter\n", TAG);
        bt_adapter_enable_le(ins);
        sleep(3);
    }

    for (i = 0; i < 5; i++) {
        rc = ble_gatt_init(&cfg);
        if (rc == 0) {
            break;
        }
        syslog(LOG_WARNING, "[%s] ble_gatt_init failed (%d), retry %d/5\n",
            TAG, rc, i + 1);
        sleep(3);
    }

    if (rc != 0) {
        syslog(LOG_ERR, "[%s] ble_gatt: channel unavailable (%d), "
            "BLE data channel disabled; voice/bt-pan unaffected\n", TAG, rc);
    } else {
        syslog(LOG_INFO, "[%s] ble_gatt: data channel ready (NUS, name=%s)\n",
            TAG, cfg.device_name);
    }
    return NULL;
}
#endif

/**
 * Runs in a background thread: waits for network, then starts all
 * network-dependent services. Main thread is NOT blocked.
 */
static void* network_watch_task(void* arg)
{
    (void)arg;
    syslog(LOG_INFO, "[%s] Network watcher started\n", TAG);

    network_wifi_reconnect();

    if (network_wait_connected(30000) == OK) {
        syslog(LOG_INFO, "[%s] Network connected: %s\n", TAG, network_get_ip());

        time_sync_start();

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
        syslog(LOG_WARNING, "[%s] Network timeout — net services not started.\n", TAG);
        syslog(LOG_WARNING, "[%s] Use 'config_show' / 'set_*' CLI commands to configure.\n", TAG);
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
#ifdef CONFIG_AI_AGENT_LVGL_UI
/* Mirror an answer to the watch UI, tagging on-device answers so the pet page
 * itself shows which half of the 端云 pair answered - without a serial log the
 * routing is otherwise invisible. */
static void ui_mirror_answer(const char* text, bool from_local)
{
    if (from_local) {
        char tagged[768];
        snprintf(tagged, sizeof(tagged), "%s（本地）", text);
        lvgl_ui_channel_log(tagged, false);
    } else {
        lvgl_ui_channel_log(text, false);
    }
}
#endif

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
            ws_server_send(msg.chat_id, msg.content, msg.from_local);
#ifdef CONFIG_AI_AGENT_SPEAK_WS_REPLIES
            /* 手机 App 提问 -> 手表把回答念出来。用异步版本: 派发线程绝不阻塞。
             * P2a: 只念最终回答 (kind=FINAL)。状态语("正在思考中...")与错误语
             * 也会走这条 outbound 通道，以前它们同样被念出来 —— 现在按 kind
             * 过滤，源头上保证只有答案出声。 */
            if (msg.kind == AGENT_MSG_KIND_FINAL) {
                extern int voice_channel_speak_async(const char* text);
                if (voice_channel_speak_async(msg.content) != 0) {
                    syslog(LOG_WARNING, "[%s] speak(ws) dispatch failed\n", TAG);
                }
            } else {
                syslog(LOG_INFO,
                    "[%s] speak(ws) skipped: kind=%u (not a final answer)\n",
                    TAG, (unsigned)msg.kind);
            }
#endif
#ifdef CONFIG_AI_AGENT_LVGL_UI
            /* 手机侧对话同样上手表小云：写进历史环与桌宠页文本层，
             * 用户在桌面时还会被自动带到桌宠页（与 cli 通道一致） */
            ui_mirror_answer(msg.content, msg.from_local);
#endif
#ifdef CONFIG_AI_AGENT_MQTT
        } else if (strcmp(msg.channel, AGENT_CHAN_MQTT) == 0) {
            mqtt_channel_send(msg.chat_id, msg.content);
#endif
        } else if (strcmp(msg.channel, AGENT_CHAN_VOICE) == 0) {
            if (msg.content == NULL || msg.content[0] == '\0') {
                /* v108 P0-4：空回复防呆（工具空输出 / 空 ASR 结果都会被过滤，
                 * 避免气泡空白 + speak 返回 -EINVAL） */
                syslog(LOG_WARNING, "[%s] empty voice answer, skip\n", TAG);
                voice_channel_turn_clear();
            } else {
#ifdef CONFIG_AI_AGENT_LVGL_UI
                /* v106 S2：先上屏/入历史，再同步播报。 */
                ui_mirror_answer(msg.content, msg.from_local);
#endif
                /* v114 F6（B 报告）：语音回答改为异步播报。旧代码在 outbound
                 * 线程里同步 voice_channel_speak()，单条实测 10995ms，期间所有
                 * 出站消息（“正在思考中”/错误提示）全部排队，用户体感就是
                 * “说完界面不动”。异步后由 speak_worker 播完再清回合门禁。 */
                /* 2026-11-03：分句 TTS 实验有队列回归，暂回退整段异步播报。 */
                /* DEMO-VOICE-A（2026-11-03）：固定演示台词优先播固件内预合成
                 * PCM（<1s 出声、不占每 boot 5 次云端 TTS 额度、离线可用）；
                 * 未命中或 voice 正忙时回落原 TTS，行为不变。 */
                if (demo_voice_try_play(msg.content) == 0) {
                    syslog(LOG_INFO,
                        "[%s] [demo-voice] cached line queued, TTS skipped\n",
                        TAG);
                } else {
                    /* 未命中：先确保本地缓存不占着音频设备，再回落云端 TTS。 */
                    demo_voice_stop();

                    int vret = voice_channel_speak_async(msg.content);

                    if (vret != 0) {
                        syslog(LOG_ERR, "[%s] voice speak failed: %d\n",
                            TAG, vret);
                    }
                }
            }

#ifdef CONFIG_AI_AGENT_LVGL_UI
        } else if (strcmp(msg.channel, AGENT_CHAN_LVGL_UI) == 0) {
            /* P2a: 把消息类别一起交给 UI 通道 —— 气泡照旧显示(状态语/错误语
             * 都上屏)，但只有 FINAL 才触发 TTS。显示与播报解耦。 */
            int uret = lvgl_ui_channel_send_kind(msg.content, msg.kind);
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
            /* 2026-11-03 P0（v125）：cli 回合**不念**，只 voice 通道 TTS。
             * 原 P2e 为了"演示/验收"让控制台 ask 的回答也出声，但实测一个
             * ask 回合的 TTS 会把整机拖进 speak 队列 / TTS 回退链，且
             * agent_loop 的回合门禁又因它多等（perf-net log:2024、1744）。
             * ws 与 care 仍由各自分支播报（演示路径/产品功能），
             * 这里仅去掉 cli。屏幕/历史镜像在下面 ui_mirror_answer 保留。 */
#ifdef CONFIG_AI_AGENT_LVGL_UI
            /* Mirror into the chat ring so the pet page history window shows
             * console conversations too; this channel renders no bubble. */
            ui_mirror_answer(msg.content, msg.from_local);
#endif
        } else if (strcmp(msg.channel, "care") == 0) {
            /* pet_care L2 follow-up: rendered as a second bubble on the pet
             * stage (L1 template already went out first).
             *
             * ★ 必须把 msg.kind 传下去 —— 这里原来调 lvgl_ui_channel_send()
             * 走默认 FINAL, 于是 care 通道的**状态语**("让我查一下...")和
             * **错误语**都被当成最终回答, 不仅上屏还会进 TTS (违 P2a)。
             * 2026-10-25 现场实测: [voice] speak: "让我查一下..."。 */
            syslog(LOG_INFO, "[agent] [Care]: %s\n", msg.content);
#ifdef CONFIG_AI_AGENT_LVGL_UI
            lvgl_ui_channel_send_kind(msg.content, msg.kind);
#endif
            /* 2026-11-01: 主动关怀 L2 的**最终回答**也念出来（L1 模板气泡
             * 仍保持静音，避免突然说话打扰）。用异步版本，不阻塞派发线程。 */
            if (msg.kind == AGENT_MSG_KIND_FINAL) {
                extern int voice_channel_speak_async(const char* text);
                if (voice_channel_speak_async(msg.content) != 0) {
                    syslog(LOG_WARNING, "[%s] speak(care) dispatch failed\n", TAG);
                }
            }
        } else if (strcmp(msg.channel, "ble") == 0) {
            /* 手机 App 从 BLE NUS 发起的对话（docs_ble/98 号文 FW-A）。
             * 回包是 evt=chat，同时把同一条回答写进手表历史环/桌宠页文本层，
             * 让屏幕和手机看到同一段对话。 */
#ifdef CONFIG_AI_AGENT_BLE_GATT
            /* 进度语（"正在思考中..."）**不回给 App**：App 的聊天列表只认
             * evt=chat 的最终回答，把进度语当成回答插进去就是一条假消息；
             * 不回包时 App 只是在等（它自己的 120s 超时兜底）。 */
            if (msg.kind != AGENT_MSG_KIND_STATUS) {
                ble_send_chat_reply(msg.content, msg.from_local);
            }
#endif
#ifdef CONFIG_AI_AGENT_LVGL_UI
            ui_mirror_answer(msg.content, msg.from_local);
#endif
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

#ifdef CONFIG_AI_AGENT_LVGL_UI
/* v114b2：开机 20s 后做一次 LCD 真掉电 reinit。
 * 依据 A/C 报告：复位后面板侧会卡黑（readid/contrast 正常、lcd_flush 成功），
 * 只有 lcdtool reinit 的 VADD 掉电 1.5s 能恢复。这里无条件做一次，保证显示。 */
static void* lcd_boot_recover(void* arg)
{
    extern int sf32lb_lcd_hw_reinit(void);
    (void)arg;
    sleep(20);
    int r = sf32lb_lcd_hw_reinit();
    syslog(LOG_INFO, "[%s] lcd_boot_recover: reinit ret=%d\n", TAG, r);
    return NULL;
}
#endif

int ai_agent_main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    g_shutdown_requested = false;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    syslog(LOG_INFO, "[%s] AI Agent - Vela AI Agent starting (build: %s)\n",
        TAG, AGENT_BUILD_VERSION);

    /* ── Phase 0: Timezone ──────────────────────────────────── */
    /* Set TZ before any time calls so localtime_r returns CST+8.
     * NuttX with CONFIG_LIBC_LOCALTIME+CONFIG_LIBC_ZONEINFO supports
     * both POSIX ("CST-8") and zoneinfo ("Asia/Shanghai") formats. */
    setenv("TZ", AGENT_TIMEZONE, 1);
#ifdef CONFIG_LIBC_LOCALTIME
    tzset();
#endif
    BOOT_LOG(&t0, "P0", "timezone set");

    /* v114b：boot 计数（RTC 备份寄存器，跨 IWDT 复位保留、掉电归零） */
    {
        extern uint32_t HAL_Get_backup(uint8_t idx);
        extern void HAL_Set_backup(uint8_t idx, uint32_t value);
        uint32_t n = HAL_Get_backup(5) + 1;
        HAL_Set_backup(5, n);
        syslog(LOG_INFO, "[boot] n=%u\n", (unsigned)n);
    }

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

    /* 人格: /data 就绪后读一次 /data/persona（只读，不写）。读到什么用什么，
     * 读不到就用默认「温柔」；后续每轮 system prompt 复用同一份内存态，
     * 不再碰文件。写盘只发生在用户主动切换时，见 ui/persona.c。 */
    (void)persona_get();
    BOOT_LOG(&t0, "P0", "persona loaded");

    /* Memory info */
    {
        struct mallinfo mi = mallinfo();
        syslog(LOG_INFO, "[%s] [boot +%ldms] heap: arena=%d free=%d used=%d\n",
            TAG, boot_ms(&t0), mi.arena, mi.fordblks, mi.uordblks);
    }

    /* ── Phase 1: Core infrastructure ──────────────────────── */
    {
        int rc;

        /* 板载音频设备开机自注册 (采集 pcm0c / 播放 pcm0p)。实现在参赛仓
         * app/ai_agent/audio/ 里, 与 audio_setup 命令共享同一份代码;
         * 重复注册只返回错误, 不影响启动 —— 这样产品镜像不再依赖手工
         * 执行 audio_setup, 语音链路(采集/播放/TTS)开机即可用。 */
#if defined(CONFIG_EXAMPLES_AUDIO_SETUP) || defined(CONFIG_EXAMPLES_AUDIO_SETUP_TX)
        extern int sf32lb52_audcodec_register(void);
        extern int sf32lb52_audio_tx_register(void);
#  ifdef CONFIG_EXAMPLES_AUDIO_SETUP
        rc = sf32lb52_audcodec_register();
        BOOT_LOG_RC(&t0, "P1", "audcodec_register(capture)", rc);
#  endif
#  ifdef CONFIG_EXAMPLES_AUDIO_SETUP_TX
        rc = sf32lb52_audio_tx_register();
        BOOT_LOG_RC(&t0, "P1", "audio_tx_register(playback)", rc);
#  endif
#endif

#ifdef CONFIG_AI_AGENT_BOOT_CHIME
        /* 开机三声短音: 不依赖串口控制台的"喇叭自检" (实现见 voice/boot_chime.c) */
        {
            extern int agent_boot_chime(void);
            (void)agent_boot_chime();
        }
#endif

        rc = config_store_init();
        BOOT_LOG_RC(&t0, "P1", "config_store_init", rc);

        /* 开机麦克风自检 (异步): 录 3 秒 PCM 并打出 "麦克风自检：RMS ... 结论：..."
         * 这一行, 于是每次开机都有一条 mic 结论, 不依赖串口能不能敲
         * audio_rec_test, 也不依赖 ask。worker 线程里跑, 不堵启动。
         *
         * 位置要求两件事都已就绪: audcodec_register 跑过 (上面 P1 开头,
         * 否则 open 得自己补注册) 且 config_store_init 建过 AGENT_DATA_DIR
         * (否则 PCM 落盘路径的目录还不存在, 只能拿到统计、拿不到文件)。 */
        /* v109：暂时关闭开机麦克风探针 —— v108 打开后与 P0-3 一起触发
         * 启动期采集挂死/看门狗复位循环（每 ~40s 重启一次）。等 P0-3 的
         * 安全重初始化方案验证后再恢复。ask 路径（手动麦克风自检）不受影响。 */
        /* v114：明确标注开机探针未启用，不再打印假的 mic_selfcheck rc=0 */
        rc = 0; (void)rc;
        BOOT_LOG_RC(&t0, "P1", "mic_probe_disabled", rc);

        rc = message_bus_init();
        BOOT_LOG_RC(&t0, "P1", "message_bus_init", rc);
        if (rc != OK)
            return -1;

        rc = memory_store_init();
        BOOT_LOG_RC(&t0, "P1", "memory_store_init", rc);

        rc = session_mgr_init();
        BOOT_LOG_RC(&t0, "P1", "session_mgr_init", rc);
    }

    /* ── Phase 2: Proxy / networking ───────────────────────── */
    {
        int rc = http_proxy_init();
        BOOT_LOG_RC(&t0, "P2", "http_proxy_init", rc);
    }

    /* ── Phase 3: Application services ─────────────────────── */
    {
        int rc;
#ifdef CONFIG_AI_AGENT_FEISHU
        rc = feishu_bot_init();
        BOOT_LOG_RC(&t0, "P3", "feishu_bot_init", rc);
#endif

        rc = llm_proxy_init();
        BOOT_LOG_RC(&t0, "P3", "llm_proxy_init", rc);

        rc = llm_router_init();
        BOOT_LOG_RC(&t0, "P3", "llm_router_init", rc);

        /* node_manager must init BEFORE tool_registry so it can
         * register itself as a tool provider. */
#ifdef CONFIG_AI_AGENT_NODE
        rc = node_manager_init();
        BOOT_LOG_RC(&t0, "P3", "node_manager_init", rc);
#endif

        rc = tool_registry_init();
        BOOT_LOG_RC(&t0, "P3", "tool_registry_init", rc);

        rc = tool_guard_init();
        BOOT_LOG_RC(&t0, "P3", "tool_guard_init", rc);

        /* Skills must init AFTER tool_registry so executable skills
         * can register themselves as tools. */
        rc = skill_loader_init();
        BOOT_LOG_RC(&t0, "P3", "skill_loader_init", rc);

        rc = agent_loop_init();
        BOOT_LOG_RC(&t0, "P3", "agent_loop_init", rc);

        rc = cron_service_init();
        BOOT_LOG_RC(&t0, "P3", "cron_service_init", rc);

        rc = heartbeat_init();
        BOOT_LOG_RC(&t0, "P3", "heartbeat_init", rc);

#ifdef CONFIG_AI_AGENT_NODE
        rc = node_client_init();
        BOOT_LOG_RC(&t0, "P3", "node_client_init", rc);
#endif

#ifdef CONFIG_AI_AGENT_MQTT
        rc = mqtt_channel_init();
        BOOT_LOG_RC(&t0, "P3", "mqtt_channel_init", rc);
#endif

        rc = voice_channel_init();
        BOOT_LOG_RC(&t0, "P3", "voice_channel_init", rc);

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
    if (agent_task_create(outbound_dispatch_task, "outbound",
            AGENT_OUTBOUND_STACK, NULL,
            AGENT_OUTBOUND_PRIO)
        != OK) {
        syslog(LOG_ERR, "[%s] Failed to start outbound dispatch thread\n", TAG);
        return -1;
    }
    BOOT_LOG(&t0, "P5", "outbound dispatch thread started");

    /* Agent loop — started here, not only from the network-up callback.  It is
     * the only consumer of the inbound queue, so leaving it to the network
     * meant that with no link (or no API key) nothing ever drained a query and
     * the on-device model was unreachable.  agent_loop_start() is idempotent,
     * so the later network-up call is a no-op. */
    if (agent_loop_start() != OK) {
        syslog(LOG_WARNING, "[%s] agent_loop_start failed\n", TAG);
    }
    BOOT_LOG(&t0, "P5", "agent loop started");

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
#ifdef CONFIG_INPUT_BUTTONS
    /* Physical keys: Key1 opens the pet page, Key2 asks the on-device model
     * one of the demo questions.  Started after the UI so the first press
     * always finds the launcher built. */
    if (key_input_start() != OK)
        syslog(LOG_WARNING, "[%s] key_input_start failed\n", TAG);
    BOOT_LOG(&t0, "P5", "key input started");
#endif
#endif

#ifdef CONFIG_AI_AGENT_BLE_GATT
    /* BLE GATT：先把适配器拉到 LE ON，再自己注册 NUS 数据通道。
     * 2026-11-02：本配置不开 BLE_NET（TUN 网络代理），因此这个实例由
     * 这里持有；ble_cmd_handler 提供 {"cmd":"health"} 等 JSON 命令。 */
    {
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
    }

#ifndef CONFIG_AI_AGENT_BLE_NET
    /* 后台任务等 bluetoothd 就绪后再起 NUS 通道（详见 ble_gatt_boot_task）。 */
    if (agent_task_create(ble_gatt_boot_task, "ble_gatt", 4096, NULL,
            AGENT_OUTBOUND_PRIO)
        != OK) {
        syslog(LOG_WARNING, "[%s] Failed to start ble_gatt thread\n", TAG);
    }
#endif
#endif

    /* Network watcher thread: reconnect + wait + start net services */
    if (agent_task_create(network_watch_task, "net_watch",
            AGENT_OUTBOUND_STACK, NULL,
            AGENT_OUTBOUND_PRIO)
        != OK) {
        syslog(LOG_WARNING, "[%s] Failed to start network_watch thread\n", TAG);
    }
    BOOT_LOG(&t0, "P5", "network_watch thread started (async)");

    /* ── 天气后台预取（网络相关，但不阻塞启动）───────────────
     * 线程自己等网络：每 5 s 探一次、一轮最多 120 s，等不到就下一轮；
     * 起来以后每 30 分钟拉一次 Open-Meteo 写进共享缓存。于是天气页
     * 打开时多数情况直接命中缓存，一次 TLS 往返都不发（"打开即最新"），
     * 语音问天气时 get_weather 也先吃到这份数据。
     *
     * 位置：放在 network_watch 之后 —— 它并不依赖那个线程，只是同属
     * "网络起来才有意义"的一段；weather_prefetch_start() 只创建线程就
     * 返回，即使完全没有网络也只是它自己在线程里等，启动路径不受影响。
     * 共享缓存先 init：三条写路径（预取/天气页/get_weather 工具）都用它。 */
    weather_cache_init();
    {
        int rc = weather_prefetch_start();
        BOOT_LOG_RC(&t0, "P5", "weather_prefetch_start", rc);
    }

#ifdef CONFIG_AI_AGENT_LVGL_UI
    /* ── 休息时段记录 + 每日健康摘要 ──────────────────────────
     * 两者都依赖 pedometer / 心率历史 / 天气缓存 / UI 通道，放在 P5 末尾；
     * 都只创建线程就返回，启动路径不受影响。
     * 注意：rest_track 不能只靠睡眠页启动 —— 用户不开睡眠页时它也要记录，
     * 否则"昨夜休息"永远没有数据。 */
    {
        int rc = rest_track_start();
        BOOT_LOG_RC(&t0, "P5", "rest_track_start", rc);
    }
    {
        int rc = health_summary_start();
        BOOT_LOG_RC(&t0, "P5", "health_summary_start", rc);
    }
#endif

    /* One-shot: rename the adapter once BT is up (see bt_name_task). */
    if (agent_task_create(bt_name_task, "bt_name", 4096, NULL,
            AGENT_OUTBOUND_PRIO)
        != OK) {
        syslog(LOG_WARNING, "[%s] Failed to start bt_name thread\n", TAG);
    }

    /* ── Phase 6: CLI thread — all services now in known state ── */
    {
        int rc = nsh_commands_start();
        BOOT_LOG_RC(&t0, "P6", "nsh_commands_start", rc);
    }

    /* ── Phase 7: 冻屏取证看门狗(独立线程, 只观察不抢锁) ── */
    {
        int rc = hangwatch_start();
        BOOT_LOG_RC(&t0, "P7", "hangwatch_start", rc);
    }

#ifdef CONFIG_AI_AGENT_LVGL_UI
    /* v114b2：20s 后自动 LCD 恢复 */
    {
        pthread_t tid;
        if (pthread_create(&tid, NULL, lcd_boot_recover, NULL) == 0) {
            pthread_detach(tid);
            syslog(LOG_INFO, "[%s] lcd boot recovery scheduled (+20s)\n", TAG);
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
