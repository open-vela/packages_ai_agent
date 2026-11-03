#!/usr/bin/env python3
"""把设计师交回来的文件同步进固件。

用法：
    python3 tools/import_design.py <设计师给的文件夹>

文件夹里应该有（缺哪个就跳过哪个）：
    ui.json      布局
    theme.ini    颜色尺寸
    text.ini     文案
    icons/*.png  图标

做完之后编译烧录即可：
    ninja nuttx.bin
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "..", "src", "ui", "assets")
ICONS_OUT = os.path.join(HERE, "..", "src", "ui", "assets", "ui_user_icons.c")


def copy_if(src, dst):
    if os.path.exists(src):
        shutil.copy(src, dst)
        print("  导入 %s" % os.path.basename(dst))
        return True
    return False


def png_to_c(name, path):
    from PIL import Image

    im = Image.open(path).convert("RGBA")
    # 统一缩放到 136 以内，避免太大占 flash
    if max(im.size) > 136:
        im = im.resize((136, 136), Image.LANCZOS)

    w, h = im.size
    px = list(im.getdata())

    rgb = []
    alpha = []
    for r, g, b, a in px:
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        rgb.append(v & 0xFF)
        rgb.append(v >> 8)
        alpha.append(a)

    sym = "user_icon_" + name.replace("-", "_")

    lines = []
    lines.append("static const uint8_t %s_data[] = {" % sym)
    for i in range(0, len(rgb), 24):
        lines.append("  " + ",".join(str(x) for x in rgb[i:i+24]) + ",")
    for i in range(0, len(alpha), 24):
        lines.append("  " + ",".join(str(x) for x in alpha[i:i+24]) + ",")
    lines.append("};")
    return sym, w, h, lines


def build_icons(folder):
    src = os.path.join(folder, "icons")

    if not os.path.isdir(src):
        print("  （没有 icons 目录，跳过图标）")
        if os.path.exists(ICONS_OUT):
            os.remove(ICONS_OUT)
        return

    pngs = sorted(f for f in os.listdir(src) if f.lower().endswith(".png"))

    if not pngs:
        print("  （icons 目录里没有 PNG）")
        return

    out = ['/* 自动生成，勿手改。源文件：icons/*.png */',
           '#include <lvgl/lvgl.h>',
           '#include "ui_user_icons.h"',
           '']
    table = []

    for f in pngs:
        name = os.path.splitext(f)[0]
        sym, w, h, body = png_to_c(name, os.path.join(src, f))
        out.extend(body)
        out.append('static const lv_image_dsc_t %s_dsc = {' % sym)
        out.append('  .header = { .magic = LV_IMAGE_HEADER_MAGIC,')
        out.append('              .cf = LV_COLOR_FORMAT_RGB565A8,')
        out.append('              .flags = 0,')
        out.append('              .w = %d, .h = %d, .stride = %d, .reserved_2 = 0 },' % (w, h, w * 2))
        out.append('  .data_size = %d,' % (w * h * 3))
        out.append('  .data = %s_data,' % sym)
        out.append('  .reserved = NULL, .reserved_2 = NULL,')
        out.append('};')
        table.append('  { "%s", &%s_dsc },' % (name, sym))
        print("  图标 %s (%dx%d)" % (name, w, h))

    out.append('')
    out.append('const ui_user_icon_t ui_user_icon_table[] = {')
    out.extend(table)
    out.append('};')
    out.append('const unsigned int ui_user_icon_count = %d;' % len(table))
    out.append('')

    open(ICONS_OUT, "w").write("\n".join(out))

    hdr = os.path.join(HERE, "..", "src", "ui", "assets", "ui_user_icons.h")
    open(hdr, "w").write(
        "#ifndef UI_USER_ICONS_H\n#define UI_USER_ICONS_H\n\n"
        "#include <lvgl/lvgl.h>\n\n"
        "typedef struct { const char *name; const lv_image_dsc_t *dsc; } ui_user_icon_t;\n\n"
        "extern const ui_user_icon_t ui_user_icon_table[];\n"
        "extern const unsigned int ui_user_icon_count;\n\n#endif\n")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    folder = sys.argv[1]

    if not os.path.isdir(folder):
        print("不是文件夹:", folder)
        return 1

    print("同步设计师文件：", folder)
    copy_if(os.path.join(folder, "ui.json"), os.path.join(ASSETS, "ui.json"))
    copy_if(os.path.join(folder, "theme.ini"), os.path.join(ASSETS, "theme.ini"))
    copy_if(os.path.join(folder, "text.ini"), os.path.join(ASSETS, "text.ini"))

    build_icons(folder)

    print("烘焙成 C 数组 ...")
    subprocess.check_call([sys.executable, os.path.join(HERE, "embed_ui.py")])

    print()
    print("完成。下一步：")
    print("  cd out/nuttx_contest_board_ai_agent && ninja nuttx.bin")
    return 0


raise SystemExit(main())
