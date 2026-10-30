#!/usr/bin/env python3
# GH Vita's system manual (sce_sys/manual/*.png, opened from the LiveArea): the buttons, and how to add songs. The
# button text follows lane_buttons() in src/songs.c and the menus in src/main.c: change them together.
# usage: manual.py <out dir>
import os
import sys
from PIL import Image, ImageDraw, ImageEnhance, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
T = os.path.expanduser('~/mods/yarg-look/YargLook/textures')
FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed.ttf'
BOLD = '/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf'
TITLE = T + '/MetalMania.ttf'

W, H = 960, 544
TEXT = (236, 232, 222)
DIM = (176, 170, 160)
HEAD = (255, 206, 110)
CHIP = (30, 28, 30)
SYMBOL = {'△': (64, 200, 160), '○': (240, 92, 108), '✕': (104, 150, 245), '□': (225, 118, 200)}
LANE = {'green': (60, 220, 70), 'red': (240, 60, 50), 'yellow': (250, 222, 40), 'blue': (60, 120, 255),
        'orange': (255, 150, 30), 'open': (190, 100, 255)}

PAGES = [
    ('How to play', [
        [('Playing (layout A)', [
            ('L', 'green'),
            ('◀', 'red'),
            ('▲ △', 'yellow'),
            ('○', 'blue'),
            ('R', 'orange'),
            ('✕ ▼', 'open notes (the purple bar)'),
            ('SELECT', 'Star Power, or tilt the Vita quickly'),
            ('R stick', 'whammy on Star Power sustains'),
            ('touch', 'tap the cups to play those lanes'),
            ('START', 'pause'),
        ])],
        [('Layout B (in Options)', [
            ('L ◀ ▲ △ R', 'green red yellow blue orange'),
            ('✕ ▼ ○', 'open notes'),
        ]),
         ('Menus', [
             ('▲ ▼', 'move'),
             ('✕', 'pick'),
             ('○', 'back'),
             ('L R', 'a page up or down the song list'),
             ('△', 'sort by title or artist'),
             ('□', 'a random song'),
             ('START', 'options, from the song list'),
         ])],
    ]),
    ('Adding songs', [
        [('From Clone Hero or YARG', [
            ('1', 'On a PC, convert the song folders or .sng files:'),
            ('', 'python3 tools/pack.py -o songs <folders>'),
            ('2', 'Copy the folders it makes to the memory card:'),
            ('', 'ux0:data/ghvita/songs/'),
            ('3', 'Start the game: the songs are in Quickplay.'),
        ]),
         ('Good to know', [
            ('', 'Guitar, bass, rhythm, co-op and keys charts play.'),
            ('', 'Drums and vocals charts are left out.'),
            ('', 'Missing a note mutes your part, as in the games.'),
            ('', 'Calibrate (main menu) matches notes to sound.'),
            ('', 'Scores: ux0:data/ghvita/scores.txt'),
        ])],
    ]),
]


def chips(d, x, y, keys, f):
    """the buttons as dark chips; returns the x after them"""
    for k in keys.split(' '):
        w = d.textlength(k, font=f) + 16
        d.rounded_rectangle((x, y, x + w, y + 28), radius=8, fill=CHIP, outline=(120, 110, 96), width=1)
        d.text((x + w / 2, y + 14), k, font=f, fill=SYMBOL.get(k, TEXT), anchor='mm')
        x += w + 5
    return x


def page(title, columns, out):
    bg = Image.open(os.path.join(ROOT, 'assets', 'bg_menu.jpg')).crop((0, 0, W, H)).convert('RGB')
    bg = ImageEnhance.Brightness(bg).enhance(0.35)
    d = ImageDraw.Draw(bg)
    d.text((W // 2, 44), title.upper(), font=ImageFont.truetype(TITLE, 54), fill=HEAD, anchor='mm',
           stroke_width=2, stroke_fill=(40, 20, 0))
    head = ImageFont.truetype(BOLD, 24)
    body = ImageFont.truetype(FONT, 21)
    chip = ImageFont.truetype(BOLD, 17)
    for c, sections in enumerate(columns):
        x0, y = 40 + c * 460, 92
        for heading, rows in sections:
            d.text((x0, y), heading, font=head, fill=HEAD)
            y += 36
            for keys, what in rows:
                x = chips(d, x0, y, keys, chip) + 8 if keys and keys not in '123' else x0
                if keys in ('1', '2', '3'):
                    d.ellipse((x0, y, x0 + 28, y + 28), fill=(120, 70, 20))
                    d.text((x0 + 14, y + 14), keys, font=chip, fill=TEXT, anchor='mm')
                    x = x0 + 38
                elif not keys:
                    x = x0 + 38 if title == 'Adding songs' and what[:1] in 'pu' else x0
                colour = LANE.get(what, TEXT)
                d.text((x, y + 3), what, font=body, fill=colour if what in LANE else (DIM if not keys else TEXT))
                y += 34
            y += 12
    bg.quantize(256, method=Image.Quantize.FASTOCTREE).save(out)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for i, (title, columns) in enumerate(PAGES):
        page(title, columns, os.path.join(out, '%03d.png' % (i + 1)))


if __name__ == '__main__':
    main()
