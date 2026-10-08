"""BRRES -> NG3D: the general part of staging (textures, materials, meshes, models, animations).

    pkg = ng3d.Package()
    cv = Converter(pkg)
    m = cv.model(brres, brres.models["WS2_common_sea"])          # a Model with its nodes and meshes
    cells = MeshSet(cv)                                          # static geometry, batched by key
    cells.add(key, cv.material(brres, mdl, mat), corners_world)  # ... then cells.build()
    ab = AnimBuilder(pkg, "name", frames=200, loop=True)         # tracks from CHR0/SRT0/CLR0 curves
    widen_model_bounds(pkg)                                      # once the instances are placed

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

Geometry (NG3D version 2): every position is snapped to a grid of POS_QUANTUM units (in the space it
is stored in) and a vertex buffer whose positions fit in s16 steps of it about a point of the grid is
written VA_POS16 (the pos_origin and pos_scale the header has): exact against the snapped values,
so two buffers that share a vertex (neighbouring cells, two materials of a cell) give it the same
place and no crack opens between them; buffers too wide for it (the open sea, the sky) stay f32.
Given a Lighting, a material lit by colour channel 0 whose lighting may be taken as static becomes
MAT_PRELIT: its lighting (preview.py's model of GX's: per vertex, the material's light set, point
lights at their positions) is baked into the vertex colours, and it is written as an unlit
material whose channel colour is the vertex's. A vertex buffer carries normals only when a material
of it needs them (unbaked lighting, environment mapping), colours only when one reads them.
"""
import math
import os
import sys
from collections import Counter, namedtuple

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

# the grid staged positions are snapped to, and VA_POS16's step: a buffer is written s16 when its
# positions lie within 32,767 steps of a grid point (5,461 units across: a cell of 4,000 with the
# triangles whose centroids are in it, 5,208 at most on the island); the snap moves a coordinate by
# at most half a step (0.042 units, 4 mm at k = 10). One step for every buffer, so a vertex two
# buffers share decodes to the same place in both (to float32's rounding of s16 * step + origin)
POS_QUANTUM = 1.0 / 12.0
POS16_SPAN = 2 * 32767 * POS_QUANTUM - POS_QUANTUM


def snap_positions(p):
    """Positions on the POS_QUANTUM grid (float32)."""
    return (np.rint(np.asarray(p, np.float64) / POS_QUANTUM) * POS_QUANTUM).astype(np.float32)


class Lighting:
    """The static lighting a MAT_PRELIT material's vertex colours take: the scene's lights and light
    sets (as the package's LITE and LSET records have them), evaluated per vertex as
    tools/g3d/preview.py's Scene.channel() does per fragment -- colour = material source x clamp(
    ambient + sum of N.L x light colour), the light set's ambient and lights replacing the
    material's, point lights from their positions (GX's per-vertex direction; World lights the
    package's point lights from their direction toward the origin), alpha = source x ambient alpha.
    lights: [(kind, position, direction, rgba)]; sets: {index: (ambient rgba, [light indices])}."""

    def __init__(self, lights, sets, ambient_shadow_boxes=()):
        self.lights, self.sets = lights, sets
        # Optional world-space boxes used for WSR's shadow areas. Each row is the inverse
        # transform of a unit box in PMP space (centered at the origin, half-width 1).
        self.ambient_shadow_boxes = [(np.asarray(m, np.float64), np.asarray(half_extent, np.float64))
                                     for m, half_extent in ambient_shadow_boxes]

    def ambient_shadow(self, pos):
        """Fade only the ambient RGB inside shadow-area boxes, with a short inward feather."""
        factor = np.ones(len(pos), np.float64)
        for inv, half_extent in self.ambient_shadow_boxes:
            local = np.asarray(pos, np.float64) @ inv[:3, :3].T + inv[:3, 3]
            inside = np.all(np.abs(local) <= 1.0, axis=1)
            if not inside.any():
                continue
            # The last five world units inside the box blend into its shaded interior.
            edge = np.min((1.0 - np.abs(local)) * half_extent[None, :], axis=1)
            t = np.clip(edge / 5.0, 0.0, 1.0)
            t = t * t * (3.0 - 2.0 * t)
            factor[inside] = np.minimum(factor[inside], 1.0 - 0.45 * t[inside])
        return factor

    def channel(self, word, light_set, matc, ambc, pos, nrm, clr, comps):
        n = len(pos)
        src = clr[:, comps] if word & 1 and clr is not None else np.broadcast_to(matc[comps], (n, len(comps)))
        if not word & 2:
            return src
        if light_set is not None and light_set in self.sets:
            amb_rgba, idx = self.sets[light_set]
            amb = np.broadcast_to(np.asarray(amb_rgba, np.float64)[comps] / 255.0, (n, len(comps)))
            lights = [self.lights[i] for i in idx]
        else:
            amb = clr[:, comps] if word & 0x40 and clr is not None else np.broadcast_to(ambc[comps], (n, len(comps)))
            lights = []
        light = np.array(amb, np.float64)
        if comps[0] == 0 and self.ambient_shadow_boxes:
            light *= self.ambient_shadow(pos)[:, None]
        if comps[0] == 0 and nrm is not None:
            fn = (word >> 7) & 3
            for kind, lpos, ldir, rgba in lights:
                if kind == 1:          # LK_POINT
                    d = np.asarray(lpos, np.float64)[None, :] - pos
                    d /= np.maximum(np.linalg.norm(d, axis=1, keepdims=True), 1e-9)
                    dot = (nrm * d).sum(1)
                else:
                    dot = nrm @ -np.asarray(ldir, np.float64)
                if fn == 0:
                    dot = np.ones_like(dot)
                elif fn >= 2:
                    dot = np.maximum(dot, 0)
                light += dot[:, None] * (np.asarray(rgba[:3], np.float64) / 255.0)[None, :]
        return src * np.clip(light, 0, 1)

    def bake(self, m, light_set, pos, nrm, clr):
        """u8 RGBA per vertex: channel 0 of material record `m` (its words before prelighting),
        lit by light set `light_set` (None: the material's own ambient, no lights)."""
        pos = np.asarray(pos, np.float64)
        nrm = None if nrm is None else np.asarray(nrm, np.float64)
        if nrm is not None:
            nrm = nrm / np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-9)
        c = None if clr is None else np.asarray(clr, np.float64) / 255.0
        matc = np.asarray(m.mat_color, np.float64) / 255.0
        ambc = np.asarray(m.amb_color, np.float64) / 255.0
        rgb = self.channel(m.chan_color, light_set, matc, ambc, pos, nrm, c, [0, 1, 2])
        a = self.channel(m.chan_alpha, light_set, matc, ambc, pos, nrm, c, [3])
        out = np.concatenate([rgb, a], 1)
        return np.clip(np.rint(out * 255.0), 0, 255).astype(np.uint8)


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


# a model drawn as a part of another (Converter.model's parts)
Part = namedtuple("Part", "name brres model matrix hidden tex_override", defaults=(None, (), None))


class Converter:
    """Makes package records from BRRES objects, each once (textures by (file, name), materials
    by (file, model, material)).

    lighting: a Lighting to bake static lighting with (MAT_PRELIT), or None (materials stay lit at
    run time). keep_lit: (model name, material name) pairs whose lighting must stay live (an
    animation drives their material or ambient colour). pos16: write VA_POS16 where it fits."""

    def __init__(self, pkg, lighting=None, keep_lit=(), pos16=True):
        self.pkg = pkg
        self.tex = {}
        self.mat = {}
        self.stats = Counter()
        self.lighting = lighting
        self.keep_lit = set(keep_lit)
        self.pos16 = pos16
        self.mat_info = {}           # material index -> what its vertices need (vertex_needs)
        self.snap_error = 0.0        # the most the snap moved a coordinate (units)
        self.snap_error_3d = 0.0     # ... a position (its distance)

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
            tg.matrix = g.get("matrix", i) if used else NONE16
            tg.post_mtx = i if g["normalize"] and g["post"] is not None else NONE16
        for i in range(mat.n_texgens, 8):
            M.texgens[i].matrix = M.texgens[i].post_mtx = NONE16
        info = vertex_needs(M, self.pkg.tex_mtx[M.tex_mtx_first:M.tex_mtx_first + M.n_tex_mtx])
        lit = info["lit"]
        if lit and self.lighting is not None and (mdl.name, mat.name) not in self.keep_lit:
            # its lighting baked into the vertex colours: the words as they were, for the bake;
            # then an unlit channel taking the vertex colour (colour and alpha), no light set
            info.update(prelit=True, words=Rec("Material", chan_color=M.chan_color, chan_alpha=M.chan_alpha,
                                               mat_color=list(M.mat_color), amb_color=list(M.amb_color)),
                        light_set=M.light_set if M.flags & ng3d.MAT_LIGHT_SET else None, clr=True)
            info["nrm"] = info["env"]
            M.flags = (M.flags | ng3d.MAT_PRELIT) & ~ng3d.MAT_LIGHT_SET
            M.chan_color = (M.chan_color & ~2) | 1
            M.chan_alpha = (M.chan_alpha & ~2) | 1
            self.stats["materials prelit (lighting in the vertex colours)"] += 1
        elif lit:
            self.stats["materials lit at run time"] += 1
        self.pkg.materials.append(M)
        idx = len(self.pkg.materials) - 1
        self.mat[key] = idx
        self.mat_info[idx] = info
        self.stats["materials"] += 1
        return idx

    # ---------------------------------------------------------------- vertices
    def prepare(self, c, material, to_world=None):
        """Corners made ready for `material`'s batch: its lighting baked into the colours (MAT_PRELIT;
        to_world: a 4x4 taking the corners' space to the one the lights are in, or None), the
        normals and colours dropped if nothing reads them, the positions snapped to the grid."""
        info = self.mat_info[material]
        out = dict(c)
        if info.get("prelit"):
            P, N = c["pos"], c.get("nrm")
            if to_world is not None:
                W = np.asarray(to_world, np.float64)
                P = np.asarray(P, np.float64) @ W[:3, :3].T + W[:3, 3]
                if N is not None:
                    N = np.asarray(N, np.float64) @ np.linalg.inv(W[:3, :3])
            if N is None:
                self.stats["prelit corners without normals (lit by the ambient alone)"] += len(P)
            out["clr"] = self.lighting.bake(info["words"], info["light_set"], P, N, c.get("clr"))
        if not info["nrm"]:
            out.pop("nrm", None)
        if not info["clr"]:
            out.pop("clr", None)
        snapped = snap_positions(c["pos"])
        if len(snapped):
            d = snapped - np.asarray(c["pos"], np.float64)
            self.snap_error = max(self.snap_error, float(np.abs(d).max()))
            self.snap_error_3d = max(self.snap_error_3d, float(np.linalg.norm(d, axis=1).max()))
        out["pos"] = snapped
        return out

    # ---------------------------------------------------------------- models
    def model(self, r, mdl, name=None, hidden=(), tex_override=None, extra_root=None, parts=(), detail=None,
              select=None, split=()):
        """A Model of `mdl`: its nodes (rest SRT, bind matrix, visibility, billboard) and a batch per
        draw (opaque list, then translucent), vertices in the space of the node they hang from.
        Draws whose visibility node is in `hidden` are left out (the node stays, invisible).
        extra_root: a name for a node put above the model's root (index 0), for a motion the
        model's own nodes don't have (a path, an orbit); the model's nodes follow it.
        parts: other models drawn as parts of this one ([Part]): each hangs from a node of the
        part's name under the model's first node (its root, or extra_root), placed in the model's
        space by its matrix (uniform scale, rotation, translation; None: as it is), and its draws
        follow this model's in each list (every part's opaque draws after this model's, then the
        translucent ones likewise) -- the sun in the sky dome, which the renderer moves with the
        eye as one model; a stretch of the sea with the island it rings. A part's nodes are in the
        map as "part/node"; a name the map already has is an error.
        detail(brres model, brres material) -> True marks a draw's batches BATCH_DETAIL; select(brres
        model, draw, triangles (n, 3, 3) in the model's space) -> which of a draw's triangles to keep.
        split: [(name, pivot, pick)]: rigid pieces cut out of the model's own draws onto nodes of their
        own (a car's wheels, to turn at run time): each a node under the model's root, at `pivot`
        (model space, unrotated), taking the triangles pick((n, 3, 3) in the model's space) -> bool
        says (the first that picks one has it), their positions about the pivot; at rest the piece is
        where it was, lit as it was.
        Returns (model index, {node name: index within the model})."""
        pkg = self.pkg
        shift = 1 if extra_root else 0
        node_map = {}
        first_node = len(pkg.nodes)

        def name_node(key, k):
            if key in node_map:
                raise ValueError("model %s: two nodes named %s" % (name or mdl.name, key))
            node_map[key] = k
        if extra_root:
            pkg.add("nodes", name=pkg.str(extra_root), parent=NONE16, flags=ng3d.NODE_VISIBLE,
                    scale=[1.0] * 3, bind=[[1.0, 0, 0, 0], [0, 1.0, 0, 0], [0, 0, 1.0, 0]])
            name_node(extra_root, 0)

        def add_nodes(m, base, root_parent, M, prefix, hide):
            for n in m.nodes:
                fl = ng3d.NODE_VISIBLE if n.name not in hide and n.flags & 0x100 else 0
                if n.billboard:
                    fl |= ng3d.NODE_BILLBOARD
                par = n.parent + base if n.parent >= 0 else root_parent
                bind = n.mtx if M is None else (M @ np.vstack([n.mtx, [0, 0, 0, 1]]))[:3]
                pkg.add("nodes", name=pkg.str(n.name), parent=par, flags=fl, scale=list(n.scale),
                        rotate=list(n.rot), translate=list(n.trans),
                        bind=[list(map(float, row)) for row in bind], billboard=n.billboard)
                name_node(prefix + n.name, n.index + base)
        add_nodes(mdl, shift, 0 if extra_root else NONE16, None, "", hidden)
        # the sources of the batches: the model, then its parts (each under a node of its own)
        # (a shape on an envelope matrix has its vertices in its model's space: the model's first
        # node, or the part's own node)
        sources = [(r, mdl, shift, 0, np.eye(4), hidden, tex_override)]
        # the first node's bind matrix: a part node's rest SRT is relative to it (posed under an
        # animated parent, the part lands where its bind matrix puts it at rest)
        B0 = np.eye(4) if extra_root or not mdl.nodes else np.vstack([mdl.nodes[0].mtx, [0, 0, 0, 1]])
        for part in parts:
            M = np.eye(4) if part.matrix is None else np.asarray(part.matrix, np.float64)
            k = len(pkg.nodes) - first_node
            s, rot, t = srt_of(np.linalg.inv(B0) @ M)
            pkg.add("nodes", name=pkg.str(part.name), parent=0, flags=ng3d.NODE_VISIBLE, scale=[s] * 3,
                    rotate=rot, translate=t, bind=[list(map(float, row)) for row in M[:3]])
            name_node(part.name, k)
            add_nodes(part.model, k + 1, k, M, part.name + "/", part.hidden)
            sources.append((part.brres, part.model, k + 1, k, M, part.hidden, part.tex_override))
        # (under the model's own root, which an animation of the model moves: its rest SRT relative
        # to that node's bind matrix)
        split_nodes = []
        Br = np.vstack([mdl.nodes[0].mtx, [0, 0, 0, 1]]) if mdl.nodes else np.eye(4)
        for sname, pivot, _ in split:
            k = len(pkg.nodes) - first_node
            T = np.eye(4)
            T[:3, 3] = np.asarray(pivot, np.float64)
            s, rot, t = srt_of(np.linalg.inv(Br) @ T)
            pkg.add("nodes", name=pkg.str(sname), parent=shift, flags=ng3d.NODE_VISIBLE, scale=[s] * 3, rotate=rot,
                    translate=t, bind=[list(map(float, row)) for row in T[:3]])
            name_node(sname, k)
            split_nodes.append((k, T))
        n_nodes = len(pkg.nodes) - first_node
        draws = [(src, d) for lst in ("DrawOpa", "DrawXlu") for src in sources for d in src[1].draws(lst)]
        # each source's rest pose, materials and shapes, looked up by index
        rest = {id(src[1]): (src[1].world_matrices(), {m.index: m for m in src[1].materials},
                             {s.index: s for s in src[1].shapes}) for src in sources}
        first_batch = len(pkg.batches)
        lo, hi = np.full(3, np.inf), np.full(3, -np.inf)
        per_node = {}
        for (sr, sm, base, space, M, hide, tov), d in draws:
            vis = sm.nodes[d.node].name if 0 <= d.node < len(sm.nodes) else None
            if vis in hide:
                self.stats["model draws hidden"] += 1
                continue
            W, mats, shapes = rest[id(sm)]
            mi = self.material(sr, sm, mats[d.mat], tov, d.prio)
            shape = shapes[d.shape]
            t = sm.shape_triangles(shape, world=False)
            if not t or "pos" not in t:
                continue
            # a batch per matrix the triangles use (rigid multi-bone shapes: a triangle spanning
            # bones goes with the bone most of its corners use, its other corners moved into that
            # bone's space at the rest pose)
            mtx = t["mtx"].reshape(-1, 3)
            maj = np.where(mtx[:, 1] == mtx[:, 2], mtx[:, 1], mtx[:, 0])
            for mid in np.unique(maj):
                sel = np.repeat(maj == mid, 3)
                node = sm.matrix_node(int(mid))
                c = {k: v[sel] for k, v in t.items()}
                mixed = c["mtx"] != mid
                if mixed.any():
                    self.stats["triangles spanning bones (rest pose)"] += int(mixed.reshape(-1, 3).any(1).sum())
                    inv = np.linalg.inv(W[node]) if node >= 0 else np.eye(4)
                    for om in np.unique(c["mtx"][mixed]):
                        s2 = c["mtx"] == om
                        Mx = inv @ (W[sm.matrix_node(int(om))] if sm.matrix_node(int(om)) >= 0 else np.eye(4))
                        c["pos"][s2] = c["pos"][s2] @ Mx[:3, :3].T.astype(np.float32) + Mx[:3, 3].astype(np.float32)
                        if "nrm" in c:
                            c["nrm"][s2] = c["nrm"][s2] @ Mx[:3, :3].T.astype(np.float32)
                own = node + base if node >= 0 else space
                Wn = M @ (W[node] if node >= 0 else np.eye(4))
                if select is not None:
                    Pw = (c["pos"] @ Wn[:3, :3].T + Wn[:3, 3]).reshape(-1, 3, 3)
                    keep = np.repeat(np.asarray(select(sm, d, Pw), bool), 3)
                    if not keep.any():
                        continue
                    c = {k: v[keep] for k, v in c.items()}
                groups = [(own, Wn, c)]
                if split and sm is mdl:
                    # the split pieces' triangles onto their nodes, about their pivots (model space:
                    # the pieces' nodes don't turn at rest)
                    Pm = (c["pos"] @ Wn[:3, :3].T + Wn[:3, 3]).reshape(-1, 3, 3)
                    rest_of = np.ones(len(Pm), bool)
                    for (k, T), (_, pivot, pick) in zip(split_nodes, split):
                        m = np.asarray(pick(Pm), bool) & rest_of
                        if not m.any():
                            continue
                        rest_of &= ~m
                        cs = {key: v[np.repeat(m, 3)] for key, v in c.items()}
                        cs["pos"] = (Pm[m].reshape(-1, 3) - T[:3, 3]).astype(np.float32)
                        if "nrm" in cs:
                            n = cs["nrm"] @ Wn[:3, :3].T
                            cs["nrm"] = (n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)).astype(np.float32)
                        groups.append((k, T, cs))
                    keep = np.repeat(rest_of, 3)
                    groups[0] = (own, Wn, {key: v[keep] for key, v in c.items()})
                for gnode, gW, gc in groups:
                    if not len(gc["pos"]):
                        continue
                    # (lit as the model stands at rest, unplaced: a moving model carries its light)
                    corners = self.prepare(corners_from(gc, sm, shape), mi, gW)
                    b = build_single_batch(self, corners, mi, gnode, (detail or (lambda *_: False))(sm, mats[d.mat]))
                    per_node.setdefault(gnode, []).append(b)
                    P = corners["pos"] @ gW[:3, :3].T + gW[:3, 3]
                    lo, hi = np.minimum(lo, P.min(0)), np.maximum(hi, P.max(0))
        n_batches = len(pkg.batches) - first_batch
        # a node's batches, when they are one run in the model's draw order (else none: the
        # renderer goes by Batch.node)
        for k in range(n_nodes):
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
        pkg.add("models", name=pkg.str(name or mdl.name), first_node=first_node, n_nodes=n_nodes,
                first_batch=first_batch, n_batches=n_batches, bbox=bbox,
                radius=float(np.linalg.norm(hi - lo) / 2), lod_next=NONE16)
        self.stats["models"] += 1
        return len(pkg.models) - 1, node_map


def vertex_needs(M, tex_mtx):
    """What a material's vertices need: normals (lighting at run time, or a texgen from the normal
    or mapping the environment), colours (its channel reads the vertex colour). -> dict(lit, env,
    nrm, clr, prelit=False)."""
    lit = M.n_chans >= 1 and bool((M.chan_color | M.chan_alpha) & 2)
    env = False
    for g in M.texgens[:M.n_texgens]:
        tm = tex_mtx[g.matrix] if g.matrix != NONE16 and g.matrix < len(tex_mtx) else None
        if g.source in (1, 2, 3) or (tm is not None and tm.map_mode in (1, 3, 4)):
            env = True
    clr = M.n_chans >= 1 and any(w & 1 or (w & 0x42) == 0x42 for w in (M.chan_color, M.chan_alpha))
    return dict(lit=lit, env=env, nrm=lit or env, clr=clr, prelit=False)


def srt_of(M):
    """A 4x4 of uniform scale, rotation and translation as nw4r's node SRT: (scale, [rx, ry, rz]
    degrees for T * Rz * Ry * Rx * S, [tx, ty, tz])."""
    M = np.asarray(M, np.float64)
    s = float(np.linalg.norm(M[:3, 0]))
    R = M[:3, :3] / s
    ry = -math.asin(max(-1.0, min(1.0, R[2, 0])))
    rx, rz = math.atan2(R[2, 1], R[2, 2]), math.atan2(R[1, 0], R[0, 0])
    return s, [math.degrees(rx), math.degrees(ry), math.degrees(rz)], [float(x) for x in M[:3, 3]]


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


def _uv_need(c, i):
    """The fraction bits UV i of a piece is exact at (None: the piece has no UV i)."""
    uv = c.get("uv%d" % i)
    if uv is None:
        return None
    need = c["uvfrac"][i]
    if need is None:                   # f32 source: the fraction its values are exact at
        need = 14
        for f in range(0, 15):
            if np.all(np.abs(uv * (1 << f) - np.rint(uv * (1 << f))) < 1e-6):
                need = f
                break
    return need


def _uv_mode(c, pieces=None):
    """('s16', frac0, frac1) or ('f32',): how a piece's UVs (or all of `pieces`', the one encoding
    a batch's vertices share) are stored exactly enough. s16 at the source's own fraction when the
    range allows (exact), else at the range's limit if that keeps at least 1/256 of a texture
    repeat, else f32."""
    pieces = pieces or [c]
    fr = []
    for i in range(2):
        needs = [n for n in (_uv_need(p, i) for p in pieces) if n is not None]
        if not needs:
            fr.append(0)
            continue
        need = max(needs)
        m = max((float(np.abs(p["uv%d" % i]).max()) for p in pieces if len(p["uv%d" % i])), default=0.0)
        limit = 15 if m == 0 else int(math.floor(math.log2(32767.0 / m)))
        if limit < need and limit < 8:
            return ("f32",)
        fr.append(min(need, limit, 15))
    return ("s16", fr[0], fr[1])


def stride_of(attrs, mode):
    """Bytes a vertex: position f32 x3 (VA_POS16: s16 x3 + pad), normal s8 x3 + pad, colour u8 x4,
    each UV s16 x2 or f32 x2."""
    uv = 8 if mode[0] == "f32" else 4
    return (8 if attrs & ng3d.VA_POS16 else 12) + (4 if attrs & ng3d.VA_NRM else 0) + \
        (4 if attrs & ng3d.VA_CLR else 0) + (uv if attrs & ng3d.VA_UV0 else 0) + (uv if attrs & ng3d.VA_UV1 else 0)


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


def pos16_fit(lo, hi):
    """The grid point a buffer of positions in [lo, hi] is written about in VA_POS16, or None when
    they don't all lie within 32,767 steps of one."""
    lo, hi = np.asarray(lo, np.float64), np.asarray(hi, np.float64)
    if not len(lo) or float((hi - lo).max()) > POS16_SPAN:
        return None
    origin = np.rint((lo + hi) / 2 / POS_QUANTUM) * POS_QUANTUM
    if float(np.maximum(hi - origin, origin - lo).max()) / POS_QUANTUM > 32767:
        return None
    return origin


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
    attrs, stride, origin, pscale = vb.attrs, vb.stride, [0.0, 0.0, 0.0], 0.0
    n = len(vb.order)
    rows = np.frombuffer(data, np.uint8).reshape(n, vb.stride)
    P = rows[:, 0:12].copy().view("<f4").reshape(n, 3).astype(np.float64)
    o = pos16_fit(P.min(0), P.max(0)) if cv.pos16 and n else None
    if o is not None:
        # s16 steps of the grid about a grid point: the snapped positions exactly
        q = np.rint((P - o) / POS_QUANTUM)
        back = (q * POS_QUANTUM + o).astype(np.float32)
        if np.abs(back - P.astype(np.float32)).max() > 1e-6 * max(1.0, float(np.abs(P).max())):
            raise ValueError("VA_POS16 isn't exact for a buffer (positions off the grid?)")
        q16 = np.concatenate([q.astype("<i2"), np.zeros((n, 1), "<i2")], 1).view(np.uint8).reshape(n, 8)
        rows = np.ascontiguousarray(np.concatenate([q16, rows[:, 12:]], 1))
        data = rows.tobytes()
        attrs = (attrs & ~ng3d.VA_POS) | ng3d.VA_POS16
        stride -= 4
        origin, pscale = [float(x) for x in o], POS_QUANTUM
        cv.stats["vertex buffers VA_POS16"] += 1
    else:
        cv.stats["vertex buffers f32 positions (too wide for VA_POS16)"] += 1
        cv.stats["vertices with f32 positions"] += n
    pkg.add("vertex_buffers", count=n, attrs=attrs, stride=stride, flags=flags, uv_scale=scale,
            pos_origin=origin, pos_scale=pscale)
    pkg.vertex_data.append(data)
    cv.stats["vertices"] += n
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


def build_single_batch(cv, c, material, node, detail=False):
    """One batch in its own vertex buffer (model meshes); c: prepared corners (Converter.prepare)."""
    mode = _uv_mode(c)
    attrs, stride = layout_of(c, mode)
    rows = encode_vertices(c, attrs, mode)
    vb = _Vbuf(attrs, stride, mode)
    idx = _index_into(vb, rows)
    if len(vb.order) > 65536:
        raise ValueError("a model mesh of %d vertices" % len(vb.order))
    vbi = _flush_vbuf(cv, vb)
    return _add_batch(cv, material, vbi, idx, c["pos"], node, detail)


def _add_batch(cv, material, vbi, idx, pos, node, detail=False):
    pkg = cv.pkg
    first = len(pkg.indices)
    pkg.indices.extend(int(i) for i in idx)
    lo, hi = pos.min(0), pos.max(0)
    pkg.add("batches", material=material, vbuf=vbi, first_index=first, n_indices=len(idx),
            bbox=[float(x) for x in list(lo) + list(hi)], node=node, flags=ng3d.BATCH_DETAIL if detail else 0)
    cv.stats["triangles"] += len(idx) // 3
    if detail:
        cv.stats["detail batches (BATCH_DETAIL)"] += 1
    return len(pkg.batches) - 1


class MeshSet:
    """Static geometry gathered as pieces (group key, material, corners in world space, the detail
    levels it is in, detail), built into batches: the pieces of a (group, material, detail, vertex
    attributes) family share one encoding and one vertex buffer, and the family has a batch per
    distinct set of pieces its levels draw (a level draws every piece that is in it; two levels
    drawing the same pieces share a batch), so a group's level lists hold one batch a material (and
    detail). detail marks a batch BATCH_DETAIL. A group's vertex buffer holds its families of one
    layout, split before 65,536 vertices and where its positions would grow too wide for VA_POS16.
    add() prepares the corners for the material (Converter.prepare)."""

    def __init__(self, cv):
        self.cv = cv
        self.pieces = {}

    def add(self, group, material, corners, levels=(0,), detail=False):
        corners = self.cv.prepare(corners, material)
        attrs, _ = layout_of(corners, ("s16", 0, 0))
        key = (group, material, bool(detail), attrs)
        self.pieces.setdefault(key, []).append((tuple(sorted(set(levels))), corners))

    def absorb_small(self, max_triangles, neighbours, margin=0.0):
        """Small families (fewer than max_triangles; the slivers a centroid cut leaves where a
        material crosses a group's edge) moved into the largest family of the same material,
        detail, attributes and levels among neighbours(group), if one has it, the sliver lies within
        that group's box (grown by `margin`: the group's box, which picks its detail level and culls
        it, stays as it was) and the two together still fit VA_POS16. Returns how many moved."""
        moved = 0
        tris = {k: sum(len(c["pos"]) // 3 for _, c in v) for k, v in self.pieces.items()}
        gbox = {}
        for k, v in self.pieces.items():
            P = np.concatenate([c["pos"] for _, c in v])
            lo, hi = gbox.get(k[0], (np.full(3, np.inf), np.full(3, -np.inf)))
            gbox[k[0]] = (np.minimum(lo, P.min(0)), np.maximum(hi, P.max(0)))
        for key in sorted(self.pieces, key=lambda k: (tris[k], str(k))):
            parts = self.pieces.get(key)
            if not parts or tris[key] >= max_triangles:
                continue
            group, material, detail, attrs = key
            levels = {lv for lv, _ in parts}
            Pk = np.concatenate([c["pos"] for _, c in parts])
            best = None
            for g in neighbours(group):
                k2 = (g, material, detail, attrs)
                other = self.pieces.get(k2)
                if not other or {lv for lv, _ in other} != levels or tris[k2] < tris[key]:
                    continue
                if (Pk.min(0) < gbox[g][0] - margin).any() or (Pk.max(0) > gbox[g][1] + margin).any():
                    continue
                P = np.concatenate([c["pos"] for _, c in other + parts])
                if pos16_fit(P.min(0), P.max(0)) is None:
                    continue
                if best is None or tris[k2] > tris[best]:
                    best = k2
            if best is not None:
                self.pieces[best].extend(parts)
                tris[best] += tris[key]
                del self.pieces[key]
                moved += 1
        self.cv.stats["small families moved into a neighbour's (one draw fewer each)"] += moved
        return moved

    def build(self):
        """-> {group: [(batch index, material, levels)]} in a stable order."""
        cv, pkg = self.cv, self.cv.pkg
        out = {}
        # each family's encoding (one for all its pieces), then its vertex buffer by (group, layout)
        fams = []
        for key in sorted(self.pieces, key=lambda k: (k[0], k[3], k[1], k[2])):
            parts = self.pieces[key]
            mode = _uv_mode(None, [c for _, c in parts])
            fams.append((key, mode))
        by_vb = {}
        for key, mode in fams:
            by_vb.setdefault((key[0], key[3], mode), []).append(key)
        for (group, attrs, mode), keys in sorted(by_vb.items(), key=lambda kv: (kv[0][0], kv[0][1], str(kv[0][2]))):
            stride = stride_of(attrs, mode)
            vb = _Vbuf(attrs, stride, mode)
            vlo, vhi = np.full(3, np.inf), np.full(3, -np.inf)
            pending = []

            def flush():
                vbi = _flush_vbuf(cv, vb)
                for k2, sets in pending:
                    for levels, (idx2, pos2) in sets.items():
                        b = _add_batch(cv, k2[1], vbi, idx2, pos2, NONE16, k2[2])
                        out.setdefault(k2[0], []).append((b, k2[1], levels))
            for key in keys:
                parts = self.pieces[key]
                cs = [c for _, c in parts]
                c = {k: np.concatenate([p[k] for p in cs]) for k in cs[0] if k != "uvfrac"}
                rows = encode_vertices(c, attrs, mode)
                lo, hi = np.minimum(vlo, c["pos"].min(0)), np.maximum(vhi, c["pos"].max(0))
                # (a buffer that would grow too wide for VA_POS16 is closed first, unless its own
                # positions already are)
                wide = cv.pos16 and pending and pos16_fit(lo, hi) is None and pos16_fit(vlo, vhi) is not None
                if pending and (wide or len(vb.order) + _new_unique(vb, rows) > 65535):
                    if wide:
                        cv.stats["vertex buffers split to stay VA_POS16"] += 1
                    flush()
                    vb, pending = _Vbuf(attrs, stride, mode), []
                    lo, hi = c["pos"].min(0), c["pos"].max(0)
                idx = _index_into(vb, rows)
                # the pieces each level draws; a batch per distinct set
                spans, at = [], 0
                for lv, p in parts:
                    n = len(p["pos"])
                    spans.append((lv, at, at + n))
                    at += n
                sets = {}
                for level in sorted({x for lv, _ in parts for x in lv}):
                    sel = [(a, b) for lv, a, b in spans if level in lv]
                    key_set = tuple(sel)
                    found = next((k for k in sets if sets[k][2] == key_set), None)
                    if found is not None:
                        sets[found + (level,)] = sets.pop(found)
                        continue
                    ii = np.concatenate([idx[a:b] for a, b in sel])
                    pp = np.concatenate([c["pos"][a:b] for a, b in sel])
                    sets[(level,)] = (ii, pp, key_set)
                pending.append((key, {lv: (v[0], v[1]) for lv, v in sets.items()}))
                vlo, vhi = lo, hi
            if pending:
                flush()
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


def add_srt0(ab, anim, texmtx_of, period=None, time_scale=1.0, skip=()):
    """SRT0 texture matrices as TT_TEXMTX_* tracks (all five components: nw4r sets the whole
    matrix while animating it). texmtx_of(material name, matrix number) -> TexMtx index or None.
    time_scale stretches the curves (frames times it), period is the loop's length after that."""
    n = 0
    for mname, mtxs in anim.materials.items():
        for j, x in mtxs.items():
            if not isinstance(j, int):
                continue
            if (mname, j) in skip:
                continue
            ti = texmtx_of(mname, j)
            if ti is None:
                continue
            for target, curve in ((ng3d.TT_TEXMTX_SCALE_S, x["scale"][0]), (ng3d.TT_TEXMTX_SCALE_T, x["scale"][1]),
                                  (ng3d.TT_TEXMTX_ROTATE, x["rot"]), (ng3d.TT_TEXMTX_TRANS_S, x["trans"][0]),
                                  (ng3d.TT_TEXMTX_TRANS_T, x["trans"][1])):
                ab.add_curve(target, ti, curve, reg=j, period=period, time_scale=time_scale)
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


# ------------------------------------------------------------------------------------- posed bounds


def track_values(pkg, tr, frames):
    """A track's values at `frames` (an array), as the engine's curves give them: the first key's
    value up to it, the last key's from it on, step / linear / Hermite in between (two keys at one
    frame: a jump to the second)."""
    k = np.array([(x.frame, x.value, x.slope) for x in pkg.keys[tr.first_key:tr.first_key + tr.n_keys]], np.float64)
    F, V, S = k[:, 0], k[:, 1], k[:, 2]
    f = np.asarray(frames, np.float64)
    if len(F) == 1:
        return np.full(len(f), V[0])
    i = np.clip(np.searchsorted(F, f, side="right") - 1, 0, len(F) - 2)
    f0, f1, v0, v1, s0, s1 = F[i], F[i + 1], V[i], V[i + 1], S[i], S[i + 1]
    d = f1 - f0
    t = np.where(d > 0, (f - f0) / np.where(d > 0, d, 1.0), 1.0)
    if tr.interp == ng3d.IP_STEP:
        out = np.where(t < 1, v0, v1)
    elif tr.interp == ng3d.IP_LINEAR:
        out = v0 + (v1 - v0) * t
    else:
        t2, t3 = t * t, t * t * t
        out = (v0 * (2 * t3 - 3 * t2 + 1) + v1 * (-2 * t3 + 3 * t2) + s0 * d * (t3 - 2 * t2 + t) +
               s1 * d * (t3 - t2))
    return np.where(f <= F[0], V[0], np.where(f >= F[-1], V[-1], out))


def _srt_matrices(s, r, t):
    """nw4r's node matrices T * Rz * Ry * Rx * S for arrays of components: s, r (degrees), t of
    shape (3, n) -> (n, 4, 4)."""
    rx, ry, rz = np.radians(r)
    cx, sx, cy, sy, cz, sz = np.cos(rx), np.sin(rx), np.cos(ry), np.sin(ry), np.cos(rz), np.sin(rz)
    m = np.zeros((len(cx), 4, 4))
    rot = [[cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz],
           [cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz],
           [-sy, sx * cy, cx * cy]]
    for a in range(3):
        for b in range(3):
            m[:, a, b] = rot[a][b] * s[b]
        m[:, a, 3] = t[a]
    m[:, 3, 3] = 1.0
    return m


def model_poses(pkg, mi, ai, frames):
    """Model-space matrices (n_nodes, len(frames), 4, 4) of model mi's nodes posed by animation ai
    (its node tracks for this model; the other components at the node's rest SRT)."""
    m = pkg.models[mi]
    nodes = pkg.nodes[m.first_node:m.first_node + m.n_nodes]
    n = len(frames)
    srt = np.array([[list(nd.scale) + list(nd.rotate) + list(nd.translate)] * n for nd in nodes],
                   np.float64).transpose(0, 2, 1)                                    # (nodes, 9, n)
    a = pkg.anims[ai]
    for tr in pkg.tracks[a.first_track:a.first_track + a.n_tracks]:
        if tr.target <= ng3d.TT_NODE_TRANS_Z and tr.model == mi:
            srt[tr.index, tr.target] = track_values(pkg, tr, frames)
    W = np.empty((len(nodes), n, 4, 4))
    for k, nd in enumerate(nodes):           # (parents come before their children)
        L = _srt_matrices(srt[k, 0:3], srt[k, 3:6], srt[k, 6:9])
        W[k] = L if nd.parent == NONE16 else W[nd.parent] @ L
    return W


def widen_model_bounds(pkg, samples=1024):
    """Model.bbox and radius made to hold every pose the animations of the model's instances give
    it (sampled over each animation's length, its node tracks' keys included), still in model
    space. A model's own box is its rest pose; an instance that moves by node animation (a world
    route on an identity matrix, a flight relative to its placement, turning blades) would leave
    it, and a renderer culls an instance by its matrix and this box. Returns {model: (old radius,
    new radius)} for the models that grew."""
    pairs = sorted({(n.model, n.anim) for n in pkg.instances
                    if n.flags & ng3d.INST_ANIMATED and n.anim != NONE16})
    grown = {}
    for mi, ai in pairs:
        m, a = pkg.models[mi], pkg.anims[ai]
        if not m.n_batches:
            continue
        keys = [k.frame for tr in pkg.tracks[a.first_track:a.first_track + a.n_tracks]
                if tr.target <= ng3d.TT_NODE_TRANS_Z and tr.model == mi
                for k in pkg.keys[tr.first_key:tr.first_key + tr.n_keys]]
        frames = np.unique(np.concatenate([np.linspace(0.0, a.frames, samples + 1),
                                           np.clip(np.array(keys, np.float64), 0.0, a.frames)]))
        W = model_poses(pkg, mi, ai, frames)
        lo, hi = np.array(m.bbox[:3], np.float64), np.array(m.bbox[3:], np.float64)
        for b in range(m.first_batch, m.first_batch + m.n_batches):
            bt = pkg.batches[b]
            corners = np.array([[bt.bbox[3 * (c >> 0 & 1) + 0], bt.bbox[3 * (c >> 1 & 1) + 1],
                                 bt.bbox[3 * (c >> 2 & 1) + 2], 1.0] for c in range(8)])
            P = (W[bt.node] @ corners.T)[:, :3, :]                                  # (frames, 3, 8)
            lo, hi = np.minimum(lo, P.min(axis=(0, 2))), np.maximum(hi, P.max(axis=(0, 2)))
        radius = float(np.linalg.norm(hi - lo) / 2)
        if radius > m.radius * (1 + 1e-6):
            grown[mi] = (grown.get(mi, (m.radius,))[0], radius)
            m.bbox = [float(x) for x in list(lo) + list(hi)]
            m.radius = radius
    return grown
