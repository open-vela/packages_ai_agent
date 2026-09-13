# CLI 命令参考

AI Agent 通过 NuttX NSH shell 提供以下命令。启动后输入 `help` 查看完整列表。

## 对话

| 命令 | 说明 |
|------|------|
| `ask <text>` | 与 AI Agent 对话 |
| `help` | 显示帮助信息 |
| `quit` | 退出 AI Agent |

## LLM 配置

| 命令 | 说明 |
|------|------|
| `set_llm <preset> [key]` | 切换 LLM 后端 (kimi/qwen/deepseek/glm/openai/claude/openrouter/mimo) |
| `set_llm model <name>` | 仅切换模型名称 |
| `set_llm <host> <model> <key>` | 自定义 OpenAI 兼容接口 |
| `list_models [--free] [keyword]` | 列出可用模型（openrouter） |

## LLM Router（多后端路由）

| 命令 | 说明 |
|------|------|
| `router_status` | 显示路由状态（后端列表、指标、当前 profile） |
| `router_set <preset> <key>` | 添加/更新 LLM 后端 |
| `router_model <idx> <model>` | 修改指定后端的模型 |
| `router_profile <p>` | 设置路由策略（eco/auto/premium） |
| `router_clear [idx]` | 清除路由后端（不指定 idx 则清除全部） |

## 搜索与外部服务

| 命令 | 说明 |
|------|------|
| `set_search_key <key>` | 设置 SerpAPI Key |
| `set_exa_key <key>` | 设置 Exa AI Key |
| `set_news_key <key>` | 设置 NewsAPI Key |
| `set_tavily_key <key>` | 设置 Tavily Search Key |

## 飞书

| 命令 | 说明 |
|------|------|
| `set_feishu_app <id> <secret>` | 设置飞书应用凭证 |
| `set_feishu_user_token <token>` | 设置飞书用户 Token |

## 微信

| 命令 | 说明 |
|------|------|
| `set_weixin_token <token>` | 设置微信 Bot Token |
| `weixin_login` | QR 码扫码登录微信 |

## 语音

| 命令 | 说明 |
|------|------|
| `set_volc_key <api_key>` | 设置 TTS API Key |
| `set_volc_asr <id> <tok> <cluster>` | 设置 ASR 凭证 |
| `set_volc_speaker <id>` | 设置 TTS 语音角色 |
| `set_voice_tts <name>` | 切换 TTS 后端 |
| `set_voice_asr <name>` | 切换 ASR 后端 |
| `voice_start` | 启动语音通道 |
| `voice_stop` | 停止语音通道 |
| `voice_test_tts <text> [out.pcm]` | 测试 TTS 合成 |
| `voice_test_asr <file>` | 测试 ASR 识别 |

## 网络

| 命令 | 说明 |
|------|------|
| `set_proxy <host> <port>` | 设置 HTTP 代理 |
| `clear_proxy` | 清除代理 |
| `set_wifi <ssid> <pass>` | 连接 WiFi |
| `wifi_reconnect` | 重新连接 WiFi |
| `net_status` | 显示网络状态 |
| `net_test` | 测试 HTTPS 连接 |

## 多节点

| 命令 | 说明 |
|------|------|
| `set_gateway <host> [port] [token]` | 设置 OpenClaw Gateway |
| `set_mqtt <host:port> [client_id]` | 设置 MQTT broker |
| `node_start` | 连接到 Gateway |
| `node_stop` | 断开 Node 连接 |
| `node_list` | 列出已连接 Node |

## Skills 与 MCP

| 命令 | 说明 |
|------|------|
| `install_skill <name> <url>` | 从 HTTPS URL 下载并安装 Skill Markdown 文件 |
| `mcp_add <name> <url> [token]` | 添加远程 MCP Server |
| `mcp_remove <name>` | 移除远程 MCP Server |
| `mcp_discover` | 从所有已添加的 MCP Server 发现远程工具 |
| `mcp_status` | 查看 MCP Client 连接状态 |
| `mcp_tools` | 列出已发现的远程 MCP 工具 |

## 会话与记忆

| 命令 | 说明 |
|------|------|
| `session_list` | 列出会话 |
| `session_clear <id>` | 清除指定会话 |
| `session_clear_all` | 清除所有会话 |
| `memory_read` | 读取 MEMORY.md |
| `memory_write <text>` | 写入 MEMORY.md |

## QuickApp

| 命令 | 说明 |
|------|------|
| `launch_app <package>` | 按包名启动快应用 |
| `exit_app` | 退出当前快应用回到主屏 |

## 系统

| 命令 | 说明 |
|------|------|
| `config_show` | 显示当前配置 |
| `config_reset` | 清除所有运行时配置 |
| `cron_start` | 启动定时任务调度器 |
| `heartbeat_trigger` | 手动触发心跳 |
| `heap_info` | 显示内存使用 |
| `restart` | 重启设备 |
