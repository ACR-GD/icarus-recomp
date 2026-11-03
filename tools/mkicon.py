#!/usr/bin/env python3
"""Draw the port's app icon (not the game's artwork): gold 'I' on a dark rounded square.
usage: mkicon.py CrimsonPro[wght].ttf outdir  -> icon_1024.png, icarus.ico, Icarus.iconset/"""
import sys, os
from PIL import Image, ImageDraw, ImageFont, ImageFilter
ttf, out = sys.argv[1], sys.argv[2]
S = 1024
im = Image.new('RGBA', (S, S), (0, 0, 0, 0))
bg = Image.new('RGBA', (S, S))
for y in range(S):   # vertical gradient
    t = y / S
    c = (int(34 - 22 * t), int(27 - 17 * t), int(21 - 13 * t), 255)
    ImageDraw.Draw(bg).line([(0, y), (S, y)], fill=c)
mask = Image.new('L', (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle([64, 64, S - 64, S - 64], radius=190, fill=255)
im.paste(bg, (0, 0), mask)
d = ImageDraw.Draw(im)
d.rounded_rectangle([96, 96, S - 96, S - 96], radius=160, outline=(138, 116, 64, 255), width=10)
f = ImageFont.truetype(ttf, 640)
try: f.set_variation_by_name(b'Bold')
except Exception: pass
glow = Image.new('RGBA', (S, S), (0, 0, 0, 0))
ImageDraw.Draw(glow).text((S / 2, S / 2 + 20), 'I', font=f, fill=(212, 175, 90, 160), anchor='mm')
im.alpha_composite(glow.filter(ImageFilter.GaussianBlur(28)))
d.text((S / 2, S / 2 + 20), 'I', font=f, fill=(226, 192, 106, 255), anchor='mm')
os.makedirs(out, exist_ok=True)
im.save(os.path.join(out, 'icon_1024.png'))
im.save(os.path.join(out, 'icarus.ico'), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
iset = os.path.join(out, 'Icarus.iconset'); os.makedirs(iset, exist_ok=True)
for s in (16, 32, 128, 256, 512):
    im.resize((s, s), Image.LANCZOS).save(os.path.join(iset, 'icon_%dx%d.png' % (s, s)))
    im.resize((s * 2, s * 2), Image.LANCZOS).save(os.path.join(iset, 'icon_%dx%d@2x.png' % (s, s)))
print('ok')
