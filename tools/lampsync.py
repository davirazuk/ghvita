#!/usr/bin/env python3
"""Streak lamps synced from the real GHWT crops.

The atlas rides along from the last release's vpk (nothing artful is in the repo), so a lamp
art change reaches the next build only by patching that atlas: every lamp_<state><hue> cell
is pasted over with tools/lampsrc/light_<state><hue>.png (32x32 source, scaled to the cell).
Sprite names and their cells are untouched, so game.txt needs no change.

    python3 tools/lampsync.py [assets/game.png] [assets/game.txt] [tools/lampsrc]
"""
import os
import sys
from PIL import Image

STATES = range(3)
HUES = ('', '_green', '_purple', '_blue')


def sync(png_path, txt_path, src_dir):
    atlas = Image.open(png_path).convert('RGBA')
    names = ['lamp_%d%s' % (st, hue) for st in STATES for hue in HUES]
    cells = {}
    for line in open(txt_path):
        parts = line.split()
        if len(parts) == 5 and parts[0] in names:
            cells[parts[0]] = tuple(int(v) for v in parts[1:])
    for name in names:
        if name not in cells:
            continue
        src = Image.open(os.path.join(src_dir, name.replace('lamp_', 'light_', 1) + '.png')).convert('RGBA')
        x, y, w, h = cells[name]
        atlas.paste(src.resize((w, h), Image.LANCZOS), (x, y))
    atlas.save(png_path)
    print('%s: %d lamp cells synced' % (os.path.basename(png_path), len(cells)))


if __name__ == '__main__':
    here = os.path.dirname(os.path.abspath(__file__))
    base = os.path.dirname(here)
    png = sys.argv[1] if len(sys.argv) > 1 else os.path.join(base, 'assets/game.png')
    txt = sys.argv[2] if len(sys.argv) > 2 else os.path.join(base, 'assets/game.txt')
    src = sys.argv[3] if len(sys.argv) > 3 else os.path.join(here, 'lampsrc')
    sync(png, txt, src)
