# AI Agent

运行在 openvela 嵌入式系统上的 AI Agent 框架。

多 LLM 后端 · ReAct 工具调用 · 多渠道接入 · 多节点协作 · 语音交互 · 低内存优化

所有运行 openvela OS 的智能设备（手表、眼镜、音箱、耳机等）都可以部署此 AI Agent，设备间通过 OpenClaw Node 协议互联互通。

## 功能亮点

- 🧠 **多 LLM 后端** — Kimi / 通义千问 / DeepSeek / GLM / MiMo / Claude / OpenAI 及任意 OpenAI 兼容接口，一条命令切换
- 🔧 **ReAct Agent** — 35+ 内置工具，支持并行执行，自动推理-行动循环
- 📡 **多渠道接入** — CLI / 飞书 Bot / 微信 Bot / WebSocket / MQTT / 语音（ASR+TTS），统一消息总线，均可通过 Kconfig 独立开关
- 🌐 **多节点协作** — Hub 模式接受设备连接，Node 模式连接 OpenClaw Gateway，跨设备工具调用
- 🔌 **MCP 协议** — 设备端 MCP Client 通过 Streamable HTTP 直连远程 MCP Server（已验证高德地图 15 工具、滴滴出行 13 工具），动态发现和调用远程工具
- 📝 **Skills 系统** — Markdown 格式可扩展技能，内置多个 Skill，对话即可创建新技能
- 🔒 **安全加固** — 工具白名单、敏感工具限流、Shell 三级策略、日志脱敏
- 💾 **低内存优化** — 堆检测、内存池、流式输出，模块化 Kconfig 裁剪，适配手表级设备（~256KB RAM）
- 🔀 **智能路由** — 多后端 LLM Router，复杂度感知路由、自动故障转移、成本优化
- 📷 **视觉能力** — 摄像头拍照 + Vision LLM 分析，支持 V4L2 设备

## 快速开始

### 编译

前提条件：下载 openvela 源码

```bash
# 打开 menuconfig 启用 AI Agent
./build.sh vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap/ --cmake menuconfig
# 路径：Application Configuration → Packages → Vela AI Agent，按空格启用

# 编译
./build.sh vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap/ --cmake -j8

# 运行 QEMU
./emulator.sh cmake_out/vela_goldfish-arm64-v8a-ap
```

### 基本使用

```bash
vela> set_llm kimi sk-xxxxxxxxxxxx       # 设置 LLM
vela> ask 现在几点了？                     # 对话
vela> ask 帮我搜索 NuttX 最新版本          # 工具调用（自动触发 web_search）
vela> set_tavily_key tvly-xxxxxxxxxxxx   # 设置搜索（可选）
vela> set_feishu_app cli_xxx xxxxxxxxxx  # 设置飞书 Bot（可选）
vela> mcp_add amap https://mcp.amap.com/mcp?key=xxx  # 接入高德地图 MCP
vela> mcp_add didi https://mcp.didichuxing.com/mcp-servers?key=xxx  # 接入滴滴出行 MCP
vela> mcp_discover                       # 发现远程工具
```

## 支持的 LLM 后端

| 预设 | 默认模型 | 说明 |
|------|----------|------|
| `kimi` | kimi-k2.5 | Moonshot AI |
| `qwen` | qwen-turbo | 通义千问 |
| `deepseek` | deepseek-chat | DeepSeek |
| `glm` | glm-4-flash | 智谱 AI |
| `mimo` | `mimo-v2-flash` | 小米大模型（Flash/Pro/Omni） |
| `openai` | gpt-4o | OpenAI |
| `claude` | claude-sonnet-4-20250514 | Anthropic Claude |
| `openrouter` | openrouter/hunter-alpha | OpenRouter 聚合 |

```bash
vela> set_llm deepseek sk-xxxxxxxxxxxx   # 切换后端
vela> set_llm model mimo-v2-pro          # 仅切换模型
vela> list_models --free                 # 列出可用模型（openrouter）
```

### LLM Router（多后端路由）

支持最多 4 个后端同时配置，按任务复杂度自动选择最优后端：

```bash
vela> router_set deepseek sk-xxx         # 添加后端
vela> router_set kimi sk-xxx             # 添加第二个后端
vela> router_profile eco                 # 切换路由策略（eco/auto/premium）
vela> router_status                      # 查看路由状态
```

## 内置工具（35+）

| 分类 | 工具 | 说明 |
|------|------|------|
| 🔍 搜索 | `web_search` `news_search` `fetch_url` | Tavily/SerpAPI/Exa/NewsAPI |
| 📁 文件 | `read_file` `write_file` `edit_file` `list_dir` | 限制在 /data/agent/ |
| ⏰ 定时 | `cron_add` `cron_list` `cron_remove` | 定时任务调度 |
| 🖼️ 视觉 | `analyze_image` `camera_capture` | Vision LLM 图片识别/OCR/截屏分析，V4L2 拍照 |
| 🐚 Shell | `run_shell` | NuttX 命令（三级安全策略） |
| ⌚ 设备 | `get_battery` `get_wear_state` `get_screen_state` `get_heartrate` `get_steps` `vibrate` | 传感器与控制 |
| 📄 飞书文档 | `feishu_doc_create` `feishu_doc_write` `feishu_doc_read` `feishu_doc_list` | 飞书云文档 CRUD |
| 💬 飞书群聊 | `feishu_chat_members` `feishu_send_mention` | 群成员查询、@提醒 |
| 🎵 音乐 | `music_search` `music_play` `music_pause` `music_resume` `music_stop` `music_seek` `music_set_volume` `music_status` | 网易云搜索 + PCM/WAV/MP3 播放 |
| 📱 QuickApp | `launch_quickapp` `exit_quickapp` | 启动/退出快应用 |
| 🌤️ 其他 | `get_weather` `get_current_time` | 天气、时间 |
| 🔌 MCP 远程 | 高德地图 15 工具、滴滴出行 13 工具等 | Streamable HTTP 动态发现，`mcp_add` 接入 |

## 接入通道

| 通道 | 协议 | 说明 |
|------|------|------|
| CLI | NSH Shell | `ask <text>` 直接对话 |
| 飞书 Bot | WebSocket | 群聊/私聊，长连接 |
| 微信 Bot | HTTPS (iLink Bot) | QR 码登录，长轮询收消息 |
| WebSocket | WS (28789) | 外部客户端接入，Hub 模式 |
| MQTT | MQTT-C | IoT 设备集成 |
| Voice | PCM/ASR/TTS | 火山引擎后端，支持多厂商切换 |
| Node | OpenClaw | 多设备工具互调 |
| MCP Client | Streamable HTTP | 远程 MCP Server 工具发现与调用（高德/滴滴等） |

详见 [接入通道文档](docs/channels.md)。

## Skills 系统

内置多个 Skill，Markdown 格式，对话即可创建新技能：

| Skill | 说明 |
|-------|------|
| `weather` | 天气查询和预报 |
| `daily-briefing` | 个性化每日简报 |
| `skill-creator` | 教 Agent 创建新 Skill |
| `reminder` | 定时提醒 |
| `note-taker` | 快速笔记 |
| `translate` | 文本翻译 |
| `news-digest` | 新闻摘要 |
| `task-manager` | TODO 任务管理 |
| `system-health` | 系统状态检查 |
| `feishu-test` | 飞书集成测试 |

```bash
vela> ask 帮我创建一个翻译技能    # 对话创建
# 或手动放 .md 文件到 /data/agent/skills/
```

## 配置

### Kconfig 模块开关

所有可选模块均可通过 Kconfig 独立开关，按需裁剪 RAM/Flash 占用：

| 开关 | 默认 | 说明 | 预估 RAM |
|------|------|------|----------|
| `AI_AGENT_FEISHU` | y | 飞书 Bot 通道（WebSocket 长连接） | ~108KB |
| `AI_AGENT_WEIXIN` | y | 微信 Bot 通道（HTTPS 长轮询） | ~45KB |
| `AI_AGENT_MQTT` | y | MQTT 通道 | ~15KB |
| `AI_AGENT_NODE` | y | Hub-Node 跨设备协议 | ~20KB |
| `AI_AGENT_MCP` | y | MCP 工具桥接（Client + Server） | ~25KB |
| `AI_AGENT_LVGL_UI` | n | LVGL 聊天 UI | ~50KB |
| `AI_AGENT_CAMERA` | n | V4L2 摄像头拍照工具 | ~5KB |
| `AI_AGENT_AUDIO_PREPROCESS` | n | 音频预处理（NS + AGC） | ~150KB |
| `AI_AGENT_SKILL_SYNC` | n | 飞书多维表格技能同步 | ~5KB |
| `AI_AGENT_NET_RPMSG` | n | RPMSG/TUN 网络（无 Wi-Fi 设备） | — |
| `AI_AGENT_TEST` | n | 集成测试命令 | ~2KB |

最小配置（CLI-only）：关闭 FEISHU + WEIXIN + MQTT + NODE + MCP + LVGL_UI，预估节省 ~260KB RAM。

### 配置文件

配置文件位于 `/data/agent/config/`：

| 文件 | 说明 |
|------|------|
| `SOUL.md` | Agent 人格设定 |
| `USER.md` | 用户信息 |
| `config.json` | 运行时配置（API Key 等，CLI 命令自动持久化） |

## 文档

| 文档 | 说明 |
|------|------|
| [架构](docs/architecture.md) | 模块概览、数据流、内存管理、安全机制 |
| [接入通道](docs/channels.md) | 飞书 / 微信 / MQTT / Voice / WebSocket / 多节点 |
| [内置工具](docs/tools.md) | 33 种工具列表、Shell 安全策略 |
| [CLI 命令](docs/cli.md) | 完整命令参考（50+ 条） |
| [Skills 系统](docs/skills.md) | 内置 Skill、自定义 Skill、文件格式 |

## Acknowledgments

AI Agent is a derivative work based on [MimiClaw](https://github.com/memovai/mimiclaw),
originally created by Ziboyan Wang and contributors under the MIT License.

AI Agent reimplements and extends MimiClaw's AI agent architecture for the
Vela/NuttX platform, adding multi-channel support (Feishu/WeChat/MQTT/Voice),
multi-device collaboration (OpenClaw Node), MCP protocol integration, and
optimizations for resource-constrained wearable devices.

We gratefully acknowledge the MimiClaw project and its contributors for the
foundational work that made AI Agent possible.

## License

Apache-2.0

This project includes code derived from [MimiClaw](https://github.com/memovai/mimiclaw)
(MIT License, Copyright (c) 2026 Ziboyan Wang). See [NOTICE](NOTICE) for details.
