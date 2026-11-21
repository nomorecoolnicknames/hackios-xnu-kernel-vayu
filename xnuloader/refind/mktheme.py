#!/usr/bin/env python3
"""
The HakiOS theme of rEFInd for vayu's portrait screen (1080x2400): drawn here from scratch, no picture taken
from anywhere (no vendor logo: the HakiOS mark is a hexagon of our own, Android a phone with layers, the
diagnostics a gear). Needs Pillow and a DejaVu font (DejaVu fonts' licence: Bitstream Vera / public domain
changes; the glyphs are rendered into font.png).

  mktheme.py OUTDIR [--font-dir /usr/share/fonts/truetype/dejavu]

OUTDIR/ gets: background.png (the whole screen: rEFInd's "banner" with banner_scale fillscreen), selection_big.png
(the bar under the chosen row, its size the row's: vertical_menu), font.png (rEFInd's font format: 96 cells
of ASCII 32..127 in one row, black glyphs in alpha), icons/os_hakios.png, os_android.png, tool_diag.png,
os_unknown.png (192x192: big_icon_size 192); and for refind_conf=bmp the same in 24-bit BMP, opaque on the
background's colour: background-half.bmp (540x1200, rEFInd scales it up), selection_big.bmp, icons/*.bmp.
"""
import argparse, math, os
from PIL import Image, ImageDraw, ImageFont, ImageFilter

W, H = 1080, 2400
ICON = 192
TILE = ICON * 9 // 8                    # rEFInd's TileSizes[0]: the row's height
ROW_W = W - 2 * (W // 12)               # VMenuRowWidth() of the vertical_menu patch
SS = 4                                  # supersampling

BG_TOP, BG_BOTTOM = (13, 17, 28), (4, 5, 9)
ACCENT = (61, 126, 255)
TEAL = (42, 209, 201)
GREEN = (78, 205, 120)
AMBER = (255, 176, 64)
GREY = (150, 158, 176)

ap = argparse.ArgumentParser()
ap.add_argument('out')
ap.add_argument('--font-dir', default='/usr/share/fonts/truetype/dejavu')
args = ap.parse_args()
os.makedirs(os.path.join(args.out, 'icons'), exist_ok=True)


def font(name, size):
    return ImageFont.truetype(os.path.join(args.font_dir, name), size)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))


def hexagon(cx, cy, r, rot=math.pi / 6):
    return [(cx + r * math.cos(rot + k * math.pi / 3), cy + r * math.sin(rot + k * math.pi / 3)) for k in range(6)]


def down(img, size):
    return img.resize(size, Image.LANCZOS)


def hakios_mark(size, ring=TEAL, core=ACCENT):
    """the HakiOS mark: a hexagon ring, a stylised H of two bars and a slanted bridge, a dot of light"""
    s = size * SS
    im = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = s / 2
    d.polygon(hexagon(c, c, s * 0.47), fill=ring + (255,))
    d.polygon(hexagon(c, c, s * 0.40), fill=(10, 14, 24, 255))
    # a soft fill inside
    grad = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    gd = ImageDraw.Draw(grad)
    for i in range(60):
        t = i / 59
        gd.polygon(hexagon(c, c, s * 0.40 * (1 - t * 0.9)), fill=lerp((22, 34, 66), (40, 70, 150), t) + (255,))
    mask = Image.new('L', (s, s), 0)
    ImageDraw.Draw(mask).polygon(hexagon(c, c, s * 0.40), fill=255)
    im.paste(grad, (0, 0), mask)
    bw, bh = s * 0.085, s * 0.40
    for x in (c - s * 0.15, c + s * 0.15 - bw):
        d.rounded_rectangle([x, c - bh / 2, x + bw, c + bh / 2], radius=bw / 2, fill=(235, 242, 255, 255))
    # the bridge: a slanted band from the left bar's middle to the right one's
    d.polygon([(c - s * 0.15 + bw * 0.5, c + s * 0.03), (c + s * 0.15 - bw * 0.5, c - s * 0.07),
               (c + s * 0.15 - bw * 0.5, c + s * 0.01), (c - s * 0.15 + bw * 0.5, c + s * 0.11)],
              fill=(235, 242, 255, 255))
    d.ellipse([c + s * 0.22, c - s * 0.30, c + s * 0.29, c - s * 0.23], fill=ring + (255,))
    return down(im, (size, size))


def phone_layers(size, col=GREEN):
    """Android, our own picture: a phone with three stacked layers on its screen"""
    s = size * SS
    im = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    pw, ph = s * 0.50, s * 0.86
    x0, y0 = (s - pw) / 2, (s - ph) / 2
    d.rounded_rectangle([x0, y0, x0 + pw, y0 + ph], radius=s * 0.08, fill=col + (255,))
    d.rounded_rectangle([x0 + s * 0.035, y0 + s * 0.07, x0 + pw - s * 0.035, y0 + ph - s * 0.09], radius=s * 0.03,
                        fill=(12, 26, 20, 255))
    d.rounded_rectangle([x0 + pw / 2 - s * 0.05, y0 + ph - s * 0.065, x0 + pw / 2 + s * 0.05, y0 + ph - s * 0.035],
                        radius=s * 0.015, fill=(12, 26, 20, 255))
    cx, cy = s / 2, s * 0.47
    for k, a in enumerate((150, 200, 255)):
        yy = cy + s * 0.10 - k * s * 0.10
        w, h = s * 0.17, s * 0.075
        d.polygon([(cx - w, yy), (cx, yy - h), (cx + w, yy), (cx, yy + h)], fill=lerp(col, (255, 255, 255), 0.25 * k) + (a,))
    return down(im, (size, size))


def gear(size, col=AMBER, teeth=8):
    """the diagnostics: a gear"""
    s = size * SS
    im = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = s / 2
    ro, ri, tw = s * 0.44, s * 0.33, 0.22
    pts = []
    for k in range(teeth):
        a = 2 * math.pi * k / teeth
        for da, r in ((-tw * 1.25, ri), (-tw * 0.75, ro), (tw * 0.75, ro), (tw * 1.25, ri)):
            pts.append((c + r * math.cos(a + da / 2 * 2 * math.pi / teeth * 2), c + r * math.sin(a + da / 2 * 2 * math.pi / teeth * 2)))
    d.polygon(pts, fill=col + (255,))
    d.ellipse([c - ri, c - ri, c + ri, c + ri], fill=col + (255,))
    d.ellipse([c - s * 0.15, c - s * 0.15, c + s * 0.15, c + s * 0.15], fill=(0, 0, 0, 0))
    # a pulse line across: the "diagnostics"
    d.ellipse([c - s * 0.15, c - s * 0.15, c + s * 0.15, c + s * 0.15], outline=(255, 236, 200, 255), width=int(s * 0.03))
    return down(im, (size, size))


def efi_app(size, col=GREY):
    """an EFI application rEFInd found: a window with a prompt"""
    s = size * SS
    im = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    m = s * 0.12
    d.rounded_rectangle([m, m * 1.3, s - m, s - m * 1.3], radius=s * 0.06, fill=col + (255,))
    d.rounded_rectangle([m + s * 0.03, m * 1.3 + s * 0.11, s - m - s * 0.03, s - m * 1.3 - s * 0.03], radius=s * 0.03,
                        fill=(16, 19, 26, 255))
    for i, cc in enumerate(((255, 95, 87), (255, 189, 46), (40, 200, 64))):
        x = m + s * 0.06 + i * s * 0.07
        d.ellipse([x, m * 1.3 + s * 0.035, x + s * 0.045, m * 1.3 + s * 0.08], fill=cc + (255,))
    f = font('DejaVuSansMono-Bold.ttf', int(s * 0.22))
    d.text((m + s * 0.08, s * 0.42), '>_', font=f, fill=(220, 226, 240, 255))
    return down(im, (size, size))


def background():
    im = Image.new('RGB', (W, H))
    px = im.load()
    for y in range(H):
        row = lerp(BG_TOP, BG_BOTTOM, (y / H) ** 0.8)
        for x in range(W):
            px[x, y] = row
    # a soft glow behind the mark
    glow = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([W / 2 - 420, 20, W / 2 + 420, 560], fill=ACCENT + (70,))
    glow = glow.filter(ImageFilter.GaussianBlur(120))
    im = Image.alpha_composite(im.convert('RGBA'), glow)
    mark = hakios_mark(200)
    im.alpha_composite(mark, (W // 2 - 100, 110))
    d = ImageDraw.Draw(im)
    t = font('DejaVuSans-Bold.ttf', 96)
    d.text((W / 2, 400), 'HakiOS', font=t, fill=(240, 244, 252), anchor='mm')
    d.text((W / 2, 500), 'choose a system', font=font('DejaVuSans.ttf', 40), fill=GREY, anchor='mm')
    # the keys, at the bottom
    k = font('DejaVuSans-Bold.ttf', 34)
    l = font('DejaVuSans.ttf', 38)
    y = 2170
    for keys, what in ((('Vol +', 'Vol −'), 'move'), (('Power',), 'start')):
        widths = [d.textlength(s, font=k) + 44 for s in keys]
        total = sum(widths) + 18 * (len(keys) - 1) + 30 + d.textlength(what, font=l)
        x = (W - total) / 2
        for s, w in zip(keys, widths):
            d.rounded_rectangle([x, y - 34, x + w, y + 34], radius=16, outline=(90, 100, 124), width=3, fill=(20, 25, 38))
            d.text((x + w / 2, y), s, font=k, fill=(220, 226, 240), anchor='mm')
            x += w + 18
        d.text((x + 12, y), what, font=l, fill=GREY, anchor='lm')
        y += 100
    return im.convert('RGB')


def selection():
    s = SS
    im = Image.new('RGBA', (ROW_W * s, TILE * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, ROW_W * s - 1, TILE * s - 1], radius=36 * s, fill=(36, 78, 170, 150),
                        outline=ACCENT + (255,), width=4 * s)
    return down(im, (ROW_W, TILE))


def fontstrip(size=44):
    f = font('DejaVuSansMono-Bold.ttf', size)
    asc, desc = f.getmetrics()
    cw = int(round(f.getlength('M')))
    ch = asc + desc + 4
    im = Image.new('RGBA', (cw * 96, ch), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for i in range(96):
        c = chr(32 + i) if i < 95 else '?'
        d.text((i * cw, 2), c, font=f, fill=(0, 0, 0, 255))
    return im


def opaque(im, under=(10, 13, 21)):
    base = Image.new('RGBA', im.size, under + (255,))
    base.alpha_composite(im.convert('RGBA'))
    return base.convert('RGB')


bg = background()
bg.save(os.path.join(args.out, 'background.png'), optimize=True)
bg.resize((W // 2, H // 2), Image.LANCZOS).save(os.path.join(args.out, 'background-half.bmp'))
sel = selection()
sel.save(os.path.join(args.out, 'selection_big.png'), optimize=True)
opaque(sel).save(os.path.join(args.out, 'selection_big.bmp'))
fontstrip().save(os.path.join(args.out, 'font.png'), optimize=True)
for name, im in (('os_hakios', hakios_mark(ICON)), ('os_android', phone_layers(ICON)), ('tool_diag', gear(ICON)),
                 ('os_unknown', efi_app(ICON))):
    im.save(os.path.join(args.out, 'icons', name + '.png'), optimize=True)
    opaque(im).save(os.path.join(args.out, 'icons', name + '.bmp'))
print(f'{args.out}: background {W}x{H}, selection {ROW_W}x{TILE}, icons {ICON}')
