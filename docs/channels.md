# 接入通道

AI Agent 支持多种接入通道，所有通道通过 message_bus 统一接入 Agent。

## CLI

NSH shell 直接输入 `ask <text>` 对话。详见 [CLI 命令参考](cli.md)。

## 飞书 Bot

WebSocket 长连接，支持群聊/私聊。

### 配置

```bash
vela> set_feishu_app <app_id> <app_secret>
```

### 使用示例

配置好飞书凭证后，直接在飞书群聊或私聊中发消息：

```
用户: 帮我查询当前系统内存用了多少
Bot:  当前系统内存情况：
      MemTotal: 8192 KB
      MemFree:  3456 KB
      已使用约 58%
```

Agent 会自动调用 `run_shell` 工具执行 `cat /proc/meminfo` 并解析结果。

## WebSocket Server

端口 28789，支持外部客户端接入。同时用于 Hub 模式接受 Node 连接。

## MQTT Channel

通过 MQTT 协议接入，适用于 IoT 设备集成。基于 MQTT-C 库。

### 配置

```bash
vela> set_mqtt <broker-host>:1883
```

### 使用示例

```bash
# 在主机上订阅回复 topic
mosquitto_sub -h <broker-host> -t "agent/out" -v

# 在主机上发送消息
mosquitto_pub -h <broker-host> -t "agent/in" \
  -m '{"type":"message","content":"现在几点了？","chat_id":"device1"}'
```

消息格式（JSON）：
- 输入 `agent/in`：`{"type":"message","content":"<文本>","chat_id":"<ID>"}`
- 输出 `agent/out`：`{"type":"response","content":"<回复>","chat_id":"<ID>"}`

## Voice Channel（语音通路）

通用 ASR/TTS 后端抽象，支持多厂商切换。内置火山引擎后端。

```
Mic → PCM → ASR → text → message_bus → agent_loop
                                          ↓
Speaker ← PCM ← TTS ← text ← message_bus (outbound)
```

### 配置

```bash
# 1. 设置 TTS API Key
vela> set_volc_key <api_key>

# 2. 设置 ASR 凭证（第三项可为 V2 cluster 或 V3 resource ID）
vela> set_volc_asr <app_id> <token> <resource_id_or_cluster>

# 3. 可选：设置 TTS 语音角色（默认 zh_male_beijingxiaoye_emo_v2_mars_bigtts）
vela> set_volc_speaker zh_male_chunhou

# 4. 切换后端（添加新后端后可用）
vela> set_voice_tts volcengine
vela> set_voice_asr volcengine
```

火山引擎 V2 可继续使用 `volcengine_streaming_common`。使用大模型 ASR
V3 时，将第三项设置为资源 ID，例如 `volc.bigasr.sauc.duration`。

### 测试（QEMU 文件模式）

QEMU 环境下无真实音频硬件，使用文件模式测试：

```bash
# TTS：文本合成为 PCM 文件
vela> voice_test_tts 你好世界
# TTS OK: 32000 bytes -> /data/agent/tts_out.pcm

# ASR：PCM 文件识别为文本，自动发送给 Agent
vela> voice_test_asr /data/agent/tts_out.pcm
# ASR result: 你好世界
# Sent to agent.
```

在真实硬件上，`voice_start` 启动麦克风采集线程，实现实时语音交互。

### 后端扩展

实现 `voice_tts_ops_t` 或 `voice_asr_ops_t` 接口（`init/synthesize/deinit` 或 `init/recognize/deinit`），在 `voice_channel_init()` 中调用 `register()`，即可通过 CLI 切换。

## 多节点架构

AI Agent 支持两种多节点协作模式：

### 模式 A：Node Client（连接到 OpenClaw Gateway）

AI Agent 作为 Node 连接到 OpenClaw Gateway，将本地工具暴露给远程调用。

```
AI Agent (Node) ──WS──▶ OpenClaw Gateway ◀── 其他客户端
```

```bash
vela> set_gateway <gateway_host> <port> <token>
vela> node_start
```

### 模式 B：Hub / Node Manager（接受 Node 连接）

AI Agent 作为 Hub，接受其他 Node 设备连接，动态注册远程工具。

```
Node A (手表) ──WS──▶ AI Agent (Hub) ◀──WS── Node B (音箱)
```

Hub 监听端口 28789，Node 连接后自动发现并注册工具。

```bash
# 查看已连接的 Node
vela> node_list

# 断开 Node 连接
vela> node_stop
```

## 音乐播放

Agent 通过内置音乐工具控制播放，支持 PCM/WAV/MP3 格式：

```bash
vela> ask 播放一下 /data/test.pcm
vela> ask 暂停
vela> ask 把音量调到 80
vela> ask 从第 5 秒开始播放 /data/music.wav
```

## 微信 Bot

通过腾讯 iLink Bot API 直接连接微信，设备无需中间网关。

### 配置

```bash
# 方式一：直接设置 Token（如果已有）
vela> set_weixin_token <bot_token>

# 方式二：QR 码扫码登录（推荐）
vela> weixin_login
# 屏幕显示 QR 码，用微信扫码确认
```

### 协议

- 所有请求走 HTTPS，目标 `ilinkai.weixin.qq.com`
- 收消息：长轮询 `/ilink/bot/getupdates`
- 发消息：POST `/ilink/bot/sendmessage`
- 登录：获取 QR 码 → 轮询扫码状态 → 自动保存 Token

### 使用示例

扫码登录后，在微信中直接给 Bot 发消息即可对话。Agent 自动处理收发。

## MiMo（小米大模型）

| 模型 | 说明 |
|------|------|
| `mimo-v2-flash` | 默认，轻量快速 |
| `mimo-v2-omni` | 多模态 |
| `mimo-v2-pro` | 高质量推理 |
| `mimo-v2-tts` | 语音合成 |

```bash
vela> set_llm mimo <api_key>
vela> set_llm model mimo-v2-pro
```
