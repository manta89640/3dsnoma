"""ミドハ's head pieces, after the user's concept: his hair (the cap, the swept locks over the front, the
locks framing his face, the strands flared over his ears, the high ponytail), his pointed ears, the hair
band at the ponytail's base and the cord of three jade beads. Smooth parametric shapes
(tools/mii/midoha_shapes.py) with their shading painted into one texture; tools/mii/midoha_parts.py
writes them as a package. All of it is ours.

Space: RFL's head units (origin at the chin, +y up, +z toward the face, +x his left: the viewer's right
in a front view). The head they fit, measured once from RFL's shapes (face 4, the bald skull 30; the
numbers are kept here, the shapes are not): the skull's top at y 67.2, its back at z -33 (y 35-45), its
sides at x +-26 (y 30-45), +-24 (y 50), +-16 (y 60); the forehead at z 23.2 (y 50), 20.5 (y 55), 17.8
(y 60), 13.1 (y 63); the face's front at z 24.8, its sides at x +-25.8 (y 30), +-23.8 (y 20).

How the concept is read (its 1024 px drawing to head units: x = (px - 500) / 5.53, y = 30.46 - (py -
406) / 5.78, the scale at which its eyes, mouth and chin fall on the Mii's): the hairline straight at y
55.5 from x -16 to 14 with a small widow's peak (52.5, 8 units wide) and a few small points; at the temples it turns down into
the face-framing locks (their inner edges at x -23.5 / 21, their points at the jaw, y 8 / 6.5); a strand
flared out over each ear (point at x -39.8, y 40); the hair's outline +-26 at y 73, +-33 at y 59, +-38 at
y 45 (here a little less: +-25, 31.5, 34: the Mii's body under it); the ponytail from the crown, up to y
91 and over to his left, its point at (44, 75) capped by the first bead, the other two at (54, 57) and
(59, 37), the cord's tassel down to y 9.

The painting (the concept's three hair tones, sampled: lines #140F16, the hair #261C27, the sheen
#4E3A4A): each lock the hair's flat colour, a dark rim along its edges (the drawing's line work), one or
two dark strand lines that taper toward the root, darker underneath; one band of sheen round the head
over the crown and the front (an angle band about the ponytail's tie), its upper edge cut into a few
broad points that run along the hair. No noise, no fine stripes.
"""
import math

import numpy as np

from midoha_shapes import Piece, Spine, profile, smoothstep, unit

# ------------------------------------------------------------------------------------- colours
LINE = np.array([22.0, 16.0, 24.0])
HAIR = np.array([38.0, 28.0, 39.0])
UNDER = np.array([27.0, 20.0, 29.0])
SHEEN = np.array([78.0, 58.0, 74.0])
SHEEN_SOFT = np.array([58.0, 43.0, 56.0])
SKIN = np.array([252.0, 196.0, 150.0])
SKIN_IN = np.array([222.0, 160.0, 118.0])
SKIN_LINE = np.array([196.0, 128.0, 92.0])
JADE = np.array([52.0, 120.0, 92.0])
JADE_DARK = np.array([34.0, 86.0, 66.0])
JADE_HI = np.array([168.0, 214.0, 192.0])
CORD = np.array([40.0, 28.0, 40.0])
BAND = np.array([150.0, 40.0, 38.0])
BAND_DARK = np.array([98.0, 24.0, 26.0])
GOLD = np.array([214.0, 168.0, 74.0])

# ------------------------------------------------------------------------------------- the head
SKULL_C = np.array([0.0, 37.0, -5.5])  # the skull's fit (tools/mii/midoha_check.py measures against it)
SKULL = dict(ax=26.0, ay=30.0, az_front=34.0, az_back=28.0, p=2.4)


def skull_radius(d):
    d = np.asarray(d, float)
    az = np.where(d[..., 2] > 0, SKULL["az_front"], SKULL["az_back"])
    p = SKULL["p"]
    v = (np.abs(d[..., 0]) / SKULL["ax"]) ** p + (np.abs(d[..., 1]) / SKULL["ay"]) ** p + (np.abs(d[..., 2]) / az) ** p
    return v ** (-1.0 / p)


# ------------------------------------------------------------------------------------- the cap
# The cap: a closed shape round the skull (a superellipsoid about CAP_C, flatter on top), cut at the
# hairline; parametrised about the ponytail's tie: t from the tie (0) to the hairline (1) and on under
# it to the lip's inner edge (LIP), s round the tie. Its strand lines run along t: toward the tie.
CAP_C = np.array([0.0, 45.0, -7.5])
CAP = dict(ax=33.5, ay_up=34.0, ay_down=29.0, az_front=30.0, az_back=31.0, p_up=2.3, p_down=2.2)
TIE = np.array([-2.0, 78.0, -12.0])   # the ponytail's tie on the cap (approximately: projected there)
LIP = 0.12                             # of t: the lip under the hairline, in to the scalp


def cap_radius(d):
    d = np.asarray(d, float)
    ax = CAP["ax"]
    ay = np.where(d[..., 1] > 0, CAP["ay_up"], CAP["ay_down"])
    az = np.where(d[..., 2] > 0, CAP["az_front"], CAP["az_back"])
    p = np.where(d[..., 1] > 0, CAP["p_up"], CAP["p_down"])
    v = (np.abs(d[..., 0]) / ax) ** p + (np.abs(d[..., 1]) / ay) ** p + (np.abs(d[..., 2]) / az) ** p
    return v ** (-1.0 / p)


def cap_point(d):
    d = unit(d)
    return CAP_C + d * cap_radius(d)[..., None]


# the hairline: the height the cap comes down to, by the angle round the head from the front (degrees,
# + toward his left), symmetric; then the points of the front edge
HAIRLINE = [(0, 55.5), (26, 55.5), (31, 53.5), (36, 48.0), (42, 42.0), (55, 39.5), (80, 38.0), (105, 37.0),
            (125, 35.0), (150, 32.0), (180, 31.0)]
# (angle, depth, half width): small points of the front edge, the widow's peak the deepest
POINTS = [(0.0, 3.0, 7.5),
          # the nape's soft points
          (180.0, 4.0, 13.0), (-154.0, 3.2, 11.0), (154.0, 3.2, 11.0)]


def hairline_y(phi):
    a = np.abs(phi)
    xs, ys = zip(*HAIRLINE)
    y = np.interp(a, xs, ys)
    for ang, depth, half in POINTS:
        x = np.abs(phi - ang) / half
        y = y - depth * np.clip(1 - x, 0, 1) ** 1.5
    return y


def _tie_frame():
    a = unit(TIE - CAP_C)
    # s = 0 toward the front-down of the tie (so the seam lies at the back of the head)
    ref = unit(np.array([0.0, 0.0, 1.0]) - a * a[2])
    b = np.cross(a, ref)
    return a, ref, b


TIE_A, TIE_U, TIE_V = _tie_frame()


def cap_dir(s, beta):
    """the direction beta (radians) from the tie's axis, s round it"""
    ang = 2 * math.pi * np.asarray(s, float)
    beta = np.asarray(beta, float)
    side = np.cos(ang)[..., None] * TIE_U + np.sin(ang)[..., None] * TIE_V
    return np.cos(beta)[..., None] * TIE_A + np.sin(beta)[..., None] * side


def _edge_beta_table(n=721):
    """the angle from the tie at which the cap meets the hairline, round the tie (table over s)"""
    ss = np.linspace(0, 1, n)
    out = np.zeros(n)
    for i, s in enumerate(ss):
        lo, hi = 0.05, math.pi * 0.98
        for _ in range(40):
            mid = 0.5 * (lo + hi)
            p = cap_point(cap_dir(s, mid))
            phi = math.degrees(math.atan2(p[0] - CAP_C[0], p[2] - CAP_C[2]))
            if p[1] > hairline_y(phi):
                lo = mid
            else:
                hi = mid
        out[i] = 0.5 * (lo + hi)
    return ss, out


_EDGE = None


def edge_beta(s):
    global _EDGE
    if _EDGE is None:
        _EDGE = _edge_beta_table()
    return np.interp(np.asarray(s, float), _EDGE[0], _EDGE[1])


def cap_fn(s, t):
    s = np.asarray(s, float)
    t = np.asarray(t, float)
    tt = np.minimum(t, 1.0 - LIP) / (1.0 - LIP)          # the outer surface: 0..1 to the hairline
    beta = edge_beta(s) * tt
    d = cap_dir(s, beta)
    p = cap_point(d)
    # the lip: from the edge in under it, level or rising, to inside the head (a rounded turn: seen
    # from below, the hair's thickness)
    k = np.clip((t - (1.0 - LIP)) / LIP, 0.0, 1.0)[..., None]
    if np.any(k > 0):
        inner = p + (CAP_C - p) * 0.25 + np.array([0.0, 2.0, 0.0])
        turn = np.sin(k * math.pi / 2)
        drop = np.array([0.0, -0.6, 0.0]) * np.sin(k * math.pi)
        p = p * (1 - turn) + inner * turn + drop
    return p


# ------------------------------------------------------------------------------------- the painting
def sheen_band(P, N, u, along=None):
    """the sheen (0..1) at points P: a band about the tie's axis, front and top; its upper (tie-ward)
    edge cut into broad points by `u` (the across coordinate of the piece: points run along the hair)"""
    d = unit(P - CAP_C)
    ang = np.degrees(np.arccos(np.clip((d * TIE_A).sum(-1), -1, 1)))  # from the tie
    front = smoothstep(-0.5, 0.35, d[..., 2]) * smoothstep(-0.2, 0.25, N[..., 1] * 0.6 + N[..., 2] * 0.8)
    lo = 46.0 + 7.0 * np.abs(((np.asarray(u) * 3.0) % 1.0) - 0.5) * 2.0  # pointed edge toward the tie
    hi = 74.0
    band = (ang > lo) & (ang < hi)
    core = (ang > lo + 5.0) & (ang < hi - 9.0)
    return band * front, core * front


def hair_paint(P, N, u, t, outer, rim=0.07, lines=(), root_dark=0.0, sheen=True):
    """the hair's colour: u across the visible face of a lock (0..1, the rim at both ends), t along it;
    outer: True where that face is seen (else its underside)"""
    shape = np.broadcast_shapes(np.shape(u), np.shape(t))
    col = np.broadcast_to(HAIR, shape + (3,)).copy()
    if sheen:
        band, core = sheen_band(P, N, u)
        col = np.where(band[..., None], SHEEN_SOFT, col)
        col = np.where(core[..., None], SHEEN, col)
    for (u0, t0, t1, w) in lines:
        # a strand line: dark, along t from t0 to t1, tapering at both ends
        taper = smoothstep(t0, t0 + 0.25 * (t1 - t0), t) * (1 - smoothstep(t1 - 0.15 * (t1 - t0), t1, t))
        on = np.abs(u - u0) < w * 0.5 * taper
        col = np.where(on[..., None], LINE, col)
    if root_dark:
        col = col * (1 - root_dark * (1 - smoothstep(0.0, 0.18, t)))[..., None]
    edge = (u < rim) | (u > 1 - rim)
    col = np.where(edge[..., None], LINE, col)
    under = np.where(N[..., 1] < -0.25, 1.0, 0.0)
    col = np.where(outer[..., None], col, UNDER * (1 - 0.15 * under[..., None]))
    return col


# ------------------------------------------------------------------------------------- locks
class Lock:
    """a lock of hair: a flattened tube along a spine, `width` across (a profile along it), `depth`
    thick, its far end a point; `cup` bends its edges in toward the head; its upper face (toward `out`)
    painted with the hair, its underside darker"""

    def __init__(self, name, points, width, depth, out, cup=0.0, lines=(), texels=(32, 64), n_t=18, n_s=10,
                 root_dark=0.25, inner=0.35, sheen=True, twist=None, rim=0.07):
        self.name, self.rim = name, rim
        self.spine = Spine(points, out)
        self.width, self.depth, self.cup, self.inner = width, depth, cup, inner
        self.lines, self.root_dark, self.sheen, self.twist = lines, root_dark, sheen, twist
        # the cross-section: s round it; s 0..0.5 the upper face (more knots there), 0.5..1 under
        s_up = 0.5 * (0.5 - 0.5 * np.cos(np.linspace(0, math.pi, n_s + 1)))
        s_dn = 0.5 + 0.5 * np.linspace(0, 1, max(4, n_s // 2) + 1)[1:]
        t_k = 1 - (1 - np.linspace(0, 1, n_t + 1)) ** 1.35  # (closer toward the point)
        self.piece = Piece(name, self.fn, np.concatenate([s_up, s_dn]), t_k, texels, self.shade)

    def fn(self, s, t):
        s = np.asarray(s, float)
        t = np.asarray(t, float)
        C, T, N, B = self.spine.at(t)
        if self.twist is not None:
            a = np.radians(profile(self.twist, t))[..., None]
            N, B = N * np.cos(a) + B * np.sin(a), B * np.cos(a) - N * np.sin(a)
        w = profile(self.width, t)[..., None] * 0.5
        h = profile(self.depth, t)[..., None]
        ang = 2 * math.pi * s
        # s 0..0.5: the upper arc from the edge at +B over to -B; 0.5..1: the underside back
        ca, sa = np.cos(math.pi * np.minimum(s, 0.5) * 2)[..., None], np.sin(math.pi * np.minimum(s, 0.5) * 2)[..., None]
        up = C + B * ca * w + N * sa * h - N * self.cup * (ca ** 2) * w
        cb = np.cos(math.pi * (2 * s - 1.0))[..., None]
        sb = np.sin(math.pi * (2 * s - 1.0))[..., None]
        dn = C - B * cb * w - N * sb * h * self.inner - N * self.cup * (cb ** 2) * w
        del ang
        return np.where((s <= 0.5)[..., None], up, dn)

    def shade(self, P, N, s, t):
        outer = s <= 0.5
        u = np.where(outer, s * 2, (s - 0.5) * 2)
        return hair_paint(P, N, u, t, outer, rim=self.rim, lines=self.lines, root_dark=self.root_dark,
                          sheen=self.sheen)


def side_out(sign):
    """outward for locks at his side: away from the head's vertical axis (through z -6)"""
    def f(C):
        d = np.asarray(C, float) - np.array([0.0, 0.0, -6.0])
        d = d * np.array([1.0, 0.0, 1.0])
        return unit(d)
    return f


def radial(C):
    return unit(np.asarray(C, float) - CAP_C)


def on_cap(phi, y, lift):
    """a point `lift` above the cap at the angle phi round the head (degrees) and height y"""
    phi = math.radians(phi)
    # march the direction's elevation until the cap point is at height y
    lo, hi = -math.pi / 2, math.pi / 2
    for _ in range(40):
        mid = 0.5 * (lo + hi)
        d = np.array([math.cos(mid) * math.sin(phi), math.sin(mid), math.cos(mid) * math.cos(phi)])
        p = cap_point(d)
        if p[1] < y:
            lo = mid
        else:
            hi = mid
    d = np.array([math.cos(lo) * math.sin(phi), math.sin(lo), math.cos(lo) * math.cos(phi)])
    p = cap_point(d)
    n = radial(p)
    return p + n * lift


# The swept locks over the front: from the hairline up and back toward the tie, lying on the cap (their
# roots at the hairline, their ends sinking into it near the crown). (angle round the head, width)
SWEEPS = [(-9.0, 12.0), (2.0, 13.0), (12.0, 12.0)]


def sweep_lock(i, phi, width):
    pts = []
    xs, ys = zip(*HAIRLINE)
    base_y = float(np.interp(abs(phi), xs, ys))  # (above the edge's points: they show under it)
    tie_dir = unit(TIE - CAP_C)
    start = on_cap(phi, base_y + 3.2, 0.2)
    p0 = unit(start - CAP_C)
    for k in range(6):
        f = k / 5.0
        d = unit(p0 * (1 - f * 0.8) + tie_dir * f * 0.8)
        lift = (0.4, 1.3, 1.5, 1.2, 0.5, -1.5)[k]
        pts.append(cap_point(d) + d * lift)
    lines = [(0.38 + 0.08 * (i % 2), 0.1, 0.8, 0.035)]
    return Lock("sweep%d" % i, pts, [width * 0.85, width, width * 0.95, width * 0.75, width * 0.45, 1.0],
                [1.2, 2.0, 2.0, 1.6, 1.0, 0.5], radial, cup=0.2, lines=lines, texels=(40, 64), n_t=14, n_s=10,
                root_dark=0.0)


# the small points of the hairline (the concept's saw teeth either side of the widow's peak): short
# pointed tips lying on the forehead under the cap's edge, a little toward the middle. (angle round the
# head, width, length)
TIPS = [(-21.0, 3.4, 2.8), (-10.5, 3.0, 2.3), (12.0, 3.0, 2.4), (22.5, 3.4, 2.9)]


def hairline_tips():
    locks = []
    for i, (phi, w, length) in enumerate(TIPS):
        # the cap's edge there: the s round the tie whose edge point is at this angle
        ss = np.linspace(0.85, 1.15, 601) % 1.0
        E = cap_fn(ss, np.full_like(ss, 1.0 - LIP))
        ang = np.degrees(np.arctan2(E[:, 0] - CAP_C[0], E[:, 2] - CAP_C[2]))
        e = E[int(np.argmin(np.abs(ang - phi)))]
        inward = -np.sign(phi) * 0.45
        pts = [e + np.array([0.0, 1.4, -0.2]), e + np.array([0.0, 0.0, 0.3]),
               e + np.array([inward * length * 0.5, -length * 0.55, 0.35]),
               e + np.array([inward * length, -length, 0.3])]
        locks.append(Lock("tip%d" % i, pts, [w, w * 0.9, w * 0.45, 0.2], [0.5, 0.5, 0.35, 0.1],
                          lambda C: unit(np.broadcast_to(np.array([0.0, 0.25, 1.0]), np.shape(C))), cup=0.05,
                          lines=[], texels=(12, 12), n_t=5, n_s=6, root_dark=0.0, sheen=False, rim=0.12))
    return locks


def blend_out(sign, y_top=58.0, y_low=48.0, front=(0.42, 0.0, 1.0)):
    """outward for the long side locks: radial from the cap's centre up on the head, turning to face
    forward and out (toward `front`, x by side) lower down, where they frame the face"""
    f = unit(np.array([front[0] * sign, front[1], front[2]]))

    def out(C):
        C = np.asarray(C, float)
        w = smoothstep(y_low, y_top, C[..., 1])[..., None]
        return unit(radial(C) * w + f * (1 - w))
    return out


def side_locks(sign):
    """per side (-1 his right, the viewer's left; +1 his left): the outer lock, from the top of his head
    out over the side and down to a point flared over his ear; the lock framing his face, from the
    front of the crown over the temple and down past the cheek to the jaw; a thin strand inside it,
    over the brow's end; the hair behind the ear"""
    x = lambda v: v * sign  # noqa: E731
    k = "r" if sign < 0 else "l"
    outer_pts = [on_cap(x(16.0), 76.0, -1.5), on_cap(x(42.0), 70.0, 2.2), on_cap(x(64.0), 60.0, 4.0),
                 on_cap(x(80.0), 50.0, 4.4), (x(39.5), 44.0, 1.0), (x(41.5 if sign < 0 else 40.0), 40.0, 0.5)]
    if sign < 0:
        frame_pts = [on_cap(x(8.0), 70.0, -1.0), on_cap(x(24.0), 63.0, 1.6), (x(20.5), 56.5, 17.6),
                     (x(25.0), 47.0, 15.0), (x(28.0), 36.0, 11.0), (x(29.3), 25.0, 9.3), (x(29.0), 15.0, 8.2),
                     (x(27.0), 7.5, 7.6)]
        frame_w = [9.0, 11.0, 11.5, 11.5, 11.0, 9.5, 7.0, 3.5, 0.6]
        strand_pts = [(x(16.0), 57.5, 17.6), (x(19.5), 50.0, 17.4), (x(22.0), 42.0, 15.0), (x(23.4), 33.0, 12.4),
                      (x(23.9), 22.0, 9.6)]
        strand_w = [4.5, 4.5, 3.8, 2.6, 0.5]
    else:
        frame_pts = [on_cap(x(8.0), 70.0, -1.0), on_cap(x(24.0), 63.0, 1.6), (x(19.5), 56.5, 17.8),
                     (x(24.0), 47.0, 15.8), (x(27.0), 36.0, 11.4), (x(28.6), 24.0, 9.4), (x(28.8), 14.0, 8.3),
                     (x(27.8), 6.0, 7.8)]
        frame_w = [8.5, 10.5, 11.0, 11.0, 10.5, 9.0, 6.5, 3.2, 0.6]
        strand_pts = [(x(15.5), 57.5, 17.8), (x(18.5), 51.0, 17.8), (x(20.4), 44.0, 16.6), (x(21.3), 37.5, 14.8)]
        strand_w = [4.2, 4.2, 3.0, 0.5]
    behind = [on_cap(x(110.0), 56.0, -1.0), on_cap(x(108.0), 46.0, 1.8), (x(31.5), 36.0, -12.0),
              (x(30.5), 27.0, -12.5), (x(28.5), 20.0, -12.0)]
    return [
        Lock("outer_" + k, outer_pts, [10.0, 14.0, 15.0, 13.0, 8.0, 2.5, 0.4], [1.6, 2.6, 3.0, 2.8, 2.2, 1.2, 0.4],
             radial, cup=0.16, lines=[(0.33, 0.12, 0.85, 0.03), (0.66, 0.2, 0.92, 0.03)], texels=(40, 96), n_t=22,
             n_s=10),
        Lock("frame_" + k, frame_pts, frame_w, [1.6, 2.4, 2.6, 2.6, 2.4, 2.2, 1.8, 1.0, 0.4], blend_out(sign),
             cup=0.1, lines=[(0.36, 0.25, 0.92, 0.035), (0.7, 0.45, 0.97, 0.03)], texels=(40, 112), n_t=30, n_s=10),
        Lock("strand_" + k, strand_pts, strand_w, [1.0, 1.1, 1.0, 0.8, 0.4][:len(strand_w)],
             lambda C: unit(np.broadcast_to(np.array([0.3 * sign, 0.05, 1.0]), np.shape(C))), cup=0.05,
             lines=[], texels=(12, 56), n_t=14, n_s=6, rim=0.16),
        Lock("behind_" + k, behind, [12.0, 14.0, 13.0, 9.0, 1.0], [1.6, 2.6, 2.4, 1.8, 0.5],
             lambda C: unit(np.broadcast_to(np.array([sign * 1.0, 0.0, -0.5]), np.shape(C))), cup=0.15,
             lines=[(0.45, 0.2, 0.9, 0.035)], texels=(32, 48), n_t=14, n_s=10),
    ]


# ------------------------------------------------------------------------------------- the ponytail
TAIL = dict(
    # from inside the head through the tie, up from the crown and over to his left, down to the point
    points=[(-9.0, 72.5, -9.0), (-6.0, 79.0, -10.5), (0.5, 83.2, -12.2), (10.5, 85.0, -13.4), (21.5, 84.0, -14.0),
            (31.5, 80.8, -14.0), (38.8, 77.7, -14.0), (44.0, 75.5, -14.0)],
    radius=[7.2, 7.8, 8.0, 7.8, 7.0, 5.8, 4.2, 2.2, 0.5],
    squash=0.8, band_t=0.1,
)


class Ponytail:
    def __init__(self):
        self.spine = Spine(TAIL["points"], lambda C: np.broadcast_to(np.array([0.0, 0.0, 1.0]), np.shape(C)))
        n_s, n_t = 14, 22
        self.piece = Piece("ponytail", self.fn, np.linspace(0, 1, n_s + 1), 1 - (1 - np.linspace(0, 1, n_t + 1)) ** 1.2,
                           (64, 96), self.shade)

    def fn(self, s, t):
        C, T, N, B = self.spine.at(t)
        r = profile(TAIL["radius"], t)[..., None]
        a = 2 * math.pi * np.asarray(s, float)[..., None]
        # (N: toward the face; the section: round, flattened front to back)
        up = unit(np.cross(T, N))
        return C + (np.cos(a) * up + np.sin(a) * N * TAIL["squash"]) * r

    def shade(self, P, N, s, t):
        # its top (s near 0: up) lit, strand lines along it, its underside darker
        a = 2 * math.pi * s
        u = (np.cos(a) + 1) / 2  # 1 on top
        col = np.broadcast_to(HAIR, np.shape(s) + (3,)).copy()
        top = np.abs(((s + 0.5) % 1.0) - 0.5)  # 0 on top
        sheen = (top < 0.07 + 0.03 * np.abs(((t * 5.0) % 1.0) - 0.5)) & (t > 0.2) & (t < 0.8)
        col = np.where(sheen[..., None], SHEEN, col)
        soft = (top < 0.14) & (t > 0.15) & (t < 0.85) & ~sheen
        col = np.where(soft[..., None], SHEEN_SOFT, col)
        for s0, t0, t1 in ((0.17, 0.2, 0.9), (0.83, 0.25, 0.92), (0.30, 0.35, 0.85), (0.70, 0.3, 0.8)):
            taper = smoothstep(t0, t0 + 0.15, t) * (1 - smoothstep(t1 - 0.1, t1, t))
            on = np.abs(((s - s0 + 0.5) % 1.0) - 0.5) < 0.012 * taper
            col = np.where(on[..., None], LINE, col)
        col = np.where((u < 0.3)[..., None], UNDER, col)
        return col


# ------------------------------------------------------------------------------------- band, cord, beads
def ring(name, centre, axis, radius, width, colour, edge=None, n_s=24):
    axis = unit(axis)
    ref = unit(np.cross(axis, [0.0, 0.0, 1.0]) if abs(axis[2]) < 0.9 else np.cross(axis, [1.0, 0.0, 0.0]))
    ref2 = np.cross(axis, ref)

    def fn(s, t):
        a = 2 * math.pi * np.asarray(s, float)[..., None]
        t = np.asarray(t, float)[..., None]
        # a rounded band: t 0..1 across it, bulging in the middle
        bulge = radius + 0.55 * np.sin(math.pi * t)
        return centre + axis * (t - 0.5) * width + (np.cos(a) * ref + np.sin(a) * ref2 * TAIL["squash"]) * bulge

    def shade(P, N, s, t):
        col = np.broadcast_to(colour, np.shape(s) + (3,)).copy()
        lit = 0.75 + 0.35 * np.clip(N[..., 1] * 0.7 + N[..., 2] * 0.5, 0, 1)
        col = col * lit[..., None]
        if edge is not None:
            e = (t < 0.18) | (t > 0.82)
            col = np.where(e[..., None], edge * lit[..., None], col)
        dark = (t < 0.04) | (t > 0.96)
        return np.where(dark[..., None], LINE, col)
    return Piece(name, fn, np.linspace(0, 1, n_s + 1), np.linspace(0, 1, 5), (48, 16), shade)


def bead(name, centre, radius):
    c = np.asarray(centre, float)
    light = unit(np.array([-0.45, 0.65, 0.6]))

    def fn(s, t):
        a = 2 * math.pi * np.asarray(s, float)
        b = math.pi * np.asarray(t, float)
        d = np.stack([np.sin(b) * np.cos(a), np.cos(b), np.sin(b) * np.sin(a)], -1)
        return c + d * radius

    def shade(P, N, s, t):
        d = unit(P - c)
        l = (d * light).sum(-1)
        col = np.broadcast_to(JADE, np.shape(s) + (3,)).copy()
        col = np.where((l < -0.25)[..., None], JADE_DARK, col)
        # the highlight: a clean round spot up and to the left, as drawn
        spot = l > 0.86
        rim = (l > 0.80) & ~spot
        col = np.where(rim[..., None], (JADE + JADE_HI) / 2, col)
        col = np.where(spot[..., None], JADE_HI, col)
        # its outline (the drawing's): darker toward the silhouette seen from the front
        return col
    n = 15 if radius > 3 else 9
    return Piece(name, fn, np.linspace(0, 1, n), np.linspace(0, 1, (n + 3) // 2), (32, 24), shade, flip=True)


CORD = dict(beads=[(44.5, 75.0, -14.0), (54.0, 57.0, -15.0), (58.0, 38.0, -16.0)], bead_r=5.0, cord_r=0.9,
            knot=(58.0, 30.0, -16.0),
            tassel=[(58.0, 31.0, -16.0), (58.4, 25.0, -16.2), (58.0, 18.0, -16.2), (57.0, 12.5, -16.0),
                    (55.8, 8.5, -15.8)],
            tassel_w=[3.0, 6.0, 6.2, 4.0, 0.8])


def cord_pieces():
    b = [np.asarray(x, float) for x in CORD["beads"]]
    path = [b[0] + np.array([2.0, 2.0, 0.0]), b[0], (b[0] + b[1]) / 2 + np.array([3.0, 0.5, 0.0]), b[1],
            (b[1] + b[2]) / 2 + np.array([2.0, 0.0, 0.0]), b[2], np.asarray(CORD["knot"], float)]
    spine = Spine(path, lambda C: np.broadcast_to(np.array([0.0, 0.0, 1.0]), np.shape(C)))

    def fn(s, t):
        C, T, N, B = spine.at(t)
        a = 2 * math.pi * np.asarray(s, float)[..., None]
        return C + (np.cos(a) * B + np.sin(a) * N) * CORD["cord_r"]

    def shade(P, N, s, t):
        col = np.broadcast_to(CORD_COL, np.shape(s) + (3,)).copy()
        return col * (0.8 + 0.3 * np.clip(N[..., 1] * 0.6 + N[..., 2] * 0.6, 0, 1))[..., None]
    pieces = [Piece("cord", fn, np.linspace(0, 1, 6), np.linspace(0, 1, 33), (8, 64), shade)]
    for i, c in enumerate(b):
        pieces.append(bead("bead%d" % i, c, CORD["bead_r"]))
    pieces.append(bead("knot", CORD["knot"], 1.8))
    tassel = Lock("tassel", CORD["tassel"], CORD["tassel_w"], [1.6, 2.6, 2.6, 2.0, 0.6],
                  lambda C: unit(np.broadcast_to(np.array([0.3, 0.0, 1.0]), np.shape(C))), cup=0.1,
                  lines=[(0.45, 0.15, 0.9, 0.06)], texels=(24, 40), n_t=12, n_s=8, root_dark=0.0, sheen=False)
    pieces.append(tassel.piece)
    return pieces


CORD_COL = np.array([44.0, 30.0, 44.0])
# the hair band: B deep red with gold edges, A dark with jade edges (tools/mii/midoha_parts.py --band)
BAND_OPTION = "B"


# ------------------------------------------------------------------------------------- the ears
# an ear: a blade swept back, its upper edge nearly level, its lower edge steep, its point out at
# (+-46, 41); its front face painted with the inner fold (a darker shape following the outline)
EAR = dict(root=(24.0, -1.0), sweep=22.0, length=24.0, tip_y=41.0, top_y=39.0, bottom_y=26.0, thick=2.4)


def ear_piece(sign):
    e = EAR
    sw = math.radians(e["sweep"])
    axis = np.array([math.cos(sw) * sign, 0.0, -math.sin(sw)])   # out from the head, swept back
    front = np.array([math.sin(sw) * sign, 0.0, math.cos(sw)])    # its front face's normal
    root = np.array([e["root"][0] * sign, 0.0, e["root"][1]])

    def outline(s):
        """the blade's outline: s along it (0 root .. 1 point): its top and bottom heights"""
        top = e["top_y"] + (e["tip_y"] - e["top_y"]) * s
        bottom = e["bottom_y"] + (e["tip_y"] - e["bottom_y"]) * s ** 1.15
        return top, bottom

    def fn(s, t):
        """s along the blade (0..1), t round its section: 0..0.5 the front face (top to bottom), 0.5..1 back"""
        s = np.asarray(s, float)
        t = np.asarray(t, float)
        top, bottom = outline(s)
        L = e["length"] * s
        th = e["thick"] * (1 - 0.75 * s)
        f = np.where(t <= 0.5, t * 2, 2 - t * 2)  # 0 top .. 1 bottom on both faces
        y = top + (bottom - top) * f
        bulge = np.sin(math.pi * f) * th * 0.5
        z_off = np.where(t <= 0.5, bulge - 0.25 * np.sin(math.pi * f) * th, -bulge)
        p = root[None] * 0 + root + axis * L[..., None] + np.array([0.0, 1.0, 0.0]) * y[..., None]
        return p + front * z_off[..., None]

    def shade(P, N, s, t):
        col = np.broadcast_to(SKIN, np.shape(s) + (3,)).copy()
        face = t <= 0.5
        f = t * 2
        # the inner fold: inset from the outline, from near the root toward the point
        inner = face & (f > 0.22) & (f < 0.78 - 0.25 * s) & (s > 0.12) & (s < 0.78 - 0.1 * f)
        col = np.where(inner[..., None], SKIN_IN, col)
        rim = face & ((f < 0.05) | (f > 0.95))
        col = np.where(rim[..., None], SKIN_LINE, col)
        back = ~face
        return np.where(back[..., None], SKIN * 0.93, col)
    return Piece("ear_%s" % ("r" if sign < 0 else "l"), fn, np.linspace(0, 1, 15), np.linspace(0, 1, 13), (40, 32),
                 shade)


# ------------------------------------------------------------------------------------- all of them
def cap_shade(P, N, s, t):
    outer = t <= 1.0 - LIP
    tt = np.minimum(t / (1.0 - LIP), 1.0)
    # strand lines toward the tie: a few round the head, tapering toward it and toward the hairline
    col = np.broadcast_to(HAIR, np.shape(s) + (3,)).copy()
    band, core = sheen_band(P, N, (s * 22.0) % 1.0)
    col = np.where(band[..., None], SHEEN_SOFT, col)
    col = np.where(core[..., None], SHEEN, col)
    for k in range(6):
        s0 = (k + 0.5 * (k % 3) / 3.0) / 6.0
        w = 0.0016 + 0.0020 * smoothstep(0.15, 0.55, tt) * (1 - smoothstep(0.7, 0.92, tt))
        on = (np.abs(((s - s0 + 0.5) % 1.0) - 0.5) < w) & (tt > 0.2 + 0.1 * (k % 2)) & (tt < 0.9)
        col = np.where(on[..., None], LINE, col)
    # darker toward the hairline at the back and the sides (under the locks), the lip dark
    col = np.where((tt > 0.88)[..., None], col * 0.86, col)
    # the lip: the hair's own colour (seen from the front it is the hair's thickness), a thin dark line
    # where it turns under, darker further in
    col = np.where(outer[..., None], col, np.where((t < 1.0 - LIP * 0.55)[..., None], HAIR * 0.92, UNDER))
    edge = (t > 1.0 - LIP - 0.012) & (t < 1.0 - LIP + 0.012)
    return np.where(edge[..., None], LINE, col)


def cap_piece():
    x = np.linspace(0, 1, 69)
    s_k = x - 0.62 * np.sin(2 * math.pi * x) / (2 * math.pi)  # (closer at the front: the hairline's points)
    t_k = np.concatenate([np.linspace(0, 1 - LIP, 16), 1 - LIP + LIP * np.array([0.35, 1.0])])
    return Piece("cap", cap_fn, s_k, t_k, (368, 88), cap_shade)


def build():
    """{name: Piece}"""
    pieces = [cap_piece()]
    for i, (phi, w) in enumerate(SWEEPS):
        pieces.append(sweep_lock(i, phi, w).piece)
    for lk in hairline_tips():
        pieces.append(lk.piece)
    for sign in (-1, 1):
        for lk in side_locks(sign):
            pieces.append(lk.piece)
        pieces.append(ear_piece(sign))
    tail = Ponytail()
    pieces.append(tail.piece)
    C, T, N, B = tail.spine.at(TAIL["band_t"])
    r = float(profile(TAIL["radius"], TAIL["band_t"]))
    colour, edge = {"A": (CORD_COL, JADE), "B": (BAND, GOLD)}[BAND_OPTION]
    pieces.append(ring("band", C, T, r + 0.5, 3.4, colour, edge=edge))
    pieces += cord_pieces()
    return {p.name: p for p in pieces}
