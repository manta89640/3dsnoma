#!/usr/bin/env python3
"""Check ミドハ's hair against the head it covers: RFL's own shapes, read from your RFL_Res.dat.

    tools/mii/midoha_check.py RFL_Res.dat [--face 0] [--hair 30]

His Mii is bald (hair 30: RFL's skull alone, the "forehead" shape, skin-coloured); his hair
(tools/mii/midoha_parts.py) is a shell over a superellipsoid fitted to that skull. This casts rays
from the fit's centre C in every direction the shell covers (above its hairline, 1 degree apart),
and reports how far the shell stands outside the skull along each: the least of it is the margin
against the skull showing through (it must stay above 0), and where it is. Nothing is written; the
file is read only (it is Nintendo's: keep it out of the repository).

RFL_Res.dat: a header of 18 big-endian section offsets at +4; a section is u16 count, u16 max, then
count + 1 u32 entry offsets (from after them); a shape entry: a 4-byte tag, for faces 9 f32 (the
hair's offset at [6:9]), u16 vertex count and s16 x3 positions (/ 256), u16 normal count and s16 x3
normals, then u8 primitive count and per primitive u8 vertex count, u8 GX opcode, and per vertex
u8 position and normal indices (shapes with texture coordinates have u16 count, s16 x2 coordinates
before the primitives, and a third index).
"""
import argparse
import math
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import midoha_parts as mp  # noqa: E402

SECTION_TAGS = (b"face", b"frhd", b"hair")


def sections(data):
    offs = struct.unpack(">18I", data[4:76])
    out = {}
    for o in offs:
        n = struct.unpack(">H", data[o:o + 2])[0]
        eo = struct.unpack(">%dI" % (n + 1), data[o + 4:o + 8 + 4 * n])
        base = o + 4 + 4 * (n + 1)
        es = [data[base + eo[i]:base + eo[i + 1]] for i in range(n)]
        if es and es[0][:4] in SECTION_TAGS:
            out[es[0][:4].decode()] = es
    return out


def prims(e, p, nidx):
    if p >= len(e):
        return None
    count = e[p]
    p += 1
    out = []
    for _ in range(count):
        if p + 2 > len(e):
            return None
        n, op = e[p], e[p + 1]
        p += 2
        if op not in (0x80, 0x90, 0x98, 0xA0) or p + n * nidx > len(e):
            return None
        out.append((op, [e[p + k * nidx] for k in range(n)]))
        p += n * nidx
    return out if p == len(e) else None


def shape(e):
    """(positions (n, 3), triangles (m, 3), the hair offset or None)"""
    p, offset = 4, None
    if e[:4] == b"face":
        offset = np.array(struct.unpack(">9f", e[4:40])[6:9])
        p = 40
    n = struct.unpack(">H", e[p:p + 2])[0]
    if n == 0:
        return None
    p += 2
    pos = np.array(struct.unpack(">%dh" % (3 * n), e[p:p + 6 * n]), float).reshape(n, 3) / 256.0
    p += 6 * n
    nn = struct.unpack(">H", e[p:p + 2])[0]
    p += 2 + 6 * nn
    pr = prims(e, p, 2)
    if pr is None:
        nt = struct.unpack(">H", e[p:p + 2])[0]
        q = p + 2 + 4 * nt
        pr = prims(e, q, 3) or prims(e, q, 2)
    if pr is None:
        return None
    tris = []
    for op, vs in pr:
        if op == 0x90:
            tris += [vs[i:i + 3] for i in range(0, len(vs) - 2, 3)]
        elif op == 0x98:
            tris += [(vs[i], vs[i + 1], vs[i + 2]) if i % 2 == 0 else (vs[i + 1], vs[i], vs[i + 2])
                     for i in range(len(vs) - 2)]
        elif op == 0xA0:
            tris += [(vs[0], vs[i], vs[i + 1]) for i in range(1, len(vs) - 1)]
    return pos, np.array(tris, int), offset


def furthest_hit(V, T, origin, d):
    A, B, Cc = V[T[:, 0]], V[T[:, 1]], V[T[:, 2]]
    e1, e2 = B - A, Cc - A
    pv = np.cross(d, e2)
    det = (e1 * pv).sum(1)
    ok = np.abs(det) > 1e-9
    inv = np.where(ok, 1 / np.where(ok, det, 1), 0)
    s = origin - A
    u = (s * pv).sum(1) * inv
    q = np.cross(s, e1)
    v = (q * d).sum(1) * inv
    t = (e2 * q).sum(1) * inv
    hit = ok & (u >= -1e-6) & (v >= -1e-6) & (u + v <= 1 + 1e-6) & (t > 0)
    return t[hit].max() if hit.any() else 0.0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("rfl_res", help="your RFL_Res.dat (the Mii channel's face library resources)")
    ap.add_argument("--face", type=int, default=0)
    ap.add_argument("--hair", type=int, default=30)
    a = ap.parse_args()
    sec = sections(open(a.rfl_res, "rb").read())
    _, _, offset = shape(sec["face"][a.face])
    skull = shape(sec["frhd"][a.hair])
    V, T = skull[0] + offset, skull[1]
    worst, where, n = 1e9, None, 0
    for j in range(360):
        phi = -180.0 + j
        top = mp.theta_at_height(phi, mp.hairline_y(phi, 1), mp.thickness)
        edge_t = lambda th, ph: mp.thickness(th, ph, top)  # noqa: E731
        th_edge = mp.theta_at_height(phi, mp.hairline_y(phi, 1), edge_t)
        for th in np.arange(0.5, th_edge - 1.0, 1.0):
            d = mp.direction(th, phi)
            r_skull = furthest_hit(V, T, mp.C, d)
            if r_skull <= 0:
                continue
            margin = mp.skull_radius(d) + edge_t(th, phi) - r_skull
            n += 1
            if margin < worst:
                worst, where = margin, (th, phi)
    print("%d directions under the hair: the shell stands at least %.2f units outside the skull "
          "(at theta %.1f, phi %.1f)%s" % (n, worst, where[0], where[1], "" if worst > 0 else ": SKIN SHOWS"))
    return 0 if worst > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
