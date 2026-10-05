#!/usr/bin/env python3
"""ミドハ's face parts, painted after the user's concept: his eyes, brows and mouth as a Mii's face
texture holds them, our own art (nothing of Nintendo's is read or written here).

    tools/mii/midoha_face.py DATA [--catchlight] [--png OUT.png]
        -> DATA/wuhu/midoha_face.bin (engine/wuhu/midoha.cpp MidohaFace writes it over the face
           textures RFL makes for him, so that RFL draws it on his face as any Mii's)
    tools/mii/midoha_face.py preview OUT.png [--catchlight]   the images on his skin, enlarged

Why ours: no Wii eye is the concept's -- an almond eye under a thin black upper line that ends in a
small wing at the outer corner, a short line under the outer corner only, a large blue iris and a
black pupil; every Wii eye that comes close has a heavy outline all round, lashes, or a lower line.
The brows (straight, fairly thin, tapering outward, blunt at the inner end) and the mouth (a thin
closed smile) are painted with them, so that the three read as one drawing. The nose stays RFL's
(nose 1, the check-mark line: it is a shape of its own over the face, not on this texture).

The texture: what RFL's face texture is, 256 x 256 texels over RFL's 64 face units (y down; the
parts' placement there is RFL's own: the eyes' centres at y = 18.452 + 1.0761 eye_y, the mouth at
29.259 + 1.0761 mouth_y; tools/mii/midoha.py's eye_y 12 and mouth_y 13 put them at 31.4 and 43.2),
RGB5A3 (GX's: RGB555 where opaque, ARGB3444 where not), blended over his skin by RFL (source alpha;
texels of alpha 0 dropped). Transparent texels carry the skin's colour so that filtering at a part's
edge blends toward the skin under it, not toward black.

The drawing: shapes in the concept's own pixels (its 1024 x 1024 image: the face centred at x 500,
his eyes' irises at y 406, 155 px apart; a face 290 px wide at the cheeks), mapped onto the texture by
one scale and offset (FIT): x 500 -> u 32, the iris line -> the eye line, CONCEPT_SCALE units a pixel.
Each shape is a signed distance field sampled 4 x 4 a texel (a 1024 x 1024 grid) and box-filtered to
256: clean edges, anti-aliased, no faceting. Images: 0 the face, 1 the blink (the eyes shut: the
upper line's curve alone, lowered to the eye's middle); the 7 expressions RFL keeps take image 0 but
the blink (5).
"""
import argparse
import math
import os
import struct
import sys

import numpy as np

N = 256                 # texels (RFL's 0x100 resolution)
SS = 4                  # samples a texel, each way
UNITS = 64.0            # RFL's face units across the texture
SKIN = (255, 188, 128)  # RFL skin tone 1 (his), the transparent texels' colour

# the concept's colours (sampled from it)
INK = (8, 6, 8)           # the eye and mouth lines (near black)
BROW = (31, 22, 29)       # the brows (#1F161D)
WHITE = (250, 241, 233)   # the eye's white
IRIS = (28, 84, 142)      # the concept's #205E98, a shade deeper: the Mii's light brightens his face
IRIS_RIM = (110, 160, 210)  # the iris's light edge (the concept's thin rim toward the outer corner)
PUPIL = (2, 2, 4)
CATCH = (255, 255, 255)

# The fit: the texture as the front view shows it, measured in-game with a grid painted on his face
# (face 1, the design shots' face_front; the frame shown at the TV's 16:9: a texture unit 5.54 px across
# and 5.8 px down at the face's middle, a face unit 1.567 head units both ways on RFL's mask), the
# concept drawn on it at k frame pixels a concept pixel (0.64: his face 34 units wide at the eyes as
# the concept's is 295 px), his eye line at v_eye (head y 29.2). At that scale the concept's eyes,
# mouth and chin fall where the Mii's are (eyes to chin 19.2 units, the Mii's 19.4); brow_dy and
# mouth_dy (concept pixels) move the brows and the mouth from where it puts them.
FIT = dict(cx=500.0, eye_y=406.0, v_eye=32.0, u_px=5.54, v_px=5.8, k=0.68, brow_dy=0.0, mouth_dy=0.0,
           eye_sy=1.1, brow_thick=1.15)


# ------------------------------------------------------------------------------------- geometry
def bezier(p0, p1, p2, p3, n=24):
    t = np.linspace(0.0, 1.0, n)[:, None]
    p0, p1, p2, p3 = (np.asarray(p, float) for p in (p0, p1, p2, p3))
    return (1 - t) ** 3 * p0 + 3 * (1 - t) ** 2 * t * p1 + 3 * (1 - t) * t * t * p2 + t ** 3 * p3


def path(*segments):
    """a polyline from cubic segments ((p0, p1, p2, p3), ...), each starting where the last ended"""
    pts = []
    for s in segments:
        b = bezier(*s)
        pts.extend(b if not pts else b[1:])
    return np.array(pts)


def smooth_curve(points, n=48):
    """a Catmull-Rom curve through `points` (n samples)"""
    P = [np.asarray(p, float) for p in points]
    P = [2 * P[0] - P[1]] + P + [2 * P[-1] - P[-2]]
    segs = len(P) - 3
    out = []
    for i in range(segs):
        p0, p1, p2, p3 = P[i:i + 4]
        m = max(2, n // segs)
        for k in range(m):
            t = k / m
            out.append(0.5 * (2 * p1 + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t
                              + (-p0 + 3 * p1 - 3 * p2 + p3) * t ** 3))
    out.append(P[-2])
    return np.array(out)


# ------------------------------------------------------------------------------------- the canvas
class Canvas:
    """premultiplied RGBA over the texture, SS x SS samples a texel; shapes are given in the concept's
    pixels and each sample is mapped there (the fit: an affine map, x from u alone, y from v alone)"""

    def __init__(self, fit, n=N, ss=SS):
        self.n, self.ss, self.fit = n, ss, fit
        self.m = n * ss
        self.spu = self.m / UNITS  # samples a texture unit
        # concept pixels a texture unit, each way (the face as the front view shows it: isotropic there)
        self.cu, self.cv = fit["u_px"] / fit["k"], fit["v_px"] / fit["k"]
        # a sample's size in concept pixels (the anti-aliasing ramp)
        self.sample = 0.5 * (self.cu + self.cv) / self.spu
        self.rgb = np.zeros((self.m, self.m, 3))
        self.a = np.zeros((self.m, self.m))

    def to_concept(self, u, v):
        f = self.fit
        return f["cx"] + (u - 32.0) * self.cu, f["eye_y"] + (v - f["v_eye"]) * self.cv

    def to_units(self, x, y):
        f = self.fit
        return 32.0 + (x - f["cx"]) / self.cu, f["v_eye"] + (y - f["eye_y"]) / self.cv

    def grid(self, lo, hi):
        """the samples whose concept positions are inside lo..hi: their positions (concept px), their
        slice"""
        u0, v0 = self.to_units(*lo)
        u1, v1 = self.to_units(*hi)
        i0, i1 = max(0, int(math.floor(v0 * self.spu)) - 2), min(self.m, int(math.ceil(v1 * self.spu)) + 2)
        j0, j1 = max(0, int(math.floor(u0 * self.spu)) - 2), min(self.m, int(math.ceil(u1 * self.spu)) + 2)
        if i1 <= i0 or j1 <= j0:
            return None
        x, _ = self.to_concept((np.arange(j0, j1) + 0.5) / self.spu, 0.0)
        _, y = self.to_concept(0.0, (np.arange(i0, i1) + 0.5) / self.spu)
        X, Y = np.meshgrid(x, y)
        return np.stack([X, Y], -1), (slice(i0, i1), slice(j0, j1))

    def put(self, sl, cov, colour, alpha=1.0):
        c = np.clip(cov, 0.0, 1.0) * alpha
        col = np.asarray(colour, float) / 255.0
        self.rgb[sl] = self.rgb[sl] * (1 - c[..., None]) + col * c[..., None]
        self.a[sl] = self.a[sl] * (1 - c) + c

    def coverage(self, sd):
        """a signed distance (concept px, < 0 inside) -> coverage over one sample's width"""
        return np.clip(0.5 - sd / self.sample, 0.0, 1.0)

    def fill(self, poly, colour, alpha=1.0, clip=None):
        poly = np.asarray(poly, float)
        g = self.grid(poly.min(0), poly.max(0))
        if g is None:
            return
        P, sl = g
        cov = self.coverage(sd_polygon(P, poly))
        if clip is not None:
            cov = cov * self.coverage(sd_polygon(P, clip))
        self.put(sl, cov, colour, alpha)

    def stroke(self, pts, widths, colour, alpha=1.0, clip=None):
        """a line along pts, `widths` (one, or one a point) wide, round ends"""
        pts = np.asarray(pts, float)
        w = np.broadcast_to(np.asarray(widths, float), (len(pts),))
        r = w.max() / 2 + 1.0
        g = self.grid(pts.min(0) - r, pts.max(0) + r)
        if g is None:
            return
        P, sl = g
        cov = self.coverage(sd_stroke(P, pts, w / 2))
        if clip is not None:
            cov = cov * self.coverage(sd_polygon(P, clip))
        self.put(sl, cov, colour, alpha)

    def disc(self, centre, radius, colour, alpha=1.0, clip=None, squash=1.0):
        c = np.asarray(centre, float)
        g = self.grid(c - radius - 1.0, c + radius + 1.0)
        if g is None:
            return
        P, sl = g
        d = P - c
        d[..., 1] /= squash
        cov = self.coverage(np.linalg.norm(d, axis=-1) - radius)
        if clip is not None:
            cov = cov * self.coverage(sd_polygon(P, clip))
        self.put(sl, cov, colour, alpha)

    def image(self, empty=SKIN):
        """RGBA8 at n x n: the samples box-filtered, un-premultiplied; empty texels the skin's colour"""
        s = self.ss
        rgb = self.rgb.reshape(self.n, s, self.n, s, 3).mean((1, 3))
        a = self.a.reshape(self.n, s, self.n, s).mean((1, 3))
        out = np.zeros((self.n, self.n, 4))
        sk = np.asarray(empty, float) / 255.0
        safe = np.maximum(a, 1e-6)[..., None]
        # (a part's edge: its own colour, so that the blend over the skin is the coverage alone)
        out[..., :3] = np.where(a[..., None] > 1e-4, rgb / safe, sk)
        out[..., 3] = a
        return np.clip(np.round(out * 255), 0, 255).astype(np.uint8)


def sd_polygon(P, poly):
    """signed distance from points P (..., 2) to a closed polygon (< 0 inside; even-odd)"""
    a = poly
    b = np.roll(poly, -1, axis=0)
    x, y = P[..., 0:1], P[..., 1:2]
    ax, ay, bx, by = a[:, 0], a[:, 1], b[:, 0], b[:, 1]
    ex, ey = bx - ax, by - ay
    wx, wy = x - ax, y - ay
    t = np.clip((wx * ex + wy * ey) / np.maximum(ex * ex + ey * ey, 1e-12), 0.0, 1.0)
    dx, dy = wx - ex * t, wy - ey * t
    d = np.sqrt((dx * dx + dy * dy).min(-1))
    cross = ((ay <= y) & (by > y)) | ((by <= y) & (ay > y))
    xi = ax + (y - ay) * ex / np.where(np.abs(ey) > 1e-12, ey, 1e-12)
    inside = (cross & (x < xi)).sum(-1) % 2 == 1
    return np.where(inside, -d, d)


def sd_stroke(P, pts, half):
    """distance from P to a polyline less its half-width there (half: one a point)"""
    a, b = pts[:-1], pts[1:]
    ha, hb = half[:-1], half[1:]
    x, y = P[..., 0:1], P[..., 1:2]
    ex, ey = b[:, 0] - a[:, 0], b[:, 1] - a[:, 1]
    wx, wy = x - a[:, 0], y - a[:, 1]
    t = np.clip((wx * ex + wy * ey) / np.maximum(ex * ex + ey * ey, 1e-12), 0.0, 1.0)
    dx, dy = wx - ex * t, wy - ey * t
    return (np.sqrt(dx * dx + dy * dy) - (ha + (hb - ha) * t)).min(-1)


# ------------------------------------------------------------------------------------- the drawing
def mirror(p, fit=FIT):
    """his left side's from his right's (concept pixels, mirrored about the face's centre line)"""
    p = np.array(p, float)
    p[..., 0] = 2 * fit["cx"] - p[..., 0]
    return p


def side_of(p, side, fit, dy=0.0):
    p = np.array(p, float)
    p[..., 1] += dy
    return p if side < 0 else mirror(p, fit)


def eye_space(p, fit):
    """the eye's shapes stretched by eye_sy about the iris's centre (the eye drawn in-game came out a
    little flatter than the concept's: its white 0.47 as tall as wide, the concept's 0.53)"""
    p = np.array(p, float)
    p[..., 1] = EYE["iris"][1] + (p[..., 1] - EYE["iris"][1]) * fit.get("eye_sy", 1.0)
    return p


# His right eye (the viewer's left), in the concept's pixels: the outer corner at the left. The lid
# line: its upper edge from the wing's point over the eye to the inner end, back along its lower edge
# (the white's top); the short line under the outer corner; the white between the lid and a gentle
# lower curve; the iris (its top under the lid), the pupil.
EYE = dict(
    lid_top=[(365.5, 400.5), (372.0, 392.0), (381.0, 384.5), (393.0, 378.8), (410.0, 375.8), (428.5, 376.6),
             (446.0, 381.5), (458.0, 389.5), (465.0, 398.5), (467.0, 406.5)],
    lid_bottom=[(461.0, 410.5), (459.5, 404.0), (455.0, 396.5), (445.5, 390.0), (429.0, 385.8),
                (410.5, 386.0), (393.5, 389.5), (381.5, 395.5), (373.0, 403.0)],
    under=[(366.0, 401.5), (372.0, 409.5), (379.0, 416.5), (386.0, 421.5), (390.5, 423.5)],
    under_w=[2.2, 5.4, 5.6, 4.4, 1.6],
    white_bottom=[(374.0, 404.5), (384.0, 416.0), (398.0, 424.5), (415.0, 428.8), (432.0, 428.5),
                  (446.0, 423.5), (456.0, 416.5), (461.0, 410.0)],
    iris=(423.0, 407.0, 22.6), pupil=(423.0, 406.5, 9.6), catch=(431.5, 398.0, 3.6),
)
# his right brow: a gently arched band, its upper and lower edges from the thin, rounded outer end
# to the blunt inner one (cut square to the band)
BROW_TOP = [(367.0, 345.0), (380.0, 341.6), (396.0, 340.0), (413.0, 340.8), (430.0, 343.6), (446.0, 348.2),
            (458.0, 353.6), (466.5, 358.2)]
BROW_BOTTOM = [(461.5, 368.0), (453.5, 363.0), (442.0, 357.8), (427.0, 353.6), (411.0, 351.0), (395.0, 350.4),
               (380.0, 350.6), (369.5, 349.2)]
# the mouth: a thin closed smile, a little fuller in the middle
MOUTH = [(460.0, 514.5), (470.0, 518.0), (485.0, 520.0), (500.0, 521.0), (515.0, 520.5), (530.0, 518.5),
         (542.0, 515.0)]
MOUTH_W = [5.6, 6.6, 7.0, 7.0, 7.2, 6.6, 5.6]


def widths(w, n):
    return np.interp(np.linspace(0, len(w) - 1, n), np.arange(len(w)), w)


def draw_eye(c, side, fit, closed=False, catchlight=False):
    """side -1 his right (the viewer's left), +1 his left"""
    top = eye_space(smooth_curve(EYE["lid_top"], 60), fit)
    if closed:
        # shut: the lid's upper curve brought down to the eye's middle, flatter, as one tapering line
        y0 = top[:, 1].max()
        top = top.copy()
        top[:, 1] = 409.0 + (top[:, 1] - y0) * 0.36
        w = np.interp(np.linspace(0, 1, len(top)), [0, 0.15, 0.6, 1.0], [2.0, 6.0, 7.0, 5.0])
        c.stroke(side_of(top, side, fit), w, INK)
        return
    bottom = eye_space(smooth_curve(EYE["lid_bottom"], 48), fit)
    lid = side_of(np.concatenate([top, bottom]), side, fit)
    under = eye_space(smooth_curve(EYE["under"], 24), fit)
    white = side_of(np.concatenate([bottom[::-1], eye_space(smooth_curve(EYE["white_bottom"], 40), fit)[::-1]]),
                    side, fit)
    ic, ir = side_of(EYE["iris"][:2], side, fit), EYE["iris"][2] * fit.get("eye_sy", 1.0) ** 0.5
    pc, pr = side_of(EYE["pupil"][:2], side, fit), EYE["pupil"][2]
    c.fill(white, WHITE)
    c.disc(ic, ir, IRIS, clip=white)
    # the iris: flat, a thin light rim toward the outer corner (the concept's)
    a0, a1 = (115, 245) if side < 0 else (-65, 65)
    c.stroke(arc(ic, ir * 0.94, a0, a1), 2.2, IRIS_RIM, alpha=0.75, clip=white)
    c.disc(pc, pr, PUPIL, clip=white)
    if catchlight:
        cc = side_of(EYE["catch"][:2], side, fit) if side < 0 else side_of(EYE["catch"][:2], -1, fit) + \
            np.array([0.0, 0.0])
        c.disc(cc, EYE["catch"][2], CATCH, alpha=0.95, clip=white)
    c.fill(lid, INK)
    c.stroke(side_of(under, side, fit), widths(EYE["under_w"], len(under)), INK)


def arc(centre, r, a0, a1, n=32):
    a = np.radians(np.linspace(a0, a1, n))
    return np.stack([centre[0] + r * np.cos(a), centre[1] + r * np.sin(a)], -1)


def draw_brow(c, side, fit):
    top, bottom = smooth_curve(BROW_TOP, 60), smooth_curve(BROW_BOTTOM, 60)
    # thicker about the middle line (the in-game brows read thinner than the concept's)
    mid = (top + bottom[::-1]) / 2
    t = fit.get("brow_thick", 1.0)
    top, bottom = mid + (top - mid) * t, mid[::-1] + (bottom - mid[::-1]) * t
    # the outer end rounded: a half disc's points between the two edges' ends
    a, b = bottom[-1], top[0]
    centre, r = (a + b) / 2, np.linalg.norm(b - a) / 2
    ang = math.degrees(math.atan2(*(a - centre)[::-1]))
    cap = arc(centre, r, ang, ang + 180, 12)[1:-1]
    poly = np.concatenate([top, bottom, cap])
    c.fill(side_of(poly, side, fit, fit["brow_dy"]), BROW)


def draw_mouth(c, fit):
    pts = smooth_curve(MOUTH, 60)
    pts[:, 1] += fit["mouth_dy"]
    c.stroke(pts, widths(MOUTH_W, len(pts)), INK)


def face_image(closed=False, catchlight=False, fit=FIT):
    c = Canvas(fit)
    for side in (-1, 1):
        draw_brow(c, side, fit)
        draw_eye(c, side, fit, closed=closed, catchlight=catchlight)
    draw_mouth(c, fit)
    return c.image()


# ------------------------------------------------------------------------------------- the file
def rgb5a3(rgba):
    """RGBA8 (h, w, 4) -> GX RGB5A3 bytes (4x4 tiles, big-endian): opaque texels RGB555, the others
    ARGB3444"""
    a = rgba.astype(np.int32)
    h, w, _ = a.shape
    r, g, b, al = a[..., 0], a[..., 1], a[..., 2], a[..., 3]
    opaque = al >= 0xF0
    v5 = 0x8000 | (((r * 31 + 127) // 255) << 10) | (((g * 31 + 127) // 255) << 5) | ((b * 31 + 127) // 255)
    a3 = (al * 7 + 127) // 255
    v4 = (a3 << 12) | (((r * 15 + 127) // 255) << 8) | (((g * 15 + 127) // 255) << 4) | ((b * 15 + 127) // 255)
    v = np.where(opaque, v5, v4).astype(np.uint32)
    v = v.reshape(h // 4, 4, w // 4, 4).transpose(0, 2, 1, 3).reshape(-1)
    return v.astype(">u2").tobytes()


BLINK = 5  # RFL's blink expression


def write(path, images, image_of):
    out = b"MDFC" + struct.pack("<HHB", N, N, len(images)) + bytes(image_of)
    for im in images:
        out += rgb5a3(im)
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    open(path, "wb").write(out)
    return len(out)


def images(catchlight=False):
    return [face_image(catchlight=catchlight), face_image(closed=True)], [1 if x == BLINK else 0 for x in range(7)]


def preview(ims, scale=3):
    from PIL import Image
    tiles = []
    for im in ims:
        a = im[..., 3:4] / 255.0
        comp = im[..., :3] * a + np.array(SKIN) * (1 - a)
        tiles.append(np.clip(comp, 0, 255).astype(np.uint8))
    out = Image.fromarray(np.concatenate(tiles, 1))
    return out.resize((out.width * scale, out.height * scale), Image.LANCZOS)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("data", help="the port's data directory (the face goes to DATA/wuhu/midoha_face.bin), "
                    "or 'preview'")
    ap.add_argument("out", nargs="?", help="preview: the PNG")
    ap.add_argument("--catchlight", action="store_true", help="a small white catch-light in each iris")
    ap.add_argument("--png", help="also the images, enlarged on his skin")
    a = ap.parse_args()
    ims, image_of = images(a.catchlight)
    if a.data == "preview":
        preview(ims).save(a.out)
        return
    path = os.path.join(a.data, "wuhu", "midoha_face.bin")
    n = write(path, ims, image_of)
    print("%s: %d bytes (%d images %dx%d)" % (path, n, len(ims), N, N))
    if a.png:
        preview(ims).save(a.png)


if __name__ == "__main__":
    sys.exit(main())
