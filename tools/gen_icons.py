#!/usr/bin/env python3
"""把 bandx 的 PNG 图标转成 LVGL 9 能直接编译的 C 数组。

为什么用 PNG 转 C 数组而不是 LVGL 的运行时 PNG 解码：
  板子上没开 LV_USE_LIBPNG，而且运行时解码每帧都要解一次 PNG，太慢。
  烘焙成 RGB565A8 数组后，绘制时直接按行拷贝，零解码开销。

为什么是 RGB565A8 而不是 ARGB8888：
  屏幕本身就是 RGB565（CONFIG_LV_COLOR_DEPTH_16）。用 ARGB8888 存储
  每个像素要 4 字节，转成 16 位还要再做一次混合；RGB565A8 是
  "RGB565 色 + 8 位 alpha"，和面板格式一致，体积也小。

用法（在 packages/ai_agent 下）：
    python3 tools/gen_icons.py
"""
import os
import struct

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
BANDX = os.path.join(HERE, "..", "..", "..", "packages/demos/bandx/resource/image/assets")
OUT = os.path.join(HERE, "..", "src", "ui", "assets", "ui_icons.c")

# 需要哪些图标：key -> (源文件名, 目标边长)
# 目标边长按设计稿里的实际显示尺寸给（不放大，只缩小）
ICONS = {
    # 数据胶囊用的小图标（24~28px）
    "heart":       ("heart.png", 28),
    "step":        ("step.png", 28),
    "weather":      ("weather.png", 28),
    "battery":     ("icon_battery.png", 28),
    "bluetooth":   ("icon_bluetooth.png", 28),
    # 应用列表用的 68px 图标
    "app_heart":   ("heart_rate.png", 68),
    "app_sport":   ("sport.png", 68),
    "app_music":   ("music.png", 68),
    "app_sleep":   ("sleep.png", 68),
    "app_settings": ("settings.png", 68),
    "app_timer":   ("stop_watch.png", 68),
    "app_light":   ("flashlight.png", 68),
    # 官方 bandx 没有血氧图标，用 draw_spo2_icon 自绘
    "app_spo2":    ("@draw_spo2", 68),
    # 其它
    "heart_big":   ("heart_color.png", 80),
    "arrow_up":    ("arrow_up.png", 22),
    "arrow_down":  ("arrow_down.png", 22),
}


def draw_spo2_icon(size):
    """画一个血氧图标（水滴 + O₂）。

    官方 bandx 没有血氧图标，这里按它的视觉语言自绘：
    白色描边、无填充、68x68。用 PIL 画而不是引入新的图片资源，
    这样跟其它图标的生成流程一致（都能用 gen_icons.py 重新生成）。
    """
    from PIL import ImageDraw

    im = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    w = max(2, size // 22)

    # 水滴：圆形主体 + 上方尖角
    cx, cy, r = size // 2, int(size * 0.60), int(size * 0.30)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=(255, 255, 255, 255),
              width=w)
    d.line([(cx, cy - r - int(size * 0.12)), (cx - r * 0.80, cy - r * 0.30)],
           fill=(255, 255, 255, 255), width=w)
    d.line([(cx, cy - r - int(size * 0.12)), (cx + r * 0.80, cy - r * 0.30)],
           fill=(255, 255, 255, 255), width=w)

    # 中间 O2 字样（用小字号，居中偏下）
    from PIL import ImageFont
    try:
        f = ImageFont.truetype(
            os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "..", "..", "..", "frameworks/graphics/uikit/test/"
                         "benchmark/assets/NotoSansSC-Regular.ttf"),
            int(size * 0.34))
    except Exception:
        f = ImageFont.load_default()
    d.text((cx, cy + r * 0.10), "O2", font=f, fill=(255, 255, 255, 255),
           anchor="mm")

    return im


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def convert(src_path, size):
    if src_path.startswith("@draw_"):
        return draw_spo2_icon(size)

    im = Image.open(src_path).convert("RGBA")
    if im.size != (size, size):
        # 保持比例缩放到 size 内的正方形画布
        im.thumbnail((size, size), Image.LANCZOS)
        canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        canvas.paste(im, ((size - im.width) // 2, (size - im.height) // 2))
        im = canvas
    return im


def emit(name, im):
    """生成一个 lv_image_dsc_t + RGB565A8 数据。"""
    w, h = im.size
    px = im.load()

    # LVGL 的 RGB565A8：先 h*w 个 RGB565（小端），再 h*w 个 alpha
    color = []
    alpha = []
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            # 预乘 alpha：LVGL 按 straight alpha 混合，这里不做预乘
            color.append(rgb565(r, g, b))
            alpha.append(a)

    data = struct.pack("<%dH" % len(color), *color) + bytes(alpha)
    total = len(data)

    lines = []
    lines.append(f"/* {name}: {w}x{h} RGB565A8, {total} B */")
    lines.append(f"static const uint8_t icon_{name}_map[{total}] = {{")
    for i in range(0, total, 16):
        chunk = data[i:i + 16]
        lines.append("    " + ", ".join("0x%02x" % b for b in chunk) + ",")
    lines.append("};")
    lines.append("")
    lines.append(f"const lv_image_dsc_t ui_icon_{name} = {{")
    lines.append("    .header = {")
    lines.append("        .magic = LV_IMAGE_HEADER_MAGIC,")
    lines.append("        .cf = LV_COLOR_FORMAT_RGB565A8,")
    lines.append("        .flags = 0,")
    lines.append(f"        .w = {w},")
    lines.append(f"        .h = {h},")
    lines.append(f"        .stride = {w * 2},")
    lines.append("        .reserved_2 = 0,")
    lines.append("    },")
    lines.append(f"    .data_size = {total},")
    lines.append(f"    .data = icon_{name}_map,")
    lines.append("    .reserved = NULL,")
    lines.append("    .reserved_2 = NULL,")
    lines.append("};")
    lines.append("")
    return "\n".join(lines), total


def main():
    out = [
        "/****************************************************************************",
        " * ui_icons.c - 界面图标（自动生成，勿手改）",
        " *",
        " * 来源: packages/demos/bandx/resource/image/assets/*.png（SiFli 官方 demo）",
        " * 生成: python3 tools/gen_icons.py",
        " * 格式: RGB565A8（与面板 RGB565 一致，绘制时零解码）",
        " ****************************************************************************/",
        "",
        "#include <lvgl/lvgl.h>",
        "",
    ]

    total_bytes = 0
    for key, (fname, size) in sorted(ICONS.items()):
        if fname.startswith("@draw_"):
            path = fname            # 自绘图标，不查文件
        else:
            path = os.path.join(BANDX, fname)
            if not os.path.exists(path):
                print(f"!! 缺少 {fname}，跳过 {key}")
                continue
        im = convert(path, size)
        code, n = emit(key, im)
        out.append(code)
        total_bytes += n
        print(f"  {key:14s} <- {fname:20s} {im.size[0]}x{im.size[1]}  {n:6d} B")

    with open(OUT, "w") as fp:
        fp.write("\n".join(out))

    print(f"\n合计 {total_bytes} B ({total_bytes/1024:.1f} KB) -> {OUT}")


if __name__ == "__main__":
    main()
