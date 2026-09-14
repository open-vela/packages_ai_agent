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
    src/infra/time_sync.c
    src/infra/network_manager.c
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
    list(APPEND AGENT_SRCS src/ui/pet_display.c)
    # pet_care: proactive companion scheduler (idle care / alarm / hr alert);
    # tick runs on the render thread, L2 goes through the message bus.
    list(APPEND AGENT_SRCS src/ui/pet_care.c)
    list(APPEND AGENT_SRCS src/ui/launcher_page.c)
    list(APPEND AGENT_SRCS src/ui/about_page.c)
    list(APPEND AGENT_SRCS src/ui/pet_page.c)
    list(APPEND AGENT_SRCS src/ui/settings_page.c)
    if(CONFIG_INPUT_BUTTONS)
      list(APPEND AGENT_SRCS src/ui/key_input.c)
    endif()
    # Only the fonts the UI actually references.  cjk_12 / sans_64 / sans_72 /
    # sans_88 were never used (gc-sections was already dropping them) and
    # sans_16_bold was a second full 3,879-glyph CJK face used for bold on two
    # labels -- 279 KB of flash for a weight change.
    list(APPEND AGENT_SRCS src/ui/assets/ui_font_cjk_18.c)
    list(APPEND AGENT_SRCS src/ui/assets/ui_font_sans_16.c)
    list(APPEND AGENT_SRCS src/ui/assets/ui_font_sans_24.c)
    list(APPEND AGENT_SRCS src/ui/assets/ui_font_sans_32.c)
    list(APPEND AGENT_SRCS src/ui/assets/ui_font_sans_88_bold.c)
    # pet_cloud_open / pet_cloud_blink (2 x 300x300 ARGB8888, 720 KB) are gone:
    # the mascot is drawn with LVGL primitives now.
    list(APPEND AGENT_SRCS src/ui/assets/img_background_watch.c)
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
    list(APPEND AGENT_SRCS
      src/infra/ble_gatt.c
      src/infra/ble_gatt_net.c
      src/infra/ble_cmd_handler.c
    )
  endif()

  if(CONFIG_AI_AGENT_BLE_NET)
    list(APPEND AGENT_SRCS src/infra/ble_net.c)
  endif()

  if(CONFIG_AI_AGENT_REST_API)
    list(APPEND AGENT_SRCS
      src/infra/api_handler.c
      src/infra/agent_logbuf.c
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

  # Bluetooth framework
  set(BT_INC ${CMAKE_SOURCE_DIR}/../frameworks/connectivity/bluetooth/framework/include)

  # MQTT-C library
  set(MQTTC_INC ${NUTTX_APPS_DIR}/netutils/mqttc/MQTT-C/include)

  # Direct ALSA capture backend (chip-specific, opt-in)
  if(CONFIG_AI_AGENT_AUDIO_ALSA_DIRECT)
    set(ALSA_DIRECT_INC
      ${NUTTX_APPS_DIR}/audioutils/alsa-lib/include
      ${NUTTX_APPS_DIR}/vendor/allwinnertech/chips/r528/drivers/rtos-hal/include/hal
      ${NUTTX_APPS_DIR}/vendor/allwinnertech/chips/r528/drivers/rtos-hal/include/osal
      ${NUTTX_APPS_DIR}/vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source
    )
  endif()

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
    ${BT_INC}
    ${MQTTC_INC}
  )

  if(CONFIG_AI_AGENT_AUDIO_PREPROCESS)
    list(APPEND AGENT_INCDIRS ${BES_AEC_INC})
  endif()

  if(CONFIG_AI_AGENT_AUDIO_ALSA_DIRECT)
    list(APPEND AGENT_INCDIRS ${ALSA_DIRECT_INC})
  endif()

  set(AGENT_DEPENDS mqttc)
  set(AGENT_DEFS "")

  if(CONFIG_TFLITEMICRO)
    # On-device language model.  The sources and the model data stay in the
    # team repo (contest rules put the work under app/ai_agent/); we reach them
    # through the packages/demos linkfile instead of keeping a second copy.
    # ai_lm.cxx includes "model/model_data.h" relative to its own directory, so
    # no extra include path is needed for the model blobs.
    set(LOCAL_LM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../demos/contest2026_181_ai_agent)
    list(APPEND AGENT_SRCS
      ${LOCAL_LM_DIR}/ai_lm.cxx
      ${LOCAL_LM_DIR}/tokenizer.c
      src/llm/local_lm.c
    )
    list(APPEND AGENT_INCDIRS ${LOCAL_LM_DIR})
    list(APPEND AGENT_DEPENDS tflite_micro)
    list(APPEND AGENT_DEFS
      TFLITE_WITH_STABLE_ABI=0
      TFLITE_USE_OPAQUE_DELEGATE=0
      TFLITE_SINGLE_ROUNDING=0
      TF_LITE_STRIP_ERROR_STRINGS
      TF_LITE_STATIC_MEMORY
    )
  endif()

  nuttx_add_application(
    NAME
      ${CONFIG_EXAMPLES_AI_AGENT_VELA_PROGNAME}
    SRCS
      ${AGENT_SRCS}
    INCLUDE_DIRECTORIES
      ${AGENT_INCDIRS}
    DEFINITIONS
      ${AGENT_DEFS}
    COMPILE_FLAGS
      -Wno-error
    DEPENDS
      ${AGENT_DEPENDS}
    STACKSIZE
      ${CONFIG_EXAMPLES_AI_AGENT_VELA_STACKSIZE}
    PRIORITY
      ${CONFIG_EXAMPLES_AI_AGENT_VELA_PRIORITY}
  )

  # "timesync" NSH builtin: force a sync now, or print the clock.
  nuttx_add_application(
    NAME
      timesync
    SRCS
      src/channels/timesync_main.c
    INCLUDE_DIRECTORIES
      ${AGENT_INCDIRS}
    COMPILE_FLAGS
      -Wno-error
    STACKSIZE
      4096
    PRIORITY
      100
  )

  # "ask <text>" as a real NSH builtin.  The agent's own CLI has the same
  # command, but that thread runs at priority 30 against NSH's 100 on one
  # console fd, so NSH wins every read and the agent CLI never gets input.
  nuttx_add_application(
    NAME
      ask
    SRCS
      src/channels/ask_main.c
    INCLUDE_DIRECTORIES
      ${AGENT_INCDIRS}
    COMPILE_FLAGS
      -Wno-error
    STACKSIZE
      2048
    PRIORITY
      100
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
