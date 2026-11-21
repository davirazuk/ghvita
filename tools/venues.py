#!/usr/bin/env python3
# Renders every decoded GHWT venue (YargLook's textures folder) with tools/venue_render.c and makes GH Vita's
# assets/venues/<zone>.jpg (the stage, graded like YargLook's VenueLook: contrast, saturation, warm filter, vignette)
# and <zone>_glow.jpg (its lights on black with a soft bloom, 480x272), 960x544 (half the memory of a 1024x1024 texture),
# plus venues.txt: "<zone>|<title>|<role x y pixels-per-metre> ..." for the game.
# usage: tools/venues.py <textures dir> <render work dir> [zone ...]
#        tools/venues.py --shots <textures dir> <render work dir> [zone ...]   (the camera's other shots only)   (needs a GL display: DISPLAY=:77)
import os, subprocess, sys
from PIL import Image, ImageChops, ImageEnhance, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.environ.get('VENUES_OUT') or os.path.join(HERE, '..', 'assets', 'venues')     # (tests: somewhere else)
RENDER = os.path.join(HERE, '..', 'build-tools', 'venue_render')
# back, up, look height, fov, lens shift up (half screens): the shift puts the band above and beside the highway
CAMERA = ['10.5', '3.4', '0.7', '34', '0.5']
# the studios are small rooms: from 10.5 m the camera stood outside the wall
CAMERA_FOR = {'studio': ['5.2', '2.4', '0.8', '52', '0.5'], 'studio2': ['5.2', '2.4', '0.8', '52', '0.5']}
# set pieces in front of the stage from the audience's spot (Tushino's parked helicopter), and what the export can't show
# as GHWT does: effect cards frozen on one frame (the Keep's flaming arrows stood round the stage as orange shards)
HIDE_TEX = {'castle': '305e261e 64a8637b'}
HIDE_PARTS = {'tushino': ','.join(str(k) for k in list(range(225, 237)) + list(range(240, 260))),
              # (with its flaming arrows and their fire, by texture) the stakes along the stage's front edge (dark red
              # teeth across the bottom of the picture), the two black boxes over the riser's steps (a pit in the stage)
              'castle': '169,174,176',
              # a black card (GHWT draws it as a light: black adds nothing) stood up in front of the riser as a black horn
              'icecave': '16',
              # plain white cards GHWT lights in the show's colours, a ring of white teeth round the riser here
              'lhc': '20',
              # holograms of a guitarist either side and two untextured panels round them: see-through sheets over the
              # band
              'carhenge': '84,85,207,208',
              # an untextured lightning bolt on the floor where the singer stands (it glows in GHWT): a black shape
              'scifi': '131'}
# parts the export hides that the stage needs: the Keep's moat floor (left out, the stage had a black hole in it)
SHOW_PARTS = {'castle': '122'}
# parts GHWT tints with a material colour the export hasn't: the Keep's floor came out a white tiled bathroom
TINT_PARTS = {'castle': '170:0.36,0.34,0.31',
              'paris': ' '.join('%d:0.74,0.72,0.78' % k for k in (96, 98, 99))}
# parts drawn solid: Paris's riser is white cloth at 80%; drawn as haze it was a ghost
OPAQUE_PARTS = {'paris': '96,98,99'}


def zone_fixes(zone):
    """The same fixes as numbers, for tools/venue3d.py: parts hidden, shown, drawn solid, texture words hidden, and
    each tinted part's brightness (its tint's mean)"""
    nums = lambda t: {int(x) for x in t.replace(',', ' ').split()} if t else set()
    tint = {}
    for item in TINT_PARTS.get(zone, '').split():
        k, rgb = item.split(':')
        tint[int(k)] = sum(float(c) for c in rgb.split(',')) / 3
    return (nums(HIDE_PARTS.get(zone)), nums(SHOW_PARTS.get(zone)), nums(OPAQUE_PARTS.get(zone)),
            HIDE_TEX.get(zone, '').split(), tint)


def zone_env(zone):
    env = dict(os.environ)
    for var, table in (('VR_HIDE_TEX', HIDE_TEX), ('VR_HIDE', HIDE_PARTS), ('VR_SHOW', SHOW_PARTS), ('VR_TINT', TINT_PARTS),
                       ('VR_OPAQUE', OPAQUE_PARTS)):
        if zone in table:
            env[var] = table[zone]
    return env

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


# the camera's other shots, cut to in the song (game.c cam_shot): name, back, up, look, fov, yaw degrees. Each one's lens
# shift is found by rendering it once: the drummer lands where they stand in the wide shot, at the far end of the highway
SHOTS = [('s1', 7.0, 2.2, 1.0, 40, 32), ('s2', 7.0, 2.2, 1.0, 40, -32), ('s3', 6.0, 0.9, 1.4, 46, 0)]
SHOTS_STUDIO = [('s1', 4.2, 1.8, 1.0, 56, 30), ('s2', 4.2, 1.8, 1.0, 56, -30), ('s3', 3.6, 0.8, 1.3, 60, 0)]


def read_marks(prefix):
    marks = {}
    for l in open(prefix + '_marks.txt'):
        p = l.split()
        marks[p[0]] = (float(p[1]) / 2, float(p[2]) / 2, float(p[3]) / 2)
    return marks


def marks_text(marks):
    return ' '.join('%s %.0f %.0f %.1f' % ((k,) + marks[k]) for k in ('drummer', 'vocalist', 'guitarist', 'bassist') if k in marks)


def render_shots(tex_dir, work, zones):
    # venues.txt keeps its lines; each gets "|<shot marks>" per shot (older games read only the first part)
    path = os.path.join(OUT, 'venues.txt')
    lines = [l.rstrip('\n').split('|') for l in open(path) if '|' in l]
    by_zone = {l[0]: l for l in lines}
    for zone in zones:
        env = zone_env(zone)
        want = by_zone[zone][2].split() if zone in by_zone else []
        want_y = float(want[want.index('drummer') + 2]) if 'drummer' in want else 191
        extra = []
        for name, back, up, look, fov, yaw in (SHOTS_STUDIO if zone.startswith('studio') else SHOTS):
            prefix = os.path.join(work, zone + '_' + name)
            shift = 0.4
            for _ in range(3):
                args = [RENDER, tex_dir, zone, prefix, str(back), str(up), str(look), str(fov), '%.3f' % shift, str(yaw), '0']
                r = subprocess.run(args, capture_output=True, text=True, env=env)
                if r.returncode != 0:
                    break
                m = read_marks(prefix)
                if 'drummer' not in m:
                    break
                # half a screen of shift moves the picture 136 px (of 544): the drummer onto the wide shot's height
                d = (m['drummer'][1] - want_y) / 272
                if abs(d) < 0.01:
                    break
                shift += d
            if r.returncode != 0 or 'drummer' not in m:
                print(zone, name, 'FAILED', r.stderr.strip())
                extra.append('')
                continue
            base = Image.open(prefix + '_base.png').convert('RGB').resize((960, 544), Image.LANCZOS)
            glow = Image.open(prefix + '_glow.png').convert('RGB').resize((960, 544), Image.LANCZOS)
            grade(base).save(os.path.join(OUT, '%s_%s.jpg' % (zone, name)), quality=84, optimize=True)
            bloom(glow).resize((480, 272), Image.LANCZOS).save(os.path.join(OUT, '%s_%s_glow.jpg' % (zone, name)), quality=80,
                                                                optimize=True)
            extra.append(marks_text(m))
            print(zone, name, 'shift %.2f' % shift, marks_text(m))
        if zone in by_zone:
            by_zone[zone][:] = by_zone[zone][:3] + extra
    with open(path, 'w') as f:
        f.write('\n'.join('|'.join(l) for l in lines) + '\n')


def main():
    if sys.argv[1] == '--shots':
        tex_dir, work = sys.argv[2], sys.argv[3]
        os.makedirs(work, exist_ok=True)
        zones = sys.argv[4:] or [l.split('|')[0] for l in open(os.path.join(OUT, 'venues.txt')) if '|' in l]
        render_shots(tex_dir, work, zones)
        return
    tex_dir, work = sys.argv[1], sys.argv[2]
    zones = sys.argv[3:] or sorted(f[len('venue_ghwt_'):-5] for f in os.listdir(tex_dir)
                                   if f.startswith('venue_ghwt_') and f.endswith('.wtch'))
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(work, exist_ok=True)
    lines = []
    for zone in zones:
        prefix = os.path.join(work, zone)
        env = zone_env(zone)
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


if __name__ == '__main__':
    main()
