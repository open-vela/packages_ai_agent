#!/usr/bin/env python3
"""Generate pet_sprite.h from an OpenGameArt CC0 pixel-animal sprite sheet.

Source:  https://opengameart.org/sites/default/files/all_43.png
         (author davidcrybao, CC0 public domain — no attribution required)
         352x16 sheet of 22 tiles, each 16x16.

We use tile 13 (x = 208..223): a front-facing white cat/dog face with black
outline, two 1px pupils, a muzzle and a mouth.  It is baked to an opaque
RGB565 LVGL v9 lv_image_dsc_t (transparent pixels filled with the app's
near-black background), plus a second "blink" variant with the two pupils
blanked out (eyes closed) for the WAITING state.

The script only READS the sheet; it writes C to stdout or --out.  Run with
`python3 -I` against an untrusted downloaded file.
"""

import argparse
import struct
import sys

TILE = 13
W = H = 16

# Target background baked into transparent pixels (must match UI_PAL_BG).
BG = (0x0F, 0x10, 0x11)
WHITE = (0xFF, 0xFF, 0xFF)
BLACK = (0x00, 0x00, 0x00)

# The two pupil pixel coordinates in tile 13 (row 6, cols 5 and 10).
PUPILS = ((6, 5), (6, 10))


def load_tile(path):
    """Return a 16x16 list of (r, g, b) with transparency resolved to BG."""
    from PIL import Image
    im = Image.open(path).convert("RGBA")
    x0 = TILE * W
    px = im.load()
    grid = []
    for y in range(H):
        row = []
        for x in range(W):
            r, g, b, a = px[x0 + x, y]
            if a < 128:
                r, g, b = BG
            # Snap near-white/black to pure for crisp two-tone pixel art.
            elif r > 200 and g > 200 and b > 200:
                r, g, b = WHITE
            elif r < 40 and g < 40 and b < 40:
                r, g, b = BLACK
            row.append((r, g, b))
        grid.append(row)
    return grid


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def emit_map(name, grid):
    """Emit a uint8_t little-endian RGB565 byte array for a 16x16 grid."""
    lines = []
    # Pack as bytes in little-endian order (target is ARM LE).
    for row in grid:
        vals = []
        for (r, g, b) in row:
            v = rgb565(r, g, b)
            vals.append("0x%02x, 0x%02x" % (v & 0xFF, v >> 8))
        lines.append("    " + ", ".join(vals) + ",")
    body = "\n".join(lines)
    return ("static const uint8_t %s_map[] = {\n%s\n};\n" % (name, body))


def emit_dsc(name):
    return (
        "static const lv_image_dsc_t %s = {\n"
        "    .header.magic = LV_IMAGE_HEADER_MAGIC,\n"
        "    .header.cf = LV_COLOR_FORMAT_RGB565,\n"
        "    .header.w = %d,\n"
        "    .header.h = %d,\n"
        "    .header.stride = %d,\n"
        "    .data = %s_map,\n"
        "    .data_size = sizeof(%s_map),\n"
        "};\n" % (name, W, H, W * 2, name, name)
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sheet", help="path to all_43.png")
    ap.add_argument("--out", help="write header here (default stdout)")
    args = ap.parse_args()

    base = load_tile(args.sheet)
    blink = [row[:] for row in base]
    for (y, x) in PUPILS:
        blink[y][x] = WHITE

    hdr = [
        "/*",
        " * pet_sprite.h — pixel-art pet sprite (CC0, auto-generated).",
        " *",
        " * Source asset: OpenGameArt all_43.png by davidcrybao (CC0 public domain).",
        " *   https://opengameart.org/sites/default/files/all_43.png",
        " * Tile 13: 16x16 white cat/dog face, scaled 6x at runtime (96px).",
        " * Regenerate: python3 -I tools/gen_pet_sprite.py <all_43.png> --out src/ui/pet_sprite.h",
        " *",
        " * pet_sprite_base  — normal face (idle/listening/thinking/executing).",
        " * pet_sprite_blink — eyes closed (waiting).",
        " *",
        " * Transparent pixels are baked to UI_PAL_BG (0x0F1011); keep them in",
        " * sync with ui_palette.h when regenerating.",
        " */",
        "",
        "#pragma once",
        "",
        "#include <lvgl/lvgl.h>",
        "",
        "#if LV_USE_IMAGE",
        "",
        emit_map("pet_sprite_base", base),
        emit_map("pet_sprite_blink", blink),
        emit_dsc("pet_sprite_base"),
        emit_dsc("pet_sprite_blink"),
        "",
        "#endif /* LV_USE_IMAGE */",
        "",
    ]
    out = "\n".join(hdr)
    if args.out:
        with open(args.out, "w") as f:
            f.write(out)
        print("wrote %s" % args.out, file=sys.stderr)
    else:
        sys.stdout.write(out)


if __name__ == "__main__":
    main()
