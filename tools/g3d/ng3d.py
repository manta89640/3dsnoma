#!/usr/bin/env python3
"""NG3D packages (engine/g3d/ng3d_format.h): the Python side of the format, writer and reader.

    import ng3d
    p = ng3d.Package()
    t = p.add("textures", name=p.str("jog_kusa"), width=256, ...)   # records are Rec objects
    p.texture_data.append(gx_bytes)                                # payloads beside the records
    open("x.ng3d", "wb").write(p.write())
    q = ng3d.Package.read(open("x.ng3d", "rb").read())             # the same records back

    ng3d.py selftest            the structs below against the header (fields, sizes, tags, enums)
    ng3d.py info FILE           sections, counts, sizes
    ng3d.py check FILE          validate every offset, index and payload size, and round-trip (read,
                                write, compare: the bytes, or for another writer's file the records)

The header is the definition: STRUCTS mirrors each of its structs field by field (name, type, array
size), SECTIONS its section tags and ENUMS its enumerations, and `selftest` parses the header and
compares, sizes included (a struct's size from its fields with C's natural alignment, and its
static_assert), so a change on one side and not the other fails loudly instead of making packages
the engine misreads. Little-endian, no padding inside the structs (the header lays them out so).

The file: Header, Section[n_sections], then the sections, each on a 32-byte boundary (TDAT on 128,
each texture's data 128-aligned in it; each vertex buffer's data 16-aligned in VDAT; each BLOB entry
32-aligned). Records refer to strings by their offset in STRS (0 = ""), to data by file offsets
(Texture.data, VertexBuffer.data, Collision.data), to each other by index. The writer lays the
sections out in a fixed order and fills those file offsets in, so writing what the reader read
gives the same bytes.
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gxtex  # noqa: E402

ROOT =os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HEADER = os.path.join(ROOT, "engine/g3d/ng3d_format.h")

MAGIC = 0x4433474E          # 'NG3D'
VERSION = 2
NONE16 = 0xFFFF
NONE32 = 0xFFFFFFFF

# ------------------------------------------------------------------------------------- the structs
# (field, type, array dimensions); type is a scalar (u8 u16 u32 s8 s16 f32) or another struct's name
STRUCTS = {
    "Header": [("magic", "u32", ()), ("version", "u32", ()), ("file_size", "u32", ()),
               ("n_sections", "u32", ()), ("flags", "u32", ()), ("reserved", "u32", (3,))],
    "Section": [("tag", "u32", ()), ("offset", "u32", ()), ("size", "u32", ()), ("count", "u32", ())],
    "Texture": [("name", "u32", ()), ("width", "u16", ()), ("height", "u16", ()), ("format", "u8", ()),
                ("levels", "u8", ()), ("flags", "u8", ()), ("gx_format", "u8", ()), ("data", "u32", ()),
                ("size", "u32", ()), ("src_width", "u16", ()), ("src_height", "u16", ()),
                ("reserved", "u32", ())],
    "TexMap": [("texture", "u16", ()), ("wrap_s", "u8", ()), ("wrap_t", "u8", ()), ("min_filter", "u8", ()),
               ("mag_filter", "u8", ()), ("max_aniso", "u8", ()), ("flags", "u8", ()), ("lod_bias", "f32", ()),
               ("min_lod", "f32", ()), ("max_lod", "f32", ())],
    "TexGen": [("type", "u8", ()), ("source", "u8", ()), ("normalize", "u8", ()), ("pad", "u8", ()),
               ("matrix", "u16", ()), ("post_mtx", "u16", ())],
    "TexMtx": [("scale", "f32", (2,)), ("rotate", "f32", ()), ("translate", "f32", (2,)), ("mode", "u8", ()),
               ("map_mode", "u8", ()), ("ref_camera", "u8", ()), ("ref_light", "u8", ()),
               ("effect", "f32", (3, 4))],
    "Material": [("name", "u32", ()), ("flags", "u32", ()), ("n_stages", "u8", ()), ("n_texgens", "u8", ()),
                 ("n_chans", "u8", ()), ("n_ind", "u8", ()), ("cull", "u8", ()), ("light_set", "u8", ()),
                 ("fog", "u8", ()), ("priority", "s8", ()), ("color_env", "u32", (16,)),
                 ("alpha_env", "u32", (16,)), ("tref", "u32", (8,)), ("ksel", "u32", (8,)),
                 ("tev_reg", "s16", (4, 4)), ("tev_konst", "s16", (4, 4)), ("alpha_compare", "u32", ()),
                 ("zmode", "u32", ()), ("blend", "u32", ()), ("const_alpha", "u32", ()),
                 ("chan_color", "u32", ()), ("chan_alpha", "u32", ()), ("mat_color", "u8", (4,)),
                 ("amb_color", "u8", (4,)), ("maps", "TexMap", (8,)), ("texgens", "TexGen", (8,)),
                 ("tex_mtx_first", "u16", ()), ("n_tex_mtx", "u16", ()), ("reserved", "u32", (2,))],
    "VertexBuffer": [("data", "u32", ()), ("count", "u32", ()), ("attrs", "u8", ()), ("stride", "u8", ()),
                     ("flags", "u8", ()), ("pad", "u8", ()), ("uv_scale", "f32", (2,)), ("pos_origin", "f32", (3,)),
                     ("pos_scale", "f32", ())],
    "Batch": [("material", "u16", ()), ("vbuf", "u16", ()), ("first_index", "u32", ()),
              ("n_indices", "u32", ()), ("bbox", "f32", (6,)), ("node", "u16", ()), ("flags", "u16", ())],
    "Node": [("name", "u32", ()), ("parent", "u16", ()), ("flags", "u16", ()), ("scale", "f32", (3,)),
             ("rotate", "f32", (3,)), ("translate", "f32", (3,)), ("bind", "f32", (3, 4)),
             ("billboard", "u8", ()), ("pad", "u8", (3,)), ("first_batch", "u16", ()), ("n_batches", "u16", ())],
    "Model": [("name", "u32", ()), ("first_node", "u16", ()), ("n_nodes", "u16", ()), ("first_batch", "u16", ()),
              ("n_batches", "u16", ()), ("bbox", "f32", (6,)), ("radius", "f32", ()), ("lod_next", "u16", ()),
              ("pad", "u16", ()), ("lod_distance", "f32", ())],
    "Cell": [("bbox", "f32", (6,)), ("first", "u32", (3,)), ("count", "u16", (3,)), ("n_lods", "u16", ()),
             ("lod_distance", "f32", (3,)), ("group", "u32", ())],
    "Instance": [("model", "u16", ()), ("flags", "u16", ()), ("anim", "u16", ()), ("class_id", "u16", ()),
                 ("mtx", "f32", (3, 4)), ("anim_offset", "f32", ()), ("cull_distance", "f32", ())],
    "Anim": [("name", "u32", ()), ("frames", "f32", ()), ("loop", "u8", ()), ("pad", "u8", (3,)),
             ("first_track", "u32", ()), ("n_tracks", "u32", ())],
    "Track": [("target", "u8", ()), ("interp", "u8", ()), ("reg", "u8", ()), ("pad", "u8", ()),
              ("index", "u16", ()), ("model", "u16", ()), ("first_key", "u32", ()), ("n_keys", "u32", ())],
    "Key": [("frame", "f32", ()), ("value", "f32", ()), ("slope", "f32", ())],
    "Light": [("name", "u32", ()), ("kind", "u8", ()), ("rgba", "u8", (4,)), ("pad", "u8", (3,)),
              ("position", "f32", (3,)), ("direction", "f32", (3,))],
    "LightSet": [("name", "u32", ()), ("lights", "u16", (8,)), ("ambient", "u8", (4,))],
    "Fog": [("type", "u8", ()), ("rgba", "u8", (4,)), ("pad", "u8", (3,)), ("start", "f32", ()), ("end", "f32", ())],
    "Marker": [("name", "u32", ()), ("kind", "u32", ()), ("position", "f32", (3,)), ("dir", "f32", (3,))],
    "Collision": [("name", "u32", ()), ("data", "u32", ()), ("size", "u32", ())],
    "Cylinder": [("x", "f32", ()), ("z", "f32", ()), ("y0", "f32", ()), ("y1", "f32", ()), ("radius", "f32", ())],
    "Env": [("sky_model", "u16", ()), ("sky_anim", "u16", ()), ("sea_model", "u16", ()),
            ("sea_anim_srt", "u16", ()), ("sea_anim_clr", "u16", ()), ("world_anim_srt", "u16", ()),
            ("world_anim_clr", "u16", ()), ("pad", "u16", ()), ("world_bbox", "f32", (6,)),
            ("mii_units_per_unit", "f32", ()), ("cell_size", "f32", ()), ("n_cells_x", "u32", ()),
            ("n_cells_z", "u32", ()), ("grid_origin", "f32", (2,)), ("pvs_row_bytes", "u32", ()),
            ("pvs_max_height", "f32", ()), ("detail_distance", "f32", ()), ("reserved", "u32", (5,))],
}

# section tags, in the order this writer lays them out (small tables first, bulk data last)
SECTIONS = [
    ("SEC_STRS", "STRS", None), ("SEC_TEXS", "TEXS", "Texture"), ("SEC_MATS", "MATS", "Material"),
    ("SEC_TMTX", "TMTX", "TexMtx"), ("SEC_VTXB", "VTXB", "VertexBuffer"), ("SEC_BTCH", "BTCH", "Batch"),
    ("SEC_NODE", "NODE", "Node"), ("SEC_MODL", "MODL", "Model"), ("SEC_CELL", "CELL", "Cell"),
    ("SEC_CBAT", "CBAT", "u16"), ("SEC_INST", "INST", "Instance"), ("SEC_ANIM", "ANIM", "Anim"),
    ("SEC_TRAK", "TRAK", "Track"), ("SEC_KEYS", "KEYS", "Key"), ("SEC_ENVR", "ENVR", "Env"),
    ("SEC_LITE", "LITE", "Light"), ("SEC_LSET", "LSET", "LightSet"), ("SEC_FOGS", "FOGS", "Fog"),
    ("SEC_MARK", "MARK", "Marker"), ("SEC_COLL", "COLL", "Collision"), ("SEC_CYLS", "CYLS", "Cylinder"),
    ("SEC_CVIS", "CVIS", None), ("SEC_IDAT", "IDAT", "u16"), ("SEC_VDAT", "VDAT", None), ("SEC_TDAT", "TDAT", None),
    ("SEC_BLOB", "BLOB", None),
]
# the Package attribute holding each array section's records
SECTION_LISTS = {"TEXS": "textures", "MATS": "materials", "TMTX": "tex_mtx", "VTXB": "vertex_buffers",
                 "BTCH": "batches", "NODE": "nodes", "MODL": "models", "CELL": "cells", "CBAT": "cell_batches",
                 "INST": "instances", "ANIM": "anims", "TRAK": "tracks", "KEYS": "keys", "LITE": "lights",
                 "LSET": "light_sets", "FOGS": "fogs", "MARK": "markers", "COLL": "collisions",
                 "CYLS": "cylinders", "IDAT": "indices"}

# the struct of each section list's records
LIST_KIND = {lst: kind for t4, lst in SECTION_LISTS.items() for _, tag4, kind in SECTIONS if tag4 == t4}

ENUMS = {
    "TexFormat": dict(TF_RGBA8=0, TF_RGB8=1, TF_RGBA5551=2, TF_RGB565=3, TF_RGBA4=4, TF_LA8=5, TF_HILO8=6,
                      TF_L8=7, TF_A8=8, TF_LA4=9, TF_L4=10, TF_A4=11, TF_ETC1=12, TF_ETC1A4=13),
    "TexFlags": dict(TEX_GX_RAW=1, TEX_ALPHA=2, TEX_CUTOUT=4),
    "MatFlags": dict(MAT_XLU=1, MAT_FOG=2, MAT_INDIRECT=4, MAT_LIGHT_SET=8, MAT_PRELIT=16),
    "VtxAttr": dict(VA_POS=1, VA_NRM=2, VA_CLR=4, VA_UV0=8, VA_UV1=16, VA_POS16=32),
    "VbFlags": dict(VB_UV_FLOAT=1),
    "BatchFlags": dict(BATCH_DETAIL=1),
    "NodeFlags": dict(NODE_VISIBLE=1, NODE_BILLBOARD=2),
    "InstFlags": dict(INST_STATIC=1, INST_COLLIDE=2, INST_ANIMATED=4),
    "TrackTarget": dict(TT_NODE_SCALE_X=0, TT_NODE_SCALE_Y=1, TT_NODE_SCALE_Z=2, TT_NODE_ROT_X=3,
                        TT_NODE_ROT_Y=4, TT_NODE_ROT_Z=5, TT_NODE_TRANS_X=6, TT_NODE_TRANS_Y=7,
                        TT_NODE_TRANS_Z=8, TT_TEXMTX_SCALE_S=16, TT_TEXMTX_SCALE_T=17, TT_TEXMTX_ROTATE=18,
                        TT_TEXMTX_TRANS_S=19, TT_TEXMTX_TRANS_T=20, TT_MAT_REG_R=32, TT_MAT_REG_G=33,
                        TT_MAT_REG_B=34, TT_MAT_REG_A=35),
    "Interp": dict(IP_STEP=0, IP_LINEAR=1, IP_HERMITE=2),
    "ColorReg": dict(CR_MAT0=0, CR_MAT1=1, CR_AMB0=2, CR_AMB1=3, CR_TEV0=4, CR_TEV1=5, CR_TEV2=6,
                     CR_KONST0=8, CR_KONST1=9, CR_KONST2=10, CR_KONST3=11),
    "LightKind": dict(LK_DIRECTIONAL=0, LK_POINT=1),
    "FogType": dict(FOG_NONE=0, FOG_LINEAR=2),
    "MarkerKind": dict(MK_SPAWN=0, MK_LANDMARK=1, MK_CAMERA=2, MK_WARP=3),
}
CONSTANTS = dict(MAX_TEV_STAGES=16, MAX_TEX_MAPS=8, MAX_TEXGENS=8, MAX_CELL_LODS=3)
# every enum value as a module constant (ng3d.TF_ETC1, ng3d.MAT_XLU, ...)
for _e in ENUMS.values():
    globals().update(_e)
globals().update(CONSTANTS)

SCALAR = {"u8": ("B", 1), "u16": ("H", 2), "u32": ("I", 4), "s8": ("b", 1), "s16": ("h", 2), "f32": ("f", 4)}


def tag(s):
    return struct.unpack("<I", s.encode("ascii"))[0]


# ------------------------------------------------------------------------------------- records


def _prod(dims):
    n = 1
    for d in dims:
        n *= d
    return n


def _fmt(kind):
    """The struct format (no byte order) of one record of `kind`."""
    if kind in SCALAR:
        return SCALAR[kind][0]
    return "".join(_fmt(t) * _prod(dims) for _, t, dims in STRUCTS[kind])


def _default(t, dims):
    if dims:
        return [_default(t, dims[1:]) for _ in range(dims[0])]
    return Rec(t) if t in STRUCTS else (0.0 if t == "f32" else 0)


class Rec:
    """One record of a struct kind: its fields as attributes (arrays as nested lists, struct
    members as Recs), zero by default."""
    __slots__ = ("_kind", "_v")

    def __init__(self, kind, /, **fields):
        object.__setattr__(self, "_kind", kind)
        v = {name: _default(t, dims) for name, t, dims in STRUCTS[kind]}
        for k, x in fields.items():
            if k not in v:
                raise KeyError("%s has no field %s" % (kind, k))
            v[k] = x
        object.__setattr__(self, "_v", v)

    def __getattr__(self, k):
        try:
            return self._v[k]
        except KeyError:
            raise AttributeError("%s has no field %s" % (self._kind, k))

    def __setattr__(self, k, x):
        if k not in self._v:
            raise AttributeError("%s has no field %s" % (self._kind, k))
        self._v[k] = x

    def __eq__(self, o):
        return isinstance(o, Rec) and o._kind == self._kind and o._v == self._v

    def __repr__(self):
        return "%s(%s)" % (self._kind, ", ".join("%s=%r" % kv for kv in self._v.items()))

    def flat(self, out):
        for name, t, dims in STRUCTS[self._kind]:
            _flatten(self._v[name], t, dims, out)
        return out

    def pack(self):
        return struct.pack("<" + _fmt(self._kind), *self.flat([]))

    @classmethod
    def unpack(cls, kind, data, off=0):
        vals = struct.unpack_from("<" + _fmt(kind), data, off)
        r, _ = cls._build(kind, vals, 0)
        return r

    @classmethod
    def _build(cls, kind, vals, i):
        r = cls(kind)
        for name, t, dims in STRUCTS[kind]:
            r._v[name], i = _unflatten(t, dims, vals, i)
        return r, i


def _flatten(x, t, dims, out):
    if dims:
        if len(x) != dims[0]:
            raise ValueError("array of %d where %d are expected" % (len(x), dims[0]))
        for e in x:
            _flatten(e, t, dims[1:], out)
    elif t in STRUCTS:
        x.flat(out)
    else:
        out.append(x)


def _unflatten(t, dims, vals, i):
    if dims:
        out = []
        for _ in range(dims[0]):
            e, i = _unflatten(t, dims[1:], vals, i)
            out.append(e)
        return out, i
    if t in STRUCTS:
        return Rec._build(t, vals, i)
    return vals[i], i + 1


def sizeof(kind):
    return struct.calcsize("<" + _fmt(kind))


# bits a texel of each PICA format (TexFormat order)
PICA_BITS = [32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8]


def texture_bytes(t):
    """The bytes a Texture record's levels take: GX's level sizes from the source size (TEX_GX_RAW;
    each level max(1, src >> level), padded to GX's tiles), else the PICA format's at width x
    height. None for a format this doesn't know."""
    if t.flags & TEX_GX_RAW:
        if t.gx_format not in gxtex.BLOCK:
            return None
        return sum(gxtex.level_bytes(t.gx_format, max(1, t.src_width >> l), max(1, t.src_height >> l))
                   for l in range(t.levels))
    if t.format >= len(PICA_BITS):
        return None
    return sum((t.width >> l) * (t.height >> l) * PICA_BITS[t.format] // 8 for l in range(t.levels))


def vertex_bytes(attrs, flags):
    """Bytes of a vertex of this layout (VtxAttr order: POS or POS16, NRM, CLR, UV0, UV1)."""
    uv = 8 if flags & VB_UV_FLOAT else 4
    return (12 if attrs & VA_POS else 8 if attrs & VA_POS16 else 0) + (4 if attrs & VA_NRM else 0) + \
        (4 if attrs & VA_CLR else 0) + (uv if attrs & VA_UV0 else 0) + (uv if attrs & VA_UV1 else 0)


def vertex_positions(vb, data):
    """A vertex buffer's positions as (count, 3) float32, decoded (VA_POS16: s16 * pos_scale +
    pos_origin, as the header says), from its bytes."""
    import numpy as np
    raw = np.frombuffer(data, np.uint8)[:vb.count * vb.stride].reshape(vb.count, vb.stride)
    if vb.attrs & VA_POS16:
        q = raw[:, 0:6].copy().view("<i2").reshape(-1, 3).astype(np.float32)
        return q * np.float32(vb.pos_scale) + np.array(vb.pos_origin, np.float32)
    return raw[:, 0:12].copy().view("<f4").reshape(-1, 3)


# ------------------------------------------------------------------------------------- the package


def _align(n, a):
    return (n + a - 1) // a * a


class Package:
    """A package in memory: a list of records per array section (attributes named after
    SECTION_LISTS), `env` (one Env), the string table, and the payloads the records point to:
    texture_data[i] (Texture i's bytes, all its levels), vertex_data[i] (VertexBuffer i's bytes),
    blobs[i] (Collision i's bytes). `indices` and `cell_batches` are lists of ints (u16); `cvis` is the
    CVIS section's bytes (Env.pvs_row_bytes a cell, bit j of row i at byte j >> 3, bit j & 7)."""

    def __init__(self):
        for attr in SECTION_LISTS.values():
            setattr(self, attr, [])
        self.env = None
        self.strs = bytearray(b"\0")
        self._str_index = {"": 0}
        self.texture_data, self.vertex_data, self.blobs = [], [], []
        self.cvis = b""

    # strings: offset in STRS, deduplicated
    def str(self, s):
        if s in self._str_index:
            return self._str_index[s]
        o = len(self.strs)
        self.strs += s.encode("utf-8") + b"\0"
        self._str_index[s] = o
        return o

    def string(self, off):
        e = self.strs.index(b"\0", off)
        return bytes(self.strs[off:e]).decode("utf-8")

    def add(self, section, **fields):
        """Append a record to an array section's list (e.g. "textures"); returns its index."""
        lst = getattr(self, section)
        lst.append(Rec(LIST_KIND[section], **fields))
        return len(lst) - 1

    # ---------------------------------------------------------------- write
    def _section_bytes(self, tag4):
        """(bytes, count) of a section, with its data records' file offsets still local."""
        if tag4 == "STRS":
            return bytes(self.strs), 0
        if tag4 == "ENVR":
            return (self.env.pack(), 1) if self.env is not None else (b"", 0)
        if tag4 == "CVIS":
            rb = self.env.pvs_row_bytes if self.env is not None else 0
            return bytes(self.cvis), (len(self.cvis) // rb if rb else 0)
        if tag4 in ("CBAT", "IDAT"):
            lst = getattr(self, SECTION_LISTS[tag4])
            return struct.pack("<%dH" % len(lst), *lst), len(lst)
        if tag4 in SECTION_LISTS:
            lst = getattr(self, SECTION_LISTS[tag4])
            return b"".join(r.pack() for r in lst), len(lst)
        raise KeyError(tag4)

    def _layout_payload(self, items, align):
        """Local offsets of payload items packed at `align`, and the total size."""
        offs, n = [], 0
        for it in items:
            n = _align(n, align)
            offs.append(n)
            n += len(it)
        return offs, n

    def write(self):
        if len(self.texture_data) != len(self.textures) or len(self.vertex_data) != len(self.vertex_buffers) \
                or len(self.blobs) != len(self.collisions):
            raise ValueError("a payload is missing: %d/%d textures, %d/%d vertex buffers, %d/%d blobs" % (
                len(self.texture_data), len(self.textures), len(self.vertex_data), len(self.vertex_buffers),
                len(self.blobs), len(self.collisions)))
        tex_offs, tdat_size = self._layout_payload(self.texture_data, 128)
        vtx_offs, vdat_size = self._layout_payload(self.vertex_data, 16)
        blob_offs, blob_size = self._layout_payload(self.blobs, 32)
        present = []
        for name, t4, kind in SECTIONS:
            if t4 == "TDAT":
                size = tdat_size
            elif t4 == "VDAT":
                size = vdat_size
            elif t4 == "BLOB":
                size = blob_size
            else:
                size = len(self._section_bytes(t4)[0])
            if size:
                present.append((t4, size))
        n = len(present)
        pos = _align(sizeof("Header") + n * sizeof("Section"), 32)
        where = {}
        for t4, size in present:
            pos = _align(pos, 128 if t4 == "TDAT" else 32)
            where[t4] = pos
            pos += size
        file_size = pos
        # file offsets into the records that point at data
        for i, t in enumerate(self.textures):
            t.data, t.size = where["TDAT"] + tex_offs[i], len(self.texture_data[i])
        for i, v in enumerate(self.vertex_buffers):
            v.data = where["VDAT"] + vtx_offs[i]
        for i, c in enumerate(self.collisions):
            c.data, c.size = where["BLOB"] + blob_offs[i], len(self.blobs[i])
        out = bytearray(file_size)
        hdr = Rec("Header", magic=MAGIC, version=VERSION, file_size=file_size, n_sections=n)
        out[0:sizeof("Header")] = hdr.pack()
        for k, (t4, size) in enumerate(present):
            if t4 in ("TDAT", "VDAT", "BLOB"):
                count = 0
                items, offs = {"TDAT": (self.texture_data, tex_offs), "VDAT": (self.vertex_data, vtx_offs),
                               "BLOB": (self.blobs, blob_offs)}[t4]
                for it, o in zip(items, offs):
                    out[where[t4] + o:where[t4] + o + len(it)] = it
            else:
                data, count = self._section_bytes(t4)
                out[where[t4]:where[t4] + len(data)] = data
            sec = Rec("Section", tag=tag(t4), offset=where[t4], size=size, count=count)
            o = sizeof("Header") + k * sizeof("Section")
            out[o:o + sizeof("Section")] = sec.pack()
        return bytes(out)

    # ---------------------------------------------------------------- read
    @classmethod
    def read(cls, data):
        hdr = Rec.unpack("Header", data)
        if hdr.magic != MAGIC or hdr.version != VERSION or hdr.file_size != len(data):
            raise ValueError("not an NG3D v%d file of %d bytes (magic %08x version %d size %d)" % (
                VERSION, len(data), hdr.magic, hdr.version, hdr.file_size))
        p = cls()
        p.sections = []
        by_tag = {}
        for k in range(hdr.n_sections):
            s = Rec.unpack("Section", data, sizeof("Header") + k * sizeof("Section"))
            if s.offset + s.size > len(data):
                raise ValueError("section %s out of the file" % struct.pack("<I", s.tag))
            t4 = struct.pack("<I", s.tag).decode("ascii")
            by_tag[t4] = s
            p.sections.append((t4, s))
        p.strs = bytearray(data[by_tag["STRS"].offset:by_tag["STRS"].offset + by_tag["STRS"].size]) \
            if "STRS" in by_tag else bytearray(b"\0")
        p._str_index = {}
        kinds = dict((s[1], s[2]) for s in SECTIONS)
        for t4, s in by_tag.items():
            if t4 in ("CBAT", "IDAT"):
                setattr(p, SECTION_LISTS[t4], list(struct.unpack_from("<%dH" % s.count, data, s.offset)))
            elif t4 == "ENVR":
                p.env = Rec.unpack("Env", data, s.offset)
            elif t4 == "CVIS":
                p.cvis = bytes(data[s.offset:s.offset + s.size])
            elif t4 in SECTION_LISTS:
                kind = kinds[t4]
                n = sizeof(kind)
                if s.count * n > s.size:
                    raise ValueError("%s: %d records do not fit in %d bytes" % (t4, s.count, s.size))
                setattr(p, SECTION_LISTS[t4], [Rec.unpack(kind, data, s.offset + i * n) for i in range(s.count)])
        p.texture_data = [bytes(data[t.data:t.data + t.size]) for t in p.textures]
        p.vertex_data = [bytes(data[v.data:v.data + v.count * v.stride]) for v in p.vertex_buffers]
        p.blobs = [bytes(data[c.data:c.data + c.size]) for c in p.collisions]
        p._data = data
        return p

    # ---------------------------------------------------------------- checks
    def check(self):
        """Every reference in range, as engine/g3d/package.cpp must find it; returns a list of
        problems (empty if none)."""
        bad = []
        ns = len(self.strs)

        def s_ok(what, off):
            if off >= ns or (off and self.strs[off - 1] != 0):
                bad.append("%s: string offset %d" % (what, off))
        # payloads inside their sections (a package read from a file: another writer may have laid
        # it out; this one's write() puts them there)
        secs = dict(getattr(self, "sections", []))

        def in_sec(what, t4, off, size):
            s = secs.get(t4)
            if secs and (s is None or off < s.offset or off + size > s.offset + s.size):
                bad.append("%s: %d bytes at %d outside %s" % (what, size, off, t4))
        for i, t in enumerate(self.textures):
            s_ok("texture %d" % i, t.name)
            if t.levels < 1 or t.width < 8 or t.height < 8 or t.width & (t.width - 1) or t.height & (t.height - 1):
                bad.append("texture %d: %dx%d %d levels" % (i, t.width, t.height, t.levels))
            if t.data % 128:
                bad.append("texture %d: data not 128-aligned" % i)
            in_sec("texture %d" % i, "TDAT", t.data, t.size)
            # (Texture.size is filled in by write(): before it, the payload's length)
            size = len(self.texture_data[i]) if i < len(self.texture_data) else t.size
            need = texture_bytes(t)
            if need is not None and (size < need if t.flags & TEX_GX_RAW else size != need):
                bad.append("texture %d: %d bytes, its %d levels take %d" % (i, size, t.levels, need))
        nt, nm, nx = len(self.textures), len(self.materials), len(self.tex_mtx)
        for i, m in enumerate(self.materials):
            s_ok("material %d" % i, m.name)
            if not 1 <= m.n_stages <= MAX_TEV_STAGES or m.n_texgens > MAX_TEXGENS:
                bad.append("material %d: %d stages, %d texgens" % (i, m.n_stages, m.n_texgens))
            for k, mp in enumerate(m.maps):
                if mp.texture != NONE16 and mp.texture >= nt:
                    bad.append("material %d map %d: texture %d" % (i, k, mp.texture))
            if m.tex_mtx_first + m.n_tex_mtx > nx:
                bad.append("material %d: tex mtx %d+%d" % (i, m.tex_mtx_first, m.n_tex_mtx))
            for k, g in enumerate(m.texgens[:m.n_texgens]):
                for which in (g.matrix, g.post_mtx):
                    if which != NONE16 and which >= m.n_tex_mtx:
                        bad.append("material %d texgen %d: matrix %d of %d" % (i, k, which, m.n_tex_mtx))
            if m.flags & MAT_LIGHT_SET and m.light_set >= len(self.light_sets):
                bad.append("material %d: light set %d" % (i, m.light_set))
            if m.flags & MAT_FOG and m.fog >= len(self.fogs):
                bad.append("material %d: fog %d" % (i, m.fog))
        ni = len(self.indices)
        for i, v in enumerate(self.vertex_buffers):
            if v.data % 16 or v.count > 65536:
                bad.append("vertex buffer %d: data %d count %d" % (i, v.data, v.count))
            if v.stride < vertex_bytes(v.attrs, v.flags) or v.stride % 4 or \
                    bool(v.attrs & VA_POS) == bool(v.attrs & VA_POS16) or v.attrs & ~63 or v.flags & ~VB_UV_FLOAT:
                bad.append("vertex buffer %d: attributes %02x, flags %02x, stride %d" % (i, v.attrs, v.flags,
                                                                                        v.stride))
            if v.attrs & VA_POS16 and not (v.pos_scale > 0 and all(abs(x) < 1e30 for x in v.pos_origin)):
                bad.append("vertex buffer %d: position origin %s, scale %g" % (i, v.pos_origin, v.pos_scale))
            in_sec("vertex buffer %d" % i, "VDAT", v.data, v.count * v.stride)
        for i, b in enumerate(self.batches):
            if b.flags & ~BATCH_DETAIL:
                bad.append("batch %d: flags %04x" % (i, b.flags))
            if b.material >= nm or b.vbuf >= len(self.vertex_buffers) or b.n_indices % 3 or \
                    b.first_index + b.n_indices > ni:
                bad.append("batch %d: material %d vbuf %d indices %d+%d" % (i, b.material, b.vbuf,
                                                                            b.first_index, b.n_indices))
            else:
                vc = self.vertex_buffers[b.vbuf].count
                hi = max(self.indices[b.first_index:b.first_index + b.n_indices], default=0)
                if hi >= vc:
                    bad.append("batch %d: index %d >= %d vertices" % (i, hi, vc))
        nb = len(self.batches)
        for i, m in enumerate(self.models):
            s_ok("model %d" % i, m.name)
            if m.first_node + m.n_nodes > len(self.nodes) or m.first_batch + m.n_batches > nb:
                bad.append("model %d: nodes %d+%d batches %d+%d" % (i, m.first_node, m.n_nodes, m.first_batch,
                                                                    m.n_batches))
                continue
            for k in range(m.first_node, m.first_node + m.n_nodes):
                n = self.nodes[k]
                if (n.parent != NONE16 and n.parent >= k - m.first_node) or \
                        n.first_batch + n.n_batches > nb:
                    bad.append("model %d node %d: parent %d batches %d+%d" % (i, k, n.parent, n.first_batch,
                                                                             n.n_batches))
            for k in range(m.first_batch, m.first_batch + m.n_batches):
                if self.batches[k].node >= m.n_nodes:
                    bad.append("model %d batch %d: node %d" % (i, k, self.batches[k].node))
            if m.lod_next != NONE16 and m.lod_next >= len(self.models):
                bad.append("model %d: lod_next %d" % (i, m.lod_next))
        nc = len(self.cell_batches)
        for i, c in enumerate(self.cells):
            if not 1 <= c.n_lods <= MAX_CELL_LODS:
                bad.append("cell %d: %d lods" % (i, c.n_lods))
            for l in range(c.n_lods):
                if c.first[l] + c.count[l] > nc:
                    bad.append("cell %d lod %d: %d+%d" % (i, l, c.first[l], c.count[l]))
                if l and c.lod_distance[l] < c.lod_distance[l - 1]:
                    bad.append("cell %d lod %d: distance %g below the level before" % (i, l, c.lod_distance[l]))
        for i, b in enumerate(self.cell_batches):
            if b >= nb:
                bad.append("cell batch %d: %d" % (i, b))
        na = len(self.anims)
        for i, n in enumerate(self.instances):
            if n.model >= len(self.models) or (n.anim != NONE16 and n.anim >= na):
                bad.append("instance %d: model %d anim %d" % (i, n.model, n.anim))
        nk = len(self.keys)
        for i, a in enumerate(self.anims):
            s_ok("anim %d" % i, a.name)
            if a.first_track + a.n_tracks > len(self.tracks) or not a.frames > 0:
                bad.append("anim %d: tracks %d+%d, %g frames" % (i, a.first_track, a.n_tracks, a.frames))
        for i, t in enumerate(self.tracks):
            if t.n_keys < 1 or t.first_key + t.n_keys > nk or t.interp > IP_HERMITE:
                bad.append("track %d: keys %d+%d, interpolation %d" % (i, t.first_key, t.n_keys, t.interp))
                continue
            fr = [k.frame for k in self.keys[t.first_key:t.first_key + t.n_keys]]
            if any(b < a for a, b in zip(fr, fr[1:])):
                bad.append("track %d: key frames out of order" % i)
            if t.target <= TT_NODE_TRANS_Z:
                if t.model >= len(self.models) or t.index >= self.models[t.model].n_nodes:
                    bad.append("track %d: node %d of model %d" % (i, t.index, t.model))
            elif TT_TEXMTX_SCALE_S <= t.target <= TT_TEXMTX_TRANS_T:
                if t.index >= nx:
                    bad.append("track %d: tex mtx %d" % (i, t.index))
            elif TT_MAT_REG_R <= t.target <= TT_MAT_REG_A:
                if t.index >= nm or t.reg > CR_KONST3 or t.reg == 7:
                    bad.append("track %d: material %d register %d" % (i, t.index, t.reg))
            else:
                bad.append("track %d: target %d" % (i, t.target))
        for i, ls in enumerate(self.light_sets):
            for l in ls.lights:
                if l != NONE16 and l >= len(self.lights):
                    bad.append("light set %d: light %d" % (i, l))
        if self.env is not None:
            e = self.env
            for what, v, n in (("sky_model", e.sky_model, len(self.models)),
                               ("sea_model", e.sea_model, len(self.models)), ("sky_anim", e.sky_anim, na), ("sea_anim_srt", e.sea_anim_srt, na),
                               ("sea_anim_clr", e.sea_anim_clr, na), ("world_anim_srt", e.world_anim_srt, na),
                               ("world_anim_clr", e.world_anim_clr, na)):
                if v != NONE16 and v >= n:
                    bad.append("env %s: %d" % (what, v))
            if e.n_cells_x * e.n_cells_z and len(self.cells) not in (0, e.n_cells_x * e.n_cells_z):
                bad.append("env grid %dx%d but %d cells" % (e.n_cells_x, e.n_cells_z, len(self.cells)))
            if e.pvs_row_bytes:
                if e.pvs_row_bytes * 8 < len(self.cells) or len(self.cvis) != e.pvs_row_bytes * len(self.cells):
                    bad.append("CVIS: %d bytes, %d a row, %d cells" % (len(self.cvis), e.pvs_row_bytes,
                                                                       len(self.cells)))
                elif not e.pvs_max_height > 0:
                    bad.append("env pvs_max_height %g" % e.pvs_max_height)
            elif self.cvis:
                bad.append("CVIS: %d bytes but env pvs_row_bytes 0" % len(self.cvis))
            if not 0 <= e.detail_distance < 1e30:
                bad.append("env detail_distance %g" % e.detail_distance)
        for i, m in enumerate(self.markers):
            s_ok("marker %d" % i, m.name)
            if m.kind > MK_WARP:
                bad.append("marker %d: kind %d" % (i, m.kind))
        for i, c in enumerate(self.collisions):
            s_ok("collision %d" % i, c.name)
            in_sec("collision %d" % i, "BLOB", c.data, c.size)
        return bad

    def records(self):
        """Everything the package holds except its layout (file offsets of the payloads): to tell
        whether two packages are the same content."""
        def strip(r, kind):
            v = dict(r._v)
            for f in {"Texture": ("data",), "VertexBuffer": ("data",), "Collision": ("data",)}.get(kind, ()):
                v[f] = None
            return v
        out = {lst: [strip(r, LIST_KIND[lst]) if isinstance(r, Rec) else r for r in getattr(self, lst)]
               for lst in SECTION_LISTS.values()}
        out.update(env=self.env, strs=bytes(self.strs), texture_data=self.texture_data,
                   vertex_data=self.vertex_data, blobs=self.blobs, cvis=bytes(self.cvis))
        return out

    def summary(self):
        """[(tag, bytes, count)] from the sections as written."""
        data = self.write()
        hdr = Rec.unpack("Header", data)
        out = []
        for k in range(hdr.n_sections):
            s = Rec.unpack("Section", data, sizeof("Header") + k * sizeof("Section"))
            out.append((struct.pack("<I", s.tag).decode(), s.size, s.count))
        return out


# ------------------------------------------------------------------------------------- self-test


def parse_header(path=HEADER):
    """The header's structs ({name: [(field, type, dims)]}), static_assert sizes, section tags,
    enums and int constants."""
    src = open(path, encoding="utf-8").read()
    code = re.sub(r"//[^\n]*", "", src)
    consts = {m.group(1): int(m.group(2)) for m in re.finditer(r"constexpr\s+int\s+(\w+)\s*=\s*(\d+)\s*;", code)}
    tags = {m.group(1): "".join(m.group(i) for i in range(2, 6)) for m in re.finditer(
        r"constexpr\s+u32\s+(SEC_\w+)\s*=\s*tag\('(.)',\s*'(.)',\s*'(.)',\s*'(.)'\)", code)}
    enums = {}
    for m in re.finditer(r"enum\s+(\w+)\s*:\s*\w+\s*\{([^}]*)\}", code):
        vals, nxt = {}, 0
        for item in [x.strip() for x in m.group(2).split(",") if x.strip()]:
            if "=" in item:
                k, v = [x.strip() for x in item.split("=", 1)]
                nxt = int(eval(v, {}, dict(vals)))
            else:
                k = item
            vals[k] = nxt
            nxt += 1
        enums[m.group(1)] = vals
    structs = {}
    for m in re.finditer(r"struct\s+(\w+)\s*\{(.*?)\};", code, re.S):
        fields = []
        for decl in m.group(2).split(";"):
            decl = " ".join(decl.split())
            if not decl:
                continue
            typ, rest = decl.split(" ", 1)
            for part in rest.split(","):
                part = part.strip()
                name = re.match(r"(\w+)", part).group(1)
                dims = tuple(int(eval(d, {}, consts)) for d in re.findall(r"\[([^\]]+)\]", part))
                fields.append((name, typ, dims))
        structs[m.group(1)] = fields
    sizes = {m.group(1): int(m.group(2)) for m in re.finditer(
        r"static_assert\s*\(\s*sizeof\s*\(\s*(\w+)\s*\)\s*==\s*(\d+)", code)}
    return structs, sizes, tags, enums, consts


def c_layout(structs, kind):
    """(size, alignment) of a struct with C's natural alignment, and whether it has padding."""
    off, align, padded = 0, 1, False
    for _, t, dims in structs[kind]:
        if t in SCALAR:
            sz = al = SCALAR[t][1]
        else:
            sz, al, p = c_layout(structs, t)
            padded |= p
        if off % al:
            padded = True
            off = _align(off, al)
        off += sz * _prod(dims)
        align = max(align, al)
    if off % align:
        padded = True
    return _align(off, align), align, padded


def selftest(path=HEADER):
    structs, sizes, tags, enums, consts = parse_header(path)
    problems = []
    for kind, fields in structs.items():
        if kind not in STRUCTS:
            problems.append("struct %s is in the header, not in ng3d.py" % kind)
            continue
        if fields != STRUCTS[kind]:
            for a, b in zip(fields + [None] * 99, STRUCTS[kind] + [None] * 99):
                if a != b:
                    problems.append("%s: header has %s, ng3d.py %s" % (kind, a, b))
                    break
        csize, _, padded = c_layout(structs, kind)
        if padded:
            problems.append("%s has padding in C (the packed mirror would disagree)" % kind)
        if sizes.get(kind) != csize or sizeof(kind) != csize:
            problems.append("%s: static_assert %s, C layout %d, ng3d.py %d" % (kind, sizes.get(kind), csize,
                                                                               sizeof(kind)))
    for kind in STRUCTS:
        if kind not in structs:
            problems.append("struct %s is in ng3d.py, not in the header" % kind)
    ours = {name: t4 for name, t4, _ in SECTIONS}
    if tags != ours:
        problems.append("section tags differ: header %s, ng3d.py %s" % (
            sorted(set(tags.items()) ^ set(ours.items())), ""))
    if enums != ENUMS:
        for k in set(enums) | set(ENUMS):
            if enums.get(k) != ENUMS.get(k):
                problems.append("enum %s: header %s, ng3d.py %s" % (k, enums.get(k), ENUMS.get(k)))
    for k, v in CONSTANTS.items():
        if consts.get(k) != v:
            problems.append("constant %s: header %s, ng3d.py %s" % (k, consts.get(k), v))
    return problems, structs, sizes


# ------------------------------------------------------------------------------------- CLI


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    if argv[1] == "selftest":
        problems, structs, sizes = selftest()
        for p in problems:
            print("MISMATCH", p)
        print("%d structs, %d static_asserts, %d sections, %d enums checked against %s: %s" % (
            len(structs), len(sizes), len(SECTIONS), len(ENUMS), os.path.relpath(HEADER, ROOT),
            "OK" if not problems else "%d problems" % len(problems)))
        return 1 if problems else 0
    if argv[1] in ("info", "check"):
        data = open(argv[2], "rb").read()
        p = Package.read(data)
        print("%s: %d bytes, %d sections" % (argv[2], len(data), len(p.sections)))
        for t4, s in p.sections:
            print("  %s at %9d  %9d bytes  %6d records" % (t4, s.offset, s.size, s.count))
        if argv[1] == "check":
            bad = p.check()
            for b in bad[:50]:
                print("BAD", b)
            again = p.write()
            same = again == data
            # a file another writer laid out (e.g. the renderer's ng3d-bake, which pads differently)
            # round-trips to other bytes: then its records must come back the same
            same_records = same or Package.read(again).records() == p.records()
            print("references: %s; round trip (read, write): %s" % (
                "%d problems" % len(bad) if bad else "all in range",
                "identical" if same else "the same records (laid out by another writer)" if same_records
                else "DIFFERENT"))
            return 0 if same_records and not bad else 1
        return 0
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
