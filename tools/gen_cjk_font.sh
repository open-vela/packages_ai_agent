#!/bin/bash
# Generate a CJK-subset LVGL font from the char set in /tmp/cjk_chars.txt
set -e
SYMBOLS=$(cat /tmp/cjk_chars.txt)
SRC=/home/aila/projects/vela_contest/frameworks/graphics/uikit/test/benchmark/assets/NotoSansSC-Regular.ttf
OUT=/home/aila/projects/vela_contest/apps/packages/ai_agent/src/ui/assets/ui_font_cjk_18.c
cd /tmp
npx -y lv_font_conv \
  --font "$SRC" \
  --size 18 \
  --bpp 4 \
  --format lvgl \
  --no-compress \
  --symbols "$SYMBOLS" \
  -o "$OUT" \
  --lv-font-name ui_font_cjk_18
echo "generated: $OUT"
ls -la "$OUT"
