"""BRRES -> NG3D: the general part of staging (textures, materials, meshes, models, animations).

    pkg = ng3d.Package()
    cv = Converter(pkg)
    m = cv.model(brres, brres.models["WS2_common_sea"])          # a Model with its nodes and meshes
    cells = MeshSet(cv)                                          # static geometry, batched by key
    cells.add(key, cv.material(brres, mdl, mat), corners_world)  # ... then cells.build()
    ab = AnimBuilder(pkg, "name", frames=200, loop=True)         # tracks from CHR0/SRT0/CLR0 curves

Nothing here knows about an island: tools/wuhu/stage_wuhu.py composes a world from these, and
tools/g3d/preview.py converts a BRRES with them to draw it the way the engine will.

Textures stay GX texels (Texture.flags TEX_GX_RAW, gx_format = GX's format) with every mip level
down to 8x8; Texture.format is the PICA format the loader converts them to (platform/3ds/pica_tex.h
from_gx), the one platform/3ds/gx_pica.cpp's pica_format() would pick, except CMPR, which becomes
ETC1, or ETC1A4 if a block uses CMPR's transparent texel (pica_tex.h cmpr_has_alpha's rule): 4 bits
a texel like CMPR itself instead of RGBA5551's 16. Paletted textures (C4/C8/C14X2) are written out in
their palette's format (IA8, RGB565, RGB5A3), since a package carries no palettes.

A material is GX's register image as its display lists leave it (brres.GxRegs): the TEV and pixel
words verbatim, colour channel 0, texture maps, texgens and one TexMtx per texgen (the texture SRT,
its nw4r matrix mode, map mode and effect matrix). Two GX facts the image can't say by itself are
stated where they are filled in (blend's masked bits; texgen matrices).
"""
import math
import os
import sys
from collections import Counter

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import brres  # noqa: E402
import gxtex  # noqa: E402
import ng3d  # noqa: E402
from ng3d import NONE16, Rec  # noqa: E402

# GX texture format -> the PICA format the loader converts it to (pica_format(), gx_pica.cpp)
PICA_FORMAT = {0: ng3d.TF_L4, 1: ng3d.TF_L8, 2: ng3d.TF_LA4, 3: ng3d.TF_LA8, 4: ng3d.TF_RGB565,
               5: ng3d.TF_RGBA8, 6: ng3d.TF_RGBA8}
# a palette's format -> the GX format its colours are written in (IA8, RGB565, RGB5A3)
PALETTE_AS_GX = {0: 3, 1: 4, 2: 5}
# BP 0x41's bits 2-4 (dither, colour update, alpha update): nw4r's material list writes the blend
# register through BP mask 0xFFE3, leaving them to the global state, where colour update is on
BLEND_GLOBAL_BITS = 0x0008

def _pow2(v, lo=8):
    p = lo
    while p < v:
        p <<= 1
    return p


def _tile_gx(img_u16, w, h, bw, bh):
    """(h, w) big-endian u16 texels -> GX tiles of bw x bh (a 16-bit format's layout), padded."""
    pw, ph = -(-w // bw) * bw, -(-h // bh) * bh
    pad = np.zeros((ph, pw), np.uint16)
    pad[:h, :w] = img_u16
    t = pad.reshape(ph // bh, bh, pw // bw, bw).transpose(0, 2, 1, 3)
    return t.astype(">u2").tobytes()


def _depalette(tex, palette_raw, plt_fmt, lod):
    """A C4/C8/C14X2 level's texels looked up in its palette, as GX 16-bit texels of the palette's
    format (bytes, GX format)."""
    w, h, n = tex.level_size(lod)
    raw = np.frombuffer(tex.level_data(lod), np.uint8)
    bw, bh = gxtex.BLOCK[tex.fmt]
    pw, ph = -(-w // bw) * bw, -(-h // bh) * bh
    if tex.fmt == 8:
        idx = np.stack([raw >> 4, raw & 15], -1).reshape(-1)
    elif tex.fmt == 9:
        idx = raw.astype(np.int64)
    else:
        idx = raw.view(">u2").astype(np.int64) & 0x3FFF
    idx = idx.reshape(ph // bh, pw // bw, bh, bw).transpose(0, 2, 1, 3).reshape(ph, pw)[:h, :w]
    pal = np.frombuffer(palette_raw, ">u2")
    vals = pal[np.minimum(idx, len(pal) - 1)]
    g = PALETTE_AS_GX[plt_fmt]
    return _tile_gx(vals, w, h, *gxtex.BLOCK[g]), g


class Converter:
    """Makes package records from BRRES objects, each once (textures by (file, name), materials
    by (file, model, material))."""

    def __init__(self, pkg):
        self.pkg = pkg
        self.tex = {}
        self.mat = {}
        self.stats = Counter()

    # ---------------------------------------------------------------- textures
    def texture(self, r, name):
        key = (id(r), name)
        if key in self.tex:
            return self.tex[key]
        t = r.textures[name]
        gx = t.fmt
        # levels: GX's own chain, as far as both sides stay >= 8 (the PICA's smallest level)
        levels = 1
        while levels < t.levels and min(t.width >> levels, t.height >> levels) >= 8:
            levels += 1
        flags = ng3d.TEX_GX_RAW
        if gx in gxtex.PALETTED:
            p = r.palettes[name]
            chunks = []
            for l in range(levels):
                data, gx2 = _depalette(t, bytes(p.b.d[p.data_off:p.data_off + 2 * p.count]), p.fmt, l)
                chunks.append(data)
            data, gx = b"".join(chunks), gx2
            self.stats["textures depaletted"] += 1
        else:
            data = b"".join(t.level_data(l) for l in range(levels))
        rgba = r.texture_rgba(name, 0)
        a = rgba[..., 3]
        if (a < 255).any():
            flags |= ng3d.TEX_ALPHA
            if np.isin(a, (0, 255)).all():
                flags |= ng3d.TEX_CUTOUT
        if gx == 14:
            alpha = any(gxtex.cmpr_has_alpha(t.level_data(l), *t.level_size(l)[:2]) for l in range(levels))
            fmt = ng3d.TF_ETC1A4 if alpha else ng3d.TF_ETC1
        else:
            fmt = PICA_FORMAT[gx]
        self.pkg.add("textures", name=self.pkg.str(name), width=_pow2(t.width), height=_pow2(t.height),
                     format=fmt, levels=levels, flags=flags, gx_format=gx, src_width=t.width,
                     src_height=t.height)
        self.pkg.texture_data.append(data)
        i = len(self.pkg.textures) - 1
        self.tex[key] = i
        self.stats["textures"] += 1
        self.stats["texture bytes"] += len(data)
        return i

    # ---------------------------------------------------------------- materials
    def material(self, r, mdl, mat, tex_override=None, priority=0, name=None):
        """The package material for `mat` (a brres Material of model `mdl` in `r`). tex_override
        maps a texture name to (brres, name) to use instead (runtime swaps)."""
        ov = tuple(sorted((k, id(v[0]), v[1]) for k, v in (tex_override or {}).items()))
        key = (id(r), mdl.name, mat.index, ov)
        if key in self.mat:
            return self.mat[key]
        bp = mat.regs.bp
        M = Rec("Material")
        M.name = self.pkg.str(name or "%s/%s" % (mdl.name, mat.name))
        flags = 0
        if mat.xlu:
            flags |= ng3d.MAT_XLU
        if mat.fog >= 0:
            flags |= ng3d.MAT_FOG
        ind = mat.n_ind > 0 or any(s["ind"] for s in mat.stages)
        if ind:
            flags |= ng3d.MAT_INDIRECT
            self.stats["materials with indirect stages (dropped)"] += 1
        if mat.light_set >= 0:
            flags |= ng3d.MAT_LIGHT_SET
        M.flags = flags
        M.n_stages, M.n_texgens, M.n_chans, M.n_ind = mat.n_stages, mat.n_texgens, mat.n_chans, mat.n_ind
        M.cull = mat.cull
        M.light_set = max(mat.light_set, 0)
        M.fog = max(mat.fog, 0)
        M.priority = max(-128, min(127, priority))
        M.color_env = [bp.get(0xC0 + 2 * i, 0) for i in range(16)]
        M.alpha_env = [bp.get(0xC1 + 2 * i, 0) for i in range(16)]
        M.tref = [bp.get(0x28 + i, 0) for i in range(8)]
        M.ksel = [bp.get(0xF6 + i, 0) for i in range(8)]
        for konst, field in ((False, "tev_reg"), (True, "tev_konst")):
            rows = []
            for i in range(4):
                ra, bg = mat.regs.tev.get((konst, i, "ra"), 0), mat.regs.tev.get((konst, i, "bg"), 0)
                c = [ra & 0x7FF, (bg >> 12) & 0x7FF, bg & 0x7FF, (ra >> 12) & 0x7FF]
                rows.append([v & 0xFF for v in c] if konst else [brres.sx11(v) for v in c])
            setattr(M, field, rows)
        M.alpha_compare = bp.get(0xF3, 0)
        M.zmode = bp.get(0x40, 0)
        M.blend = bp.get(0x41, 0) | BLEND_GLOBAL_BITS
        M.const_alpha = bp.get(0x42, 0)
        ch = mat.chans[0]
        M.chan_color, M.chan_alpha = ch["color"]["raw"], ch["alpha"]["raw"]
        M.mat_color, M.amb_color = list(ch["mat"]), list(ch["amb"])
        for mp in M.maps:
            mp.texture = NONE16
        for l in mat.layers:
            src = (tex_override or {}).get(l["tex"], (r, l["tex"]))
            ti = self.texture(*src)
            t = self.pkg.textures[ti]
            mp = M.maps[l["map_id"]]
            mp.texture = ti
            mp.wrap_s, mp.wrap_t = l["wrap_id"]
            mp.min_filter, mp.mag_filter = l["min_id"], l["mag_id"]
            mp.max_aniso = l["aniso"]
            mp.flags = (l["bias_clamp"] & 1) | ((l["edge_lod"] & 1) << 1)
            mp.lod_bias = l["lod_bias"]
            tex0 = src[0].textures[src[1]]
            mp.min_lod, mp.max_lod = float(tex0.min_lod), float(min(tex0.max_lod, t.levels - 1))
        # One TexMtx per texgen: texgen i is GX texture matrix i (nw4r loads the material's SRT i
        # there); `matrix` points at it when nw4r applies one (its SRT flag says it exists, or it
        # maps the environment / a projection), and env-mapping texgens (normalised, with a post
        # matrix) use it as their post matrix too: normal -> view space, then SRT x effect matrix.
        M.tex_mtx_first, M.n_tex_mtx = len(self.pkg.tex_mtx), mat.n_texgens
        for i, g in enumerate(mat.texgens):
            s = mat.srt[i]
            self.pkg.add("tex_mtx", scale=list(s["scale"]), rotate=s["rot"], translate=list(s["trans"]),
                         mode=mat.tex_mtx_mode, map_mode=s["map_mode_id"], ref_camera=s["cam"] & 0xFF,
                         ref_light=s["light"] & 0xFF, effect=[list(map(float, row)) for row in s["effect"]])
            if g["src"] not in (0, 1, 4, 5):
                raise ValueError("%s: texgen %d reads %s (only position, normal, tex0, tex1 are kept)" % (
                    mat.name, i, g["src_name"]))
            used = (s["flag"] & 1) or s["map_mode_id"] != 0
            tg = M.texgens[i]
            tg.type, tg.source, tg.normalize = g["type"], g["src"], int(g["normalize"])
            tg.matrix = i if used else NONE16
            tg.post_mtx = i if g["normalize"] and g["post"] is not None else NONE16
        for i in range(mat.n_texgens, 8):
            M.texgens[i].matrix = M.texgens[i].post_mtx = NONE16
        self.pkg.materials.append(M)
        idx = len(self.pkg.materials) - 1
        self.mat[key] = idx
        self.stats["materials"] += 1
        return idx

    # ---------------------------------------------------------------- models
    def model(self, r, mdl, name=None, hidden=(), tex_override=None, extra_root=None):
        """A Model of `mdl`: its nodes (rest SRT, bind matrix, visibility, billboard) and a batch per
        draw (opaque list, then translucent), vertices in the space of the node they hang from.
        Draws whose visibility node is in `hidden` are left out (the node stays, invisible).
        extra_root: a name for a node put above the model's root (index 0), for a motion the
        model's own nodes don't have (a path, an orbit); the model's nodes follow it.
        Returns (model index, {node name: index within the model})."""
        pkg = self.pkg
        shift = 1 if extra_root else 0
        node_map = {}
        first_node = len(pkg.nodes)
        if extra_root:
            pkg.add("nodes", name=pkg.str(extra_root), parent=NONE16, flags=ng3d.NODE_VISIBLE,
                    scale=[1.0] * 3, bind=[[1.0, 0, 0, 0], [0, 1.0, 0, 0], [0, 0, 1.0, 0]])
            node_map[extra_root] = 0
        for n in mdl.nodes:
            fl = ng3d.NODE_VISIBLE if n.name not in hidden and n.flags & 0x100 else 0
            if n.billboard:
                fl |= ng3d.NODE_BILLBOARD
            par = n.parent + shift if n.parent >= 0 else (0 if extra_root else NONE16)
            pkg.add("nodes", name=pkg.str(n.name), parent=par, flags=fl, scale=list(n.scale),
                    rotate=list(n.rot), translate=list(n.trans),
                    bind=[list(map(float, row)) for row in n.mtx], billboard=n.billboard)
            node_map[n.name] = n.index + shift
        W = mdl.world_matrices()
        mats = {m.index: m for m in mdl.materials}
        shapes = {s.index: s for s in mdl.shapes}
        draws = [d for d in mdl.draws("DrawOpa")] + [d for d in mdl.draws("DrawXlu")]
        first_batch = len(pkg.batches)
        lo, hi = np.full(3, np.inf), np.full(3, -np.inf)
        per_node = {}
        for d in draws:
            vis = mdl.nodes[d.node].name if 0 <= d.node < len(mdl.nodes) else None
            if vis in hidden:
                self.stats["model draws hidden"] += 1
                continue
            mat = mats[d.mat]
            mi = self.material(r, mdl, mat, tex_override, d.prio)
            t = mdl.shape_triangles(shapes[d.shape], world=False)
            if not t or "pos" not in t:
                continue
            # a batch per matrix the triangles use (rigid multi-bone shapes: a triangle spanning
            # bones goes with the bone most of its corners use, its other corners moved into that
            # bone's space at the rest pose)
            mtx = t["mtx"].reshape(-1, 3)
            maj = np.where(mtx[:, 1] == mtx[:, 2], mtx[:, 1], mtx[:, 0])
            for mid in np.unique(maj):
                sel = np.repeat(maj == mid, 3)
                node = mdl.matrix_node(int(mid))
                c = {k: v[sel] for k, v in t.items()}
                mixed = c["mtx"] != mid
                if mixed.any():
                    self.stats["triangles spanning bones (rest pose)"] += int(mixed.reshape(-1, 3).any(1).sum())
                    inv = np.linalg.inv(W[node]) if node >= 0 else np.eye(4)
                    for om in np.unique(c["mtx"][mixed]):
                        s2 = c["mtx"] == om
                        Mx = inv @ (W[mdl.matrix_node(int(om))] if mdl.matrix_node(int(om)) >= 0 else np.eye(4))
                        c["pos"][s2] = c["pos"][s2] @ Mx[:3, :3].T.astype(np.float32) + Mx[:3, 3].astype(np.float32)
                        if "nrm" in c:
                            c["nrm"][s2] = c["nrm"][s2] @ Mx[:3, :3].T.astype(np.float32)
                corners = corners_from(c, mdl, shapes[d.shape])
                b = build_single_batch(self, corners, mi, node + shift if node >= 0 else 0)
                per_node.setdefault(node + shift, []).append(b)
                Wn = W[node] if node >= 0 else np.eye(4)
                P = c["pos"] @ Wn[:3, :3].T + Wn[:3, 3]
                lo, hi = np.minimum(lo, P.min(0)), np.maximum(hi, P.max(0))
        n_batches = len(pkg.batches) - first_batch
        # a node's batches, when they are one run in the model's draw order (else none: the
        # renderer goes by Batch.node)
        for k in range(len(node_map)):
            idx = sorted(per_node.get(k, []))
            nd = pkg.nodes[first_node + k]
            if idx and idx[-1] - idx[0] + 1 == len(idx):
                nd.first_batch, nd.n_batches = idx[0], len(idx)
            else:
                nd.first_batch, nd.n_batches = (idx[0] if idx else first_batch), 0
                if idx:
                    self.stats["nodes with batches not in one run"] += 1
        if not np.isfinite(lo).all():
            lo = hi = np.zeros(3)
        bbox = [float(x) for x in list(lo) + list(hi)]
        pkg.add("models", name=pkg.str(name or mdl.name), first_node=first_node, n_nodes=len(node_map),
                first_batch=first_batch, n_batches=n_batches, bbox=bbox,
                radius=float(np.linalg.norm(hi - lo) / 2), lod_next=NONE16)
        self.stats["models"] += 1
        return len(pkg.models) - 1, node_map


# ------------------------------------------------------------------------------------- meshes


def array_frac(a):
    """The fraction bits a vertex array's values are exact at (None for f32)."""
    if a is None or a.fmt == 4:
        return None
    return a.frac


def corners_from(t, mdl, shape):
    """shape_triangles() output -> corners: pos, nrm, clr, uv0, uv1 and the UV arrays' fractions."""
    c = dict(pos=np.asarray(t["pos"], np.float32))
    if "nrm" in t:
        c["nrm"] = np.asarray(t["nrm"], np.float32)
    if "clr0" in t:
        c["clr"] = np.asarray(t["clr0"], np.uint8)
    fr = []
    for i in range(2):
        if "uv%d" % i in t:
            c["uv%d" % i] = np.asarray(t["uv%d" % i], np.float32)
            fr.append(array_frac(mdl.array("uv", shape.uv_id[i])))
        else:
            fr.append(None)
    c["uvfrac"] = fr
    return c


def transform_corners(c, M):
    """Corners moved by a 3x4 / 4x4 matrix (normals by its inverse transpose, renormalised)."""
    M = np.asarray(M, np.float64)
    out = dict(c)
    out["pos"] = (c["pos"] @ M[:3, :3].T + M[:3, 3]).astype(np.float32)
    if "nrm" in c:
        N = np.linalg.inv(M[:3, :3]).T
        n = c["nrm"] @ N.T
        n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
        out["nrm"] = n.astype(np.float32)
    return out


def _uv_mode(c):
    """('s16', frac0, frac1) or ('f32',): how a piece's UVs are stored exactly enough. s16 at the
    source's own fraction when the range allows (exact), else at the range's limit if that keeps
    at least 1/256 of a texture repeat, else f32."""
    fr = []
    for i in range(2):
        uv = c.get("uv%d" % i)
        if uv is None:
            fr.append(0)
            continue
        need = c["uvfrac"][i]
        if need is None:                   # f32 source: the fraction its values are exact at
            need = 14
            for f in range(0, 15):
                if np.all(np.abs(uv * (1 << f) - np.rint(uv * (1 << f))) < 1e-6):
                    need = f
                    break
        m = float(np.abs(uv).max()) if len(uv) else 0.0
        limit = 15 if m == 0 else int(math.floor(math.log2(32767.0 / m)))
        if limit < need and limit < 8:
            return ("f32",)
        fr.append(min(need, limit, 15))
    return ("s16", fr[0], fr[1])


def stride_of(attrs, mode):
    """Bytes a vertex: position f32 x3, normal s8 x3 + pad, colour u8 x4, each UV s16 x2 or f32 x2."""
    uv = 8 if mode[0] == "f32" else 4
    return 12 + (4 if attrs & ng3d.VA_NRM else 0) + (4 if attrs & ng3d.VA_CLR else 0) + \
        (uv if attrs & ng3d.VA_UV0 else 0) + (uv if attrs & ng3d.VA_UV1 else 0)


def layout_of(c, mode):
    attrs = ng3d.VA_POS
    for k, bit in (("nrm", ng3d.VA_NRM), ("clr", ng3d.VA_CLR), ("uv0", ng3d.VA_UV0), ("uv1", ng3d.VA_UV1)):
        if k in c:
            attrs |= bit
    return attrs, stride_of(attrs, mode)


def encode_vertices(c, attrs, mode):
    """Corners -> (n, stride) uint8 rows in VertexBuffer layout (POS, NRM, CLR, UV0, UV1)."""
    n = len(c["pos"])
    cols = [np.ascontiguousarray(c["pos"], "<f4").view(np.uint8).reshape(n, 12)]
    if attrs & ng3d.VA_NRM:
        q = np.clip(np.rint(c["nrm"] * 127.0), -127, 127).astype(np.int8)
        cols.append(np.concatenate([q, np.zeros((n, 1), np.int8)], 1).view(np.uint8))
    if attrs & ng3d.VA_CLR:
        cols.append(np.ascontiguousarray(c["clr"], np.uint8))
    for i, bit in ((0, ng3d.VA_UV0), (1, ng3d.VA_UV1)):
        if attrs & bit:
            uv = c["uv%d" % i]
            if mode[0] == "f32":
                cols.append(np.ascontiguousarray(uv, "<f4").view(np.uint8).reshape(n, 8))
            else:
                q = np.clip(np.rint(uv * (1 << mode[1 + i])), -32768, 32767).astype("<i2")
                cols.append(q.view(np.uint8).reshape(n, 4))
    return np.ascontiguousarray(np.concatenate(cols, 1))


class _Vbuf:
    def __init__(self, attrs, stride, mode):
        self.attrs, self.stride, self.mode = attrs, stride, mode
        self.rows = {}
        self.order = []


def _flush_vbuf(cv, vb):
    pkg = cv.pkg
    data = b"".join(vb.order)
    scale = [0.0, 0.0]
    flags = 0
    if vb.mode[0] == "f32":
        flags = ng3d.VB_UV_FLOAT
    else:
        for i, bit in ((0, ng3d.VA_UV0), (1, ng3d.VA_UV1)):
            if vb.attrs & bit:
                scale[i] = 1.0 / (1 << vb.mode[1 + i])
    pkg.add("vertex_buffers", count=len(vb.order), attrs=vb.attrs, stride=vb.stride, flags=flags, uv_scale=scale)
    pkg.vertex_data.append(data)
    cv.stats["vertices"] += len(vb.order)
    cv.stats["vertex bytes"] += len(data)
    return len(pkg.vertex_buffers) - 1


def _index_into(vb, rows):
    """VB-local indices of encoded rows (deduplicated, first come first numbered)."""
    out = np.empty(len(rows), np.int64)
    for k, x in enumerate(rows):
        r = x.tobytes()
        i = vb.rows.get(r)
        if i is None:
            i = vb.rows[r] = len(vb.order)
            vb.order.append(r)
        out[k] = i
    return out


def _new_unique(vb, rows):
    return len({x.tobytes() for x in rows} - vb.rows.keys())


def build_single_batch(cv, c, material, node):
    """One batch in its own vertex buffer (model meshes)."""
    mode = _uv_mode(c)
    attrs, stride = layout_of(c, mode)
    rows = encode_vertices(c, attrs, mode)
    vb = _Vbuf(attrs, stride, mode)
    idx = _index_into(vb, rows)
    if len(vb.order) > 65536:
        raise ValueError("a model mesh of %d vertices" % len(vb.order))
    vbi = _flush_vbuf(cv, vb)
    return _add_batch(cv, material, vbi, idx, c["pos"], node)


def _add_batch(cv, material, vbi, idx, pos, node):
    pkg = cv.pkg
    first = len(pkg.indices)
    pkg.indices.extend(int(i) for i in idx)
    lo, hi = pos.min(0), pos.max(0)
    pkg.add("batches", material=material, vbuf=vbi, first_index=first, n_indices=len(idx),
            bbox=[float(x) for x in list(lo) + list(hi)], node=node)
    cv.stats["triangles"] += len(idx) // 3
    return len(pkg.batches) - 1


class MeshSet:
    """Static geometry gathered as pieces (group key, material, corners in world space, tag), built
    into one batch per (group, material, tag) -- tag tells pieces that must stay apart (detail
    levels) -- and one vertex buffer per (group, vertex layout), split before 65,536 vertices."""

    def __init__(self, cv):
        self.cv = cv
        self.pieces = {}

    def add(self, group, material, corners, tag=0):
        mode = _uv_mode(corners)
        attrs, stride = layout_of(corners, mode)
        key = (group, material, tag, attrs, mode)
        self.pieces.setdefault(key, []).append(corners)

    def build(self):
        """-> {group: [(batch index, material, tag)]} in a stable order (opaque materials first)."""
        cv, pkg = self.cv, self.cv.pkg
        out = {}
        by_vb = {}
        for key in sorted(self.pieces, key=lambda k: (k[0], k[3], str(k[4]), k[1], k[2])):
            by_vb.setdefault((key[0], key[3], key[4]), []).append(key)
        for (group, attrs, mode), keys in by_vb.items():
            stride = stride_of(attrs, mode)
            vb = _Vbuf(attrs, stride, mode)
            pending = []
            for key in keys:
                parts = self.pieces[key]
                c = {k: np.concatenate([p[k] for p in parts]) for k in parts[0] if k != "uvfrac"}
                rows = encode_vertices(c, attrs, mode)
                if len(vb.order) + _new_unique(vb, rows) > 65535:
                    vbi = _flush_vbuf(cv, vb)
                    for (k2, idx2, pos2) in pending:
                        b = _add_batch(cv, k2[1], vbi, idx2, pos2, NONE16)
                        out.setdefault(k2[0], []).append((b, k2[1], k2[2]))
                    vb, pending = _Vbuf(attrs, stride, mode), []
                pending.append((key, _index_into(vb, rows), c["pos"]))
            if pending:
                vbi = _flush_vbuf(cv, vb)
                for (k2, idx2, pos2) in pending:
                    b = _add_batch(cv, k2[1], vbi, idx2, pos2, NONE16)
                    out.setdefault(k2[0], []).append((b, k2[1], k2[2]))
        return out


# ------------------------------------------------------------------------------------- animations

def curve_keys(curve, time_scale=1.0, frames=None):
    """A brres.Curve -> (interp, [(frame, value, slope)]) with frames multiplied by time_scale."""
    if curve.kind == "const":
        return ng3d.IP_LINEAR, [(0.0, float(curve.value), 0.0)]
    if curve.kind == "frames":
        v = [float(x) for x in curve.frames]
        keep = [0] + [i for i in range(1, len(v) - 1) if v[i] - v[i - 1] != v[i + 1] - v[i]] + [len(v) - 1]
        if len(v) == 1:
            keep = [0]
        keys = [(i * time_scale, v[i], 0.0) for i in keep]
        if all(k[1] == keys[0][1] for k in keys):
            keys = keys[:1]
        return ng3d.IP_LINEAR, keys
    keys = [(f * time_scale, v, s / time_scale) for f, v, s in curve.keys]
    if all(k[1] == keys[0][1] and k[2] == 0 for k in keys):
        return ng3d.IP_LINEAR, keys[:1]
    return ng3d.IP_HERMITE, keys


def repeat_keys(keys, period, times):
    """Keys of a loop of `period` frames laid end to end `times` times (a key on a seam that
    repeats the one before it is kept once)."""
    if len(keys) == 1 or times <= 1:
        return keys
    out = []
    for r in range(times):
        for f, v, s in keys:
            k = (f + r * period, v, s)
            if out and abs(out[-1][0] - k[0]) < 1e-6 and out[-1][1] == k[1] and out[-1][2] == k[2]:
                continue
            out.append(k)
    return out


class AnimBuilder:
    """Collects one Anim's tracks; end() writes Anim, Track and Key records."""

    def __init__(self, pkg, name, frames, loop=True):
        self.pkg, self.name, self.frames, self.loop = pkg, name, float(frames), loop
        self.tracks = []

    def add(self, target, index, keys, interp, reg=0, model=NONE16):
        self.tracks.append((target, interp, reg, index, model, keys))

    def add_curve(self, target, index, curve, reg=0, model=NONE16, time_scale=1.0, period=None):
        """A curve (time-scaled, then repeated to fill the anim if it loops in `period` frames)."""
        interp, keys = curve_keys(curve, time_scale)
        if period and len(keys) > 1:
            times = int(round(self.frames / period))
            keys = repeat_keys(keys, period, times)
        self.add(target, index, keys, interp, reg, model)

    def end(self):
        pkg = self.pkg
        first = len(pkg.tracks)
        for target, interp, reg, index, model, keys in self.tracks:
            fk = len(pkg.keys)
            for f, v, s in keys:
                pkg.add("keys", frame=float(f), value=float(v), slope=float(s))
            pkg.add("tracks", target=target, interp=interp, reg=reg, index=index, model=model, first_key=fk,
                    n_keys=len(keys))
        pkg.add("anims", name=pkg.str(self.name), frames=self.frames, loop=1 if self.loop else 0,
                first_track=first, n_tracks=len(self.tracks))
        return len(pkg.anims) - 1


# nw4r CLR0 target -> ColorReg (the format skips 7 so that the konst colours start at 8)
CLR_TARGET_REG = {0: ng3d.CR_MAT0, 1: ng3d.CR_MAT1, 2: ng3d.CR_AMB0, 3: ng3d.CR_AMB1, 4: ng3d.CR_TEV0,
                  5: ng3d.CR_TEV1, 6: ng3d.CR_TEV2, 7: ng3d.CR_KONST0, 8: ng3d.CR_KONST1, 9: ng3d.CR_KONST2,
                  10: ng3d.CR_KONST3}


def add_clr0(ab, anim, material_of, period=None):
    """CLR0 colour targets as TT_MAT_REG_* tracks, one per animated component (a component whose
    mask byte is set keeps the material's own value: no track). material_of(name) -> index or None."""
    n = 0
    for mname, targets in anim.materials.items():
        mi = material_of(mname)
        if mi is None:
            continue
        for t, x in targets.items():
            for comp in range(4):
                if (x["mask"] >> (24 - 8 * comp)) & 0xFF:
                    continue
                vals = [((c >> (24 - 8 * comp)) & 0xFF) for c in x["values"]]
                curve = brres.Curve.const(vals[0]) if len(vals) == 1 else brres.Curve("frames", frames=vals)
                ab.add_curve(ng3d.TT_MAT_REG_R + comp, mi, curve, reg=CLR_TARGET_REG[t], period=period)
                n += 1
    return n


def add_srt0(ab, anim, texmtx_of, period=None):
    """SRT0 texture matrices as TT_TEXMTX_* tracks (all five components: nw4r sets the whole
    matrix while animating it). texmtx_of(material name, matrix number) -> TexMtx index or None."""
    n = 0
    for mname, mtxs in anim.materials.items():
        for j, x in mtxs.items():
            if not isinstance(j, int):
                continue
            ti = texmtx_of(mname, j)
            if ti is None:
                continue
            for target, curve in ((ng3d.TT_TEXMTX_SCALE_S, x["scale"][0]), (ng3d.TT_TEXMTX_SCALE_T, x["scale"][1]),
                                  (ng3d.TT_TEXMTX_ROTATE, x["rot"]), (ng3d.TT_TEXMTX_TRANS_S, x["trans"][0]),
                                  (ng3d.TT_TEXMTX_TRANS_T, x["trans"][1])):
                ab.add_curve(target, ti, curve, reg=j, period=period)
                n += 1
    return n


def add_chr0(ab, anim, model_index, node_map, mdl=None, rename=None, time_scale=1.0, period=None,
             override=None):
    """CHR0 node tracks as TT_NODE_* (nine a node). node_map: node name -> index in the model;
    rename maps an animated name to a node name (an anim made for another node); a part the node
    data leaves out is the model's rest value ('model'), or identity. override: {(node name,
    target): constant} replaces a component."""
    n = 0
    rest = {nd.name: nd for nd in (mdl.nodes if mdl else [])}
    for nname, nd in anim.nodes.items():
        tgt = (rename or {}).get(nname, nname)
        if tgt not in node_map:
            continue
        fl = nd["flags"]
        for part, base in (("scale", ng3d.TT_NODE_SCALE_X), ("rot", ng3d.TT_NODE_ROT_X),
                           ("trans", ng3d.TT_NODE_TRANS_X)):
            curves = nd[part]
            # a part the node data leaves out: identity if the flags say so (2 identity, 4
            # rotation and translation zero, 8 scale one), else the model's rest value
            ident = fl & 2 or (part == "scale" and fl & 8) or (part != "scale" and fl & 4)
            for k in range(3):
                target = base + k
                if override and (tgt, target) in override:
                    c = brres.Curve.const(override[(tgt, target)])
                elif isinstance(curves, list):
                    c = curves[k]
                elif not ident and tgt in rest:
                    c = brres.Curve.const({"scale": rest[tgt].scale, "rot": rest[tgt].rot,
                                           "trans": rest[tgt].trans}[part][k])
                else:
                    c = brres.Curve.const(1.0 if part == "scale" else 0.0)
                ab.add_curve(target, node_map[tgt], c, model=model_index, time_scale=time_scale, period=period)
                n += 1
    return n
