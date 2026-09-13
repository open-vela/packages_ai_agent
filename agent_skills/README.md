# AI Agent Skills

AI Agent 内置 Skill 的 Markdown 源文件。设备启动时自动安装到 `/data/ai_agent/skills/`。

## 目录

| 分类 | Skill | 工具链 |
|------|-------|--------|
| 天气 | weather | get_current_time → web_search |
| 简报 | daily-briefing | get_current_time → read_file → web_search → cron_list |
| 新闻 | news-digest | get_current_time → news_search → web_search |
| 健康 | health-monitor | get_heartrate → get_steps → get_wear_state |
| 运动 | exercise-coach | get_steps → get_heartrate → get_current_time → cron_add |
| 电池 | battery-advisor | get_battery → get_current_time |
| 诊断 | device-diagnostics | get_battery → get_screen_state → get_wear_state → run_shell |
| 提醒 | reminder | get_current_time → cron_add |
| 笔记 | note-taker | get_current_time → read_file → write_file/edit_file |
| 任务 | task-manager | read_file → get_current_time → edit_file/write_file |
| 翻译 | translate | 语言知识 → web_search（术语验证） |
| 会议 | meeting-notes | get_current_time → feishu_doc_create → feishu_doc_write |
| 通知 | team-notify | feishu_chat_members → feishu_send_mention |
| 文档 | doc-search | feishu_doc_list → feishu_doc_read |
| 音乐 | music-dj | music_play → music_set_volume → music_status |
| 助眠 | sleep-music | music_play → music_set_volume → cron_add(music_stop) |
| 紧急 | sos-alert | vibrate → get_current_time → feishu_send_mention |
| 调研 | deep-research | web_search → fetch_url → write_file |
| 截屏 | screenshot-reader | analyze_image |
| 应用 | app-launcher | launch_quickapp / exit_quickapp |
| 系统 | system-health | get_current_time → list_dir → read_file → cron_list |
| 创建 | skill-creator | write_file（创建新 .md） |
| 飞书 | feishu-test | 多工具端到端测试 |

| 语音 | voice-memo | get_current_time → write_file（语音转文字笔记） |
| 语音 | voice-control | run_shell(voice_start/stop/test) |
| 语音 | voice-translate | ASR文字 → 翻译 → TTS播报 |
| 语音 | read-aloud | feishu_doc_read/read_file/fetch_url → TTS |
| 语音 | voice-assistant | 语音优先交互指南（短句、口语化） |
| 语音 | tts-speak | music_play(Google TTS URL) — 免配置TTS |
| IoT | mqtt-iot | MQTT消息收发控制IoT设备 |
| 微信 | weixin-reply | 微信消息风格指南 |
| 协作 | multi-device | Node协议跨设备工具互调 |
| 路由 | llm-router | 多LLM后端路由管理 |
| 记忆 | memory-manager | MEMORY.md长期记忆管理 |
| 配置 | config-wizard | 引导式配置向导 |
| 预警 | weather-alert | 天气预警+定时提醒 |

## 自定义 Skill

用户可以通过对话创建新 Skill：
```
vela> ask 帮我创建一个翻译技能
```

或手动放 `.md` 文件到 `/data/ai_agent/skills/` 目录。
