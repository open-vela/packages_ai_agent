################################################################################
# packages/agent/CMakeLists.txt
#
# AI Agent for Vela/NuttX.
#
# Platform stack:
#   - POSIX pthreads, mqueue, mutex+condvar
#   - mbedTLS for HTTPS
#   - File-based JSON config
#   - netlib for network
#   - Raw sockets for WebSocket
#   - syslog for logging
################################################################################

if(CONFIG_EXAMPLES_AI_AGENT_VELA)

  set(AGENT_SRCS
    src/agent_main.c
    src/core/message_bus.c
    src/core/message_bus_tap.c
    src/core/agent_loop.c
    src/core/agent_trace.c
    src/core/context_builder.c
    src/core/memory_store.c
    src/core/session_mgr.c
    src/core/arena_alloc.c
    src/llm/llm_proxy.c
    src/llm/llm_parse.c
    src/llm/llm_vision.c
    src/llm/llm_router.c
    src/llm/llm_cache.c
    src/infra/http_proxy.c
    src/infra/vela_tls.c
    src/infra/config_store.c
    src/infra/network_manager.c
    src/infra/url_parse.c
    src/infra/cron_service.c
    src/infra/heartbeat.c
    src/channels/nsh_commands.c
    src/channels/cmd_llm.c
    src/channels/cmd_voice.c
    src/channels/cmd_channel.c
    src/channels/ws_server.c
    src/tools/tool_registry.c
    src/tools/tool_files.c
    src/tools/tool_get_time.c
    src/tools/tool_web_search.c
    src/tools/tool_cron.c
    src/tools/tool_fetch_url.c
    src/tools/tool_feishu_doc.c
    src/tools/tool_feishu_chat.c
    src/tools/tool_vision.c
    src/tools/tool_shell.c
    src/tools/tool_system.c
    src/tools/tool_health.c
    src/tools/tool_control.c
    src/tools/tool_media.c
    src/tools/tool_proxyquickapp.c
    src/tools/skill_loader.c
    src/tools/tool_guard.c
    src/tools/tool_amap.c
    src/voice/volc_tts.c
    src/voice/volc_tts_ws.c
    src/voice/volc_asr.c
    src/voice/voice_tts.c
    src/voice/voice_asr.c
    src/voice/audio_capture.c
    src/voice/audio_playback.c
    src/voice/voice_channel.c
    src/stubs.c
    src/ui/qrcode_display.c
  )

  # Optional modules
  if(CONFIG_AI_AGENT_FEISHU)
    list(APPEND AGENT_SRCS
      src/channels/feishu_bot.c
      src/channels/feishu_ws.c
      src/channels/feishu_http.c
      src/channels/feishu_proto.c
      src/channels/feishu_recv.c
      src/channels/feishu_send.c
    )
  endif()

  if(CONFIG_AI_AGENT_MQTT)
    list(APPEND AGENT_SRCS src/channels/mqtt_channel.c)
  endif()

  if(CONFIG_AI_AGENT_WEIXIN)
    list(APPEND AGENT_SRCS src/channels/weixin_channel.c)
  endif()

  if(CONFIG_AI_AGENT_SKILL_SYNC)
    list(APPEND AGENT_SRCS src/tools/skill_sync.c)
  endif()

  if(CONFIG_AI_AGENT_TEST)
    list(APPEND AGENT_SRCS tests/integs/test_vision_integ.c)
  endif()

  if(CONFIG_AI_AGENT_LVGL_UI)
    list(APPEND AGENT_SRCS src/ui/lvgl_ui_channel.c)
  endif()

  if(CONFIG_AI_AGENT_CAMERA)
    list(APPEND AGENT_SRCS src/tools/tool_camera.c)
  endif()

  if(CONFIG_AI_AGENT_NODE)
    list(APPEND AGENT_SRCS
      src/node/node_client.c
      src/node/node_manager.c
    )
  endif()

  if(CONFIG_AI_AGENT_MCP)
    list(APPEND AGENT_SRCS
      src/tools/mcp_bridge.c
      src/tools/mcp_server.c
      src/tools/mcp_tool_registry.c
      src/tools/mcp_builtin_tools.c
      src/tools/mcp_client.c
    )
  endif()

  if(CONFIG_AI_AGENT_BLE_GATT)
    list(APPEND AGENT_SRCS src/infra/ble_gatt.c)
  endif()

  if(CONFIG_AI_AGENT_BLE_NET)
    list(APPEND AGENT_SRCS src/infra/ble_net.c)
  endif()

  if(CONFIG_AI_AGENT_XIAOZHI)
    list(APPEND AGENT_SRCS
      src/channels/xiaozhi_channel.c
      src/channels/xiaozhi_opus.c
      src/channels/xiaozhi_udp.c
    )
  endif()

  # cJSON from apps/netutils/cjson
  set(CJSON_INC ${NUTTX_APPS_DIR}/netutils/cjson/cJSON)

  # mbedTLS headers
  set(MBEDTLS_INC ${NUTTX_APPS_DIR}/crypto/mbedtls/mbedtls/include)

  # openvela system topics (battery, wear, screen, fitness, etc.)
  set(TOPICS_INC ${CMAKE_SOURCE_DIR}/../frameworks/system/topics/include)

  # Vibrator API
  set(VIBRATOR_INC ${CMAKE_SOURCE_DIR}/../frameworks/system/vibrator)

  # Media framework
  set(MEDIA_INC ${CMAKE_SOURCE_DIR}/../frameworks/multimedia/media/include)

  # MQTT-C library
  set(MQTTC_INC ${NUTTX_APPS_DIR}/netutils/mqttc/MQTT-C/include)

  # BES AEC (audio preprocessing: NS + AGC)
  if(CONFIG_AI_AGENT_AUDIO_PREPROCESS)
    set(BES_AEC_INC ${NUTTX_APPS_DIR}/vendor/xiaomi/miwear/bes_aec/inc)
  endif()

  # Include directories
  set(AGENT_INCDIRS
    include
    src
    tests
    ${CJSON_INC}
    ${MBEDTLS_INC}
    ${TOPICS_INC}
    ${VIBRATOR_INC}
    ${MEDIA_INC}
    ${MQTTC_INC}
  )

  if(CONFIG_AI_AGENT_AUDIO_PREPROCESS)
    list(APPEND AGENT_INCDIRS ${BES_AEC_INC})
  endif()

  nuttx_add_application(
    NAME
      ${CONFIG_EXAMPLES_AI_AGENT_VELA_PROGNAME}
    SRCS
      ${AGENT_SRCS}
    INCLUDE_DIRECTORIES
      ${AGENT_INCDIRS}
    DEPENDS
      mqttc
    STACKSIZE
      ${CONFIG_EXAMPLES_AI_AGENT_VELA_STACKSIZE}
    PRIORITY
      ${CONFIG_EXAMPLES_AI_AGENT_VELA_PRIORITY}
  )

  # Force agent_main.c to recompile every build so that
  # AGENT_BUILD_VERSION (__DATE__ " " __TIME__) stays fresh.
  # We touch the source file before each build, which invalidates
  # the cached .o and forces the compiler to run again.
  add_custom_target(agent_touch_main ALL
    COMMAND ${CMAKE_COMMAND} -E touch
      ${CMAKE_CURRENT_SOURCE_DIR}/src/agent_main.c
    COMMENT "Refreshing AI Agent build timestamp"
  )

endif()
