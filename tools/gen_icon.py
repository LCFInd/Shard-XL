# SPDX-License-Identifier: GPL-3.0-only
"""The app icon: assets/AppIcon.iconset/*.png (the Mac build turns them into AppIcon.icns with iconutil).
Run from the repository root:  python3 tools/gen_icon.py"""
import os
from PIL import Image, ImageDraw, ImageFont

def icon(px):
    s = 4  # draw big, then shrink
    W = px * s
    im = Image.new('RGBA', (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    m = int(W * 0.09)
    d.rounded_rectangle([m, m, W - m, W - m], radius=int(W * 0.2), fill=(0, 0, 0, 255), outline=(51, 51, 51, 255), width=max(1, W // 128))
    f = ImageFont.truetype('assets/fonts/InterTight.ttf', int(W * 0.40)); f.set_variation_by_axes([800])
    g = ImageFont.truetype('assets/fonts/InterTight.ttf', int(W * 0.13)); g.set_variation_by_axes([700])
    d.text((W / 2, W * 0.60), 'XL', font=f, fill=(255, 59, 48, 255), anchor='mm')
    d.text((W / 2, W * 0.30), 'SHARD', font=g, fill=(255, 255, 255, 255), anchor='mm')
    return im.resize((px, px), Image.LANCZOS)

os.makedirs('assets/AppIcon.iconset', exist_ok=True)
for base in (16, 32, 128, 256, 512):
    icon(base).save(f'assets/AppIcon.iconset/icon_{base}x{base}.png')
    icon(base * 2).save(f'assets/AppIcon.iconset/icon_{base}x{base}@2x.png')
print('assets/AppIcon.iconset written')
