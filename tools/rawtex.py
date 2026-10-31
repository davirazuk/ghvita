#!/usr/bin/env python3
# The game's start-up pictures as plain pixels in assets/raw/: 8 bytes of width and height, then RGBA. A PNG this size
# takes the Vita seconds to decode and these go straight in; gfx.c falls back on the PNG or JPG if one's missing
import os, struct, sys
from PIL import Image

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'assets')
os.makedirs(os.path.join(root, 'raw'), exist_ok=True)
for name in ('bg_title.jpg', 'bg_menu.jpg', 'bg_results.jpg', 'bg_stage.jpg', 'font_title.png', 'font_ui.png',
             'game.png', 'hud.png', 'stage.png', 'logo.png', 'vignette.png'):
    im = Image.open(os.path.join(root, name)).convert('RGBA')
    with open(os.path.join(root, 'raw', os.path.splitext(name)[0] + '.rgba'), 'wb') as f:
        f.write(struct.pack('<II', *im.size))
        f.write(im.tobytes())
print(sum(os.path.getsize(os.path.join(root, 'raw', n)) for n in os.listdir(os.path.join(root, 'raw'))) // 1024 ** 2, 'MB')
