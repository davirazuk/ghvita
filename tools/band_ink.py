#!/usr/bin/env python3
# The band's cut-out look: a bold ink line round every frame of the sheets from tools/band.py, and a little more punch.
# usage: band_ink.py <sheets as band.py made them> [assets/band]
import glob, os, sys
import numpy as np
from PIL import Image, ImageEnhance, ImageFilter

SRC = sys.argv[1]
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'assets', 'band')
INK = (16, 9, 28)
CELLS = {'drummer': (266, 232)}
STAND = (170, 256)


def ink(cell):
    a = cell.getchannel('A').point(lambda v: 255 if v > 90 else 0)
    line = a.filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.GaussianBlur(0.6))
    under = Image.new('RGBA', cell.size, INK + (0,))
    under.putalpha(line)
    rgb = ImageEnhance.Contrast(ImageEnhance.Color(cell.convert('RGB')).enhance(1.15)).enhance(1.08)
    top = rgb.convert('RGBA')
    top.putalpha(cell.getchannel('A'))
    under.alpha_composite(top)
    return under


for path in sorted(glob.glob(os.path.join(SRC, '*.png'))):
    name = os.path.basename(path)
    sheet = Image.open(path).convert('RGBA')
    cw, ch = CELLS.get(name.split('.')[0].split('_')[0], STAND)
    out = Image.new('RGBA', sheet.size, (0, 0, 0, 0))
    for y in range(0, sheet.height, ch):
        for x in range(0, sheet.width, cw):
            cell = sheet.crop((x, y, x + cw, y + ch))
            if cell.getchannel('A').getbbox():
                out.paste(ink(cell), (x, y))
    out.quantize(256, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE).save(os.path.join(OUT, name), optimize=True)
    print(name, os.path.getsize(os.path.join(OUT, name)) // 1024, 'KB')
