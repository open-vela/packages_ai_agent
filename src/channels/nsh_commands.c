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

#include "channels/nsh_commands.h"
#include "channels/cmd_channel.h"
#include "channels/cmd_llm.h"
#include "channels/cmd_voice.h"
#include "core/message_bus.h"
#include "infra/config_store.h"
#include "infra/cron_service.h"
#include "infra/heartbeat.h"
#include "llm/llm_proxy.h"
#ifdef CONFIG_AI_AGENT_MCP
#include "tools/mcp_client.h"
#endif
#include "core/memory_store.h"
#include "core/session_mgr.h"
#include "infra/network_manager.h"
#include "infra/http_proxy.h"
#include "infra/vela_tls.h"
#include "tools/tool_get_time.h"
#include "tools/tool_proxyquickapp.h"
#include "tools/tool_registry.h"
#include "tools/tool_web_search.h"
#include "agent_compat.h"
#include "agent_config.h"

#if AGENT_SKILL_SYNC_ENABLED
#include "tools/skill_sync.h"
#endif

#ifdef CONFIG_AI_AGENT_LVGL_UI
#include "ui/lvgl_ui_channel.h"
#endif

#ifdef CONFIG_AI_AGENT_TEST
#include "integs/test_vision_integ.h"
#include "core/arena_alloc.h"
#endif

#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef CONFIG_BOARDCTL_RESET
#include <sys/boardctl.h>
#endif

static const char* TAG = "cli";

#define MAX_ARGS 8
#define LINE_LEN 256

/* ── Helpers ──────────────────────────────────────────────────── */

static int tokenise(char* line, char** argv, int max_argc)
{
    int argc = 0;
    char* tok = strtok(line, " \t\r\n");
    while (tok && argc < max_argc) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t\r\n");
    }
    return argc;
}

static void cmd_help(void)
{
    printf(
        "Available commands:\n"
        "  net_status           - Show network connection status\n"
        "  net_test             - Test HTTPS connection to Baidu\n"
        "  set_feishu_app <app_id> <app_secret>  - Set Feishu app credentials\n"
        "  set_feishu_user_token <token>  - Set Feishu user_access_token for doc APIs\n"
        "  set_llm <preset|host> [model] [key] - Switch LLM backend (kimi/qwen/deepseek/glm/openai)\n"
        "  set_vision_llm <preset|host> [model] [key] - Set independent vision model\n"
        "  list_models [--free] [keyword] - List available models (openrouter)\n"
        "  memory_read          - Read MEMORY.md\n"
        "  memory_write <text>  - Write MEMORY.md (quote text)\n"
        "  session_list         - List sessions\n"
        "  session_clear <id>   - Clear a session\n"
        "  session_clear_all    - Clear all sessions\n"
        "  heap_info            - Show memory usage\n"
        "  set_proxy <host> <port> - Set HTTP proxy\n"
        "  clear_proxy          - Remove proxy config\n"
        "  set_wifi <ssid> <pw> - Connect to WiFi (real hardware; saved for reboot)\n"
        "  wifi_reconnect       - Re-join WiFi using saved credentials\n"
        "  set_search_key <key> - Set SerpAPI (Google) key\n"
        "  set_exa_key <key>    - Set Exa AI search key\n"
        "  set_tavily_key <key> - Set Tavily search key\n"
        "  set_news_key <key>   - Set NewsAPI key\n"
        "  set_tavily_key <key> - Set Tavily AI search key\n"
        "  config_show          - Show current configuration\n"
        "  config_reset         - Clear all runtime config\n"
        "  ask <text>           - Chat with AI Agent\n"
        "  heartbeat_trigger    - Manually trigger heartbeat check\n"
        "  cron_start           - Start cron scheduler\n"
#ifdef CONFIG_AI_AGENT_NODE
        "  node_list            - List connected remote Nodes\n"
        "  set_gateway <host> [port] [token] - Set OpenClaw Gateway\n"
        "  node_start          - Connect to OpenClaw Gateway as Node\n"
        "  node_stop           - Disconnect from OpenClaw Gateway\n"
#endif
        "  restart              - Restart the device\n"
        "  quit                 - Exit agent\n"
        "  set_mqtt <broker> [client_id] - Set MQTT broker (host:port)\n"
        "  set_volc_key <api_key>       - Set Doubao voice API key\n"
        "  set_volc_asr <id> <tok> <cluster> - Set ASR credentials\n"
        "  set_volc_speaker <id>  - Set TTS voice (e.g. zh_female_cancan)\n"
        "  voice_start            - Start voice channel\n"
        "  voice_stop             - Stop voice channel\n"
        "  voice_test_tts <text> [out.pcm] - Test TTS synthesis\n"
        "  voice_test_asr <file>  - Test ASR recognition\n"
        "  set_voice_tts <name>   - Switch TTS backend\n"
        "  set_voice_asr <name>   - Switch ASR backend\n"
        "  set_weixin_token <tok> - Set WeChat bot token\n"
        "  weixin_login           - QR code login to WeChat\n"
#ifdef CONFIG_AI_AGENT_XIAOZHI
        "  set_xiaozhi <ota_url>  - Set XiaoZhi OTA endpoint URL\n"
#endif
        "  router_status          - Show LLM router status\n"
        "  router_set <preset> <key> - Add LLM backend (deepseek/kimi/qwen/openai...)\n"
        "  router_model <idx> <model> - Change model for a backend\n"
        "  router_profile <p>     - Set routing profile (eco/auto/premium)\n"
        "  router_clear [idx]     - Clear router backends\n"
        "  launch_app <pkg>     - Test launch a QuickApp by package name\n"
        "  exit_app             - Exit current QuickApp and go home\n"
        "  install_skill <name> <url|-> - Install skill from URL or stdin\n"
#if AGENT_SKILL_SYNC_ENABLED
        "  skill_sync            - Sync skills from Feishu Bitable\n"
#endif
#ifdef CONFIG_AI_AGENT_MCP
        "  mcp_add <name> <url> [token] - Add remote MCP server\n"
        "  mcp_remove <name>     - Remove remote MCP server\n"
        "  mcp_discover          - Discover tools from remote servers\n"
        "  mcp_status            - Show MCP client status\n"
        "  mcp_tools             - List remote MCP tools\n"
#endif
#ifdef CONFIG_AI_AGENT_LVGL_UI
        "  show_chat            - Show chat UI on screen\n"
#endif
#ifdef CONFIG_AI_AGENT_NET_RPMSG
        "  net_diag [ping|http] <target>  Network diagnostics\n"
        "  net_reconnect                  Reconnect network\n"
        "  set_netproxy <mode> [cpu]      Set network proxy mode\n"
        "  set_dns <primary> [secondary]  Set DNS servers\n"
#endif
#ifdef CONFIG_AI_AGENT_TEST
        "  claw_test [V-XX]    - Run vision integration tests\n"
#endif
        "  help                 - This message\n");
}

/* ── Command implementations ──────────────────────────────────── */

static void cmd_net_test(void)
{
    char resp[1024];
    printf("Testing HTTPS to www.baidu.com...\n");
    int status = vela_https_get("www.baidu.com", "443", "/", resp, sizeof(resp));
    if (status > 0) {
        printf("SUCCESS! HTTP Status: %d\n", status);
    } else {
        printf("FAILED! TLS Error: 0x%x\n", -status);
    }
}

static void cmd_net_status(void)
{
    printf("Network connected: %s\n", network_is_connected() ? "yes" : "no");
    printf("IP: %s\n", network_get_ip());
#ifdef CONFIG_AI_AGENT_NET_RPMSG
    printf("State: %s\n",
        network_get_state() == NET_STATE_CONNECTED ? "CONNECTED" : "DISCONNECTED");
    printf("Active TCP conns: %d/%d\n",
        network_get_active_conns(), NET_MAX_TCP_CONNS);
    printf("IOB usage: %d%%\n", network_get_iob_usage());
    printf("Proxy mode: %s\n", network_get_proxy_mode());
    printf("RPMSG CPU: %s\n", network_get_rpmsg_cpu());
    /* Show DNS from resolv.conf */
    {
        FILE* fp = fopen("/tmp/resolv.conf", "r");
        if (fp) {
            char line[128];
            while (fgets(line, sizeof(line), fp)) {
                line[strcspn(line, "\n")] = '\0';
                printf("DNS: %s\n", line);
            }
            fclose(fp);
        }
    }
#endif
}

#ifdef CONFIG_AI_AGENT_NET_RPMSG
static void cmd_net_diag(int argc, char** argv)
{
    if (argc < 2) {
        /* Basic diagnostics */
        printf("=== Network Diagnostics ===\n");
        printf("Interface: %s\n",
            network_is_connected() ? "UP" : "DOWN");
        printf("IP: %s\n", network_get_ip());
        printf("State: %s\n",
            network_get_state() == NET_STATE_CONNECTED
                ? "CONNECTED"
                : "DISCONNECTED");
        printf("IOB: %d%%\n", network_get_iob_usage());
        printf("===========================\n");
        return;
    }

    if (strcmp(argv[1], "ping") == 0 && argc >= 3) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "ping -c 3 %s", argv[2]);
        system(cmd);
    } else if (strcmp(argv[1], "http") == 0 && argc >= 3) {
        printf("HTTP HEAD %s ...\n", argv[2]);
        /* Simple connectivity test using existing TLS layer */
        char resp[256] = { 0 };
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int status = vela_https_get(argv[2], "443", "/",
            resp, sizeof(resp));
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ms = (t1.tv_sec - t0.tv_sec) * 1000L
            + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
        printf("Status: %d, Latency: %ldms\n", status, ms);
    } else {
        printf("Usage: net_diag [ping <host> | http <host>]\n");
    }
}
#endif

static void cmd_memory_read(void)
{
    char* buf = malloc(4096);
    if (!buf) {
        printf("Out of memory.\n");
        return;
    }
    if (memory_read_long_term(buf, 4096) == OK && buf[0]) {
        printf("=== MEMORY.md ===\n%s\n=================\n", buf);
    } else {
        printf("MEMORY.md is empty or not found.\n");
    }
    free(buf);
}

static void cmd_memory_write(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: memory_write <content>\n");
        return;
    }
    memory_write_long_term(argv[1]);
    printf("MEMORY.md updated.\n");
}

static void cmd_session_list(void)
{
    printf("Sessions:\n");
    session_list();
}

static void cmd_session_clear(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: session_clear <chat_id> | session_clear_all\n");
        return;
    }
    if (session_clear(argv[1]) == OK) {
        printf("Session cleared.\n");
    } else {
        printf("Session not found.\n");
    }
}

static void cmd_session_clear_all(void)
{
    session_clear_all();
    printf("All sessions cleared.\n");
}

static void cmd_heap_info(void)
{
    struct mallinfo mi = mallinfo();
    printf("Heap: arena=%d fordblks(free)=%d uordblks(used)=%d\n",
        mi.arena, mi.fordblks, mi.uordblks);
}

static void cmd_set_proxy(int argc, char** argv)
{
    if (argc < 3) {
        printf("Usage: set_proxy <host> <port>\n");
        return;
    }
    uint16_t port = (uint16_t)atoi(argv[2]);
    http_proxy_set(argv[1], port);
    printf("Proxy set to %s:%d.\n", argv[1], port);
}

static void cmd_clear_proxy(void)
{
    http_proxy_clear();
    printf("Proxy cleared. Restart to apply.\n");
}

static void cmd_set_wifi(int argc, char** argv)
{
    if (argc < 3) {
        printf("Usage: set_wifi <ssid> <password>\n");
        return;
    }
    printf("Connecting to '%s' ...\n", argv[1]);
    int err = network_wifi_connect(NULL, argv[1], argv[2]);
    if (err == OK)
        printf("WiFi connected: %s\n", network_get_ip());
    else
        printf("WiFi failed. Check SSID/password or run net_status.\n");
}

static void cmd_wifi_reconnect(void)
{
    printf("Reconnecting WiFi ...\n");
    int err = network_wifi_reconnect();
    if (err == OK)
        printf("Reconnected: %s\n", network_get_ip());
    else if (err == ERROR)
        printf("No saved credentials. Use: set_wifi <ssid> <pass>\n");
    else
        printf("WiFi reconnect failed.\n");
}

#ifdef CONFIG_AI_AGENT_NET_RPMSG
static void cmd_net_reconnect(void)
{
    printf("Reconnecting...\n");
    int ret = network_reconnect();
    printf("Reconnect %s\n", ret == OK ? "triggered" : "failed");
}
#endif

#ifdef CONFIG_AI_AGENT_NET_RPMSG
static void cmd_set_netproxy(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: set_netproxy <usrsock|tun> [cpu_name]\n");
        return;
    }
    const char* cpu = (argc >= 3) ? argv[2] : NULL;
    int ret = network_save_proxy_config(argv[1], cpu);
    if (ret == OK) {
        printf("Proxy config saved: mode=%s cpu=%s\n",
            argv[1], cpu ? cpu : "(default)");
    } else {
        printf("Failed to save proxy config: %d\n", ret);
    }
}
#endif

#ifdef CONFIG_AI_AGENT_NET_RPMSG
static void cmd_set_dns(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: set_dns <primary> [secondary]\n");
        return;
    }
    const char* secondary = (argc >= 3) ? argv[2] : NULL;
    int ret = network_set_dns(argv[1], secondary);
    if (ret == OK) {
        printf("DNS configured: %s %s\n",
            argv[1], secondary ? secondary : "");
    } else {
        printf("Failed to set DNS: %d\n", ret);
    }
}
#endif

static void cmd_set_search_key(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: set_search_key <key>\n");
        return;
    }
    tool_web_search_set_serp_key(argv[1]);
    printf("SerpAPI (Google) key saved.\n");
}

static void cmd_set_exa_key(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: set_exa_key <key>\n");
        return;
    }
    tool_web_search_set_exa_key(argv[1]);
    printf("Exa AI key saved.\n");
}

static void cmd_set_tavily_key(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: set_tavily_key <key>\n");
        return;
    }
    tool_web_search_set_tavily_key(argv[1]);
    printf("Tavily key saved.\n");
}

static void cmd_set_news_key(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: set_news_key <key>\n");
        return;
    }
    tool_web_search_set_news_key(argv[1]);
    printf("NewsAPI key saved.\n");
}

static void cmd_config_show(void)
{
    char val[128] = { 0 };

    printf("=== Current Configuration ===\n");

#define SHOW_CFG(label, key, mask)                                        \
    do {                                                                  \
        memset(val, 0, sizeof(val));                                      \
        if (claw_config_get((key), val, sizeof(val)) != OK || !val[0])    \
            strcpy(val, "(not set)");                                     \
        if ((mask) && strlen(val) > 6 && strcmp(val, "(not set)") != 0) { \
            char masked[128];                                             \
            snprintf(masked, sizeof(masked), "%.4s****", val);            \
            printf("  %-14s: %s\n", (label), masked);                     \
        } else {                                                          \
            printf("  %-14s: %s\n", (label), val);                        \
        }                                                                 \
    } while (0)

    SHOW_CFG("Feishu AppID", AGENT_CFG_KEY_FEISHU_APP_ID, false);
    SHOW_CFG("Feishu Secret", AGENT_CFG_KEY_FEISHU_APP_SECRET, true);
    SHOW_CFG("API Key", AGENT_CFG_KEY_API_KEY, true);
    SHOW_CFG("Model", AGENT_CFG_KEY_MODEL, false);
    SHOW_CFG("LLM Host", AGENT_CFG_KEY_LLM_HOST, false);
    SHOW_CFG("LLM Path", AGENT_CFG_KEY_LLM_PATH, false);
    SHOW_CFG("Vision Model", AGENT_CFG_KEY_VISION_MODEL, false);
    SHOW_CFG("Vision Host", AGENT_CFG_KEY_VISION_HOST, false);
    SHOW_CFG("Vision Key", AGENT_CFG_KEY_VISION_API_KEY, true);
    SHOW_CFG("Proxy Host", AGENT_CFG_KEY_PROXY_HOST, false);
    SHOW_CFG("Proxy Port", AGENT_CFG_KEY_PROXY_PORT, false);
    SHOW_CFG("SerpAPI Key", AGENT_CFG_KEY_SERP_KEY, true);
    SHOW_CFG("Exa Key", AGENT_CFG_KEY_EXA_KEY, true);
    SHOW_CFG("Tavily Key", AGENT_CFG_KEY_TAVILY_KEY, true);
    SHOW_CFG("News Key", AGENT_CFG_KEY_NEWS_KEY, true);
    SHOW_CFG("Tavily Key", AGENT_CFG_KEY_TAVILY_KEY, true);
    SHOW_CFG("Gateway", AGENT_CFG_KEY_GATEWAY_HOST, false);
    SHOW_CFG("GW Port", AGENT_CFG_KEY_GATEWAY_PORT, false);
    SHOW_CFG("GW Token", AGENT_CFG_KEY_GATEWAY_TOKEN, true);
    SHOW_CFG("MQTT Broker", AGENT_CFG_KEY_MQTT_BROKER, false);
    SHOW_CFG("Volc AppKey", AGENT_CFG_KEY_VOLC_APPKEY, true);
    SHOW_CFG("Volc Token", AGENT_CFG_KEY_VOLC_TOKEN, true);
    SHOW_CFG("Volc API Key", AGENT_CFG_KEY_VOLC_API_KEY, true);
    SHOW_CFG("Volc Speaker", AGENT_CFG_KEY_VOLC_SPEAKER, false);

#undef SHOW_CFG

    printf("Network: %s / %s\n",
        network_is_connected() ? "connected" : "disconnected",
        network_get_ip());
#ifdef CONFIG_AI_AGENT_NET_RPMSG
    printf("Net Proxy: %s (CPU: %s)\n",
        network_get_proxy_mode(), network_get_rpmsg_cpu());
    printf("Net Timeout: connect=%ds read=%ds\n",
        network_get_connect_timeout(), network_get_read_timeout());
    printf("Net Retry: max=%d base=%ds\n",
        network_get_retry_max(), network_get_retry_base_sec());
#endif
    printf("=============================\n");
}

static void cmd_config_reset(void)
{
    config_erase_all();
    printf("All runtime config cleared. Build-time defaults will be used on restart.\n");
}

static void cmd_restart(void)
{
    printf("Restarting...\n");
    fflush(stdout);
#ifdef CONFIG_BOARDCTL_RESET
    boardctl(BOARDIOC_RESET, 0);
#else
    /* Last resort: just loop forever (caller should call reboot()) */
    while (1)
        sleep(1);
#endif
}

static void cmd_heartbeat_trigger(void)
{
    if (heartbeat_trigger()) {
        printf("Heartbeat: triggered agent check.\n");
    } else {
        printf("Heartbeat: no actionable tasks found.\n");
    }
}

static void cmd_ask(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: ask <message>\n");
        return;
    }

    /* Concatenate all arguments as the message */
    char content[256] = { 0 };
    for (int i = 1; i < argc; i++) {
        strncat(content, argv[i], sizeof(content) - strlen(content) - 1);
        if (i < argc - 1)
            strncat(content, " ", sizeof(content) - strlen(content) - 1);
    }

    agent_msg_t msg = { 0 };
    strncpy(msg.channel, "cli", sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, "console", sizeof(msg.chat_id) - 1);
    msg.content = strdup(content);
    if (msg.content)
        message_bus_push_inbound(&msg);
    printf("Sent to agent: %s\n", content);
    syslog(LOG_INFO, "[agent] ask: %s\n", content);
}

static void cmd_cron_start(void)
{
    if (cron_service_start() == OK) {
        printf("Cron scheduler started.\n");
    } else {
        printf("Cron scheduler already running or failed to start.\n");
    }
}



/* ── list_models helpers ──────────────────────────────────────── */


#ifdef CONFIG_AI_AGENT_TEST
static void cmd_claw_test(int argc, char** argv)
{
    const char* filter = argc >= 2 ? argv[1] : NULL;

    /* Arena allocator test */
    if (!filter || strcmp(filter, "arena") == 0) {
        printf("=== Arena Allocator Test ===\n");
        arena_t a;
        int rc = arena_init(&a, 4096);
        printf("  init(4096): %s\n", rc == 0 ? "OK" : "FAIL");
        if (rc == 0) {
            void* p1 = arena_alloc(&a, 128);
            void* p2 = arena_alloc(&a, 256);
            void* p3 = arena_alloc(&a, 512);
            printf("  alloc 128+256+512: p1=%p p2=%p p3=%p\n",
                p1, p2, p3);
            printf("  used=%zu remaining=%zu\n",
                arena_used(&a), arena_remaining(&a));

            /* Verify no overlap */
            bool ok = p1 && p2 && p3
                && (char*)p2 >= (char*)p1 + 128
                && (char*)p3 >= (char*)p2 + 256;
            printf("  no-overlap: %s\n", ok ? "OK" : "FAIL");

            /* Test reset */
            arena_reset(&a);
            printf("  reset: used=%zu (expect 0)\n", arena_used(&a));

            /* Test overflow */
            void* big = arena_alloc(&a, 8192);
            printf("  overflow(8192): %s (expect NULL)\n",
                big == NULL ? "OK" : "FAIL");

            arena_destroy(&a);
            printf("  destroy: OK\n");
            printf("Arena test: PASSED\n\n");
        }
        if (filter) return;
    }

    int rc = test_vision_integ_run(filter);
    printf("Test suite %s\n", rc == 0 ? "PASSED" : "FAILED");
}
#endif

static void cmd_quit(void)
{
    printf("Exiting agent...\n");
    fflush(stdout);
    agent_request_shutdown();
}

static void cmd_launch_app(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: launch_app <package_name>\n");
        return;
    }

    char input[256];
    snprintf(input, sizeof(input), "{\"package_name\":\"%s\"}", argv[1]);

    char output[512];
    int ret = tool_launch_quickapp_execute(input, output, sizeof(output));
    printf("Result(%d): %s\n", ret, output);
}

static void cmd_exit_app(void)
{
    char output[512];
    int ret = tool_exit_quickapp_execute("{}", output, sizeof(output));
    printf("Result(%d): %s\n", ret, output);
}

/* ── Router commands ──────────────────────────────────────────── */




/* ── mcp_add: add a remote MCP server ─────────────────────── */

#ifdef CONFIG_AI_AGENT_MCP
static void cmd_mcp_add(int argc, char** argv)
{
    if (argc < 3) {
        printf("Usage: mcp_add <name> <url> [token]\n"
               "  name:  server name (e.g. amap)\n"
               "  url:   MCP endpoint (e.g. http://host:port/mcp)\n"
               "  token: optional bearer token\n");
        return;
    }

    const char* token = (argc >= 4) ? argv[3] : NULL;
    int ret = mcp_client_add_server(argv[1], argv[2], token);
    if (ret == OK) {
        printf("Added MCP server: %s → %s\n", argv[1], argv[2]);
    } else {
        printf("Failed to add MCP server\n");
    }
}

/* ── mcp_remove: remove a remote MCP server ───────────────── */

static void cmd_mcp_remove(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: mcp_remove <name>\n");
        return;
    }

    int ret = mcp_client_remove_server(argv[1]);
    if (ret == OK) {
        printf("Removed MCP server: %s\n", argv[1]);
    } else {
        printf("Server not found: %s\n", argv[1]);
    }
}

/* ── mcp_discover: discover tools from all servers ────────── */

static void cmd_mcp_discover(void)
{
    printf("Discovering remote MCP tools...\n");
    int n = mcp_client_discover();
    if (n >= 0) {
        printf("Discovered %d remote tools\n", n);
        /* Rebuild tools JSON so agent sees new tools */
        tool_registry_invalidate();
    } else {
        printf("Discovery failed\n");
    }
}

/* ── mcp_status: show MCP client status ───────────────────── */

static void cmd_mcp_status(void)
{
    char* json = mcp_client_status_json();
    if (json) {
        printf("%s\n", json);
        free(json);
    } else {
        printf("No MCP client data\n");
    }
}

/* ── mcp_tools: list remote tools ─────────────────────────── */

static void cmd_mcp_tools(void)
{
    char* json = mcp_client_get_tools_json();
    if (json) {
        printf("%s\n", json);
        free(json);
    } else {
        printf("No remote tools (run mcp_discover first)\n");
    }
}
#endif /* CONFIG_AI_AGENT_MCP */

/* ── install_skill: install a skill from URL ──────────────── */

static void cmd_install_skill(int argc, char** argv)
{
    if (argc < 3) {
        printf("Usage: install_skill <name> <url>\n"
               "  name: skill filename (without .md)\n"
               "  url:  HTTPS URL to download skill markdown\n");
        return;
    }

    const char* name = argv[1];
    const char* url = argv[2];

    /* Validate name (lowercase + digits + hyphens) */
    for (const char* p = name; *p; p++) {
        if (!(*p >= 'a' && *p <= 'z') && !(*p >= '0' && *p <= '9')
            && *p != '-' && *p != '_') {
            printf("Invalid skill name: use a-z, 0-9, hyphens\n");
            return;
        }
    }

    /* Validate URL starts with https:// */
    if (strncmp(url, "https://", 8) != 0) {
        printf("Only HTTPS URLs are allowed\n");
        return;
    }

    char path[128];
    int n = snprintf(path, sizeof(path), "%s%s.md",
        AGENT_SKILLS_DIR, name);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        printf("Skill name too long\n");
        return;
    }

    char* buf = malloc(8192);
    if (!buf) {
        printf("OOM\n");
        return;
    }

    memset(buf, 0, 8192);
    int rc = vela_https_get(url, "443", NULL, buf, 8192);
    if (rc < 200 || rc >= 300) {
        printf("Download failed: HTTP %d\n", rc);
        free(buf);
        return;
    }

    /* Validate content looks like a markdown skill file */
    if (buf[0] == '\0') {
        printf("Downloaded content is empty\n");
        free(buf);
        return;
    }

    /* Basic content validation: must start with # or --- (frontmatter) */
    const char* p = buf;
    while (*p == ' ' || *p == '\n' || *p == '\r')
        p++;
    if (*p != '#' && strncmp(p, "---", 3) != 0) {
        printf("Invalid skill format: must start with # or ---\n");
        free(buf);
        return;
    }

    /* Size sanity check */
    size_t content_len = strlen(buf);
    if (content_len < 10) {
        printf("Downloaded content too small (%zu bytes)\n", content_len);
        free(buf);
        return;
    }

    FILE* f = fopen(path, "w");
    if (!f) {
        printf("Cannot write: %s\n", path);
        free(buf);
        return;
    }

    fputs(buf, f);
    fclose(f);
    free(buf);
    printf("Skill installed: %s (%zu bytes)\n", path, content_len);
}

#if AGENT_SKILL_SYNC_ENABLED
static void cmd_skill_sync(void)
{
    printf("Syncing skills from Bitable...\n");
    int rc = skill_sync_from_bitable();
    if (rc == OK) {
        printf("Skill sync completed successfully.\n");
    } else {
        printf("Skill sync failed (check syslog for details).\n");
    }
}
#endif

#ifdef CONFIG_AI_AGENT_XIAOZHI
static void cmd_set_xiaozhi(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: set_xiaozhi <ota_url>\n"
               "  Example: set_xiaozhi https://api.tenclass.net/xiaozhi/ota/\n");
        return;
    }

    claw_config_set(AGENT_CFG_KEY_XIAOZHI_ENDPOINT, argv[1]);
    printf("XiaoZhi endpoint set to: %s\n", argv[1]);
    printf("Restart ai_agent to connect.\n");
}
#endif

/* ── CLI thread ───────────────────────────────────────────────── */

static void* cli_thread(void* arg)
{
    (void)arg;
    char line[LINE_LEN];
    char* argv[MAX_ARGS];

    syslog(LOG_INFO, "[%s] NSH CLI started. Type 'help' for commands.\n", TAG);
    pthread_mutex_lock(&g_stdout_lock);
    printf("vela> ");
    fflush(stdout);
    pthread_mutex_unlock(&g_stdout_lock);

    while (fgets(line, sizeof(line), stdin) != NULL) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (len == 0) {
            pthread_mutex_lock(&g_stdout_lock);
            printf("vela> ");
            fflush(stdout);
            pthread_mutex_unlock(&g_stdout_lock);
            continue;
        }

        int argc = tokenise(line, argv, MAX_ARGS);
        if (argc == 0) {
            pthread_mutex_lock(&g_stdout_lock);
            printf("vela> ");
            fflush(stdout);
            pthread_mutex_unlock(&g_stdout_lock);
            continue;
        }

        const char* cmd = argv[0];

        if (strcmp(cmd, "help") == 0)
            cmd_help();
        else if (strcmp(cmd, "net_status") == 0)
            cmd_net_status();
        else if (strcmp(cmd, "net_test") == 0)
            cmd_net_test();
        else if (strcmp(cmd, "set_feishu_app") == 0)
            cmd_set_feishu_app(argc, argv);
        else if (strcmp(cmd, "set_feishu_user_token") == 0)
            cmd_set_feishu_user_token(argc, argv);
        else if (strcmp(cmd, "set_llm") == 0)
            cmd_set_llm(argc, argv);
        else if (strcmp(cmd, "set_vision_llm") == 0)
            cmd_set_vision_llm(argc, argv);
        else if (strcmp(cmd, "list_models") == 0)
            cmd_list_models(argc, argv);
        else if (strcmp(cmd, "memory_read") == 0)
            cmd_memory_read();
        else if (strcmp(cmd, "memory_write") == 0)
            cmd_memory_write(argc, argv);
        else if (strcmp(cmd, "session_list") == 0)
            cmd_session_list();
        else if (strcmp(cmd, "session_clear") == 0)
            cmd_session_clear(argc, argv);
        else if (strcmp(cmd, "session_clear_all") == 0)
            cmd_session_clear_all();
        else if (strcmp(cmd, "heap_info") == 0)
            cmd_heap_info();
        else if (strcmp(cmd, "set_proxy") == 0)
            cmd_set_proxy(argc, argv);
        else if (strcmp(cmd, "clear_proxy") == 0)
            cmd_clear_proxy();
        else if (strcmp(cmd, "set_wifi") == 0)
            cmd_set_wifi(argc, argv);
        else if (strcmp(cmd, "wifi_reconnect") == 0)
            cmd_wifi_reconnect();
#ifdef CONFIG_AI_AGENT_NET_RPMSG
        else if (strcmp(cmd, "net_diag") == 0)
            cmd_net_diag(argc, argv);
        else if (strcmp(cmd, "net_reconnect") == 0)
            cmd_net_reconnect();
        else if (strcmp(cmd, "set_netproxy") == 0)
            cmd_set_netproxy(argc, argv);
        else if (strcmp(cmd, "set_dns") == 0)
            cmd_set_dns(argc, argv);
#endif
        else if (strcmp(cmd, "set_search_key") == 0)
            cmd_set_search_key(argc, argv);
        else if (strcmp(cmd, "set_exa_key") == 0)
            cmd_set_exa_key(argc, argv);
        else if (strcmp(cmd, "set_news_key") == 0)
            cmd_set_news_key(argc, argv);
        else if (strcmp(cmd, "set_tavily_key") == 0)
            cmd_set_tavily_key(argc, argv);
        else if (strcmp(cmd, "config_show") == 0)
            cmd_config_show();
        else if (strcmp(cmd, "config_reset") == 0)
            cmd_config_reset();
        else if (strcmp(cmd, "heartbeat_trigger") == 0)
            cmd_heartbeat_trigger();
        else if (strcmp(cmd, "ask") == 0)
            cmd_ask(argc, argv);
        else if (strcmp(cmd, "cron_start") == 0)
            cmd_cron_start();
#ifdef CONFIG_AI_AGENT_NODE
        else if (strcmp(cmd, "node_list") == 0)
            cmd_node_list();
        else if (strcmp(cmd, "set_gateway") == 0)
            cmd_set_gateway(argc, argv);
        else if (strcmp(cmd, "node_start") == 0)
            cmd_node_start();
        else if (strcmp(cmd, "node_stop") == 0)
            cmd_node_stop();
#endif
        else if (strcmp(cmd, "quit") == 0) {
            cmd_quit();
            break;
        } else if (strcmp(cmd, "set_mqtt") == 0)
            cmd_set_mqtt(argc, argv);
        else if (strcmp(cmd, "set_volc_key") == 0)
            cmd_set_volc_key(argc, argv);
        else if (strcmp(cmd, "set_volc_asr") == 0)
            cmd_set_volc_asr(argc, argv);
        else if (strcmp(cmd, "set_volc_speaker") == 0)
            cmd_set_volc_speaker(argc, argv);
        else if (strcmp(cmd, "voice_start") == 0)
            cmd_voice_start();
        else if (strcmp(cmd, "voice_stop") == 0)
            cmd_voice_stop();
        else if (strcmp(cmd, "voice_test_tts") == 0)
            cmd_voice_test_tts(argc, argv);
        else if (strcmp(cmd, "voice_test_asr") == 0)
            cmd_voice_test_asr(argc, argv);
        else if (strcmp(cmd, "set_voice_tts") == 0)
            cmd_set_voice_tts(argc, argv);
        else if (strcmp(cmd, "set_voice_asr") == 0)
            cmd_set_voice_asr(argc, argv);
        else if (strcmp(cmd, "set_weixin_token") == 0)
            cmd_set_weixin_token(argc, argv);
        else if (strcmp(cmd, "weixin_login") == 0)
            cmd_weixin_login();
#ifdef CONFIG_AI_AGENT_XIAOZHI
        else if (strcmp(cmd, "set_xiaozhi") == 0)
            cmd_set_xiaozhi(argc, argv);
#endif
        else if (strcmp(cmd, "launch_app") == 0)
            cmd_launch_app(argc, argv);
        else if (strcmp(cmd, "exit_app") == 0)
            cmd_exit_app();
        else if (strcmp(cmd, "install_skill") == 0)
            cmd_install_skill(argc, argv);
#if AGENT_SKILL_SYNC_ENABLED
        else if (strcmp(cmd, "skill_sync") == 0)
            cmd_skill_sync();
#endif
#ifdef CONFIG_AI_AGENT_MCP
        else if (strcmp(cmd, "mcp_add") == 0)
            cmd_mcp_add(argc, argv);
        else if (strcmp(cmd, "mcp_remove") == 0)
            cmd_mcp_remove(argc, argv);
        else if (strcmp(cmd, "mcp_discover") == 0)
            cmd_mcp_discover();
        else if (strcmp(cmd, "mcp_status") == 0)
            cmd_mcp_status();
        else if (strcmp(cmd, "mcp_tools") == 0)
            cmd_mcp_tools();
#endif
#ifdef CONFIG_AI_AGENT_LVGL_UI
        else if (strcmp(cmd, "show_chat") == 0)
            lvgl_ui_channel_show();
#endif
#ifdef CONFIG_AI_AGENT_TEST
        else if (strcmp(cmd, "claw_test") == 0)
            cmd_claw_test(argc, argv);
#endif
        else if (strcmp(cmd, "router_status") == 0)
            cmd_router_status();
        else if (strcmp(cmd, "router_set") == 0)
            cmd_router_set(argc, argv);
        else if (strcmp(cmd, "router_model") == 0)
            cmd_router_model(argc, argv);
        else if (strcmp(cmd, "router_profile") == 0)
            cmd_router_profile(argc, argv);
        else if (strcmp(cmd, "router_clear") == 0)
            cmd_router_clear(argc, argv);
        else if (strcmp(cmd, "restart") == 0)
            cmd_restart();
        else
            printf("Unknown command: %s (type 'help')\n", cmd);

        pthread_mutex_lock(&g_stdout_lock);
        printf("vela> ");
        fflush(stdout);
        pthread_mutex_unlock(&g_stdout_lock);
    }

    syslog(LOG_INFO, "[%s] CLI thread exiting\n", TAG);
    return NULL;
}

/* ── Init / Start ─────────────────────────────────────────────── */

int nsh_commands_init(void)
{
    /* Pure registration — no thread spawned here.
     * Commands are statically defined; nothing to do at the moment.
     * Kept as a hook for future dynamic command registration. */
    syslog(LOG_INFO, "NSH commands registered");
    return OK;
}

int nsh_commands_start(void)
{
    return agent_task_create(cli_thread, "agent_cli", AGENT_CLI_STACK, NULL, AGENT_CLI_PRIO);
}
