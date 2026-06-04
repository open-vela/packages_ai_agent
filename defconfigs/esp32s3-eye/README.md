# ai_agent Device Defconfigs

Pre-built defconfig files for running ai_agent on specific hardware.

## Available Devices

| Device | File | Features |
|--------|------|----------|
| ESP32-S3-EYE | `esp32s3-eye_defconfig` | WiFi, LCD (ST7789 240x240), PSRAM 8MB, no BLE |

## Quick Start

```bash
# 1. Enter project root
cd <openvela-project-root>

# 2. Copy defconfig to board directory
cp packages/ai_agent/defconfigs/esp32s3-eye_defconfig \
   nuttx/boards/xtensa/esp32s3/esp32s3-eye/configs/ai_agent/defconfig

# 3. Load ESP-IDF environment
source /path/to/esp-idf/export.sh
export CCACHE_DISABLE=1

# 4. Clean build + run fix script in background
./build.sh esp32s3-eye:ai_agent distclean
bash packages/ai_agent/fix_esp32s3.sh &
./build.sh esp32s3-eye:ai_agent

# 5. Flash
esptool.py -c esp32s3 -p /dev/ttyACM0 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash 0x0 nuttx/nuttx.bin

# 6. Run
# Connect serial: minicom -D /dev/ttyACM0 -b 115200
nsh> ai_agent
vela> set_wifi <ssid> <password>
vela> set_llm <host> <model> <api_key>
vela> ask hello
```

## What does `fix_esp32s3.sh` do?

The script applies 3 patches that cannot be expressed in defconfig:

1. **Header priority** - `apps/crypto/mbedtls/Make.defs`: `-I` -> `-isystem` so ESP-IDF's mbedtls headers take precedence over NuttX's (struct layout differs)
2. **CCM cipher** - ESP-IDF `mbedtls_config.h`: disable `MBEDTLS_CCM_C` (CCM*-NO-TAG structs incompatible with NuttX)
3. **Spinlock** - ESP-IDF `clk_ctrl_os.c`: `LOCK_INITIALIZER_UNLOCKED 0` -> `SP_UNLOCKED` (NuttX spinlock_t is a struct, not int)

These are upstream compatibility issues between ESP-IDF's and NuttX's mbedtls forks.

## Expected build warnings

These warnings are **non-fatal** and can be safely ignored:

```
ccache: error: execute_noreturn of /usr/lib/gcc failed: Permission denied
expr: syntax error: unexpected argument "13"
```

To suppress the ccache warning, run `export CCACHE_DISABLE=1` before building.

## Adding a new device

1. Create `defconfigs/<device-name>_defconfig`
2. If the device needs additional patches, create `fix_<device>.sh`
3. Update this README

## References

- [Development Record](https://feishu.cn/docx/OnzPdpo4Po0xA0xbcbEccMNcnwh)
- [ai_agent Usage Guide](https://my.feishu.cn/docx/IYuydFp6noXE3Dxk2n3cnH78nFf)
