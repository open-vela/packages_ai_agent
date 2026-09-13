# ai_agent / mini_memo on Gemini-s1 (Allwinner R528)

Pre-built defconfig and a minimal audio-framework patch for running ai_agent
and the `mini_memo` LVGL demo on the Gemini-s1 board (`r528s3-gemini-s1`,
ARM Cortex-A7).

## Available config

| Device | File | Features |
|--------|------|----------|
| Gemini-s1 (R528) | `gemini-s1_defconfig` | LCD (ILI9341), Wi-Fi, BLE GATT, media server, ai_agent + `mini_memo` demo |

## Files in this directory

| File | Purpose |
|------|---------|
| `gemini-s1_defconfig` | Full board defconfig with ai_agent + `mini_memo` + media server enabled |
| `mini_memo_ptt_record_fix.patch` | Cross-repo C fixes for the PTT recording path (applied by the fix script) |
| `README.md` | This file |

The matching patch script lives one level up at
`packages/ai_agent/fix_gemini_s1.sh`.

## Key config options (on top of the stock nsh_minidisplay defconfig)

```
CONFIG_EXAMPLES_AI_AGENT_VELA=y   # enable ai_agent
CONFIG_LVX_USE_DEMO_MINI_MEMO=y   # enable the mini_memo LVGL demo
CONFIG_AI_AGENT_BLE_GATT=y        # BLE GATT data channel
CONFIG_MEDIA=y                    # media framework
CONFIG_MEDIA_SERVER=y             # media server (record + playback)
CONFIG_MEDIA_SERVER_PORT=4040
CONFIG_MEDIA_FOCUS=y
CONFIG_LIB_PFW=y                  # policy framework (criteria.txt)
CONFIG_LIB_FFMPEG=y               # PCM/WAV pipeline used by the media graph
```

`mini_memo` records voice via the media server, classifies it through
ai_agent, and stores memos. The record/playback path relies on the minimal
media graph applied by `fix_gemini_s1.sh` (see below).

## Quick Start

```bash
# 1. Enter project root
cd <openvela-project-root>

# 2. Install this defconfig into the board config directory
cp packages/ai_agent/defconfigs/gemini-s1/gemini-s1_defconfig \
   vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh_minidisplay/defconfig

# 3. Apply the minimal audio-framework patch (no background process needed)
bash packages/ai_agent/fix_gemini_s1.sh

# 4. Build (ARM Cortex-A7)
./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh_minidisplay/ \
    -e -Wno-error -j"$(nproc)"

# 5. Flash with the Allwinner tooling for this board, then boot.
```

## Run

In the board NSH console:

```
# Bring up the agent and configure the LLM backend (persisted to /data).
nsh> ai_agent
vela> set_llm <url> <model> <api_key>
vela> ask hello
```

`mini_memo` starts from the launcher / demo entry on the display. Hold the
PTT button to record a memo; on release the audio is classified
(memo / todo / schedule) and stored.

> Do **not** commit real API keys into the repo. `set_llm` writes them to
> `/data/ai_agent/config/config.json` on the board's persistent partition.

## What does `fix_gemini_s1.sh` do?

The stock Gemini-s1 media graph wires up the full smart-speaker pipeline
(Ring / Music / Alarm movie sources, Video0 / Video1, A2DP sink, a 10-input
`amix`, VOIP / WWE / MDS capture sinks, voice-change, ...). ai_agent /
mini_memo only needs a plain microphone capture path and a plain PCM playback
path.

The script rewrites two media-framework resource files with a minimal pair
that exposes exactly two routes through the media server:

1. **graph.conf** - capture `adevsrc(/dev/audio/pcm0c) -> abufsink@acap` and
   playback `abufsrc@apb -> adevsink(/dev/audio/pcm0p)`, both 48 kHz stereo.
2. **criteria.txt** - point `Capture` / `Ring` / `Music` / `Alarm` at the
   minimal `abufsink` / `abufsrc` nodes so the policy engine resolves against
   the new graph.

Nothing outside the audio framework is touched. The original files are saved
as `*.ai_agent.bak` next to the originals. These changes cannot be expressed
in defconfig because `graph.conf` / `criteria.txt` are plain resource files,
not Kconfig options.

It then applies `mini_memo_ptt_record_fix.patch`, which bundles the cross-repo
C fixes for the "recording receives no frames / PTT hangs" root cause. The
bundle is split by `===== REPO: <path> =====` markers and `git apply`-ed in
each repo:

1. **vendor/allwinnertech** - `hal_dma.c`: disable and clear a DMA channel's
   IRQ before teardown so a latched completion interrupt cannot re-enter the
   handler and touch a freed descriptor (use-after-free guard).
2. **frameworks/multimedia/media** - media server accept pump thread,
   non-blocking socket handling (`EAGAIN` / `EWOULDBLOCK`), format-negotiation
   wildcard handling, and recorder open by stream name.
3. **external/ffmpeg/ffmpeg** - `abufsink`: stop constraining sample rate /
   channel layout on the link so the internal `amix` resamples to the
   recorder-requested target instead of forcing the upstream format.

Each segment is applied idempotently: if it already reverse-applies (i.e. is
already present) it is skipped. The ai_agent side of this fix
(`src/voice/voice_channel.c`) is already upstream and is **not** part of the
patch.

## Adding a new device

1. Create `defconfigs/<device-name>/<device-name>_defconfig`
2. If the device needs patches that cannot live in defconfig, create
   `fix_<device>.sh` next to the other fix scripts
3. Add a `README.md` describing the config and the patch
