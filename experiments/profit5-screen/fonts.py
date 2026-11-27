#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Rasterise the pinned upstream's OFL Liberation Sans into build-only C arrays."""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

font_dir, output = map(Path, sys.argv[1:])
parts = ['// Generated from Liberation Sans (SIL OFL 1.1); see the upstream skin/fonts licence.']
for name, height in [('small', 14), ('value', 18), ('title', 25)]:
    font = ImageFont.truetype(str(font_dir / 'LiberationSans-Bold.ttf'), height)
    glyphs, pixels = [], []
    for code in range(32, 127):
        ch = chr(code)
        width = max(1, round(font.getlength(ch)))
        image = Image.new('L', (width, height + 4))
        ImageDraw.Draw(image).text((0, -2), ch, font=font, fill=255)
        glyphs.append((len(pixels), width))
        pixels.extend(image.getdata())
    parts.append(f'static constexpr Glyph {name}_glyphs[] = {{' + ','.join('{%d,%d}' % g for g in glyphs) + '};')
    parts.append(f'static constexpr unsigned char {name}_pixels[] = {{' + ','.join(map(str, pixels)) + '};')
    parts.append(f'static constexpr Font {name}_font {{{name}_glyphs, {name}_pixels, {height + 4}}};')
output.write_text('\n'.join(parts) + '\n')
