#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
"""Convert the actual SDK framebuffers and arrange a review sheet; never changes panel art."""
from pathlib import Path
import sys
from PIL import Image, ImageDraw, ImageFont

out = Path(sys.argv[1])
font_path = out / 'upstream/skin/fonts/LiberationSans-Regular.ttf'
font = ImageFont.truetype(str(font_path), 21)
small = ImageFont.truetype(str(font_path), 16)
panels = out / 'panels'
for ppm in panels.glob('*.ppm'):
    Image.open(ppm).save(ppm.with_suffix('.png'))
sheet = Image.new('RGB', (1048, 610), '#101210')
draw = ImageDraw.Draw(sheet)
draw.text((24, 18), 'PROFIT-5 / MELODEE SCREEN SDK', font=font, fill='#f0e6cd')
draw.text((24, 47), 'Local UI experiment   /   320 x 218 native panels   /   Kerem Kilic - Ellic Studio', font=small, fill='#b0b0a0')
for i, (row, name) in enumerate([(0, '01 / OSCILLATORS'), (6, '02 / FILTER'), (8, '03 / ENVELOPES'),
                                  (12, '04 / MODULATION'), (18, '05 / VOICE'), (21, '06 / PATCH')]):
    x, y = 24 + (i % 3) * 344, 92 + (i // 3) * 256
    draw.text((x, y), name, font=small, fill='#b0b0a0')
    sheet.paste(Image.open(panels / f'{row}.png'), (x, y + 24))
sheet.save(out / 'profit5-overview.png')
Image.open(panels / '6.png').resize((960, 654), Image.Resampling.NEAREST).save(out / 'profit5-filter-3x.png')
