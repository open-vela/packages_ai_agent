#!/usr/bin/env python3
"""Generate frog_sprite.h — a 16x16 green frog face with 6 expressions.

Source asset is tile 18 of OpenGameArt all_43.png by davidcrybao (CC0 public
domain), a 352x16 sheet of 22 16x16 pixel animals.  Tile 18 (x = 288..303) is
a front-facing green frog: two 2x2 black eyes at rows 4-5 (cols 3-4 and 11-12),
a black mouth line at row 8, green fill (RGB 106,190,48) on a black outline.

Six facial-expression frames are derived by editing only the eye region
(face/mouth/stalk dots stay identical):

  frog_sprite_base      — normal open eyes
  frog_sprite_blink     — eyes closed (green)
  frog_sprite_eyes_up   — pupils shifted up one row
  frog_sprite_eyes_down — pupils shifted down one row
  frog_sprite_focused   — straight brows above the eyes
  frog_sprite_error     — X over each eye

Regenerate: python3 -I tools/gen_frog_expressions.py --out src/ui/frog_sprite.h
Transparent pixels are baked to UI_PAL_BG (0x0F1011 -> RGB565 0x0882); keep
them in sync with ui_palette.h when regenerating.
"""

import argparse
import sys

W = H = 16
BG = 0x0882   # RGB565 of UI_PAL_BG 0x0F1011
BK = 0x0000   # outline / pupil / mouth
GR = 0x6DE6   # frog green (106,190,48) -> RGB565

# 16 rows of 16 chars: '.'=BG  '#'=BK(black)  'G'=GR(green)
FROG_ROWS = [
    "...#.......#....",
    "..#G#.....#G#...",
    "..#G#.....#G#...",
    ".#GGGGGGGGGGGG#.",
    "#GG##GGGGGG##GG#",
    "#GG##GGGGGG##GG#",
    "#GGGGGGGGGGGGGG#",
    "#GGGGGGGGGGGGGG#",
    "#GGGG######GGGG#",
    "####GGGGGGGGG###",
    "...##########...",
    "...#GG####GG#...",
    ".###GG#..#GG###.",
    "#GG#GG#..#GG#GG#",
    "#GG#GG#..#GG#GG#",
    "#GG#GG#..#GG#GG#",
]
CMAP = {'.': BG, '#': BK, 'G': GR}

# Eye anchors: left pupil rows 4-5 cols 3-4; right pupil rows 4-5 cols 11-12.
def _frog_x_eye(c0):
    """3x3 X over one eye: black diagonals, green cross-arms."""
    return {
        (3, c0 + 0): BK, (3, c0 + 1): GR, (3, c0 + 2): BK,
        (4, c0 + 0): GR, (4, c0 + 1): BK, (4, c0 + 2): GR,
        (5, c0 + 0): BK, (5, c0 + 1): GR, (5, c0 + 2): BK,
    }

# Left eye 2x2 = (4,3)(4,4)(5,3)(5,4); right = (4,11)(4,12)(5,11)(5,12).
LE = ((4, 3), (4, 4), (5, 3), (5, 4))
RE = ((4, 11), (4, 12), (5, 11), (5, 12))

def _eyes_closed():
    d = {}
    for (r, c) in LE + RE:
        d[(r, c)] = GR
    return d

def _eyes_up():
    d = {}
    for (r, c) in LE:
        d[(r, c)] = GR  # clear lower row of the 2x2
    for (r, c) in RE:
        d[(r, c)] = GR
    # pupils land one row up
    d.update({(3, 3): BK, (3, 4): BK, (3, 11): BK, (3, 12): BK})
    return d

def _eyes_down():
    d = {}
    for (r, c) in LE:
        d[(r, c)] = GR  # clear upper row of the 2x2
    for (r, c) in RE:
        d[(r, c)] = GR
    d.update({(6, 3): BK, (6, 4): BK, (6, 11): BK, (6, 12): BK})
    return d

VARIANTS = [
    ("base", {}),
    ("blink", _eyes_closed()),
    ("eyes_up", _eyes_up()),
    ("eyes_down", _eyes_down()),
    ("focused", {(3, 3): BK, (3, 4): BK, (3, 11): BK, (3, 12): BK}),
    ("error", {**_frog_x_eye(3), **_frog_x_eye(11)}),
]


def base_grid():
    g = []
    for r in FROG_ROWS:
        assert len(r) == W, "row width %d: %r" % (len(r), r)
        g += [CMAP[c] for c in r]
    assert len(g) == W * H
    return g


def apply_edits(grid, edits):
    g = list(grid)
    for (r, c), v in edits.items():
        assert 0 <= r < H and 0 <= c < W, "coord out of range %r" % ((r, c),)
        g[r * W + c] = v
    return g


def emit_map(name, grid):
    full = "frog_sprite_" + name
    lines = []
    for row in range(H):
        vals = []
        for col in range(W):
            v = grid[row * W + col]
            vals.append("0x%02x, 0x%02x" % (v & 0xFF, v >> 8))
        lines.append("    " + ", ".join(vals) + ",")
    return "static const uint8_t %s_map[] = {\n%s\n};\n" % (full, "\n".join(lines))


def emit_dsc(name):
    full = "frog_sprite_" + name
    return (
        "static const lv_image_dsc_t %s = {\n"
        "    .header.magic = LV_IMAGE_HEADER_MAGIC,\n"
        "    .header.cf = LV_COLOR_FORMAT_RGB565,\n"
        "    .header.w = %d,\n"
        "    .header.h = %d,\n"
        "    .header.stride = %d,\n"
        "    .data = %s_map,\n"
        "    .data_size = sizeof(%s_map),\n"
        "};\n" % (full, W, H, W * 2, full, full)
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", help="write frog_sprite.h here (default stdout)")
    args = ap.parse_args()

    base = base_grid()

    hdr = [
        "/*",
        " * frog_sprite.h — pixel-art frog sprite (CC0, auto-generated).",
        " *",
        " * Source asset: OpenGameArt all_43.png by davidcrybao (CC0 public domain).",
        " *   https://opengameart.org/sites/default/files/all_43.png",
        " * Tile 18: 16x16 green frog face (RGB 106,190,48), scaled 6x at runtime (96px).",
        " *",
        " * Six facial-expression frames, eyes-only edits on the base face.",
        " * State mapping lives in pet_status_view.c:",
        " *   base      — IDLE / WAITING open-eye frame",
        " *   blink     — WAITING closed-eye frame",
        " *   eyes_up   — THINKING (pupils shifted up)",
        " *   eyes_down — LISTENING (pupils shifted down)",
        " *   focused   — EXECUTING (straight brows)",
        " *   error     — ERROR (X eyes)",
        " *",
        " * Regenerate: python3 -I tools/gen_frog_expressions.py --out src/ui/frog_sprite.h",
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
    ]

    for name, edits in VARIANTS:
        grid = apply_edits(base, edits)
        hdr.append(emit_map(name, grid))
        hdr.append("")

    for name, _ in VARIANTS:
        hdr.append(emit_dsc(name))

    hdr.append("")
    hdr.append("#endif /* LV_USE_IMAGE */")
    hdr.append("")

    out = "\n".join(hdr)
    if args.out:
        with open(args.out, "w") as f:
            f.write(out)
        print("wrote %s" % args.out, file=sys.stderr)
    else:
        sys.stdout.write(out)


if __name__ == "__main__":
    main()
