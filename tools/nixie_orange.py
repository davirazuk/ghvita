#!/usr/bin/env python3
"""Make the normal (not Star Power) nixie tubes World Tour's art lacks: x6 and x8 come only lit blue ("b").
The colour change from the blue 4 to the normal 4 (both exist, same digit) is fitted per pixel as an affine RGB map,
then put on the blue 6 and 8. Writes hud_score_nixie_6a / _8a into assets/hud.png at free space and lists them in
hud.txt (re-run tools/rawtex.py after). usage: nixie_orange.py [assets dir]"""
import os, sys
import numpy as np
from PIL import Image

A = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'assets')
png, txt = os.path.join(A, 'hud.png'), os.path.join(A, 'hud.txt')
atlas = Image.open(png).convert('RGBA')
rects = {}
for line in open(txt):
    p = line.split()
    if len(p) == 5:
        rects[p[0]] = tuple(map(int, p[1:]))


def cut(name):
    x, y, w, h = rects[name]
    return np.asarray(atlas.crop((x, y, x + w, y + h))).astype(np.float64)


a4, b4 = cut('hud_score_nixie_4a'), cut('hud_score_nixie_4b')
m = (a4[..., 3] > 20) & (b4[..., 3] > 20)
X = np.c_[b4[m][:, :3], np.ones(m.sum())]
M, *_ = np.linalg.lstsq(X, a4[m][:, :3], rcond=None)
err = np.abs(X @ M - a4[m][:, :3]).mean()
print('fit error %.1f per channel' % err)

bottom = max(y + h for x, y, w, h in rects.values())
x0, y0 = 2, bottom + 4
lines = []
for d in ('6', '8'):
    name = 'hud_score_nixie_%sa' % d
    src = cut('hud_score_nixie_%sb' % d)
    rgb = np.clip(np.c_[src[..., :3].reshape(-1, 3), np.ones(src.shape[0] * src.shape[1])] @ M, 0, 255)
    out = np.dstack([rgb.reshape(src.shape[0], src.shape[1], 3), src[..., 3]]).astype(np.uint8)
    if name in rects:
        x, y, w, h = rects[name]
    else:
        x, y = x0, y0
        x0 += src.shape[1] + 4
        lines.append('%s %d %d %d %d\n' % (name, x, y, src.shape[1], src.shape[0]))
    atlas.paste(Image.fromarray(out, 'RGBA'), (x, y))
assert y0 + 128 <= atlas.size[1]
atlas.save(png)
with open(txt, 'a') as f:
    f.writelines(lines)
print('added', [l.split()[0] for l in lines])
