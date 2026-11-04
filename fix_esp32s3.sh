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
#   4. esp32s3_bringup.c: persistent /data via SMARTFS (board mount) plus a
#      tmpfs fallback so the system still boots if the SPI-flash storage
#      partition is absent or board_spiflash_init() fails.

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

# ---- Fix 4: persistent /data via SMARTFS, with tmpfs fallback --
# The board file esp32s3_bringup.c mounts SMARTFS at /data through
# board_spiflash_init() (when CONFIG_ESP32S3_SPIFLASH_SMARTFS=y): that call
# creates /dev/smart0 and mounts it at /data. It runs LATER in bringup,
# after the TMPDIR tmpfs block. We must NOT mount /data ourselves here,
# because doing so before board_spiflash_init would make the later board
# mount fail with EBUSY and leave /data as tmpfs (non-persistent).
#
# Instead we inject a FALLBACK right after board_spiflash_init(): if /data
# was not mounted as SMARTFS (SPI flash init failed or the storage MTD
# partition is missing), we mount tmpfs so the system still boots.
BRINGUP="$ROOT_DIR/nuttx/boards/xtensa/esp32s3/esp32s3-eye/src/esp32s3_bringup.c"
if [ -f "$BRINGUP" ]; then
    if grep -q 'tmpfs fallback at /data' "$BRINGUP" 2>/dev/null; then
        echo "  [4/4] esp32s3_bringup.c: /data fallback already present"
    else
        python3 - "$BRINGUP" <<'PYEOF'
import sys, re
path = sys.argv[1]
s = open(path).read()

# Inject a tmpfs fallback between the closing brace of the
# board_spiflash_init() if-block and its #endif. The original board block
# (which actually mounts SMARTFS at /data) is preserved untouched.
pattern = re.compile(
    r'(#ifdef CONFIG_ESP32S3_SPIFLASH\s+'
    r'ret = board_spiflash_init\(\);\s+'
    r'if \(ret\)\s+'
    r'\{\s+'
    r'syslog\(LOG_ERR, "ERROR: Failed to initialize SPI Flash.*?"\);\s+'
    r'\})'
    r'(\s+#endif)',
    re.DOTALL)

fallback = '''
  /* Fallback: if /data was not mounted as SMARTFS by board_spiflash_init
   * (SPI flash init failed or the storage MTD partition is missing), mount
   * tmpfs so the system still boots. Config then lives in RAM only. */
  struct stat _dst;
  if (stat("/data", &_dst) != 0)
    {
      ret = nx_mount(NULL, "/data", "tmpfs", 0, NULL);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: Failed to mount tmpfs fallback at /data: %d\\n", ret);
        }
    }'''

new_s, n = pattern.subn(lambda m: m.group(1) + fallback + m.group(2), s)
if n == 0:
    print("ERROR: board_spiflash_init block not found")
    sys.exit(1)
open(path, "w").write(new_s)
print("ok")
PYEOF
        echo "  [4/4] esp32s3_bringup.c: /data fallback injected"
    fi
fi

echo "[fix] All fixes applied."
