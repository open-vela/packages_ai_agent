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
    src/infra/cell_net.c
    # 2026-10-31: 健康数据(步数/心率历史)持久化 + 天气共享缓存/后台预取
    src/infra/health_store.c
    src/infra/weather_cache.c
    src/infra/weather_prefetch.c
    src/infra/cron_service.c
    src/infra/heartbeat.c
    src/infra/hangwatch.c
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
    src/tools/tool_alarm.c
    src/tools/tool_fetch_url.c
    src/tools/tool_feishu_doc.c
    src/tools/tool_feishu_chat.c
    src/tools/tool_vision.c
    src/tools/tool_shell.c
    src/tools/tool_system.c
    src/tools/tool_health.c
    # 2026-10-31: 步数历史工具(步数持久化/每日重置由 health/pedometer.c 提供)
    src/tools/tool_steps_history.c
    # persona: 三种可切换人格（温柔/毒舌/元气）的 set_persona 工具。
    # 状态和持久化在 src/ui/persona.c，这里只是薄工具层。
    src/tools/tool_persona.c
    # mic_selfcheck: "ask 麦克风自检" 的诊断后端 + 开机自检探针。
    # 采集序列照抄 contest 仓 audio_rec_test_main.c; 放 health/ 与
    # hr_measure.c 同目录, 两者都是"设备侧自检服务 + 上屏结论"。
    src/health/mic_selfcheck.c
    src/tools/tool_control.c
    src/tools/tool_media.c
    src/tools/tool_proxyquickapp.c
    src/tools/skill_loader.c
    src/tools/tool_guard.c
    src/tools/tool_amap.c
    src/voice/volc_tts.c
    src/voice/volc_tts_ws.c
    src/voice/edge_tts.c
    src/voice/edge_mp3.c
    src/voice/minimax_tts.c
    src/voice/minimax_asr.c
    src/voice/mimo_tts.c
    src/voice/mimo_asr.c
    src/voice/opus_ogg.c
    src/voice/vad_trim.c
    src/voice/pcm_media_recorder.c
    src/voice/volc_asr.c
    src/voice/voice_tts.c
    src/voice/voice_asr.c
    src/voice/audio_capture.c
    src/voice/audio_playback.c
    src/voice/voice_channel.c
    src/voice/pcm_media_player.c
    src/voice/pcm_file_player.c
    src/voice/boot_chime.c
    src/stubs.c
    # persona: 桌宠人格状态 + 三种 system prompt（纯文本，无 LVGL 依赖）。
    # system prompt 构建、set_persona 工具、persona NSH 命令共用这一份状态。
    src/ui/persona.c
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
    # PROACTIVE-MVP P0：主动关怀规则 Router（特征快照 + 评分 + ACTION/SILENT/
    # DEFER + DND/夜间/预算/冷却）。无新线程，由 pet_care 1Hz tick 驱动。
    list(APPEND AGENT_SRCS src/core/care_router.c)
    # PROACTIVE-P1A（docs_ble/109）：RAM ticket（max 8，TTL/DEFER/去重/dump）
    # 与 RAM 环形日志（16 条，决策/隐式反馈）。均不写盘、不加线程。
    list(APPEND AGENT_SRCS src/core/care_ticket.c)
    list(APPEND AGENT_SRCS src/core/care_log.c)
    # hr_monitor: simulated heart-rate walk (scenario 3); threshold routes
    # back into pet_care_hr_report.
    list(APPEND AGENT_SRCS src/health/hr_monitor.c)
    # hr_measure: one-shot "测心率" measurement behind the pet page button.
    # Owns its own worker thread; drives the real sensor through the existing
    # hr_sensor CLI path (task_spawn) and renders via lvgl_ui_post.
    list(APPEND AGENT_SRCS src/health/hr_measure.c)
    # spo2_measure: one-shot "测血氧" measurement behind the pet page button.
    # 与 hr_measure 同构(自己的 worker 线程 + lvgl_ui_post 上屏), 但血氧不走
    # uORB —— 框架没有 algo_spo2 话题, 结果由 max30102.c 的全局变量交出来。
    list(APPEND AGENT_SRCS src/health/spo2_measure.c)
    # 计步器：25 Hz 读 /dev/mpu0。数据源是内核 mpu60x0 驱动，
    # 板级在 I2C3(PA39/PA40) 上 register 出 /dev/mpu0。
    list(APPEND AGENT_SRCS src/health/pedometer.c)
    # hr_watch: 后台静默心率守护（10 分钟一轮），超阈值走 pet_care_hr_report。
    # 真·心率告警的数据源；测量本身复用 hr_measure（start_silent）。
    list(APPEND AGENT_SRCS src/health/hr_watch.c)
    # 第一梯队 5 页（2026-10-19 重做，深色主题 + 全局设计令牌）
    # 导航中枢：所有页面切换的唯一入口，负责先删旧页再建新页
    list(APPEND AGENT_SRCS src/ui/ui_theme.c)   # 设计令牌的公共控件
    # bubble_grid.c（Apple Watch 蜂窝气泡网格）2026-11-02 起不再被 applist 使用：
    # 应用入口改成 2x3 宫格（见 src/ui/applist_page.c 文件头）。源码保留未删。
    list(APPEND AGENT_SRCS src/ui/ui_icon_runtime.c)  # 运行时图标覆盖（真机只用内置）
    list(APPEND AGENT_SRCS src/ui/assets/ui_json_embedded.c)  # 设计稿烘焙成的 JSON
    list(APPEND AGENT_SRCS src/ui/assets/ui_user_icons.c)     # 设计师导入的图标
    list(APPEND AGENT_SRCS src/ui/ui_engine.c)        # 数据驱动界面引擎
    list(APPEND AGENT_SRCS src/ui/ui_text_runtime.c)  # 运行时文案（真机无文件则用默认）
    list(APPEND AGENT_SRCS src/ui/ui_gesture.c)   # 统一手势识别
    list(APPEND AGENT_SRCS src/ui/ui_nav.c)
    list(APPEND AGENT_SRCS src/ui/watchface_page.c)   # 表盘
    list(APPEND AGENT_SRCS src/ui/applist_page.c)     # 应用列表
    list(APPEND AGENT_SRCS src/ui/heart_page.c)       # 心率
    # 心率历史工具：依赖 hr_measure.c(仅 LVGL_UI 下编译)，必须同条件
    list(APPEND AGENT_SRCS src/tools/tool_hr_history.c)
    # 2026-10-31: 自动休息时段检测 + 每日健康摘要（都依赖 pedometer/UI 通道）
    list(APPEND AGENT_SRCS src/health/rest_track.c)
    list(APPEND AGENT_SRCS src/ui/health_summary.c)
    list(APPEND AGENT_SRCS src/tools/tool_rest.c)
    list(APPEND AGENT_SRCS src/tools/tool_health_summary.c)
    list(APPEND AGENT_SRCS src/ui/spo2_page.c)        # 血氧
    # 音乐页：语音能播但屏幕进不去，这个页面把已有的 8 个音乐工具接出来
    list(APPEND AGENT_SRCS src/ui/music_page.c)
    # 本地音乐：在线搜索受版权限制放不出来，本地文件是唯一能保证出声的路径
    list(APPEND AGENT_SRCS src/ui/music_local.c)
    # 播放操作的后台执行器：播放器接口是阻塞的，绝不能在 UI 线程上调
    list(APPEND AGENT_SRCS src/ui/music_worker.c)
    # 计时器页：应用列表里一直有这个图标，但之前点下去只打印 not implemented
    list(APPEND AGENT_SRCS src/ui/timer_page.c)
    list(APPEND AGENT_SRCS src/ui/sport_page.c)       # 运动：真实计步数据
    list(APPEND AGENT_SRCS src/ui/sleep_page.c)       # 静止/休息：如实显示静止时长
    list(APPEND AGENT_SRCS src/ui/alarm_page.c)       # 闹钟：列 cron 持久化任务
    list(APPEND AGENT_SRCS src/ui/weather_page.c)     # 天气：北京 Open-Meteo（免 key）
    # 内嵌的 60 秒演示曲（首次开机写到 /data/music/）
    list(APPEND AGENT_SRCS src/ui/assets/audio/demo_music.c)
    list(APPEND AGENT_SRCS src/ui/pet_page.c)         # AI 对话
    list(APPEND AGENT_SRCS src/ui/settings_page.c)
    list(APPEND AGENT_SRCS src/ui/about_page.c)
    # 2026-11-02 批次 3：小云表情（状态 + 持久化 + 选择页 + 位图资产）
    list(APPEND AGENT_SRCS src/ui/expression.c)
    list(APPEND AGENT_SRCS src/ui/expression_page.c)
    # launcher_page.c 已由 watchface_page 取代，不再编译
    # （保留源码备查；它里面还有定时器泄漏的修复记录）
    if(CONFIG_INPUT_BUTTONS)
      list(APPEND AGENT_SRCS src/ui/key_input.c)
    endif()
    # Only the fonts the UI actually references.  cjk_12 / sans_64 / sans_72 /
    # sans_88 were never used (gc-sections was already dropping them) and
    # sans_16_bold was a second full 3,879-glyph CJK face used for bold on two
    # labels -- 279 KB of flash for a weight change.
    # 字体：
    #   cjk_24 = 按 UI 实际用字烘焙（349 字符，68 KB）—— 中文全部走它
    #   sans_88_bold = 只有 15 个数字字形，给表盘大时间用
    #   cjk_18 = 旧的全集字体，仍被 lvgl_ui_channel / pet_care 引用
    list(APPEND AGENT_SRCS src/ui/assets/ui_font_cjk_24.c)
    list(APPEND AGENT_SRCS src/ui/assets/ui_font_cjk_18.c)
    list(APPEND AGENT_SRCS src/ui/assets/ui_font_sans_88_bold.c)
    # pet_cloud_open / pet_cloud_blink (2 x 300x300 ARGB8888, 720 KB) are gone:
    # the mascot is drawn with LVGL primitives now.
    # 界面图标（从官方 bandx demo 的 PNG 烘焙成 RGB565A8，128 KB）
    list(APPEND AGENT_SRCS src/ui/assets/ui_icons.c)
    # 小云表情位图（8 款 RGB565A8，209.2 KiB）—— 构建产物，见文件头
    list(APPEND AGENT_SRCS src/ui/assets/ui_expressions.c)
    # ★ 2026-11-02：壁纸（390x450 RGB565 = 351,000 B）已下线。
    # 浅色云光主题改用纯色底（src/ui/ui_theme.c 的 ui_page_apply_bg()），
    # 全树已无任何符号引用 img_background_watch_px：
    #   - ui_theme.c 的 s_wallpaper 已删
    #   - pet_page.c 的 s_bg_dsc 已删
    #   - launcher_page.c 本就不参与编译（见上面第 195 行）
    # 源文件 src/ui/assets/img_background_watch.c 保留在磁盘作历史参考，
    # 但不再加入编译 —— 这是 docs_ble/92 号文资源表里的 -342.8 KiB。
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
    # 2026-11-02 BLE 数据通道：只编 GATT 透传 + JSON 命令处理。
    # ble_gatt_net.c 是 "BLE 网络代理"（TUN/IP 桥），额外占 4KB 静态缓冲
    # 并且自己会调 ble_gatt_init() 抢实例；本方案不用它，挪到 BLE_NET 下。
    list(APPEND AGENT_SRCS
      src/infra/ble_gatt.c
      src/infra/ble_cmd_handler.c
    )
  endif()

  if(CONFIG_AI_AGENT_BLE_NET)
    list(APPEND AGENT_SRCS
      src/infra/ble_gatt_net.c
      src/infra/ble_net.c
    )
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

  # DEMO-FW（2026-11-03）：演示"固定短语本地极速回复"编译开关。
  # 默认 ON（演示构建）；正式构建执行 cmake -DDEMO_FAST_REPLY=OFF 重新
  # configure 即关闭（等价于把 agent_loop.c 顶部 #define DEMO_FAST_REPLY
  # 改成 0）。关闭后 handle_demo_fast_path() 整段不参与编译。
  option(DEMO_FAST_REPLY
    "DEMO-FW: local fast replies for fixed demo phrases" ON)
  if(DEMO_FAST_REPLY)
    list(APPEND AGENT_DEFS DEMO_FAST_REPLY=1)
  else()
    list(APPEND AGENT_DEFS DEMO_FAST_REPLY=0)
  endif()

  # VOICE-C（2026-11-04）：模板 TTS 音频缓存编译开关（替换已失效的
  # DEMO_VOICE_CACHE：demo_voice_cache.c/demo_voice_data.c 源文件已从树中
  # 删除，旧 option 默认 ON 会让 clean configure 直接失败）。
  # 默认 ON：把 voice_tts_cache.c + voice_tts_cache_data.c 编进主 app。4 条
  # 固定台词（hello/greet/hr/alarm）16kHz mono PCM16 以 const 数组链接进
  # .rodata（flash XIP），不占 SRAM；数据 373,760 B，代码约 1.4 KB。
  # 命中时本地 audio_playback 播放，无网络 TTS、不占 TTS session、不写 /data。
  # 正式构建 cmake -DVOICE_TTS_CACHE=OFF 关闭：缓存源不参与编译（省约
  # 366 KiB flash）；调用方需用 #if VOICE_TTS_CACHE 守卫（AGENT_DEFS 会传
  # 0/1），未命中路径回落云端 TTS。
  option(VOICE_TTS_CACHE
    "VOICE-C: template TTS audio cache (4 canned PCM lines, ~366KB flash)" ON)
  if(VOICE_TTS_CACHE)
    list(APPEND AGENT_SRCS
      src/voice/voice_tts_cache.c
      src/voice/voice_tts_cache_data.c)
    list(APPEND AGENT_DEFS VOICE_TTS_CACHE=1)
  else()
    list(APPEND AGENT_DEFS VOICE_TTS_CACHE=0)
    # OFF 时生成空实现桩（-ENOENT/0/false）：voice_channel_speak / is_busy /
    # tts_abort 等调用方无需改代码即可继续链接，播放查询恒未命中 → 自动回落
    # 云端 TTS；PCM 数据不参与编译，节省约 366 KiB Flash。file(WRITE) 保持
    # "只改 CMakeLists.txt"。
    set(_vc_stub ${CMAKE_CURRENT_BINARY_DIR}/voice_tts_cache_stub.c)
    file(WRITE ${_vc_stub}
"/* auto-generated by packages/ai_agent/CMakeLists.txt when VOICE_TTS_CACHE=OFF */
#include \"voice/voice_tts_cache.h\"
#include <errno.h>
int voice_tts_cache_play(const char *text) { (void)text; return -ENOENT; }
void voice_tts_cache_stop(void) { }
bool voice_tts_cache_is_playing(void) { return false; }
int voice_tts_cache_lookup(const char *text, const void **pcm, size_t *bytes)
{
  (void)text;
  if (pcm) *pcm = 0;
  if (bytes) *bytes = 0;
  return -ENOENT;
}
")
    list(APPEND AGENT_SRCS ${_vc_stub})
  endif()

  # libopus: 语音识别上传前的编码(见 src/voice/opus_ogg.c)。
  # 必要性是实测出来的: 蓝牙 PAN 上行结构上限约 12KB/s, 而 16kHz/16bit 原始
  # PCM 是 32KB/s, 不压缩根本传不上去。用 DEPENDS 让 apps 侧也链接它 ——
  # opus_ogg.c 内部有 CONFIG_LIB_OPUS 守卫, 没开时不编真实现, 自动回退 WAV。
  if(CONFIG_LIB_OPUS)
    list(APPEND AGENT_DEPENDS libopus)
  endif()

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

  # "persona" NSH builtin: 查看/切换人格，方便串口演示。
  # 只写 1 字节的 /data/persona；与 UI 无关，任何构建都可用来验证。
  nuttx_add_application(
    NAME
      persona
    SRCS
      src/ui/persona_cmd.c
    INCLUDE_DIRECTORIES
      ${AGENT_INCDIRS}
    COMPILE_FLAGS
      -Wno-error
    # 8192 而不是 2048：写 /data/persona 要走 littlefs -> NOR 擦写，调用栈
    # 比只读路径深得多。2048 在真机上直接 hard fault —— 2026-10-20 实测：
    # 不带参数的 persona 正常，persona sassy 崩在 persona_main 入口
    # （arm_hardfault.c:186）。NSH 应用只在人敲命令时短暂存在，多 6KB 栈
    # 没有内存压力。
    STACKSIZE
      8192
    PRIORITY
      100
  )

  # "ui" NSH builtin: 从串口切页 / 切表情（演示与无触摸验证用）。
  # 只在有 LVGL UI 的构建里存在；实现见 src/ui/ui_cmd.c 文件头。
  if(CONFIG_AI_AGENT_LVGL_UI)
    nuttx_add_application(
      NAME
        ui
      SRCS
        src/ui/ui_cmd.c
      INCLUDE_DIRECTORIES
        ${AGENT_INCDIRS}
      COMPILE_FLAGS
        -Wno-error
      # DEMO-FW：4096 -> 8192。ui demo alarm 会走 cron_add_job() ->
      # cron_save_jobs() -> fopen/fwrite(/data)，littlefs/NOR 写调用栈较深；
      # 同仓 persona 应用有实测：2048 栈写 /data 直接 hard fault，8192 正常
      # （CMakeLists persona 段注释）。NSH 应用只在该命令运行期间占栈。
      STACKSIZE
        8192
      PRIORITY
        100
    )

    nuttx_add_application(
      NAME
        hr_set
      SRCS
        src/health/hr_cmd.c
      INCLUDE_DIRECTORIES
        ${AGENT_INCDIRS}
      COMPILE_FLAGS
        -Wno-error
      STACKSIZE
        2048
      PRIORITY
        100
    )
  endif()

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
