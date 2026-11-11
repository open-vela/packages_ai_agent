# AI Coding 开发日志 — mimo 真后端接入（2026-09-20 下半场）

本文件记录 2026-09-20 下午一轮 AI 主导开发（Claude Code）的真实过程：安装
MiMoCode 并把 acp-bridge 的 mimo 后端从「起进程即崩」打到「设备端真编码
闭环」。三个问题都不在任何一层的报错里——全部靠对比实验与原始报文定位。

## 闭环链路（打通后）

```
qemu 内 ai_agent                      PC（宿主）
  rask <整行指令>                       mosquitto(1883, 本机) + acp-bridge(--backend mimo)
    │ MQTT deskmate/goldfish-1/          │ spawn: mimo acp --pure（外部插件会挂死 initialize）
    │ device-to-bridge                   │ 模型: xiaomi-gw/mimo-v2.5-pro
    ▼                                    ▼（token-plan 网关，Anthropic 兼容 /v1/messages）
  指令全量 JSON ──────────────────> session/prompt → 建文件/跑命令
  [Agent] transcript + 绿 toast <── session/update 流 + transcript 回传
```

## 三个问题（均实测定位）

| # | 现象（零报错） | 根因（实测证据） | 修复 |
|---|----------------|------------------|------|
| 1 | bridge 起 mimo 秒崩 `Error: initialize_timeout` | mimo 0.1.14：`~/.config/mimocode/plugins/` 装**任意外部插件**即永远不回 initialize——空插件 `export default {}` 同样复现（rc=124、stdout 0 字节）；藏起 plugins 目录 / `--pure` / 隔离 MIMOCODE_HOME 三者任一即恢复（433 B 正常回包） | acp-bridge mimo 后端改 `--pure` 启动，`DESKMATE_MIMO_PURE=0` 可退回（选手仓 70798fb，与仓库自带 stdio-verifier 的默认一致） |
| 2 | `session/prompt` 秒回 `end_turn`，0 tokens，无任何报错 | 本机 mimo 未登录（`mimo providers whoami` = Not logged in, 0 credentials）；模型从未被调用。配好自定义 provider 后又踩第二坑：`@ai-sdk/anthropic` 自拼 `/messages`，baseURL 少 `/v1` → 网关 404（DEBUG 日志 `AI_APICallError url=…/anthropic/messages statusCode=404`） | `~/.config/mimocode/mimocode.jsonc`（本机私有，不入仓）配 provider `xiaomi-gw`：`@ai-sdk/anthropic` + baseURL `https://token-plan-cn.xiaomimimo.com/anthropic/v1` + claude-mimo 包装器同款 token；provider id 不能叫 `xiaomi`（与内置 provider 撞名被影子化） |
| 3 | 设备长指令被截成 7 个词（模型收到 `…containing exactly print('hello`，反问"内容呢？"） | CLI `tokenise()` 上限 `MAX_ARGS=8`（含命令名），`rask`/`ask` 重组 argv[1..] 时静默丢词；`memory_write`/`voice_test_speak` 更只取 argv[1] 单词。MQTT 抓包复现实证：≤7 词全量、>7 词恰在第 7 词截断 | `cli_thread` 在 `strtok` 原地切分前捕获整行剩余文本，四个自由文本命令（ask/rask/memory_write/voice_test_speak）改收原始文本（packages/ai_agent d3588b6） |

## 真实运行记录（2026-09-20 17:0x，qemu goldfish + 本机 bridge）

设备端发起（118 字符指令，此前会被截断）：

```
vela> rask create a file named mimo_test.py containing exactly print('hello from mimo bridge'), then run it with python3 and tell me the output
Prompt sent. Use 'rstat' for progress ...
```

MQTT 抓包（mosquitto_sub device-to-bridge）——全量过桥，逐字节无截断：

```
{"v":1,"cmd":"prompt_submit",...,"text":"create a file named mimo_test.py containing exactly print('hello from mimo bridge'), then run it with python3 and tell me the output"}
```

mimo 真实产物（bridge cwd = tools/acp-bridge）：

```
$ cat mimo_test.py
print('hello from mimo bridge')
```

设备端收尾（transcript 回传 + 完成 toast）：

```
[Agent]: Output:

```
hello from mimo bridge
```
```

计费侧：bridge 会话累计 ~110K tokens（本轮 input 16 / output 118 起，缓存读为主）。

## 排查方法（可复用）

- **mimo acp 直连探测**：`{ printf '<initialize JSON>'; sleep N; } | timeout 10 mimo acp`——比经 bridge 快一个数量级；变量拆分（--pure × MIMOCODE_HOME × plugins 目录）三轮定位插件挂死。
- **response-driven ACP 探测脚本**（await initialize → session/new → session/prompt，stderr 全开）：`--print-logs --log-level DEBUG` 才能看到 `AI_APICallError` 与请求体；acp-adapter 把子进程 stderr 整个吞掉，经 bridge 永远看不到。
- **MQTT 原始报文对照**：`mosquitto_sub -t 'deskmate/<id>/device-to-bridge' -v` 抓 prompt_submit 的 text 字段，区分「设备侧截断」与「bridge/模型侧截断」。

## 顺带发现（未修，已记录）

- `~/.config/mimocode/plugins/contest-collector.js` 自身 import `../shared/get_github_login.js` 不存在（部署时缺 shared/ 目录）——它不背问题 1 的锅（空插件同样挂死），但目前完全不工作。
- 工作区存在两棵构建树：`./build.sh --cmake` → `cmake_out/vela_*`（陈旧 .config，缺 AI_AGENT_* 全部选项），`lunch && m` → `out/openvela_vela_*`（模拟器在用，配置正确）。改代码后误走前者会静默产出配置错误固件；本轮固件重编走 lunch 流。
- mimo `--pure` 下 claude-import 仍会同步本机 Claude Code 的 MCP 配置（如 mi-feishu），bridge 会话里可见其工具，属预期行为。

## 提交清单

| 仓库 | 提交 | 内容 |
|------|------|------|
| 选手仓 contest2026_272_tokenwujixian | 70798fb | fix(acp-bridge): mimo 后端 --pure 启动（插件挂死 initialize） |
| packages/ai_agent | d3588b6 | fix(cli): 自由文本命令收整行剩余文本（rask 7 词截断） |
| packages/ai_agent | （本提交） | 本开发日志 |

mimocode.jsonc 属本机私有配置（含凭证），不入仓。
