#!/bin/bash
# fix_esp32s3.sh - Fix ESP32-S3 build issues for ai_agent
#
# Usage:
#   cd <project-root>   # e.g. contest-2026/
#   ./build.sh esp32s3-eye:ai_agent distclean
#   bash packages/ai_agent/fix_esp32s3.sh &
#   ./build.sh esp32s3-eye:ai_agent
#
# This script must run in the BACKGROUND during the first build after distclean.
# It waits for ESP-IDF's esp-hal-3rdparty to be cloned and patched, then applies
# three fixes that cannot be expressed in defconfig alone.
#
# Fixes applied:
#   1. apps/crypto/mbedtls/Make.defs: -I -> -isystem (header priority)
#   2. esp-hal-3rdparty mbedtls_config.h: disable CCM cipher (struct conflict)
#   3. esp-hal-3rdparty clk_ctrl_os.c: SP_UNLOCKED spinlock initializer
#   4. esp32s3_bringup.c: mount tmpfs at /data for ai_agent config store
#
# See: https://feishu.cn/docx/OnzPdpo4Po0xA0xbcbEccMNcnwh

set -euo pipefail

# Resolve project root (parent of packages/)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

APPS_DIR="$ROOT_DIR/apps"
HAL_DIR="$ROOT_DIR/nuttx/arch/xtensa/src/esp32s3/esp-hal-3rdparty"

# ---- Wait for ESP-IDF patches to be applied --------------------
echo "[fix] Waiting for esp-hal-3rdparty to be cloned and patched..."
MBEDTLS_CFG="$HAL_DIR/components/mbedtls/mbedtls/include/mbedtls/mbedtls_config.h"

for i in $(seq 1 180); do
    if [ -f "$MBEDTLS_CFG" ]; then
        if grep -q "MBEDTLS_THREADING_C" "$MBEDTLS_CFG" 2>/dev/null; then
            break
        fi
    fi
    sleep 1
done

if [ ! -f "$MBEDTLS_CFG" ]; then
    echo "[fix] ERROR: mbedtls_config.h not found after 180s. Is the build running?"
    exit 1
fi

echo "[fix] Applying fixes..."

# ---- Fix 1: NuttX mbedtls header priority ----------------------
# ESP-IDF and NuttX ship different cipher_info_t layouts.
# Use -isystem so ESP-IDF's -I headers win for esp-hal source files.
MAKEDEFS="$APPS_DIR/crypto/mbedtls/Make.defs"
if [ -f "$MAKEDEFS" ]; then
    sed -i 's|CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/crypto/mbedtls/include|CFLAGS += -isystem $(APPDIR)/crypto/mbedtls/include|' "$MAKEDEFS"
    sed -i 's|CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/crypto/mbedtls/mbedtls/include|CFLAGS += -isystem $(APPDIR)/crypto/mbedtls/mbedtls/include|' "$MAKEDEFS"
    sed -i 's|CXXFLAGS += ${INCDIR_PREFIX}$(APPDIR)/crypto/mbedtls/include|CXXFLAGS += -isystem $(APPDIR)/crypto/mbedtls/include|' "$MAKEDEFS"
    sed -i 's|CXXFLAGS += ${INCDIR_PREFIX}$(APPDIR)/crypto/mbedtls/mbedtls/include|CXXFLAGS += -isystem $(APPDIR)/crypto/mbedtls/mbedtls/include|' "$MAKEDEFS"
    echo "  [1/4] Make.defs: -isystem fix applied"
fi

# ---- Fix 2: Disable CCM cipher in ESP-IDF mbedtls --------------
# CCM*-NO-TAG structs differ between ESP-IDF and NuttX mbedtls forks.
if [ -f "$MBEDTLS_CFG" ]; then
    sed -i 's/^#define MBEDTLS_CCM_C$/\/\* #define MBEDTLS_CCM_C \*\//' "$MBEDTLS_CFG"
    echo "  [2/4] mbedtls_config.h: CCM disabled"
fi

# ---- Fix 3: Spinlock initializer -------------------------------
# ESP-IDF uses 0 to init spinlock_t; NuttX requires SP_UNLOCKED macro.
CLK_FILE="$HAL_DIR/components/esp_hw_support/clk_ctrl_os.c"
if [ -f "$CLK_FILE" ]; then
    sed -i 's/#define LOCK_INITIALIZER_UNLOCKED       0/#define LOCK_INITIALIZER_UNLOCKED       SP_UNLOCKED/' "$CLK_FILE"
    echo "  [3/4] clk_ctrl_os.c: spinlock fix applied"
fi

# ---- Fix 4: Mount tmpfs at /data for ai_agent config store ------
# ai_agent persists config to /data/ai_agent/config/config.json.
# Without this mount, config_show always shows "(not set)".
# This cannot be expressed in defconfig; it's a board-level init change.
BRINGUP="$ROOT_DIR/nuttx/boards/xtensa/esp32s3/esp32s3-eye/src/esp32s3_bringup.c"
if [ -f "$BRINGUP" ]; then
    if ! grep -q 'mount tmpfs at /data' "$BRINGUP" 2>/dev/null; then
        python3 -c "
import re, sys
path = sys.argv[1]
with open(path) as f:
    content = f.read()
old = '''  ret = nx_mount(NULL, CONFIG_LIBC_TMPDIR, \"tmpfs\", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, \"ERROR: Failed to mount tmpfs at %s: %d\\n\",
             CONFIG_LIBC_TMPDIR, ret);
    }
#endif'''
new = '''  ret = nx_mount(NULL, CONFIG_LIBC_TMPDIR, \"tmpfs\", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, \"ERROR: Failed to mount tmpfs at %s: %d\\n\",
             CONFIG_LIBC_TMPDIR, ret);
    }

  /* Mount tmpfs at /data for ai_agent config store */

  ret = nx_mount(NULL, \"/data\", \"tmpfs\", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, \"ERROR: Failed to mount tmpfs at /data: %d\\n\", ret);
    }
#endif'''
if old in content:
    content = content.replace(old, new)
    with open(path, 'w') as f:
        f.write(content)
    print('ok')
else:
    print('pattern not found or already patched')
" "$BRINGUP"
        echo "  [4/4] esp32s3_bringup.c: /data tmpfs mount added"
    else
        echo "  [4/4] esp32s3_bringup.c: /data mount already present"
    fi
fi

echo "[fix] All fixes applied."
