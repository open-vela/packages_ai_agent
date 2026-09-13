#
# Copyright (C) 2026 Xiaomi Corporation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

############################################################################
# packages/agent/Makefile
############################################################################

include $(APPDIR)/Make.defs

MODULE = $(CONFIG_EXAMPLES_AI_AGENT_VELA)

# Include paths

CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/include
CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src
CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/tests
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/netutils/cjson/cJSON
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/crypto/mbedtls/mbedtls/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../frameworks/system/topics/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../frameworks/system/vibrator
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../frameworks/multimedia/media/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../frameworks/connectivity/bluetooth/framework/include
ifeq ($(CONFIG_AI_AGENT_AUDIO_ALSA_DIRECT),y)
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/audioutils/alsa-lib/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/vendor/allwinnertech/chips/r528/drivers/rtos-hal/include/hal
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/vendor/allwinnertech/chips/r528/drivers/rtos-hal/include/osal
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source
endif
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/netutils/mqttc/MQTT-C/include

# Source files

# core/ - Agent 核心（ReAct 循环、消息总线、会话管理）
CSRCS += src/core/message_bus.c
CSRCS += src/core/message_bus_tap.c
CSRCS += src/core/agent_loop.c
CSRCS += src/core/agent_trace.c
CSRCS += src/core/context_builder.c
CSRCS += src/core/memory_store.c
CSRCS += src/core/session_mgr.c

# llm/ - LLM 代理层
CSRCS += src/llm/llm_proxy.c
CSRCS += src/llm/llm_parse.c
CSRCS += src/llm/llm_vision.c
CSRCS += src/llm/llm_router.c
CSRCS += src/llm/llm_cache.c

# tools/ - 工具系统（注册表、MCP、安全、技能）
CSRCS += src/tools/tool_registry.c
CSRCS += src/tools/tool_files.c
CSRCS += src/tools/tool_get_time.c
CSRCS += src/tools/tool_web_search.c
CSRCS += src/tools/tool_cron.c
CSRCS += src/tools/tool_fetch_url.c
CSRCS += src/tools/tool_feishu_doc.c
CSRCS += src/tools/tool_feishu_chat.c
CSRCS += src/tools/tool_vision.c
CSRCS += src/tools/tool_shell.c
CSRCS += src/tools/tool_system.c
CSRCS += src/tools/tool_health.c
CSRCS += src/tools/tool_control.c
CSRCS += src/tools/tool_media.c
CSRCS += src/tools/tool_proxyquickapp.c
ifeq ($(CONFIG_AI_AGENT_MCP),y)
CSRCS += src/tools/mcp_bridge.c
CSRCS += src/tools/mcp_server.c
CSRCS += src/tools/mcp_tool_registry.c
CSRCS += src/tools/mcp_builtin_tools.c
CSRCS += src/tools/mcp_client.c
endif
CSRCS += src/tools/skill_loader.c
CSRCS += src/tools/tool_guard.c

ifeq ($(CONFIG_AI_AGENT_SKILL_SYNC),y)
CSRCS += src/tools/skill_sync.c
endif

# channels/ - 所有接入通道
CSRCS += src/channels/nsh_commands.c
CSRCS += src/channels/cmd_llm.c
CSRCS += src/channels/cmd_channel.c
CSRCS += src/channels/cmd_voice.c

ifeq ($(CONFIG_AI_AGENT_FEISHU),y)
CSRCS += src/channels/feishu_bot.c
CSRCS += src/channels/feishu_ws.c
CSRCS += src/channels/feishu_http.c
CSRCS += src/channels/feishu_proto.c
CSRCS += src/channels/feishu_recv.c
CSRCS += src/channels/feishu_send.c
endif

ifeq ($(CONFIG_AI_AGENT_WEIXIN),y)
CSRCS += src/channels/weixin_channel.c
endif

ifeq ($(CONFIG_AI_AGENT_MQTT),y)
CSRCS += src/channels/mqtt_channel.c
endif

CSRCS += src/channels/ws_server.c

# voice/ - 语音管线
CSRCS += src/voice/voice_channel.c
CSRCS += src/voice/voice_tts.c
CSRCS += src/voice/voice_asr.c
CSRCS += src/voice/volc_tts.c
CSRCS += src/voice/volc_tts_ws.c
CSRCS += src/voice/volc_asr.c
CSRCS += src/voice/audio_capture.c
CSRCS += src/voice/audio_playback.c

# infra/ - 基础设施
CSRCS += src/infra/config_store.c
CSRCS += src/infra/cron_service.c
CSRCS += src/infra/heartbeat.c
CSRCS += src/infra/network_manager.c
CSRCS += src/infra/http_proxy.c
CSRCS += src/infra/vela_tls.c

ifeq ($(CONFIG_AI_AGENT_BLE_GATT),y)
CSRCS += src/infra/ble_gatt.c
CSRCS += src/infra/ble_cmd_handler.c
endif

ifeq ($(CONFIG_AI_AGENT_BLE_NET),y)
CSRCS += src/infra/ble_net.c
endif

ifeq ($(CONFIG_AI_AGENT_REST_API),y)
CSRCS += src/infra/api_handler.c
CSRCS += src/infra/agent_logbuf.c
endif

# node/ - 分布式节点
ifeq ($(CONFIG_AI_AGENT_NODE),y)
CSRCS += src/node/node_client.c
CSRCS += src/node/node_manager.c
endif

# stubs
CSRCS += src/stubs.c

# local IPC client (flat-build direct call)
CSRCS += src/sdk/velaclaw_client_local.c

CSRCS += src/ui/qrcode_display.c

ifeq ($(CONFIG_AI_AGENT_LVGL_UI),y)
CSRCS += src/lvgl_ui/lvgl_ui_channel.c
endif

ifeq ($(CONFIG_AI_AGENT_TEST),y)
CSRCS += tests/integs/test_vision_integ.c
endif

MAINSRC = src/agent_main.c

# Force agent_main.c to recompile every build so that
# AGENT_BUILD_VERSION stays fresh.  The $(shell date ...)
# value changes every invocation, invalidating the cached .o.
CFLAGS += -DAGENT_BUILD_TIMESTAMP='"$(shell date "+%Y-%m-%d %H:%M:%S")"'

PROGNAME  = $(CONFIG_EXAMPLES_AI_AGENT_VELA_PROGNAME)
PRIORITY  = $(CONFIG_EXAMPLES_AI_AGENT_VELA_PRIORITY)
STACKSIZE = $(CONFIG_EXAMPLES_AI_AGENT_VELA_STACKSIZE)

include $(APPDIR)/Application.mk
