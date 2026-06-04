# ai_agent on goldfish-arm64-v8a-ap (QEMU Emulator)

Run ai_agent and the QuickApp ↔ ai_agent integration (`system.velaclaw`) on the
openvela goldfish ARM64 emulator. 

## What this verifies

This config exercises three capabilities together:

1. **QuickApp install** — install and launch a `.rpk` QuickApp via `vapp`.
2. **ai_agent enablement** — `CONFIG_EXAMPLES_AI_AGENT_VELA=y`.
3. **QuickApp ↔ ai_agent linkage** — the QuickApp calls `@system.velaclaw`,
   which bridges to the running ai_agent (`CONFIG_FEATURE_SYSTEM_VELACLAW=y`).

## Files in this directory

| File | Purpose |
|------|---------|
| `goldfish-arm64-v8a-ap_defconfig` | Full board defconfig with ai_agent + velaclaw + QuickApp enabled |
| `com.application.agent.demo.debug.1.0.0.rpk` | Demo QuickApp that uses `system.velaclaw` |
| `README.md` | This file |

## Key config options (on top of the stock goldfish-arm64-v8a-ap defconfig)

```
CONFIG_EXAMPLES_AI_AGENT_VELA=y   # enable ai_agent
CONFIG_FEATURE_SYSTEM_VELACLAW=y  # enable system.velaclaw feature (depends on ai_agent)
CONFIG_MQ_MAXMSGSIZE=4096         # message queue size for the velaclaw bridge
CONFIG_ADBD_SHELL_SERVICE=y       # enable `adb shell`
CONFIG_SYSLOG_CONSOLE=y           # syslog to console
```

QuickApp framework options (`CONFIG_QUICKAPP`, `CONFIG_QUICKAPP_VAPP`,
`CONFIG_INTERPRETERS_QUICKJS`, `CONFIG_LIBASH`, `CONFIG_LIB_YOGA`,
`CONFIG_PROTOBUF_C`, `CONFIG_LV_USE_QRCODE`, `CONFIG_LV_USE_VECTOR_GRAPHIC`,
`CONFIG_UTILS_CURL`, ...) are already enabled in the stock goldfish defconfig.

## Quick Start

```bash
# 1. Enter project root
cd <openvela-project-root>

# 2. Install this defconfig into the board config directory
cp packages/ai_agent/defconfigs/goldfish-arm64-v8a-ap/goldfish-arm64-v8a-ap_defconfig \
   vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap/defconfig

# 3. Build (clean build recommended after a config change)
rm -rf cmake_out/vela_goldfish-arm64-v8a-ap
./build.sh vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap --cmake -j8

# 4. Run the emulator
./emulator.sh cmake_out/vela_goldfish-arm64-v8a-ap/
```

> **Note on rebuilds:** changing the defconfig requires a fresh build. An
> incremental build reuses the previously generated `.config` and your changes
> will silently not take effect. Delete `cmake_out/vela_goldfish-arm64-v8a-ap`
> (or run the `distclean` target and remove the out dir) before rebuilding.

## Install the demo QuickApp

The `vapp hap://app/<package>` launcher expects the app **unpacked** under
`/data/app/<package>/` (an `.rpk` is just a zip).

```bash
# Connect to the emulator's adb (default port 5555)
adb connect 127.0.0.1:5555

# Unpack the rpk on the host
mkdir -p /tmp/agent && \
  unzip -o packages/ai_agent/defconfigs/goldfish-arm64-v8a-ap/com.application.agent.demo.debug.1.0.0.rpk \
  -d /tmp/agent

# Push the unpacked app into /data/app
adb -s 127.0.0.1:5555 shell 'mkdir -p /data/app/com.application.agent.demo'
adb -s 127.0.0.1:5555 push /tmp/agent/. /data/app/com.application.agent.demo/

# Push fonts used by the QuickApp UI (see "Fonts" below)
adb -s 127.0.0.1:5555 push vendor/openvela/boards/vela/resource/font /data/
```

## Fonts

The QuickApp UI needs the MiSans / SimHei / SimKai fonts. They already ship in
the repo at `vendor/openvela/boards/vela/resource/font/` (~44 MB), so they are
**not duplicated here**. Push them straight from there:

```bash
adb -s 127.0.0.1:5555 push vendor/openvela/boards/vela/resource/font /data/
```

## Run

In the emulator NSH console:

```
# Bring up the agent. Run it in the FOREGROUND so its `vela>` CLI owns the
# console (see "Agent CLI vs NSH" below).
nsh> ai_agent

# At the agent prompt, configure the LLM backend and (optional) web search.
# These persist to /data/ai_agent/config/config.json and survive reboot.
vela> set_llm <url> <model> <api_key>
vela> set_tavily_key <key>          # optional, enables web search
vela> ask 你好

# Launch the QuickApp (from NSH; you can background the agent first, or open a
# second console). It navigates to the velaclaw demo page.
nsh> vapp hap://app/com.application.agent.demo &
```

In the QuickApp, tapping "发起对话" calls:

```js
import velaclaw from '@system.velaclaw'

velaclaw.ask({
  query: '北京今天天气怎么样',
  success: function (res) { console.log('AI reply:', res.reply) },
  fail:    function (data, code) { console.log('fail, code:', code) },
  complete:function () { console.log('complete') }
})
```

The request flows: QuickApp → `system.velaclaw` feature → velaclaw bridge →
ai_agent → LLM → reply dispatched back to the QuickApp.

### Configure once (persisted)

`set_llm` / `set_tavily_key` write to `/data/ai_agent/config/config.json` on the
(persistent) `/data` partition, so they survive emulator reboots — set them once.

To pre-seed without typing in the CLI, push a config file (note `set_llm` splits
the URL into `llm_host` + `llm_path`, defaulting `/v1` to `/v1/chat/completions`):

```bash
adb -s 127.0.0.1:5555 shell 'mkdir -p /data/ai_agent/config'
cat > /tmp/config.json <<'EOF'
{
  "llm_host": "<host>",
  "llm_path": "/v1/chat/completions",
  "model":    "<model>",
  "api_key":  "<api_key>",
  "tavily_key": "<tavily_key>"
}
EOF
adb -s 127.0.0.1:5555 push /tmp/config.json /data/ai_agent/config/config.json
```

These values are lost only on a data wipe (deleting `vela_data.bin`) or
`distclean`. Do **not** commit real API keys into the repo.

## Agent CLI vs NSH (gotcha)

`set_llm`, `set_tavily_key`, `ask`, `config_show`, etc. are commands of the
agent's own interactive CLI (the `vela>` prompt), **not** NSH builtins. If you
start the agent with `ai_agent &`, NSH keeps owning the console and these
commands return `command not found`. Run `ai_agent` in the foreground to reach
`vela>`, or pre-seed the config file as shown above.

## Notes / known issues

- **Networking:** the goldfish emulator has working NAT networking by default
  (`eth0` gets `10.0.2.15`). You do **not** need the `ifup wlan0` / `wapi` steps
  from the wearable flow.
- **Avoid pipes in NSH:** `ps | grep ...` can trip an fdsan assert and kill the
  NSH session on this build. Run commands without pipes.
