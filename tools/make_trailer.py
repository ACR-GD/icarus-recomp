#!/usr/bin/env python3
"""Cut the release trailer from recorded footage.

usage: make_trailer.py footage.mp4 music.wav CrimsonPro[wght].ttf CrimsonPro-Italic[wght].ttf launcher.png out.mp4

footage.mp4: 640x480 30 fps recording (ICARUS_RECORD, see README). Text cards are drawn with Pillow
(this ffmpeg build has no drawtext) and overlaid; game frames are upscaled sharply to 1440x1080 over a
blurred, darkened copy that fills the 16:9 frame.
"""
import os, subprocess, sys
from PIL import Image, ImageDraw, ImageFont, ImageFilter

footage, music, ttf, ttf_it, launcher_png, out = sys.argv[1:7]
W, H, FPS = 1920, 1080, 30
TMP = os.path.join(os.path.dirname(os.path.abspath(out)), 'trailer_tmp')
os.makedirs(TMP, exist_ok=True)
GOLD, CREAM = (226, 192, 106), (236, 230, 216)

def font(size, var=b'SemiBold', italic=False):
    f = ImageFont.truetype(ttf_it if italic else ttf, size)
    try: f.set_variation_by_name(var)
    except Exception: pass
    return f

def shadow_text(img, xy, text, f, fill, anchor='mm', blur=6, spread=3):
    sh = Image.new('RGBA', img.size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).text(xy, text, font=f, fill=(0, 0, 0, 230), anchor=anchor, stroke_width=spread, stroke_fill=(0, 0, 0, 230))
    img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(blur)))
    ImageDraw.Draw(img).text(xy, text, font=f, fill=fill + (255,), anchor=anchor)

def caption(name, title, sub=None, top=False, mid=False):
    """lower-third (or top) caption: gold title, cream subtitle, over a soft dark band"""
    im = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    band = Image.new('RGBA', (W, 300), (0, 0, 0, 0))
    for y in range(300):
        ImageDraw.Draw(band).line([(0, y), (W, y)], fill=(0, 0, 0, int(190 * (y / 300) ** 1.4)))
    if top: band = band.transpose(Image.FLIP_TOP_BOTTOM)
    if not mid: im.alpha_composite(band, (0, 0 if top else H - 300))
    y1, y2 = (95, 170) if top else (H - (150 if sub else 110), H - 75)
    if mid: y1, y2 = 520, 595     # e.g. the empty starfield between the title logo and the menu
    shadow_text(im, (W // 2, y1), title, font(70, b'Bold'), GOLD)
    if sub: shadow_text(im, (W // 2, y2), sub, font(44, b'Medium Italic', True), CREAM)
    p = os.path.join(TMP, name + '.png'); im.save(p); return p

def card(name, lines, bg=None):
    """full-screen card. lines: (text, size, var, italic, colour, y)"""
    im = Image.new('RGBA', (W, H), (0, 0, 0, 0 if bg == 'clear' else 255))
    if isinstance(bg, Image.Image): im.alpha_composite(bg)
    for text, size, var, it, col, y in lines:
        shadow_text(im, (W // 2, y), text, font(size, var, it), col)
    p = os.path.join(TMP, name + '.png'); im.save(p); return p

def rule(img, y, w=520):
    d = ImageDraw.Draw(img); d.line([(W // 2 - w // 2, y), (W // 2 + w // 2, y)], fill=(138, 116, 64, 255), width=2)

def run(args):
    subprocess.run(['ffmpeg', '-v', 'error', '-y'] + args, check=True)

FG = ("[0:v]split[a][b];[b]scale=1920:1440:flags=neighbor,crop=1920:1080,gblur=sigma=40,eq=saturation=0.6,colorchannelmixer=rr=0.3:gg=0.3:bb=0.3[bg];"
      "[a]scale=1920:1440:flags=neighbor,scale=1440:1080:flags=lanczos{blur}[fg];[bg][fg]overlay=(W-w)/2:0[base]")

def game_clip(name, start, dur, overlay=None, blur=False, ov_in=0.4):
    """footage[start:start+dur] framed at 1080p, optional text overlay fading in/out"""
    outp = os.path.join(TMP, name + '.mp4')
    g = FG.format(blur=',gblur=sigma=14,eq=brightness=-0.35' if blur else '')
    if overlay:
        g += (";[1:v]format=rgba,fade=t=in:st=%.2f:d=0.7:alpha=1,fade=t=out:st=%.2f:d=0.6:alpha=1[ov];"
              "[base][ov]overlay=0:0,format=yuv420p[v]") % (ov_in, dur - 0.9)
        run(['-ss', str(start), '-t', str(dur), '-i', footage, '-loop', '1', '-t', str(dur), '-i', overlay,
             '-filter_complex', g, '-map', '[v]', '-r', str(FPS), '-c:v', 'libx264', '-crf', '16', '-preset', 'medium', outp])
    else:
        run(['-ss', str(start), '-t', str(dur), '-i', footage, '-filter_complex', g + ';[base]format=yuv420p[v]',
             '-map', '[v]', '-r', str(FPS), '-c:v', 'libx264', '-crf', '16', '-preset', 'medium', outp])
    return outp, dur

def still_clip(name, png, dur, zoom=0.0):
    outp = os.path.join(TMP, name + '.mp4')
    vf = 'format=yuv420p'
    if zoom:  # slow push-in
        vf = ("scale=%d:%d,zoompan=z='1+%f*on/%d':x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)':d=1:s=%dx%d:fps=%d,format=yuv420p"
              % (W * 2, H * 2, zoom, dur * FPS, W, H, FPS))
    run(['-loop', '1', '-framerate', str(FPS), '-t', str(dur), '-i', png, '-vf', vf, '-r', str(FPS), '-c:v', 'libx264', '-crf', '16', '-preset', 'medium', outp])
    return outp, dur

# ---------------------------------------------------------------- cards
cap_1998 = caption('cap1998', 'A 1998 PC role-playing game', 'Icarus: Sanctuary of the Gods')
cap_win95 = caption('capwin95', 'Built for Windows 95.', 'Recompiled for today.')
cap_title = caption('captitle', 'Recompiled to run natively', 'No emulator. No Windows 95.', mid=True)
cap_text = caption('captext', 'Crisp, readable dialogue', 'and the full original story')
cap_battle = caption('capbattle', 'Smooth, display-synced battles', 'with a lag-free mouse cursor')
c_intro = card('intro', [('SECOND SAVE', 64, b'Bold', False, GOLD, 500), ('presents', 42, b'Medium Italic', True, CREAM, 572)])
feat = Image.new('RGBA', (W, H), (0, 0, 0, 0))
c_feat = card('features', [
    ('Native on macOS  ·  Windows  ·  Linux', 64, b'Bold', False, GOLD, 330),
    ('Sharp scaling  ·  Fullscreen  ·  Fast-forward', 46, b'Medium', False, CREAM, 470),
    ('Restored music and sound  ·  Faster dialogue', 46, b'Medium', False, CREAM, 550),
    ('Lag-free cursor  ·  Screenshots  ·  Simple launcher', 46, b'Medium', False, CREAM, 630),
], bg='clear')
# launcher card: screenshot on a dark gradient
lc = Image.new('RGBA', (W, H), (0, 0, 0, 255))
for y in range(H):
    ImageDraw.Draw(lc).line([(0, y), (W, y)], fill=(int(24 - 14 * y / H), int(19 - 11 * y / H), int(15 - 8 * y / H), 255))
shot = Image.open(launcher_png).convert('RGBA').resize((880, 660), Image.LANCZOS)
fr = Image.new('RGBA', (896, 676), (138, 116, 64, 255)); lc.alpha_composite(fr, ((W - 896) // 2, 112)); lc.alpha_composite(shot, ((W - 880) // 2, 120))
c_launch = card('launcher', [('Bring your own discs. Install in seconds.', 58, b'Bold', False, GOLD, 860),
                             ('Pick your Icarus CD images, click Install, then Play.', 40, b'Medium Italic', True, CREAM, 935)], bg=lc)
end = Image.new('RGBA', (W, H), (0, 0, 0, 255))
for y in range(H):
    ImageDraw.Draw(end).line([(0, y), (W, y)], fill=(int(22 - 15 * y / H), int(17 - 12 * y / H), int(13 - 9 * y / H), 255))
rule(end, 452)
c_end = card('end', [
    ('ICARUS', 170, b'Bold', False, GOLD, 300),
    ('Sanctuary of the Gods', 56, b'Medium Italic', True, CREAM, 405),
    ('Recomp by Second Save', 44, b'SemiBold', False, GOLD, 480),
    ('AVAILABLE NOW', 74, b'Bold', False, CREAM, 640),
    ('github.com/ACR-GD/icarus-recomp', 46, b'Medium', False, GOLD, 730),
    ('macOS  ·  Windows  ·  Linux', 40, b'Medium', False, CREAM, 800),
    ('Requires the original game discs. No game files included. Unofficial fan project.', 30, b'Medium Italic', True, (170, 160, 145), 1010),
], bg=end)

# ---------------------------------------------------------------- edit (start, duration in footage seconds)
clips = [
    still_clip('s00', c_intro, 4),
    game_clip('s01', 15.3, 5.0, cap_1998, ov_in=0.6),       # intro movie: canyon
    game_clip('s02', 25.0, 4.0),                             # the heroes
    game_clip('s03', 33.5, 4.5, cap_win95, ov_in=0.3),       # village
    game_clip('s04', 46.5, 3.5),                             # the villain
    game_clip('s05', 5.5, 5.5, cap_title, ov_in=0.8),        # flaming title screen
    game_clip('s06', 52.0, 4.5),                             # prologue painting
    game_clip('s07', 74.5, 6.0, cap_text, ov_in=0.6),        # dialogue
    game_clip('s08', 98.0, 3.0),                             # "Battle 1"
    game_clip('s09', 105.5, 6.0, cap_battle, ov_in=0.6),     # battle grid
    game_clip('s10', 112.0, 6.0, c_feat, blur=True, ov_in=0.3),  # features over blurred battle
    still_clip('s11', c_launch, 6, zoom=0.04),
    still_clip('s12', c_end, 8, zoom=0.03),
]
X = 0.6   # crossfade length
inputs, graph, prev, t = [], [], '0:v', clips[0][1]
for k, (p, d) in enumerate(clips):
    inputs += ['-i', p]
for k in range(1, len(clips)):
    off = t - X
    graph.append('[%s][%d:v]xfade=transition=fade:duration=%.2f:offset=%.2f[x%d]' % (prev, k, X, off, k))
    prev = 'x%d' % k
    t = off + clips[k][1]
total = t
graph.append('[%s]fade=t=in:st=0:d=0.8,fade=t=out:st=%.2f:d=1.2,format=yuv420p[v]' % (prev, total - 1.2))
graph.append('[%d:a]atrim=0:%.2f,asetpts=PTS-STARTPTS,afade=t=in:st=0:d=1.5,afade=t=out:st=%.2f:d=3.5,volume=0.9[a]' % (len(clips), total, total - 3.5))
run(inputs + ['-i', music, '-filter_complex', ';'.join(graph), '-map', '[v]', '-map', '[a]',
              '-c:v', 'libx264', '-crf', '18', '-preset', 'slow', '-profile:v', 'high', '-pix_fmt', 'yuv420p',
              '-c:a', 'aac', '-b:a', '192k', '-movflags', '+faststart', out])
print('trailer: %s (%.1f s)' % (out, total))
