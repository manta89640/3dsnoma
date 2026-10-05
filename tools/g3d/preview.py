#!/usr/bin/env python3
"""A reference renderer for NG3D packages (and, through tools/g3d/convert.py, for BRRES models), with
numpy only: pictures of what a package holds, drawn the way GX would draw its source, to check the
pipeline and the engine's renderers against.

    preview.py PACKAGE OUT.png --camera NAME [--frame F] [--size 1280x720] [--fov 55]
                                [--lod N] [--no-instances] [--no-sky] [--no-fog]
    preview.py PACKAGE OUT.png --eye X,Y,Z --target X,Y,Z ...
    preview.py --brres FILE MODEL OUT.png --eye ... --target ...

--camera is a camera marker of the package (kind 2, e.g. view_town). --lod forces a cell detail
level everywhere (default: by distance, as the engine picks it); --lod 2 is the bare terrain.

Everything it draws comes from the package's own records: cells, the sea and sky models, instances
with their node animation, materials as GX register images -- each TEV stage from its colour and
alpha words, TREF and KSEL (konst selections, swap tables), the colour and konst registers; texgens
with their TexMtx (nw4r's Maya-mode SRT, environment and projection mapping); colour channel 0 lit by
the material's light set (directional lights, and point lights at their positions, unattenuated);
alpha compare, depth mode, blending, back-face culling, fog by the material's fog index; textures
from their GX texels, every stored mip level, bilinear, the level picked per fragment with the
material's LOD bias. Material animations (texture SRT, colour registers) and node animations are
evaluated at --frame (60 a second).

Not modelled: indirect texturing (dropped, as the engine drops it), specular, billboards (a node is
drawn as its bind matrix places it), the GX/PICA difference in texture filtering, dithering. Back
faces: GX's front faces wind clockwise. The background is the colour of the first fog in use.
Ported from the research renderer (scratchpad wuhu/tools/g3d_preview.py), whose BRRES views it is
compared with (docs/wuhu.md).
"""
import argparse
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gxtex  # noqa: E402
import ng3d  # noqa: E402
from ng3d import NONE16  # noqa: E402

KONST_FRAC = [1.0, 7 / 8, 3 / 4, 5 / 8, 1 / 2, 3 / 8, 1 / 4, 1 / 8]
BLEND_FAC = ["zero", "one", "srcclr", "invsrcclr", "srca", "invsrca", "dsta", "invdsta"]


# ------------------------------------------------------------------------------------- textures


def _wrap(i, n, mode):
    if mode == 1:
        return np.mod(i, n)
    if mode == 2:
        m = np.mod(i, 2 * n)
        return np.where(m >= n, 2 * n - 1 - m, m)
    return np.clip(i, 0, n - 1)


def sample(levels, uv, lod, wrap):
    """Bilinear sample at mip level round(lod); uv (n, 2), lod (n,)."""
    out = np.empty((len(uv), 4), np.float32)
    lvl = np.clip(np.rint(lod), 0, len(levels) - 1).astype(np.int32)
    for L in np.unique(lvl):
        sel = lvl == L
        img = levels[L]
        h, w = img.shape[:2]
        u = uv[sel, 0] * w - 0.5
        v = uv[sel, 1] * h - 0.5
        u0, v0 = np.floor(u), np.floor(v)
        fu, fv = (u - u0)[:, None], (v - v0)[:, None]
        u0, v0 = u0.astype(np.int64), v0.astype(np.int64)
        x0, x1 = _wrap(u0, w, wrap[0]), _wrap(u0 + 1, w, wrap[0])
        y0, y1 = _wrap(v0, h, wrap[1]), _wrap(v0 + 1, h, wrap[1])
        out[sel] = ((img[y0, x0] * (1 - fu) + img[y0, x1] * fu) * (1 - fv) +
                    (img[y1, x0] * (1 - fu) + img[y1, x1] * fu) * fv)
    return out


def tex_srt_matrix(s, r_deg, t):
    """nw4r's Maya-mode texture matrix (2x3) from scale, rotation (degrees) and translation."""
    sx, sy = s
    r = math.radians(r_deg)
    tx, ty = t
    c, si = math.cos(r), math.sin(r)
    return np.array([[sx * c, sx * -si, sx * ((-0.5 * c) - (0.5 * si - 0.5) - tx)],
                     [sy * si, sy * c, sy * ((-0.5 * c) + (0.5 * si - 0.5) + ty) + 1.0]], np.float32)


def srt_matrix(s, r, t):
    """nw4r's node matrix T * Rz * Ry * Rx * S (degrees), 4x4."""
    rx, ry, rz = [math.radians(a) for a in r]
    cx, sx, cy, sy, cz, sz = math.cos(rx), math.sin(rx), math.cos(ry), math.sin(ry), math.cos(rz), math.sin(rz)
    rot = np.array([[cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz],
                    [cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz],
                    [-sy, sx * cy, cx * cy]])
    m = np.eye(4)
    m[:3, :3] = rot * np.array(s)[None, :]
    m[:3, 3] = t
    return m


def hermite(f, f0, v0, s0, f1, v1, s1):
    d = f1 - f0
    if d <= 0:
        return v1
    t = (f - f0) / d
    t2, t3 = t * t, t * t * t
    return v0 * (2 * t3 - 3 * t2 + 1) + v1 * (-2 * t3 + 3 * t2) + s0 * d * (t3 - 2 * t2 + t) + s1 * d * (t3 - t2)


def track_value(pkg, tr, f):
    keys = pkg.keys[tr.first_key:tr.first_key + tr.n_keys]
    if len(keys) == 1 or f <= keys[0].frame:
        return keys[0].value
    if f >= keys[-1].frame:
        return keys[-1].value
    i = 0
    while keys[i + 1].frame <= f:
        i += 1
    a, b = keys[i], keys[i + 1]
    if tr.interp == ng3d.IP_STEP:
        return a.value
    if tr.interp == ng3d.IP_LINEAR:
        t = (f - a.frame) / (b.frame - a.frame) if b.frame > a.frame else 1.0
        return a.value + (b.value - a.value) * t
    return hermite(f, a.frame, a.value, a.slope, b.frame, b.value, b.slope)


def anim_tracks(pkg, ai, f):
    """[(track, value)] of anim `ai` at frame f (wrapped when it loops)."""
    if ai == NONE16:
        return []
    a = pkg.anims[ai]
    if a.loop and a.frames > 0:
        f = f % a.frames
    tracks = pkg.tracks[a.first_track:a.first_track + a.n_tracks]
    return [(tr, track_value(pkg, tr, f)) for tr in tracks]


# ------------------------------------------------------------------------------------- the scene


class Item:
    """A batch to draw: world-space corners, its material (and the dynamic state animations gave
    it), its pass."""
    pass


class Scene:
    """point_as_directional: point lights lit as directional ones from the origin toward them (the
    research renderer's model of the sun); fog_index: one fog for every fogged material."""

    def __init__(self, pkg, point_as_directional=False, fog_index=None):
        self.pkg = pkg
        self.point_as_directional, self.fog_index = point_as_directional, fog_index
        self.tex_levels = {}
        self.geo = {}
        self.texmtx = [dict(scale=list(t.scale), rotate=t.rotate, translate=list(t.translate)) for t in pkg.tex_mtx]
        self.regs = {}          # (material, ColorReg) -> [r, g, b, a] overrides from animations

    # textures from their GX texels, as float RGBA levels
    def levels(self, ti):
        if ti in self.tex_levels:
            return self.tex_levels[ti]
        t = self.pkg.textures[ti]
        data = self.pkg.texture_data[ti]
        out, o = [], 0
        for l in range(t.levels):
            w, h = max(1, t.src_width >> l), max(1, t.src_height >> l)
            n = gxtex.level_bytes(t.gx_format, w, h)
            out.append(gxtex.decode(data[o:o + n], w, h, t.gx_format).astype(np.float32) / 255.0)
            o += n
        self.tex_levels[ti] = out
        return out

    def batch_corners(self, bi):
        """A batch's corners in its own space: pos, nrm, clr, uv0, uv1 (None where absent)."""
        if bi in self.geo:
            return self.geo[bi]
        pkg = self.pkg
        b = pkg.batches[bi]
        vb = pkg.vertex_buffers[b.vbuf]
        raw = np.frombuffer(pkg.vertex_data[b.vbuf], np.uint8).reshape(vb.count, vb.stride)
        idx = np.array(pkg.indices[b.first_index:b.first_index + b.n_indices], np.int64)
        rows = raw[idx]
        c = {}
        o = 0
        c["pos"] = rows[:, 0:12].copy().view("<f4").reshape(-1, 3).astype(np.float32)
        o = 12
        c["nrm"] = c["clr"] = c["uv0"] = c["uv1"] = None
        if vb.attrs & ng3d.VA_NRM:
            n = rows[:, o:o + 3].copy().view(np.int8).astype(np.float32) / 127.0
            c["nrm"] = n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-6)
            o += 4
        if vb.attrs & ng3d.VA_CLR:
            c["clr"] = rows[:, o:o + 4].astype(np.float32) / 255.0
            o += 4
        for i, bit in ((0, ng3d.VA_UV0), (1, ng3d.VA_UV1)):
            if vb.attrs & bit:
                if vb.flags & ng3d.VB_UV_FLOAT:
                    c["uv%d" % i] = rows[:, o:o + 8].copy().view("<f4").reshape(-1, 2)
                    o += 8
                else:
                    q = rows[:, o:o + 4].copy().view("<i2").reshape(-1, 2)
                    c["uv%d" % i] = q.astype(np.float32) * vb.uv_scale[i]
                    o += 4
        self.geo[bi] = c
        return c

    def apply_material_anims(self, frame):
        """The env's material animations (world, sea, sky) at `frame`."""
        e = self.pkg.env
        for ai in (e.world_anim_srt, e.world_anim_clr, e.sea_anim_srt, e.sea_anim_clr, e.sky_anim):
            for tr, v in anim_tracks(self.pkg, ai, frame):
                self._apply(tr, v)

    def _apply(self, tr, v):
        t = tr.target
        if ng3d.TT_TEXMTX_SCALE_S <= t <= ng3d.TT_TEXMTX_TRANS_T:
            m = self.texmtx[tr.index]
            if t == ng3d.TT_TEXMTX_SCALE_S:
                m["scale"][0] = v
            elif t == ng3d.TT_TEXMTX_SCALE_T:
                m["scale"][1] = v
            elif t == ng3d.TT_TEXMTX_ROTATE:
                m["rotate"] = v
            elif t == ng3d.TT_TEXMTX_TRANS_S:
                m["translate"][0] = v
            else:
                m["translate"][1] = v
        elif ng3d.TT_MAT_REG_R <= t <= ng3d.TT_MAT_REG_A:
            key = (tr.index, tr.reg)
            if key not in self.regs:
                self.regs[key] = list(self._reg_base(tr.index, tr.reg))
            self.regs[key][t - ng3d.TT_MAT_REG_R] = v

    def _reg_base(self, mi, reg):
        m = self.pkg.materials[mi]
        if reg == ng3d.CR_MAT0:
            return m.mat_color
        if reg == ng3d.CR_AMB0:
            return m.amb_color
        if ng3d.CR_TEV0 <= reg <= ng3d.CR_TEV2:
            return m.tev_reg[1 + reg - ng3d.CR_TEV0]
        if ng3d.CR_KONST0 <= reg <= ng3d.CR_KONST3:
            return m.tev_konst[reg - ng3d.CR_KONST0]
        return [0, 0, 0, 0]

    def model_node_worlds(self, mi, anim=NONE16, frame=0.0):
        """4x4 model-space matrices of a model's nodes, animated by `anim` at `frame`."""
        pkg = self.pkg
        m = pkg.models[mi]
        nodes = pkg.nodes[m.first_node:m.first_node + m.n_nodes]
        srt = [[list(n.scale), list(n.rotate), list(n.translate)] for n in nodes]
        animated = set()
        for tr, v in anim_tracks(pkg, anim, frame):
            if tr.target <= ng3d.TT_NODE_TRANS_Z and tr.model == mi:
                srt[tr.index][tr.target // 3][tr.target % 3] = v
                animated.add(tr.index)
        W = []
        for k, n in enumerate(nodes):
            if not animated and anim == NONE16:
                M = np.eye(4)
                M[:3] = np.array(n.bind)
            else:
                M = srt_matrix(*srt[k])
                if n.parent != NONE16:
                    M = W[n.parent] @ M
            W.append(M)
        return W

    def gather(self, eye, lod=None, instances=True, sky=True, frame=0.0, cells=True, sky_follows=True):
        """The items of a frame seen from `eye`: sky (centred on the eye, as the engine draws it, or
        at the origin), cells at their detail level, sea, instances."""
        pkg = self.pkg
        e = pkg.env
        items = []
        order = 0

        def add(bi, M, group):
            nonlocal order
            c = self.batch_corners(bi)
            it = Item()
            b = pkg.batches[bi]
            it.batch, it.mat = bi, b.material
            if M is None:
                it.P, it.N = c["pos"], c["nrm"]
            else:
                it.P = (c["pos"] @ M[:3, :3].T + M[:3, 3]).astype(np.float32)
                if c["nrm"] is not None:
                    n = c["nrm"] @ np.linalg.inv(M[:3, :3]).astype(np.float32)     # (M^-1)^T n
                    it.N = n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-6)
                else:
                    it.N = None
            it.C, it.UV = c["clr"], [c["uv0"], c["uv1"]]
            it.group, it.order = group, order
            order += 1
            items.append(it)

        def add_model(mi, base, anim=NONE16, f=0.0, group=1):
            W = self.model_node_worlds(mi, anim, f)
            m = pkg.models[mi]
            for bi in range(m.first_batch, m.first_batch + m.n_batches):
                node = pkg.batches[bi].node
                if not pkg.nodes[m.first_node + node].flags & ng3d.NODE_VISIBLE:
                    continue
                add(bi, base @ W[node], group)

        if sky and e.sky_model != NONE16:
            T = np.eye(4)
            if sky_follows:
                T[:3, 3] = eye
            add_model(e.sky_model, T, group=0)
        if cells:
            for c in pkg.cells:
                if lod is None:
                    lo, hi = np.array(c.bbox[:3]), np.array(c.bbox[3:])
                    d = float(np.linalg.norm(np.maximum(0, np.maximum(lo - eye, eye - hi))))
                    l = max(k for k in range(c.n_lods) if c.lod_distance[k] <= d)
                else:
                    l = min(lod, c.n_lods - 1)
                for k in range(c.first[l], c.first[l] + c.count[l]):
                    add(pkg.cell_batches[k], None, 1)
        if e.sea_model != NONE16:
            add_model(e.sea_model, np.eye(4), group=1)
        if instances:
            for inst in pkg.instances:
                M = np.eye(4)
                M[:3] = np.array(inst.mtx)
                if inst.cull_distance and np.linalg.norm(M[:3, 3] - eye) > inst.cull_distance:
                    continue
                # its model chain, as the engine walks it: the next model from that model's
                # distance on, the distance from the eye to the centre of its first model's box
                mi = inst.model
                bb = pkg.models[mi].bbox
                d = float(np.linalg.norm(M[:3, :3] @ (np.add(bb[:3], bb[3:]) / 2) + M[:3, 3] - eye))
                for _ in range(len(pkg.models)):
                    m = pkg.models[mi]
                    if m.lod_next == NONE16 or d < m.lod_distance:
                        break
                    mi = m.lod_next
                add_model(mi, M, inst.anim, frame + inst.anim_offset, group=1)
        return items

    # ------------------------------------------------------------------ shading
    def material_view(self, mi):
        """The material's TEV stages and state decoded from its register words."""
        m = self.pkg.materials[mi]
        st = []
        for i in range(m.n_stages):
            t = (m.tref[i // 2] >> (12 * (i & 1))) & 0xFFF
            ks = m.ksel[i // 2]
            st.append(dict(texmap=(t & 7) if t & 0x40 else None, texcoord=(t >> 3) & 7, chan=(t >> 7) & 7,
                           kc=(ks >> (4 + 10 * (i & 1))) & 31, ka=(ks >> (9 + 10 * (i & 1))) & 31,
                           ce=m.color_env[i], ae=m.alpha_env[i]))
        swaps = [(m.ksel[2 * j] & 3, (m.ksel[2 * j] >> 2) & 3, m.ksel[2 * j + 1] & 3, (m.ksel[2 * j + 1] >> 2) & 3)
                 for j in range(4)]
        return m, st, swaps

    def texcoords(self, m, frag):
        out = []
        for i in range(m.n_texgens):
            g = m.texgens[i]
            tm = self.pkg.tex_mtx[m.tex_mtx_first + g.matrix] if g.matrix != NONE16 else None
            mode = tm.map_mode if tm is not None else 0
            if mode in (1, 3, 4) or g.source == 1:               # environment: the view-space normal
                Nv = frag["nrm_view"]
                st = np.stack([0.5 + 0.5 * Nv[:, 0], 0.5 - 0.5 * Nv[:, 1]], 1)
            elif mode in (2, 5, 6) or g.source == 0:             # projection: the screen
                st = frag["ndc"] * np.array([0.5, -0.5], np.float32) + 0.5
            elif g.source in (4, 5):
                k = g.source - 4
                st = frag.get("uv%d" % k)
                if st is None:
                    st = np.zeros((frag["n"], 2), np.float32)
            else:
                st = np.zeros((frag["n"], 2), np.float32)
            if tm is not None and mode == 0:
                d = self.texmtx[m.tex_mtx_first + g.matrix]
                M = tex_srt_matrix(d["scale"], d["rotate"], d["translate"])
                st = st @ M[:, :2].T + M[:, 2]
            out.append(st.astype(np.float32))
        return out

    def channel(self, m, mi, frag):
        n = frag["n"]
        if m.n_chans < 1:
            return np.zeros((n, 4), np.float32)
        pkg = self.pkg
        vtx = frag.get("clr")
        matc = np.array(self.regs.get((mi, ng3d.CR_MAT0), m.mat_color), np.float32) / 255.0
        ambc = np.array(self.regs.get((mi, ng3d.CR_AMB0), m.amb_color), np.float32) / 255.0
        res = np.empty((n, 4), np.float32)
        for word, sl in ((m.chan_color, slice(0, 3)), (m.chan_alpha, slice(3, 4))):
            k = sl.stop - sl.start
            mat = vtx[:, sl] if (word & 1 and vtx is not None) else np.broadcast_to(matc[sl], (n, k))
            if not word & 2:                                   # no lighting
                res[:, sl] = mat
                continue
            ls = None
            if m.flags & ng3d.MAT_LIGHT_SET and m.light_set < len(pkg.light_sets):
                ls = pkg.light_sets[m.light_set]
            if ls is not None:                                 # the scene's light set (nw4r ScnRoot)
                amb = np.array(ls.ambient, np.float32)[sl] / 255.0
                lights = [pkg.lights[l] for l in ls.lights if l != NONE16]
            else:
                amb = (vtx[:, sl] if word & 0x40 and vtx is not None else np.broadcast_to(ambc[sl], (n, k)))
                lights = []
            light = np.broadcast_to(amb, (n, k)).astype(np.float32).copy()
            if sl.start == 0 and frag.get("nrm") is not None:
                for L in lights:
                    if L.kind == ng3d.LK_POINT and self.point_as_directional:
                        d = np.array(L.position, np.float32)
                        dot = frag["nrm"] @ (d / max(np.linalg.norm(d), 1e-6))
                    elif L.kind == ng3d.LK_POINT:
                        d = np.array(L.position, np.float32)[None, :] - frag["pos"]
                        d /= np.maximum(np.linalg.norm(d, axis=1, keepdims=True), 1e-6)
                        dot = (frag["nrm"] * d).sum(1)
                    else:
                        dot = frag["nrm"] @ -np.array(L.direction, np.float32)
                    fn = (word >> 7) & 3
                    if fn == 2 or fn == 3:
                        dot = np.maximum(dot, 0)
                    elif fn == 0:
                        dot = np.ones_like(dot)
                    light += dot[:, None] * (np.array(L.rgba[:3], np.float32) / 255.0)[None, :]
            res[:, sl] = mat * np.clip(light, 0, 1)
        return res

    def shade(self, it, frag):
        m, stages, swaps = self.material_view(it.mat)
        mi = it.mat
        n = frag["n"]
        coords = self.texcoords(m, frag)
        R = []
        for i in range(4):
            v = list(m.tev_reg[i])
            if i >= 1 and (mi, ng3d.CR_TEV0 + i - 1) in self.regs:
                v = self.regs[(mi, ng3d.CR_TEV0 + i - 1)]
            R.append(np.repeat((np.array(v, np.float32) / 255.0)[None, :], n, 0))
        K = [np.array(self.regs.get((mi, ng3d.CR_KONST0 + i), m.tev_konst[i]), np.float32) / 255.0 for i in range(4)]
        ras_cache = None
        for st in stages:
            tm = st["texmap"]
            if tm is not None and m.maps[tm].texture != NONE16 and st["texcoord"] < len(coords):
                mp = m.maps[tm]
                lv = self.levels(mp.texture)
                tc = st["texcoord"]
                g = m.texgens[tc]
                key = "lod_uv%d" % (g.source - 4) if g.source in (4, 5) else None
                lod = frag[key] if key in frag else frag["lod_base"]
                h, w = lv[0].shape[:2]
                lod = np.clip(lod + 0.5 * math.log2(max(w * h, 1)) + mp.lod_bias, mp.min_lod, mp.max_lod)
                tex = sample(lv, coords[tc], lod, (mp.wrap_s, mp.wrap_t))
                tex = tex[:, list(swaps[(st["ae"] >> 2) & 3])]
            else:
                tex = np.ones((n, 4), np.float32)
            if st["chan"] == 0:
                if ras_cache is None:
                    ras_cache = self.channel(m, mi, frag)
                ras = ras_cache[:, list(swaps[st["ae"] & 3])]
            else:
                ras = np.zeros((n, 4), np.float32)
            kc, ka = st["kc"], st["ka"]
            if kc < 8:
                kcol = np.full(3, KONST_FRAC[kc], np.float32)
            elif 12 <= kc <= 15:
                kcol = K[kc - 12][:3]
            elif kc >= 16:
                kcol = np.full(3, K[(kc - 16) % 4][(kc - 16) // 4], np.float32)
            else:
                kcol = np.ones(3, np.float32)
            kal = KONST_FRAC[ka] if ka < 8 else (K[(ka - 16) % 4][(ka - 16) // 4] if ka >= 16 else 1.0)
            ce, ae = st["ce"], st["ae"]
            cin = {0: R[0][:, :3], 1: np.repeat(R[0][:, 3:4], 3, 1), 2: R[1][:, :3], 3: np.repeat(R[1][:, 3:4], 3, 1),
                   4: R[2][:, :3], 5: np.repeat(R[2][:, 3:4], 3, 1), 6: R[3][:, :3], 7: np.repeat(R[3][:, 3:4], 3, 1),
                   8: tex[:, :3], 9: np.repeat(tex[:, 3:4], 3, 1), 10: ras[:, :3], 11: np.repeat(ras[:, 3:4], 3, 1),
                   12: 1.0, 13: 0.5, 14: kcol, 15: 0.0}
            ain = {0: R[0][:, 3], 1: R[1][:, 3], 2: R[2][:, 3], 3: R[3][:, 3], 4: tex[:, 3], 5: ras[:, 3],
                   6: kal, 7: 0.0}
            colv = _combine(cin, ce, 15, (12, 8, 4, 0))
            alpv = _combine(ain, ae, 7, (13, 10, 7, 4))
            dc, da = (ce >> 22) & 3, (ae >> 22) & 3
            R[dc] = R[dc].copy()
            R[dc][:, :3] = np.broadcast_to(colv, (n, 3))
            R[da] = R[da].copy()
            R[da][:, 3] = np.broadcast_to(alpv, (n,))
        if not stages:
            return np.ones((n, 4), np.float32)
        out = R[(stages[-1]["ce"] >> 22) & 3].copy()
        out[:, 3] = R[(stages[-1]["ae"] >> 22) & 3][:, 3]
        return np.clip(out, 0, 1)


def _combine(inp, e, mask, shifts):
    a, b, c, d = [inp[(e >> s) & mask] for s in shifts]
    bias, sub, clamp, scale = (e >> 16) & 3, (e >> 18) & 1, (e >> 19) & 1, (e >> 20) & 3
    if bias == 3:                       # compare modes, approximated per component (GT / EQ)
        a, b = np.asarray(a, np.float32), np.asarray(b, np.float32)
        hit = (np.abs(a - b) < 0.5 / 255) if sub else (a > b)
        v = np.asarray(d, np.float32) + np.where(hit, c, 0.0)
    else:
        c = np.asarray(c, np.float32)
        v = (np.asarray(d, np.float32) + (-1 if sub else 1) * ((1 - c) * a + c * b) + (0.0, 0.5, -0.5)[bias])
        v = v * (1.0, 2.0, 4.0, 0.5)[scale]
    return np.clip(v, 0, 1) if clamp else np.clip(v, -4, 4)


def _alpha_test(word, a):
    def cmp(op, ref):
        r = ref / 255.0
        return [np.zeros_like(a, bool), a < r, np.abs(a - r) < 0.5 / 255, a <= r, a > r,
                np.abs(a - r) >= 0.5 / 255, a >= r, np.ones_like(a, bool)][op]
    p0, p1 = cmp((word >> 16) & 7, word & 0xFF), cmp((word >> 19) & 7, (word >> 8) & 0xFF)
    return [p0 & p1, p0 | p1, p0 ^ p1, ~(p0 ^ p1)][(word >> 22) & 3]


def _has_alpha_test(word):
    c0, c1, op = (word >> 16) & 7, (word >> 19) & 7, (word >> 22) & 3
    if op in (0, 1) and c0 == 7 and c1 == 7:
        return False
    if op == 1 and 7 in (c0, c1):
        return False
    return True


def _blend(word, src, dst):
    if not word & 1:
        return src[:, :3]
    sa = src[:, 3:4]
    da = np.ones_like(sa)

    def fac(i, is_src):
        name = BLEND_FAC[i]
        if name == "zero":
            return 0.0
        if name == "one":
            return 1.0
        if name == "srcclr":           # as a source factor GX reads the destination colour
            return dst if is_src else src[:, :3]
        if name == "invsrcclr":
            return 1 - dst if is_src else 1 - src[:, :3]
        if name == "srca":
            return sa
        if name == "invsrca":
            return 1 - sa
        return da if name == "dsta" else 1 - da
    if word & 0x800:
        return np.clip(dst - src[:, :3], 0, 1)
    return np.clip(src[:, :3] * fac((word >> 8) & 7, True) + dst * fac((word >> 5) & 7, False), 0, 1)


# ------------------------------------------------------------------------------------- rasteriser


def _raster(sx, sy, sz, sw, W, H, keep):
    """Fragments of triangles: triangle, pixel, perspective-correct barycentrics, depth."""
    tri_l, pix_l, bar_l, z_l = [], [], [], []
    xmin = np.clip(np.floor(sx.min(1)), 0, W).astype(np.int64)
    xmax = np.clip(np.ceil(sx.max(1)), 0, W).astype(np.int64)
    ymin = np.clip(np.floor(sy.min(1)), 0, H).astype(np.int64)
    ymax = np.clip(np.ceil(sy.max(1)), 0, H).astype(np.int64)
    area = (sx[:, 1] - sx[:, 0]) * (sy[:, 2] - sy[:, 0]) - (sx[:, 2] - sx[:, 0]) * (sy[:, 1] - sy[:, 0])
    ok = keep & (xmax > xmin) & (ymax > ymin) & (np.abs(area) > 1e-12)
    for t in np.nonzero(ok)[0]:
        px = np.arange(xmin[t], xmax[t], dtype=np.float32) + 0.5
        py = np.arange(ymin[t], ymax[t], dtype=np.float32) + 0.5
        X, Y = np.meshgrid(px, py)
        ax, ay = sx[t], sy[t]
        w0 = (ax[1] - X) * (ay[2] - Y) - (ax[2] - X) * (ay[1] - Y)
        w1 = (ax[2] - X) * (ay[0] - Y) - (ax[0] - X) * (ay[2] - Y)
        w2 = (ax[0] - X) * (ay[1] - Y) - (ax[1] - X) * (ay[0] - Y)
        A = area[t]
        inside = ((w0 >= 0) & (w1 >= 0) & (w2 >= 0)) if A > 0 else ((w0 <= 0) & (w1 <= 0) & (w2 <= 0))
        if not inside.any():
            continue
        b0, b1, b2 = w0[inside] / A, w1[inside] / A, w2[inside] / A
        q0, q1, q2 = b0 * sw[t, 0], b1 * sw[t, 1], b2 * sw[t, 2]
        qs = q0 + q1 + q2
        # the view depth: 1/w is what is linear on the screen, not w (a big triangle, the sea's, gets
        # the depth of its far corners at its near pixels otherwise, and their fog)
        z = 1.0 / qs
        tri_l.append(np.full(len(z), t, np.int64))
        pix_l.append((Y[inside] - 0.5).astype(np.int64) * W + (X[inside] - 0.5).astype(np.int64))
        bar_l.append(np.stack([q0 / qs, q1 / qs, q2 / qs], 1).astype(np.float32))
        z_l.append(z.astype(np.float32))
    if not tri_l:
        return None
    return np.concatenate(tri_l), np.concatenate(pix_l), np.concatenate(bar_l), np.concatenate(z_l)


class View:
    """A perspective camera: world -> screen x, y, view depth, 1/w."""

    def __init__(self, eye, target, fov_deg, W, H, near=20.0, far=250000.0):
        self.W, self.H = W, H
        self.eye = np.asarray(eye, np.float32)
        f = np.asarray(target, np.float32) - self.eye
        f /= np.linalg.norm(f)
        r = np.cross(f, np.array([0, 1, 0], np.float32))
        r /= np.linalg.norm(r)
        u = np.cross(r, f)
        self.R = np.stack([r, u, -f])            # rows: view x, y, z (looking down -z)
        self.near, self.far = near, far
        self.fy = self.fx = (H / 2) / math.tan(math.radians(fov_deg) / 2)

    def proj(self, P):
        Q = (P - self.eye) @ self.R.T
        zc = -Q[:, 2]
        w = np.maximum(zc, 1e-3)
        return self.W / 2 + self.fx * Q[:, 0] / w, self.H / 2 - self.fy * Q[:, 1] / w, zc, 1.0 / w


def _clip_near(P, attrs, view):
    """Triangles crossing the near plane split; P (T, 3, 3), attrs list of (T, 3, k) or None."""
    d = ((P - view.eye) @ (-view.R[2])) - view.near
    inside = d > 0
    allin, anyin = inside.all(1), inside.any(1)
    keepP = [P[allin]]
    keepA = [[a[allin] if a is not None else None for a in attrs]]
    part = np.nonzero(anyin & ~allin)[0]
    newP, newA = [], [[] for _ in attrs]
    for t in part:
        poly = []
        for i in range(3):
            j = (i + 1) % 3
            di, dj = d[t, i], d[t, j]
            if di > 0:
                poly.append((i, i, 0.0))
            if (di > 0) != (dj > 0):
                poly.append((i, j, di / (di - dj)))

        def lerp(arr, e):
            i, j, s = e
            return arr[t, i] * (1 - s) + arr[t, j] * s
        for k in range(1, len(poly) - 1):
            tri = (poly[0], poly[k], poly[k + 1])
            newP.append(np.stack([lerp(P, e) for e in tri]))
            for ai, a in enumerate(attrs):
                newA[ai].append(None if a is None else np.stack([lerp(a, e) for e in tri]))
    if newP:
        keepP.append(np.stack(newP))
        keepA.append([None if a is None else np.stack(newA[ai]) for ai, a in enumerate(attrs)])
    return np.concatenate(keepP), [None if a is None else np.concatenate([ka[ai] for ka in keepA])
                                   for ai, a in enumerate(attrs)]


def render(scene, view, items, fog=True, background=None, progress=False):
    """Draw items: the sky group first, then opaque items, then translucent ones back to front within
    their material priority. Returns (H, W, 3) uint8 and the depth buffer."""
    pkg = scene.pkg
    W, H = view.W, view.H
    fog_tab = pkg.fogs
    # the clear colour: the first fog in use's (the open sea's: the sky dome's lower edge meets the
    # fogged sea in that colour)
    used = [f for f in fog_tab if f.type != ng3d.FOG_NONE]
    bg = np.array(background if background is not None else
                  ([c / 255.0 for c in used[0].rgba[:3]] if used else (0.5, 0.7, 0.9)), np.float32)
    color = np.tile(bg, (H * W, 1))
    depth = np.full(H * W, np.inf, np.float32)

    def xlu(it):
        return bool(pkg.materials[it.mat].flags & ng3d.MAT_XLU)

    def dist(it):
        return float(np.linalg.norm(it.P.mean(0) - view.eye)) if it.P is not None and len(it.P) else 0.0
    sky = [it for it in items if it.group == 0]
    opa = [it for it in items if it.group != 0 and not xlu(it)]
    trn = sorted([it for it in items if it.group != 0 and xlu(it)],
                 key=lambda it: (pkg.materials[it.mat].priority, -dist(it), it.order))
    for k, it in enumerate(sky + opa + trn):
        if progress and k % 100 == 0:
            print("  item %d/%d" % (k, len(items)), file=sys.stderr)
        _draw(scene, view, it, color, depth, fog)
    return (np.clip(color, 0, 1).reshape(H, W, 3) * 255 + 0.5).astype(np.uint8), depth.reshape(H, W)


def _draw(scene, view, it, color, depth, fog):
    pkg = scene.pkg
    m = pkg.materials[it.mat]
    W, H = view.W, view.H
    T = len(it.P) // 3
    if T == 0:
        return
    # cheap rejection: all corners behind the camera
    P = it.P.reshape(T, 3, 3)
    attrs = [None if it.N is None else it.N.reshape(T, 3, 3), None if it.C is None else it.C.reshape(T, 3, 4)] + \
        [None if u is None else u.reshape(T, 3, 2) for u in it.UV]
    P, attrs = _clip_near(P, attrs, view)
    T = len(P)
    if T == 0:
        return
    flat = P.reshape(-1, 3)
    sx, sy, sz, sw = [a.reshape(T, 3) for a in view.proj(flat)]
    keep = np.ones(T, bool)
    if m.cull in (1, 2, 3):
        fn = np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0])
        facing = (fn * (view.eye[None, :] - P.mean(1))).sum(1) < 0     # clockwise = front (GX)
        keep &= facing if m.cull == 2 else (~facing if m.cull == 1 else False)
    fr = _raster(sx, sy, sz, sw, W, H, keep)
    if fr is None:
        return
    tri, pix, bar, z = fr
    zt, zw = m.zmode & 1, m.zmode & 0x10
    if zt:
        ok = z <= depth[pix] + 1e-3 * np.maximum(1, np.abs(z))
        tri, pix, bar, z = tri[ok], pix[ok], bar[ok], z[ok]
    if len(z) == 0:
        return
    has_at = _has_alpha_test(m.alpha_compare)
    if not m.blend & 1 and not has_at:          # opaque: the nearest fragment of each pixel only
        o = np.lexsort((z, pix))
        first = np.ones(len(o), bool)
        first[1:] = pix[o][1:] != pix[o][:-1]
        o = o[first]
        tri, pix, bar, z = tri[o], pix[o], bar[o], z[o]
    frag = dict(n=len(z))
    N = attrs[0]
    if N is not None:
        nv = (N[tri] * bar[:, :, None]).sum(1)
        nv /= np.maximum(np.linalg.norm(nv, axis=1, keepdims=True), 1e-6)
        frag["nrm"], frag["nrm_view"] = nv, nv @ view.R.T
    else:
        frag["nrm"], frag["nrm_view"] = None, np.tile(np.array([0, 0, 1], np.float32), (len(z), 1))
    frag["pos"] = (P[tri] * bar[:, :, None]).sum(1)
    if attrs[1] is not None:
        frag["clr"] = (attrs[1][tri] * bar[:, :, None]).sum(1)
    px = (pix % W).astype(np.float32) + 0.5
    py = (pix // W).astype(np.float32) + 0.5
    frag["ndc"] = np.stack([px / W * 2 - 1, 1 - py / H * 2], 1).astype(np.float32)
    sarea = np.maximum(np.abs((sx[:, 1] - sx[:, 0]) * (sy[:, 2] - sy[:, 0]) -
                              (sx[:, 2] - sx[:, 0]) * (sy[:, 1] - sy[:, 0])) * 0.5, 1e-6)
    wmean = 1.0 / np.maximum(sw.mean(1), 1e-9)
    wpix = 1.0 / np.maximum((bar * sw[tri]).sum(1), 1e-9)
    corr = np.log2(np.maximum(wpix / wmean[tri], 1e-6))
    frag["lod_base"] = np.zeros(len(z), np.float32)
    for k in range(2):
        a = attrs[2 + k]
        if a is None:
            continue
        frag["uv%d" % k] = (a[tri] * bar[:, :, None]).sum(1)
        uarea = np.abs((a[:, 1, 0] - a[:, 0, 0]) * (a[:, 2, 1] - a[:, 0, 1]) -
                       (a[:, 2, 0] - a[:, 0, 0]) * (a[:, 1, 1] - a[:, 0, 1])) * 0.5
        frag["lod_uv%d" % k] = (0.5 * np.log2(np.maximum(uarea / sarea, 1e-12)))[tri] + corr
    rgba = scene.shade(it, frag)
    if has_at:
        ok = _alpha_test(m.alpha_compare, rgba[:, 3])
        rgba, tri, pix, z = rgba[ok], tri[ok], pix[ok], z[ok]
        if not m.blend & 1 and len(z):
            o = np.lexsort((z, pix))
            first = np.ones(len(o), bool)
            first[1:] = pix[o][1:] != pix[o][:-1]
            o = o[first]
            rgba, tri, pix, z = rgba[o], tri[o], pix[o], z[o]
    if len(z) == 0:
        return
    fi = m.fog if scene.fog_index is None else scene.fog_index
    if fog and m.flags & ng3d.MAT_FOG and fi < len(pkg.fogs) and pkg.fogs[fi].type:
        F = pkg.fogs[fi]
        f = np.clip((z - F.start) / max(F.end - F.start, 1e-6), 0, 1)[:, None]
        rgba[:, :3] = rgba[:, :3] * (1 - f) + (np.array(F.rgba[:3], np.float32) / 255.0) * f
    order = np.argsort(tri, kind="stable")             # in triangle order; a pixel hit twice blends twice
    rgba, pix, z = rgba[order], pix[order], z[order]
    remaining = np.arange(len(pix))
    while len(remaining):
        u, first = np.unique(pix[remaining], return_index=True)
        sel = remaining[first]
        p = pix[sel]
        color[p] = _blend(m.blend, rgba[sel], color[p])
        if zw:
            depth[p] = np.minimum(depth[p], z[sel])
        mask = np.ones(len(remaining), bool)
        mask[first] = False
        remaining = remaining[mask]


# ------------------------------------------------------------------------------------- CLI


def render_package(pkg, eye, target, fov, W, H, frame=0.0, lod=None, instances=True, sky=True, fog=True,
                   progress=False, material_anims=True, sky_follows=True, fog_index=None,
                   point_as_directional=False):
    sc = Scene(pkg, point_as_directional, fog_index)
    if material_anims:
        sc.apply_material_anims(frame)
    items = sc.gather(np.asarray(eye, np.float32), lod=lod, instances=instances, sky=sky, frame=frame,
                      sky_follows=sky_follows)
    img, _ = render(sc, View(eye, target, fov, W, H), items, fog=fog, progress=progress)
    return img, len(items)


def brres_package(path, model):
    """A one-model package of a BRRES model (its own materials and textures), for a quick look."""
    import brres
    import convert
    r = brres.BRRES.load(path)
    pkg = ng3d.Package()
    cv = convert.Converter(pkg)
    mi, _ = cv.model(r, r.models[model])
    pkg.add("instances", model=mi, anim=NONE16, mtx=[[1.0, 0, 0, 0], [0, 1.0, 0, 0], [0, 0, 1.0, 0]])
    pkg.env = ng3d.Rec("Env", sky_model=NONE16, sky_anim=NONE16, sea_model=NONE16, sea_anim_srt=NONE16,
                       sea_anim_clr=NONE16, world_anim_srt=NONE16, world_anim_clr=NONE16)
    return pkg


def main(argv=None):
    ap = argparse.ArgumentParser(description="render an NG3D package (or a BRRES model) to PNG")
    ap.add_argument("package", nargs="?")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--brres", nargs=3, metavar=("FILE", "MODEL", "OUT"))
    ap.add_argument("--camera", help="a camera marker of the package (e.g. view_town)")
    ap.add_argument("--eye")
    ap.add_argument("--target")
    ap.add_argument("--fov", type=float, default=55.0)
    ap.add_argument("--size", default="1280x720")
    ap.add_argument("--frame", type=float, default=0.0)
    ap.add_argument("--lod", type=int)
    ap.add_argument("--no-instances", action="store_true")
    ap.add_argument("--no-sky", action="store_true")
    ap.add_argument("--no-fog", action="store_true")
    a = ap.parse_args(argv)
    from PIL import Image
    W, H = [int(x) for x in a.size.split("x")]
    if a.brres:
        pkg = brres_package(a.brres[0], a.brres[1])
        out = a.brres[2]
    else:
        pkg = ng3d.Package.read(open(a.package, "rb").read())
        out = a.out
        if any(not t.flags & ng3d.TEX_GX_RAW for t in pkg.textures):
            raise SystemExit("%s is baked (PICA texels): this renderer reads GX texels; give it the unbaked "
                             "package (tools/wuhu/stage_wuhu.py --keep-raw keeps island.unbaked.ng3d)" % a.package)
    if a.camera:
        mk = [m for m in pkg.markers if pkg.string(m.name) == a.camera]
        if not mk:
            raise SystemExit("no marker %s" % a.camera)
        eye, target = mk[0].position, mk[0].dir
    else:
        eye = [float(x) for x in a.eye.split(",")]
        target = [float(x) for x in a.target.split(",")]
    img, n = render_package(pkg, eye, target, a.fov, W, H, a.frame, a.lod, not a.no_instances, not a.no_sky,
                            not a.no_fog, progress=True)
    Image.fromarray(img, "RGB").save(out, optimize=True)
    print("wrote %s (%d items)" % (out, n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
