# 内置工具

AI Agent 内置 35+ 种工具，Agent 通过 ReAct 循环自动调用。

## 工具列表

| 工具名 | 说明 |
|--------|------|
| `web_search` | Tavily/SerpAPI/Exa 搜索 |
| `news_search` | NewsAPI 新闻搜索 |
| `get_weather` | 天气查询 |
| `get_current_time` | 获取当前时间 |
| `read_file` | 读取文件 |
| `write_file` | 写入文件 |
| `edit_file` | 编辑文件（查找替换） |
| `list_dir` | 列出目录 |
| `cron_add` | 添加定时任务 |
| `cron_list` | 列出定时任务 |
| `cron_remove` | 删除定时任务 |
| `fetch_url` | 抓取 URL 内容 |
| `analyze_image` | 截屏+图片识别/OCR（Vision LLM，自动调用 fbcapture） |
| `camera_capture` | V4L2 摄像头拍照（需 CONFIG_AI_AGENT_CAMERA） |
| `run_shell` | 执行 NuttX Shell 命令（三级安全策略） |
| `get_battery` | 电池电量、充电状态、电压、温度 |
| `get_wear_state` | 穿戴设备佩戴检测 |
| `get_screen_state` | 屏幕开关状态 |
| `get_heartrate` | 最新心率（BPM） |
| `get_steps` | 当日步数和步频 |
| `vibrate` | 触发设备振动 |
| `feishu_doc_create` | 创建飞书云文档 |
| `feishu_doc_write` | 写入飞书文档内容 |
| `feishu_doc_read` | 读取飞书文档内容 |
| `feishu_doc_list` | 列出飞书文件夹中的文档 |
| `feishu_chat_members` | 列出飞书群聊成员（返回 name + open_id） |
| `feishu_send_mention` | 在飞书群聊中 @提醒指定用户 |
| `music_play` | 播放音乐（本地/URL，PCM/WAV/MP3） |
| `music_search` | 网易云音乐搜索（按关键词返回歌曲列表+URL） |
| `music_pause` | 暂停播放 |
| `music_resume` | 恢复播放 |
| `music_stop` | 停止播放 |
| `music_seek` | 跳转到指定位置（毫秒） |
| `music_set_volume` | 设置音量（0-100） |
| `music_status` | 查询播放状态、进度、时长 |
| `launch_quickapp` | 按包名启动快应用 |
| `exit_quickapp` | 退出当前快应用回到主屏 |

文件操作限制在 `/data/ai_agent/` 目录内。

## Shell 安全策略

`run_shell` 通过 Kconfig 配置三级安全策略：

| 模式 | Kconfig 选项 | 说明 |
|------|-------------|------|
| Allowlist（默认） | `EXAMPLES_AI_AGENT_VELA_SHELL_ALLOWLIST` | 仅允许白名单命令，禁止管道和重定向 |
| Full | `EXAMPLES_AI_AGENT_VELA_SHELL_FULL` | 允许大部分命令和 shell 特性，仅拦截关键命令 |
| Deny | `EXAMPLES_AI_AGENT_VELA_SHELL_DENY` | 完全禁用 run_shell |

非内置命令通过 `popen()` 执行（需启用 `CONFIG_SYSTEM_POPEN`）。
