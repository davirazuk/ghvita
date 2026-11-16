#!/usr/bin/env python3
"""Each venue's picture out of focus (assets/venues/<zone>_blur.jpg, 240 x 136): what's behind the 3D band in a close shot.
venue_blur.py [assets/venues]"""
import glob, os, sys
from PIL import Image, ImageFilter
d = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'assets', 'venues')
n = 0
for p in sorted(glob.glob(os.path.join(d, '*.jpg'))):
    b = os.path.basename(p)[:-4]
    if '_' in b:                # the other shots, the glows, the blurs themselves
        continue
    im = Image.open(p).convert('RGB')
    glow = os.path.join(d, b + '_glow.jpg')
    if os.path.exists(glow):
        # the venue's own lamps lit, a little: out of focus they're what the picture is made of
        g = Image.open(glow).convert('RGB').resize(im.size)
        from PIL import ImageChops
        im = ImageChops.add(im, Image.eval(g, lambda v: v * 6 // 10))
    im = im.resize((240, 136), Image.LANCZOS).filter(ImageFilter.GaussianBlur(4.5))
    im.save(os.path.join(d, b + '_blur.jpg'), quality=82)
    n += 1
print(n, 'venues')
