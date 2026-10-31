#!/usr/bin/env python3
# What the venue adds to its picture, drawn here: assets/stage.png and stage.txt (an atlas: name x y w h).
# White where the game tints it; the crowd is dark with a lit edge, so the stage lights catch it.
import math, os, random
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'assets')
SIZE = 1024
atlas = Image.new('RGBA', (SIZE, SIZE), (255, 255, 255, 0))
entries = []
SS = 3
rng = random.Random(11)


def put(name, im, x, y):
    atlas.paste(im, (x, y))
    entries.append('%s %d %d %d %d' % (name, x, y, im.width, im.height))


def white(alpha):
    a = np.clip(alpha, 0, 1)
    px = np.zeros(a.shape + (4,), np.uint8)
    px[..., :3] = 255
    px[..., 3] = (a * 255).astype(np.uint8)
    return Image.fromarray(px, 'RGBA')


def dot():
    n = 64
    y, x = np.mgrid[0:n, 0:n] / (n - 1) * 2 - 1
    return white(np.clip(1 - np.hypot(x, y), 0, 1) ** 1.6)


def beam():
    w, h = 128, 256
    y, x = np.mgrid[0:h, 0:w]
    f = y / (h - 1)
    half = 3 + f * 58
    across = np.clip(1 - np.abs(x - (w - 1) / 2) / half, 0, 1)
    return white(across ** 1.2 * (1 - f) ** 0.8)


def star():
    n = 48
    y, x = np.mgrid[0:n, 0:n] / (n - 1) * 2 - 1
    r, t = np.hypot(x, y), np.arctan2(y, x)
    edge = 0.34 + 0.62 * np.abs(np.cos(2 * t)) ** 3
    return white(np.clip((edge - r) * 9, 0, 1))


def confetti():
    im = Image.new('L', (24 * SS, 24 * SS), 0)
    ImageDraw.Draw(im).rounded_rectangle((6 * SS, 2 * SS, 18 * SS, 22 * SS), 2 * SS, fill=255)
    return white(np.asarray(im.resize((24, 24), Image.BOX)) / 255)


def person(d, x, base, s, arms):
    # a fan seen from behind: shoulders, a head, maybe hands in the air
    r = 15 * s
    y = base - 58 * s
    d.ellipse((x - r, y - r, x + r, y + r), fill=255)
    d.rounded_rectangle((x - 30 * s, y + r * 0.8, x + 30 * s, base + 40), 16 * s, fill=255)
    for side in (-1, 1):
        if arms == 2 or (arms == 1 and side == 1):
            hx, hy = x + side * (34 + rng.uniform(-6, 12)) * s, y - rng.uniform(34, 62) * s
            d.line((x + side * 24 * s, y + 24 * s, hx, hy), fill=255, width=int(13 * s))
            d.ellipse((hx - 9 * s, hy - 9 * s, hx + 9 * s, hy + 9 * s), fill=255)


def fan(arms):
    # one of the crowd, alone so each can jump for themselves
    w, h = 112, 150
    m = Image.new('L', (w * SS, h * SS), 0)
    person(ImageDraw.Draw(m), w / 2 * SS, (h - 10) * SS, SS * rng.uniform(0.9, 1.1), arms)
    a = np.asarray(m.resize((w, h), Image.BOX), float) / 255
    # the lit edge: what the stage lights catch from above
    shifted = np.roll(a, 3, axis=0)
    shifted[:3] = 0
    rim = np.clip(a - shifted, 0, 1)
    rim = np.asarray(Image.fromarray((rim * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.7)), float) / 255
    v = 26 + np.clip(rim * 1.6, 0, 1) * 229
    px = np.zeros((h, w, 4), np.uint8)
    px[..., 0] = px[..., 1] = px[..., 2] = v.astype(np.uint8)
    px[..., 3] = (a * 255).astype(np.uint8)
    return Image.fromarray(px, 'RGBA')


def truss():
    w, h = 256, 32
    im = Image.new('RGBA', (w * SS, h * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = (58, 56, 70, 255)
    lit = (120, 118, 136, 255)
    for k, y in enumerate((2, h - 4)):
        d.rectangle((0, y * SS, w * SS, (y + 3) * SS), fill=lit if k == 0 else c)
    for x in range(0, w, 16):
        d.line((x * SS, 3 * SS, (x + 16) * SS, (h - 3) * SS), fill=c, width=2 * SS)
        d.line(((x + 16) * SS, 3 * SS, x * SS, (h - 3) * SS), fill=c, width=2 * SS)
    return im.resize((w, h), Image.BOX)


def lamp():
    w, h = 40, 48
    im = Image.new('RGBA', (w * SS, h * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((8 * SS, 0, 32 * SS, 28 * SS), 5 * SS, fill=(30, 28, 38, 255), outline=(12, 8, 20, 255), width=2 * SS)
    d.ellipse((6 * SS, 22 * SS, 34 * SS, 44 * SS), fill=(12, 8, 20, 255))
    d.ellipse((10 * SS, 25 * SS, 30 * SS, 41 * SS), fill=(255, 255, 255, 255))
    return im.resize((w, h), Image.BOX)


def haze():
    n = 256
    rs = np.random.RandomState(4)
    a = np.zeros((128, n))
    for octave, amp in ((4, 1), (8, 0.5), (16, 0.25)):
        g = rs.rand(octave // 2 + 2, octave + 1)
        g[:, -1] = g[:, 0]
        a += amp * np.asarray(Image.fromarray((g * 255).astype(np.uint8)).resize((n, 128), Image.BICUBIC), float) / 255
    a = (a - a.min()) / (a.max() - a.min())
    y = np.linspace(0, 1, 128)[:, None]
    return white(a ** 1.5 * np.sin(y * math.pi) ** 1.5)


# fan_0-3 stand, fan_4-7 have their arms up
for i in range(8):
    put('fan_%d' % i, fan(0 if i < 4 else 1 + i % 2), i * 114, 0)
put('beam', beam(), 0, 152)
put('haze', haze(), 130, 152)
put('dot', dot(), 388, 152)
put('star', star(), 454, 152)
put('confetti', confetti(), 504, 152)
put('lamp', lamp(), 530, 152)
put('truss', truss(), 574, 152)
atlas.save(os.path.join(OUT, 'stage.png'), optimize=True)
open(os.path.join(OUT, 'stage.txt'), 'w').write('\n'.join(entries) + '\n')
print(os.path.getsize(os.path.join(OUT, 'stage.png')) // 1024, 'KB')
