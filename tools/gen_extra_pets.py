#!/usr/bin/env python3
"""Generate 3 extra pet sprite headers: blackcat, dog, pig.

Each is a 16x16 front-facing animal face in the same two-tone pixel style as
pet_sprite.h (cat) and frog_sprite.h, with 2x2 pupils at rows 7-8 (cols 4-5 and
10-11) so the six eye-region expressions read clearly when scaled 6x to 96px.

  blackcat_sprite_*  — black face, white 2x2 pupils, white muzzle/chin
  dog_sprite_*       — brown face, droopy ears, black nose, pink tongue
  pig_sprite_*       — pink face, big ears, two black nostrils

All three use the same expression edit scheme (eyes-only, rows 6-9 around the
2x2 pupils): base / blink / eyes_up / eyes_down / focused (brows) / error (X).
Transparent pixels are baked to UI_PAL_BG (0x0F1011 -> RGB565 0x0882).

Regenerate: python3 -I tools/gen_extra_pets.py --out-dir src/ui
"""

import argparse
import os
import sys

W = H = 16
BG = 0x0882   # RGB565 of UI_PAL_BG 0x0F1011
BK = 0x0000   # black (pupil / nose / nostril / outline)
WH = 0xFFFF   # white (blackcat pupils + muzzle)
PK = 0xFDB8   # soft pink (pig face / dog tongue) ~ #FFB6C5
DP = 0xEC54   # deep pink (pig outline) ~ #E88AA0
BN = 0x8AC5   # dog brown (saddle brown) ~ #8B5A2B

# Each pet: name, rows, color map, and (eye_color, fill) for expression edits.
# '.'=BG everywhere; per-pet CMAP supplies the rest.
PETS = [
    {
        "name": "blackcat",
        "eye_color": WH, "fill": BK,
        "cmap": {'.': BG, '#': BK, 'W': WH},
        "rows": [
            "....#......#....",
            "...###....###...",
            "...####..####...",
            "...##########...",
            "..############..",
            "..############..",
            "..############..",
            "..##WW####WW##..",
            "..##WW####WW##..",
            "..############..",
            "..#####WW#####..",
            "..####WWWW####..",
            "..############..",
            "...##########...",
            "...#WWWWWWWW#...",
            "....########....",
        ],
    },
    {
        "name": "dog",
        "eye_color": BK, "fill": BN,
        "cmap": {'.': BG, 'B': BN, 'O': BK, 'K': BK, 'P': PK},
        "rows": [
            "................",
            ".BBBB......BBBB.",
            ".BBBB......BBBB.",
            ".BBBBBBBBBBBBBB.",
            ".BBBBBBBBBBBBBB.",
            ".BBBBBBBBBBBBBB.",
            ".BBBBBBBBBBBBBB.",
            ".BBBOOBBBBOOBBB.",
            ".BBBOOBBBBOOBBB.",
            ".BBBBBBBBBBBBBB.",
            ".BBBBKKKKKKBBBB.",
            ".BBBBBKKKKBBBBB.",
            ".BBBBPPPPPPPPBB.",
            ".BBBBPPPPPPPPBB.",
            ".BBBBBBBBBBBBBB.",
            "..BBBBBBBBBBBB..",
        ],
    },
    {
        "name": "pig",
        "eye_color": BK, "fill": PK,
        "cmap": {'.': BG, 'D': DP, 'P': PK, 'O': BK},
        "rows": [
            "................",
            ".DPPD......DPPD.",
            ".DPPD......DPPD.",
            ".DPPDDDDDDDDPPD.",
            ".DPPPPPPPPPPPPD.",
            ".DPPPPPPPPPPPPD.",
            ".DPPPPPPPPPPPPD.",
            ".DPPOOPPPPOOPPD.",
            ".DPPOOPPPPOOPPD.",
            ".DPPPPPPPPPPPPD.",
            ".DPPPPPPPPPPPPD.",
            ".DPPPPPPPPPPPPD.",
            ".DPPPPOOPPOOPPD.",
            ".DPPPPPPPPPPPPD.",
            "..DDDDDDDDDDDD..",
            "................",
        ],
    },
]

# Pupils are 2x2 at rows 7-8, cols 4-5 and 10-11 (all three pets).
LE = [(7, 4), (7, 5), (8, 4), (8, 5)]
RE = [(7, 10), (7, 11), (8, 10), (8, 11)]


def _eyes_closed(eye_color, fill):
    d = {}
    for (r, c) in LE + RE:
        d[(r, c)] = fill
    return d


def _eyes_up(eye_color, fill):
    d = {}
    for (r, c) in LE + RE:
        d[(r, c)] = fill  # clear the 2x2
    # pupils land one row up (row 6), bottom row (8) already cleared above
    d.update({(6, 4): eye_color, (6, 5): eye_color,
              (6, 10): eye_color, (6, 11): eye_color})
    return d


def _eyes_down(eye_color, fill):
    d = {}
    for (r, c) in LE + RE:
        d[(r, c)] = fill  # clear the 2x2
    d.update({(9, 4): eye_color, (9, 5): eye_color,
              (9, 10): eye_color, (9, 11): eye_color})
    return d


def _focused(eye_color, fill):
    return {(6, 4): eye_color, (6, 5): eye_color,
            (6, 10): eye_color, (6, 11): eye_color}


def _x_eye(c0, eye_color, fill):
    """3x3 X over one eye (rows 6-8): eye_color diagonals, fill cross-arms."""
    return {
        (6, c0 + 0): eye_color, (6, c0 + 1): fill, (6, c0 + 2): eye_color,
        (7, c0 + 0): fill, (7, c0 + 1): eye_color, (7, c0 + 2): fill,
        (8, c0 + 0): eye_color, (8, c0 + 1): fill, (8, c0 + 2): eye_color,
    }


def variants_for(pet):
    ec = pet["eye_color"]
    fl = pet["fill"]
    return [
        ("base", {}),
        ("blink", _eyes_closed(ec, fl)),
        ("eyes_up", _eyes_up(ec, fl)),
        ("eyes_down", _eyes_down(ec, fl)),
        ("focused", _focused(ec, fl)),
        ("error", {**_x_eye(3, ec, fl), **_x_eye(9, ec, fl)}),
    ]


def base_grid(pet):
    cmap = pet["cmap"]
    g = []
    for r in pet["rows"]:
        assert len(r) == W, "%s row width %d: %r" % (pet["name"], len(r), r)
        g += [cmap[c] for c in r]
    assert len(g) == W * H
    return g


def apply_edits(grid, edits):
    g = list(grid)
    for (r, c), v in edits.items():
        assert 0 <= r < H and 0 <= c < W, "coord out of range %r" % ((r, c),)
        g[r * W + c] = v
    return g


def emit_map(name, grid):
    full = name + "_map"
    lines = []
    for row in range(H):
        vals = []
        for col in range(W):
            v = grid[row * W + col]
            vals.append("0x%02x, 0x%02x" % (v & 0xFF, v >> 8))
        lines.append("    " + ", ".join(vals) + ",")
    return "static const uint8_t %s[] = {\n%s\n};\n" % (full, "\n".join(lines))


def emit_dsc(name):
    full = name
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


def emit_header(pet):
    pname = pet["name"]
    hdr = [
        "/*",
        " * %s_sprite.h — pixel-art %s sprite (CC0, auto-generated)." % (pname, pname),
        " *",
        " * 16x16 front-facing %s face, two-tone pixel style matching pet_sprite.h.",
        " * Six facial-expression frames, eyes-only edits on the base face.",
        " *   base      — IDLE / WAITING open-eye frame",
        " *   blink     — WAITING closed-eye frame",
        " *   eyes_up   — THINKING (pupils shifted up)",
        " *   eyes_down — LISTENING (pupils shifted down)",
        " *   focused   — EXECUTING (straight brows)",
        " *   error     — ERROR (X eyes)",
        " *",
        " * Regenerate: python3 -I tools/gen_extra_pets.py --out-dir src/ui",
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
    # replace the format placeholder line with concrete text
    hdr[3] = " * 16x16 front-facing %s face, two-tone pixel style matching pet_sprite.h." % pname

    variants = variants_for(pet)
    base = base_grid(pet)
    for name, edits in variants:
        grid = apply_edits(base, edits)
        hdr.append(emit_map("%s_sprite_%s" % (pname, name), grid))
        hdr.append("")

    for name, _ in variants:
        hdr.append(emit_dsc("%s_sprite_%s" % (pname, name)))

    hdr.append("")
    hdr.append("#endif /* LV_USE_IMAGE */")
    hdr.append("")
    return "\n".join(hdr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", help="directory to write the headers into")
    args = ap.parse_args()

    for pet in PETS:
        content = emit_header(pet)
        if args.out_dir:
            path = os.path.join(args.out_dir, "%s_sprite.h" % pet["name"])
            with open(path, "w") as f:
                f.write(content)
            print("wrote %s" % path, file=sys.stderr)
        else:
            sys.stdout.write(content)
            sys.stdout.write("\n\n")


if __name__ == "__main__":
    main()
