#!/usr/bin/env python3
# Each band member's picture for the options' band picker: one frame of their sheet (assets/band/<name>.png, as
# band_ink.py left it), cut to the figure, into assets/band/thumb/<name>.png. The drummer behind the kit in its idle,
# the others in the first frame of their groove (the singer at the mic).
# usage: band_thumbs.py [assets/band]
import glob, os, sys
from PIL import Image

BAND = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'assets', 'band')
OUT = os.path.join(BAND, 'thumb')
os.makedirs(OUT, exist_ok=True)
for path in sorted(glob.glob(os.path.join(BAND, '*.png'))):
    name = os.path.basename(path)[:-4]
    sheet = Image.open(path).convert('RGBA')
    cw = 266 if name.startswith('drummer') else sheet.width // 8
    ch = 232 if name.startswith('drummer') else round(cw * 256 / 170)
    f = 48 if name.startswith('drummer') else 0
    cell = sheet.crop(((f % 8) * cw, (f // 8) * ch, (f % 8 + 1) * cw, (f // 8 + 1) * ch))
    box = cell.getchannel('A').getbbox()
    if not box:
        continue
    cell = cell.crop(box)
    h = 256
    cell = cell.resize((max(1, round(cell.width * h / cell.height)), h), Image.LANCZOS)
    cell.save(os.path.join(OUT, name + '.png'), optimize=True)
    print(name, cell.size)
