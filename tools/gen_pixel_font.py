#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
"""Melodee Pixel (src/melodee/ui/fonts/melodee_pixel.txt, FM-1 Melodee's 5 x 7 face) as TrueType fonts in
build/deps/fonts/melodeepixel/, for Settings > SCREEN > Font: tools/dependencies.py runs this before each
build and src/melodee/ui/font_data.cpp embeds the result. Each '#' is a square of 100 units in a 1000-unit
em: one screen pixel at 10 px, so ui/paint.cpp sets it at whole multiples of 10 px only. The glyphs Melodee
draws that the FM-1 face never needed (minus, times, dashes, angle quotes, arrow, at most, at least, one
half) are drawn here in the same grid. Plain Python: the font tables are written here, squares only.
"""
from pathlib import Path
import re

import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/melodee/ui/fonts/melodee_pixel.txt"
FOLDER = "build/deps/fonts/melodeepixel"  # MelodeePixel.ttf, and MelodeePixelNarrow.ttf (FM-1's narrow letters)
UNIT = 100  # one pixel
CAP = 7     # rows 0..6 are the capital; 7..8 the descenders

EXTRA = {  # codepoint: (top row, rows)
    0x2212: (0, [".....", ".....", ".....", ".....", "#####"]),              # minus, on the plus's bar
    0x00D7: (0, [".....", ".....", "#...#", ".#.#.", "..#..", ".#.#.", "#...#"]),  # times
    0x2013: (0, ["....", "....", "....", "....", "####"]),                   # en dash
    0x2014: (0, ["......", "......", "......", "......", "######"]),         # em dash
    0x2039: (0, ["...", "...", "..#", ".#.", "#..", ".#.", "..#"]),          # single angle quote left
    0x203A: (0, ["...", "...", "#..", ".#.", "..#", ".#.", "#.."]),          # single angle quote right
    0x2192: (0, [".....", ".....", "...#.", "....#", "#####", "....#", "...#."]),  # arrow right
    0x2264: (0, ["...#", "..#.", ".#..", "#...", ".#..", "..##", "##.."]),   # at most
    0x2265: (0, ["#...", ".#..", "..#.", "...#", "..#.", "##..", "..##"]),   # at least
    0x00BD: (0, ["#....", "#...#", "#..#.", "..#..", ".#.##", "#...#", "...#.", "..###"]),  # one half
}


def read_glyphs(narrow=False):
    """The glyphs, each its first drawing; with `narrow`, a 'narrow' drawing where there is one."""
    glyphs, narrows = {}, {}
    cp, is_narrow, top, rows = None, False, 0, []
    def done():
        if cp is None: return
        table = narrows if is_narrow else glyphs
        table.setdefault(cp, (top, rows))
    for line in SOURCE.read_text().splitlines():
        m = re.match(r"glyph U\+([0-9A-Fa-f]+)(.*)", line)
        if m:
            done()
            rest = m.group(2)
            cp, is_narrow = int(m.group(1), 16), "narrow" in rest.split("  ")[0]
            t = re.search(r"top (-?\d+)", rest)
            top, rows = (int(t.group(1)) if t else 0), []
        elif cp is not None and line and line[0] in ".#":
            rows.append(line)
        elif not line.strip():
            done()
            cp = None
    done()
    for k, v in EXTRA.items():
        glyphs.setdefault(k, v)
    if narrow:
        glyphs.update(narrows)
    return glyphs


def contours(top, rows):
    """Each run of ink in a row as a clockwise rectangle, in font units."""
    out = []
    for r, row in enumerate(rows):
        y1 = (CAP - (top + r)) * UNIT
        y0 = y1 - UNIT
        x = 0
        while x < len(row):
            if row[x] != "#":
                x += 1
                continue
            start = x
            while x < len(row) and row[x] == "#":
                x += 1
            out.append([(start * UNIT, y0), (start * UNIT, y1), (x * UNIT, y1), (x * UNIT, y0)])
    return out


def glyf_data(shapes):
    if not shapes:
        return b"", (0, 0, 0, 0)
    pts = [p for c in shapes for p in c]
    box = (min(p[0] for p in pts), min(p[1] for p in pts), max(p[0] for p in pts), max(p[1] for p in pts))
    out = struct.pack(">hhhhh", len(shapes), *box)
    end = -1
    for c in shapes:
        end += len(c)
        out += struct.pack(">H", end)
    out += struct.pack(">H", 0) + bytes([1]) * len(pts)  # no instructions; every point on the curve, two-byte deltas
    px = py = 0
    xs, ys = b"", b""
    for x, y in pts:
        xs += struct.pack(">h", x - px)
        ys += struct.pack(">h", y - py)
        px, py = x, y
    out += xs + ys
    return out + b"\0" * (-len(out) % 4), box


def checksum(data):
    data += b"\0" * (-len(data) % 4)
    return sum(struct.unpack(f">{len(data) // 4}I", data)) & 0xFFFFFFFF


def cmap_table(cps):
    """Format 4: a segment per run of consecutive codepoints (their glyphs are consecutive too)."""
    segs, start, first = [], None, 0
    for i, cp in enumerate(cps):
        if start is None:
            start, first = cp, i + 1
        if i + 1 == len(cps) or cps[i + 1] != cp + 1:
            segs.append((start, cp, (first - start) & 0xFFFF))
            start = None
    segs.append((0xFFFF, 0xFFFF, 1))
    n = len(segs)
    search = 2 * 2 ** (n.bit_length() - 1)
    body = b"".join(struct.pack(">H", e) for _, e, _ in segs) + b"\0\0"
    body += b"".join(struct.pack(">H", s) for s, _, _ in segs)
    body += b"".join(struct.pack(">H", d) for _, _, d in segs)
    body += b"\0\0" * n
    sub = struct.pack(">HHHHHHH", 4, 14 + len(body), 0, 2 * n, search, n.bit_length() - 1, 2 * n - search) + body
    return struct.pack(">HHHHI", 0, 1, 3, 1, 12) + sub


def name_table(family):
    names = {1: family, 2: "Regular", 4: family, 6: family.replace(" ", "")}
    records, strings = b"", b""
    for nid, text in names.items():
        data = text.encode("utf-16-be")
        records += struct.pack(">HHHHHH", 3, 1, 0x409, nid, len(data), len(strings))
        strings += data
    return struct.pack(">HHH", 0, len(names), 6 + len(records)) + records + strings


def build(glyphs, out, family):
    cps = sorted(glyphs)
    shapes = [[]] + [contours(*glyphs[cp]) for cp in cps]
    advances = [600] + [(max((len(r) for r in glyphs[cp][1]), default=3) + 1) * UNIT for cp in cps]
    glyf, loca, boxes = b"", [0], []
    for sh in shapes:
        data, box = glyf_data(sh)
        glyf += data
        loca.append(len(glyf))
        boxes.append(box)
    n = len(shapes)
    xmin, ymin = min(b[0] for b in boxes), min(b[1] for b in boxes)
    xmax, ymax = max(b[2] for b in boxes), max(b[3] for b in boxes)
    points = max(sum(len(c) for c in sh) for sh in shapes)
    conts = max(len(sh) for sh in shapes)
    tables = {
        b"head": struct.pack(">IIIIHHqqhhhhHHhhh", 0x10000, 0x10000, 0, 0x5F0F3CF5, 0x000B, 1000, 0, 0,
                             xmin, ymin, xmax, ymax, 0, 8, 2, 1, 0),
        b"hhea": struct.pack(">IhhhHhhhhhhhhhhhH", 0x10000, 900, -200, 0, max(advances), min(b[0] for b in boxes), 0,
                             xmax, 1, 0, 0, 0, 0, 0, 0, 0, n),
        b"maxp": struct.pack(">IHHHHHHHHHHHHHH", 0x10000, n, points, conts, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0),
        b"OS/2": struct.pack(">HhHHHhhhhhhhhhhh10sIIII4sHHHhhhHHIIhhHHH", 4, sum(advances) // n, 400, 5, 0,
                             650, 700, 0, 140, 650, 700, 0, 480, 50, 250, 0, bytes(10), 1, 0, 0, 0, b"MLDE",
                             0x40, cps[0], min(cps[-1], 0xFFFF), 900, -200, 0, 900, 200, 1, 0, 500, 700, 0, 32, 0),
        b"hmtx": b"".join(struct.pack(">Hh", a, b[0]) for a, b in zip(advances, boxes)),
        b"cmap": cmap_table(cps),
        b"loca": b"".join(struct.pack(">I", o) for o in loca),
        b"glyf": glyf,
        b"name": name_table(family),
        b"post": struct.pack(">IIhhIIIII", 0x30000, 0, -100, 50, 0, 0, 0, 0, 0),
    }
    tags = sorted(tables)
    count = len(tags)
    search = 16 * 2 ** (count.bit_length() - 1)
    font = struct.pack(">IHHHH", 0x10000, count, search, count.bit_length() - 1, 16 * count - search)
    offset = 12 + 16 * count
    body, head_at = b"", 0
    for tag in tags:
        data = tables[tag]
        if tag == b"head":
            head_at = offset
        font += struct.pack(">4sIII", tag, checksum(data), offset, len(data))
        padded = data + b"\0" * (-len(data) % 4)
        body += padded
        offset += len(padded)
    font += body
    adjust = (0xB1B0AFBA - checksum(font)) & 0xFFFFFFFF
    font = font[:head_at + 8] + struct.pack(">I", adjust) + font[head_at + 12:]
    out.parent.mkdir(parents=True, exist_ok=True)
    if not out.exists() or out.read_bytes() != font:  # unchanged: the assembler's input keeps its time
        out.write_bytes(font)


def prepare(root=ROOT):
    folder = Path(root) / FOLDER
    build(read_glyphs(), folder / "MelodeePixel.ttf", "Melodee Pixel")
    build(read_glyphs(narrow=True), folder / "MelodeePixelNarrow.ttf", "Melodee Pixel Narrow")


if __name__ == "__main__":
    prepare(sys.argv[1] if len(sys.argv) > 1 else ROOT)
