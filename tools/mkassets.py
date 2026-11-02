#!/usr/bin/env python3
"""Build GH Vita's assets/ from the GHWT art extracted for YargLook and YARG's sound effects.

usage: mkassets.py [OUT]   (default: assets/ next to tools/)
  game.png/.txt  the highway's sprites: GHWT gems composited per lane colour (normal, hammer-on, tap, star frames,
                 open bars), now-bar cups and caps, hit flames, bars, sustain, lightning, multipliers, LCD digits
  hud.png/.txt   the rock meter, Star Power tubes, score amp, nixie multipliers, lamps, menu panels and stars
  *.jpg          backgrounds; hw_*.png highways; font_*.png/.fnt fonts; sfx/*.ogg sounds
An atlas .txt has one line per sprite: name x y w h (pixels).
"""
import os, struct, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HOME = os.path.expanduser('~')
T = HOME + '/mods/yarg-look/YargLook/textures'
G = T + '/ghwt_gems'
YSFX = HOME + '/mods/refs/yarg/Assets/StreamingAssets/sfx'
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'assets')

# lane colours (GHWT): green, red, yellow, blue, orange, then Star Power cyan and the open notes' purple
LANES = [(0.10, 0.85, 0.12), (0.95, 0.10, 0.08), (1.00, 0.88, 0.05), (0.10, 0.38, 1.00), (1.00, 0.50, 0.04)]
SP = (0.45, 0.88, 1.00)
OPEN = (0.72, 0.25, 1.00)


def load(path):
    return np.asarray(Image.open(path).convert('RGBA'), dtype=np.float32) / 255.0


def to_img(a):
    return Image.fromarray((np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8), 'RGBA')


def gem(kind, c, star=False):
    """GHWT's gem: the dome (collar art) carries the colour (x1.75), the base is the silver rim with the white cap,
    the highlight is added over both, hammer-ons add the halo; taps use the see-through collar"""
    silver = star and tuple(c) != tuple(SP)
    if kind == 'open':
        b, co, hi, ha = (load(G + '/Button_Drum_Kick_Base01.png'), load(G + '/Button_Drum_Kick_Collar01.png'),
                         load(G + '/Button_Drum_Kick_highlight01.png'), None)
    else:
        pre = 'star_grey_' if star else 'button_grey_'
        b = load(G + ('/star_grey_base_01.png' if star else '/button_grey_Base01.png'))
        coll = {'normal': 'star_grey_collar_01' if star else 'button_grey_Collar01',
                'hopo': 'star_grey_collar_01' if star else 'button_grey_Collar01',
                'tap': 'star_grey_collar_Cthru01' if star else 'button_grey_Collar_Ctrhu01'}[kind]
        co = load(G + '/' + coll + '.png')
        hi = load(G + ('/Star_Grey_Highlight_01.png' if star else '/Button_Grey_Highlight_01.png'))
        ha = load(G + ('/star_grey_hammer_halo_01.png' if star else '/button_grey_Hammer_Halo01.png')) \
            if kind == 'hopo' else None
    c = np.array(c, dtype=np.float32)
    o = b.copy()
    if kind == 'open':
        o[..., :3] *= c * 1.5
    if co is not None and co.shape == b.shape:
        ca = co[..., 3:4] * (0.6 if kind == 'tap' else 1.0)
        cc = co[..., :3] if kind == 'open' else np.clip(co[..., :3] * c * 1.75, 0, 1)
        o[..., :3] = cc * ca + o[..., :3] * (1 - ca)
        o[..., 3:4] = np.maximum(o[..., 3:4], ca)
    if hi is not None and hi.shape == b.shape:
        o[..., :3] += hi[..., :3] * hi[..., 3:4] * 0.85
    if ha is not None and ha.shape == b.shape:
        o[..., :3] += ha[..., :3] * ha[..., 3:4]
        o[..., 3:4] = np.maximum(o[..., 3:4], ha[..., 3:4] * 0.85)
    out = to_img(o)
    if silver:
        # the star's base has a cyan edge that reads as a layer slipped off the gem: silver, as on the round ones
        a = np.array(out.convert('RGBA'))
        r, g, b = a[..., 0].astype(int), a[..., 1].astype(int), a[..., 2].astype(int)
        m = (b > r + 40) & (g > r + 30) & (b > 110)
        v = ((g + b) // 2).clip(0, 255)
        a[..., 0][m], a[..., 1][m], a[..., 2][m] = (v[m] * 0.86).astype(np.uint8), (v[m] * 0.86).astype(np.uint8), (v[m] * 0.9).astype(np.uint8)
        out = Image.fromarray(a, 'RGBA')
    return out


def scaled_on(canvas, img, scale, tint=None):
    """img scaled about the canvas centre, alpha-composited onto it"""
    w, h = canvas.size
    im = img.resize((max(1, int(w * scale)), max(1, int(h * scale))), Image.LANCZOS)
    if tint is not None:
        a = np.asarray(im, dtype=np.float32) / 255
        a[..., :3] = np.clip(a[..., :3] * np.array(tint, dtype=np.float32), 0, 1)
        im = to_img(a)
    layer = Image.new('RGBA', (w, h))
    layer.paste(im, ((w - im.size[0]) // 2, (h - im.size[1]) // 2))
    return Image.alpha_composite(canvas, layer)


class Atlas:
    def __init__(self, size):
        self.size = size
        self.items = []

    def add(self, name, img):
        self.items.append((name, img.convert('RGBA')))

    def save(self, png, txt, pad=2):
        S = self.size
        sheet = Image.new('RGBA', (S, S))
        lines = []
        x = y = row = 0
        for name, img in sorted(self.items, key=lambda it: -it[1].size[1]):
            w, h = img.size
            if x + w + pad * 2 > S:
                x, y, row = 0, y + row, 0
            if y + h + pad * 2 > S:
                raise SystemExit('atlas %s is full at %s' % (png, name))
            # extrude the edges into the padding so filtering doesn't pull in neighbours
            ext = img.resize((w + pad * 2, h + pad * 2), Image.NEAREST)
            ext.paste(img, (pad, pad))
            sheet.paste(ext, (x, y))
            lines.append('%s %d %d %d %d' % (name, x + pad, y + pad, w, h))
            x += w + pad * 2
            row = max(row, h + pad * 2)
        sheet.save(png, optimize=True)
        with open(txt, 'w') as f:
            f.write('\n'.join(lines) + '\n')
        print('%s: %d sprites, %d%% used' % (os.path.basename(png), len(lines),
                                            100 * sum(i[1].size[0] * i[1].size[1] for i in self.items) // (S * S)))


def frames(img, cols, rows):
    w, h = img.size[0] // cols, img.size[1] // rows
    return [img.crop(((f % cols) * w, (f // cols) * h, (f % cols + 1) * w, (f // cols + 1) * h))
            for f in range(cols * rows)]


def lamp(state, hue):
    """one rope lamp, crisp at any size: a glass dome, the round end left, the foot on the right under the rope.
    0 dark glass, 1 half lit, 2 lit with a white-hot middle and the bloom outside it"""
    S = 4
    n = 64 * S
    yy, xx = np.mgrid[0:n, 0:n] / S
    cx, cy, r = 28.0, 32.0, 25.0
    # a dome: round at the end, running straight on under the rope so it grows out of it
    d = np.where(xx >= cx, np.abs(yy - cy), np.hypot(xx - cx, yy - cy))
    body = d < r
    ndist = np.clip(d / r, 0, 1)
    hue = np.array(hue, float)
    if state == 2:
        rgb = hue[None, None] * (0.75 + 0.25 * (1 - ndist[..., None])) + (255 - hue[None, None]) * (1 - ndist[..., None] ** 2.2)[..., :] * 0.95
    elif state == 1:
        rgb = hue[None, None] * (0.32 + 0.5 * (1 - ndist[..., None])) + 25
    else:
        rgb = hue[None, None] * (0.13 + 0.16 * ndist[..., None] ** 2) + 12
    # the glass: a bright edge, and a window of light up on the shoulder
    edge = np.clip((ndist - 0.84) / 0.16, 0, 1)[..., None]
    k = 0.55 if state < 2 else 0.22
    rgb = rgb * (1 - edge * k) + hue[None, None] * edge * k + (40 if state < 2 else 0) * edge
    spot = np.exp(-(((xx - cx + 9) / 7.5) ** 2 + ((yy - cy + 10) / 4.5) ** 2))[..., None]
    rgb = rgb + (255 - rgb) * spot * (0.55 if state < 2 else 0.8)
    alpha = np.where(body, 1.0 if state else 0.9, 0.0)
    if state == 2:
        # lit, the light eats the glass edge: no outline, a white middle going soft into the bloom
        alpha = np.clip(1 - (d - r * 0.72) / (r * 0.46), 0, 1) ** 1.3
        white = np.clip(1 - d / (r * 0.95), 0, 1)[..., None] ** 0.55
        rgb = hue[None, None] * 0.55 + 115 + (140 - hue[None, None] * 0.55) * white
    a = np.dstack([np.clip(rgb, 0, 255), alpha * 255]).astype(np.uint8)
    return Image.fromarray(a, 'RGBA').resize((64, 64), Image.LANCZOS)


def mult_extra(m, G):
    """GHWT's multiplier plates stop at x8: its bass never went past x4. Ours goes to x6 and doubles under Star
    Power, so x10 and x12 are composed here: the real cross from the x8 plate, the real 2 from the x2 plate, and a
    1 and a 0 drawn to match - white with the plate's dark outline"""
    plate = Image.open(G + '/mult_x8.png').convert('RGBA')
    ImageDraw.Draw(plate).rectangle((56, 0, 192, 96), fill=(0, 0, 0, 0))
    two = Image.open(G + '/mult_x2.png').convert('RGBA').crop((55, 10, 124, 86))
    pad = 10
    def digit(kind):
        msk = Image.new('L', (58 + 2 * pad, 66 + 2 * pad), 0)
        d = ImageDraw.Draw(msk)
        if kind == '1':
            d.polygon([(pad + 30, pad + 16), (pad + 44, pad), (pad + 56, pad), (pad + 56, pad + 66),
                       (pad + 30, pad + 66)], fill=255)
            d.polygon([(pad + 4, pad + 28), (pad + 30, pad + 2), (pad + 30, pad + 24), (pad + 16, pad + 36)], fill=255)
            left = 4
        else:
            d.ellipse((pad + 2, pad, pad + 56, pad + 66), fill=255)
            d.ellipse((pad + 21, pad + 19, pad + 37, pad + 47), fill=0)
            left = 2
        out = Image.new('RGBA', msk.size, (0, 0, 0, 0))
        out.paste(Image.new('RGBA', msk.size, (10, 10, 14, 255)), (0, 0), msk.filter(ImageFilter.MaxFilter(11)))
        out.paste(Image.new('RGBA', msk.size, (255, 255, 255, 255)), (0, 0), msk)
        return out, left
    for k, cell in (('1', 58), ('2' if m == 12 else '0', 118)):
        if k == '2':
            plate.paste(two, (cell - 5, 5))
            continue
        g, left = digit(k)
        plate.paste(g, (cell - left - pad, 15 - pad))
    return plate


def game_atlas():
    A = Atlas(2048)
    colours = LANES + [SP]
    for i, c in enumerate(colours):
        for kind in ('normal', 'hopo', 'tap'):
            A.add('gem_%s_%d' % (kind, i), gem(kind, c))
            # the star gems turn: 8 of the sheet's 16 frames
            for f, im in enumerate(frames(gem(kind, c, star=True), 4, 4)[::2]):
                A.add('star_%s_%d_%d' % (kind, i, f), im)
    for kind in ('normal', 'hopo', 'tap'):
        for name, c in (('open', OPEN), ('opensp', SP)):
            img = gem('open', c)
            if kind == 'hopo':
                a = np.asarray(img, dtype=np.float32) / 255
                a[..., :3] = np.clip(a[..., :3] * 1.35 + 0.12, 0, 1)
                img = to_img(a)
            elif kind == 'tap':
                a = np.asarray(img, dtype=np.float32) / 255
                a[..., 3] *= 0.7
                img = to_img(a)
            A.add('%s_%s' % (name, kind), img.resize((384, 48), Image.LANCZOS))
    # now-bar cups: dark well, thick lane-coloured band, silver rim; the cap rises out of the well while held
    base = Image.open(G + '/NB_Cup1_Base_02.png').convert('RGBA')
    hollow = Image.open(G + '/NB_Cup1_Base_01.png').convert('RGBA')
    hook = Image.open(G + '/nb_Cup3_Hook_01.png').convert('RGBA')
    ring = Image.open(G + '/NB_Cup1_Ring_01.png').convert('RGBA')
    cap = Image.open(G + '/nb_cap1_base_01.png').convert('RGBA')
    cap_collar = Image.open(G + '/nb_cap1_collar_01.png').convert('RGBA')
    for i, c in enumerate(LANES + [SP, OPEN]):
        cc = tuple(min(1.0, v * 1.3) for v in c)
        cup = Image.new('RGBA', base.size)
        cup = scaled_on(cup, hook, 1.0)
        band = tuple(v * 1.7 for v in cc)    # the ring art is mid grey: the footage's band is full colour
        for k in (1.0, 0.975, 0.95, 0.925):
            cup = scaled_on(cup, ring, k, band)
        cup = scaled_on(cup, ring, 0.88, (1.25, 1.25, 1.3))
        cup = scaled_on(cup, base, 0.82)
        A.add('cup_%d' % i, cup)
        deep = scaled_on(Image.new('RGBA', hollow.size), hollow, 0.9)
        deep = scaled_on(deep, ring, 1.0, cc)
        deep = scaled_on(deep, ring, 0.955, cc)
        deep = scaled_on(deep, ring, 0.88, (0.9, 0.9, 0.93))
        A.add('cupdeep_%d' % i, deep)
        cp = Image.new('RGBA', cap.size)
        cp = scaled_on(cp, cap, 1.0, tuple(min(1.0, v * 1.15) for v in c))
        cp = scaled_on(cp, cap_collar, 1.0)
        A.add('cap_%d' % i, cp)
    A.add('cup_glow', Image.open(G + '/NB_Cup1_Base_GLOW_02.png'))
    A.add('flame', Image.open(G + '/Note_Hit01.png'))
    A.add('bar_large', Image.open(G + '/FretBar_large01_DNC.png'))
    A.add('bar_med', Image.open(G + '/FretBar_Med01_DNC.png'))
    A.add('bar_small', Image.open(G + '/FretBar_Small01_DNC.png'))
    A.add('rail', Image.open(G + '/sidebar01.png'))
    A.add('now_neck', Image.open(G + '/NowBar_Neck.png'))
    A.add('string', Image.open(G + '/string.png'))
    A.add('lightning', Image.open(G + '/JOW_StarPower_TeslaLightning01.png'))
    A.add('bolt', Image.open(G + '/VH_Lightning_fx.png').convert('RGB').crop((128, 0, 256, 32)))
    for hname, hc in (('', (255, 196, 70)), ('_green', (90, 245, 110)), ('_purple', (255, 140, 225)), ('_blue', (95, 225, 255))):
        for st in range(3):
            A.add('lamp_%d%s' % (st, hname), lamp(st, hc))
    A.add('sp_side', Image.open(G + '/starpower_sidegrade.png'))
    A.add('flare', Image.open(G + '/JOW_ButtonFlare01.png'))
    A.add('spark', Image.open(G + '/JOW_ButtonStar02.png'))
    A.add('smoke', Image.open(G + '/PH_Smoke_SinglePuff_Blend_02.png').resize((128, 128), Image.LANCZOS))
    A.add('whammy_cap', Image.open(G + '/WhammyBAR_Grey.png'))
    for m in (2, 3, 4, 5, 6, 8, 10, 12):
        p = G + '/mult_x%d.png' % m
        A.add('mult_x%d' % m, Image.open(p) if os.path.exists(p) else mult_extra(m, G))
    A.add('lcd_green', Image.open(G + '/lcd_green.png'))
    A.add('lcd_orange', Image.open(G + '/lcd_orange.png'))
    # the sustain beam: a bright core with soft edges (white, tinted when drawn)
    x = np.linspace(-1, 1, 32)
    prof = np.clip(1.15 - np.abs(x) ** 2.2, 0, 1)
    core = np.exp(-(x / 0.35) ** 2)
    a = np.zeros((8, 32, 4), np.float32)
    a[..., 0] = a[..., 1] = a[..., 2] = 0.55 + 0.45 * core
    a[..., 3] = prof
    A.add('sustain', to_img(a))
    # soft round glow and a soft line
    yy, xx = np.mgrid[-1:1:64j, -1:1:64j]
    g = np.clip(1 - np.sqrt(xx ** 2 + yy ** 2), 0, 1) ** 2
    a = np.ones((64, 64, 4), np.float32)
    a[..., 3] = g
    A.add('glow', to_img(a))
    a = np.ones((4, 32, 4), np.float32)
    a[..., 3] = np.clip(1 - np.abs(np.linspace(-1, 1, 32)), 0, 1)[None, :]
    A.add('line', to_img(a))
    A.save(os.path.join(OUT, 'game.png'), os.path.join(OUT, 'game.txt'))


def hud_atlas():
    A = Atlas(2048)
    for n in ('HUD_rock_body', 'HUD_rock_BG_green', 'HUD_rock_BG_yellow', 'HUD_rock_BG_red', 'HUD_rock_lights_all',
              'HUD_rock_lights_green', 'HUD_rock_lights_yellow', 'HUD_rock_lights_red', 'HUD_rock_needle',
              'HUD_rock_tube', 'hud_rock_tube_glow_full', 'hud_rock_tube_glow_full_b', 'HUD_score_body',
              'HUD_score_flash', 'hud_star_power_ready_glow', 'hud_seat', 'star_bulb_0',
              'hud_score_light_0', 'hud_score_light_1', 'hud_score_light_2',
              'hud_score_light_0_blue', 'hud_score_light_1_blue', 'hud_score_light_2_blue',
              'hud_score_light_0_green', 'hud_score_light_1_green', 'hud_score_light_2_green',
              'hud_score_light_0_purple', 'hud_score_light_1_purple', 'hud_score_light_2_purple',
              'HUD_score_nixie_1a', 'HUD_score_nixie_2a', 'HUD_score_nixie_2b', 'HUD_score_nixie_3a',
              'HUD_score_nixie_4a', 'HUD_score_nixie_4b', 'HUD_score_nixie_6b', 'HUD_score_nixie_8b',
              'list_bg', 'list_highlight', 'generic_title_bg', 'dialog_bg', 'helper_background',
              'menu_pause_bg', 'menu_pause_frame_banner', 'message_container', 'setlist_B_head',
              'setlist_B_list_hilite_R', 'setlist_griffin', 'skull_title_top', 'logo_GH4_LoR_256',
              'song_summary_detail_bg'):
        A.add(n.lower(), Image.open(T + '/ghwt_' + n + '.png'))
    # GHWT's solo HUD: the score counter and streak plate under the dome, the lamp seat on the rail
    for n in ('HUD_counter_body', 'HUD_RM_solo_bottom', 'multi_light_seat', 'star_light_seat_1g',
              'HUD_rock_tube_glow_fill', 'HUD_rock_tube_glow_fill_b'):
        A.add(n.lower(), Image.open(T + '/ghwt_hud/' + n + '.png'))
    # the rope comes slanted for the screen; the board's perspective does that here, so stand it up
    rope = np.asarray(Image.open(T + '/ghwt_hud_seat.png').convert('RGBA')).copy()
    out = np.zeros((rope.shape[0], 16, 4), dtype=np.uint8)
    for y in range(rope.shape[0]):
        xs = np.nonzero(rope[y, :, 3] > 40)[0]
        if len(xs):
            c = int(xs.mean())
            for x in range(16):
                sx = c - 8 + x
                if 0 <= sx < rope.shape[1]:
                    out[y, x] = rope[y, sx]
    A.add('hud_rope', Image.fromarray(out, 'RGBA'))
    for n in ('hud_star_on', 'hud_star_off', 'hud_star_gold'):
        A.add(n, Image.open(T + '/' + n + '.png'))
    A.add('wor_row', Image.open(T + '/wor_row.png').resize((768, 72), Image.LANCZOS))
    A.add('wor_select', Image.open(T + '/wor_select.png').resize((768, 72), Image.LANCZOS))
    # the buttons' glyphs for the help line: circles with the PlayStation shapes
    for name, col in (('cross', (120, 160, 255)), ('circle', (255, 90, 90)), ('square', (240, 120, 220)),
                      ('triangle', (60, 220, 160))):
        im = Image.new('RGBA', (64, 64))
        d = ImageDraw.Draw(im)
        d.ellipse((2, 2, 61, 61), fill=(30, 30, 34, 255), outline=(200, 200, 200, 255), width=3)
        if name == 'cross':
            d.line((20, 20, 44, 44), fill=col, width=6)
            d.line((44, 20, 20, 44), fill=col, width=6)
        elif name == 'circle':
            d.ellipse((18, 18, 46, 46), outline=col, width=6)
        elif name == 'square':
            d.rectangle((20, 20, 44, 44), outline=col, width=6)
        else:
            d.polygon((32, 16, 48, 44, 16, 44), outline=col, width=5)
        A.add('btn_' + name, im)
    for name, text in (('l', 'L'), ('r', 'R'), ('start', 'START'), ('select', 'SELECT')):
        f = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf', 26)
        w = int(f.getlength(text)) + 28
        im = Image.new('RGBA', (w, 44))
        d = ImageDraw.Draw(im)
        d.rounded_rectangle((1, 1, w - 2, 42), radius=12, fill=(30, 30, 34, 255), outline=(200, 200, 200, 255),
                            width=3)
        d.text((w // 2, 22), text, font=f, fill=(235, 235, 235, 255), anchor='mm')
        A.add('btn_' + name, im)
    for name, pts in (('up', (32, 12, 52, 40, 12, 40)), ('down', (32, 52, 52, 24, 12, 24)),
                      ('left', (12, 32, 40, 12, 40, 52)), ('right', (52, 32, 24, 12, 24, 52))):
        im = Image.new('RGBA', (64, 64))
        d = ImageDraw.Draw(im)
        d.rounded_rectangle((4, 4, 59, 59), radius=10, fill=(30, 30, 34, 255), outline=(200, 200, 200, 255),
                            width=3)
        d.polygon(pts, fill=(235, 235, 235, 255))
        A.add('btn_' + name, im)
    A.save(os.path.join(OUT, 'hud.png'), os.path.join(OUT, 'hud.txt'))


def backgrounds():
    def fit(src, out, w=960, h=544, darken=1.0, blur=0):
        im = Image.open(src).convert('RGB')
        s = max(w / im.size[0], h / im.size[1])
        im = im.resize((int(im.size[0] * s + 0.5), int(im.size[1] * s + 0.5)), Image.LANCZOS)
        x, y = (im.size[0] - w) // 2, (im.size[1] - h) // 2
        im = im.crop((x, y, x + w, y + h))
        if blur:
            im = im.filter(ImageFilter.GaussianBlur(blur))
        if darken != 1.0:
            im = Image.eval(im, lambda v: int(v * darken))
        # 1024x1024 power-of-two canvas: the Vita's GPU likes those, the picture is drawn from its top-left part
        canvas = Image.new('RGB', (1024, 1024))
        canvas.paste(im, (0, 0))
        canvas.save(os.path.join(OUT, out), quality=90)
    fit(T + '/ghwt_title_bg.png', 'bg_title.jpg')
    fit(T + '/ghwt_menu_bg.png', 'bg_menu.jpg')
    fit(T + '/ghwt_song_summary_bg.png', 'bg_results.jpg')
    fit(T + '/ghwt_menu_bg.png', 'bg_stage.jpg', darken=0.55, blur=2)
    Image.open(T + '/vignette.png').convert('RGBA').resize((512, 256), Image.LANCZOS).save(
        os.path.join(OUT, 'vignette.png'))
    # World Tour's own logo, not gh_logo.png (a stand-in made for YargLook)
    logo = Image.open(T + '/ghwt_logo_GH4_LoR_256.png').convert('RGBA')
    logo = logo.crop(logo.getchannel('A').getbbox())
    k = min(512 / logo.width, 370 / logo.height)
    logo = logo.resize((int(logo.width * k), int(logo.height * k)), Image.LANCZOS)
    page = Image.new('RGBA', (512, 370), (0, 0, 0, 0))
    page.paste(logo, ((512 - logo.width) // 2, (370 - logo.height) // 2))
    page.save(os.path.join(OUT, 'logo.png'))


def highways():
    for n in ('classic', 'gh3_axel', 'gh3_judy', 'gh3_izzy', 'gh2_wood', 'gh2_rock', 'gh2_goth', 'ghm_james', 'axel',
              'judy', 'midori', 'johnny', 'casey', 'lars'):
        src = T + '/ghwt_hw/%s.png' % n
        if os.path.exists(src):
            Image.open(src).convert('RGBA').save(os.path.join(OUT, 'hw_%s.png' % n), optimize=True)


def font(ttf, size, out, chars, pad=2, sheet=1024):
    f = ImageFont.truetype(ttf, size)
    ascent, descent = f.getmetrics()
    glyphs = []
    for cp in chars:
        ch = chr(cp)
        try:
            if not f.getmask(ch).getbbox() and ch != ' ':
                continue
        except Exception:
            continue
        mask, (ox, oy) = f.getmask2(ch, mode='L')
        w, h = mask.size
        img = Image.frombytes('L', (w, h), bytes(mask)) if w and h else Image.new('L', (1, 1))
        adv = f.getlength(ch)
        glyphs.append((cp, img, ox, oy, adv))
    glyphs.sort(key=lambda g: -g[1].size[1])
    S = sheet
    atlas = Image.new('L', (S, S))
    x = y = row = 0
    entries = []
    for cp, img, ox, oy, adv in glyphs:
        w, h = img.size
        if x + w + pad > S:
            x, y, row = 0, y + row + pad, 0
        if y + h + pad > S:
            raise SystemExit('font %s: sheet full' % out)
        atlas.paste(img, (x, y))
        entries.append((cp, x, y, w, h, ox, oy, int(round(adv * 4))))
        x += w + pad
        row = max(row, h)
    entries.sort()
    rgba = Image.merge('RGBA', (Image.new('L', (S, S), 255),) * 3 + (atlas,))
    rgba.save(os.path.join(OUT, out + '.png'), optimize=True)
    with open(os.path.join(OUT, out + '.fnt'), 'wb') as o:
        o.write(b'VFN1' + struct.pack('<HHHHI', size, ascent + descent, ascent, 0, len(entries)))
        for e in entries:
            o.write(struct.pack('<IHHHHhhHH', *e, 0))
    print('%s: %d glyphs' % (out, len(entries)))


def fonts():
    latin = list(range(32, 127)) + list(range(160, 384)) + [0x2018, 0x2019, 0x201C, 0x201D, 0x2013, 0x2014,
                                                            0x2026, 0x2022, 0x2122, 0x00D7, 0x266A, 0x2605, 0x2606]
    font('/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf', 30, 'font_ui',
         latin + list(range(0x370, 0x3D0)) + list(range(0x400, 0x460)))
    font(T + '/MetalMania.ttf', 56, 'font_title', list(range(32, 127)) + list(range(160, 256)))


def sfx():
    d = os.path.join(OUT, 'sfx')
    os.makedirs(d, exist_ok=True)

    def conv(src, name, vol=1.0):
        # some GHWT banks are FSB4 containers: the MP3 data follows the 48-byte header and the sample headers
        with open(src, 'rb') as f:
            head = f.read(48)
            if head[:4] == b'FSB4':
                shdr = struct.unpack_from('<I', head, 8)[0]
                f.seek(48 + shdr)
                tmp = os.path.join(d, name + '.unwrapped.mp3')
                with open(tmp, 'wb') as o:
                    o.write(f.read())
                conv(tmp, name, vol)
                os.remove(tmp)
                return
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-nostdin', '-i', src, '-af', 'volume=%.2f' % vol, '-ar',
                        '48000', '-ac', '2', '-c:a', 'libvorbis', '-q:a', '3', os.path.join(d, name + '.ogg')],
                       check=True)
    conv(YSFX + '/note_miss.ogg', 'miss')
    for i in range(1, 5):
        conv(YSFX + '/overstrum_%d.wav' % i, 'overstrum_%d' % i)
    conv(YSFX + '/starpower_award.ogg', 'sp_award')
    conv(YSFX + '/starpower_deploy.ogg', 'sp_deploy')
    conv(YSFX + '/starpower_ready.opus', 'sp_ready')
    conv(YSFX + '/starpower_release.ogg', 'sp_release')
    conv(YSFX + '/star.wav', 'star')
    conv(YSFX + '/star_gold.wav', 'star_gold')
    conv(YSFX + '/overdrive_deploy_crowd.ogg', 'sp_crowd')
    C = T + '/sfx/medium_int'
    conv(C + '/INT_MD_Crowd_Good_Loop_FT.mp3', 'crowd_good')
    conv(C + '/INT_MD_Crowd_Neutral_Loop_FT.mp3', 'crowd_neutral')
    conv(C + '/INT_MD_Crowd_Bad_Loop_FT.mp3', 'crowd_bad')
    for i in (1, 2, 3):
        conv(C + '/Crowd_Cheer_MD_INT_SG_%02d.mp3' % i, 'cheer_%d' % i)
        conv(C + '/Crowd_Boo_GRP_Medium_INT_%02d.mp3' % i, 'boo_%d' % i)
    applause = [f for f in sorted(os.listdir(C)) if f.startswith('INT_MD_Crowd_Applause_L')]
    if applause:
        conv(os.path.join(C, applause[0]), 'applause')
    # menu ticks: short bright clicks
    rate = 48000

    def tone(name, parts):
        out = []
        for f0, dur, amp in parts:
            t = np.arange(int(rate * dur)) / rate
            env = np.exp(-t * 38)
            out.append(amp * env * (np.sin(2 * np.pi * f0 * t) + 0.35 * np.sin(2 * np.pi * f0 * 2.01 * t)))
        s = np.concatenate(out)
        pcm = (np.clip(s, -1, 1) * 32767 * 0.5).astype('<i2')
        raw = os.path.join(d, name + '.raw')
        np.stack([pcm, pcm], axis=1).tofile(raw)
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-nostdin', '-f', 's16le', '-ar', '48000', '-ac', '2', '-i',
                        raw, '-c:a', 'libvorbis', '-q:a', '3', os.path.join(d, name + '.ogg')], check=True)
        os.remove(raw)
    tone('ui_move', [(1800, 0.05, 0.5)])
    tone('ui_select', [(1200, 0.06, 0.6), (1800, 0.10, 0.6)])
    tone('ui_back', [(900, 0.06, 0.6), (600, 0.10, 0.6)])


def main():
    os.makedirs(OUT, exist_ok=True)
    game_atlas()
    hud_atlas()
    backgrounds()
    highways()
    fonts()
    sfx()


if __name__ == '__main__':
    main()
