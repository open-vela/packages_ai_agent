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

#pragma once
/*
 * agent_config.h — AI Agent global configuration (Vela/NuttX version)
 *
 * Build-time secrets are loaded from agent_secrets.h if present.
 * Runtime overrides are stored in the file-based config store
 * (config_store.c) under CONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR.
 */

#include <nuttx/config.h>

/* ── Build version — auto-updated on every compilation ──────── */
/* The build system touches agent_main.c before each build
 * (CMake: add_custom_target touch; Make: -D flag with shell date)
 * so __DATE__/__TIME__ are always fresh.  The Makefile also injects
 * AGENT_BUILD_TIMESTAMP via -D for extra reliability. */
#ifdef AGENT_BUILD_TIMESTAMP
#define AGENT_BUILD_VERSION AGENT_BUILD_TIMESTAMP
#else
#define AGENT_BUILD_VERSION (__DATE__ " " __TIME__)
#endif

#if __has_include("agent_secrets.h")
#include "agent_secrets.h"
#endif

/* Build-time secrets defaults */
#ifndef AGENT_SECRET_API_KEY
#define AGENT_SECRET_API_KEY ""
#endif
#ifndef AGENT_SECRET_MODEL
#define AGENT_SECRET_MODEL ""
#endif
#ifndef AGENT_SECRET_PROXY_HOST
#define AGENT_SECRET_PROXY_HOST ""
#endif
#ifndef AGENT_SECRET_PROXY_PORT
#define AGENT_SECRET_PROXY_PORT ""
#endif
#ifndef AGENT_SECRET_SEARCH_KEY
#define AGENT_SECRET_SEARCH_KEY ""
#endif
#ifndef AGENT_SECRET_SERP_KEY
#define AGENT_SECRET_SERP_KEY ""
#endif
#ifndef AGENT_SECRET_EXA_KEY
#define AGENT_SECRET_EXA_KEY ""
#endif
#ifndef AGENT_SECRET_TAVILY_KEY
#define AGENT_SECRET_TAVILY_KEY ""
#endif
#ifndef AGENT_SECRET_NEWS_KEY
#define AGENT_SECRET_NEWS_KEY ""
#endif
#ifndef AGENT_SECRET_FEISHU_APP_ID
#define AGENT_SECRET_FEISHU_APP_ID ""
#endif
#ifndef AGENT_SECRET_FEISHU_APP_SECRET
#define AGENT_SECRET_FEISHU_APP_SECRET ""
#endif

/* ── Data directories ────────────────────────────────────── */
#ifndef CONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR
#define CONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR "/data/agent"
#endif

#define AGENT_DATA_DIR CONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR
#define AGENT_CONFIG_DIR AGENT_DATA_DIR "/config"
#define AGENT_MEMORY_DIR AGENT_DATA_DIR "/memory"
#define AGENT_SESSION_DIR AGENT_DATA_DIR "/sessions"
#define AGENT_MEMORY_FILE AGENT_DATA_DIR "/memory/MEMORY.md"
#define AGENT_SOUL_FILE AGENT_DATA_DIR "/config/SOUL.md"
#define AGENT_USER_FILE AGENT_DATA_DIR "/config/USER.md"
#define AGENT_CONFIG_FILE AGENT_DATA_DIR "/config/config.json"

/* ── Feishu (Lark) Bot ──────────────────────────────────────── */
#define AGENT_FEISHU_POLL_STACK (20 * 1024) /* WS client needs more stack \
                                                */
#define AGENT_FEISHU_POLL_PRIO 50
#define AGENT_FEISHU_MAX_MSG_LEN 4000 /* Feishu text message limit */

/* ── Agent Loop ─────────────────────────────────────────────── */
#define AGENT_AI_AGENT_STACK (32 * 1024)
#define AGENT_AI_AGENT_PRIO 60
#define AGENT_AI_AGENT_CORE 0
#define AGENT_AI_AGENT_MAX_HISTORY 10
#define AGENT_AI_AGENT_MAX_TOOL_ITER 10
#define AGENT_MAX_TOOL_CALLS 2
#define AGENT_TOOL_NAME_REPEAT_MAX 4

/* ── LLM Watchdog ──────────────────────────────────────────── */
/* Agent-level timeout for a single LLM call (seconds).
 * If llm_chat_tools takes longer than this, the agent treats it as
 * a timeout and replies with a user-friendly error message.
 * The socket-level SO_RCVTIMEO (AGENT_LLM_SOCKET_TIMEOUT_SEC)
 * acts as the hard backstop that actually unblocks the read. */
#define AGENT_LLM_TIMEOUT_SEC 60

/* Socket-level read timeout applied via SO_RCVTIMEO in vela_tls.
 * Must be >= AGENT_LLM_TIMEOUT_SEC to allow the agent-level
 * watchdog to fire first on normal slow responses.  Set higher
 * to cover TLS handshake + full response read. */
#define AGENT_LLM_SOCKET_TIMEOUT_SEC 120

/* ── Timezone (POSIX TZ format) ────────────────────────────── */
#define AGENT_TIMEZONE "CST-8"

/* ── LLM ────────────────────────────────────────────────────── */
#define AGENT_LLM_DEFAULT_MODEL ""
#define AGENT_LLM_MAX_TOKENS 4096
#define AGENT_LLM_MAX_TOKENS_OPENAI 16384
#define AGENT_LLM_API_HOST ""
#define AGENT_LLM_API_PATH "/v1/chat/completions"
#ifndef AGENT_LLM_API_URL
#define AGENT_LLM_API_URL ""
#endif
#define AGENT_LLM_API_VERSION "openai"
#define AGENT_LLM_MAX_RETRIES 3
#define AGENT_LLM_RETRY_BASE_SEC 2
#define AGENT_LLM_STREAM_BUF_SIZE (8 * 1024)
#define AGENT_LLM_MAX_RESP_SIZE \
    (512 * 1024) /* hard cap for growable resp buffer */

/* ── Qwen (Alibaba DashScope) backend constants ─────────────── */
#define AGENT_LLM_QWEN_HOST "dashscope.aliyuncs.com"
#define AGENT_LLM_QWEN_PATH "/compatible-mode/v1/chat/completions"
#define AGENT_LLM_QWEN_MODEL "qwen-turbo"

/* ── OpenRouter (unified multi-model gateway) ───────────────── */
#define AGENT_LLM_OPENROUTER_HOST "openrouter.ai"
#define AGENT_LLM_OPENROUTER_PATH "/api/v1/chat/completions"
#define AGENT_LLM_OPENROUTER_MODEL "qwen/qwen3-coder:free"

/* ── MiMo (Xiaomi MiMo platform) ───────────────────────────── */
#define AGENT_LLM_MIMO_HOST "api.xiaomimimo.com"
#define AGENT_LLM_MIMO_PATH "/v1/chat/completions"
#define AGENT_LLM_MIMO_MODEL "mimo-v2-flash"

/* ── Message Bus ────────────────────────────────────────────── */
#define AGENT_BUS_QUEUE_LEN 16
#define AGENT_BUS_PUSH_TIMEOUT_MS 5000
#define AGENT_OUTBOUND_STACK (16 * 1024)
#define AGENT_OUTBOUND_PRIO 50
#define AGENT_OUTBOUND_CORE 0

/* ── Memory / Context ───────────────────────────────────────── */
#define AGENT_CONTEXT_BUF_SIZE (8 * 1024)

/* ── Cron / Heartbeat ──────────────────────────────────────── */
#define AGENT_CRON_FILE AGENT_DATA_DIR "/cron.json"
#define AGENT_CRON_FILE_MAX_SIZE (8 * 1024)
#define AGENT_CRON_MAX_JOBS 16
#define AGENT_CRON_CHECK_INTERVAL_MS (10 * 1000)
#define AGENT_CRON_ID_LEN 9 /* 8 hex chars + NUL */
#define AGENT_CRON_STACK (8 * 1024)
#define AGENT_CRON_PRIO 40
#define AGENT_HEARTBEAT_FILE AGENT_DATA_DIR "/HEARTBEAT.md"
#define AGENT_HEARTBEAT_INTERVAL_MS (30 * 60 * 1000)

/* ── Skills ─────────────────────────────────────────────────── */
#define AGENT_SKILLS_DIR AGENT_DATA_DIR "/skills/"
#define AGENT_SESSION_MAX_MSGS 20

/* ── Bitable Skill Sync ───────────────────────────────────────── */
#ifdef CONFIG_AI_AGENT_SKILL_SYNC
#define AGENT_SKILL_SYNC_ENABLED 1
#else
#define AGENT_SKILL_SYNC_ENABLED 0
#endif

/* Bitable app_token (multi-dimensional table ID) */
#ifndef CONFIG_AI_AGENT_SKILL_SYNC_APP_TOKEN
#define CONFIG_AI_AGENT_SKILL_SYNC_APP_TOKEN ""
#endif
#define AGENT_SKILL_SYNC_APP_TOKEN CONFIG_AI_AGENT_SKILL_SYNC_APP_TOKEN

/* Bitable table_id */
#ifndef CONFIG_AI_AGENT_SKILL_SYNC_TABLE_ID
#define CONFIG_AI_AGENT_SKILL_SYNC_TABLE_ID ""
#endif
#define AGENT_SKILL_SYNC_TABLE_ID CONFIG_AI_AGENT_SKILL_SYNC_TABLE_ID

/* Sync mode: 0=incremental (version-based), 1=full (rewrite all) */
#ifndef CONFIG_AI_AGENT_SKILL_SYNC_MODE
#define CONFIG_AI_AGENT_SKILL_SYNC_MODE 0
#endif
#define AGENT_SKILL_SYNC_MODE CONFIG_AI_AGENT_SKILL_SYNC_MODE

/* Device type identifier for target_device filtering */
#ifndef CONFIG_AI_AGENT_SKILL_SYNC_DEVICE_TYPE
#define CONFIG_AI_AGENT_SKILL_SYNC_DEVICE_TYPE "all"
#endif
#define AGENT_SKILL_SYNC_DEVICE_TYPE CONFIG_AI_AGENT_SKILL_SYNC_DEVICE_TYPE

/* HTTP response buffer size for Bitable API */
#define AGENT_SKILL_SYNC_RESP_SIZE (64 * 1024)

/* Local version tracking file */
#define AGENT_SKILL_SYNC_VERSION_FILE AGENT_SKILLS_DIR ".versions.json"

/* ── WebSocket Gateway ──────────────────────────────────────── */
#define AGENT_WS_PORT 28789
#define AGENT_WS_MAX_CLIENTS 8

/* ── WS client thread stack ──────────────────────────────────── */
#define AGENT_WS_CLIENT_STACK (16 * 1024)

/* ── Serial CLI ─────────────────────────────────────────────── */
#define AGENT_CLI_STACK (16 * 1024)
#define AGENT_CLI_PRIO 30
#define AGENT_CLI_CORE 0

/* ── Config store keys (config store key strings) ─── */
#define AGENT_CFG_KEY_API_KEY "api_key"
#define AGENT_CFG_KEY_MODEL "model"
#define AGENT_CFG_KEY_PROXY_HOST "proxy_host"
#define AGENT_CFG_KEY_PROXY_PORT "proxy_port"
#define AGENT_CFG_KEY_SEARCH_KEY "search_key"
#define AGENT_CFG_KEY_SERP_KEY "serp_key"
#define AGENT_CFG_KEY_EXA_KEY "exa_key"
#define AGENT_CFG_KEY_TAVILY_KEY "tavily_key"
#define AGENT_CFG_KEY_NEWS_KEY "news_key"
#define AGENT_CFG_KEY_WIFI_SSID "wifi_ssid"
#define AGENT_CFG_KEY_WIFI_PASS "wifi_pass"
#define AGENT_CFG_KEY_FEISHU_APP_ID "feishu_app_id"
#define AGENT_CFG_KEY_FEISHU_APP_SECRET "feishu_app_secret"
#define AGENT_CFG_KEY_FEISHU_USER_TOKEN "feishu_user_token"
#define AGENT_CFG_KEY_LLM_HOST "llm_host"
#define AGENT_CFG_KEY_LLM_PATH "llm_path"
#define AGENT_CFG_KEY_VISION_MODEL "vision_model"
#define AGENT_CFG_KEY_VISION_HOST "vision_host"
#define AGENT_CFG_KEY_VISION_API_KEY "vision_api_key"
#define AGENT_CFG_KEY_GATEWAY_HOST "gateway_host"
#define AGENT_CFG_KEY_GATEWAY_PORT "gateway_port"
#define AGENT_CFG_KEY_GATEWAY_TOKEN "gateway_token"
#define AGENT_CFG_KEY_XIAOZHI_ENDPOINT "xiaozhi_endpoint"
#define AGENT_CFG_KEY_XIAOZHI_CLIENT_ID "xiaozhi_client_id"
#define AGENT_CFG_KEY_XIAOZHI_DEVICE_ID "xiaozhi_device_id"

/* ── Vision / Multimodal ─────────────────────────────────────── */
#define AGENT_VISION_MAX_IMAGE_SIZE \
    (256 * 1024) /* max JPEG file size: 256 KB */
#define AGENT_VISION_MAX_B64_SIZE (350 * 1024) /* base64 ≈ 4/3 × raw */
#define AGENT_VISION_MAX_TOKENS 4096
#define AGENT_VISION_DEFAULT_PROMPT                                      \
    "Please analyze this image in detail. If it contains text, tables, or " \
    "documents, extract and describe the content accurately."
#define AGENT_CAPTURE_PATH AGENT_DATA_DIR "/capture.jpg"
#define AGENT_TEST_IMAGE_PATH AGENT_DATA_DIR "/test.jpg"

/* ── Integration test image directory (Kconfig overridable) ──── */
#ifdef CONFIG_AI_AGENT_TEST_IMAGE_DIR
#define AGENT_TEST_IMAGE_DIR CONFIG_AI_AGENT_TEST_IMAGE_DIR
#else
#define AGENT_TEST_IMAGE_DIR AGENT_DATA_DIR "/test"
#endif

/* ── Node client identity ─────────────────────────────────── */
#ifndef CONFIG_EXAMPLES_AI_AGENT_VELA_NODE_ID
#define CONFIG_EXAMPLES_AI_AGENT_VELA_NODE_ID "watch-01"
#endif
#ifndef CONFIG_EXAMPLES_AI_AGENT_VELA_NODE_DISPLAY_NAME
#define CONFIG_EXAMPLES_AI_AGENT_VELA_NODE_DISPLAY_NAME "Smart Watch"
#endif

#define AGENT_NODE_ID CONFIG_EXAMPLES_AI_AGENT_VELA_NODE_ID
#define AGENT_NODE_DISPLAY_NAME \
    CONFIG_EXAMPLES_AI_AGENT_VELA_NODE_DISPLAY_NAME

/* ── Channel identifiers ────────────────────────────────────── */
#define AGENT_CHAN_WEBSOCKET "websocket"
#define AGENT_CHAN_CLI "cli"
#define AGENT_CHAN_SYSTEM "system"
#define AGENT_CHAN_FEISHU "feishu"
#define AGENT_CHAN_MQTT "mqtt"
#define AGENT_CHAN_WEIXIN "weixin"
#ifdef CONFIG_AI_AGENT_LVGL_UI
#define AGENT_CHAN_LVGL_UI "lvgl_ui"
#endif

/* ── WeChat Channel ─────────────────────────────────────────── */
#define AGENT_WEIXIN_STACK (12 * 1024)
#define AGENT_WEIXIN_PRIO 45

/* ── LVGL UI Channel ───────────────────────────────────────── */
#ifdef CONFIG_AI_AGENT_LVGL_UI
#define AGENT_LVGL_UI_STACK (16 * 1024)
#define AGENT_LVGL_UI_PRIO 45
#endif

/* ── MQTT Channel ───────────────────────────────────────────── */
#define AGENT_MQTT_DEFAULT_PORT 1883
#define AGENT_MQTT_KEEPALIVE 60
#define AGENT_MQTT_BUF_SIZE 1024
#define AGENT_MQTT_STACK (12 * 1024)
#define AGENT_MQTT_PRIO 45
#define AGENT_MQTT_SYNC_INTERVAL_MS 200
#define AGENT_MQTT_TOPIC_IN_DEFAULT "agent/in"
#define AGENT_MQTT_TOPIC_OUT_DEFAULT "agent/out"

#define AGENT_CFG_KEY_MQTT_BROKER "mqtt_broker"
#define AGENT_CFG_KEY_MQTT_CLIENT_ID "mqtt_client_id"
#define AGENT_CFG_KEY_MQTT_TOPIC_IN "mqtt_topic_in"
#define AGENT_CFG_KEY_MQTT_TOPIC_OUT "mqtt_topic_out"
#define AGENT_CFG_KEY_MQTT_USERNAME "mqtt_username"
#define AGENT_CFG_KEY_MQTT_PASSWORD "mqtt_password"

/* ── Voice Channel (Doubao ASR/TTS) ─────────────────────────── */
#define AGENT_CHAN_VOICE "voice"

#define AGENT_VOICE_STACK (16 * 1024)
#define AGENT_VOICE_PRIO 50

/* Doubao TTS V3 API endpoint (HTTP Chunked, x-api-key auth) */
#define AGENT_DOUBAO_TTS_HOST "openspeech.bytedance.com"
#define AGENT_DOUBAO_TTS_PORT "443"
#define AGENT_DOUBAO_TTS_V3_PATH "/api/v3/tts/unidirectional"
#define AGENT_DOUBAO_TTS_RESOURCE "volc.service_type.10029"

/* Doubao streaming ASR WebSocket API (V2) */
#define AGENT_DOUBAO_ASR_HOST "openspeech.bytedance.com"
#define AGENT_DOUBAO_ASR_PORT "443"
#define AGENT_DOUBAO_ASR_WS_PATH "/api/v2/asr"

/* Default ASR cluster (streaming, common Chinese) */
#define AGENT_DOUBAO_ASR_CLUSTER "volcengine_streaming_common"

/* Audio device paths (platform-specific) */
#ifndef AGENT_AUDIO_CAPTURE_DEV
#define AGENT_AUDIO_CAPTURE_DEV "/dev/audio/pcm0c"
#endif
#ifndef AGENT_AUDIO_PLAYBACK_DEV
#define AGENT_AUDIO_PLAYBACK_DEV "/dev/audio/pcm0p"
#endif

/* Audio chunk size for streaming: 3200 bytes = 100ms at 16kHz/16bit/mono */
#define AGENT_ASR_CHUNK_SIZE 3200

/* Audio parameters: PCM 16-bit signed LE, 16kHz, mono */
#define AGENT_VOICE_SAMPLE_RATE 16000
#define AGENT_VOICE_CHANNELS 1
#define AGENT_VOICE_BITS 16
#define AGENT_VOICE_FRAME_MS 20

/* Buffer sizes — keep small to avoid OOM during concurrent agent+voice.
 * 128KB PCM ≈ 4s at 16kHz/16bit/mono; enough for typical TTS output.
 * Recording uses the same cap; 128KB ≈ 4s of audio which is fine for
 * short voice commands.  For longer recordings, increase as needed. */
#define AGENT_VOICE_PCM_BUF_SIZE (128 * 1024)
#define AGENT_VOICE_RESP_BUF_SIZE (128 * 1024)
#define AGENT_VOICE_B64_BUF_SIZE (96 * 1024)

/* Config store keys */
#define AGENT_CFG_KEY_VOLC_APPKEY "volc_appkey"
#define AGENT_CFG_KEY_VOLC_TOKEN "volc_token"
#define AGENT_CFG_KEY_VOLC_API_KEY "volc_api_key"
#define AGENT_CFG_KEY_VOLC_SPEAKER "volc_speaker"
#define AGENT_CFG_KEY_VOLC_CLUSTER "volc_cluster"
#define AGENT_CFG_KEY_VOLC_ASR_CLUSTER "volc_asr_cluster"

/* Default TTS speaker and cluster */
#define AGENT_VOICE_DEFAULT_SPEAKER "zh_male_beijingxiaoye_emo_v2_mars_bigtts"
#define AGENT_VOICE_DEFAULT_CLUSTER "volcano_tts"

/* WebSocket TTS output sample rate (big-model voices output 24kHz) */
#define AGENT_TTS_WS_SAMPLE_RATE 24000

/* ── Tool Guard (security) ──────────────────────────────────── */
#define AGENT_TOOL_MAX_INPUT_LEN (32 * 1024)
#define AGENT_TOOL_RATE_LIMIT_WINDOW_SEC 60
#define AGENT_TOOL_RATE_LIMIT_MAX_CALLS 10

/* ── Shell security policy ──────────────────────────────────── */
#define AGENT_SHELL_SECURITY_ALLOWLIST 0 /* Whitelist mode (production) */
#define AGENT_SHELL_SECURITY_FULL 1 /* Full access (development/skills) */
#define AGENT_SHELL_SECURITY_DENY 2 /* Disabled */

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA_SHELL_ALLOWLIST
#define AGENT_SHELL_SECURITY AGENT_SHELL_SECURITY_ALLOWLIST
#elif defined(CONFIG_EXAMPLES_AI_AGENT_VELA_SHELL_FULL)
#define AGENT_SHELL_SECURITY AGENT_SHELL_SECURITY_FULL
#elif defined(CONFIG_EXAMPLES_AI_AGENT_VELA_SHELL_DENY)
#define AGENT_SHELL_SECURITY AGENT_SHELL_SECURITY_DENY
#else
#define AGENT_SHELL_SECURITY AGENT_SHELL_SECURITY_ALLOWLIST /* default \
                                                                   */
#endif
