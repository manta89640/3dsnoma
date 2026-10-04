#!/usr/bin/env python3
"""A reader for NW4R BRRES resource files (the Wii's g3d models, textures and animations).

    import brres
    r = brres.BRRES.load("WS2_common_island.brres")
    m = r.models["WS2_common_island"]
    for d in m.draws(): ...                    # Draw(list, material, shape, node, priority)
    t = m.shape_triangles(m.shapes[0])         # per-corner numpy arrays (pos, nrm, clr0, uv0, ...)
    mat = m.materials[0]; mat.bp[0xC0]         # the material's GX register image (BP/XF)
    rgba = r.texture_rgba("jog_kusa", lod=0)   # (h, w, 4) uint8
    a = r.anims["SRT0"]["WS2_common_sea_P"]    # keyframes: a.materials[name][texmtx].curves

    brres.py ls FILE...                  one line per subfile
    brres.py model FILE [MODEL]          nodes, arrays, materials, shapes, draw lists
    brres.py mat FILE MODEL [MATERIAL]   a material in detail, TEV stages included
    brres.py anim FILE [NAME]            animation tracks and their keys
    brres.py tex FILE NAME OUT.png [LOD] one texture to PNG

Versions: the ones Wii Sports Resort and Wii no Ma use (MDL0 v11, TEX0/PLT0 v3, CHR0/SRT0/SCN0 v5,
CLR0/PAT0/VIS0/SHP0 v4), checked over WSR's 366 Stage/Common BRRES and Wii no Ma's 190: every
display list's triangle count equals its shape's polygon count, every bind matrix recomputed from
the SRT tree equals the stored one, every texture decodes. Layouts follow nw4r::g3d's Res* data
(ResMdlData, ResNodeData, ResMatData, ResTevData, ResShpData, ResTexData, ResAnmChrData, ...); the
offsets are commented where they are read. Big-endian throughout.

Display lists (material, TEV and shape set-up) are run through a small GX register file (GxRegs)
instead of being read write by write: nw4r writes BP registers in masked halves (BP 0xFE, the mask
register, then a partial write), e.g. a TEV KSEL word gets its swap-table bits and its konst
selections in two writes, so the register's value is what a GPU would hold after the list.
"""
import json
import math
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gxtex  # noqa: E402

# ---------------------------------------------------------------- primitives


class Buf:
    """Big-endian accessors over bytes."""

    def __init__(self, data):
        self.d = data

    def u8(self, o):
        return self.d[o]

    def s8(self, o):
        v = self.d[o]
        return v - 256 if v >= 128 else v

    def u16(self, o):
        return struct.unpack_from(">H", self.d, o)[0]

    def s16(self, o):
        return struct.unpack_from(">h", self.d, o)[0]

    def u32(self, o):
        return struct.unpack_from(">I", self.d, o)[0]

    def s32(self, o):
        return struct.unpack_from(">i", self.d, o)[0]

    def f32(self, o):
        return struct.unpack_from(">f", self.d, o)[0]

    def fv(self, o, n):
        return list(struct.unpack_from(">%df" % n, self.d, o))

    def cstr(self, o):
        if o <= 0 or o >= len(self.d):
            return None
        e = self.d.index(b"\0", o)
        return self.d[o:e].decode("ascii", "replace")

    def name(self, base, rel):
        """A name stored as an offset relative to `base` (0 = none)."""
        return self.cstr(base + rel) if rel else None


def read_dict(b, off):
    """nw4r ResDic: u32 size, u32 count, (count + 1) entries of 16 bytes {u16 ref, u16 flag,
    u16 left, u16 right, s32 name, s32 data}, offsets relative to the dictionary; entry 0 is the
    search tree's root. Returns [(name, absolute data offset)] in stored order."""
    n = b.u32(off + 4)
    out = []
    for i in range(1, n + 1):
        e = off + 8 + i * 16
        nm, da = b.s32(e + 8), b.s32(e + 12)
        out.append((b.cstr(off + nm), off + da))
    return out


# the number of section offsets in each subfile kind's header, per version
SECTIONS = {
    (b"MDL0", 11): 14, (b"MDL0", 10): 14, (b"MDL0", 9): 11, (b"MDL0", 8): 11,
    (b"TEX0", 3): 1, (b"TEX0", 1): 1, (b"TEX0", 2): 2,
    (b"PLT0", 3): 1, (b"PLT0", 1): 1,
    (b"CHR0", 5): 2, (b"CHR0", 4): 1,
    (b"CLR0", 4): 2, (b"CLR0", 3): 1,
    (b"SRT0", 5): 2, (b"SRT0", 4): 1,
    (b"PAT0", 4): 6, (b"PAT0", 3): 5,
    (b"VIS0", 4): 2, (b"VIS0", 3): 1,
    (b"SHP0", 4): 3, (b"SHP0", 3): 2,
    (b"SCN0", 5): 7, (b"SCN0", 4): 6,   # data, light sets, ambient, lights, fogs, cameras, (v5) user
}


class SubFile:
    """The common header: magic, u32 size, u32 version, s32 to the BRRES, s32 section offsets[n],
    s32 name; `hdr` is the first byte after the name."""

    def __init__(self, b, off):
        self.b, self.off = b, off
        self.magic = bytes(b.d[off:off + 4])
        self.size = b.u32(off + 4)
        self.version = b.u32(off + 8)
        n = SECTIONS[(self.magic, self.version)]
        self.sections = [b.s32(off + 0x10 + 4 * i) for i in range(n)]
        self.hdr = off + 0x10 + 4 * n + 4
        self.name = b.name(off, b.s32(off + 0x10 + 4 * n))

    def sec(self, i):
        s = self.sections[i] if i < len(self.sections) else 0
        return self.off + s if s else 0


# ---------------------------------------------------------------- GX

GX_TEXFMT = gxtex.FORMATS
PLT_FMT = {0: "IA8", 1: "RGB565", 2: "RGB5A3"}
WRAP = {0: "clamp", 1: "repeat", 2: "mirror"}
FILTER = {0: "near", 1: "linear", 2: "near_mip_near", 3: "lin_mip_near", 4: "near_mip_lin", 5: "lin_mip_lin"}
CULL = {0: "none", 1: "front", 2: "back", 3: "all"}
COMP_TYPE = {0: "u8", 1: "s8", 2: "u16", 3: "s16", 4: "f32"}
COMP_SIZE = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4}
COMP_NP = {0: ">u1", 1: ">i1", 2: ">u2", 3: ">i2", 4: ">f4"}
CLR_FMT = {0: "RGB565", 1: "RGB8", 2: "RGBX8", 3: "RGBA4", 4: "RGBA6", 5: "RGBA8"}
# nw4r texture map modes (ResTexMtxEffect.mapMode)
MAPMODE = {0: "texcoord", 1: "env_camera", 2: "proj_camera", 3: "env_light", 4: "env_spec",
           5: "proj_light", 6: "proj_spec"}
TEV_CIN = ["cprev", "aprev", "c0", "a0", "c1", "a1", "c2", "a2", "texc", "texa",
           "rasc", "rasa", "one", "half", "konst", "zero"]
TEV_AIN = ["aprev", "a0", "a1", "a2", "texa", "rasa", "konst", "zero"]
TEV_DEST = ["prev", "r0", "r1", "r2"]
TEV_BIAS = ["", "+0.5", "-0.5", "cmp"]
TEV_SCALE = ["", "*2", "*4", "/2"]
CHAN_ID = {0: "c0a0", 1: "c1a1", 5: "alpha_bump", 6: "alpha_bumpN", 7: "zero"}
COMPARE = ["never", "less", "equal", "lequal", "greater", "nequal", "gequal", "always"]
ALPHA_OP = ["and", "or", "xor", "xnor"]
BLEND_FAC = ["zero", "one", "srcclr", "invsrcclr", "srca", "invsrca", "dsta", "invdsta"]
LOGIC_OP = ["clear", "and", "revand", "copy", "invand", "noop", "xor", "or", "nor",
            "equiv", "inv", "revor", "invcopy", "invor", "nand", "set"]
# XF texgen source rows (XF 0x1040 bits 7-11) -> GXTexGenSrc
TG_ROW_TO_SRC = {0: 0, 1: 1, 3: 2, 4: 3, 5: 4, 6: 5, 7: 6, 8: 7, 9: 8, 10: 9, 11: 10, 12: 11}
TG_SRC_NAME = {0: "pos", 1: "nrm", 2: "binrm", 3: "tangent", 4: "tex0", 5: "tex1", 6: "tex2",
               7: "tex3", 8: "tex4", 9: "tex5", 10: "tex6", 11: "tex7", 12: "texcoord0",
               13: "texcoord1", 14: "texcoord2", 15: "texcoord3", 16: "texcoord4", 17: "texcoord5",
               18: "texcoord6", 19: "color0", 20: "color1"}


def scan_gx(d, start, end):
    """Walk a GX command stream: ('bp', reg, val) / ('xf', addr, [vals]) / ('cp', reg, val) /
    ('indx', 'A'..'D', index, addr, n) / ('prim', op, vat, count, offset). Stops at a primitive
    (its vertex data's size depends on the vertex descriptor)."""
    out = []
    o = start
    while o < end:
        c = d[o]
        if c == 0x00:                                       # NOP
            o += 1
        elif c == 0x61:                                     # BP: 8-bit register, 24-bit value
            v = struct.unpack_from(">I", d, o + 1)[0]
            out.append(("bp", v >> 24, v & 0xFFFFFF))
            o += 5
        elif c == 0x10:                                     # XF: (n - 1) << 16 | address, n words
            hdr = struct.unpack_from(">I", d, o + 1)[0]
            n = (hdr >> 16) + 1
            out.append(("xf", hdr & 0xFFFF, list(struct.unpack_from(">%dI" % n, d, o + 5))))
            o += 5 + 4 * n
        elif c == 0x08:                                     # CP
            out.append(("cp", d[o + 1], struct.unpack_from(">I", d, o + 2)[0]))
            o += 6
        elif c in (0x20, 0x28, 0x30, 0x38):                 # indexed XF loads (matrices, lights)
            idx, a = struct.unpack_from(">HH", d, o + 1)
            out.append(("indx", "ABCD"[(c - 0x20) >> 3], idx, a & 0xFFF, (a >> 12) + 1))
            o += 5
        elif c == 0x48:                                     # invalidate vertex cache
            o += 1
        elif c == 0x40:                                     # call display list
            o += 9
        elif c & 0x80:
            out.append(("prim", c & 0xF8, c & 7, struct.unpack_from(">H", d, o + 1)[0], o))
            break
        else:
            raise ValueError("bad GX opcode %02x at %x" % (c, o))
    return out


class GxRegs:
    """The BP/XF/CP registers a display list leaves behind. BP 0xFE sets a mask for the next BP write
    only (bits outside it keep their value); the TEV colour registers BP 0xE0..0xE7 are two register
    files in one address range (bit 23 set: the konst colours), kept apart in `tev`."""

    def __init__(self, base=None):
        self.bp = dict(base.bp) if base else {}
        self.xf = dict(base.xf) if base else {}
        self.cp = dict(base.cp) if base else {}
        self.tev = dict(base.tev) if base else {}       # (konst, register 0-3, "ra"/"bg") -> value
        self.writes = set(base.writes) if base else set()
        self._mask = 0xFFFFFF

    def run(self, cmds):
        for c in cmds:
            if c[0] == "bp":
                reg, val = c[1], c[2]
                if reg == 0xFE:
                    self._mask = val
                    continue
                mask, self._mask = self._mask, 0xFFFFFF
                if 0xE0 <= reg <= 0xE7:
                    key = (bool(val & 0x800000), (reg - 0xE0) // 2, "bg" if reg & 1 else "ra")
                    self.tev[key] = (self.tev.get(key, 0) & ~mask) | (val & mask)
                    continue
                self.bp[reg] = (self.bp.get(reg, 0) & ~mask) | (val & mask)
                self.writes.add(reg)
            elif c[0] == "xf":
                for i, v in enumerate(c[2]):
                    self.xf[c[1] + i] = v
            elif c[0] == "cp":
                self.cp[c[1]] = c[2]
        return self


def sx11(v):
    """An 11-bit signed TEV colour register component."""
    return v - 2048 if v & 0x400 else v


def chan_ctrl(v):
    """GX channel control (XF 0x100E..0x1011): bit 0 material from vertex, bit 1 lighting, bits 2-5
    lights 0-3, bit 6 ambient from vertex, bits 7-8 diffuse fn, bit 9 attenuation on, bit 10 spot
    (else specular), bits 11-14 lights 4-7."""
    lights = [(v >> (2 + i)) & 1 for i in range(4)] + [(v >> (11 + i)) & 1 for i in range(4)]
    return dict(raw=v, mat_src="vtx" if v & 1 else "reg", lighting=bool(v & 2),
                amb_src="vtx" if v & 0x40 else "reg", lights=[i for i, x in enumerate(lights) if x],
                diffuse=["none", "sign", "clamp", "clamp"][(v >> 7) & 3],
                attn=("spot" if v & 0x400 else "spec") if v & 0x200 else "none")


def texgen_from_xf(info, post):
    """XF 0x1040+i (texgen) and 0x1050+i (dual/post) words -> GX texgen parameters:
    type (GXTexGenType: 0 MTX3x4, 1 MTX2x4, 2-9 BUMP0-7, 10 SRTG), src (GXTexGenSrc), the post
    matrix (GX_PTTEXMTX index 0-19, or None for GX_PTIDENTITY) and normalize."""
    proj = (info >> 1) & 1                      # 1: STQ (3x4)
    kind = (info >> 4) & 7                      # 0 regular, 1 emboss (bump), 2/3 colour strgb 0/1
    row = (info >> 7) & 31
    if kind == 0:
        typ, src = (0 if proj else 1), TG_ROW_TO_SRC.get(row, 0)
    elif kind == 1:
        typ, src = 2 + ((info >> 15) & 7), 12 + ((info >> 12) & 7)
    else:
        typ, src = 10, 19 if kind == 2 else 20
    ptm = post & 0x3F                           # GX_PTTEXMTXn - 64 = 3n; 61 = GX_PTIDENTITY
    return dict(type=typ, src=src, src_name=TG_SRC_NAME.get(src, str(src)), proj=bool(proj),
                post=None if ptm >= 60 else ptm // 3, normalize=bool(post & 0x100))


def fmt_env(inp, e):
    """A TEV combiner word as text, e.g. 'prev=(texc*rasc)'."""
    a, b_, c, d = inp
    sub = (e >> 18) & 1
    bias = (e >> 16) & 3
    scale = (e >> 20) & 3
    dest = TEV_DEST[(e >> 22) & 3]
    if bias == 3:
        expr = "cmp(%s,%s,%s,%s)" % (a, b_, c, d)
    else:
        lerp = "lerp(%s,%s,%s)" % (a, b_, c)
        if b_ == "zero" and c == "zero":
            lerp = a
        elif a == "zero" and c == "one":
            lerp = b_
        elif a == "zero" and b_ == "zero":
            lerp = ""
        elif a == "zero":
            lerp = "%s*%s" % (b_, c)
        expr = d if not lerp else ("%s%s%s" % (d, "-" if sub else "+", lerp) if d != "zero" else
                                    ("-" if sub else "") + lerp)
        expr = "(" + expr + ")" + TEV_BIAS[bias] + TEV_SCALE[scale]
    return "%s=%s%s" % (dest, expr, "" if (e >> 19) & 1 else " noclamp")


# ---------------------------------------------------------------- MDL0


class Node:
    pass


class VtxArray:
    pass


class Material:
    pass


class Shader:
    pass


class Shape:
    pass


class Draw:
    """One draw of a model's draw list (DrawOpa / DrawXlu): material, shape, the node whose
    visibility it follows, and its priority."""
    __slots__ = ("list", "mat", "shape", "node", "prio")

    def __init__(self, lst, mat, shape, node, prio):
        self.list, self.mat, self.shape, self.node, self.prio = lst, mat, shape, node, prio

    def __iter__(self):
        return iter((self.list, self.mat, self.shape, self.node, self.prio))


class MDL0(SubFile):
    SEC_NAMES = ["bytecode", "nodes", "pos", "nrm", "clr", "uv", "furvec", "furpos",
                 "mats", "tevs", "shapes", "texlinks", "pltlinks", "user"]

    def __init__(self, b, off):
        super().__init__(b, off)
        h = self.hdr                                   # ResMdlInfoData (0x40)
        assert b.u32(h) == 0x40, "MDL0 info size"
        self.info = dict(
            scaling_rule=b.u32(h + 0x08),              # 0 standard, 1 softimage, 2 maya
            texmtx_mode=b.u32(h + 0x0C),               # 0 maya, 1 xsi, 2 3dsmax
            num_vertices=b.u32(h + 0x10),
            num_triangles=b.u32(h + 0x14),
            num_view_mtx=b.u32(h + 0x1C),
            need_nrm_mtx=b.u8(h + 0x20),
            need_tex_mtx=b.u8(h + 0x21),
            bbox_enabled=b.u8(h + 0x22),
            envelope_mode=b.u8(h + 0x23),
            bbox=(b.fv(h + 0x28, 3), b.fv(h + 0x34, 3)),
        )
        t = h + b.s32(h + 0x24)                        # matrix id -> node id (-1: an envelope)
        self.mtx_to_node = [b.s32(t + 4 + 4 * i) for i in range(b.u32(t))]
        self._read_bytecode()
        self._read_nodes()
        self.pos = self._read_arrays(2, "pos")
        self.nrm = self._read_arrays(3, "nrm")
        self.clr = self._read_arrays(4, "clr")
        self.uv = self._read_arrays(5, "uv")
        self.shaders = self._read_shaders()
        self.materials = self._read_materials()
        self.shapes = self._read_shapes()
        self.tex_links = self._read_links(11)
        self.plt_links = self._read_links(12)
        self._world = None

    # -- bytecode (NodeTree / NodeMix / DrawOpa / DrawXlu)
    def _read_bytecode(self):
        b = self.b
        self.bytecode = {}
        self.envelopes = {}       # matrix id -> [(node or matrix id, weight)]
        s = self.sec(0)
        if not s:
            return
        for name, o in read_dict(b, s):
            ops = []
            while True:
                c = b.u8(o)
                if c == 0x01:       # end
                    break
                if c == 0x00:
                    o += 1
                elif c == 0x02:     # node: u16 node, u16 parent matrix
                    ops.append(("node", b.u16(o + 1), b.u16(o + 3)))
                    o += 5
                elif c == 0x03:     # weight mix: u16 matrix, u8 n, n x (u16 id, f32 weight)
                    m, n = b.u16(o + 1), b.u8(o + 3)
                    ws = [(b.u16(o + 4 + 6 * i), b.f32(o + 6 + 6 * i)) for i in range(n)]
                    ops.append(("mix", m, ws))
                    self.envelopes[m] = ws
                    o += 4 + 6 * n
                elif c == 0x04:     # draw: u16 material, u16 shape, u16 node, u8 priority
                    ops.append(("draw", b.u16(o + 1), b.u16(o + 3), b.u16(o + 5), b.u8(o + 7)))
                    o += 8
                elif c == 0x05:     # envelope matrix: u16 matrix, u16 node
                    ops.append(("evp", b.u16(o + 1), b.u16(o + 3)))
                    o += 5
                elif c == 0x06:     # matrix duplicate
                    ops.append(("dup", b.u16(o + 1), b.u16(o + 3)))
                    o += 5
                else:
                    raise ValueError("bytecode op %02x in %s" % (c, name))
            self.bytecode[name] = ops

    def draws(self, which=None):
        """[Draw] in draw-list order (DrawOpa first if both exist, as nw4r stores them)."""
        out = []
        for name, ops in self.bytecode.items():
            if (which and name != which) or not name.startswith("Draw"):
                continue
            out += [Draw(name, op[1], op[2], op[3], op[4]) for op in ops if op[0] == "draw"]
        return out

    # -- nodes
    def _read_nodes(self):
        b = self.b
        self.nodes = []
        s = self.sec(1)
        if not s:
            return
        offs = {}
        for name, o in read_dict(b, s):        # ResNodeData (0xD0)
            n = Node()
            n.off = o
            n.name = b.name(o, b.s32(o + 0x08))
            n.index = b.u32(o + 0x0C)
            n.mtx_id = b.u32(o + 0x10)
            # flags: 1 identity, 2 trans zero, 4 rot zero, 8 scale one, 0x10 scale uniform,
            # 0x20 SSC apply, 0x40 SSC parent, 0x80 XSI scaling, 0x100 visible, 0x200 has
            # geometry, 0x400 billboard parent
            n.flags = b.u32(o + 0x14)
            n.billboard = b.u32(o + 0x18)
            n.scale = b.fv(o + 0x20, 3)
            n.rot = b.fv(o + 0x2C, 3)              # degrees, XYZ
            n.trans = b.fv(o + 0x38, 3)
            n.bbox = (b.fv(o + 0x44, 3), b.fv(o + 0x50, 3))
            rel = [b.s32(o + 0x5C + 4 * i) for i in range(4)]   # parent, child, next, prev
            n._links = [o + r if r else None for r in rel]
            n.mtx = np.array(b.fv(o + 0x70, 12)).reshape(3, 4)       # world (bind) matrix
            n.inv = np.array(b.fv(o + 0xA0, 12)).reshape(3, 4)
            offs[o] = n
            self.nodes.append(n)
        self.nodes.sort(key=lambda n: n.index)
        for n in self.nodes:
            p = n._links[0]
            n.parent = offs[p].index if p in offs else -1

    @staticmethod
    def srt_matrix(s, r, t):
        """nw4r's node matrix: T * Rz * Ry * Rx * S (rotations in degrees)."""
        rx, ry, rz = [math.radians(a) for a in r]
        cx, sx, cy, sy, cz, sz = (math.cos(rx), math.sin(rx), math.cos(ry), math.sin(ry),
                                  math.cos(rz), math.sin(rz))
        rot = np.array([[cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz],
                        [cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz],
                        [-sy, sx * cy, cx * cy]])
        m = np.zeros((3, 4))
        m[:, :3] = rot * np.array(s)[None, :]
        m[:, 3] = t
        return m

    def world_matrices(self, from_srt=False):
        """{node index: 4x4 world matrix}: the stored bind matrices (ResNodeData +0x70), or with
        from_srt the SRT tree multiplied out (they agree on every WSR model)."""
        if self._world is not None and not from_srt:
            return self._world
        W = {}
        for n in self.nodes:          # parents precede children in index order
            m = np.eye(4)
            if from_srt:
                m[:3] = self.srt_matrix(n.scale, n.rot, n.trans)
                W[n.index] = (W[n.parent] @ m) if n.parent >= 0 else m
            else:
                m[:3] = n.mtx
                W[n.index] = m
        if not from_srt:
            self._world = W
        return W

    def matrix_node(self, mtx_id):
        """The node a draw matrix id belongs to, or -1 for an envelope (a weighted mix)."""
        return self.mtx_to_node[mtx_id] if 0 <= mtx_id < len(self.mtx_to_node) else -1

    def matrix_world(self, mtx_id):
        node = self.matrix_node(mtx_id)
        return self.world_matrices()[node] if node >= 0 else np.eye(4)

    # -- vertex arrays (ResVtxPosData, ResVtxNrmData, ResVtxClrData, ResVtxTexCoordData)
    def _read_arrays(self, sec, kind):
        b = self.b
        out = []
        s = self.sec(sec)
        if not s:
            return out
        for name, o in read_dict(b, s):
            a = VtxArray()
            a.kind, a.off = kind, o
            a.size = b.u32(o)
            a.data_off = o + b.s32(o + 0x08)
            a.name = b.name(o, b.s32(o + 0x0C))
            a.index = b.u32(o + 0x10)
            a.comp_cnt = b.u32(o + 0x14)
            a.fmt = b.u32(o + 0x18)
            if kind == "clr":           # stride u8 at 0x1C, count u16 at 0x1E
                a.frac = 0
                a.stride = b.u8(o + 0x1C)
                a.count = b.u16(o + 0x1E)
                a.ncomp = 4 if a.comp_cnt else 3
            else:
                a.frac = b.u8(o + 0x1C)
                a.stride = b.u8(o + 0x1D)
                a.count = b.u16(o + 0x1E)
                if kind == "pos":
                    a.ncomp = 3 if a.comp_cnt else 2
                    a.bbox = (b.fv(o + 0x20, 3), b.fv(o + 0x2C, 3))
                elif kind == "nrm":
                    a.ncomp = {0: 3, 1: 9, 2: 9}[a.comp_cnt]
                elif kind == "uv":
                    a.ncomp = 2 if a.comp_cnt else 1
                    a.bbox = (b.fv(o + 0x20, 2), b.fv(o + 0x28, 2))
            a.bytes = a.stride * a.count
            out.append(a)
        out.sort(key=lambda a: a.index)
        return out

    def array_values(self, a):
        """(count, ncomp) float32, colours (count, 4) uint8 RGBA."""
        d = self.b.d
        if a.kind == "clr":
            raw = np.frombuffer(d, np.uint8, a.count * a.stride, a.data_off).reshape(a.count, a.stride)
            f = a.fmt
            out = np.full((a.count, 4), 255, np.uint8)
            if f == 0:
                v = (raw[:, 0].astype(np.uint16) << 8) | raw[:, 1]
                out[:, 0] = ((v >> 11) & 31) * 255 // 31
                out[:, 1] = ((v >> 5) & 63) * 255 // 63
                out[:, 2] = (v & 31) * 255 // 31
            elif f in (1, 2):
                out[:, :3] = raw[:, :3]
            elif f == 3:
                v = (raw[:, 0].astype(np.uint16) << 8) | raw[:, 1]
                for i, sh in enumerate((12, 8, 4, 0)):
                    out[:, i] = ((v >> sh) & 15) * 17
            elif f == 4:
                v = (raw[:, 0].astype(np.uint32) << 16) | (raw[:, 1].astype(np.uint32) << 8) | raw[:, 2]
                for i, sh in enumerate((18, 12, 6, 0)):
                    out[:, i] = ((v >> sh) & 63) * 255 // 63
            elif f == 5:
                out[:] = raw[:, :4]
            return out
        cs = COMP_SIZE[a.fmt]
        n = a.ncomp
        assert a.stride >= cs * n, (a.name, a.stride, cs, n)
        raw = np.frombuffer(d, np.uint8, a.count * a.stride, a.data_off).reshape(a.count, a.stride)
        vals = raw[:, :cs * n].copy().view(COMP_NP[a.fmt]).reshape(a.count, n).astype(np.float32)
        if a.fmt != 4:
            frac = a.frac
            if a.kind == "nrm":      # normals have fixed fractions: s8 6 bits, s16 14 bits
                frac = {1: 6, 3: 14}.get(a.fmt, a.frac)
            vals /= float(1 << frac)
        return vals

    def array(self, kind, index):
        for a in getattr(self, kind):
            if a.index == index:
                return a
        return None

    # -- TEV configurations (ResTevData)
    def _read_shaders(self):
        b = self.b
        out = {}
        s = self.sec(9)
        if not s:
            return out
        for name, o in read_dict(b, s):
            if o in out:
                continue
            t = Shader()
            t.off = o
            t.size = b.u32(o)
            t.index = b.u32(o + 0x08)
            t.num_stages = b.u8(o + 0x0C)
            t.cmds = scan_gx(b.d, o + 0x20, o + t.size)
            out[o] = t
        return out

    # -- materials (ResMatData, v11)
    def _read_materials(self):
        b = self.b
        out = []
        s = self.sec(8)
        if not s:
            return out
        for name, o in read_dict(b, s):
            m = Material()
            m.off = o
            m.size = b.u32(o)
            m.name = b.name(o, b.s32(o + 0x08))
            m.index = b.u32(o + 0x0C)
            m.flags = b.u32(o + 0x10)
            m.xlu = bool(m.flags & 0x80000000)
            m.n_texgens, m.n_chans, m.n_tev, m.n_ind = b.d[o + 0x14:o + 0x18]
            m.cull = b.u32(o + 0x18)
            m.early_z = b.u8(o + 0x1C)
            m.light_set = b.s8(o + 0x1D)            # -1: none
            m.fog = b.s8(o + 0x1E)                  # -1: none
            m.ind_method = list(b.d[o + 0x20:o + 0x24])
            m.nrm_ref_light = [b.s8(o + 0x24 + i) for i in range(4)]
            m.shader_off = o + b.s32(o + 0x28)
            n_tex = b.u32(o + 0x2C)
            to = o + b.s32(o + 0x30)
            dl = o + b.s32(o + 0x3C)
            # texture SRTs (0x1A8): u32 flags (4 bits a matrix: 1 used, 2 scale one, 4 rotation
            # zero, 8 translation zero), u32 matrix mode, ResTexSrt[8] {scale s, t, rotation,
            # translation s, t}, ResTexMtxEffect[8] {s8 camera, s8 light, u8 map mode, u8
            # identity effect, Mtx34 effect}
            srt_flag = b.u32(o + 0x1A8)
            m.tex_mtx_mode = b.u32(o + 0x1AC)
            m.srt = []
            for i in range(8):
                so = o + 0x1B0 + 0x14 * i
                eo = o + 0x250 + 0x34 * i
                m.srt.append(dict(flag=(srt_flag >> (4 * i)) & 15, scale=b.fv(so, 2),
                                  rot=b.f32(so + 8), trans=b.fv(so + 12, 2),
                                  cam=b.s8(eo), light=b.s8(eo + 1), map_mode_id=b.u8(eo + 2),
                                  map_mode=MAPMODE.get(b.u8(eo + 2), b.u8(eo + 2)),
                                  ident=b.u8(eo + 3),
                                  effect=np.array(b.fv(eo + 4, 12), np.float64).reshape(3, 4)))
            # colour channels (0x3F0): 2 x {u32 flag, RGBA material, RGBA ambient, u32 colour
            # control, u32 alpha control}
            m.chans = []
            for i in range(2):
                co = o + 0x3F0 + 0x14 * i
                m.chans.append(dict(flag=b.u32(co), mat=list(b.d[co + 4:co + 8]),
                                    amb=list(b.d[co + 8:co + 12]),
                                    color=chan_ctrl(b.u32(co + 12)), alpha=chan_ctrl(b.u32(co + 16))))
            # texture layers: ResTexPlttInfo (0x34 each)
            m.layers = []
            for i in range(n_tex):
                lo = to + 0x34 * i
                m.layers.append(dict(
                    tex=b.name(lo, b.s32(lo)), plt=b.name(lo, b.s32(lo + 4)),
                    map_id=b.u32(lo + 0x10), tlut_id=b.u32(lo + 0x14),
                    wrap_id=(b.u32(lo + 0x18), b.u32(lo + 0x1C)),
                    wrap=(WRAP.get(b.u32(lo + 0x18)), WRAP.get(b.u32(lo + 0x1C))),
                    min_id=b.u32(lo + 0x20), mag_id=b.u32(lo + 0x24),
                    min=FILTER.get(b.u32(lo + 0x20)), mag=FILTER.get(b.u32(lo + 0x24)),
                    lod_bias=b.f32(lo + 0x28), aniso=b.u32(lo + 0x2C),
                    bias_clamp=b.u8(lo + 0x30), edge_lod=b.u8(lo + 0x31)))
            # the material display list (pixel state, TEV colours, indirect matrices, texgens),
            # then the TEV configuration's list: the GX registers the material sets
            m.cmds = scan_gx(b.d, dl, dl + 0x180)
            sh = self.shaders.get(m.shader_off)
            m.regs = GxRegs().run(m.cmds)
            if sh:
                m.regs.run(sh.cmds)
            m.n_stages = sh.num_stages if sh else 0
            _describe_material(m)
            out.append(m)
        out.sort(key=lambda m: m.index)
        return out

    def shader_of(self, m):
        return self.shaders.get(m.shader_off)

    # -- shapes (ResShpData)
    def _read_shapes(self):
        b = self.b
        out = []
        s = self.sec(10)
        if not s:
            return out
        for name, o in read_dict(b, s):
            p = Shape()
            p.off = o
            p.size = b.u32(o)
            p.mtx_id = b.s32(o + 0x08)                # -1: per-vertex matrices (PNMTXIDX)
            p.setup_dl = (o + 0x18 + b.s32(o + 0x20), b.u32(o + 0x1C))
            p.prim_dl = (o + 0x24 + b.s32(o + 0x2C), b.u32(o + 0x28))
            p.vtx_attr = b.u32(o + 0x30)
            p.flags = b.u32(o + 0x34)
            p.name = b.name(o, b.s32(o + 0x38))
            p.index = b.u32(o + 0x3C)
            p.num_vertices = b.u32(o + 0x40)
            p.num_faces = b.u32(o + 0x44)
            p.pos_id = b.s16(o + 0x48)
            p.nrm_id = b.s16(o + 0x4A)
            p.clr_id = [b.s16(o + 0x4C), b.s16(o + 0x4E)]
            p.uv_id = [b.s16(o + 0x50 + 2 * i) for i in range(8)]
            ml = o + b.s32(o + 0x64)                  # the matrices it uses
            p.mtx_list = [b.u16(ml + 4 + 2 * i) for i in range(b.u32(ml))]
            sd, sn = p.setup_dl
            p.setup = scan_gx(b.d, sd, sd + sn)
            p.vcd = decode_vcd(p.setup)
            p.dl_bytes = p.prim_dl[1]
            out.append(p)
        out.sort(key=lambda p: p.index)
        return out

    def decode_shape(self, p):
        """The primitive display list as [(op, {attr: index array}, {PNMTXIDX slot: matrix id})]."""
        d = self.b.d
        start, length = p.prim_dl
        end = start + length
        order = p.vcd["order"]
        stride = sum(sz for _, sz in order)
        o = start
        slots = {}
        prims = []
        while o < end:
            c = d[o]
            if c == 0x00:
                o += 1
            elif c in (0x20, 0x28, 0x30, 0x38):
                idx, a = struct.unpack_from(">HH", d, o + 1)
                if c == 0x20:                         # a position matrix into an XF slot
                    slots[(a & 0xFFF) // 12] = idx
                o += 5
            elif c == 0x08:
                o += 6
            elif c == 0x10:
                o += 5 + 4 * (struct.unpack_from(">H", d, o + 1)[0] + 1)
            elif c == 0x61:
                o += 5
            elif c == 0x48:
                o += 1
            elif c & 0x80:
                op = c & 0xF8
                cnt = struct.unpack_from(">H", d, o + 1)[0]
                o += 3
                raw = np.frombuffer(d, np.uint8, cnt * stride, o).reshape(cnt, stride)
                attrs = {}
                col = 0
                for attr, sz in order:
                    if sz == 1:
                        attrs[attr] = raw[:, col].astype(np.int32)
                    else:
                        attrs[attr] = (raw[:, col].astype(np.int32) << 8) | raw[:, col + 1]
                    col += sz
                prims.append((op, attrs, dict(slots)))
                o += cnt * stride
            else:
                raise ValueError("shape %s: GX opcode %02x at %x" % (p.name, c, o))
        return prims

    def shape_triangles(self, p, world=True, want=None):
        """Triangulate a shape (GX front faces wind clockwise; kept as stored). Per-corner arrays
        (3 x triangles rows): 'pos' (float32, model space if world else as stored, i.e. in its
        matrix's node space), 'nrm', 'clr0'/'clr1' (uint8 RGBA), 'uv0'..'uv7', and 'mtx' (the
        draw matrix id of each corner)."""
        want = want or ("pos", "nrm", "clr0", "clr1") + tuple("uv%d" % i for i in range(8))
        idx_lists = {}
        for op, attrs, slots in self.decode_shape(p):
            n = len(next(iter(attrs.values())))
            if op == 0x90:                                  # triangles
                tri = np.arange(n)
            elif op == 0x98:                                # strip: every other one flipped
                k = np.arange(n - 2)
                odd = (k & 1) == 1
                tri = np.stack([np.where(odd, k + 1, k), np.where(odd, k, k + 1), k + 2], 1).ravel()
            elif op == 0xA0:                                # fan
                k = np.arange(1, n - 1)
                tri = np.stack([np.zeros_like(k), k, k + 1], 1).ravel()
            elif op == 0x80:                                # quads
                q = np.arange(0, n - 3, 4)
                tri = np.stack([q, q + 1, q + 2, q, q + 2, q + 3], 1).ravel()
            else:                                           # lines, points: not drawn here
                continue
            for attr, v in attrs.items():
                idx_lists.setdefault(attr, []).append(v[tri])
            if "pnmtx" in attrs:
                m = np.array([slots.get(int(s) // 3, p.mtx_id) for s in attrs["pnmtx"][tri]])
            else:
                m = np.full(len(tri), p.mtx_id)
            idx_lists.setdefault("mtx", []).append(m)
        out = {}
        if not idx_lists:
            return out
        cat = {k: np.concatenate(v) for k, v in idx_lists.items()}
        out["mtx"] = cat["mtx"]
        if "pos" in want and "pos" in cat:
            P = self.array_values(self.array("pos", p.pos_id))[cat["pos"]]
            if P.shape[1] == 2:
                P = np.concatenate([P, np.zeros((len(P), 1), np.float32)], 1)
            out["pos"] = self._to_world(P, cat["mtx"]) if world else P
        if "nrm" in want and "nrm" in cat and p.nrm_id >= 0:
            out["nrm"] = self.array_values(self.array("nrm", p.nrm_id))[cat["nrm"]][:, :3]
        for i in range(2):
            key = "clr%d" % i
            if key in want and key in cat and p.clr_id[i] >= 0:
                out[key] = self.array_values(self.array("clr", p.clr_id[i]))[cat[key]]
        for i in range(8):
            key = "uv%d" % i
            if key in want and key in cat and p.uv_id[i] >= 0:
                out[key] = self.array_values(self.array("uv", p.uv_id[i]))[cat[key]]
        return out

    def _to_world(self, P, mtx):
        out = np.empty_like(P)
        for m in np.unique(mtx):
            sel = mtx == m
            W = self.matrix_world(int(m)) if m >= 0 else np.eye(4)
            out[sel] = P[sel] @ W[:3, :3].T.astype(np.float32) + W[:3, 3].astype(np.float32)
        return out

    def _read_links(self, sec):
        b = self.b
        out = {}
        s = self.sec(sec)
        if s:
            for name, o in read_dict(b, s):
                out[name] = b.u32(o)
        return out

    def summary(self):
        return dict(
            name=self.name, size=self.size, vertices=self.info["num_vertices"],
            triangles=self.info["num_triangles"], shape_faces=sum(p.num_faces for p in self.shapes),
            nodes=len(self.nodes), materials=len(self.materials), shapes=len(self.shapes),
            textures=sorted(self.tex_links), bbox=self.info["bbox"])

    def geometry_bytes(self):
        """(bytes of vertex arrays, bytes of shape display lists)."""
        return (sum(a.bytes for a in self.pos + self.nrm + self.clr + self.uv),
                sum(p.dl_bytes for p in self.shapes))


def decode_vcd(cmds):
    """The vertex descriptor from a shape's set-up list (CP 0x50 / 0x60): [(attribute, index bytes)]
    in the order GX sends them."""
    lo = hi = 0
    for c in cmds:
        if c[0] == "cp" and c[1] == 0x50:
            lo = c[2]
        elif c[0] == "cp" and c[1] == 0x60:
            hi = c[2]
    order = []
    if lo & 1:
        order.append(("pnmtx", 1))
    for i in range(8):
        if lo & (2 << i):
            order.append(("tmtx%d" % i, 1))

    def add(name, t):
        if t == 0:
            return
        if t == 1:
            raise NotImplementedError("direct %s attribute" % name)
        order.append((name, 1 if t == 2 else 2))
    add("pos", (lo >> 9) & 3)
    add("nrm", (lo >> 11) & 3)
    add("clr0", (lo >> 13) & 3)
    add("clr1", (lo >> 15) & 3)
    for i in range(8):
        add("uv%d" % i, (hi >> (2 * i)) & 3)
    return dict(lo=lo, hi=hi, order=order)


def _describe_material(m):
    """Readable views of the register image: TEV stages, pixel state, colour registers, texgens."""
    bp, xf = m.regs.bp, m.regs.xf
    m.stages = []
    for i in range(m.n_stages):
        t = (bp.get(0x28 + i // 2, 0) >> (12 * (i & 1))) & 0xFFF
        ksel = bp.get(0xF6 + i // 2, 0)
        ce, ae = bp.get(0xC0 + 2 * i, 0), bp.get(0xC1 + 2 * i, 0)
        ind = bp.get(0x10 + i, 0)
        cin = [TEV_CIN[(ce >> s) & 15] for s in (12, 8, 4, 0)]
        ain = [TEV_AIN[(ae >> s) & 7] for s in (13, 10, 7, 4)]
        m.stages.append(dict(
            texmap=(t & 7) if t & 0x40 else None, texcoord=(t >> 3) & 7,
            chan=CHAN_ID.get((t >> 7) & 7, (t >> 7) & 7),
            kc=(ksel >> (4 + 10 * (i & 1))) & 31, ka=(ksel >> (9 + 10 * (i & 1))) & 31,
            color=fmt_env(cin, ce), alpha=fmt_env(ain, ae), ce=ce, ae=ae,
            rswap=ae & 3, tswap=(ae >> 2) & 3, ind=ind))
    # swap tables: KSEL 2j (bits 0-1 red, 2-3 green) and 2j+1 (blue, alpha)
    m.swap_tables = [(bp.get(0xF6 + 2 * j, 0) & 3, (bp.get(0xF6 + 2 * j, 0) >> 2) & 3,
                      bp.get(0xF7 + 2 * j, 0) & 3, (bp.get(0xF7 + 2 * j, 0) >> 2) & 3) for j in range(4)]
    ac = bp.get(0xF3, 0)
    m.alpha_test = dict(comp0=COMPARE[(ac >> 16) & 7], ref0=ac & 0xFF,
                        comp1=COMPARE[(ac >> 19) & 7], ref1=(ac >> 8) & 0xFF, op=ALPHA_OP[(ac >> 22) & 3])
    z = bp.get(0x40, 0)
    m.zmode = dict(test=bool(z & 1), func=COMPARE[(z >> 1) & 7], write=bool(z & 0x10))
    bm = bp.get(0x41, 0)
    m.blend = dict(enable=bool(bm & 1), logic=bool(bm & 2), dither=bool(bm & 4),
                   color_update=bool(bm & 8), alpha_update=bool(bm & 16),
                   dst=BLEND_FAC[(bm >> 5) & 7], src=BLEND_FAC[(bm >> 8) & 7],
                   subtract=bool(bm & 0x800), logic_op=LOGIC_OP[(bm >> 12) & 15])
    ca = bp.get(0x42)
    m.const_alpha = (ca & 0xFF) if ca is not None and ca & 0x100 else None
    # TEV registers: index 0 = GX_TEVPREV, 1..3 = GX_TEVREG0..2 (the TEV inputs c0..c2); konst i
    regs = {}
    for k in (False, True):
        for i in range(4):
            ra, bg = m.regs.tev.get((k, i, "ra")), m.regs.tev.get((k, i, "bg"))
            if ra is None and bg is None:
                continue
            ra, bg = ra or 0, bg or 0
            r, a = ra & 0x7FF, (ra >> 12) & 0x7FF
            b_, g = bg & 0x7FF, (bg >> 12) & 0x7FF
            if k:
                regs["k%d" % i] = [r & 0xFF, g & 0xFF, b_ & 0xFF, a & 0xFF]
            else:
                regs[("prev", "c0", "c1", "c2")[i]] = [sx11(r), sx11(g), sx11(b_), sx11(a)]
    m.tev_regs = regs
    m.texgens = []
    for i in range(m.n_texgens):
        g = texgen_from_xf(xf.get(0x1040 + i, 0), xf.get(0x1050 + i, 0x3D))
        g["post_raw"] = xf.get(0x1050 + i)
        m.texgens.append(g)


# ---------------------------------------------------------------- TEX0 / PLT0


class TEX0(SubFile):
    def __init__(self, b, off):
        super().__init__(b, off)
        o = off
        self.data_off = self.sec(0)
        self.flags = b.u32(o + 0x18)          # 1: paletted
        self.width = b.u16(o + 0x1C)
        self.height = b.u16(o + 0x1E)
        self.fmt = b.u32(o + 0x20)
        self.fmt_name = GX_TEXFMT.get(self.fmt, str(self.fmt))
        self.levels = b.u32(o + 0x24)
        self.min_lod, self.max_lod = b.f32(o + 0x28), b.f32(o + 0x2C)

    def level_size(self, lod):
        """(w, h, bytes) of mip level `lod`."""
        w, h = max(1, self.width >> lod), max(1, self.height >> lod)
        return w, h, gxtex.level_bytes(self.fmt, w, h)

    def data_bytes(self, levels=None):
        return sum(self.level_size(i)[2] for i in range(self.levels if levels is None else levels))

    def level_offset(self, lod):
        return self.data_off + sum(self.level_size(i)[2] for i in range(lod))

    def level_data(self, lod):
        o = self.level_offset(lod)
        return bytes(self.b.d[o:o + self.level_size(lod)[2]])

    def decode(self, lod=0, palette=None):
        w, h, n = self.level_size(lod)
        return gxtex.decode(self.level_data(lod), w, h, self.fmt, palette)


class PLT0(SubFile):
    def __init__(self, b, off):
        super().__init__(b, off)
        self.fmt = b.u32(off + 0x18)
        self.count = b.u16(off + 0x1C)
        self.data_off = self.sec(0)

    def colors(self):
        return gxtex.decode_palette(self.b.d[self.data_off:self.data_off + 2 * self.count], self.fmt)


# ---------------------------------------------------------------- animation curves


class Curve:
    """One animated value. kind 'const' (value), 'hermite' (keys: [(frame, value, slope)], nw4r's
    cubic Hermite with slopes per frame, two keys at one frame for a jump) or 'frames' (a value
    per frame 0..n, linear in between)."""

    def __init__(self, kind, value=0.0, keys=None, frames=None):
        self.kind, self.value, self.keys, self.frames = kind, value, keys, frames

    @classmethod
    def const(cls, v):
        return cls("const", value=float(v))

    def __call__(self, f):
        if self.kind == "const":
            return self.value
        if self.kind == "frames":
            v = self.frames
            if f <= 0:
                return v[0]
            if f >= len(v) - 1:
                return v[-1]
            i = int(f)
            t = f - i
            return v[i] * (1 - t) + v[i + 1] * t
        k = self.keys
        if f <= k[0][0]:
            return k[0][1]
        if f >= k[-1][0]:
            return k[-1][1]
        i = 0
        while k[i + 1][0] <= f:
            i += 1
        (f0, v0, s0), (f1, v1, s1) = k[i], k[i + 1]
        return hermite(f, f0, v0, s0, f1, v1, s1)

    def to_dict(self):
        if self.kind == "const":
            return {"const": self.value}
        if self.kind == "frames":
            return {"frames": len(self.frames)}
        return {"keys": [[round(x, 4) for x in k] for k in self.keys]}


def hermite(f, f0, v0, s0, f1, v1, s1):
    """nw4r's key interpolation: cubic Hermite between (f0, v0) and (f1, v1), slopes per frame."""
    d = f1 - f0
    if d <= 0:
        return v1
    t = (f - f0) / d
    t2, t3 = t * t, t * t * t
    return (v0 * (2 * t3 - 3 * t2 + 1) + v1 * (-2 * t3 + 3 * t2) +
            s0 * d * (t3 - 2 * t2 + t) + s1 * d * (t3 - t2))


def read_fvs96(b, o):
    """Keys as f32 frame, value, slope: u16 count, u16 pad, f32 1/frame range, keys (12 bytes)."""
    n = b.u16(o)
    return [tuple(b.fv(o + 8 + 12 * i, 3)) for i in range(n)]


def read_chr_curve(b, o, fmt, n_frames):
    """A CHR0 track's data in format `fmt` (bits 25.. of the node flags): 1 FVS32, 2 FVS48,
    3 FVS96 (Hermite keys packed in 4, 6 or 12 bytes), 4 FRM8, 5 FRM16, 6 FRM32 (a value per
    frame; rotations only). Packed values are base + step * the stored integer."""
    if fmt == 3:
        return Curve("hermite", keys=read_fvs96(b, o))
    if fmt in (1, 2):
        n = b.u16(o)
        step, base = b.f32(o + 8), b.f32(o + 12)
        keys = []
        for i in range(n):
            if fmt == 1:        # u32: frame u8 (bits 24-31), value u12, slope s12 with 5 fraction bits
                k = b.u32(o + 16 + 4 * i)
                slope = ((k & 0xFFF) ^ 0x800) - 0x800
                keys.append((float(k >> 24), base + step * ((k >> 12) & 0xFFF), slope / 32.0))
            else:               # s16 frame * 32, u16 value, s16 slope * 256
                keys.append((b.s16(o + 16 + 6 * i) / 32.0, base + step * b.u16(o + 18 + 6 * i),
                             b.s16(o + 20 + 6 * i) / 256.0))
        return Curve("hermite", keys=keys)
    if fmt in (4, 5):
        step, base = b.f32(o), b.f32(o + 4)
        if fmt == 4:
            vals = [base + step * b.u8(o + 8 + i) for i in range(n_frames + 1)]
        else:
            vals = [base + step * b.u16(o + 8 + 2 * i) for i in range(n_frames + 1)]
        return Curve("frames", frames=vals)
    if fmt == 6:
        return Curve("frames", frames=b.fv(o, n_frames + 1))
    raise ValueError("CHR0 track format %d" % fmt)


class Anim(SubFile):
    """An animation: frames, loop, and its targets' curves (CHR0 nodes, SRT0 texture matrices,
    CLR0 colour registers); PAT0/VIS0/SHP0 keep their header and target names only."""

    def __init__(self, b, off):
        super().__init__(b, off)
        h = self.hdr + 4                     # past the original-path offset
        m = self.magic
        self.targets = [n for n, _ in read_dict(b, self.sec(0))] if self.sec(0) else []
        self.extra = {}
        if m == b"CHR0":       # u16 frames, u16 nodes, u32 loop, u32 scaling rule
            self.frames, self.loop = b.u16(h), b.u32(h + 4)
            self.nodes = {name: self._chr_node(o) for name, o in read_dict(b, self.sec(0))}
        elif m == b"CLR0":     # u16 frames, u16 materials, u32 loop
            self.frames, self.loop = b.u16(h), b.u32(h + 4)
            self.materials = {name: self._clr_mat(o) for name, o in read_dict(b, self.sec(0))}
        elif m == b"SRT0":     # u16 frames, u16 materials, u32 matrix mode, u32 loop
            self.frames, self.mtx_mode, self.loop = b.u16(h), b.u32(h + 4), b.u32(h + 8)
            self.materials = {name: self._srt_mat(o) for name, o in read_dict(b, self.sec(0))}
        elif m == b"PAT0":     # u16 frames, u16 materials, u16 textures, u16 palettes, u32 loop
            self.frames, self.loop = b.u16(h), b.u32(h + 8)
            to = self.sec(1)
            self.extra["textures"] = [b.name(to, b.s32(to + 4 * i)) for i in range(b.u16(h + 4))]
        elif m in (b"VIS0", b"SHP0"):
            self.frames, self.loop = b.u16(h), b.u32(h + 4)
        else:
            self.frames, self.loop = 0, 0

    # CHR0 node data (ResAnmChrNodeData): s32 name, u32 flags, then the present tracks' words
    # (a constant f32 or an offset from the node data to the track). Flags: 1 exists, 2 identity,
    # 4 rotation and translation zero, 8 scale one, 0x10/0x20/0x40 scale/rotation/translation
    # uniform, 0x80/0x100/0x200 scale/rotation/translation from the model, 0x2000 << i the i-th
    # of the 9 components constant, 0x400000/0x800000/0x1000000 has scale/rotation/translation,
    # bits 25-26 / 27-29 / 30-31 their formats.
    def _chr_node(self, o):
        b = self.b
        fl = b.u32(o + 4)
        p = o + 8
        out = dict(flags=fl)
        for part, has, uni, use_model, fmt, k0 in (
                ("scale", 0x400000, 0x10, 0x80, (fl >> 25) & 3, 0),
                ("rot", 0x800000, 0x20, 0x100, (fl >> 27) & 7, 3),
                ("trans", 0x1000000, 0x40, 0x200, (fl >> 30) & 3, 6)):
            if not fl & has:
                out[part] = "model" if fl & use_model else None
                continue
            curves = []
            for i in range(1 if fl & uni else 3):
                if fl & (0x2000 << (k0 + i)):
                    curves.append(Curve.const(b.f32(p)))
                else:
                    curves.append(read_chr_curve(b, o + b.s32(p), fmt, self.frames))
                p += 4
            out[part] = curves * 3 if len(curves) == 1 else curves
        return out

    # SRT0 material data: s32 name, u32 texture matrix bits, u32 indirect matrix bits, then an
    # offset (from the material data) to each present matrix's data: u32 flags (1 exists, 2 scale
    # one, 4 rotation zero, 8 translation zero, 0x10 scale uniform, 0x20/0x40 scale s/t constant,
    # 0x80 rotation constant, 0x100/0x200 translation s/t constant), then the words (constant f32
    # or an offset from the word itself to FVS96 keys) of scale s[, t], rotation, translation s, t
    def _srt_mat(self, o):
        b = self.b
        tex_bits, ind_bits = b.u32(o + 4), b.u32(o + 8)
        out = {}
        k = 0
        for i in range(8 + 3):
            bits, j = (tex_bits, i) if i < 8 else (ind_bits, i - 8)
            if not bits & (1 << j):
                continue
            to = o + b.s32(o + 12 + 4 * k)
            k += 1
            fl = b.u32(to)
            p = to + 4

            def word(const_bit):
                nonlocal p
                c = Curve.const(b.f32(p)) if fl & const_bit else Curve("hermite", keys=read_fvs96(b, p + b.s32(p)))
                p += 4
                return c
            if fl & 2:
                sc = [Curve.const(1), Curve.const(1)]
            else:
                s0 = word(0x20)
                sc = [s0, s0] if fl & 0x10 else [s0, word(0x40)]
            rot = Curve.const(0) if fl & 4 else word(0x80)
            tr = [Curve.const(0), Curve.const(0)] if fl & 8 else [word(0x100), word(0x200)]
            out[("ind%d" % j) if i >= 8 else j] = dict(flags=fl, scale=sc, rot=rot, trans=tr)
        return out

    # CLR0 material data: s32 name, u32 flags (2 bits a target: 1 exists, 2 constant), then per
    # present target u32 mask (bits set: keep the material's own) and a word: the RGBA constant or
    # an offset (from the word) to RGBA values, one a frame (frames + 1 of them)
    CLR_TARGETS = ["mat0", "mat1", "amb0", "amb1", "tev0", "tev1", "tev2", "k0", "k1", "k2", "k3"]

    def _clr_mat(self, o):
        b = self.b
        fl = b.u32(o + 4)
        p = o + 8
        out = {}
        for t in range(11):
            if not fl & (1 << (2 * t)):
                continue
            mask = b.u32(p)
            if fl & (2 << (2 * t)):
                vals = [b.u32(p + 4)]
            else:
                q = p + 4 + b.s32(p + 4)
                vals = [b.u32(q + 4 * i) for i in range(self.frames + 1)]
            out[t] = dict(name=self.CLR_TARGETS[t], mask=mask, values=vals)
            p += 8
        return out


class SCN0(SubFile):
    """A scene: light sets (light names + an ambient name), lights and ambient lights with their
    ids (the env lights' indices), fogs."""

    def __init__(self, b, off):
        super().__init__(b, off)
        h = self.hdr + 4
        self.frames, self.loop = b.u16(h), b.u32(h + 4)
        secs = dict(read_dict(b, self.sec(0)))
        self.light_sets, self.lights, self.ambients, self.fogs = [], {}, {}, {}
        for nm, e in read_dict(b, secs.get("LightSet(NW4R)", 0)) if "LightSet(NW4R)" in secs else []:
            sid = b.u32(e + 0x0C)
            amb = b.s32(e + 0x14)
            n = b.u8(e + 0x1A)
            names = [b.cstr(e + 0x1C + b.s32(e + 0x1C + 4 * k)) for k in range(n)]
            self.light_sets.append(dict(id=sid, name=nm, ambient=b.cstr(e + amb) if amb else None,
                                        lights=names))
        for key, sec in (("lights", "Lights(NW4R)"), ("ambients", "AmbLights(NW4R)"), ("fogs", "Fogs(NW4R)")):
            if sec in secs:
                for nm, e in read_dict(b, secs[sec]):
                    getattr(self, key)[nm] = b.u32(e + 0x0C)
        self.targets = [s["name"] for s in self.light_sets]


KINDS = {b"MDL0": MDL0, b"TEX0": TEX0, b"PLT0": PLT0, b"SCN0": SCN0}


class RawEntry:
    """A folder entry that is not an nw4r subfile (Wii no Ma keeps raw user data in some BRRES
    folders)."""

    def __init__(self, b, off):
        self.b, self.off = b, off
        self.magic = bytes(b.d[off:off + 4])
        self.version = None
        self.name = None


class BRRES:
    def __init__(self, data, path=None):
        self.path = path
        self.b = Buf(data)
        b = self.b
        assert data[:4] == b"bres", "not a BRRES"
        assert b.u16(4) == 0xFEFF, "not big-endian"
        root = b.u16(0x0C)
        assert data[root:root + 4] == b"root"
        self.folders = {}
        self.models, self.textures, self.palettes, self.anims, self.scenes = {}, {}, {}, {}, {}
        self.raw = {}
        for folder, fo in read_dict(b, root + 8):
            items = {}
            for name, so in read_dict(b, fo):
                magic = bytes(data[so:so + 4])
                if (magic, b.u32(so + 8)) not in SECTIONS:
                    items[name] = self.raw[name] = RawEntry(b, so)
                    continue
                obj = KINDS.get(magic, Anim)(b, so)
                items[name] = obj
                if magic == b"MDL0":
                    self.models[name] = obj
                elif magic == b"TEX0":
                    self.textures[name] = obj
                elif magic == b"PLT0":
                    self.palettes[name] = obj
                elif magic == b"SCN0":
                    self.scenes[name] = obj
                else:
                    self.anims.setdefault(magic.decode(), {})[name] = obj
            self.folders[folder] = items

    @classmethod
    def load(cls, path):
        with open(path, "rb") as f:
            return cls(f.read(), path)

    def palette_of(self, name):
        t = self.textures[name]
        if t.fmt in gxtex.PALETTED:
            p = self.palettes.get(name)
            return p.colors() if p else None
        return None

    def texture_rgba(self, name, lod=0):
        return self.textures[name].decode(lod, self.palette_of(name))


# ---------------------------------------------------------------- CLI


def _bb(bb):
    return "(%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f)" % tuple(list(bb[0]) + list(bb[1]))


def cmd_ls(paths):
    for p in paths:
        r = BRRES.load(p)
        print("==", p, len(r.b.d))
        for folder, items in r.folders.items():
            print(" %s: %d" % (folder, len(items)))
            for name, o in items.items():
                m = o.magic.decode("ascii", "replace")
                if m == "MDL0":
                    s = o.summary()
                    ga, gd = o.geometry_bytes()
                    print("   %-34s MDL0 %8d vtx=%d tri=%d nodes=%d mats=%d shp=%d arr=%d dl=%d bb=%s" % (
                        name, o.size, s["vertices"], s["triangles"], s["nodes"], s["materials"],
                        s["shapes"], ga, gd, _bb(s["bbox"])))
                elif m == "TEX0":
                    print("   %-34s TEX0 %8d %dx%d %s lv=%d" % (name, o.size, o.width, o.height, o.fmt_name, o.levels))
                elif m == "PLT0":
                    print("   %-34s PLT0 %8d %s n=%d" % (name, o.size, PLT_FMT.get(o.fmt), o.count))
                elif isinstance(o, RawEntry):
                    print("   %-34s raw" % name)
                else:
                    print("   %-34s %s %8d frames=%d loop=%d targets=%s" % (
                        name, m, o.size, o.frames, o.loop, ",".join(o.targets[:12]) +
                        ("..." if len(o.targets) > 12 else "")))


def cmd_model(path, which=None):
    r = BRRES.load(path)
    for name, m in r.models.items():
        if which and name != which:
            continue
        print("== MDL0", name, m.info)
        print(" draw lists:", {k: len(v) for k, v in m.bytecode.items()})
        for n in m.nodes:
            print("  node %2d %-28s parent=%-3d mtx=%-3d flags=%04x bb=%d S=%s R=%s T=%s bb=%s" % (
                n.index, n.name, n.parent, n.mtx_id, n.flags, n.billboard,
                [round(x, 3) for x in n.scale], [round(x, 2) for x in n.rot],
                [round(x, 1) for x in n.trans], _bb(n.bbox)))
        for kind, arrs in (("pos", m.pos), ("nrm", m.nrm), ("clr", m.clr), ("uv", m.uv)):
            for a in arrs:
                f = CLR_FMT[a.fmt] if kind == "clr" else COMP_TYPE[a.fmt]
                print("  %s %3d %-34s %s x%d frac=%d n=%d" % (kind, a.index, a.name, f, a.ncomp, a.frac, a.count))
        for mt in m.materials:
            print("  mat %2d %-30s %s cull=%s tev=%d ind=%d fog=%d lset=%d tex=%s" % (
                mt.index, mt.name, "XLU" if mt.xlu else "OPA", CULL[mt.cull], mt.n_stages,
                mt.n_ind, mt.fog, mt.light_set, [l["tex"] for l in mt.layers]))
        draws = {}
        for d in m.draws():
            draws.setdefault(d.shape, []).append(d)
        for p in m.shapes:
            for d in draws.get(p.index, [Draw("-", -1, p.index, -1, 0)]):
                print("  shp %3d %-34s v=%d f=%d mtx=%d mtxs=%s %s mat=%s node=%s prio=%d vcd=%s" % (
                    p.index, p.name, p.num_vertices, p.num_faces, p.mtx_id, p.mtx_list[:8], d.list,
                    m.materials[d.mat].name if d.mat >= 0 else "-",
                    m.nodes[d.node].name if 0 <= d.node < len(m.nodes) else d.node, d.prio,
                    ",".join(a for a, _ in p.vcd["order"])))


def cmd_mat(path, model, which=None):
    r = BRRES.load(path)
    m = r.models[model]
    for mt in m.materials:
        if which and mt.name != which:
            continue
        print("== mat %d %s (%s) cull=%s early_z=%d fog=%d light_set=%d ind=%d %s" % (
            mt.index, mt.name, "XLU" if mt.xlu else "OPA", CULL[mt.cull], mt.early_z, mt.fog,
            mt.light_set, mt.n_ind, mt.ind_method[:mt.n_ind]))
        for i, l in enumerate(mt.layers):
            print("  layer %d tex=%s map=%d wrap=%s %s/%s bias=%.2f" % (
                i, l["tex"], l["map_id"], l["wrap"], l["min"], l["mag"], l["lod_bias"]))
        for i in range(mt.n_texgens):
            s = mt.srt[i]
            print("  texgen %d %s | srt flag=%x S=%s R=%.1f T=%s map=%s cam=%d light=%d" % (
                i, {k: v for k, v in mt.texgens[i].items() if k != "post_raw"}, s["flag"], s["scale"],
                s["rot"], s["trans"], s["map_mode"], s["cam"], s["light"]))
        for i, c in enumerate(mt.chans[:mt.n_chans]):
            print("  chan %d mat=%s amb=%s C=%s A=%s" % (i, c["mat"], c["amb"],
                                                         {k: v for k, v in c["color"].items() if k != "raw"},
                                                         {k: v for k, v in c["alpha"].items() if k != "raw"}))
        print("  alpha", mt.alpha_test, "z", mt.zmode, "blend", mt.blend, "constA", mt.const_alpha)
        print("  regs", mt.tev_regs, "swap", mt.swap_tables)
        for i, s in enumerate(mt.stages):
            print("  tev %d map=%s coord=%s %s k=(%d,%d) %s | %s%s" % (
                i, s["texmap"], s["texcoord"], s["chan"], s["kc"], s["ka"], s["color"], s["alpha"],
                " ind=%06x" % s["ind"] if s["ind"] else ""))


def cmd_anim(path, which=None):
    r = BRRES.load(path)
    for kind, d in r.anims.items():
        for name, a in d.items():
            if which and name != which:
                continue
            print("== %s %s frames=%d loop=%d" % (kind, name, a.frames, a.loop))
            if kind == "CHR0":
                for n, v in a.nodes.items():
                    print("  node %s flags=%08x" % (n, v["flags"]))
                    for part in ("scale", "rot", "trans"):
                        c = v[part]
                        print("    %s %s" % (part, c if not isinstance(c, list) else
                                             [json.dumps(x.to_dict())[:120] for x in c]))
            elif kind == "SRT0":
                for n, v in a.materials.items():
                    for t, x in v.items():
                        print("  %s tex%s flags=%x S=%s R=%s T=%s" % (
                            n, t, x["flags"], [c.to_dict() for c in x["scale"]], x["rot"].to_dict(),
                            [c.to_dict() for c in x["trans"]]))
            elif kind == "CLR0":
                for n, v in a.materials.items():
                    for t, x in v.items():
                        print("  %s %s mask=%08x %d values: %s" % (
                            n, x["name"], x["mask"], len(x["values"]), " ".join("%08x" % c for c in x["values"][:8])))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    c = argv[1]
    if c == "ls":
        cmd_ls(argv[2:])
    elif c == "model":
        cmd_model(argv[2], argv[3] if len(argv) > 3 else None)
    elif c == "mat":
        cmd_mat(argv[2], argv[3], argv[4] if len(argv) > 4 else None)
    elif c == "anim":
        cmd_anim(argv[2], argv[3] if len(argv) > 3 else None)
    elif c == "tex":
        from PIL import Image
        r = BRRES.load(argv[2])
        Image.fromarray(r.texture_rgba(argv[3], int(argv[5]) if len(argv) > 5 else 0), "RGBA").save(argv[4])
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
