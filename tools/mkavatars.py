#!/usr/bin/env python3
"""Guitar Hero pictures for the players' cards: assets/avatars/*.jpg, 256x256.
usage: mkavatars.py TEXTURES [OUT]
  TEXTURES  YargLook's decoded GHWT textures (the source-art release's ghwt-textures.7z:
            mods/yarg-look/YargLook/textures)
  OUT       default: assets/avatars next to tools/
The logos, the griffin and the magazines GHWT puts on its menus, each on one of its menu backgrounds. The game lists the folder: names sort into the order shown."""
import os, sys, glob
from PIL import Image, ImageEnhance, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
TEX = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser('~/mods/yarg-look/YargLook/textures')
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, '..', 'assets', 'avatars')
S = 256
os.makedirs(OUT, exist_ok=True)
for old in glob.glob(os.path.join(OUT, '*.jpg')):
    os.remove(old)


def tex(name):
    return Image.open(os.path.join(TEX, name)).convert('RGBA')


def square(im, focus=0.5):
    """the middle square of a picture (focus: where across it, 0 left .. 1 right), at S x S"""
    w, h = im.size
    side = min(w, h)
    x = int((w - side) * focus)
    y = (h - side) // 2
    return im.crop((x, y, x + side, y + side)).resize((S, S), Image.LANCZOS)


def backdrop(name, focus, dim=0.55):
    bg = square(tex(name).convert('RGB'), focus)
    return ImageEnhance.Brightness(bg).enhance(dim)


def on(bg, art, scale, y=0.5, glow=True):
    """art centred on the backdrop at scale of its width, with a soft dark halo so it reads on any part of it"""
    w = int(S * scale)
    h = int(art.height * w / art.width)
    if h > S * 0.9:
        h = int(S * 0.9)
        w = int(art.width * h / art.height)
    a = art.resize((w, h), Image.LANCZOS)
    x, top = (S - w) // 2, int(S * y - h / 2)
    out = bg.convert('RGBA')
    if glow:
        halo = Image.new('RGBA', out.size, (0, 0, 0, 0))
        shadow = Image.new('RGBA', a.size, (0, 0, 0, 200))
        halo.paste(shadow, (x, top), a.split()[3])
        out = Image.alpha_composite(out, halo.filter(ImageFilter.GaussianBlur(10)))
    out.alpha_composite(a, (x, top))
    return out.convert('RGB')


made = []


def save(name, im):
    im.save(os.path.join(OUT, name + '.jpg'), quality=90)
    made.append(name)


# GHWT's plain dark menu backgrounds (its title one has words on it; the others are strips)
backs = ['ghwt_menu_bg.png']
emblems = [('01_world_tour', 'ghwt_logo_GH4_LoR_256.png', 0.92, 0), ('02_guitar_hero', 'gh_logo.png', 0.9, 1),
           ('03_griffin', 'ghwt_setlist_griffin.png', 0.88, 2)]
for name, art, scale, b in emblems:
    save(name, on(backdrop(backs[b % len(backs)], 0.3), tex(art), scale))
mags = ['Kerrang', 'AP', 'Decibel', 'GPlayer', 'Paste', 'HELL', 'BACKYARD', 'BAR']
for k, m in enumerate(mags):
    save('%02d_mag_%s' % (10 + k, m.lower()), on(backdrop(backs[k % len(backs)], (k % 3) / 2, 0.45),
                                                 tex('ghwt_mag_%s.png' % m), 0.94))

print(len(made), 'pictures in', OUT)
