#!/usr/bin/env python3
"""ミドハ's hanbok as a model of its own: a garment mesh skinned to the Mii body's skeleton.

    tools/mii/midoha_outfit_model.py build [--out tools/mii/midoha_outfit_mesh.json]
    tools/mii/midoha_outfit_model.py preview OUT.png        the front view over the concept's

What it builds (all of it ours: the user's concept, our geometry; the Mii body is read, from the
user's extracted files, only to fit the garment and to find its weights, and nothing of it is
written out):
  * the jeogori (the wrap top): a shell over the torso with some volume, its hem blousing over the
    belt, the shoulders as the concept draws them;
  * the sleeves: wide tubes along the arms, widening to broad open cuffs at the wrists (the Mii's
    hands come out of them), lined inside;
  * the collar: the git (the dark green band) as one raised ribbon from under the over-panel up
    round the back of the neck and down across the chest to the hip, with the white dongjeong on
    its neck side as a second ribbon on top of it;
  * the neck: a skin-coloured tube from inside the collar up into the head;
  * the belt with its buckle (a raised plate and an inner frame);
  * the skirt (chima-like) from the waist to the ankles, flaring, with real pleats (knife pleats
    turned away from a flat front and back panel), lined inside.

It is modelled in the pose the concept shows him in: the Mii's Wait clip, frame 0, seen from the
front (the concept's own measures: 8.9 pixels of the concept to a Mii unit, its chin at the head
node). Each vertex then gets its weights (at most 4 of the body's nodes), and its bind-pose place
is the inverse of its skinning in that pose (p_bind = (sum w_i Wait_i bind_i^-1)^-1 p_wait): so in
the Wait clip the garment is exactly as modelled, and every other clip moves it as it moves the body.

Weights: the jeogori and the sleeves from the nearest body vertices (the torso's, the arm's), then
smoothed over the garment's own surface; the collar, the belt and the buckle take the weights of the
surface under them (so they never part from it); the skirt is blended by hand between the hips and
the legs (the hips at the waist, the thighs then the shins lower down, each side to its own leg and
the middle front and back half and half), the waist (belt, the jeogori's hem, the skirt's top)
half the chest and half the hips.

The mesh file (JSON, our data, committed): the bones by the Mii's node names with their inverse bind
matrices, the vertices (bind-pose position, normal, texture coordinate, weights), the triangles by
piece, and the atlas's layout. tools/mii/midoha_outfit_paint.py paints its texture,
tools/mii/midoha_outfit.py packs both for the game.
"""
import argparse
import json
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import midoha_body as body  # noqa: E402

MESH_FILE = os.path.join(HERE, "midoha_outfit_mesh.json")

# ------------------------------------------------------------------------------- the concept's measures
# concept.png (1024 x 1024): 8.9 pixels a Mii unit, his chin at pixel (507, 583) = the head node's
# height in the Wait pose (Head_Only, 108.65), the figure's middle at x 507
CONCEPT_SCALE = 8.9
CONCEPT_ORIGIN = (507.0, 583.0)
CHIN_Y = 108.65
HEAD_Z = 0.9


def c2m(px, py):
    """concept pixel -> (x, y) Mii units in the front view"""
    return ((px - CONCEPT_ORIGIN[0]) / CONCEPT_SCALE, CHIN_Y - (py - CONCEPT_ORIGIN[1]) / CONCEPT_SCALE)


def m2c(x, y):
    return (CONCEPT_ORIGIN[0] + x * CONCEPT_SCALE, CONCEPT_ORIGIN[1] + (CHIN_Y - y) * CONCEPT_SCALE)


# ------------------------------------------------------------------------------------------ helpers
def smoothstep(e0, e1, x):
    t = np.clip((np.asarray(x, float) - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def interp_rows(table, y):
    """table: [(y, v0, v1, ...)] sorted by y ascending -> values at y (linear, clamped)"""
    t = np.asarray(table, float)
    return np.array([np.interp(y, t[:, 0], t[:, k]) for k in range(1, t.shape[1])])


def spow(v, e):
    return np.sign(v) * np.abs(v) ** e


def unit(v):
    v = np.asarray(v, float)
    n = np.linalg.norm(v, axis=-1, keepdims=True)
    return v / np.maximum(n, 1e-12)


def catmull(points, n):
    """a smooth curve through points (m,3), n samples evenly in arc length"""
    P = np.asarray(points, float)
    ext = np.vstack([2 * P[0] - P[1], P, 2 * P[-1] - P[-2]])
    dense = []
    for i in range(1, len(ext) - 2):
        p0, p1, p2, p3 = ext[i - 1], ext[i], ext[i + 1], ext[i + 2]
        for t in np.linspace(0, 1, 24, endpoint=False):
            t2, t3 = t * t, t * t * t
            dense.append(0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 +
                                (-p0 + 3 * p1 - 3 * p2 + p3) * t3))
    dense.append(P[-1])
    dense = np.array(dense)
    seg = np.linalg.norm(np.diff(dense, axis=0), axis=1)
    s = np.concatenate([[0], np.cumsum(seg)])
    want = np.linspace(0, s[-1], n)
    return np.stack([np.interp(want, s, dense[:, k]) for k in range(3)], 1)


class Piece:
    """a part of the garment: vertices in the modelling pose (Wait, frame 0), triangles wound
    counter-clockwise seen from outside (right-handed outward normals; the game's GX wants the
    other order, tools/mii/midoha_outfit.py turns them), the piece's own surface coordinates
    st (n,2) in Mii units (round, along) from which the atlas is laid out and the texture painted,
    and `weld`: for each vertex the vertex whose normal it shares (a seam's duplicate column)."""

    def __init__(self, name, P, T, st, weld=None, attrs=None):
        self.name = name
        self.P = np.asarray(P, float)
        self.T = np.asarray(T, np.int32)
        self.st = np.asarray(st, float)
        self.weld = np.arange(len(P)) if weld is None else np.asarray(weld)
        self.attrs = attrs or {}
        self.W = None          # (n, n_nodes) weights
        self.N = None          # normals (modelling pose)
        self.uv = None         # atlas coordinates (0..1)

    def flip(self):
        self.T = self.T[:, ::-1].copy()

    def normals(self):
        P, T = self.P, self.T
        fn = np.cross(P[T[:, 1]] - P[T[:, 0]], P[T[:, 2]] - P[T[:, 0]])
        acc = np.zeros_like(P)
        for c in range(3):
            np.add.at(acc, self.weld[T[:, c]], fn)
        self.N = unit(acc[self.weld])
        return self.N


def grid_tris(rows, cols, closed=False):
    """quads of a rows x cols vertex grid (row-major) -> triangles; closed: the last column joins
    the first (no duplicate column)"""
    T = []
    cmax = cols if closed else cols - 1
    for r in range(rows - 1):
        for c in range(cmax):
            c1 = (c + 1) % cols
            a, b = r * cols + c, r * cols + c1
            d, e = (r + 1) * cols + c, (r + 1) * cols + c1
            T.append((a, b, e))
            T.append((a, e, d))
    return np.array(T, np.int32)


def tube(rings, along=None):
    """rings (R, N, 3): a tube; a duplicate seam column is added for the texture (its normals
    welded to the first). st: s the arc length round each ring from its first point, t the
    distance along (`along` (R,) if given, else between the rings' centres)."""
    rings = np.asarray(rings, float)
    R, N, _ = rings.shape
    P = np.concatenate([rings, rings[:, :1]], axis=1)          # (R, N+1, 3)
    seg = np.linalg.norm(np.diff(P, axis=1), axis=2)            # (R, N)
    s = np.concatenate([np.zeros((R, 1)), np.cumsum(seg, axis=1)], axis=1)
    if along is None:
        c = rings.mean(axis=1)
        along = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(c, axis=0), axis=1))])
    t = np.repeat(np.asarray(along, float)[:, None], N + 1, axis=1)
    st = np.stack([s, t], -1).reshape(-1, 2)
    T = grid_tris(R, N + 1)
    weld = np.arange(R * (N + 1)).reshape(R, N + 1)
    weld[:, N] = weld[:, 0]
    return P.reshape(-1, 3), T, st, weld.reshape(-1)


def orient_outward(piece, centre_fn):
    """flip the piece's triangles if they face toward centre_fn(points) on the whole"""
    P, T = piece.P, piece.T
    fn = np.cross(P[T[:, 1]] - P[T[:, 0]], P[T[:, 2]] - P[T[:, 0]])
    mid = P[T].mean(axis=1)
    if (fn * (mid - centre_fn(mid))).sum() < 0:
        piece.flip()


# ------------------------------------------------------------------------------------- the body
class Body:
    """the Mii body in the modelling pose (Wait, frame 0) and its bind pose"""

    def __init__(self):
        self.m = body.model()
        self.names = [n.name for n in self.m.nodes]
        self.n = len(self.names)
        self.Wb = body.bind(self.m)
        self.Wg = body.pose(self.m, "wait", 0.0)
        self.S = body.skin_matrices(self.Wg, self.Wb, self.n)
        self.idx = {nm: i for i, nm in enumerate(self.names)}
        Pb, wts, mats = body.vertices(self.m)
        self.vw = np.zeros((len(Pb), self.n))
        for i, wd in enumerate(wts):
            for nd, w in wd.items():
                self.vw[i, nd] += w
        self.vp_bind = Pb
        self.vp = body.skin_dense(Pb, self.vw, self.S)   # the body's vertices posed (Wait)

    def joint(self, name):
        return self.Wg[self.idx[name]][:3, 3].copy()

    def onehot(self, name):
        w = np.zeros(self.n)
        w[self.idx[name]] = 1
        return w


# ----------------------------------------------------------------------------- the jeogori's shell
# rows: y, half-width A, front depth Bf, back depth Bb (from the middle z0), z0 -- in the Wait pose.
# The front view's half-width follows the concept (the torso between the sleeves' creases, 14.7 at
# the chest, the belt's 15), the depth a slim chest (the Mii's own belly is hidden under it)
SHELL_ROWS = [
    (66.6, 14.55, 10.9, 9.15, 1.1),
    (72.6, 14.75, 11.3, 9.35, 1.1),
    (75.0, 15.1, 11.9, 9.7, 1.1),
    (76.6, 15.3, 12.3, 9.95, 1.1),
    (78.6, 15.3, 12.35, 10.0, 1.1),
    (82.0, 15.15, 12.2, 10.0, 1.1),
    (86.0, 14.95, 12.0, 9.9, 1.1),
    (90.0, 14.6, 11.6, 9.7, 1.1),
    (94.0, 14.1, 11.0, 9.35, 1.1),
    (97.5, 13.35, 10.2, 8.9, 1.1),
    (100.3, 12.3, 9.3, 8.2, 1.1),
    (102.6, 10.9, 8.3, 7.3, 1.1),
    (104.5, 9.3, 7.2, 6.3, 1.1),
    (105.9, 7.8, 6.3, 5.5, 1.0),
    (107.0, 6.4, 5.5, 4.8, 0.9),
]
SHELL_N = 28
SHELL_EXP = 2.5     # superellipse exponent: a little boxy, as the concept's front panels fall


def shell_point(y, phi):
    """the shell at height y and angle phi (0 the front middle, +pi/2 the Mii's left)"""
    A, Bf, Bb, z0 = interp_rows(SHELL_ROWS, y)
    e = 2.0 / SHELL_EXP
    sx, cz = np.sin(phi), np.cos(phi)
    x = A * spow(sx, e)
    z = z0 + np.where(cz >= 0, Bf, Bb) * spow(cz, e)
    return np.stack([x, np.full_like(x, y), z], -1)


def make_shell():
    ys = [r[0] for r in SHELL_ROWS]
    phis = -math.pi + 2 * math.pi * np.arange(SHELL_N) / SHELL_N   # from the back middle, round by
    rings = np.array([shell_point(y, phis) for y in ys])           # the Mii's right
    P, T, st, weld = tube(rings)
    # arc length along: up the front middle
    front = np.array([shell_point(y, np.array([0.0]))[0] for y in ys])
    along = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(front, axis=0), axis=1))])
    st[:, 1] = np.repeat(along, SHELL_N + 1)
    # s measured from the front middle (the texture's middle), seam at the back
    for r in range(len(ys)):
        row = st[r * (SHELL_N + 1):(r + 1) * (SHELL_N + 1), 0]
        row -= row[SHELL_N // 2]
    pc = Piece("jeogori", P, T, st, weld, attrs={"phi": np.tile(np.append(phis, math.pi), len(ys)),
                                                  "y": np.repeat(ys, SHELL_N + 1)})
    orient_outward(pc, lambda p: np.stack([np.zeros(len(p)), p[:, 1], np.full(len(p), 1.1)], 1))
    return pc


# ------------------------------------------------------------------------------------ the sleeves
# along the arm (the Wait pose's shoulder, elbow, wrist): the cap's top, the shoulder, the elbow,
# the cuff. Radii (lateral, front-back) at fractions of the length; the concept's sleeve is 12.8
# wide at the bottom of its picture (y 59), its outer edge from (19.7, 95) on, at the arm's angle
SLEEVE_N = 12
SLEEVE_PROFILE = [
    (0.00, 0.5, 0.5),
    (0.025, 2.4, 2.7),
    (0.06, 3.5, 3.9),
    (0.11, 4.35, 4.8),
    (0.18, 4.8, 5.3),
    (0.32, 5.1, 5.75),
    (0.50, 5.4, 6.25),
    (0.68, 5.7, 6.75),
    (0.84, 6.0, 7.2),
    (0.94, 6.15, 7.5),
    (1.00, 6.2, 7.6),
]
CUFF_Y = 57.4          # the cuff's lower edge (the hand ball hangs below it, its top inside)
CUFF_BAND = 2.9        # the darker green band at the cuff (kkeutdong), along the sleeve
LINING_UP = 0.4       # the lining goes this fraction of the way up from the cuff


def sleeve_path(bd, side):
    """the sleeve's centre line (Wait pose) for side +1 (the Mii's left) or -1"""
    sfx = "L" if side > 0 else "R"
    sh, el, wr = bd.joint("Arm_%s1" % sfx), bd.joint("Arm_%s2" % sfx), bd.joint("Wrist_%s" % sfx)
    # the cuff's centre: on the forearm's line, at CUFF_Y
    d = wr - el
    cuff = el + d * ((CUFF_Y - el[1]) / d[1])
    cuff[0] += side * 0.25
    top = sh + np.array([-side * 1.2, 1.5, 0.0])
    pts = [top, sh + np.array([side * 0.7, -2.0, 0.2]), 0.5 * (sh + el) + np.array([-side * 0.5, 0, 0.5]),
           el + np.array([-side * 0.9, 0, 1.0]), 0.5 * (el + cuff) + np.array([-side * 0.6, 0, 0.6]), cuff]
    return catmull(pts, 40)


def make_sleeve(bd, side):
    path = sleeve_path(bd, side)
    seg = np.linalg.norm(np.diff(path, axis=0), axis=1)
    s = np.concatenate([[0], np.cumsum(seg)])
    L = s[-1]
    prof = np.asarray(SLEEVE_PROFILE)
    # rings: denser at the cap and the cuff
    ts = np.array([0.0, 0.025, 0.06, 0.11, 0.18, 0.27, 0.38, 0.5, 0.62, 0.74, 0.85,
                   1.0 - CUFF_BAND / L, 1.0])
    ts = np.unique(np.clip(ts, 0, 1))
    rings, centres, frames = [], [], []
    psi = 2 * math.pi * np.arange(SLEEVE_N) / SLEEVE_N
    for t in ts:
        c = np.array([np.interp(t * L, s, path[:, k]) for k in range(3)])
        j = min(int(np.searchsorted(s, t * L)), len(path) - 1)
        tan = unit(path[min(j + 1, len(path) - 1)] - path[max(j - 1, 0)])
        lat = unit(np.cross(tan, [0, 0, 1.0])) * -side      # outward, in the frontal plane
        if lat[0] * side < 0:
            lat = -lat
        dep = unit(np.cross(lat, tan))
        if dep[2] < 0:
            dep = -dep
        rl, rd = np.interp(t, prof[:, 0], prof[:, 1]), np.interp(t, prof[:, 0], prof[:, 2])
        # psi = 0 points down-inward-back (the seam, toward the body and behind)
        a = psi + math.pi * 0.8
        pts = c + np.outer(np.cos(a) * rl, lat) + np.outer(np.sin(a) * rd, dep)
        rings.append(pts)
        centres.append(c)
        frames.append((lat, dep, tan))
    P, T, st, weld = tube(np.array(rings), along=ts * L)
    attrs = {"t": np.repeat(ts, SLEEVE_N + 1), "L": L, "side": side,
             "lat": np.repeat(np.array([fr[0] for fr in frames]), SLEEVE_N + 1, axis=0)}
    pc = Piece("sleeve_%s" % ("L" if side > 0 else "R"), P, T, st, weld, attrs)
    cen = np.repeat(np.array(centres), SLEEVE_N + 1, axis=0)
    pc.attrs["centre"] = cen
    orient_outward(pc, lambda p: cen[np.argmin(((cen[None, :, :] - p[:, None, :]) ** 2).sum(-1), axis=1)])
    # the lining: the last rings, pulled in, wound inward, and the rim between
    k0 = int(np.searchsorted(ts, 1 - LINING_UP))
    inner = []
    for r in range(k0, len(ts)):
        c = centres[r]
        ring = np.array(rings[r])
        inner.append(c + (ring - c) * (1 - 0.55 / np.linalg.norm(ring - c, axis=1))[:, None])
    inner = np.array(inner)
    # rim: the outer last ring and the inner last ring
    rim = np.array([rings[-1], inner[-1]])
    Pi, Ti, sti, wi = tube(inner[::-1], along=ts[k0:][::-1] * L)   # from the cuff upward
    lining = Piece("lining_%s" % ("L" if side > 0 else "R"), Pi, Ti, sti, wi,
                   {"t": np.repeat(ts[k0:][::-1], SLEEVE_N + 1), "side": side})
    ci = np.repeat(np.array(centres[k0:])[::-1], SLEEVE_N + 1, axis=0)
    orient_outward(lining, lambda p: ci[np.argmin(((ci[None, :, :] - p[:, None, :]) ** 2).sum(-1), axis=1)])
    lining.flip()   # it faces inward
    Pr, Tr, str_, wr = tube(rim, along=[0.0, 0.55])
    rimp = Piece("cuffrim_%s" % ("L" if side > 0 else "R"), Pr, Tr, str_, wr, {"side": side})
    # the rim faces down, out of the cuff: away from the sleeve's last-but-one ring's centre
    cprev = centres[-3]
    orient_outward(rimp, lambda p: np.broadcast_to(cprev, p.shape))
    return pc, lining, rimp


# ------------------------------------------------------------------------------------- the skirt
# rows (y, half-width, front depth, back depth, z0) and the pleats' depth at each
SKIRT_ROWS = [
    (11.4, 24.4, 18.0, 14.8, 1.6, 1.5),
    (16.0, 24.0, 17.7, 14.6, 1.6, 1.45),
    (24.0, 23.1, 17.0, 14.1, 1.55, 1.3),
    (33.0, 21.9, 16.1, 13.4, 1.5, 1.15),
    (42.0, 20.5, 15.1, 12.6, 1.4, 0.98),
    (50.5, 19.0, 14.0, 11.7, 1.3, 0.8),
    (57.0, 17.95, 13.1, 11.0, 1.2, 0.64),
    (62.0, 17.0, 12.4, 10.4, 1.1, 0.5),
    (66.5, 15.8, 11.6, 9.7, 1.1, 0.37),
    (71.5, 14.2, 10.6, 8.8, 1.1, 0.24),
]
SKIRT_PLEATS = 12        # knife pleats, six each side, turned away from the front and back panels
SKIRT_EXP = 2.25
PLEAT_SAMPLES = 4        # vertices across a pleat: the fold's foot, the face, the face, the crest
FRONT_PANEL = 0.075      # the flat front and back panels' half-width, as a fraction of the round


def pleat_profile(f):
    """f in [0,1) round the skirt (0 the front middle, 0.25 the Mii's left): (offset -1..1 of the
    pleat depth, phase within the pleat 0..1, the pleat's number). The front and back panels are
    flat; each side between them has SKIRT_PLEATS/2 pleats, whose folds face away from the panels."""
    f = np.mod(f, 1.0)
    # mirror: 0..0.5 is the Mii's left, 0.5..1 the right (seen as 0.5..0 mirrored)
    g = np.where(f <= 0.5, f, 1.0 - f)                     # 0 front .. 0.5 back
    side_span = 0.5 - 2 * FRONT_PANEL
    u = (g - FRONT_PANEL) / side_span                      # 0..1 over a side's pleats
    npl = SKIRT_PLEATS // 2
    x = np.clip(u, 0, 1) * npl
    k = np.minimum(np.floor(x), npl - 1)
    ph = x - k                                             # 0 at the fold toward the front
    off = np.where((u > 0) & (u < 1), 2 * ph - 1, 0.0)     # rises across the face, drops at the fold
    return off, ph, k


def skirt_angles():
    """the ring's angles (as fractions f of the round, the panel edges and pleat folds exact)"""
    npl = SKIRT_PLEATS // 2
    side_span = 0.5 - 2 * FRONT_PANEL
    fs = []
    # the Mii's left, from the front middle to the back middle
    fs += [0.0, FRONT_PANEL * 0.5]
    for k in range(npl):
        a = FRONT_PANEL + side_span * k / npl
        w = side_span / npl
        fs += [a + 0.004, a + w * 0.5, a + w - 0.004]
    fs += [0.5 - FRONT_PANEL, 0.5 - FRONT_PANEL * 0.5]
    left = np.array(fs)
    right = 1.0 - left[1:][::-1]
    ring = np.concatenate([left, [0.5], right[:-1]])
    return np.sort(np.mod(ring, 1.0))


def skirt_point(y, f, depth_scale=1.0, pleats=True):
    A, Bf, Bb, z0, pd = interp_rows(SKIRT_ROWS, y)
    phi = 2 * math.pi * f
    e = 2.0 / SKIRT_EXP
    sx, cz = np.sin(phi), np.cos(phi)
    base = np.stack([A * spow(sx, e), np.full_like(sx, y), z0 + np.where(cz >= 0, Bf, Bb) * spow(cz, e)], -1)
    if not pleats:
        return base
    off, _, _ = pleat_profile(f)
    # outward in the ring's plane
    nrm = np.stack([sx / A, np.zeros_like(sx), cz / np.where(cz >= 0, Bf, Bb)], -1)
    nrm = unit(nrm)
    return base + nrm * (pd * depth_scale * 0.5 * off)[:, None]


def make_skirt():
    ys = [r[0] for r in SKIRT_ROWS]
    fs = skirt_angles()
    # start the ring at the back middle (the seam), going round the Mii's right
    fs = np.sort(np.mod(fs - 0.5, 1.0)) + 0.5
    rings = np.array([skirt_point(y, np.mod(fs, 1.0)) for y in ys])
    P, T, st, weld = tube(rings)
    front = np.array([skirt_point(y, np.array([0.0]))[0] for y in ys])
    along = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(front, axis=0), axis=1))])
    st[:, 1] = np.repeat(along, len(fs) + 1)
    n1 = len(fs) + 1
    mid = int(np.argmin(np.abs(np.mod(fs, 1.0) - 0.0) + (np.mod(fs, 1.0) > 0.5)))
    for r in range(len(ys)):
        row = st[r * n1:(r + 1) * n1, 0]
        row -= row[mid]
    fv = np.tile(np.append(fs, fs[0] + 1.0), len(ys))     # unwrapped: 0.5 .. 1.5 round the ring
    pc = Piece("skirt", P, T, st, weld, attrs={"f": fv, "y": np.repeat(ys, n1)})
    orient_outward(pc, lambda p: np.stack([np.zeros(len(p)), p[:, 1], np.full(len(p), 1.3)], 1))
    # the lining: the same round without pleats, pulled in under their folds, hem to the waist
    lys = [11.4, 26.0, 46.0, 66.0]
    lr = np.array([skirt_point(y, np.mod(fs, 1.0), pleats=False) for y in lys])
    for r, y in enumerate(lys):
        A, Bf, Bb, z0, pd = interp_rows(SKIRT_ROWS, y)
        c = np.array([0, y, z0])
        d = lr[r] - c
        lr[r] = c + d * (1 - (0.5 * pd + 0.35) / np.linalg.norm(d, axis=1))[:, None]
    Pl, Tl, stl, wl = tube(lr)
    lining = Piece("skirt_lining", Pl, Tl, stl, wl, attrs={"f": np.tile(np.append(fs, fs[0] + 1.0), len(lys)),
                                                             "y": np.repeat(lys, n1)})
    orient_outward(lining, lambda p: np.stack([np.zeros(len(p)), p[:, 1], np.full(len(p), 1.3)], 1))
    lining.flip()
    # the hem's rim: the skirt's lowest ring to the lining's
    rim = np.array([rings[0], lr[0]])
    Pr, Tr, str_, wr = tube(rim, along=[0.0, 0.8])
    rimp = Piece("skirt_rim", Pr, Tr, str_, wr, attrs={"f": np.tile(np.append(fs, fs[0] + 1.0), 2)})
    orient_outward(rimp, lambda p: np.stack([np.zeros(len(p)), p[:, 1] + 5, np.full(len(p), 1.3)], 1))
    return pc, lining, rimp


# ------------------------------------------------------------------------------- surfaces' queries
def closest_on_mesh(P, T, Q):
    """for points Q (m,3): the closest point on triangles P[T], the triangle, its barycentrics"""
    A, B, C = P[T[:, 0]], P[T[:, 1]], P[T[:, 2]]
    best_d = np.full(len(Q), np.inf)
    best_p = np.zeros_like(Q)
    best_t = np.zeros(len(Q), int)
    best_b = np.zeros((len(Q), 3))
    for i0 in range(0, len(Q), 256):
        q = Q[i0:i0 + 256][:, None, :]
        ab, ac, ap = B - A, C - A, q - A
        d1, d2 = (ab * ap).sum(-1), (ac * ap).sum(-1)
        bp = q - B
        d3, d4 = (ab * bp).sum(-1), (ac * bp).sum(-1)
        cp = q - C
        d5, d6 = (ab * cp).sum(-1), (ac * cp).sum(-1)
        va = d3 * d6 - d5 * d4
        vb = d5 * d2 - d1 * d6
        vc = d1 * d4 - d3 * d2
        denom = np.where(np.abs(va + vb + vc) < 1e-12, 1e-12, va + vb + vc)
        v = vb / denom
        w = vc / denom
        # region tests (Ericson): vertex, edge or face
        u = 1 - v - w
        bu, bv, bw = u.copy(), v.copy(), w.copy()
        m = (d1 <= 0) & (d2 <= 0)
        bu[m], bv[m], bw[m] = 1, 0, 0
        m2 = (d3 >= 0) & (d4 <= d3) & ~m
        bu[m2], bv[m2], bw[m2] = 0, 1, 0
        m3 = (d6 >= 0) & (d5 <= d6) & ~m & ~m2
        bu[m3], bv[m3], bw[m3] = 0, 0, 1
        mab = (vc <= 0) & (d1 >= 0) & (d3 <= 0) & ~m & ~m2 & ~m3
        t = np.where(mab, d1 / np.where(d1 - d3 == 0, 1e-12, d1 - d3), 0)
        bu = np.where(mab, 1 - t, bu)
        bv = np.where(mab, t, bv)
        bw = np.where(mab, 0, bw)
        mac = (vb <= 0) & (d2 >= 0) & (d6 <= 0) & ~m & ~m2 & ~m3 & ~mab
        t = np.where(mac, d2 / np.where(d2 - d6 == 0, 1e-12, d2 - d6), 0)
        bu = np.where(mac, 1 - t, bu)
        bv = np.where(mac, 0, bv)
        bw = np.where(mac, t, bw)
        mbc = (va <= 0) & ((d4 - d3) >= 0) & ((d5 - d6) >= 0) & ~m & ~m2 & ~m3 & ~mab & ~mac
        den = (d4 - d3) + (d5 - d6)
        t = np.where(mbc, (d4 - d3) / np.where(den == 0, 1e-12, den), 0)
        bu = np.where(mbc, 0, bu)
        bv = np.where(mbc, 1 - t, bv)
        bw = np.where(mbc, t, bw)
        X = bu[..., None] * A + bv[..., None] * B + bw[..., None] * C
        d = ((X - q) ** 2).sum(-1)
        j = np.argmin(d, axis=1)
        r = np.arange(len(j))
        best_d[i0:i0 + 256] = d[r, j]
        best_p[i0:i0 + 256] = X[r, j]
        best_t[i0:i0 + 256] = j
        best_b[i0:i0 + 256] = np.stack([bu[r, j], bv[r, j], bw[r, j]], -1)
    return best_p, best_t, best_b, np.sqrt(best_d)


def ray_front(P, T, xy, z_from=60.0):
    """the surface's frontmost point under front-view points xy (m,2) (a ray down -z), its triangle
    and barycentrics; nan where it misses"""
    A, B, C = P[T[:, 0]], P[T[:, 1]], P[T[:, 2]]
    out = np.full((len(xy), 3), np.nan)
    tri = np.full(len(xy), -1)
    bary = np.zeros((len(xy), 3))
    for i, (x, y) in enumerate(xy):
        # 2D point in triangle (xy plane)
        v0 = C[:, :2] - A[:, :2]
        v1 = B[:, :2] - A[:, :2]
        v2 = np.array([x, y]) - A[:, :2]
        d00, d01, d11 = (v0 * v0).sum(1), (v0 * v1).sum(1), (v1 * v1).sum(1)
        d20, d21 = (v2 * v0).sum(1), (v2 * v1).sum(1)
        den = d00 * d11 - d01 * d01
        ok = np.abs(den) > 1e-12
        den = np.where(ok, den, 1)
        u = (d11 * d20 - d01 * d21) / den      # weight of C
        v = (d00 * d21 - d01 * d20) / den      # weight of B
        hit = ok & (u >= -1e-6) & (v >= -1e-6) & (u + v <= 1 + 1e-6)
        if not hit.any():
            continue
        z = A[:, 2] + u * (C[:, 2] - A[:, 2]) + v * (B[:, 2] - A[:, 2])
        z = np.where(hit, z, -np.inf)
        j = int(np.argmax(z))
        out[i] = (x, y, z[j])
        tri[i] = j
        bary[i] = (1 - u[j] - v[j], v[j], u[j])
    return out, tri, bary


def on_front(piece, xy):
    """front-view points on the piece's surface: where a ray down -z meets it, else (beyond its
    silhouette) the surface's closest point to (x, y, its depth there)"""
    pts, _, _ = ray_front(piece.P, piece.T, xy)
    miss = np.isnan(pts[:, 0])
    if miss.any():
        q = np.stack([xy[miss, 0], xy[miss, 1], np.full(miss.sum(), 6.0)], 1)
        pts[miss] = closest_on_mesh(piece.P, piece.T, q)[0]
    return pts


def interp_on(piece, tri, bary, values):
    T = piece.T[tri]
    return (values[T] * bary[:, :, None]).sum(1) if values.ndim == 2 else (values[T] * bary).sum(1)


# ---------------------------------------------------------------------------------- the collar
# Its inner edge (the neck's side) through the front view as the concept draws it (pixels), then
# round the back of the neck. Both sides are one ribbon: from under the over-panel's band (its end
# hidden there) up the Mii's right chest, round the back of the neck, and down from the Mii's left
# neck across the chest to the hem at his right hip.
COLLAR_UNDER_FRONT = [(520, 668), (499, 640), (484, 620), (470, 600)]          # up to the right neck
COLLAR_OVER_FRONT = [(537, 600), (506, 640), (477, 680), (448, 720), (421, 760), (393, 800), (366, 840),
                     (346, 872), (330, 905)]                                    # down under the belt
COLLAR_WIDTH = 7.4          # the git, across (its outer edge from the inner)
WHITE_WIDTH = 3.55          # the dongjeong, from the inner edge
WHITE_END_Y = 90.6          # where the over side's white strip ends, squared (concept y 740)
NECK_C = np.array([0.0, 0.0, 0.95])
NECK_R = 5.35               # the collar's inner edge round the back of the neck


def neck_ring_point(ang, y):
    """round the neck: ang 0 the back middle, +pi/2 the Mii's left"""
    return np.array([NECK_R * math.sin(ang), y, NECK_C[2] - NECK_R * math.cos(ang) * 0.92])


def collar_path(shell):
    """the inner edge's points (m,3) and the side of each (-1 the under side, +1 the over side),
    with the collar's arc length; the front points lie on the shell"""
    under = [c2m(*p) for p in COLLAR_UNDER_FRONT]
    over = [c2m(*p) for p in COLLAR_OVER_FRONT]
    fu = on_front(shell, np.array(under))
    fo = on_front(shell, np.array(over))
    back = [neck_ring_point(a, y) for a, y in ((-1.25, 107.15), (-0.75, 107.9), (0.0, 108.3), (0.75, 107.9),
                                              (1.25, 107.15))]
    pts = list(fu) + back + list(fo)
    side = [-1] * len(fu) + [0] * len(back) + [1] * len(fo)
    return np.array(pts), np.array(side, float)


def make_collar(shell, bd):
    """the git and the dongjeong as ribbons lying on the shell"""
    ctrl, side = collar_path(shell)
    n = 62
    path = catmull(ctrl, n)
    # the side's value along the path (-1 under, +1 over), smoothly
    cs = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(ctrl, axis=0), axis=1))])
    ps = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(path, axis=0), axis=1))])
    sval = np.interp(ps / ps[-1], cs / cs[-1], side)
    tan = unit(np.gradient(path, axis=0))
    # the surface's normal under each point: the shell's at the closest point
    sp, st_, sb, _ = closest_on_mesh(shell.P, shell.T, path)
    nrm = unit(interp_on(shell, st_, sb, shell.N))
    # across: in the surface, perpendicular to the path, away from the neck
    acr = unit(np.cross(nrm, tan))
    # which way is outward (away from the V and the neck): on the front sides out and up, round the
    # back of the neck down and away from it
    radial = path * [1, 0, 1] - [0, 0, NECK_C[2]]
    radial = unit(radial)
    back_ref = unit(radial + [0, -1.5, 0])
    front_ref = np.stack([np.sign(sval) * 1.0, np.full(n, 0.55), np.zeros(n)], 1)
    k = np.clip(np.abs(sval) * 2.0 - 0.6, 0, 1)[:, None]
    ref = back_ref * (1 - k) + front_ref * k
    flip = (acr * ref).sum(1) < 0
    acr[flip] *= -1
    # the offsets off the shell: the under side low, the over side above it
    lift = np.interp(sval, [-1, 0, 1], [0.32, 0.55, 0.78])
    across = np.array([0.0, 0.5, 1.0])

    def ribbon(width, extra, keep, name):
        rows = []
        for k in range(n):
            if not keep[k]:
                continue
            row = []
            for a in across:
                q = path[k] + acr[k] * (a * width[k])
                row.append(q)
            rows.append(row)
        rows = np.array(rows)                        # (m, A, 3)
        m_, A_ = rows.shape[:2]
        flat = rows.reshape(-1, 3)
        cp, ct, cb, _ = closest_on_mesh(shell.P, shell.T, flat)
        cn = unit(interp_on(shell, ct, cb, shell.N))
        lk = np.repeat(lift[keep] + extra, A_)
        top = cp + cn * lk[:, None]
        base = cp + cn * (lk - 0.3)[:, None]         # the walls' foot, under the edge
        top = top.reshape(m_, A_, 3)
        base = base.reshape(m_, A_, 3)
        # the cross-section: base(inner) - top(across...) - base(outer)
        sect = np.concatenate([base[:, :1], top, base[:, -1:]], axis=1)    # (m, A+2, 3)
        P = sect.reshape(-1, 3)
        T = grid_tris(m_, A_ + 2)
        along = ps[keep]
        w_ = width[keep]
        cross = np.concatenate([[-0.3], across, [1.0 + 0.3 / max(w_.max(), 1e-3)]])
        st = np.stack([np.outer(w_, cross).ravel(), np.repeat(along, A_ + 2)], 1)
        pc = Piece(name, P, T, st, attrs={"across": np.tile(cross, m_), "along": np.repeat(along, A_ + 2),
                                          "side": np.repeat(sval[keep], A_ + 2)})
        # outward: the shell's normal
        cnr = np.repeat(np.concatenate([cn.reshape(m_, A_, 3)[:, :1], cn.reshape(m_, A_, 3),
                                        cn.reshape(m_, A_, 3)[:, -1:]], axis=1).reshape(-1, 3), 1, axis=0)
        fnrm = np.cross(P[T[:, 1]] - P[T[:, 0]], P[T[:, 2]] - P[T[:, 0]])
        if (fnrm * cnr[T[:, 0]]).sum() < 0:
            pc.flip()
        return pc

    git_w = np.full(n, COLLAR_WIDTH)
    # narrower round the back of the neck (it stands closer to the neck there)
    git_w *= np.interp(sval, [-1, -0.3, 0, 0.3, 1], [1.0, 0.82, 0.78, 0.82, 1.0])
    keep_all = np.ones(n, bool)
    git = ribbon(git_w, 0.0, keep_all, "git")
    # the white strip: on the neck side, to its squared end on the over side
    white_keep = ~((sval > 0.5) & (path[:, 1] < WHITE_END_Y))
    wht = ribbon(git_w * (WHITE_WIDTH / COLLAR_WIDTH), 0.13, white_keep, "dongjeong")
    git.attrs["width"] = git_w
    return git, wht


# ------------------------------------------------------------------------------------- the neck
def make_neck(bd):
    ys = [101.5, 104.0, 106.6, 109.2, 112.6]
    rs = [6.8, 5.9, 5.0, 4.7, 4.4]
    n = 12
    a = -math.pi + 2 * math.pi * np.arange(n) / n
    rings = []
    for y, r in zip(ys, rs):
        rings.append(np.stack([r * np.sin(a), np.full(n, y), HEAD_Z + 0.2 + r * np.cos(a) * 0.95], 1))
    P, T, st, weld = tube(np.array(rings))
    st[:, 0] -= st[:, 0].reshape(len(ys), n + 1)[:, n // 2].repeat(n + 1)
    pc = Piece("neck", P, T, st, weld, attrs={"y": np.repeat(ys, n + 1)})
    orient_outward(pc, lambda p: np.stack([np.zeros(len(p)), p[:, 1], np.full(len(p), HEAD_Z + 0.2)], 1))
    return pc


# ------------------------------------------------------------------------------------- the belt
BELT_Y0, BELT_Y1 = 67.9, 74.8       # the concept's belt, bottom and top edges
BELT_OFF = 0.7                      # off the jeogori's shell
BUCKLE = dict(w=10.0, h=5.3, inner_w=4.8, inner_h=2.2, frame=0.42, lift=0.5, frame_lift=0.28, y=71.35)


def belt_ring(shell, y, off, n=32):
    phis = -math.pi + 2 * math.pi * np.arange(n) / n
    p = shell_point(y, phis)
    A, Bf, Bb, z0 = interp_rows(SHELL_ROWS, y)
    c = np.array([0, y, z0])
    d = p - c
    dn = unit(d * [1.0 / A ** 2 * A, 0, 1.0])
    return p + unit(np.stack([d[:, 0] / A ** 2, np.zeros(n), d[:, 2] / np.where(d[:, 2] >= 0, Bf, Bb) ** 2], 1)) * off


def make_belt(shell):
    n = 32
    ys = [BELT_Y0, BELT_Y1]
    outer = np.array([belt_ring(shell, y, BELT_OFF) for y in ys])
    lo = belt_ring(shell, BELT_Y0, 0.0)[None]
    hi = belt_ring(shell, BELT_Y1, 0.0)[None]
    rings = np.concatenate([lo, outer, hi])
    along = np.concatenate([[-BELT_OFF], np.array(ys) - BELT_Y0, [BELT_Y1 - BELT_Y0 + BELT_OFF]])
    P, T, st, weld = tube(rings, along=along)
    phis = -math.pi + 2 * math.pi * np.arange(n) / n
    st[:, 0] -= st[:, 0].reshape(len(rings), n + 1)[:, n // 2].repeat(n + 1)
    pc = Piece("belt", P, T, st, weld, attrs={"along": np.repeat(along, n + 1)})
    orient_outward(pc, lambda p: np.stack([np.zeros(len(p)), p[:, 1], np.full(len(p), 1.1)], 1))
    return pc


def make_buckle(belt):
    """a plate on the belt's front (its corners rounded) and the inner frame on it"""
    b = BUCKLE
    pieces = []

    def plate(xy_loop, lift, name, inner_loop=None, wall=0.32):
        """a raised plate over the belt: the loop (m,2) of front-view points (x, y) -> its top,
        and walls down to the belt; inner_loop: a hole (a frame)"""
        loops = [np.asarray(xy_loop, float)]
        if inner_loop is not None:
            loops.append(np.asarray(inner_loop, float))
        surf, _, _ = ray_front(belt.P, belt.T, np.vstack(loops))
        return surf
    # the plate: a rounded rectangle, a grid over it
    nx, ny = 9, 5
    xs = np.linspace(-b["w"] / 2, b["w"] / 2, nx)
    ys = np.linspace(b["y"] - b["h"] / 2, b["y"] + b["h"] / 2, ny)
    X, Y = np.meshgrid(xs, ys)
    # round the corners: pull the corner points in
    r = 0.9
    for (i, j) in ((0, 0), (0, nx - 1), (ny - 1, 0), (ny - 1, nx - 1)):
        X[i, j] -= np.sign(X[i, j]) * r * 0.3
        Y[i, j] -= np.sign(Y[i, j] - b["y"]) * r * 0.3
    pts, tri, bary = ray_front(belt.P, belt.T, np.stack([X.ravel(), Y.ravel()], 1))
    nrm = unit(interp_on(belt, tri, bary, belt.N))
    top = pts + nrm * b["lift"]
    # walls: the rim's points at the belt surface
    rim_idx = [(0, j) for j in range(nx)] + [(i, nx - 1) for i in range(1, ny)] + \
              [(ny - 1, j) for j in range(nx - 2, -1, -1)] + [(i, 0) for i in range(ny - 2, 0, -1)]
    grid = top.reshape(ny, nx, 3)
    P = list(grid.reshape(-1, 3))
    st = [(x, y) for y, x in zip(Y.ravel(), X.ravel())]
    T = list(grid_tris(ny, nx))
    base0 = len(P)
    for k, (i, j) in enumerate(rim_idx):
        P.append(pts.reshape(ny, nx, 3)[i, j] - nrm.reshape(ny, nx, 3)[i, j] * 0.05)
        st.append((X[i, j] * 1.0, Y[i, j] - 0.5))
    m = len(rim_idx)
    for k in range(m):
        i, j = rim_idx[k]
        i2, j2 = rim_idx[(k + 1) % m]
        a, bb = i * nx + j, i2 * nx + j2
        c, d = base0 + k, base0 + (k + 1) % m
        T.append((a, c, d))
        T.append((a, d, bb))
    plate = Piece("buckle", np.array(P), np.array(T), np.array(st), attrs={})
    fnrm = np.cross(plate.P[plate.T[:, 1]] - plate.P[plate.T[:, 0]], plate.P[plate.T[:, 2]] - plate.P[plate.T[:, 0]])
    if fnrm[:len(grid_tris(ny, nx)), 2].sum() < 0:
        plate.flip()
    pieces.append(plate)
    # the inner frame: a rectangular ring raised on the plate
    fw = b["frame"]
    w2, h2 = b["inner_w"] / 2, b["inner_h"] / 2
    outer = [(-w2 - fw, -h2 - fw), (w2 + fw, -h2 - fw), (w2 + fw, h2 + fw), (-w2 - fw, h2 + fw)]
    inner = [(-w2, -h2), (w2, -h2), (w2, h2), (-w2, h2)]
    ring_xy = np.array(outer + inner, float) + [0, b["y"]]
    pts2, tri2, bary2 = ray_front(belt.P, belt.T, ring_xy)
    n2 = unit(interp_on(belt, tri2, bary2, belt.N))
    top2 = pts2 + n2 * (b["lift"] + b["frame_lift"])
    low2 = pts2 + n2 * (b["lift"] - 0.02)
    P2 = np.concatenate([top2, low2])
    T2 = []
    for k in range(4):
        o0, o1, i0, i1 = k, (k + 1) % 4, 4 + k, 4 + (k + 1) % 4
        T2 += [(o0, o1, i1), (o0, i1, i0)]                       # the frame's top
        T2 += [(o0 + 8, o1 + 8, o1), (o0 + 8, o1, o0)]           # its outer wall
        T2 += [(i0, i1, i1 + 8), (i0, i1 + 8, i0 + 8)]           # its inner wall
    st2 = np.concatenate([ring_xy, ring_xy - [0, 0.3]])
    frame = Piece("buckle_frame", P2, np.array(T2), st2, attrs={})
    fn2 = np.cross(P2[frame.T[:, 1]] - P2[frame.T[:, 0]], P2[frame.T[:, 2]] - P2[frame.T[:, 0]])
    if fn2[:8, 2].sum() < 0:
        frame.flip()
    pieces.append(frame)
    return pieces


# ----------------------------------------------------------------------------------- the weights
def smooth_weights(piece, W, iters=6, keep=None):
    """Laplacian smoothing of per-vertex weights over the piece's (welded) edges"""
    n = len(piece.P)
    E = np.concatenate([piece.T[:, [0, 1]], piece.T[:, [1, 2]], piece.T[:, [2, 0]]])
    E = piece.weld[E]
    for _ in range(iters):
        acc = np.zeros_like(W)
        cnt = np.zeros(n)
        np.add.at(acc, E[:, 0], W[E[:, 1]])
        np.add.at(acc, E[:, 1], W[E[:, 0]])
        np.add.at(cnt, E[:, 0], 1)
        np.add.at(cnt, E[:, 1], 1)
        nw = 0.5 * W + 0.5 * acc / np.maximum(cnt, 1)[:, None]
        if keep is not None:
            nw[keep] = W[keep]
        W = nw[piece.weld]
    return W


def transfer(bd, Q, nodes, k=6):
    """weights for points Q from the k nearest body vertices among those of `nodes`"""
    sel = bd.vw[:, [bd.idx[n] for n in nodes]].sum(1) > 0.5
    V, VW = bd.vp[sel], bd.vw[sel]
    out = np.zeros((len(Q), bd.n))
    for i0 in range(0, len(Q), 512):
        d = ((Q[i0:i0 + 512, None, :] - V[None]) ** 2).sum(-1)
        j = np.argsort(d, axis=1)[:, :k]
        dj = np.take_along_axis(d, j, 1)
        w = 1.0 / (np.sqrt(dj) + 0.5) ** 2
        w /= w.sum(1, keepdims=True)
        out[i0:i0 + 512] = (VW[j] * w[..., None]).sum(1)
    # only the nodes asked for
    mask = np.zeros(bd.n)
    for n_ in nodes:
        mask[bd.idx[n_]] = 1
    out *= mask
    return out / np.maximum(out.sum(1, keepdims=True), 1e-9)


WEIGHT_STEPS = 8       # weights in eighths: few distinct sets (each a matrix a frame), smooth enough


def limit4(W, steps=WEIGHT_STEPS):
    """at most 4 weights a vertex, in steps of 1/steps summing to 1 (largest remainders up)"""
    W = W.copy()
    order = np.argsort(-W, axis=1)
    for i in range(len(W)):
        W[i, order[i, 4:]] = 0
    W[W < 0.02] = 0
    W /= W.sum(1, keepdims=True)
    out = np.zeros_like(W)
    for i in range(len(W)):
        x = W[i] * steps
        f = np.floor(x)
        rem = int(round(steps - f.sum()))
        if rem > 0:
            f[np.argsort(-(x - f))[:rem]] += 1
        out[i] = f / steps
    return out


def waist_blend(bd, y):
    """the waist's share of the hips (the rest the chest): half at the belt"""
    return 0.5 * smoothstep(77.0, 72.0, y)


def weights_shell(bd, pc):
    W = transfer(bd, pc.P, ["Chest", "Arm_L1", "Arm_R1"], k=8)
    W = smooth_weights(pc, W, iters=8)
    h = waist_blend(bd, pc.P[:, 1])[:, None]
    W = W * (1 - h) + bd.onehot("Hip")[None] * h
    return W


def weights_sleeve(bd, pc, side):
    sfx = "L" if side > 0 else "R"
    # the arm's nodes by the point's place along the sleeve: its centre line's nearest body vertices
    cen = pc.attrs["centre"]
    W = transfer(bd, cen, ["Arm_%s1" % sfx, "Arm_%s2" % sfx, "Wrist_%s" % sfx], k=10)
    t = pc.attrs["t"]
    # the cap: into the chest at the top
    ch = smoothstep(0.16, 0.0, t)[:, None] * 0.55
    W = W * (1 - ch) + bd.onehot("Chest")[None] * ch
    # the wrist's share small, at the cuff only
    wi = bd.idx["Wrist_%s" % sfx]
    W[:, wi] = np.minimum(W[:, wi], 0.25 * smoothstep(0.8, 1.0, t))
    W /= W.sum(1, keepdims=True)
    W = smooth_weights(pc, W, iters=6)
    return W


def weights_skirt(bd, pc):
    P = pc.P
    y = P[:, 1]
    t = smoothstep(64.0, 31.0, y)          # the legs' share
    k = smoothstep(36.0, 21.0, y)          # of which the shins'
    A = interp_rows(SKIRT_ROWS, y)[0] if np.ndim(y) == 0 else np.array([interp_rows(SKIRT_ROWS, yy)[0] for yy in y])
    a = smoothstep(-0.62, 0.62, P[:, 0] / A)   # the left leg's share by side
    W = np.zeros((len(P), bd.n))
    h = waist_blend(bd, y)
    W[:, bd.idx["Hip"]] = (1 - t) * (1 - h)
    W[:, bd.idx["Chest"]] = (1 - t) * h
    W[:, bd.idx["Foot_L1"]] = t * a * (1 - k)
    W[:, bd.idx["Foot_L2"]] = t * a * k
    W[:, bd.idx["Foot_R1"]] = t * (1 - a) * (1 - k)
    W[:, bd.idx["Foot_R2"]] = t * (1 - a) * k
    return W


def weights_neck(bd, pc):
    y = pc.P[:, 1]
    hd = smoothstep(105.5, 110.0, y)[:, None]
    return bd.onehot("Chest")[None] * (1 - hd) + bd.onehot("Head_Only")[None] * hd


def weights_from(surface_pieces, Q):
    """the weights of the closest point on any of the surfaces"""
    best = None
    for s in surface_pieces:
        p, t, b, d = closest_on_mesh(s.P, s.T, Q)
        w = interp_on(s, t, b, s.W)
        if best is None:
            best = (d, w)
        else:
            m = d < best[0]
            best = (np.where(m, d, best[0]), np.where(m[:, None], w, best[1]))
    return best[1]


# ----------------------------------------------------------------------------------- the atlas
ATLAS_SIZE = 1024
ISLAND_PAD = 10         # texels between islands (the lower levels need room to bleed)


def layout_atlas(pieces):
    """the densest even layout that fits: shelf-packs each piece's st rectangle at the density,
    sets piece.uv, returns (layout {name: (x0, y0, w, h) texels}, density texels a unit)"""
    lo, hi = 2.0, 12.0
    for _ in range(18):
        mid = 0.5 * (lo + hi)
        if _pack(pieces, mid) is None:
            hi = mid
        else:
            lo = mid
    D = math.floor(lo * 20) / 20
    return _pack(pieces, D, assign=True), D


def _pack(pieces, D, assign=False):
    rects = []
    for pc in pieces:
        dens = D * pc.attrs.get("density", 1.0)
        s0, t0 = pc.st.min(0)
        s1, t1 = pc.st.max(0)
        w = (s1 - s0) * dens
        h = (t1 - t0) * dens
        rects.append([pc, s0, t0, w, h, dens])
    order = sorted(range(len(rects)), key=lambda i: -rects[i][4])
    x, y, shelf_h = ISLAND_PAD, ISLAND_PAD, 0
    layout = {}
    for i in order:
        pc, s0, t0, w, h, dens = rects[i]
        wi, hi = int(math.ceil(w)) + 1, int(math.ceil(h)) + 1
        if x + wi + ISLAND_PAD > ATLAS_SIZE:
            x = ISLAND_PAD
            y += shelf_h + ISLAND_PAD
            shelf_h = 0
        layout[pc.name] = (x, y, wi, hi)
        if assign:
            # texture v down; t (along) grows upward for the vertical pieces: flip so up is up
            u = (x + 0.5 + (pc.st[:, 0] - s0) * dens) / ATLAS_SIZE
            v = (y + 0.5 + (t1_of(pc) - pc.st[:, 1]) * dens) / ATLAS_SIZE
            pc.uv = np.stack([u, v], 1)
            pc.attrs["island"] = (x, y, wi, hi, s0, t1_of(pc), dens)
        x += wi + ISLAND_PAD
        shelf_h = max(shelf_h, hi)
    if y + shelf_h + ISLAND_PAD > ATLAS_SIZE:
        return None
    return layout


def t1_of(pc):
    return pc.st[:, 1].max()


# ------------------------------------------------------------------------------------- building
def build_pieces(bd):
    shell = make_shell()
    shell.normals()
    sl, ll, rl = make_sleeve(bd, +1)
    sr, lr, rr = make_sleeve(bd, -1)
    skirt, sk_lining, sk_rim = make_skirt()
    neck = make_neck(bd)
    belt = make_belt(shell)
    belt.normals()
    buckle = make_buckle(belt)
    git, white = make_collar(shell, bd)
    pieces = [shell, sl, sr, ll, lr, rl, rr, skirt, sk_lining, sk_rim, neck, belt] + buckle + [git, white]
    for pc in pieces:
        pc.normals()
    # weights
    shell.W = weights_shell(bd, shell)
    sl.W = weights_sleeve(bd, sl, +1)
    sr.W = weights_sleeve(bd, sr, -1)
    for lin, sv in ((ll, sl), (rl, sl), (lr, sr), (rr, sr)):
        lin.W = weights_from([sv], lin.P)
    skirt.W = weights_skirt(bd, skirt)
    sk_lining.W = weights_skirt(bd, sk_lining)
    sk_rim.W = weights_skirt(bd, sk_rim)
    neck.W = weights_neck(bd, neck)
    belt.W = weights_from([shell, skirt], belt.P)
    for b in buckle:
        b.W = weights_from([belt], b.P)
    git.W = weights_from([shell], git.P)
    white.W = weights_from([shell], white.P)
    for pc in pieces:
        pc.W = limit4(pc.W)
    # texture density: the linings and the rims and the inside of things at a lower one
    for pc in pieces:
        if pc.name.startswith(("lining", "skirt_lining")):
            pc.attrs["density"] = 0.35
        if pc.name.startswith(("cuffrim", "skirt_rim")):
            pc.attrs["density"] = 1.0
    return pieces


def inverse_skin(bd, pieces):
    """each piece's bind-pose positions and normals from the modelling pose"""
    for pc in pieces:
        M = body.blend_matrices(pc.W, bd.S)                 # (n,4,4) Wait-pose skinning
        Minv = np.linalg.inv(M)
        pc.Pb = np.einsum("nij,nj->ni", Minv[:, :3, :3], pc.P) + Minv[:, :3, 3]
        # normals: n_wait = M^-T n_bind, so n_bind = M^T n_wait
        pc.Nb = unit(np.einsum("nji,nj->ni", M[:, :3, :3], pc.N))


def build():
    bd = Body()
    pieces = build_pieces(bd)
    inverse_skin(bd, pieces)
    layout, density = layout_atlas(pieces)
    return bd, pieces, (layout, density)


# the bones a vertex may use, by the Mii's node names
def used_bones(bd, pieces):
    used = np.zeros(bd.n, bool)
    for pc in pieces:
        used |= (pc.W > 0).any(0)
    return [bd.names[i] for i in range(bd.n) if used[i]]


def write_mesh(bd, pieces, layout_d, path=MESH_FILE):
    layout, density = layout_d
    bones = used_bones(bd, pieces)
    bi = {nm: k for k, nm in enumerate(bones)}
    inv_bind = []
    for nm in bones:
        B = bd.Wb[bd.idx[nm]]
        inv_bind.append([round(float(x), 6) for x in np.linalg.inv(B)[:3].ravel()])
    out = {"format": "midoha_outfit_mesh 1", "units": "Wii no Ma Mii units, bind pose (T-pose)",
           "atlas": {"size": ATLAS_SIZE, "density": density, "islands": {k: list(v) for k, v in layout.items()}},
           "bones": bones, "inverse_bind": inv_bind, "pieces": []}
    for pc in pieces:
        verts = []
        for i in range(len(pc.P)):
            ws = [(bi[bd.names[j]], round(float(pc.W[i, j]), 5)) for j in np.nonzero(pc.W[i] > 0)[0]]
            ws.sort(key=lambda x: -x[1])
            verts.append([[round(float(c), 4) for c in pc.Pb[i]], [round(float(c), 4) for c in pc.Nb[i]],
                          [round(float(c), 6) for c in pc.uv[i]], ws])
        out["pieces"].append({"name": pc.name, "vertices": verts, "triangles": pc.T.tolist()})
    with open(path, "w") as f:
        json.dump(out, f, separators=(",", ":"))
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("what", choices=["build", "stats"])
    ap.add_argument("--out", default=MESH_FILE)
    a = ap.parse_args()
    bd, pieces, layout = build()
    tris = sum(len(pc.T) for pc in pieces)
    verts = sum(len(pc.P) for pc in pieces)
    for pc in pieces:
        print("%-14s %5d vertices %5d triangles  island %s" % (pc.name, len(pc.P), len(pc.T), layout[0].get(pc.name)))
    print("total %d vertices, %d triangles; atlas %d at %.2f texels a unit; bones %s" % (
        verts, tris, ATLAS_SIZE, layout[1], used_bones(bd, pieces)))
    if a.what == "build":
        print(write_mesh(bd, pieces, layout, a.out))


if __name__ == "__main__":
    main()
