#!/usr/bin/env python3
# Renders every decoded GHWT venue (YargLook's textures folder) with tools/venue_render.c and makes GH Vita's
# assets/venues/<zone>.jpg (the stage, graded like YargLook's VenueLook: contrast, saturation, warm filter, vignette)
# and <zone>_glow.jpg (its lights on black with a soft bloom, 480x272), 960x544 (half the memory of a 1024x1024 texture),
# plus venues.txt: "<zone>|<title>|<role x y pixels-per-metre> ..." for the game.
# usage: tools/venues.py <textures dir> <render work dir> [zone ...]   (needs a GL display: DISPLAY=:77)
import os, subprocess, sys
from PIL import Image, ImageChops, ImageEnhance, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'assets', 'venues')
RENDER = os.path.join(HERE, '..', 'build-tools', 'venue_render')
# back, up, look height, fov, lens shift up (half screens): the shift puts the band above and beside the highway
CAMERA = ['10.5', '3.4', '0.7', '34', '0.5']
# the studios are small rooms: from 10.5 m the camera stood outside the wall
CAMERA_FOR = {'studio': ['5.2', '2.4', '0.8', '52', '0.5'], 'studio2': ['5.2', '2.4', '0.8', '52', '0.5']}
# set pieces in front of the stage from the audience's spot (Tushino's parked helicopter)
HIDE_TEX = {}
HIDE_PARTS = {'tushino': ','.join(str(k) for k in list(range(225, 237)) + list(range(240, 260)))}

TITLES = {"ballpark": "AT&T Park", "bayou": "Swamp Shack", "castle": "Will Heilm's Keep", "cathedral": "Bone Church",
          "fairgrounds": "Strutter's Farm", "frathouse": "Phi Psi Kappa", "goth": "Wilted Orchid", "harbor": "Pang Tang Bay",
          "hotel": "Ted's Tiki Hut", "military": "Rock Brigade", "newyork": "Times Square", "recordstore": "Amoeba Records",
          "hob": "House of Blues", "carhenge": "Carhenge", "donnington": "Donington", "forum": "The Forum",
          "lhc": "Large Hadron Collider", "s_stage": "S Stage", "studio2": "Studio 2", "icecave": "Ice Cave",
          "neworleans": "New Orleans"}


def title(zone):
    return TITLES.get(zone, zone.replace('_', ' ').title())


def vignette(size, strength):
    w, h = size
    small = Image.new('L', (96, 54))
    px = small.load()
    for y in range(54):
        for x in range(96):
            dx, dy = (x - 47.5) / 48, (y - 26.5) / 27
            d = min(1.0, (dx * dx + dy * dy) ** 0.5 / 1.25)
            px[x, y] = int(255 * (1 - strength * d * d))
    return small.resize(size, Image.BICUBIC)


def grade(im):
    # VenueLook: contrast +18, saturation +12, colour filter (1, 0.97, 0.92), vignette 0.32
    im = ImageEnhance.Contrast(im).enhance(1.18)
    im = ImageEnhance.Color(im).enhance(1.12)
    r, g, b = im.split()
    g = g.point(lambda v: int(v * 0.97))
    b = b.point(lambda v: int(v * 0.92))
    im = Image.merge('RGB', (r, g, b))
    v = vignette(im.size, 0.32)
    return ImageChops.multiply(im, Image.merge('RGB', (v, v, v)))


def bloom(glow):
    # the lights' own glare (VenueLook's bloom): blurred copies added back
    wide = glow.filter(ImageFilter.GaussianBlur(14))
    near = glow.filter(ImageFilter.GaussianBlur(4))
    out = ImageChops.add(glow, ImageEnhance.Brightness(near).enhance(0.5))
    return ImageChops.add(out, ImageEnhance.Brightness(wide).enhance(0.5))


def main():
    tex_dir, work = sys.argv[1], sys.argv[2]
    zones = sys.argv[3:] or sorted(f[len('venue_ghwt_'):-5] for f in os.listdir(tex_dir)
                                   if f.startswith('venue_ghwt_') and f.endswith('.wtch'))
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(work, exist_ok=True)
    lines = []
    for zone in zones:
        prefix = os.path.join(work, zone)
        env = dict(os.environ)
        if zone in HIDE_TEX:
            env['VR_HIDE_TEX'] = HIDE_TEX[zone]
        if zone in HIDE_PARTS:
            env['VR_HIDE'] = HIDE_PARTS[zone]
        r = subprocess.run([RENDER, tex_dir, zone, prefix] + CAMERA_FOR.get(zone, CAMERA), capture_output=True, text=True,
                           env=env)
        if r.returncode != 0:
            print(zone, 'FAILED', r.stderr.strip())
            continue
        print(r.stdout.strip())
        base = Image.open(prefix + '_base.png').convert('RGB').resize((960, 544), Image.LANCZOS)
        glow = Image.open(prefix + '_glow.png').convert('RGB').resize((960, 544), Image.LANCZOS)
        grade(base).save(os.path.join(OUT, zone + '.jpg'), quality=86, optimize=True)
        # soft enough for half size: a quarter of the memory and of the decoding at song start
        bloom(glow).resize((480, 272), Image.LANCZOS).save(os.path.join(OUT, zone + '_glow.jpg'), quality=82, optimize=True)
        marks = {}
        for l in open(prefix + '_marks.txt'):
            p = l.split()
            marks[p[0]] = (float(p[1]) / 2, float(p[2]) / 2, float(p[3]) / 2)
        m = ' '.join('%s %.0f %.0f %.1f' % ((k,) + marks[k]) for k in ('drummer', 'vocalist', 'guitarist', 'bassist') if k in marks)
        lines.append('%s|%s|%s' % (zone, title(zone), m))
    if len(sys.argv) <= 3:
        with open(os.path.join(OUT, 'venues.txt'), 'w') as f:
            f.write('\n'.join(lines) + '\n')


main()
