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
    src/infra/time_sync.c
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
    list(APPEND AGENT_SRCS src/ui/lvgl_toast.c)
    # Pre-rasterized MiSans 16 CJK font (lv_font_conv, 3755 GB2312-1 chars).
    # App-owned like xiaozhi_gui's font_awesome_*.c: LV_FONT_DECLARE in code,
    # no patching of apps_graphics_lvgl. The file header carries the exact
    # regeneration command.
    list(APPEND AGENT_SRCS src/ui/lv_font_misans_16_cjk.c)
  endif()

  # M7 差异化页 (pure-LVGL 退路): round-screen pet status ring home screen.
  # Compiles on the qemu single-core (LVGL_UI) and on the CP (UI_CP) alike.
  if(CONFIG_AI_AGENT_PET_STATUS)
    list(APPEND AGENT_SRCS src/ui/pet_status_view.c)
  endif()

  # M7 差异化页 ② (pure-LVGL 退路): round-screen particle flow visualizer.
  # Distinct "好玩" page alongside the pet; integer-only physics, no float.
  if(CONFIG_AI_AGENT_PARTICLE_FLOW)
    list(APPEND AGENT_SRCS src/ui/particle_flow_view.c)
  endif()

  # M7 待机界面: dim digital clock standby screen (standby_view.c), faded in by
  # lvgl_ui_channel.c after 60s of no activity.  AP-local only.
  if(CONFIG_AI_AGENT_STANDBY_UI)
    list(APPEND AGENT_SRCS src/ui/standby_view.c)
  endif()

  # 路 B (dual-core BK7258): the AP runs the agent but has no LCD/LVGL/FPU.
  # Instead of building bubbles locally, a thin shim (ui_rpmsg_client.c)
  # implements the SAME lvgl_ui_channel_* / lvgl_toast_* API as RPMsg
  # messages to the CP core, where ui_rpmsg_server.c renders them.  Call
  # sites are guarded with
  #   #if defined(CONFIG_AI_AGENT_LVGL_UI) || defined(CONFIG_AI_AGENT_UI_RPMSG)
  # so agent_main.c / nsh_commands.c / remote_ctrl_channel.c /
  # notify_service.c compile unchanged on both cores.
  if(CONFIG_AI_AGENT_UI_RPMSG)
    list(APPEND AGENT_SRCS src/ui/ui_rpmsg_client.c)
  endif()

  # Physical S1 hold-to-talk button (M5 PTT).  AP-only: the buttons + voice
  # pipeline + UI shim all live on the AP core, so no reverse RPMsg.  Calls
  # voice_channel_start/stop_with_text + lvgl_ui_channel_set_ptt/send_user.
  if(CONFIG_AI_AGENT_PTT)
    list(APPEND AGENT_SRCS src/voice/ptt_button.c)
  endif()

  # S2/S3 context keys now live in the board's button input layer
  # (board/bk7258-devkit/src/bk7258_buttons_input.c), which registers as a
  # remote_ctrl input adapter: short press scrolls the transcript when idle
  # or approves/denies a pending permission, long press cycles the model /
  # opens a fresh session.  The retired key_manager.c owned this before the
  # fusion with the colleague's layer.

  if(CONFIG_AI_AGENT_NOTIFY_SERVICE)
    list(APPEND AGENT_SRCS src/infra/notify_service.c)
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

  if(CONFIG_AI_AGENT_REMOTE_CTRL)
    list(APPEND AGENT_SRCS
      src/remote/remote_link.c
      src/remote/remote_codec.c
      src/remote/remote_session.c
      src/remote/remote_link_mqtt.c
      src/channels/remote_ctrl_channel.c
      src/tools/tool_remote_agent.c
    )
  endif()

  if(CONFIG_AI_AGENT_BLE_GATT)
    list(APPEND AGENT_SRCS
      src/infra/ble_gatt.c
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

################################################################################
# CP-side UI renderer (路 B). Independent of EXAMPLES_AI_AGENT_VELA so the
# CP core can render the chat UI without agent_main/llm/voice/TLS. Builds
# only the LVGL chat-view assets; voice_channel_* + message_bus are stubbed
# (src/ui_cp/cp_stubs.c). The AP->CP UI RPMsg server will be added here next.
################################################################################
if(CONFIG_EXAMPLES_AI_AGENT_UI_CP)

  # NOTE: lv_font_misans_16_cjk.c is NOT listed here. The canonical font
  # lives in the LVGL tree (apps/graphics/lvgl/.../font/) and is compiled into
  # liblvgl.a when CONFIG_LV_FONT_MISANS_16_CJK=y (set in the CP defconfig).
  # We only LV_FONT_DECLARE + reference &lv_font_misans_16_cjk from
  # lvgl_ui_channel.c; the symbol is provided by the LVGL lib at link time,
  # the same way the built-in Montserrat fonts are consumed.
  set(AI_AGENT_UI_CP_SRCS
    src/ui_cp/cp_ui_main.c
    src/ui_cp/cp_stubs.c
    src/ui_cp/ui_rpmsg_server.c
    src/ui/lvgl_ui_channel.c
    src/ui/lvgl_toast.c
  )

  # M7 差异化页 (pure-LVGL 退路): pet status ring home screen on the CP.
  if(CONFIG_AI_AGENT_PET_STATUS)
    list(APPEND AI_AGENT_UI_CP_SRCS src/ui/pet_status_view.c)
  endif()

  # M7 差异化页 ② (pure-LVGL 退路): particle flow visualizer on the CP.
  if(CONFIG_AI_AGENT_PARTICLE_FLOW)
    list(APPEND AI_AGENT_UI_CP_SRCS src/ui/particle_flow_view.c)
  endif()

  # The shared UI sources (lvgl_ui_channel.c / lvgl_toast.c / agent_config.h)
  # gate their real implementations behind CONFIG_AI_AGENT_LVGL_UI.  On the AP
  # that symbol comes from the VELA Kconfig; the CP app does not build the full
  # agent so we define it here instead, making the shared UI subset compile the
  # SAME real code on both cores (stubs in cp_stubs.c cover the voice/bus calls
  # that only exist on AP).
  nuttx_add_application(
    NAME
      ${CONFIG_EXAMPLES_AI_AGENT_UI_CP_PROGNAME}
    SRCS
      ${AI_AGENT_UI_CP_SRCS}
    INCLUDE_DIRECTORIES
      include
      src
    DEFINITIONS
      CONFIG_AI_AGENT_LVGL_UI
    DEPENDS
      lvgl
    STACKSIZE
      ${CONFIG_EXAMPLES_AI_AGENT_UI_CP_STACKSIZE}
    PRIORITY
      ${CONFIG_EXAMPLES_AI_AGENT_UI_CP_PRIORITY}
  )

endif()
