#!/usr/bin/env python3
"""从 TTF 生成 LVGL 9 字体 C 文件（纯 Python + fontTools/PIL）。

为什么自己写：
  官方 lv_font_conv 是 npm 包，本机没装。但 LVGL 字体格式很简单：
  一张位图 + 一张字形描述表 + 一张 cmap，照着写就行。

为什么要"按实际用字烘焙"：
  现有 ui_font_cjk_18 烘焙 GB2312 全集 6763 字 = 604 KB（固件的 8%），
  而 UI 里真正出现的汉字只有几十个。按用字烘焙，24px 只要几十 KB。

用法（在 packages/ai_agent 下）：
    python3 tools/gen_font.py --size 24 --bpp 4 --out src/ui/assets/ui_font_cjk_24.c
    # 字符集默认从 src/ 下所有 .c/.h 的中文字面量自动提取
"""
import argparse
import glob
import os
import re
import sys

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

BPP_MAX = {1: 1, 2: 3, 4: 15, 8: 255}

# UI 里需要、但源码字符串里扫描不到的字符：
#   - 图标字形（☰ 等）在源码里是用转义写的，扫描抓不到
#   - 运行时才拼出来的字（历史、记录）
# 显式列出来，保证一定烘焙进字体，否则显示成方块 □。
EXTRA_CHARS = (
    "历史记录关闭返回心率血氧活动天气计时音乐睡眠设置应用"
    "亮度音量蓝牙勿扰模式服务地址模型配置未已连接关开关"
    "开始测量中保持手臂静止秒点按钮"
    "上滑进入下滑退出去打开通知消息"
    "周一三四五六日天月年时分上午下午早安晚上好"
    "电量充满正在充电暂无数据"
    "≡☰★◆●○"
)


def scan_ui_text(src_root):
    """从 UI 源码里提取实际用到的字符（中文字面量 + ASCII）。"""
    chars = set(chr(c) for c in range(0x20, 0x7F))
    chars |= set("，。？！：；“”‘’（）【】《》、…—～·")

    patterns = [
        os.path.join(src_root, "ui", "*.c"),
        os.path.join(src_root, "ui", "*.h"),
    ]
    files = []
    for p in patterns:
        files.extend(glob.glob(p))

    # 匹配 C 字符串字面量里的内容
    #
    # ★ 2026-11-02 修：必须先把 C 转义还原成**字节**、再整体 UTF-8 解码。
    #   原来逐字符扫字面量，遇到 key_input.c 里
    #       "\xe6\x89\x93\xe5\xbc\x80..."（"打开…"的转义写法）
    #   只会把 'x','e','6' 这些 ASCII 字符加进来，真正的汉字一个都没进
    #   字体 —— 结果这批 demo 问句在 24px 子集里永远缺字，只能降级到 18px。
    #   （同样的坑在 tools/lumina_preview/scripts/check_our_fonts.py 里已经
    #   用 scan_c_literals/decode_bytes 处理过，这里对齐它的做法。）
    _ESC = {'n': 10, 't': 9, 'r': 13, '\\': 92, '"': 34, "'": 39,
            '0': 0, 'a': 7, 'b': 8, 'f': 12, 'v': 11, '?': 63}
    lit = re.compile(r'"((?:[^"\\]|\\.)*)"')
    for f in files:
        try:
            txt = open(f, encoding="utf-8", errors="ignore").read()
        except OSError:
            continue
        # 跳过注释里的内容（注释里的中文不需要上屏）
        txt = re.sub(r'/\*.*?\*/', '', txt, flags=re.S)
        txt = re.sub(r'//[^\n]*', '', txt)
        # ★ 2026-11-02 修：C 会把**相邻的字符串字面量拼接**成一个
        #   （"20\xe5\x88\x86..." "\x91..." 这种被拆行写）。逐字面量解码时，
        #   拆点正好落在 UTF-8 多字节序列中间，decode 失败后整段退化成
        #   latin-1 —— '后'(U+540E) 和 '提'(U+63D0) 就是这么丢的。
        #   先按 C 的规则把相邻字面量合并，再解码。注释已在上一行剥掉，
        #   所以这里剩下的 `"空白 "` 一定都是拼接。
        txt = re.sub(r'"\s*"', '', txt)
        for m in lit.finditer(txt):
            raw = m.group(1)
            buf = bytearray()
            i, n = 0, len(raw)
            while i < n:
                c = raw[i]
                if c == '\\' and i + 1 < n:
                    e = raw[i + 1]
                    if e in _ESC:
                        buf.append(_ESC[e])
                        i += 2
                    elif e == 'x':
                        mm = re.match(r'[0-9a-fA-F]{1,2}', raw[i + 2:i + 4])
                        if mm:
                            buf.append(int(mm.group(0), 16))
                            i += 2 + len(mm.group(0))
                        else:
                            buf.extend(e.encode())
                            i += 2
                    elif e in '01234567':
                        mm = re.match(r'[0-7]{1,3}', raw[i + 1:i + 4])
                        buf.append(int(mm.group(0), 8) & 0xFF)
                        i += 1 + len(mm.group(0))
                    else:
                        buf.extend(e.encode())
                        i += 2
                else:
                    buf.extend(c.encode("utf-8"))
                    i += 1
            try:
                text_lit = buf.decode("utf-8")
            except UnicodeDecodeError:
                text_lit = buf.decode("latin-1")
            for ch in text_lit:
                if ord(ch) >= 0x20:
                    chars.add(ch)

    chars |= set(EXTRA_CHARS)

    chars.discard("\n")
    chars.discard("\r")
    chars.discard("\t")
    return sorted(chars, key=ord), files


def render_glyphs(font_path, size, chars, cmap):
    """渲染每个字符，返回 LVGL 字形描述。

    对照官方 lv_font_conv 的产物（ui_font_sans_24.c）：
      '#'  box_w=13 box_h=18 ofs_y=0
      'A'  box_w=21 box_h=23 ofs_y=-5
    说明官方**做垂直裁剪**（box_h 只含墨迹高度），ofs_y 表示
    box 顶相对基线的位置（向上为负）。

    所以这里也做垂直裁剪：
      - 横向：宽度用 advance（保证 advance 准确，不裁剪）
      - 纵向：只保留墨迹范围，ofs_y = 墨迹顶 - ascent
    """
    pil_font = ImageFont.truetype(font_path, size)
    ascent, descent = pil_font.getmetrics()
    line_h = ascent + 1

    glyphs = []

    for ch in chars:
        if ord(ch) not in cmap:
            continue

        adv = int(round(pil_font.getlength(ch)))
        gw = max(adv, 1)

        # 先画到足够高的画布（baseline 在 ascent 处）
        big_h = ascent + descent + size
        img = Image.new("L", (gw, big_h), 0)
        d = ImageDraw.Draw(img)
        d.text((0, ascent), ch, font=pil_font, fill=255, anchor="ls")

        bbox = img.getbbox()
        if bbox is None:
            # 空白字符：有 advance，无墨迹
            glyphs.append((0, 0, 0, adv, 0, None))
            continue

        x0, y0, x1, y1 = bbox
        # **横向和纵向都裁到墨迹范围**（对照官方 sans_24：
        #   '!' box_w=4 box_w 是墨迹宽，不是 advance 宽）。
        g = img.crop((x0, y0, x1, y1))
        gw, gh = g.size
        ofs_x = x0

        # 官方 lv_font_conv 的 ofs_y 定义（对照 sans_24 反解）：
        #   ofs_y = ascent - 墨迹底
        #   '!' 墨迹底=28, ascent=28 -> 0   （官方 0）✓
        #   '$' 墨迹底=31, ascent=28 -> -3  （官方 -3）✓
        ofs_y = ascent - y1

        glyphs.append((gw, gh, ofs_x, adv, ofs_y, g))

    return glyphs, line_h


def pack_bitmap(g, bpp):
    """把 PIL 灰度图打包成 LVGL 的 bpp 位图。

    关键（对照 lv_font_fmt_txt.c 的 bpp==4 分支）：
        for(x < box_w) { i = i & 0x1; ... }
        bitmap_out_tmp += stride;   // 只有输出行指针前进
    位流计数器 `i` 在**行与行之间不重置**，bitmap_in 也只在攒满一个字节时
    前进。也就是说 LVGL 的位图是**一整条连续位流**，不做逐行字节对齐。

    所以这里也不能按行补齐 —— 先前按行对齐，导致每行错开，
    宽字形（24px 汉字，整除 8）看不出问题，窄字形（13px 数字）整个糊掉。
    """
    if g is None:
        return b""

    gw, gh = g.size
    px = g.load()
    levels = BPP_MAX[bpp]
    data = bytearray()

    acc = 0
    nbits = 0

    for y in range(gh):
        for x in range(gw):
            q = (px[x, y] * levels + 127) // 255
            acc = (acc << bpp) | (q & levels)
            nbits += bpp
            if nbits == 8:
                data.append(acc & 0xFF)
                acc = 0
                nbits = 0

    # 末尾不足一字节的补零（LVGL 会略过多余位）
    if nbits:
        data.append((acc << (8 - nbits)) & 0xFF)

    return bytes(data)


def emit(name, font_path, size, bpp, chars, out_path, fallback=None):
    tf = TTFont(font_path)
    cmap = tf.getBestCmap()

    glyphs, line_h = render_glyphs(font_path, size, chars, cmap)

    # 逐字形打包，记录偏移
    blob = bytearray()
    gdsc = []
    for (gw, gh, ofs_x, adv, ofs_y, g) in glyphs:
        idx = len(blob)
        blob += pack_bitmap(g, bpp)
        gdsc.append((idx, adv * 16, gw, gh, ofs_x, ofs_y))

    # 字符 -> glyph id 映射（id 从 1 开始，0 是保留的 .notdef）
    used = [c for c in chars if ord(c) in cmap]
    gid_of = {ord(c): i + 1 for i, c in enumerate(used)}

    # cmap 分段：连续的 unicode 合并成 FORMAT0，其余 SPARSE
    unis = sorted(gid_of)
    segments = []
    if unis:
        start = unis[0]
        prev = unis[0]
        for u in unis[1:]:
            if u == prev + 1:
                prev = u
                continue
            segments.append((start, prev))
            start = u
            prev = u
        segments.append((start, prev))

    cmap_lines = []
    ulist_blocks = []
    ci = 0
    for (a, b) in segments:
        rng_len = b - a + 1
        gid_start = gid_of[a]
        if rng_len == (b - a + 1) and all((a + k) in gid_of for k in range(rng_len)) \
           and all(gid_of[a + k] == gid_start + k for k in range(rng_len)):
            # 连续且 gid 连续：FORMAT0
            cmap_lines.append(
                f"    {{.range_start = {a}, .range_length = {rng_len}, "
                f".glyph_id_start = {gid_start}, .unicode_list = NULL, "
                f".glyph_id_ofs_list = NULL, .list_length = 0, "
                f".type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY}},")
        else:
            # 稀疏：逐个列 unicode
            ul = [u - a for u in unis if a <= u <= b]
            nm = f"unicode_list_{len(ulist_blocks)}"
            ulist_blocks.append((nm, ul))
            cmap_lines.append(
                f"    {{.range_start = {a}, .range_length = {b - a + 1}, "
                f".glyph_id_start = {gid_start}, .unicode_list = {nm}, "
                f".glyph_id_ofs_list = NULL, .list_length = {len(ul)}, "
                f".type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY}},")
        ci += 1

    L = []
    A = L.append
    A("/*******************************************************************************")
    A(f" * Size: {size} px")
    A(f" * Bpp: {bpp}")
    A(f" * 字符数: {len(used)}（只含 UI 源码里实际出现的字符）")
    A(" * 由 tools/gen_font.py 生成，勿手改")
    A(" ******************************************************************************/")
    A("")
    A("#ifdef LV_LVGL_H_INCLUDE_SIMPLE")
    A('#include "lvgl.h"')
    A("#else")
    A('#include "lvgl/lvgl.h"')
    A("#endif")
    A("")
    A(f"#ifndef {name.upper()}")
    A(f"#define {name.upper()} 1")
    A(f"#endif")
    A("")
    A(f"#if {name.upper()}")
    A("")
    A("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {")
    for i in range(0, len(blob), 16):
        A("    " + ", ".join("0x%02x" % v for v in blob[i:i + 16]) + ",")
    A("};")
    A("")
    A("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
    A("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0,"
      " .ofs_x = 0, .ofs_y = 0},")
    for (idx, adv_w, w, h, ox, oy) in gdsc:
        A(f"    {{.bitmap_index = {idx}, .adv_w = {adv_w}, .box_w = {w},"
          f" .box_h = {h}, .ofs_x = {ox}, .ofs_y = {oy}}},")
    A("};")
    A("")

    for (nm, ul) in ulist_blocks:
        A(f"static const uint16_t {nm}[] = {{")
        for i in range(0, len(ul), 8):
            A("    " + ", ".join("0x%x" % v for v in ul[i:i + 8]) + ",")
        A("};")
        A("")

    A("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
    for line in cmap_lines:
        A(line)
    A("};")
    A("")
    A("static const lv_font_fmt_txt_dsc_t font_dsc = {")
    A("    .glyph_bitmap = glyph_bitmap,")
    A("    .glyph_dsc = glyph_dsc,")
    A("    .cmaps = cmaps,")
    A("    .kern_dsc = NULL,")
    A("    .kern_scale = 0,")
    A(f"    .cmap_num = {len(cmap_lines)},")
    A(f"    .bpp = {bpp},")
    A("    .kern_classes = 0,")
    A("    .bitmap_format = 0,")
    A("};")
    A("")
    if fallback:
        A(f"extern const lv_font_t {fallback};")
        A("")
    A(f"const lv_font_t {name} = {{")
    A("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
    A("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
    A(f"    .line_height = {line_h},")
    A("    .base_line = 0,")
    A("    .subpx = LV_FONT_SUBPX_NONE,")
    A("    .underline_position = 0,")
    A("    .underline_thickness = 0,")
    A("    .dsc = &font_dsc,")
    A("    .fallback = " + (f"&{fallback}," if fallback else "NULL,"))
    A("};")
    A("")
    A(f"#endif /* {name.upper()} */")
    A("")

    with open(out_path, "w") as fp:
        fp.write("\n".join(L))

    print(f"  {name}: {len(used)} 字形, {len(blob)} B 位图, "
          f"{len(cmap_lines)} cmap 段, line_h={line_h}")
    return len(blob)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", default=None)
    ap.add_argument("--size", type=int, default=24)
    ap.add_argument("--bpp", type=int, default=4, choices=[1, 2, 4, 8])
    ap.add_argument("--out", required=True)
    ap.add_argument("--name", default=None)
    ap.add_argument("--text", default=None, help="手动指定字符集（默认自动扫源码）")
    ap.add_argument("--fallback", default=None, help="缺字回退字体符号名")
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(os.path.join(here, "..", "..", ".."))
    src_root = os.path.abspath(os.path.join(here, "..", "src"))

    font = args.font or os.path.join(
        root, "frameworks/graphics/uikit/test/benchmark/assets/NotoSansSC-Regular.ttf")
    name = args.name or f"ui_font_cjk_{args.size}"

    if args.text:
        chars = sorted(set(args.text), key=ord)
        chars = [c for c in chars if ord(c) >= 0x20]
        print(f"  手动字符集: {len(chars)} 个")
    else:
        chars, files = scan_ui_text(src_root)
        cjk = [c for c in chars if ord(c) > 0x2E80]
        print(f"  扫描 {len(files)} 个文件, 共 {len(chars)} 字符（中文 {len(cjk)}）")

    emit(name, font, args.size, args.bpp, chars, args.out, args.fallback)


if __name__ == "__main__":
    main()
