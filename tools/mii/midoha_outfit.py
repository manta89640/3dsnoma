#!/usr/bin/env python3
"""ミドハ's dress: a texture for the standard Mii body (Male_Body), any outfit painted on one image.

    tools/mii/midoha_outfit.py paint OUT.png            the hanbok, painted (tools/mii/midoha_outfit.png)
    tools/mii/midoha_outfit.py template OUT.png         a guide to paint another outfit on (the body's
                                                        parts' places, no Nintendo data: measures only)
    tools/mii/midoha_outfit.py DATA [--png IN.png]      IN.png (default the hanbok) -> DATA/wuhu/midoha_outfit.bin

The image is 512 x 256: the FRONT on its left half, the BACK on its right half drawn as seen from
behind. The body has no texture coordinates; engine/wuhu/midoha.cpp makes them from the position in
the bind pose (the T-pose, Wii no Ma's Mii units: 158 tall, +z the front, +x the Mii's left):
    front half, the triangles that face forward in the bind pose:  u = (x + 72) / 144, v = (128 - y) / 128
    back half, the others:                                         u = (72 - x) / 144 (seen from behind)
(u across its half, v down). So the image is the body seen from the front and from behind, arms out:
the torso's middle at the half's middle, the hands at its sides (|x| 64-72), the shoulders at y 112
(v 0.125), the hips at y 71 (v 0.45), the feet at the bottom. Where front and back meet (the torso's
sides, x +-16..20, the arms' upper and lower edges, the legs' sides) both halves must carry the same
colour at the same height: triangles there fall on either side. Seen from the front the body's sides
are stretched (a projection from the front), so keep them plain.

The game's file: "MDOF", u16 width, u16 height (of each half), f32 x at u = 0 and at u = 1, f32 y at
v = 0 and at v = 1 (little-endian), then the front's texels and the back's (the back mirrored, so
both are addressed by the front's u), each GX RGB565 (big-endian, 4x4 tiles).
"""
import argparse
import os
import struct

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_PNG = os.path.join(HERE, "midoha_outfit.png")
W, H = 512, 256
X0, X1, Y0, Y1 = -72.0, 72.0, 128.0, 0.0

# the concept's colours (sampled), and proposals where it shows nothing
PAL = dict(
    green="#2B614B",      # jeogori (the jacket)
    green_lt="#427760",   # its light edge (hem, the over-panel's edge)
    trim="#1B4234",       # git (the collar band), kkeutdong (the cuffs), seams
    white="#EDE3DC",      # dongjeong (the white collar strip)
    white_sh="#D6CBC3",   # its shaded edge
    skin="#FFBC80",       # the neck's V and the hands: RFL skin tone 1, his face's
    skin_sh="#CC9266",    # the shade under the chin
    belt="#271B27",       # the belt
    belt_dk="#0B070C",    # its edges, the buckle's frames
    baji="#271B27",       # the skirt / baji below the belt
    baji_fold="#18111A",  # its folds
    tie="#1B4234",        # daenim, the ankle ties
    shoe="#17141A",       # shoes
    shoe_trim="#EDE3DC",  # their edge
)


def paint(pal=PAL, hands_skin=True):
    """the hanbok: green jeogori with the white crossed collar and its dark band (the wearer's left
    panel over the right, as drawn), the dark belt with its buckle, dark baji to the ankle ties"""
    im = Image.new("RGB", (W, H), pal["green"])
    d = ImageDraw.Draw(im)
    half = W // 2

    def F(x, y):  # the front half's pixel of body point (x, y)
        return ((x - X0) / (X1 - X0) * half, (y - Y0) / (Y1 - Y0) * H)

    def B(x, y):  # the back half's, seen from behind
        return (half + (X1 - x) / (X1 - X0) * half, (y - Y0) / (Y1 - Y0) * H)

    def poly(M, pts, c):
        d.polygon([M(*p) for p in pts], fill=pal[c])

    def rect(M, x0, y0, x1, y1, c):
        poly(M, [(x0, y0), (x1, y0), (x1, y1), (x0, y1)], c)

    def line(M, pts, c, w):
        d.line([M(*p) for p in pts], fill=pal[c], width=max(1, int(round(w / 128.0 * H))))

    for M in (F, B):
        # below the belt: baji, its folds, the ankle ties, shoes
        rect(M, -72, -1, 72, 66.5, "baji")
        for x in (-9.5, -4.5, 4.5, 9.5):
            line(M, [(x, 64), (x * 1.08, 16)], "baji_fold", 0.9)
        rect(M, -72, 8.5, 72, 12.0, "tie")
        rect(M, -72, -1, 72, 8.5, "shoe")
        line(M, [(-72, 8.0), (72, 8.0)], "shoe_trim", 0.6)
        # the belt (across the top's and the trousers' seam at y 71)
        rect(M, -72, 66.5, 72, 74.5, "belt")
        line(M, [(-72, 74.3), (72, 74.3)], "belt_dk", 0.6)
        line(M, [(-72, 66.7), (72, 66.7)], "belt_dk", 0.6)
        # the jacket's hem just above the belt
        line(M, [(-72, 75.2), (72, 75.2)], "trim", 0.9)
        # the sleeves: shoulder seams, cuffs, hands
        for s in (-1, 1):
            line(M, [(s * 21.0, 95), (s * 21.0, 112)], "trim", 0.8)
            rect(M, s * 55.5, 94, s * 63.6, 112, "trim")
            line(M, [(s * 55.5, 94), (s * 55.5, 112)], "green_lt", 0.7)
            if hands_skin:
                rect(M, s * 63.6, 94, s * 72, 112, "skin")
    # the front: the crossed collar; the dark band from his left neck to his right hip
    poly(F, [(7.0, 112.5), (14.5, 112.5), (-10.5, 75.5), (-18.5, 75.5)], "trim")
    # the under-panel's collar on his right, seen above the crossing
    poly(F, [(-10.0, 112.5), (-4.5, 112.5), (1.5, 99.5), (-2.5, 97.5)], "trim")
    poly(F, [(-8.6, 112.5), (-5.6, 112.5), (-0.6, 101.5), (-3.2, 100.4)], "white")
    # the neck's V between the collars, the shade under the chin
    poly(F, [(-5.6, 112.5), (7.0, 112.5), (0.6, 101.0), (-0.6, 101.5)], "skin")
    poly(F, [(-5.6, 112.5), (7.0, 112.5), (5.6, 109.8), (-4.4, 109.8)], "skin_sh")
    rect(F, -4.7, 112.5, 4.7, 129, "skin")  # the neck (mostly inside the head)
    # the white strip on the band's neck side, its end squared at y 92
    poly(F, [(7.0, 112.5), (11.0, 112.5), (-1.6, 92.0), (-5.6, 92.0)], "white")
    line(F, [(11.0, 112.5), (-1.6, 92.0)], "white_sh", 0.6)
    line(F, [(14.5, 112.5), (-10.5, 75.5)], "green_lt", 0.6)
    # the buckle at the front: a dark frame round a dark inner frame on the belt's colour

    def frame(M, x0, y0, x1, y1, w, c):
        for pts in ([(x0, y0), (x1, y0)], [(x1, y0), (x1, y1)], [(x1, y1), (x0, y1)], [(x0, y1), (x0, y0)]):
            line(M, pts, c, w)
    frame(F, -4.4, 67.6, 4.4, 73.4, 1.0, "belt_dk")
    frame(F, -2.3, 69.3, 2.3, 71.7, 0.8, "belt_dk")
    # the back: the collar round the neck, the centre seam
    rect(B, -9.0, 104.5, 9.0, 112.5, "trim")
    rect(B, -7.5, 108.0, 7.5, 112.5, "white")
    rect(B, -4.7, 112.5, 4.7, 129, "skin")
    line(B, [(0, 76), (0, 104.5)], "trim", 0.6)
    return im


def template():
    """a guide: the halves, the body's measures (the places above), a grid of 8 units"""
    im = Image.new("RGB", (W * 2, H * 2), (250, 250, 250))
    d = ImageDraw.Draw(im)
    s = 2
    half = W // 2
    for hx in (0, half):
        for k in range(0, 145, 8):
            x = (hx + k / 144.0 * half) * s
            d.line([(x, 0), (x, H * s)], fill=(225, 225, 225))
        for k in range(0, 129, 8):
            y = k / 128.0 * H * s
            d.line([(hx * s, y), ((hx + half) * s, y)], fill=(225, 225, 225))

    def at(x, y, back):
        u = (X1 - x) / 144.0 if back else (x - X0) / 144.0
        return ((half if back else 0) + u * half) * s, (Y0 - y) / 128.0 * H * s
    for back in (False, True):
        for (x0, y0, x1, y1, name) in ((-16, 112, 16, 71, "torso"), (-16, 71, 16, 0, "legs"),
                                       (16, 112, 72, 94, "arm"), (-72, 112, -16, 94, "arm"),
                                       (63.6, 112, 72, 94, "hand"), (-72, 112, -63.6, 94, "hand")):
            a, b = at(x0, y0, back), at(x1, y1, back)
            d.rectangle([min(a[0], b[0]), min(a[1], b[1]), max(a[0], b[0]), max(a[1], b[1])], outline=(90, 90, 90))
            d.text((min(a[0], b[0]) + 4, min(a[1], b[1]) + 4), name, fill=(60, 60, 60))
        d.text(((half if back else 0) * s + 6, 6), "BACK (seen from behind)" if back else "FRONT", fill=(0, 0, 0))
    d.line([(half * s, 0), (half * s, H * s)], fill=(0, 0, 0), width=2)
    return im


def gx_rgb565(img):
    """GX RGB565 texels: 4x4 tiles in rows, each 16 big-endian texels"""
    w, h = img.size
    px = img.load()
    out = bytearray()
    for ty in range(0, h, 4):
        for tx in range(0, w, 4):
            for y in range(ty, ty + 4):
                for x in range(tx, tx + 4):
                    r, g, b = px[x, y][:3]
                    out += struct.pack(">H", (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3))
    return bytes(out)


def outfit_file(png):
    im = Image.open(png).convert("RGB")
    if im.size != (W, H):
        im = im.resize((W, H), Image.LANCZOS)
    half = W // 2
    front = im.crop((0, 0, half, H))
    back = im.crop((half, 0, W, H)).transpose(Image.FLIP_LEFT_RIGHT)  # addressed by the front's u
    head = b"MDOF" + struct.pack("<HH4f", half, H, X0, X1, Y0, Y1)
    return head + gx_rgb565(front) + gx_rgb565(back)


def write(data_dir, png=DEFAULT_PNG):
    out = os.path.join(data_dir, "wuhu", "midoha_outfit.bin")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    data = outfit_file(png)
    open(out, "wb").write(data)
    return out, len(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("what", help="paint, template, or the data directory")
    ap.add_argument("out", nargs="?", help="(paint, template) the PNG to write")
    ap.add_argument("--png", default=DEFAULT_PNG, help="the outfit painted (default the hanbok)")
    a = ap.parse_args()
    if a.what == "paint":
        paint().save(a.out or DEFAULT_PNG)
    elif a.what == "template":
        template().save(a.out or "midoha_outfit_template.png")
    else:
        out, n = write(a.what, a.png)
        print("%s: %d bytes (from %s)" % (out, n, a.png))


if __name__ == "__main__":
    main()
