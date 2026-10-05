"""The machinery under ミドハ's head pieces (tools/mii/midoha_parts.py): smooth parametric pieces, meshed
on a grid, each with its own region of one texture atlas into which its shading is baked (painted):
the texture is evaluated on the same surface, texel by texel, so the painting follows the piece's
shape exactly; 3 x 3 samples a texel make its edges clean. Everything here is ours.

    Piece(name, fn, s_knots, t_knots, texels, shade)   fn(s, t) -> positions (..., 3) for s, t in 0..1
    Atlas(size).place(pieces)                          a shelf packing of their regions
    atlas.bake(pieces)                                 the texture (size x size RGBA8)
    gx_levels(image, fmt)                              GX RGB565 (4) or RGBA8 (6) bytes, the mip chain

A piece's grid: s across (or round), t along; its mesh shares the grid's vertices (so smooth), its
texture region maps s, t linearly (a texel's centre at the region's edge texels: no seam bleeds).
"""
import math

import numpy as np


def smoothstep(e0, e1, x):
    t = np.clip((np.asarray(x, float) - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def unit(v):
    v = np.asarray(v, float)
    n = np.linalg.norm(v, axis=-1, keepdims=True)
    return v / np.maximum(n, 1e-12)


def catmull(points, n=200):
    """a Catmull-Rom curve through `points`, n samples evenly spaced by arc length"""
    P = [np.asarray(p, float) for p in points]
    P = [2 * P[0] - P[1]] + P + [2 * P[-1] - P[-2]]
    dense = []
    for i in range(len(P) - 3):
        p0, p1, p2, p3 = P[i:i + 4]
        for k in range(40):
            t = k / 40.0
            dense.append(0.5 * (2 * p1 + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t
                                + (-p0 + 3 * p1 - 3 * p2 + p3) * t ** 3))
    dense.append(P[-2])
    dense = np.array(dense)
    seg = np.linalg.norm(np.diff(dense, axis=0), axis=1)
    L = np.concatenate([[0.0], np.cumsum(seg)])
    want = np.linspace(0.0, L[-1], n)
    return np.stack([np.interp(want, L, dense[:, k]) for k in range(3)], -1), L[-1]


def profile(values, t):
    """a profile given at evenly spaced points (smooth: Catmull-Rom between them), at t (0..1)"""
    v = np.asarray(values, float)
    x = np.asarray(t, float) * (len(v) - 1)
    i = np.clip(np.floor(x).astype(int), 0, len(v) - 2)
    f = x - i
    p0 = v[np.maximum(i - 1, 0)]
    p1, p2 = v[i], v[i + 1]
    p3 = v[np.minimum(i + 2, len(v) - 1)]
    return 0.5 * (2 * p1 + (-p0 + p2) * f + (2 * p0 - 5 * p1 + 4 * p2 - p3) * f * f + (-p0 + 3 * p1 - 3 * p2 + p3) * f ** 3)


class Spine:
    """a curve with a frame along it: T (tangent), N (toward `out`), B = T x N; t by arc length"""

    def __init__(self, points, out, n=240):
        self.C, self.length = catmull(points, n)
        T = np.gradient(self.C, axis=0)
        self.T = unit(T)
        o = np.asarray(out(self.C) if callable(out) else np.broadcast_to(np.asarray(out, float), self.C.shape), float)
        N = o - self.T * (o * self.T).sum(-1, keepdims=True)
        self.N = unit(N)
        self.B = np.cross(self.T, self.N)
        self.n = n

    def at(self, t):
        """C, T, N, B at t (any shape)"""
        x = np.clip(np.asarray(t, float), 0.0, 1.0) * (self.n - 1)
        i = np.clip(np.floor(x).astype(int), 0, self.n - 2)
        f = (x - i)[..., None]

        def lerp(a):
            return a[i] * (1 - f) + a[i + 1] * f
        return lerp(self.C), unit(lerp(self.T)), unit(lerp(self.N)), unit(lerp(self.B))


class Piece:
    """fn(s, t) -> (..., 3); shade(P, N, s, t) -> RGB (..., 3) 0..255 (alpha 255); texels (w, h) of its
    region; s_knots, t_knots its grid (s across / round, t along)"""

    def __init__(self, name, fn, s_knots, t_knots, texels, shade, flip=False, colour=None):
        self.name, self.fn, self.shade = name, fn, shade
        self.s = np.asarray(s_knots, float)
        self.t = np.asarray(t_knots, float)
        self.texels = texels
        self.flip = flip           # the normals' sign (outward)
        self.region = None         # (x, y, w, h) in the atlas, texels
        self.colour = colour       # a vertex colour (RGB) for all, else white

    def normals(self, s, t, eps=1e-3):
        s0, s1 = np.clip(s - eps, 0, 1), np.clip(s + eps, 0, 1)
        t0, t1 = np.clip(t - eps, 0, 1), np.clip(t + eps, 0, 1)
        ds = self.fn(s1, t) - self.fn(s0, t)
        dt = self.fn(s, t1) - self.fn(s, t0)
        n = np.cross(dt, ds)
        if self.flip:
            n = -n
        return unit(n)

    def mesh(self, detail=1.0):
        """vertices (n, 3), normals, (s, t) (n, 2), triangles (m, 3) -- counter-clockwise seen from the
        normals' side; detail < 1: fewer of the grid's knots (the same distribution, every so many)"""
        def fewer(k):
            if detail >= 1.0:
                return k
            n = max(3, int(math.ceil(len(k) * detail)))
            return k[np.round(np.linspace(0, len(k) - 1, n)).astype(int)]
        S, T = np.meshgrid(fewer(self.s), fewer(self.t), indexing="ij")  # (ns, nt)
        V = self.fn(S, T).reshape(-1, 3)
        N = self.normals(S, T).reshape(-1, 3)
        ST = np.stack([S.reshape(-1), T.reshape(-1)], -1)
        ns, nt = S.shape
        F = []
        for i in range(ns - 1):
            for j in range(nt - 1):
                a, b, c, d = i * nt + j, (i + 1) * nt + j, (i + 1) * nt + j + 1, i * nt + j + 1
                # (normal = dP/dt x dP/ds: a -> d -> c is counter-clockwise about it)
                tri = [(a, d, c), (a, c, b)] if not self.flip else [(a, c, d), (a, b, c)]
                for x in tri:
                    if len({tuple(np.round(V[k], 5)) for k in x}) == 3:  # (degenerate at points: left out)
                        F.append(x)
        return V, N, ST, np.array(F, int)

    def uv(self, ST, atlas):
        x, y, w, h = self.region
        # texel centres: s = 0 at the first texel's centre, 1 at the last's (the padding beyond)
        pad = Atlas.PAD
        u = (x + pad + 0.5 + ST[:, 0] * (w - 2 * pad - 1)) / atlas.w
        v = (y + pad + 0.5 + ST[:, 1] * (h - 2 * pad - 1)) / atlas.h
        return np.stack([u, v], -1)


class Atlas:
    PAD = 3  # texels round each region (copies of its edge)
    SS = 3   # samples a texel, each way

    def __init__(self, w, h=None):
        self.w, self.h = w, h or w

    def place(self, pieces):
        """a skyline packing (each region where it lands lowest, then leftmost), tallest first; returns
        the rows used"""
        sky = np.zeros(self.w, int)
        for p in sorted(pieces, key=lambda p: (-p.texels[1], -p.texels[0])):
            w, h = p.texels[0] + 2 * self.PAD, p.texels[1] + 2 * self.PAD
            best = None
            for x in range(0, self.w - w + 1, 2):
                y = sky[x:x + w].max()
                if y + h <= self.h and (best is None or y < best[1]):
                    best = (x, y)
            if best is None:
                raise ValueError("the atlas (%dx%d) is full at %s" % (self.w, self.h, p.name))
            x, y = best
            p.region = (x, y, w, h)
            sky[x:x + w] = y + h
        return int(sky.max())

    def bake(self, pieces, background=(30, 22, 31)):
        img = np.zeros((self.h, self.w, 3), float)
        img[:] = background
        for p in pieces:
            x, y, w, h = p.region
            pad, ss = self.PAD, self.SS
            iw, ih = w - 2 * pad - 1, h - 2 * pad - 1
            # sample positions in texels from the region's first inner texel centre
            jx = (np.arange(w * ss) + 0.5) / ss - 0.5 - pad
            jy = (np.arange(h * ss) + 0.5) / ss - 0.5 - pad
            s = np.clip(jx / max(iw, 1), 0.0, 1.0)
            t = np.clip(jy / max(ih, 1), 0.0, 1.0)
            S, T = np.meshgrid(s, t)  # (h ss, w ss)
            P = p.fn(S, T)
            N = p.normals(S, T)
            rgb = np.asarray(p.shade(P, N, S, T), float)
            rgb = rgb.reshape(h, ss, w, ss, 3).mean((1, 3))
            img[y:y + h, x:x + w] = rgb
        return np.clip(np.round(img), 0, 255).astype(np.uint8)


def mip_chain(img, smallest=8):
    levels = [img.astype(float)]
    while min(levels[-1].shape[:2]) > smallest:
        a = levels[-1]
        h, w = a.shape[0] // 2, a.shape[1] // 2
        levels.append(a.reshape(h, 2, w, 2, -1).mean((1, 3)))
    return [np.clip(np.round(l), 0, 255).astype(np.uint8) for l in levels]


def gx_rgba8(img):
    """(h, w, 3|4) uint8 -> GX RGBA8: 4x4 tiles, each 32 bytes of A R pairs then 32 of G B pairs"""
    h, w = img.shape[:2]
    if img.shape[2] == 3:
        img = np.concatenate([img, np.full((h, w, 1), 255, np.uint8)], -1)
    t = img.reshape(h // 4, 4, w // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4)
    ar = t[:, :, [3, 0]].reshape(-1, 32)
    gb = t[:, :, [1, 2]].reshape(-1, 32)
    return np.concatenate([ar, gb], 1).astype(np.uint8).tobytes()


def gx_rgb565(img):
    """(h, w, 3) uint8 -> GX RGB565: 4x4 tiles, big-endian texels"""
    h, w = img.shape[:2]
    a = img[..., :3].astype(np.int32)
    r = (a[..., 0] * 31 + 127) // 255
    g = (a[..., 1] * 63 + 127) // 255
    b = (a[..., 2] * 31 + 127) // 255
    v = (r << 11) | (g << 5) | b
    v = v.reshape(h // 4, 4, w // 4, 4).transpose(0, 2, 1, 3).reshape(-1)
    return v.astype(">u2").tobytes()


def gx_levels(img, fmt=4, smallest=8):
    """the mip chain in GX format fmt (4 RGB565, 6 RGBA8), concatenated; (bytes, levels)"""
    levels = mip_chain(img, smallest)
    enc = gx_rgb565 if fmt == 4 else gx_rgba8
    return b"".join(enc(l) for l in levels), len(levels)
