#!/usr/bin/env python3
"""ミドハ's head pieces: his whole hair (sculpted), his pointed ears, the hair band at his ponytail's
base and the cord of three jade beads hanging from its tip, as low-poly meshes with their shading
baked into vertex colours (unlit, as a Mii is), written as a native g3d package.

    tools/mii/midoha_parts.py DATA [--band A|B|C] [--obj OUT.obj]
        -> DATA/wuhu/midoha.ng3d (and DATA/wuhu/midoha_outfit.bin: tools/mii/midoha_outfit.py)

All of it is ours (the user's concept, our geometry): nothing of Nintendo's is read or written.

Space: RFL's head units, the space RFL draws a Mii's head in: origin at the chin, +y up, +z toward
the face, +x the Mii's left (the viewer's right in a front view). engine/wuhu/midoha.cpp draws the
package with the matrix RFL draws his face with (his body's Head_Only node, 0.69 of these units a
Wii no Ma unit), so the pieces sit on his head wherever it turns.

The head they fit: his Mii is bald (RFL hair 30: the skin-coloured skull alone), face 0. The skull,
measured once from RFL's shapes and fitted here as a superellipsoid about C = (0, 37, -5.5),
|x/26|^2.4 + |y/30|^2.4 + |z/a|^2.4 = 1 (a = 34 in front, 28 behind), lies inside the fit by at
most 0.6 unit over the scalp; the hair shell is the fit pushed out by at least 1.6, so no skin
shows through it. (tools/mii/midoha_check.py, run where RFL_Res.dat is at hand, measures the
clearance from every side and renders the pieces on the real head; it writes nothing here.)

The pieces, after the concept:
  * the hair: a shell over the skull from a jagged hairline (a widow's peak at the centre) round
    the temples, above the ears and down to the nape, full over the forehead and the temples and
    pulled tight at the sides and the back, its strands drawn as streaks that run up to the
    ponytail's base; the side locks framing the face (the Mii's right one down to the jaw, the
    left one shorter, as drawn), two flared strands over the ears; the high ponytail rising from
    the crown and sweeping over to his left in a curve that ends in a point;
  * the ears: blades with a nearly level upper edge and a steep lower one, swept back, the roots
    inside the hair at the temples, the tips out at (+-42, 40);
  * the hair band where the ponytail leaves the crown (B: deep red with gold edges; A: dark with
    a small jade bead; C: the bead cord wound round it);
  * the cord: bead 1 caps the ponytail's point, beads 2 and 3 hang below it behind the ear's tip,
    then a knot and a tassel of hair ending at jaw height.
Colours (the concept's, sampled): hair #291E29 with #4E394B highlights and #161018 in the shade,
skin #FFBC80 (RFL's skin tone 1, his face's: the concept's #FCC697 is a tone between), jade
#3A7A60 / #83BCA6 / #296048, band red #A8322D, gold #D6A84A.

The package: one model ("midoha_head", one node), one material (the vertex colour, unlit, depth
tested and written, both faces drawn: some thin pieces are seen from behind), one vertex buffer
(position, normal, colour), one instance (the head's matrix goes on it every frame), no
environment (k = 1: the package's units are the guest's).
"""
import argparse
import math
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "g3d"))
import ng3d  # noqa: E402

sys.path.insert(0, HERE)
import midoha_outfit  # noqa: E402

# ----------------------------------------------------------------------------------------- colours
COLOURS = {
    "hair": "#2B202D",       # the shell's base (concept #291E29, a shade lifted: it is lit below)
    "hair_hi": "#4E394B",    # strand highlights (concept)
    "hair_dark": "#161018",  # the underside, the hairline's lip, the locks' backs (concept)
    "skin": "#FFBC80",       # RFL skin tone 1
    "skin_in": "#D9925E",    # the ear's inner bowl (concept #CC9266 on #FCC697)
    "jade": "#3A7A60", "jade_hi": "#83BCA6", "jade_lo": "#296048",
    "cord": "#2A1E2A",
    "band_red": "#A8322D", "gold": "#D6A84A", "band_dark": "#1E1820",
}


def rgb(name):
    h = COLOURS[name].lstrip("#")
    return np.array([int(h[i:i + 2], 16) for i in (0, 2, 4)], float)


LIGHT = np.array([-0.35, 0.8, 0.5]) / np.linalg.norm([-0.35, 0.8, 0.5])  # from above, the front, his right


def shade(n, lo=0.78, hi=1.22):
    """baked light: the colour times this (lo facing away from the light, hi facing it)"""
    return lo + (hi - lo) * max(0.0, float(np.dot(n, LIGHT)))


class Mesh:
    """Triangles with a colour per corner (vertices are split per face: flat, hard-edged, as the
    package's vertex buffer takes them)."""

    def __init__(self, name):
        self.name = name
        self.P, self.C = [], []  # corners: positions, colours (3 a triangle)

    def tri(self, a, b, c, ca, cb=None, cc=None):
        cb = ca if cb is None else cb
        cc = ca if cc is None else cc
        self.P += [np.asarray(a, float), np.asarray(b, float), np.asarray(c, float)]
        self.C += [np.asarray(ca, float), np.asarray(cb, float), np.asarray(cc, float)]

    def quad(self, a, b, c, d, ca, cb=None, cc=None, cd=None):
        cb = ca if cb is None else cb
        cc = ca if cc is None else cc
        cd = ca if cd is None else cd
        self.tri(a, b, c, ca, cb, cc)
        self.tri(a, c, d, ca, cc, cd)

    def extend(self, other):
        self.P += other.P
        self.C += other.C

    def triangles(self):
        return len(self.P) // 3


def normal(a, b, c):
    n = np.cross(np.asarray(b) - a, np.asarray(c) - a)
    ln = np.linalg.norm(n)
    return n / ln if ln > 1e-12 else np.array([0.0, 1.0, 0.0])


def smooth(x):
    x = min(max(x, 0.0), 1.0)
    return x * x * (3 - 2 * x)


def catmull(points, per_seg):
    P = [np.asarray(p, float) for p in points]
    P = [2 * P[0] - P[1]] + P + [2 * P[-1] - P[-2]]
    out = []
    for i in range(1, len(P) - 2):
        p0, p1, p2, p3 = P[i - 1], P[i], P[i + 1], P[i + 2]
        for k in range(per_seg):
            t = k / per_seg
            out.append(0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t
                              + (-p0 + 3 * p1 - 3 * p2 + p3) * t ** 3))
    out.append(P[-2])
    return np.array(out)


def frames(spine, up=(0, 0, 1)):
    """tangents and two normals along a polyline (parallel transport from `up`)"""
    T = np.gradient(spine, axis=0)
    T /= np.linalg.norm(T, axis=1, keepdims=True)
    n = np.cross(T[0], up)
    n /= np.linalg.norm(n)
    N = [n]
    for i in range(1, len(spine)):
        v = N[-1] - T[i] * (N[-1] @ T[i])
        N.append(v / np.linalg.norm(v))
    N = np.array(N)
    return T, N, np.cross(T, N)


def tube(mesh, spine, radii, sides, colour_at, squash=1.0, up=(0, 0, 1), cap_start=True, tip=None, twist=0.0):
    """a ring a spine point; colour_at(i, k, outward normal) gives a corner's colour; closed at the
    start with a fan, at the end with a point (tip) if given"""
    T, N, B = frames(spine, up)
    rings, outs = [], []
    for i, (p, r) in enumerate(zip(spine, radii)):
        ring, out = [], []
        for k in range(sides):
            a = 2 * math.pi * k / sides + twist
            o = math.cos(a) * N[i] + squash * math.sin(a) * B[i]
            ring.append(p + o * r)
            out.append(o / np.linalg.norm(o))
        rings.append(ring)
        outs.append(out)
    for i in range(len(rings) - 1):
        for k in range(sides):
            k2 = (k + 1) % sides
            a, b, c, d = rings[i][k], rings[i][k2], rings[i + 1][k2], rings[i + 1][k]
            n = normal(a, d, c)
            mesh.quad(a, d, c, b, colour_at(i, k, outs[i][k], n), colour_at(i + 1, k, outs[i + 1][k], n),
                      colour_at(i + 1, k2, outs[i + 1][k2], n), colour_at(i, k2, outs[i][k2], n))
    if cap_start:
        c0 = spine[0]
        for k in range(sides):
            k2 = (k + 1) % sides
            n = normal(c0, rings[0][k], rings[0][k2])
            col = colour_at(0, k, -T[0], n)
            mesh.tri(c0, rings[0][k2], rings[0][k], col)
    if tip is not None:
        i = len(rings) - 1
        for k in range(sides):
            k2 = (k + 1) % sides
            n = normal(rings[i][k], rings[i][k2], tip)
            mesh.tri(rings[i][k], rings[i][k2], tip, colour_at(i, k, outs[i][k], n),
                     colour_at(i, k2, outs[i][k2], n), colour_at(i, k, T[i], n))
    return rings


def sphere(mesh, c, r, seg, rows, base, hi=None, lo=None):
    c = np.asarray(c, float)
    hi_dir = np.array([-0.45, 0.6, 0.65]) / np.linalg.norm([-0.45, 0.6, 0.65])
    lo_dir = np.array([0.5, -0.6, -0.6]) / np.linalg.norm([0.5, -0.6, -0.6])

    def at(i, k):
        th = math.pi * i / rows
        ph = 2 * math.pi * k / seg
        return np.array([math.sin(th) * math.cos(ph), math.cos(th), math.sin(th) * math.sin(ph)])

    def col(n):
        if hi is not None and n @ hi_dir > 0.78:
            return rgb(hi)
        if lo is not None and n @ lo_dir > 0.5:
            return rgb(lo)
        return rgb(base) * shade(n, 0.85, 1.12)
    for i in range(rows):
        for k in range(seg):
            n00, n01, n11, n10 = at(i, k), at(i, k + 1), at(i + 1, k + 1), at(i + 1, k)
            mid = n00 + n01 + n11 + n10
            fc = col(mid / np.linalg.norm(mid))
            if i == 0:
                mesh.tri(c + n00 * r, c + n11 * r, c + n10 * r, fc)
            elif i == rows - 1:
                mesh.tri(c + n00 * r, c + n01 * r, c + n10 * r, fc)
            else:
                mesh.quad(c + n00 * r, c + n01 * r, c + n11 * r, c + n10 * r, fc)


# ----------------------------------------------------------------------------------------- the skull
C = np.array([0.0, 37.0, -5.5])     # the fit's centre
SKULL = dict(ax=26.0, ay=30.0, az_front=34.0, az_back=28.0, p=2.4)


def direction(theta, phi):
    """theta from straight up, phi round from the front (+z) toward his left (+x); degrees"""
    t, p = math.radians(theta), math.radians(phi)
    return np.array([math.sin(t) * math.sin(p), math.cos(t), math.sin(t) * math.cos(p)])


def skull_radius(d):
    s = SKULL
    az = s["az_front"] if d[2] > 0 else s["az_back"]
    v = (abs(d[0]) / s["ax"]) ** s["p"] + (abs(d[1]) / s["ay"]) ** s["p"] + (abs(d[2]) / az) ** s["p"]
    return v ** (-1.0 / s["p"])


def interp(points, x):
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    return float(np.interp(x, xs, ys))


# ----------------------------------------------------------------------------------------- the hair
# the hairline: the height the shell comes down to, by phi (symmetric; the jagged front is added)
HAIRLINE = [(0, 54.5), (12, 57.0), (24, 58.0), (34, 55.5), (44, 49.0), (56, 42.0), (70, 37.5), (86, 35.5),
            (100, 27.0), (115, 20.0), (135, 15.0), (160, 12.0), (180, 11.5)]
N_PHI, N_THETA = 40, 13
# where the strands run to: the ponytail's base, at the top of the back of the head
TAIL_THETA, TAIL_PHI = 24.0, 180.0


def thickness(theta, phi, theta_edge=None):
    """how far the hair stands off the skull's fit: full over the forehead and the temples, tight at
    the sides behind and at the back; at the sides and the back it comes down to the scalp toward the
    hairline (theta_edge: the column's), at the front it stays full to the edge (the fringe's lip)"""
    a = abs(phi)
    front = max(0.0, math.cos(math.radians(min(a, 100.0) * 0.9))) ** 1.1
    top = math.exp(-((theta - 26.0) / 32.0) ** 2) * front
    side = math.exp(-((theta - 56.0) / 26.0) ** 2) * math.exp(-((a - 55.0) / 34.0) ** 2)
    crown = math.exp(-((theta - 15.0) / 25.0) ** 2)
    upper = math.exp(-((theta - 42.0) / 18.0) ** 2) * math.exp(-((a - 70.0) / 40.0) ** 2)
    t = 1.8 + 12.5 * top + 11.0 * side + 4.0 * upper + 3.0 * crown
    # pulled tight behind the ears and at the nape
    back = smooth((a - 95.0) / 50.0)
    t *= 1.0 - 0.35 * back
    if theta_edge is not None:
        # toward the hairline the hair lies on the scalp: at the sides and the back nearly flat, at
        # the front it rises from the forehead (swept up and back) without overhanging it
        lay = 0.6 + 0.2 * smooth((a - 35.0) / 35.0)
        near = smooth((theta - (theta_edge - 34.0)) / 34.0)
        t = t * (1.0 - lay * near)
        t = max(t, 2.2 if a < 40.0 else 1.7)
    return t


def strand_coord(d):
    """the angle round the axis through the ponytail's base: the strands run along it (degrees)"""
    a = direction(TAIL_THETA, TAIL_PHI)
    u = np.cross(a, [0.0, 0.0, 1.0])
    u /= np.linalg.norm(u)
    v = np.cross(a, u)
    q = d - a * (d @ a)
    return math.degrees(math.atan2(q @ v, q @ u))


def hair_colour(p, n, d, theta, phi):
    base = rgb("hair") * shade(n)
    # highlights: streaks along the strands over the top and the front (the concept's purple sheen)
    s = strand_coord(d)
    band = 0.5 + 0.5 * math.cos(math.radians(s * 11.0))
    region = math.exp(-((theta - 36.0) / 30.0) ** 2) * max(0.0, math.cos(math.radians(abs(phi) * 0.7)))
    w = 0.85 * smooth((band - 0.62) / 0.25) * region
    col = base * (1 - w) + rgb("hair_hi") * w
    # deeper toward the hairline's edge and underneath
    under = smooth((theta - 80.0) / 40.0)
    return col * (1 - 0.25 * under)


def hairline_y(phi, j):
    """the shell's lowest height in column j: the jagged front (every other column a point; the
    widow's peak at the middle)"""
    a = abs(phi)
    y = interp(HAIRLINE, a)
    if a < 42.0:
        if j % 2 == 0:
            y -= 2.6 * (1.0 - a / 60.0)
        else:
            y += 0.6
        if a < 1.0:
            y = 50.5  # the widow's peak
    return y


def theta_at_height(phi, y, t_of):
    """the theta at which the shell (skull fit + thickness) comes down to height y"""
    lo, hi = 0.0, 175.0
    for _ in range(40):
        mid = 0.5 * (lo + hi)
        d = direction(mid, phi)
        p = C + d * (skull_radius(d) + t_of(mid, phi))
        if p[1] > y:
            lo = mid
        else:
            hi = mid
    return 0.5 * (lo + hi)


def hair_shell():
    m = Mesh("hair")
    phis = [-180.0 + 360.0 * j / N_PHI for j in range(N_PHI)]
    cols = []
    for j, phi in enumerate(phis):
        y_edge = hairline_y(phi, j)
        th0 = theta_at_height(phi, y_edge, thickness)
        t_of = lambda th, ph: thickness(th, ph, th0)  # noqa: E731
        th_edge = theta_at_height(phi, y_edge, t_of)
        col = []
        for i in range(N_THETA + 1):
            # rows closer together toward the edge (the hairline's curve)
            f = i / N_THETA
            th = th_edge * (1 - (1 - f) ** 1.25) if i else 0.0
            d = direction(th, phi)
            col.append((th, C + d * (skull_radius(d) + t_of(th, phi)), d))
        cols.append(col)
    top = cols[0][0][1]
    n = len(phis)
    for j in range(n):
        j2 = (j + 1) % n
        a, b = cols[j], cols[j2]
        # the top: a fan round the pole
        p1, p2 = a[1][1], b[1][1]
        nn = normal(top, p2, p1)
        m.tri(top, p2, p1, hair_colour(top, nn, a[0][2], 0, phis[j]))
        for i in range(1, N_THETA):
            q00, q01, q11, q10 = a[i][1], b[i][1], b[i + 1][1], a[i + 1][1]
            nn = normal(q00, q01, q11)
            cs = [hair_colour(q, nn, dd, th, ph) for (q, dd, th, ph) in
                  ((q00, a[i][2], a[i][0], phis[j]), (q01, b[i][2], b[i][0], phis[j2]),
                   (q11, b[i + 1][2], b[i + 1][0], phis[j2]), (q10, a[i + 1][2], a[i + 1][0], phis[j]))]
            m.quad(q00, q01, q11, q10, cs[0], cs[1], cs[2], cs[3])
        # the lip under the edge: from the shell's edge in to the scalp, a little higher
        e0, e1 = a[N_THETA], b[N_THETA]
        in0 = C + e0[2] * (skull_radius(e0[2]) + 0.4) + np.array([0, 1.2, 0])
        in1 = C + e1[2] * (skull_radius(e1[2]) + 0.4) + np.array([0, 1.2, 0])
        dark = rgb("hair_dark")
        m.quad(e0[1], e1[1], in1, in0, dark * 1.15, dark * 1.15, dark, dark)
    return m


def lock(name, spine, widths, depth, z_out=(0.0, 0.0, 1.0), sides=6, front_col="hair", back_col="hair_dark",
         highlight=True, per_seg=3):
    """a lock of hair: a flattened tube along `spine` (its width across, `depth` thick), tapering to
    its last point; lit like the shell, a highlight down its front, darker behind"""
    m = Mesh(name)
    spine = np.asarray(spine, float)
    sp = catmull(spine, per_seg)
    w = np.interp(np.linspace(0, len(widths) - 1, len(sp)), np.arange(len(widths)), widths)
    tip = sp[-1]
    body = sp[:-1]
    out = np.asarray(z_out, float)

    def colour_at(i, k, o, n):
        facing = o @ out
        if facing < -0.2:
            return rgb(back_col) * shade(n, 0.9, 1.1)
        c = rgb(front_col) * shade(n)
        if highlight and facing > 0.75 and 1 <= i <= len(body) - 3:
            c = c * 0.45 + rgb("hair_hi") * 0.55
        return c
    tube(m, body, [x * 0.5 for x in w[:-1]], sides, colour_at, squash=depth / max(w.max(), 1e-6) * 1.0,
         up=out, tip=tip)
    return m


def side_locks():
    """the locks that frame the face, the strands flared over the ears, as drawn (the Mii's right,
    -x, longer)"""
    m = Mesh("locks")
    # the Mii's right (the viewer's left): the long lock down past the jaw
    m.extend(lock("lock_r", [(-10.0, 65.0, 18.5), (-18.5, 58.0, 17.0), (-23.0, 47.0, 14.5), (-25.0, 34.0, 12.5),
                             (-25.0, 20.0, 10.5), (-24.0, 7.5, 9.0)],
                  [12.0, 13.0, 10.5, 8.0, 5.0, 1.5], 3.0, z_out=(-0.35, 0.0, 1.0)))
    # the Mii's left (the viewer's right): shorter, curling in toward the cheek
    m.extend(lock("lock_l", [(10.0, 65.0, 18.5), (18.0, 58.0, 17.0), (22.5, 47.0, 14.5), (24.0, 34.0, 12.5),
                             (22.5, 19.0, 11.0)],
                  [11.5, 12.5, 10.0, 6.0, 1.5], 3.0, z_out=(0.35, 0.0, 1.0)))
    # the strands flared out over the ears (the concept's pointed tips above the ears)
    m.extend(lock("flare_r", [(-24.0, 66.0, 6.0), (-32.0, 57.0, 5.0), (-37.5, 46.0, 3.5), (-39.5, 37.0, 2.5)],
                  [10.0, 9.5, 5.5, 1.2], 2.6, z_out=(-0.9, 0.2, 0.4)))
    m.extend(lock("flare_l", [(24.0, 66.0, 6.0), (31.5, 57.0, 5.0), (35.0, 46.0, 3.8), (36.0, 37.0, 3.0)],
                  [10.0, 9.0, 5.0, 1.2], 2.6, z_out=(0.9, 0.2, 0.4)))
    # a second, inner strand on the right side, between the long lock and the flare
    m.extend(lock("lock_r2", [(-20.0, 62.0, 10.5), (-27.5, 50.0, 8.5), (-30.0, 36.0, 6.5), (-29.0, 24.0, 5.5)],
                  [6.5, 6.5, 4.5, 1.2], 2.2, z_out=(-0.6, 0.0, 0.8)))
    m.extend(lock("lock_l2", [(20.0, 62.0, 10.5), (27.0, 50.0, 8.5), (29.0, 37.0, 6.5), (28.0, 27.0, 5.5)],
                  [6.0, 6.0, 4.0, 1.2], 2.2, z_out=(0.6, 0.0, 0.8)))
    return m


# swept-back clumps over the top: from the hairline up and back toward the tie, lying on the shell
CLUMPS = [(-35.0, 6.0), (-21.0, 7.0), (-7.0, 6.5), (7.0, 6.5), (21.0, 7.0), (35.0, 6.0)]  # (phi, width)


def clumps():
    m = Mesh("clumps")
    tie = direction(*TAIL["tie"])
    for j, (phi, width) in enumerate(CLUMPS):
        th0 = theta_at_height(phi, hairline_y(phi, 0) + 1.5, thickness)
        a = direction(th0, phi)
        pts = []
        for k in range(5):
            f = 0.5 * k / 4.0
            d = a * (1 - f) + tie * f
            d /= np.linalg.norm(d)
            th = math.degrees(math.acos(max(-1.0, min(1.0, d[1]))))
            ph = math.degrees(math.atan2(d[0], d[2]))
            # (on the shell; the last point sinks into it: the clump runs into the hair)
            lift = (0.4, 0.8, 0.7, 0.3, -1.2)[k]
            pts.append(C + d * (skull_radius(d) + thickness(th, ph) + lift))
        out = (a + tie) / np.linalg.norm(a + tie)
        m.extend(lock("clump%d" % j, pts, [width, width * 0.95, width * 0.75, width * 0.5, 1.0], 1.6,
                      z_out=out, sides=4, per_seg=2))
    return m


# the ponytail: tied on the crown (a little toward his right, toward the back), up over the top and
# over to his left in a curve, down to a point (the concept's swoosh, its peak 22 over his brows)
TAIL = dict(
    tie=(21.0, -145.0),  # (theta, phi) of the tie on the hair's surface
    arc=[(-10.5, 81.0, -15.5), (-5.0, 87.0, -16.5), (5.0, 90.5, -17.0), (16.0, 91.0, -16.5), (26.5, 88.0, -15.0),
         (34.0, 82.0, -14.0), (39.0, 76.0, -13.5)],
    radii=[5.0, 4.8, 6.4, 7.0, 6.8, 6.3, 5.3, 3.6, 1.4],
    sides=8, squash=0.82, band_at=1,
)


def tail_spine():
    """the tie's point inside the head, just outside the hair (the band), then the arc"""
    d = direction(*TAIL["tie"])
    r = skull_radius(d)
    surface = C + d * (r + thickness(*TAIL["tie"]))
    return [C + d * (r - 1.0), surface + d * 0.8] + [np.asarray(p, float) for p in TAIL["arc"]]


def ponytail():
    m = Mesh("ponytail")
    sp = catmull(tail_spine(), 3)
    rr = np.interp(np.linspace(0, len(TAIL["radii"]) - 1, len(sp)), np.arange(len(TAIL["radii"])), TAIL["radii"])
    tip = sp[-1] + (sp[-1] - sp[-2]) * 1.4
    n_body = len(sp) - 1

    def colour_at(i, k, o, n):
        c = rgb("hair") * shade(n)
        # a sheen along its top, from past the band to near the point (the concept's swoosh)
        up = o @ np.array([0.0, 0.85, 0.53])
        if 4 <= i <= n_body - 3 and up > 0.55:
            c = c * 0.4 + rgb("hair_hi") * 0.6
        if o[1] < -0.55:
            c = rgb("hair_dark") * shade(n, 0.9, 1.1)
        return c
    tube(m, sp[:-1], rr[:-1], TAIL["sides"], colour_at, squash=TAIL["squash"], up=(0, 0, 1), tip=tip)
    return m, sp, rr


def band(option, sp, rr):
    """the hair band round the ponytail where it leaves the crown"""
    m = Mesh("band_" + option)
    i = TAIL["band_at"] * 3
    c = sp[i]
    t = sp[i + 1] - sp[i - 1]
    t /= np.linalg.norm(t)
    r = rr[i]

    def ring(centre, radius, width, colour, sides=10):
        seg = np.array([centre - t * width / 2, centre + t * width / 2])
        tube(m, seg, [radius, radius], sides, lambda i, k, o, n: rgb(colour) * shade(n, 0.85, 1.2),
             squash=TAIL["squash"], tip=None, cap_start=True)
    if option == "A":
        ring(c, r + 0.8, 3.6, "band_dark")
        fwd = np.array([0.0, 0.0, 1.0]) - t * t[2]
        fwd /= np.linalg.norm(fwd)
        sphere(m, c + fwd * (r + 2.0), 1.8, 6, 4, "jade", "jade_hi", "jade_lo")
    elif option == "B":
        ring(c, r + 0.8, 3.6, "band_red")
        ring(c - t * 1.95, r + 1.05, 0.7, "gold")
        ring(c + t * 1.95, r + 1.05, 0.7, "gold")
    else:
        T_, N_, B_ = frames(np.array([c - t, c, c + t]))
        pts = [c + t * (-1.8 + 3.6 * k / 30) + (math.cos(2 * math.pi * 2.5 * k / 30) * N_[1] +
                                                 math.sin(2 * math.pi * 2.5 * k / 30) * B_[1]) * (r + 0.9)
               for k in range(31)]
        tube(m, np.array(pts), [0.85] * 31, 5, lambda i, k, o, n: rgb("cord") * shade(n), tip=None)
    return m


# bead 1 caps the ponytail's point; 2 and 3 hang behind the ear's tip, as drawn
CORD = dict(beads=[(41.5, 73.0, -13.5), (50.5, 57.0, -15.5), (54.5, 39.0, -17.0)], bead_r=5.0, cord_r=1.0,
            knot=(54.0, 31.5, -17.2), tassel=[(54.0, 30.0, -17.2), (54.5, 25.0, -17.4), (54.0, 18.0, -17.4),
                                               (52.8, 12.0, -17.3), (51.5, 8.0, -17.2)],
            tassel_w=[3.0, 6.4, 6.8, 4.5, 1.0])


def cord(option):
    m = Mesh("cord")
    b = [np.asarray(x, float) for x in CORD["beads"]]
    path = catmull([b[0], b[1], b[2], np.asarray(CORD["knot"])], 4)
    tube(m, path, [CORD["cord_r"]] * len(path), 5, lambda i, k, o, n: rgb("cord") * shade(n), tip=None,
         cap_start=False)
    for c in b:
        sphere(m, c, CORD["bead_r"], 10, 6, "jade", "jade_hi", "jade_lo")
    sphere(m, CORD["knot"], 2.0, 6, 4, "band_red" if option == "B" else "cord")
    m.extend(lock("tassel", CORD["tassel"], CORD["tassel_w"], 3.6, z_out=(0.4, 0.0, 1.0), sides=6))
    return m


# the ear: an outline in (s along the blade from the root, y), swept back, the inner bowl recessed
EAR = dict(root=(24.5, -2.5), sweep=28.0, top=[(-5.0, 36.6), (5.0, 37.8), (10.0, 38.7), (15.0, 39.7)],
           tip=(19.5, 40.6), bottom=[(16.0, 36.3), (13.0, 32.2), (10.0, 28.4), (7.0, 24.6), (4.5, 22.4),
                                     (2.0, 21.3), (-5.0, 21.2)],
           thick_root=3.0, thick_tip=0.6, bowl_inset=0.3, bowl_depth=0.9)


def ear(side):
    m = Mesh("ear")
    e = EAR
    sw = math.radians(e["sweep"])
    s_axis = np.array([math.cos(sw), 0.0, -math.sin(sw)])
    front_n = np.array([math.sin(sw), 0.0, math.cos(sw)])
    outline = e["top"] + [e["tip"]] + e["bottom"]
    s_tip = e["tip"][0]
    rx, rz = e["root"]

    def P(s, y, depth=0.0):
        return np.array([rx, 0.0, rz]) + s_axis * s + np.array([0.0, y, 0.0]) - front_n * depth

    def thick(s):
        f = min(max(s / s_tip, 0.0), 1.0)
        return e["thick_root"] * (1 - f) + e["thick_tip"] * f
    F = [P(s, y) for s, y in outline]
    Bk = [P(s, y, thick(s)) for s, y in outline]
    cs = float(np.mean([o[0] for o in outline]))
    cy = float(np.mean([o[1] for o in outline]))
    Bo = []
    for s, y in outline[1:-1]:
        bs, by = s + (cs - s) * e["bowl_inset"], y + (cy - y) * e["bowl_inset"]
        f = min(max(bs / s_tip, 0.0), 1.0)
        Bo.append(P(bs, by, e["bowl_depth"] * (1 - 0.6 * f)))
    n = len(outline)
    skin, inner = rgb("skin"), rgb("skin_in")

    def lit(c, a, b, cc):
        return c * shade(normal(a, b, cc) if side > 0 else normal(a, b, cc) * [-1, 1, 1], 0.9, 1.08)
    for i in range(len(Bo) - 1):
        a, b, c, d = F[i + 1], F[i + 2], Bo[i], Bo[i + 1]
        m.tri(a, c, b, lit(skin, a, c, b))
        m.tri(b, c, d, lit(skin, b, c, d))
    m.tri(F[0], Bo[0], F[1], lit(skin, F[0], Bo[0], F[1]))
    m.tri(F[n - 1], F[n - 2], Bo[-1], lit(skin, F[n - 1], F[n - 2], Bo[-1]))
    m.tri(F[0], F[n - 1], Bo[0], lit(skin, F[0], F[n - 1], Bo[0]))
    bc = P(cs + 1.0, cy, e["bowl_depth"] * 1.05)
    for i in range(len(Bo) - 1):
        m.tri(Bo[i], bc, Bo[i + 1], inner)
    m.tri(Bo[-1], bc, F[n - 1], inner)
    m.tri(F[n - 1], bc, Bo[0], inner)
    bb = P(cs, cy, thick(cs) + 0.4)
    for i in range(n - 1):
        m.tri(Bk[i + 1], bb, Bk[i], lit(skin, Bk[i + 1], bb, Bk[i]) * 0.94)
    m.tri(Bk[0], bb, Bk[n - 1], lit(skin, Bk[0], bb, Bk[n - 1]) * 0.94)
    for i in range(n):
        i2 = (i + 1) % n
        m.quad(F[i], F[i2], Bk[i2], Bk[i], lit(skin, F[i], F[i2], Bk[i2]) * 0.97)
    if side < 0:
        m.P = [p * np.array([-1.0, 1.0, 1.0]) for p in m.P]
        # (mirrored: the corners' order reversed, so faces keep facing out)
        for t in range(0, len(m.P), 3):
            m.P[t + 1], m.P[t + 2] = m.P[t + 2], m.P[t + 1]
            m.C[t + 1], m.C[t + 2] = m.C[t + 2], m.C[t + 1]
    return m


def build(band_option="B"):
    """{piece name: Mesh}"""
    tail, sp, rr = ponytail()
    return {
        "hair": hair_shell(),
        "clumps": clumps(),
        "locks": side_locks(),
        "ponytail": tail,
        "band": band(band_option, sp, rr),
        "cord": cord(band_option),
        "ear_l": ear(+1),
        "ear_r": ear(-1),
    }


# ----------------------------------------------------------------------------------------- the package
def material(p):
    """the vertex colour, unlit: TEV stage 0 passes the rasterised colour (channel 0: material colour
    from the vertex, no lighting); depth tested (LEQUAL) and written; no culling"""
    col_env = 15 | 15 << 4 | 15 << 8 | 15 << 12  # a = b = c = d = zero ...
    col_env = (col_env & ~0xF) | 10                # ... d = RASC
    col_env |= 1 << 19                             # clamp
    alpha_env = (7 << 13) | (7 << 10) | (7 << 7) | (5 << 4) | (1 << 19)  # d = RASA
    tref = 7 | 7 << 3 | 0 << 6 | 0 << 7            # no texture, no coordinates, colour channel 0
    maps = [ng3d.Rec("TexMap", texture=ng3d.NONE16) for _ in range(8)]
    gens = [ng3d.Rec("TexGen", matrix=ng3d.NONE16, post_mtx=ng3d.NONE16) for _ in range(8)]
    return p.add("materials", name=p.str("midoha_vertex_colour"), flags=0, n_stages=1, n_texgens=0, n_chans=1,
                 maps=maps, texgens=gens,
                 cull=0, color_env=[col_env] + [0] * 15, alpha_env=[alpha_env] + [0] * 15,
                 tref=[tref] + [0] * 7, ksel=[0] * 8,
                 alpha_compare=7 << 16 | 7 << 19,  # always, always
                 zmode=1 | 3 << 1 | 1 << 4,          # test, LEQUAL, write
                 blend=0x18,                         # no blending, colour and alpha written
                 chan_color=1, chan_alpha=1,         # material colour from the vertex, unlit
                 mat_color=[255, 255, 255, 255], amb_color=[0, 0, 0, 255])


def package(pieces):
    p = ng3d.Package()
    mat = material(p)
    vdata = bytearray()
    indices = []
    lo, hi = np.full(3, 1e9), np.full(3, -1e9)
    batches = []
    n = 0
    for name, m in pieces.items():
        first = len(indices)
        for t in range(0, len(m.P), 3):
            a, b, c = m.P[t], m.P[t + 1], m.P[t + 2]
            # (GX's front faces wind clockwise: our triangles are counter-clockwise seen from
            # outside, so they go in reversed)
            nn = normal(a, b, c)
            for q, col in ((a, m.C[t]), (c, m.C[t + 2]), (b, m.C[t + 1])):
                rgba = [int(min(255, max(0, round(x)))) for x in col] + [255]
                vdata += struct.pack("<3f", *q) + struct.pack("<3bx", *[int(round(x * 127)) for x in nn])
                vdata += bytes(rgba)
                indices.append(n)
                n += 1
                lo, hi = np.minimum(lo, q), np.maximum(hi, q)
        if n > 0xFFFF:
            raise ValueError("more than 65535 vertices")
        P = np.array(m.P)
        batches.append(dict(material=mat, vbuf=0, first_index=first, n_indices=len(indices) - first,
                            bbox=list(P.min(0)) + list(P.max(0)), node=0, flags=0))
    p.add("vertex_buffers", data=0, count=n, attrs=ng3d.VA_POS | ng3d.VA_NRM | ng3d.VA_CLR, stride=20, flags=0,
          uv_scale=[1.0, 1.0])
    p.vertex_data.append(bytes(vdata))
    p.indices = indices
    for b in batches:
        p.add("batches", **b)
    ident = [[1.0, 0, 0, 0], [0, 1.0, 0, 0], [0, 0, 1.0, 0]]
    p.add("nodes", name=p.str("midoha_head"), parent=ng3d.NONE16, flags=ng3d.NODE_VISIBLE, scale=[1.0, 1.0, 1.0],
          rotate=[0.0, 0.0, 0.0], translate=[0.0, 0.0, 0.0], bind=ident, first_batch=0, n_batches=len(batches))
    centre = (lo + hi) / 2
    p.add("models", name=p.str("midoha_head"), first_node=0, n_nodes=1, first_batch=0, n_batches=len(batches),
          bbox=list(lo) + list(hi), radius=float(np.linalg.norm(hi - centre)), lod_next=ng3d.NONE16,
          lod_distance=0.0)
    p.add("instances", model=0, flags=0, anim=ng3d.NONE16, class_id=0, mtx=ident, anim_offset=0.0,
          cull_distance=0.0)
    return p


def write_obj(pieces, path):
    out = ["# ミドハ's head pieces (tools/mii/midoha_parts.py): RFL head units, vertex colours"]
    k = 1
    for name, m in pieces.items():
        out.append("o " + name)
        for q, c in zip(m.P, m.C):
            out.append("v %.3f %.3f %.3f %.3f %.3f %.3f" % (q[0], q[1], q[2], c[0] / 255, c[1] / 255, c[2] / 255))
        for t in range(0, len(m.P), 3):
            out.append("f %d %d %d" % (k + t, k + t + 1, k + t + 2))
        k += len(m.P)
    open(path, "w").write("\n".join(out) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("data", help="the port's data directory (the package goes to DATA/wuhu/midoha.ng3d)")
    ap.add_argument("--band", default="B", choices="ABC", help="the hair band (B: red with gold edges)")
    ap.add_argument("--out", help="the package's path (default DATA/wuhu/midoha.ng3d)")
    ap.add_argument("--obj", help="also an OBJ of the pieces (vertex colours)")
    a = ap.parse_args()
    pieces = build(a.band)
    p = package(pieces)
    problems = p.check()
    if problems:
        raise SystemExit("the package doesn't check: " + "; ".join(problems[:5]))
    out = a.out or os.path.join(a.data, "wuhu", "midoha.ng3d")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    data = p.write()
    open(out, "wb").write(data)
    if a.obj:
        write_obj(pieces, a.obj)
    print("%s: %d bytes; triangles: %s" % (out, len(data),
                                          ", ".join("%s %d" % (k, m.triangles()) for k, m in pieces.items())))
    if not a.out:
        print("%s: %d bytes" % midoha_outfit.write(a.data))


if __name__ == "__main__":
    main()
