#!/usr/bin/env python3
"""What a frame of a package submits, counted: a Python copy of engine/g3d/world.cpp's culling and
packet building (World::Impl::cull and build: cells by their box against the frustum and their
level by its distance, instances by their sphere, cull distance and model chain, the sea's batches
by their boxes, the sky's every batch), with the rules a version 2 World adds switchable, to see
what the package gives before the renderer does it.

    viewstats.py PACKAGE [--v1] [--detail] [--batchcull] [--cvis] [--cams FILE] [--no-views]
                         [--breakdown]

Cameras: the package's view_* markers as NOMA_SELFTEST=g3d frames them (fovy 45, aspect 400:240,
near 1.5, far 80,000; without the switches its numbers are the selftest's), and/or a file of
"name ex ey ez tx ty tz [fovy [aspect]]" lines (tools/wuhu/walk_cams.txt: the speed round's 144
walking frames). Per camera: cells and instances in view, draws
(opaque, translucent), triangles; the means at the end with the vertices the draws index (each
draw's distinct ones: what the vertex shader runs on), those of materials still lit at run time,
and their bytes. The switches:
  --detail     BATCH_DETAIL batches farther than Env.detail_distance (to their box) left out
  --batchcull  each cell batch culled by its own box as well as its cell's
  --cvis       a cell whose bit is clear in the row of the eye's cell left out, when the eye is
               no higher than Env.pvs_max_height over the ground under it (the highest collision
               surface below it that faces up, not an invisible fence: the follow camera's floor)
  --v1         the package is version 1 (read with version 1's records)
  --breakdown  triangles and draws a frame by kind (the island's cells: the island, its far copy,
               the sea, trees, props; instances by model)
"""
import argparse
import collections
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "wuhu"))
sys.path.insert(0, os.path.dirname(HERE))
import ng3d  # noqa: E402
from ng3d import NONE16  # noqa: E402


def frustum(eye, target, fovy, aspect, near, far):
    """World's frustum planes (n, d): n.x + d >= 0 inside."""
    f = np.asarray(target, np.float64) - eye
    f /= np.linalg.norm(f)
    r = np.cross(f, [0, 1, 0])
    r /= np.linalg.norm(r)
    u = np.cross(r, f)
    tv = math.tan(math.radians(fovy) / 2)
    th = tv * aspect

    def plane(n, p):
        n = n / np.linalg.norm(n)
        return n, -float(n @ p)
    return [plane(f, eye + f * near), plane(-f, eye + f * far), plane(r + f * th, eye), plane(-r + f * th, eye),
            plane(u + f * tv, eye), plane(-u + f * tv, eye)]


def box_visible(fr, b):
    for n, d in fr:
        if n @ np.where(n >= 0, b[3:], b[:3]) + d < 0:
            return False
    return True


def sphere_visible(fr, c, r):
    return all(n @ c + d >= -r for n, d in fr)


def box_distance(b, p):
    return float(np.linalg.norm(np.maximum(np.maximum(b[:3] - p, p - b[3:]), 0)))


def transform_box(M, b):
    lo, hi = M[:3, 3].copy(), M[:3, 3].copy()
    for i in range(3):
        for j in range(3):
            a, c = M[i, j] * b[j], M[i, j] * b[3 + j]
            lo[i] += min(a, c)
            hi[i] += max(a, c)
    return np.concatenate([lo, hi])


def kcl_of(pkg):
    """The package's collision triangles, attributes and normals (tools/wuhu/stage_wuhu.py's reader)."""
    import stage_wuhu as S
    tris, attrs, normals = [], [], []
    for blob in pkg.blobs:
        t, a, n = S.kcl_triangles(blob)
        ok = np.isfinite(t.reshape(len(t), -1)).all(1)
        tris.append(t[ok]), attrs.append(a[ok]), normals.append(n[ok])
    return np.concatenate(tris), np.concatenate(attrs), np.concatenate(normals)


class Stats:
    def __init__(self, pkg, kcl=None, v1=False):
        self.p, self.kcl, self.v1 = pkg, kcl, v1
        self.cb = [np.array(c.bbox, np.float64) for c in pkg.cells]
        self.bb = [np.array(b.bbox, np.float64) for b in pkg.batches]
        self.xlu = [bool(m.flags & ng3d.MAT_XLU) for m in pkg.materials]
        self.cull3 = [m.cull == 3 for m in pkg.materials]
        self.tris = [b.n_indices // 3 for b in pkg.batches]
        idx = np.array(pkg.indices, np.int64)
        self.verts = [len(np.unique(idx[b.first_index:b.first_index + b.n_indices])) for b in pkg.batches]
        self.vbytes = [self.verts[i] * pkg.vertex_buffers[b.vbuf].stride for i, b in enumerate(pkg.batches)]
        self.lit = [bool((pkg.materials[b.material].chan_color | pkg.materials[b.material].chan_alpha) & 2)
                    for b in pkg.batches]
        self.cats, self.cat_items = collections.Counter(), collections.Counter()
        self._nw = {}

    def ground(self, x, z, y):
        """The highest floor at or below y under (x, z): facing up, not an invisible fence."""
        import stage_wuhu as S
        T, A, N = self.kcl
        ys, attrs, ny = S.kcl_surfaces(T, A, N, x, z)
        ok = (ys <= y + 1e-3) & (ny > 0.05) & ~np.isin(attrs >> 8, (0x00, 0x08))
        return float(ys[ok][0]) if ok.any() else None

    def node_worlds(self, mi):
        """A model's nodes at rest (the sea's and detail batches' boxes: World poses instances)."""
        if mi not in self._nw:
            m = self.p.models[mi]
            W = []
            for k in range(max(m.n_nodes, 1)):
                M = np.eye(4)
                if k < m.n_nodes:
                    M[:3] = np.array(self.p.nodes[m.first_node + k].bind)
                W.append(M)
            self._nw[mi] = W
        return self._nw[mi]

    def count(self, b, where):
        nm = self.p.string(self.p.materials[self.p.batches[b].material].name)
        model = nm.split("/")[0]
        if where.startswith("cell"):
            kind = {"WS2_common_island": "island", "WS2_common_island_V": "island", "WS2_common_island_R": "far island",
                    "WS2_common_sea": "sea"}.get(model, "tree" if "tree" in model else "prop")
            key = "%s %s%s" % (where, kind, " (detail)" if self.p.batches[b].flags & ng3d.BATCH_DETAIL else "")
        else:
            key = where
        self.cats[key] += self.tris[b]
        self.cat_items[key] += 1
        self.fv += np.array([self.verts[b], self.verts[b] * self.lit[b], self.vbytes[b]])

    def frame(self, eye, target, fovy=45.0, aspect=400 / 240, near=1.5, far=80000.0, detail=False, cvis=False,
              batchcull=False):
        p, e = self.p, self.p.env
        eye = np.asarray(eye, np.float64)
        fr = frustum(eye, target, fovy, aspect, near, far)
        row = None
        if cvis and not self.v1 and e.pvs_row_bytes:
            ix = int(math.floor((eye[0] - e.grid_origin[0]) / e.cell_size))
            iz = int(math.floor((eye[2] - e.grid_origin[1]) / e.cell_size))
            g = self.ground(eye[0], eye[2], eye[1])
            if 0 <= ix < e.n_cells_x and 0 <= iz < e.n_cells_z and g is not None and eye[1] - g <= e.pvs_max_height:
                i = iz * e.n_cells_x + ix
                row = p.cvis[i * e.pvs_row_bytes:(i + 1) * e.pvs_row_bytes]
        dd = e.detail_distance if detail and not self.v1 else 0.0
        cells = []
        for i, c in enumerate(p.cells):
            if not box_visible(fr, self.cb[i]) or (row is not None and not row[i >> 3] >> (i & 7) & 1):
                continue
            d = box_distance(self.cb[i], eye)
            lod = 0
            while lod + 1 < c.n_lods and d >= c.lod_distance[lod + 1]:
                lod += 1
            cells.append((i, lod))
        insts = []
        for inst in p.instances:
            m = p.models[inst.model]
            M = np.eye(4)
            M[:3] = np.array(inst.mtx)
            bb = np.array(m.bbox)
            cen = M[:3, :3] @ ((bb[:3] + bb[3:]) / 2) + M[:3, 3]
            rad = m.radius * max(np.linalg.norm(M[:3, j]) for j in range(3))
            if not sphere_visible(fr, cen, rad):
                continue
            d = float(np.linalg.norm(cen - eye))
            if inst.cull_distance > 0 and max(d - rad, 0.0) > inst.cull_distance:
                continue
            mi = inst.model
            for _ in range(len(p.models)):
                if p.models[mi].lod_next == NONE16 or d < p.models[mi].lod_distance:
                    break
                mi = p.models[mi].lod_next
            insts.append((mi, M))
        out = dict(cells=len(cells), instances=len(insts))
        self.fv = np.zeros(3)
        for pas in ("opaque", "translucent"):
            items = tris = 0
            want_xlu = pas == "translucent"
            if pas == "opaque" and e.sky_model != NONE16:
                m = p.models[e.sky_model]
                for b in range(m.first_batch, m.first_batch + m.n_batches):
                    if not self.cull3[p.batches[b].material]:
                        items, tris = items + 1, tris + self.tris[b]
                        self.count(b, "sky")
            for ci, lod in cells:
                c = p.cells[ci]
                for k in range(c.first[lod], c.first[lod] + c.count[lod]):
                    b = p.cell_batches[k]
                    bt = p.batches[b]
                    if self.xlu[bt.material] != want_xlu or self.cull3[bt.material]:
                        continue
                    if dd > 0 and bt.flags & ng3d.BATCH_DETAIL and box_distance(self.bb[b], eye) > dd:
                        continue
                    if batchcull and not box_visible(fr, self.bb[b]):
                        continue
                    items, tris = items + 1, tris + self.tris[b]
                    self.count(b, "cell L%d" % lod)
            models = [(mi, M, False) for mi, M in insts]
            if e.sea_model != NONE16:
                models.append((e.sea_model, np.eye(4), True))
            for mi, M, sea in models:
                m = p.models[mi]
                W = self.node_worlds(mi)
                for b in range(m.first_batch, m.first_batch + m.n_batches):
                    bt = p.batches[b]
                    if self.xlu[bt.material] != want_xlu or self.cull3[bt.material]:
                        continue
                    if sea or (dd > 0 and bt.flags & ng3d.BATCH_DETAIL):
                        box = transform_box(M @ W[bt.node if bt.node < m.n_nodes else 0], self.bb[b])
                        if (sea and not box_visible(fr, box)) or \
                                (dd > 0 and bt.flags & ng3d.BATCH_DETAIL and box_distance(box, eye) > dd):
                            continue
                    items, tris = items + 1, tris + self.tris[b]
                    self.count(b, "the open sea" if sea else "instance " + p.string(m.name).split("#")[0])
            out["items_" + pas], out["tris_" + pas] = items, tris
        out["items"] = out["items_opaque"] + out["items_translucent"]
        out["tris"] = out["tris_opaque"] + out["tris_translucent"]
        out["verts"], out["lit_verts"], out["vbytes"] = (float(x) for x in self.fv)
        return out


def read_v1(path):
    """A version 1 package, read with version 1's records (VertexBuffer of 20 bytes, Env's
    reserved[8])."""
    old = dict(VertexBuffer=ng3d.STRUCTS["VertexBuffer"], Env=ng3d.STRUCTS["Env"])
    ng3d.STRUCTS["VertexBuffer"] = old["VertexBuffer"][:7]
    ng3d.STRUCTS["Env"] = old["Env"][:13] + [("reserved", "u32", (8,))]
    ver, ng3d.VERSION = ng3d.VERSION, 1
    try:
        return ng3d.Package.read(open(path, "rb").read())
    finally:
        ng3d.STRUCTS.update(old)
        ng3d.VERSION = ver


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("package")
    ap.add_argument("--v1", action="store_true")
    ap.add_argument("--detail", action="store_true")
    ap.add_argument("--batchcull", action="store_true")
    ap.add_argument("--cvis", action="store_true")
    ap.add_argument("--cams")
    ap.add_argument("--no-views", action="store_true")
    ap.add_argument("--breakdown", action="store_true")
    a = ap.parse_args(argv)
    pkg = read_v1(a.package) if a.v1 else ng3d.Package.read(open(a.package, "rb").read())
    st = Stats(pkg, kcl_of(pkg) if a.cvis else None, a.v1)
    cams = []
    if not a.no_views:
        cams += [(pkg.string(m.name), m.position, m.dir, 45.0, 400 / 240) for m in pkg.markers
                 if pkg.string(m.name).startswith("view_")]
    if a.cams:
        for line in open(a.cams):
            f = line.split()
            if len(f) >= 7 and not f[0].startswith("#"):
                cams.append((f[0], [float(x) for x in f[1:4]], [float(x) for x in f[4:7]],
                             float(f[7]) if len(f) > 7 else 45.0, float(f[8]) if len(f) > 8 else 16 / 9))
    tot = collections.Counter()
    print("%-26s %5s %5s %6s %6s %8s" % ("camera", "cells", "insts", "opa", "xlu", "tris"))
    for name, eye, tgt, fovy, aspect in cams:
        r = st.frame(eye, tgt, fovy, aspect, detail=a.detail, cvis=a.cvis, batchcull=a.batchcull)
        print("%-26s %5d %5d %6d %6d %8d" % (name, r["cells"], r["instances"], r["items_opaque"],
                                             r["items_translucent"], r["tris"]))
        tot.update(dict(r, n=1))
    n = tot["n"]
    if not n:
        return 0
    if a.breakdown:
        for k, v in sorted(st.cats.items(), key=lambda kv: -kv[1]):
            print("   %-34s %8.0f triangles %6.1f draws a frame" % (k, v / n, st.cat_items[k] / n))
    print("mean: draws %.1f (%.1f + %.1f), triangles %.0f, vertices %.0f (lit at run time %.0f), %.0f KB of them" % (
        tot["items"] / n, tot["items_opaque"] / n, tot["items_translucent"] / n, tot["tris"] / n, tot["verts"] / n,
        tot["lit_verts"] / n, tot["vbytes"] / n / 1024))
    return 0


if __name__ == "__main__":
    sys.exit(main())
