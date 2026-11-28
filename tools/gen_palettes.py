#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
"""Melodee's screen palettes (Settings > SCREEN > Palette) into src/melodee/ui/palettes.h.

NIGHT is the design's own (ui/tokens.h, ui/shades.h) and is written as it is. The others are FM-1 Melodee's
(mpc-sample-plugins tools/gen_ui_palettes.py: BG, SURF, TEXT) with every other token worked out from them:

  PANEL, QUIET   between BG and SURF          RAISED  SURF a step towards TEXT
  LINE           BG towards TEXT (dividers)   SEC, MID, DIM  BG towards TEXT, raised until they pass the checks

Each track colour keeps its hue (track 1 stays coral, so the pads, which keep their own colours, still match):
its chroma is scaled by the palette's own amount, then its lightness moved away from BG until it reads as
values and labels do (4.5:1 on BG, SURF and RAISED). REC, AMBER, PLAY and ICE are treated the same way.

Run: python3 tools/gen_palettes.py (writes the header; prints each palette's weakest pair).
"""
from pathlib import Path
import math
import re

ROOT = Path(__file__).resolve().parents[1]


def hexrgb(v):
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255)


def rgbhex(c):
    return (c[0] << 16) | (c[1] << 8) | c[2]


def lin(u):
    u /= 255
    return u / 12.92 if u <= 0.04045 else ((u + 0.055) / 1.055) ** 2.4


def unlin(u):
    u = max(0.0, min(1.0, u))
    return round(255 * (12.92 * u if u <= 0.0031308 else 1.055 * u ** (1 / 2.4) - 0.055))


def luminance(c):
    r, g, b = (lin(x) for x in c)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def contrast(a, b):
    la, lb = luminance(a), luminance(b)
    return (max(la, lb) + 0.05) / (min(la, lb) + 0.05)


def to_oklch(c):
    r, g, b = (lin(x) for x in c)
    l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b
    m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b
    s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b
    l, m, s = (math.copysign(abs(x) ** (1 / 3), x) for x in (l, m, s))
    L = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s
    A = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s
    B = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s
    return L, math.hypot(A, B), math.atan2(B, A)


def from_oklch(L, C, h):
    A, B = C * math.cos(h), C * math.sin(h)
    l = (L + 0.3963377774 * A + 0.2158037573 * B) ** 3
    m = (L - 0.1055613458 * A - 0.0638541728 * B) ** 3
    s = (L - 0.0894841775 * A - 1.2914855480 * B) ** 3
    r = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s
    g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s
    b = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s
    return (r, g, b)


def in_gamut(rgb):
    return all(-1e-4 <= x <= 1 + 1e-4 for x in rgb)


def oklch_rgb(L, C, h):
    """The colour, its chroma lowered until sRGB can show it."""
    while C > 0 and not in_gamut(from_oklch(L, C, h)):
        C -= 0.002
    return tuple(unlin(x) for x in from_oklch(L, max(C, 0), h))


def mix(a, b, t):
    return tuple(round(a[n] + (b[n] - a[n]) * t) for n in range(3))


# NIGHT: the design's own tokens, as ui/tokens.h and ui/shades.h had them before palettes.
NIGHT = dict(bg=0x17141F, panel=0x1D1926, surf=0x221E2C, raised=0x262033, quiet=0x1A1722, line=0x2E2939, text=0xF3EEF8,
             sec=0xC9C2D6, mid=0x9A93A8, dim=0x5A5468, rec=0xFF4848, amber=0xFFC145, play=0x5FD97A, ice=0xBFE3FF)


def night_shades():
    src = (ROOT / "src/melodee/ui/shades.h").read_text()
    def table(name, rows):
        body = src[src.index(name):]
        body = body[:body.index("}};")]
        vals = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{6})", body)]
        return [vals[3 * n:3 * n + 3] for n in range(rows)]
    return table("track_shades", 16), table("loop_shades", 4), table("sub_shades", 8)


# name, BG, SURF, TEXT (FM-1's), chroma scale, lightness shift of the track colours (before the checks)
PALETTES = [
    ("DAY",      (246, 242, 234), (231, 224, 212), (30, 26, 38),    1.00, 0.0),
    ("CONTRAST", (0, 0, 0),       (34, 34, 34),    (255, 255, 255), 1.00, 0.06),
    ("SUNSET",   (34, 16, 34),    (54, 28, 52),    (255, 236, 240), 1.00, 0.0),
    ("OCEAN",    (12, 22, 40),    (24, 40, 64),    (232, 244, 255), 0.90, 0.0),
    ("NEON",     (10, 8, 20),     (28, 24, 46),    (244, 242, 255), 1.35, 0.04),
    ("CANDY",    (255, 241, 246), (247, 224, 236), (44, 24, 48),    1.10, 0.0),
    ("LO-FI",    (32, 28, 26),    (48, 42, 38),    (236, 226, 208), 0.55, -0.02),
]

# LCD: an old sampler's backlit LCD, no colour. Dark ink on a grey-green ground; every track, knob and state in
# the ink (lighter inks for the knob shades), and the screen always in Melodee Pixel (FACE).
LCD = ("LCD", (167, 181, 150), (152, 166, 136), (24, 33, 24))
FACE = {"LCD": 6}  # a palette that sets its own face (ui/faces.h)

READ = 4.5       # values, labels, a track's colour: on BG, SURF and RAISED
TEXT_MIN = 7.0   # SEC (names) on BG
MID_MIN = 4.6    # MID (labels, units) on BG and SURF
DIM_MIN = 2.2    # DIM: inactive, not read


def fit_colour(rgb, grounds, chroma, shift, light):
    """The colour at its own hue, its lightness moved away from the grounds until it reads on each."""
    L, C, h = to_oklch(rgb)
    C *= chroma
    L += -shift if light else shift
    way = -1 if light else 1
    out = oklch_rgb(L, C, h)
    for _ in range(200):
        if all(contrast(out, g) >= READ for g in grounds): break
        L += way * 0.005
        if not 0 <= L <= 1: break
        out = oklch_rgb(L, C, h)
    return out


def neutral_at(bg, text, start, ground, need):
    t = start
    c = mix(bg, text, t)
    while any(contrast(c, g) < need for g in ground) and t < 1:
        t += 0.01
        c = mix(bg, text, t)
    return c


def build(name, bg, surf, text, chroma, shift, tracks, loops, subs):
    light = luminance(bg) > 0.4
    p = dict(bg=bg, surf=surf, text=text)
    p["panel"] = mix(bg, surf, 0.45)
    p["quiet"] = mix(bg, surf, 0.2)
    p["raised"] = mix(surf, text, 0.06 if light else 0.05)
    p["line"] = mix(bg, text, 0.16 if light else 0.12)
    p["sec"] = neutral_at(bg, text, 0.80, [bg, p["raised"]], TEXT_MIN)
    p["mid"] = neutral_at(bg, text, 0.60, [bg, surf], MID_MIN)
    p["dim"] = neutral_at(bg, text, 0.30, [bg], DIM_MIN)
    grounds = [bg, surf, p["raised"]]
    for key in ("rec", "amber", "play", "ice"):
        p[key] = fit_colour(hexrgb(NIGHT[key]), grounds, chroma, shift, light)
    p["tracks"] = [[fit_colour(hexrgb(v), grounds, chroma, shift, light) for v in row] for row in tracks]
    p["loops"] = [[fit_colour(hexrgb(v), grounds, chroma, shift, light) for v in row] for row in loops]
    p["subs"] = [[fit_colour(hexrgb(v), grounds, chroma, shift, light) for v in row] for row in subs]
    p["light"] = light
    return p


def build_lcd(name, bg, surf, text):
    p = dict(bg=bg, surf=surf, text=text, light=True)
    p["panel"] = mix(bg, surf, 0.45)
    p["quiet"] = mix(bg, surf, 0.2)
    p["raised"] = mix(surf, text, 0.08)
    p["line"] = mix(bg, text, 0.25)
    p["sec"] = neutral_at(bg, text, 0.85, [bg, p["raised"]], TEXT_MIN)
    p["mid"] = neutral_at(bg, text, 0.70, [bg, surf], MID_MIN)
    p["dim"] = neutral_at(bg, text, 0.40, [bg], DIM_MIN)
    grounds = [bg, surf, p["raised"]]
    shades = [text, neutral_at(bg, text, 0.85, grounds, READ), neutral_at(bg, text, 0.78, grounds, READ)]
    for key in ("rec", "amber", "play", "ice"):
        p[key] = text
    p["tracks"] = [list(shades) for _ in range(16)]
    p["loops"] = [list(shades) for _ in range(4)]
    p["subs"] = [list(shades) for _ in range(8)]
    return p


def weakest(p):
    pairs = [("text", "bg"), ("sec", "bg"), ("mid", "bg"), ("mid", "surf"), ("dim", "bg"), ("rec", "bg"), ("amber", "bg"),
             ("play", "bg"), ("ice", "bg")]
    worst = min(((contrast(p[a], p[b]), f"{a}/{b}") for a, b in pairs))
    tr = min(contrast(c, g) for row in p["tracks"] for c in row for g in (p["bg"], p["surf"], p["raised"]))
    return worst, tr


def main():
    tracks, loops, subs = night_shades()
    night = {k: hexrgb(v) for k, v in NIGHT.items()}
    night["tracks"] = [[hexrgb(v) for v in row] for row in tracks]
    night["loops"] = [[hexrgb(v) for v in row] for row in loops]
    night["subs"] = [[hexrgb(v) for v in row] for row in subs]
    night["light"] = False
    all_ = [("NIGHT", night)] + [(n, build(n, bg, surf, text, c, s, tracks, loops, subs)) for n, bg, surf, text, c, s in PALETTES]
    all_.append((LCD[0], build_lcd(*LCD)))

    keys = ["bg", "panel", "surf", "raised", "quiet", "line", "text", "sec", "mid", "dim", "rec", "amber", "play", "ice"]
    h = lambda c: f"0x{rgbhex(c):06X}"
    lines = ["// SPDX-License-Identifier: AGPL-3.0-only",
             "// Copyright (C) 2026 Kerem Kilic (Ellic Studio)",
             "// Written by tools/gen_palettes.py: change the script, not this file.",
             "#pragma once",
             "#include <array>",
             "#include <cstdint>",
             "",
             "// The screen's palettes (Settings > SCREEN > Palette; ui/tokens.h use_palette()). NIGHT is the design's",
             "// own; the others keep every track's hue and move its lightness until it reads on that palette's grounds.",
             "// The pads keep ui/shades.h: a palette changes the screen only.",
             "namespace melodee::ui {",
             "",
             "struct PaletteSpec {",
             "    const char* name;",
             "    uint32_t " + ", ".join(keys) + ";",
             "    std::array<std::array<uint32_t, 3>, 16> tracks;",
             "    std::array<std::array<uint32_t, 3>, 4> loops;",
             "    std::array<std::array<uint32_t, 3>, 8> subs; // the mixer's submixes",
             "    bool light; // a light ground (DAY, CANDY, LCD)",
             "    int face;   // a face of its own (ui/faces.h), whatever Settings' Font says; -1 none",
             "};",
             "",
             f"constexpr std::array<PaletteSpec, {len(all_)}> palettes {{{{"]
    for name, p in all_:
        lines.append(f'    {{"{name}", ' + ", ".join(h(p[k]) for k in keys) + ",")
        lines.append("     {{" + ", ".join("{" + ", ".join(h(c) for c in row) + "}" for row in p["tracks"]) + "}},")
        lines.append("     {{" + ", ".join("{" + ", ".join(h(c) for c in row) + "}" for row in p["loops"]) + "}},")
        lines.append("     {{" + ", ".join("{" + ", ".join(h(c) for c in row) + "}" for row in p["subs"]) + "}},")
        lines.append(f"     {'true' if p['light'] else 'false'}, {FACE.get(name, -1)}}},")
        (w, which), tr = weakest(p)
        print(f"{name:9} weakest {which} {w:.2f}  tracks min {tr:.2f}")
    lines += ["}};", "", "} // namespace melodee::ui", ""]
    (ROOT / "src/melodee/ui/palettes.h").write_text("\n".join(lines))


if __name__ == "__main__":
    main()
