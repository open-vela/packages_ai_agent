#!/usr/bin/env python3
"""Generate pet_sprite.h — a 16x16 white cat face with 6 facial expressions.

The base face is a front-facing white cat: two upright ears with pink inner
ears, a round white face, two 2x2 black pupils, pink blush on the cheeks, a
pink nose and a small mouth, all on a black outline.  Six expression frames
are derived by editing only the eye region (ears/face/blush/nose stay
identical), so the agent state is readable at a glance:

  pet_sprite_base      — normal open eyes (IDLE / WAITING open frame)
  pet_sprite_blink     — closed eyes (WAITING closed frame)
  pet_sprite_eyes_up   — pupils shifted up one row  (THINKING)
  pet_sprite_eyes_down — pupils shifted down one row (LISTENING)
  pet_sprite_focused   — straight brows above the eyes (EXECUTING)
  pet_sprite_error     — X over each eye (ERROR)

Art direction is the CC0 white cat face from OpenGameArt all_43.png by
davidcrybao (public domain), recolored here with a soft pink accent
(#FFB6C5 -> RGB565 0xFDB8) and enlarged 2x2 pupils so the six expressions
read clearly when the 16px sprite is scaled 6x to 96px on the round LCD.

Regenerate: python3 -I tools/gen_pet_expressions.py --out src/ui/pet_sprite.h
Transparent pixels are baked to UI_PAL_BG (0x0F1011 -> RGB565 0x0882); keep
them in sync with ui_palette.h when regenerating.
"""

import argparse
import sys

W = H = 16
BG = 0x0882   # RGB565 of UI_PAL_BG 0x0F1011
BK = 0x0000   # outline / pupil / brow / mouth
WH = 0xFFFF   # face fill
PK = 0xFDB8   # soft pink (inner ear / blush / nose) ~ #FFB6C5

# Base face, 16 rows of 16 chars:  '.'=BG  'O'=BK(black)  'W'=WH(white)  'P'=PK(pink)
BASE_ROWS = [
    "................",
    "....OO....OO....",
    "...OWW....WWO...",
    "..OWWW....WWWO..",
    "..OWPP....PPWO..",
    ".OWWWWWWWWWWWWO.",
    ".OWWWWWWWWWWWWO.",
    ".OWWOO.WW.OOWWO.",
    ".OWWOO.WW.OOWWO.",
    ".OWWWWWWWWWWWWO.",
    ".OWWPPWWWWPPWWO.",
    ".OWWPPWWWWPPWWO.",
    ".OWWWWWWWWWWWWO.",
    ".OWWWWWPPWWWWWO.",
    "..OWWW.OO.WWWO..",
    "...OOOO..OOOO...",
]
CMAP = {'.': BG, 'O': BK, 'W': WH, 'P': PK}

# Expression edits, (row, col) -> color, applied on top of the base grid.
# Eye anchors: left pupil rows 7-8 cols 4-5; right pupil rows 7-8 cols 10-11.
def _x_eye(c0):
    """3x3 X over one eye: black diagonals, white cross-arms."""
    return {
        (6, c0 + 0): BK, (6, c0 + 1): WH, (6, c0 + 2): BK,
        (7, c0 + 0): WH, (7, c0 + 1): BK, (7, c0 + 2): WH,
        (8, c0 + 0): BK, (8, c0 + 1): WH, (8, c0 + 2): BK,
    }

VARIANTS = [
    ("base", {}),
    ("blink", {(7, 4): WH, (7, 5): WH, (7, 10): WH, (7, 11): WH}),
    ("eyes_up", {(6, 4): BK, (6, 5): BK, (8, 4): WH, (8, 5): WH,
                 (6, 10): BK, (6, 11): BK, (8, 10): WH, (8, 11): WH}),
    ("eyes_down", {(7, 4): WH, (7, 5): WH, (9, 4): BK, (9, 5): BK,
                   (7, 10): WH, (7, 11): WH, (9, 10): BK, (9, 11): BK}),
    ("focused", {(5, 4): BK, (5, 5): BK, (5, 10): BK, (5, 11): BK}),
    ("error", {**_x_eye(3), **_x_eye(9)}),
]


def base_grid():
    g = []
    for r in BASE_ROWS:
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
    full = "pet_sprite_" + name
    lines = []
    for row in range(H):
        vals = []
        for col in range(W):
            v = grid[row * W + col]
            vals.append("0x%02x, 0x%02x" % (v & 0xFF, v >> 8))
        lines.append("    " + ", ".join(vals) + ",")
    return "static const uint8_t %s_map[] = {\n%s\n};\n" % (full, "\n".join(lines))


def emit_dsc(name):
    full = "pet_sprite_" + name
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
    ap.add_argument("--out", help="write pet_sprite.h here (default stdout)")
    args = ap.parse_args()

    base = base_grid()

    hdr = [
        "/*",
        " * pet_sprite.h — pixel-art pet sprite (CC0, auto-generated).",
        " *",
        " * Source asset: OpenGameArt all_43.png by davidcrybao (CC0 public domain).",
        " *   https://opengameart.org/sites/default/files/all_43.png",
        " * Tile 13: 16x16 white cat/dog face, recolored here with a soft pink",
        " * accent (#FFB6C5) and enlarged 2x2 pupils, scaled 6x at runtime (96px).",
        " *",
        " * Six facial-expression frames, eyes-only edits on the base face (ears,",
        " * blush, nose, mouth stay identical). State mapping lives in pet_status_view.c:",
        " *   base      — IDLE / WAITING open-eye frame",
        " *   blink     — WAITING closed-eye frame (swap in the blink anim)",
        " *   eyes_up   — THINKING (pupils shifted up)",
        " *   eyes_down — LISTENING (pupils shifted down)",
        " *   focused   — EXECUTING (straight brows)",
        " *   error     — ERROR (X eyes)",
        " *",
        " * Regenerate: python3 -I tools/gen_pet_expressions.py --out src/ui/pet_sprite.h",
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
