#!/usr/bin/env python3
"""Stage Wuhu Island for the native g3d: Wii Sports Resort's island -> DATA/wuhu/island.ng3d.

    tools/wuhu/stage_wuhu.py DATA [--wsr ROOT] [--bake TOOL] [--keep-raw] [--json OUT]

ROOT is your Wii Sports Resort extraction (default ../wii-sports-extract/wii-sports-resort next to
this repository; files/ under it is the disc's root). DATA is the port's data directory (the one
tools/stage_data.py makes); the package goes to DATA/wuhu/island.ng3d, a summary of what went into
it to DATA/wuhu/island.json. Nothing of the disc is copied anywhere else, and nothing it makes
belongs in the repository.

What goes in (docs/wuhu.md has the reasons and the numbers; docs/wuhu_design.md section 0.3):
  - WS2_common_island as Island Flyover (mode 13) shows it, cut into cells of CELL units on x/z by
    triangle centroid, a batch per (cell, material), and the placed objects of
    PlnEnv0.carc/WS2_pln_island_tag0.pmp group 1 (what the Flyover's 1-player mode spawns: exclude
    bit 2 skipped) that stand still, baked into the cells: trees (the full model near, the lighter
    one from 3,000 units), boats at anchor, the fountain, the venues' gates (to 20,000 units), and
    near the eye only parasols, tents, parked cars, kayaks, flags at rest, poles, tees, buoys and
    the venues' small props; WS2_common_island_V too.
  - the moving ones as models placed by instances with their own node animation: windmills,
    balloons, the airship, moving cars, cruisers and the liner on their routes, whales, gulls and
    hawks on circles.
  - WS2_common_sea (all but Water16b; sea2_dummy -> sea2_day) and WS2_common_vr (the sky) as
    models; the island's, sea's, sky's and fountain's texture and colour animations.
  - the day environment (lights and light sets, fogs), k, markers (spawn, landmarks, camera
    presets) and the island's collision (WS2_common_island1..5.kcl, tree trunks as cylinders).
--bake TOOL: after writing, run `TOOL RAW OUT` (the renderer's ng3d-bake, which converts the GX
texels to PICA ones offline): RAW = DATA/wuhu/island.raw.ng3d, OUT = DATA/wuhu/island.ng3d.
"""
import argparse
import json
import math
import os
import struct
import subprocess
import sys
import time
from collections import Counter, OrderedDict

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tools/g3d"))
import brres  # noqa: E402
import convert  # noqa: E402
import ng3d  # noqa: E402
import nintendo_formats as nf  # noqa: E402
from ng3d import NONE16  # noqa: E402

DEFAULT_WSR = os.path.join(os.path.dirname(ROOT), "wii-sports-extract/wii-sports-resort")

# ----------------------------------------------------------------------------------- choices
# Island Flyover's node visibility (main.dol's table at 0x8066A37C, column 13 = Pln day; research
# wsrdol.md section 2): every node of the island and sea models visible but these. Island_22 (a
# villa) is hidden in every mode; Light1-4 are the night lights (island_L, not staged).
HIDDEN_MODE13 = {"Island_16b_Model", "Island_17_Model", "Island_17b_Model", "Island_19_Model",
                 "Island_19c_Model", "Island_20_Model", "Island_20c_Model", "Island_22_Model",
                 "Water16b", "Light1", "Light2", "Light3", "Light4"}

# x/z cells of the static world, units (research gfx.md section 4: 39 non-empty cells, 53-264 draws
# a frame, the largest cell 16k vertices -- within one u16 index space)
CELL = 4000.0
# detail levels of a cell, by the eye's distance to its box: 0 = everything, the trees' full models;
# 1 (from 3,000 units) = the trees' lighter models (WS2_common_tree_M, tree2_M) and the large props
# only -- on the 3DS's 240-line screen with a 45-degree view a 230-unit tree is about 22 pixels tall
# at 3,000 units, where the two tree models look alike, and a 30-unit car or parasol 3 pixels; 2
# (from 20,000) = the terrain alone: a tree is 3 pixels tall there, and only Wedge Island and the
# far islets are that far from anywhere on the main island
LOD_DISTANCES = [0.0, 3000.0, 20000.0]
TAG_ALL, TAG_PROP, TAG_NEAR, TAG_MID = 0, 1, 2, 3          # which levels a piece is in
TAG_LEVELS = {TAG_ALL: (0, 1, 2), TAG_PROP: (0, 1), TAG_NEAR: (0,), TAG_MID: (1,)}

# where the Mii appears (research placement.md section 5): Wuhu Town's plaza south of the coin-toss
# fountain, facing it (-Z); checked on the collision below
SPAWN = (-2029.0, 69.0, 2330.0)
SPAWN_DIR = (0.0, 0.0, -1.0)

# k: Wii no Ma's Mii units per island unit. Both games hang the Mii's head (the same RFL head) from
# a body node: Wii no Ma's Mii body (g3d/Mii/Model.lex Female_Body / Male_Body) has its Head node
# 70 + 38.9 = 108.9 above the feet of a 158-tall Mii (height scale 1); WSR's spectator body
# (Audience/G3D/WS2_common_avaC0.brres) has head_set at its bind height, times the spectators'
# placement scale. The ratio of the two neck heights is k.
NOMA_MII_HEIGHT = 158.0
NOMA_MII_HEAD_Y = 108.9

# research render_island.py's cameras (eye, target, vertical fov): the reference views
VIEWS = OrderedDict([
    ("town", ((-2350.0, 115.0, 2350.0), (-1500.0, 170.0, 1250.0), 55.0)),
    ("coast", ((8300.0, 105.0, 3000.0), (6000.0, 150.0, -1500.0), 55.0)),
    ("beach", ((6300.0, 115.0, -200.0), (8700.0, 40.0, 5200.0), 55.0)),
    ("interior", ((1450.0, 2260.0, -3500.0), (2350.0, 2140.0, -4350.0), 65.0)),
    ("distant", ((-7700.0, 1100.0, -300.0), (-29600.0, 300.0, 16100.0), 40.0)),
    ("overview", ((-9000.0, 3500.0, 12000.0), (0.0, 400.0, -2000.0), 50.0)),
])

# CmnObjManager classes (main.dol 0x80669518) by PMP (group 1) type (0x80669858), and how each
# is staged: "static" baked into the cells for levels 0-1 (TAG_PROP), "small" baked for level 0
# only (TAG_NEAR), "tree" baked with two detail levels, "instance" a model + instance (+ animation),
# "out" left out (the reason in docs/wuhu.md)
PMP_CLASS = {0: 2, 1: 3, 2: 4, 3: 6, 4: 8, 5: 41, 6: 42, 7: 5, 9: 7, 10: 9, 11: 10, 12: 10, 18: 10, 19: 10,
             13: 11, 15: 11, 24: 11, 25: 11, 35: 12, 33: 13, 14: 14, 16: 15, 20: 16, 22: 16, 23: 16, 21: 17,
             26: 18, 27: 19, 28: 19, 29: 20, 30: 21, 31: 22, 32: 23, 34: 24, 36: 25, 37: 26, 39: 27, 40: 29,
             41: 28, 42: 30, 43: 31, 45: 32, 46: 33, 48: 34, 49: 35, 50: 36, 51: 37, 52: 38, 53: 39, 54: 40}
SA = "Stage/Static/StageArc.carc"
CLASSES = {
    2: ("conifer", "tree", "Tree/G3D/WS2_common_tree.brres", ("WS2_common_tree_H", "WS2_common_tree_M")),
    3: ("broadleaf tree", "tree", "Tree/G3D/WS2_common_tree2.brres", ("WS2_common_tree2_H", "WS2_common_tree2_M")),
    4: ("palm", "tree", "Tree/G3D/WS2_common_tree3.brres", ("WS2_common_tree3", None)),
    5: ("seagull", "instance", "Bird/G3D/WS2_common_bird.brres", ("WS2_common_bird",)),
    6: ("airship", "instance", "Airship/G3D/WS2_common_airship.brres", ("WS2_common_airship",)),
    7: ("cruiser (at anchor)", "static", "Cruiser/G3D/WS2_common_cruiser.brres", ("WS2_common_cruiser",)),
    8: ("yacht", "static", "Yacht/G3D/WS2_common_yacht.brres", ("WS2_common_yacht",)),
    9: ("parasol", "small", "Parasol/G3D/WS2_common_parasol.brres", ("WS2_common_parasol",)),
    10: ("flag A", "small", "Flag/G3D/WS2_common_FlagA1.brres", ("WS2_common_FlagA1",)),
    11: ("flag B", "small", "Flag/G3D/WS2_common_FlagB1.brres", ("WS2_common_FlagB1",)),
    12: ("flag C", "small", "Flag/G3D/WS2_common_FlagC1.brres", ("WS2_common_FlagC1",)),
    13: ("golf flag", "small", "Flag/G3D/WS2_common_glf_flag.brres", ("WS2_common_glf_flag",)),
    14: ("tent", "small", "Tent/G3D/WS2_common_tent.brres", ("WS2_common_tent",)),
    15: ("parked car", "small", "Car/G3D/WS2_common_car.brres", ("WS2_common_car",)),
    16: ("balloon", "instance", "Balloon/G3D/WS2_common_balloon.brres", ("WS2_common_balloon",)),
    17: ("hermit crab", "out", None, ()),
    18: ("windmill", "instance", "WindMill/G3D/WS2_common_windmill.brres", ("WS2_common_windmill",)),
    19: ("kayak", "small", "Kayak/G3D/WS2_common_kayak.brres", ("WS2_common_kayak",)),
    20: ("pole", "small", "Pole/G3D/WS2_common_pole.brres", ("WS2_common_pole",)),
    21: ("fountain", "static", "Fountain/G3D/WS2_common_fountain.brres", ("WS2_common_fountain",)),
    22: ("waterfall (a particle effect)", "out", None, ()),
    23: ("hawk", "instance", "Hawk/G3D/WS2_common_hawk.brres", ("WS2_common_hawk",)),
    24: ("golf tee", "small", "GlfTee/G3D/WS2_common_tee.brres", ("WS2_common_tee",)),
    25: ("fireworks (night)", "out", None, ()),
    26: ("moving car", "instance", "Car/G3D/WS2_common_car.brres", ("WS2_common_car",)),
    27: ("campfire (night)", "out", None, ()),
    28: ("torch (night)", "out", None, ()),
    29: ("seaplane", "out", None, ()),
    30: ("shooting star (an effect)", "out", None, ()),
    31: ("sea no-entry buoys", "small", "SeaGuard/G3D/WS2_common_seaguard.brres", ("WS2_common_seaguard",)),
    32: ("whale", "instance", "Whale/G3D/WS2_common_whale.brres", ("WS2_common_whale",)),
    33: ("moving cruiser", "instance", "Cruiser/G3D/WS2_common_cruiser.brres", ("WS2_common_cruiser",)),
    34: ("frisbee-dog plate", "small", "IslandLowObj/G3D/WS2_common_fld_plate.brres", ("WS2_common_fld_plate",)),
    35: ("archery target", "small", "IslandLowObj/G3D/WS2_common_arc_target.brres", ("WS2_common_arc_target",)),
    36: ("jet-ski start gate", "static", "IslandLowObj/G3D/WS2_common_jsk_start.brres", ("WS2_common_jsk_start",)),
    37: ("wakeboard goal gate", "static", "IslandLowObj/G3D/WS2_common_wkb_goal.brres", ("WS2_common_wkb_goal",)),
    38: ("table-tennis table", "small", "IslandLowObj/G3D/WS2_common_png_table.brres", ("WS2_common_png_table",)),
    39: ("cycling gate", "small", "IslandLowObj/G3D/WS2_common_bic_gate.brres", ("WS2_common_bic_gate",)),
    40: ("moving liner", "instance", "Liner/G3D/WS2_common_move_liner.brres", ("WS2_common_move_liner",)),
    41: ("spectator Mii", "out", None, ()),
    42: ("spectator Mii (hands)", "out", None, ()),
}
# the moving ones' speeds, from WSR's update functions where they are known: the liner advances its
# 29-key route by 0.00067 frame a frame (fn_802A5694); a moving cruiser by 20 / the route's length
# (fn_802A4A8C), i.e. 20/29 units a frame along it; the rest play their animation at one frame a
# frame. A gull or hawk circles at 1.9 units a frame, its steering's cap (fn_80281030).
LINER_RATE = 0.00067
CRUISER_SPEED = 20.0 / 29.0
BIRD_SPEED = 1.9
WINDMILL_DESYNC = 37.0         # frames between windmills' phases (they turn together in WSR's data)

# distances beyond which small instances aren't drawn (0 = always): a 20-unit gull is under two
# pixels at 6,000; the big ones (airship, liner, balloons, whales, windmills) are left to the fog
INSTANCE_CULL = {5: 6000.0, 23: 6000.0, 26: 12000.0, 33: 20000.0}


# ----------------------------------------------------------------------------------- sources


class Wsr:
    """The extraction's archives (Yaz0 U8 .carc), unpacked in memory on first use."""

    def __init__(self, root):
        self.root = root
        self.files = os.path.join(root, "files")
        if not os.path.isdir(self.files):
            raise SystemExit("%s: no files/ here (give --wsr the extraction's root)" % root)
        self._arcs = {}
        self._brres = {}

    def arc(self, rel):
        if rel not in self._arcs:
            data = open(os.path.join(self.files, rel), "rb").read()
            data, _ = nf.maybe_decompress(data)
            self._arcs[rel] = {p: data[o:o + s] for p, o, s in nf.u8_list(data) if o is not None}
        return self._arcs[rel]

    def get(self, rel, member):
        return self.arc(rel)[member]

    def brres(self, rel, member):
        key = (rel, member)
        if key not in self._brres:
            self._brres[key] = brres.BRRES(self.get(rel, member), "%s/%s" % (rel, member))
        return self._brres[key]


# ----------------------------------------------------------------------------------- KCL


def kcl_triangles(d):
    """A KCL file's triangles ((n, 3, 3) float64) and attributes (research placement.md section 3:
    prisms of height h on a vertex, a face normal and three edge normals)."""
    o_pos, o_nrm, o_prism, o_oct = struct.unpack_from(">4I", d, 0)
    pos = np.frombuffer(d, ">f4", (o_nrm - o_pos) // 4, o_pos).reshape(-1, 3).astype(np.float64)
    nrm = np.frombuffer(d, ">f4", (o_prism + 0x10 - o_nrm) // 4, o_nrm).reshape(-1, 3).astype(np.float64)
    n = (o_oct - (o_prism + 0x10)) // 0x10
    pr = np.frombuffer(d, np.dtype([("h", ">f4"), ("p", ">u2"), ("f", ">u2"), ("a", ">u2"), ("b", ">u2"),
                                    ("c", ">u2"), ("attr", ">u2")]), n, o_prism + 0x10)
    v1 = pos[pr["p"]]
    fn, na, nb, nc = nrm[pr["f"]], nrm[pr["a"]], nrm[pr["b"]], nrm[pr["c"]]
    h = pr["h"].astype(np.float64)[:, None]
    ca, cb = np.cross(na, fn), np.cross(nb, fn)
    with np.errstate(divide="ignore", invalid="ignore"):
        v2 = v1 + cb * (h / np.einsum("ij,ij->i", cb, nc)[:, None])
        v3 = v1 + ca * (h / np.einsum("ij,ij->i", ca, nc)[:, None])
    return np.stack([v1, v2, v3], 1), pr["attr"].astype(np.int64), fn


def kcl_ground(tris, attrs, normals, x, z, y_from):
    """The highest triangle under (x, z) at or below y_from: (y, attribute, normal) or None."""
    a, b, c = tris[:, 0], tris[:, 1], tris[:, 2]

    def edge(p, q):
        return (q[:, 0] - p[:, 0]) * (z - p[:, 2]) - (q[:, 2] - p[:, 2]) * (x - p[:, 0])
    e0, e1, e2 = edge(a, b), edge(b, c), edge(c, a)
    with np.errstate(invalid="ignore", divide="ignore"):
        inside = ((e0 >= 0) & (e1 >= 0) & (e2 >= 0)) | ((e0 <= 0) & (e1 <= 0) & (e2 <= 0))
        n = np.cross(b - a, c - a)
        y = a[:, 1] - (n[:, 0] * (x - a[:, 0]) + n[:, 2] * (z - a[:, 2])) / n[:, 1]
    ok = inside & np.isfinite(y) & (y <= y_from + 1e-3) & (np.abs(n[:, 1]) > 1e-9)
    if not ok.any():
        return None
    i = np.nonzero(ok)[0][np.argmax(y[ok])]
    return float(y[i]), int(attrs[i]), normals[i]


# ----------------------------------------------------------------------------------- env files


def rgba(b):
    return list(b[:4])


def read_plight(d):
    """LGHT v2 (EGG::LightManager): 16 LightObjects of 0x50 at 0x28, then 16 ambient colours (8
    bytes apart). Per light: type 0x12 (0 world point, 1 world directional, ...), flags 0x16 (bit 0
    on), position 0x18, aim 0x24, intensity 0x30 (multiplied into the RGB), colour 0x34."""
    n, na = struct.unpack_from(">HH", d, 0x10)
    lights = []
    for i in range(n):
        o = 0x28 + 0x50 * i
        typ = d[o + 0x12]
        idx, flags = struct.unpack_from(">HH", d, o + 0x14)
        pos = struct.unpack_from(">3f", d, o + 0x18)
        aim = struct.unpack_from(">3f", d, o + 0x24)
        inten = struct.unpack_from(">f", d, o + 0x30)[0]
        col = d[o + 0x34:o + 0x38]
        eff = [min(255, int(c * inten)) for c in col[:3]] + [col[3]]
        lights.append(dict(index=idx, type=typ, on=bool(flags & 1), pos=pos, aim=aim, color=eff))
    ab = 0x28 + 0x50 * n
    amb = [rgba(d[ab + 8 * i:ab + 8 * i + 4]) for i in range(na)]
    return lights, amb


def read_fogd(d, o=0):
    """FOGD (EGG::Fog): start 0x10, end 0x14, RGBA 0x20, GX fog type 0x24, flags 0x25 (bit 0 on)."""
    st, en = struct.unpack_from(">2f", d, o + 0x10)
    return dict(start=st, end=en, color=rgba(d[o + 0x20:o + 0x24]), type=d[o + 0x24], on=bool(d[o + 0x25] & 1))


def read_landmarks(d):
    """LandMarkData.exbin: 118 records of 0xBC from 0x10: radius, kind (0 fixed, 3 riding a vehicle:
    position relative to it, 6 unused), message id, Shift-JIS name at 0x10, position at 0x9C."""
    out = []
    for i in range((len(d) - 0x10) // 0xBC):
        o = 0x10 + i * 0xBC
        kind = struct.unpack_from(">I", d, o + 4)[0]
        name = d[o + 0x10:o + 0x90].split(b"\0")[0].decode("shift_jis", "replace")
        out.append(dict(index=i, kind=kind, name=name, pos=struct.unpack_from(">3f", d, o + 0x9C)))
    return out


def read_campar(d):
    """WS2_seq1_IslandCamParam.exbin: 15 x {eye xyz, target xyz} at 0x10."""
    return [(struct.unpack_from(">3f", d, 0x10 + 24 * i), struct.unpack_from(">3f", d, 0x1C + 24 * i))
            for i in range(15)]


def read_pmp(d):
    """PMPF objects: (group, type, position, scale, Mtx34 as WSR builds it [X*sx | Y*sy | Z*sz | pos],
    include bit, exclude bit, sub id). Research placement.md section 2.1."""
    n = struct.unpack_from(">H", d, 0x10)[0]
    out = []
    for i in range(n):
        b = 0x80 + 0x58 * i
        g, t = struct.unpack_from(">HH", d, b)
        pos = struct.unpack_from(">3f", d, b + 8)
        sc = struct.unpack_from(">3f", d, b + 0x14)
        zx, yx, xx = (struct.unpack_from(">3f", d, b + o) for o in (0x20, 0x2C, 0x38))
        m = np.eye(4)
        for r in range(3):
            m[r, :] = [xx[r] * sc[0], yx[r] * sc[1], zx[r] * sc[2], pos[r]]
        inc, exc, sub = struct.unpack_from(">hhh", d, b + 0x48)
        out.append(dict(index=i, group=g, type=t, filter=d[b + 4], pos=np.array(pos), scale=sc, mtx=m,
                        axes=(np.array(xx), np.array(yx), np.array(zx)), include=inc, exclude=exc, sub=sub))
    return out


def spawned(objs, exclude_mask):
    """What CmnObjManager makes of a PMP's group-1 records (fn_8026D230 / fn_8026D514): filter 0,
    a known type, include bit 0 (the Flyover passes include mask 0), exclude bit not in the mask."""
    out, dropped = [], Counter()
    for o in objs:
        if o["group"] != 1 or o["filter"] != 0 or o["type"] not in PMP_CLASS:
            continue
        cls = PMP_CLASS[o["type"]]
        if o["include"] > 0:
            dropped[cls] += 1
            continue
        if o["exclude"] > 0 and exclude_mask & (1 << (o["exclude"] - 1)):
            dropped[cls] += 1
            continue
        out.append(dict(o, cls=cls))
    return out, dropped


# ----------------------------------------------------------------------------------- staging


class Stager:
    def __init__(self, wsr):
        self.wsr = wsr
        self.pkg = ng3d.Package()
        self.cv = convert.Converter(self.pkg)
        self.cells = convert.MeshSet(self.cv)
        self.inv = OrderedDict()           # the inventory written next to the package
        self.cell_groups = {}              # cell -> Counter(node name -> triangles)
        self.mat_by_name = {}              # (model name, material name) -> package material
        self.log = []

    def say(self, *a):
        s = " ".join(str(x) for x in a)
        self.log.append(s)
        print(s, flush=True)

    def cell_of(self, x, z):
        return (int(math.floor(x / CELL)), int(math.floor(z / CELL)))

    # -- materials by name, for animations
    def material(self, r, mdl, mat, tex_override=None, prio=0):
        mi = self.cv.material(r, mdl, mat, tex_override, prio)
        self.mat_by_name[(mdl.name, mat.name)] = mi
        return mi

    def texmtx_of(self, model):
        def f(mname, j):
            mi = self.mat_by_name.get((model, mname))
            if mi is None:
                return None
            m = self.pkg.materials[mi]
            return m.tex_mtx_first + j if j < m.n_tex_mtx else None
        return f

    def material_of(self, model):
        return lambda mname: self.mat_by_name.get((model, mname))

    # -- static geometry into the cells
    def bake_model(self, r, mdl, M, tag, hidden=(), tex_override=None, by_centroid=False, cell=None,
                   group_name=None):
        """Every visible draw of `mdl`, moved by M (4x4), into the cells: by triangle centroid
        (terrain) or all into `cell`."""
        mats = {m.index: m for m in mdl.materials}
        shapes = {s.index: s for s in mdl.shapes}
        tris = 0
        for d in mdl.draws("DrawOpa") + mdl.draws("DrawXlu"):
            vis = mdl.nodes[d.node].name if 0 <= d.node < len(mdl.nodes) else None
            if vis in hidden:
                continue
            mi = self.material(r, mdl, mats[d.mat], tex_override, d.prio)
            t = mdl.shape_triangles(shapes[d.shape], world=True)
            if not t or "pos" not in t:
                continue
            c = convert.corners_from(t, mdl, shapes[d.shape])
            if M is not None:
                c = convert.transform_corners(c, M)
            n = len(c["pos"]) // 3
            tris += n
            if by_centroid:
                cen = c["pos"].reshape(n, 3, 3).mean(1)
                cx = np.floor(cen[:, 0] / CELL).astype(np.int64)
                cz = np.floor(cen[:, 2] / CELL).astype(np.int64)
                for key in set(zip(cx.tolist(), cz.tolist())):
                    sel = np.repeat((cx == key[0]) & (cz == key[1]), 3)
                    self.cells.add(key, mi, {k: (v[sel] if k != "uvfrac" else v) for k, v in c.items()}, tag)
                    if group_name:
                        self.cell_groups.setdefault(key, Counter())[vis or group_name] += int(sel.sum() // 3)
            else:
                self.cells.add(cell, mi, c, tag)
        return tris

    # ------------------------------------------------------------------------------- island
    def stage_island(self):
        w = self.wsr
        self.island = isl = w.brres(SA, "Island/G3D/WS2_common_island.brres")
        self.seatex = sea = w.brres(SA, "Island/G3D/WS2_common_seatex.brres")
        self.sea_override = {"sea2_dummy": (sea, "sea2_day")}
        n = self.bake_model(isl, isl.models["WS2_common_island"], None, TAG_ALL, HIDDEN_MODE13,
                            by_centroid=True, group_name="island")
        nv = self.bake_model(isl, isl.models["WS2_common_island_V"], None, TAG_ALL, HIDDEN_MODE13,
                             by_centroid=True, group_name="island_V")
        self.say("island: %d triangles (mode 13), island_V %d" % (n, nv))
        self.inv["island"] = dict(source="%s Island/G3D/WS2_common_island.brres" % SA, triangles=n,
                                  island_V_triangles=nv, hidden_nodes=sorted(HIDDEN_MODE13))
        # the sea and the sky: models of their own
        self.sea_model, _ = self.cv.model(isl, isl.models["WS2_common_sea"], hidden=HIDDEN_MODE13,
                                          tex_override=self.sea_override)
        for m in isl.models["WS2_common_sea"].materials:
            k = (id(isl), "WS2_common_sea", m.index, tuple(sorted(
                (a, id(b[0]), b[1]) for a, b in self.sea_override.items())))
            if k in self.cv.mat:
                self.mat_by_name[("WS2_common_sea", m.name)] = self.cv.mat[k]
        self.sky_model, _ = self.cv.model(isl, isl.models["WS2_common_vr"])
        for m in isl.models["WS2_common_vr"].materials:
            k = (id(isl), "WS2_common_vr", m.index, ())
            if k in self.cv.mat:
                self.mat_by_name[("WS2_common_vr", m.name)] = self.cv.mat[k]

    # ------------------------------------------------------------------------------- props
    def stage_props(self):
        w = self.wsr
        tag0 = read_pmp(w.get("Stage/PlnScene/PlnEnv0.carc", "WS2_pln_island_tag0.pmp"))
        objs, dropped = spawned(tag0, exclude_mask=2)
        self.objs = objs
        by_class = Counter(o["cls"] for o in objs)
        self.say("tag0: %d records, %d group-1 objects spawned (exclude mask 2 drops %s)" % (
            len(tag0), len(objs), dict(dropped)))
        counts = OrderedDict()
        handled = Counter()
        for cls in sorted(by_class):
            name, how, path, models = CLASSES[cls]
            counts[cls] = dict(name=name, placed=by_class[cls], handling=how, brres=path, models=list(models))
        for cls, n in sorted(dropped.items()):
            counts.setdefault(cls, dict(name=CLASSES[cls][0], placed=0, handling="out", brres=CLASSES[cls][2],
                                        models=list(CLASSES[cls][3])))
            counts[cls]["dropped_by_exclude_bit"] = n
        # static props and trees
        for o in objs:
            name, how, path, models = CLASSES[o["cls"]]
            if how not in ("static", "small", "tree"):
                continue
            r = w.brres(SA, path)
            cell = self.cell_of(o["pos"][0], o["pos"][2])
            if how == "tree":
                near, mid = models
                self.bake_model(r, r.models[near], o["mtx"], TAG_NEAR if mid else TAG_PROP, cell=cell)
                if mid:
                    self.bake_model(r, r.models[mid], o["mtx"], TAG_MID, cell=cell)
            else:
                self.bake_model(r, r.models[models[0]], o["mtx"], TAG_PROP if how == "static" else TAG_NEAR,
                                cell=cell)
            handled[o["cls"]] += 1
        self.inv["props"] = counts
        self.say("baked: %s" % ", ".join("%s %d" % (CLASSES[c][0], n) for c, n in sorted(handled.items())))

    # ------------------------------------------------------------------------------- moving props
    def stage_instances(self):
        w, pkg = self.wsr, self.pkg
        objs = [o for o in self.objs if CLASSES[o["cls"]][1] == "instance"]
        made = Counter()
        models = {}

        def model_for(cls, extra_root=None):
            if (cls, extra_root) not in models:
                path, mname = CLASSES[cls][2], CLASSES[cls][3][0]
                r = w.brres(SA, path)
                mi, nm = self.cv.model(r, r.models[mname], name="%s#%d" % (mname, cls) if extra_root else mname,
                                       extra_root=extra_root)
                models[(cls, extra_root)] = (r, r.models[mname], mi, nm)
            return models[(cls, extra_root)]

        def place(cls, model, anim, mtx, offset, flags=ng3d.INST_ANIMATED):
            pkg.add("instances", model=model, flags=flags, anim=anim, class_id=cls,
                    mtx=[list(map(float, row)) for row in np.asarray(mtx)[:3]], anim_offset=float(offset),
                    cull_distance=INSTANCE_CULL.get(cls, 0.0))
            made[cls] += 1

        def chr0(r, name):
            return r.anims["CHR0"][name]

        ident = np.eye(4)
        notes = OrderedDict()
        # windmills: the blades turn about the hub (CHR0 WS2_common_windmill, 299 frames), on the
        # placed matrix; they turn in step in WSR, staggered here
        ws = [o for o in objs if o["cls"] == 18]
        if ws:
            r, mdl, mi, nm = model_for(18)
            ab = convert.AnimBuilder(pkg, "WS2_common_windmill", chr0(r, "WS2_common_windmill").frames)
            convert.add_chr0(ab, chr0(r, "WS2_common_windmill"), mi, nm, mdl)
            ai = ab.end()
            for k, o in enumerate(ws):
                place(18, mi, ai, o["mtx"], (k * WINDMILL_DESYNC) % 299)
            notes[18] = "CHR0 WS2_common_windmill (the flier node), on the placed matrix"
        # balloons: WS2_common_balloon_flight (7,999 frames, a ~2,000-unit loop) relative to the
        # placed matrix; all three variants use it here (the others are basketball / canoe scenes')
        bs = [o for o in objs if o["cls"] == 16]
        if bs:
            r, mdl, mi, nm = model_for(16)
            a = chr0(r, "WS2_common_balloon_flight")
            ab = convert.AnimBuilder(pkg, a.name, a.frames)
            convert.add_chr0(ab, a, mi, nm, mdl)
            ai = ab.end()
            for k, o in enumerate(bs):
                place(16, mi, ai, o["mtx"], k * a.frames / len(bs))
            notes[16] = "CHR0 WS2_common_balloon_flight relative to the placed matrix, phases spread"
        # the airship: the title sequence's flight (a world path 2,400-2,800 units up over the main
        # island). Its default 'flight' is a 2,500-unit circle at sea level round the origin that
        # the Flyover must place by code not traced here.
        for o in [o for o in objs if o["cls"] == 6]:
            r, mdl, mi, nm = model_for(6)
            a = chr0(r, "WS2_common_airship_flight_title")
            ab = convert.AnimBuilder(pkg, a.name, a.frames)
            convert.add_chr0(ab, a, mi, nm, mdl)
            place(6, mi, ab.end(), ident, 0)
            notes[6] = "CHR0 WS2_common_airship_flight_title (world path)"
        # moving cars: WS2_common_car_drive is one world route along the roads (12,000 frames);
        # each car starts where the route passes nearest its placement
        cs = [o for o in objs if o["cls"] == 26]
        if cs:
            r, mdl, mi, nm = model_for(26)
            a = chr0(r, "WS2_common_car_drive")
            ab = convert.AnimBuilder(pkg, a.name, a.frames)
            convert.add_chr0(ab, a, mi, nm, mdl)
            ai = ab.end()
            path = sample_path(a, "WS2_common_car", 1.0)
            for o in cs:
                place(26, mi, ai, ident, nearest_frame(path, o["pos"]))
            notes[26] = "CHR0 WS2_common_car_drive (world route), each starting nearest its placement"
        # moving cruisers: eight routes (WS2_common_cruiser_revolve1..8, 29 keys each, world
        # coordinates); each cruiser takes the route passing nearest it, at CRUISER_SPEED, with its
        # placed scale
        ms = [o for o in objs if o["cls"] == 33]
        if ms:
            r, mdl, mi, nm = model_for(33)
            routes = {}
            for k in range(1, 9):
                a = chr0(r, "WS2_common_cruiser_revolve%d" % k)
                p1 = sample_path(a, a.targets[0], 1.0)
                length = path_length(p1)
                routes[k] = (a, length, p1)
            per_route = {}
            for o in ms:
                best = min(routes, key=lambda k: np.min(np.linalg.norm(routes[k][2][:, 1:4] - o["pos"], axis=1)))
                per_route.setdefault(best, []).append(o)
            for k, lst in sorted(per_route.items()):
                a, length, p1 = routes[k]
                ts = (length / CRUISER_SPEED) / a.frames          # game frames per anim frame
                ab = convert.AnimBuilder(pkg, "%s@%.0f" % (a.name, ts * a.frames), a.frames * ts)
                sc = lst[0]["scale"][0]
                convert.add_chr0(ab, a, mi, nm, mdl, rename={a.targets[0]: mdl.nodes[0].name}, time_scale=ts,
                                 override={(mdl.nodes[0].name, ng3d.TT_NODE_SCALE_X + i): sc for i in range(3)})
                ai = ab.end()
                for o in lst:
                    place(33, mi, ai, ident, nearest_frame(p1, o["pos"]) * ts)
            notes[33] = "CHR0 WS2_common_cruiser_revolve1..8 (world routes) at %.2f units a frame" % CRUISER_SPEED
        # the liner: WS2_common_move_liner_revolve1 at LINER_RATE
        for o in [o for o in objs if o["cls"] == 40]:
            r, mdl, mi, nm = model_for(40)
            a = chr0(r, "WS2_common_move_liner_revolve1")
            ts = 1.0 / LINER_RATE
            ab = convert.AnimBuilder(pkg, "%s@%.0f" % (a.name, a.frames * ts), a.frames * ts)
            convert.add_chr0(ab, a, mi, nm, mdl, time_scale=ts)
            place(40, mi, ab.end(), ident, 0)
            notes[40] = "CHR0 WS2_common_move_liner_revolve1 (world route) at %g frame a frame" % LINER_RATE
        # whales: surfacing, blowing and diving where they are placed (CHR0 _surface_gush_dive, 1,319
        # frames); WSR also moves them along its revolve routes (not staged)
        wh = [o for o in objs if o["cls"] == 32]
        if wh:
            r, mdl, mi, nm = model_for(32)
            a = chr0(r, "WS2_common_whale_surface_gush_dive")
            ab = convert.AnimBuilder(pkg, a.name, a.frames)
            convert.add_chr0(ab, a, mi, nm, mdl)
            ai = ab.end()
            for k, o in enumerate(wh):
                place(32, mi, ai, o["mtx"], (k * 0.37 % 1.0) * a.frames)
            notes[32] = "CHR0 WS2_common_whale_surface_gush_dive in place"
        # gulls and hawks: a circle of the placed scale's radius (WSR's PMP scale is the radius) at
        # BIRD_SPEED, flapping (fly_a); an 'orbit' node above the model's root carries the circle
        for cls, flap in ((5, "WS2_common_bird_fly_a"), (23, "WS2_common_hawk_fly_a")):
            lst = [o for o in objs if o["cls"] == cls]
            if not lst:
                continue
            r, mdl, mi, nm = model_for(cls, extra_root="orbit")
            a = chr0(r, flap)
            by_radius = {}
            for o in lst:
                by_radius.setdefault(round(o["scale"][0], 1), []).append(o)
            for radius, group in sorted(by_radius.items()):
                laps = max(1, int(round(2 * math.pi * radius / BIRD_SPEED / a.frames)))
                period = laps * a.frames
                ab = convert.AnimBuilder(pkg, "%s@r%.0f" % (flap, radius), period)
                convert.add_chr0(ab, a, mi, nm, mdl, period=a.frames)
                add_orbit(ab, mi, nm["orbit"], radius, period)
                ai = ab.end()
                for k, o in enumerate(group):
                    M = np.eye(4)
                    M[:3, 3] = o["pos"]
                    place(cls, mi, ai, M, (k * 0.618 % 1.0) * period)
            notes[cls] = "%s flapping on a circle of the placed scale's radius at %.1f units a frame" % (
                flap, BIRD_SPEED)
        self.say("instances: %s" % ", ".join("%s %d" % (CLASSES[c][0], n) for c, n in sorted(made.items())))
        for cls, n in made.items():
            self.inv["props"][cls]["instances"] = n
            self.inv["props"][cls]["animation"] = notes.get(cls)

    # ------------------------------------------------------------------------------- the cells
    def build_cells(self):
        pkg = self.pkg
        built = self.cells.build()
        keys = list(built)
        xs = [k[0] for k in keys]
        zs = [k[1] for k in keys]
        x0, x1, z0, z1 = min(xs), max(xs), min(zs), max(zs)
        nx, nz = x1 - x0 + 1, z1 - z0 + 1
        world_lo, world_hi = np.full(3, np.inf), np.full(3, -np.inf)
        stats = Counter()
        for iz in range(z0, z1 + 1):
            for ix in range(x0, x1 + 1):
                lst = built.get((ix, iz), [])
                lo, hi = np.full(3, np.inf), np.full(3, -np.inf)
                for b, _, _ in lst:
                    bb = pkg.batches[b].bbox
                    lo, hi = np.minimum(lo, bb[:3]), np.maximum(hi, bb[3:])
                if not lst:
                    lo = np.array([ix * CELL, 0.0, iz * CELL])
                    hi = np.array([(ix + 1) * CELL, 0.0, (iz + 1) * CELL])
                else:
                    world_lo, world_hi = np.minimum(world_lo, lo), np.maximum(world_hi, hi)

                def level(l):
                    sel = [(b, m) for b, m, t in lst if l in TAG_LEVELS[t]]
                    sel.sort(key=lambda bm: (pkg.materials[bm[1]].flags & ng3d.MAT_XLU,
                                             pkg.materials[bm[1]].priority, bm[0]))
                    return [b for b, _ in sel]
                levels = [level(l) for l in range(3)]
                n_lods = 3 if any(t != TAG_ALL for _, _, t in lst) else 1
                first, count = [0, 0, 0], [0, 0, 0]
                for l in range(n_lods):
                    first[l] = len(pkg.cell_batches)
                    count[l] = len(levels[l])
                    pkg.cell_batches.extend(levels[l])
                    stats["batches level %d" % l] += len(levels[l])
                grp = self.cell_groups.get((ix, iz))
                group = grp.most_common(1)[0][0] if grp else ""
                pkg.add("cells", bbox=[float(v) for v in list(lo) + list(hi)], first=first, count=count,
                        n_lods=n_lods, lod_distance=LOD_DISTANCES[:n_lods] + [0.0] * (3 - n_lods),
                        group=pkg.str(group))
                stats["cells"] += 1
                stats["cells with geometry"] += bool(lst)
        self.grid = (x0 * CELL, z0 * CELL, nx, nz)
        self.world_bbox = [float(v) for v in list(world_lo) + list(world_hi)]
        self.say("cells: %d x %d of %.0f units from (%.0f, %.0f), %d with geometry; batches by level %s" % (
            nx, nz, CELL, x0 * CELL, z0 * CELL, stats["cells with geometry"],
            [stats["batches level %d" % l] for l in range(3)]))
        self.inv["cells"] = dict(cell=CELL, grid=[nx, nz], origin=[x0 * CELL, z0 * CELL],
                                 with_geometry=stats["cells with geometry"], lod_distances=LOD_DISTANCES,
                                 batches_per_level=[stats["batches level %d" % l] for l in range(3)])

    # ------------------------------------------------------------------------------- animations
    def stage_anims(self):
        pkg, isl = self.pkg, self.island
        fountain = self.wsr.brres(SA, CLASSES[21][2])
        out = OrderedDict()
        # world: lava (island SRT0, 2,400 frames) and the fountain's water (300, repeated 8 times);
        # colours: lava and the volcano's inside (island CLR0, 200) and island_V's blend (200)
        ab = convert.AnimBuilder(pkg, "world_srt", 2400)
        n = convert.add_srt0(ab, isl.anims["SRT0"]["WS2_common_island"], self.texmtx_of("WS2_common_island"))
        n += convert.add_srt0(ab, fountain.anims["SRT0"]["WS2_common_fountain"], self.texmtx_of("WS2_common_fountain"),
                              period=300)
        self.world_srt = ab.end()
        out["world_srt"] = dict(frames=2400, tracks=n, sources=["SRT0 WS2_common_island", "SRT0 WS2_common_fountain"])
        ab = convert.AnimBuilder(pkg, "world_clr", 200)
        n = convert.add_clr0(ab, isl.anims["CLR0"]["WS2_common_island"], self.material_of("WS2_common_island"))
        n += convert.add_clr0(ab, isl.anims["CLR0"]["WS2_common_island_V"], self.material_of("WS2_common_island_V"))
        self.world_clr = ab.end()
        out["world_clr"] = dict(frames=200, tracks=n, sources=["CLR0 WS2_common_island", "CLR0 WS2_common_island_V"])
        # the sea: the Flyover's set (main.dol 0x806BE698: sea_P with sea_nami); 20,000 frames
        ab = convert.AnimBuilder(pkg, "sea_srt", 20000)
        n = convert.add_srt0(ab, isl.anims["SRT0"]["WS2_common_sea_P"], self.texmtx_of("WS2_common_sea"))
        n += convert.add_srt0(ab, isl.anims["SRT0"]["WS2_common_sea_nami"], self.texmtx_of("WS2_common_sea"),
                              period=1000)
        self.sea_srt = ab.end()
        out["sea_srt"] = dict(frames=20000, tracks=n, sources=["SRT0 WS2_common_sea_P", "SRT0 WS2_common_sea_nami"])
        ab = convert.AnimBuilder(pkg, "sea_clr", 1000)
        n = convert.add_clr0(ab, isl.anims["CLR0"]["WS2_common_sea_nami"], self.material_of("WS2_common_sea"))
        self.sea_clr = ab.end()
        out["sea_clr"] = dict(frames=1000, tracks=n, sources=["CLR0 WS2_common_sea_nami"])
        a = isl.anims["SRT0"]["WS2_common_vr"]
        ab = convert.AnimBuilder(pkg, "sky_srt", a.frames)
        n = convert.add_srt0(ab, a, self.texmtx_of("WS2_common_vr"))
        self.sky_srt = ab.end()
        out["sky_srt"] = dict(frames=a.frames, tracks=n, sources=["SRT0 WS2_common_vr"])
        self.inv["anims"] = out
        self.say("animations: %d, %d tracks, %d keys" % (len(pkg.anims), len(pkg.tracks), len(pkg.keys)))

    # ------------------------------------------------------------------------------- environment
    def stage_env(self):
        w, pkg = self.wsr, self.pkg
        plight = w.get("Stage/PlnScene/PlnEnv0.carc", "WS2_pln_F0/WS2_pln_F0_Light.plight")
        lights, ambient = read_plight(plight)
        scn = w.brres("Common/Static/common.carc", "G3D/WS2_Scene.brres")
        scene = next(iter(scn.scenes.values()))
        by_id = {l["index"]: l for l in lights}
        light_index = {}
        env_lights = []
        for ls in sorted(scene.light_sets, key=lambda s: s["id"]):
            for nm in ls["lights"]:
                lid = scene.lights[nm]
                l = by_id.get(lid)
                if l is None or not l["on"] or lid in light_index:
                    continue
                # EGG type 0: a light at a position in the world (the sun 48,000-100,000 units off,
                # unattenuated); type 1: directional, from position toward the aim point. Both
                # carry the aim direction (what a directional-only renderer uses)
                d = np.array(l["aim"], np.float64) - np.array(l["pos"], np.float64)
                d /= max(np.linalg.norm(d), 1e-9)
                light_index[lid] = pkg.add("lights", name=pkg.str(nm),
                                           kind=ng3d.LK_POINT if l["type"] == 0 else ng3d.LK_DIRECTIONAL,
                                           rgba=l["color"], position=list(map(float, l["pos"])),
                                           direction=[float(x) for x in d])
                env_lights.append(dict(name=nm, id=lid, type=l["type"], pos=l["pos"], color=l["color"]))
        sets = []
        for ls in sorted(scene.light_sets, key=lambda s: s["id"]):
            while len(pkg.light_sets) < ls["id"]:
                pkg.add("light_sets", lights=[NONE16] * 8)
            idx = [light_index[scene.lights[nm]] for nm in ls["lights"] if scene.lights[nm] in light_index]
            amb = ambient[scene.ambients[ls["ambient"]]] if ls["ambient"] in scene.ambients else [0, 0, 0, 255]
            pkg.add("light_sets", name=pkg.str(ls["name"]), lights=(idx + [NONE16] * 8)[:8], ambient=amb)
            sets.append(dict(id=ls["id"], name=ls["name"], lights=ls["lights"], ambient=amb))
        fog0 = read_fogd(w.get(SA, "Binary/WS2_common_binary/ws2_common_day.pfobj"))
        fogm = w.get("Stage/PlnScene/PlnEnv0.carc", "WS2_pln_F0/WS2_pln_F0_fog.pfog")
        fog1 = read_fogd(fogm, 0x14 + 0x30 * 1)
        for f in (fog0, fog1):
            pkg.add("fogs", type=ng3d.FOG_LINEAR if f["on"] and f["type"] == 2 else ng3d.FOG_NONE, rgba=f["color"],
                    start=f["start"], end=f["end"])
        # materials asking for a fog the day set doesn't have (an index past 1) get none
        for m in pkg.materials:
            if m.flags & ng3d.MAT_FOG and m.fog >= len(pkg.fogs):
                m.flags &= ~ng3d.MAT_FOG
                self.cv.stats["materials with a fog index past the table (fog off)"] += 1
        # k from the spectators (see NOMA_MII_HEAD_Y)
        ava = w.brres(SA, "Audience/G3D/WS2_common_avaC0.brres").models["WS2_common_avaC0"]
        head = [n for n in ava.nodes if n.name == "head_set"][0]
        ava_scale = float(np.median([o["scale"][1] for o in self.all_tag0 if o["group"] == 1 and o["type"] == 5]))
        neck = float(head.mtx[1, 3]) * ava_scale
        k = NOMA_MII_HEAD_Y / neck
        self.k = k
        pkg.env = ng3d.Rec("Env", sky_model=self.sky_model, sky_anim=self.sky_srt, sea_model=self.sea_model,
                           sea_anim_srt=self.sea_srt, sea_anim_clr=self.sea_clr, world_anim_srt=self.world_srt,
                           world_anim_clr=self.world_clr, world_bbox=self.world_bbox, mii_units_per_unit=k,
                           cell_size=CELL, n_cells_x=self.grid[2], n_cells_z=self.grid[3],
                           grid_origin=[self.grid[0], self.grid[1]])
        self.say("env: %d lights, %d light sets, fogs %s; k = %.3f (Wii no Ma head node %.1f / WSR spectator "
                 "head_set %.2f x scale %.2f = %.2f; a %.0f-tall Mii is %.1f island units)" % (
                     len(pkg.lights), len(pkg.light_sets), [(f["start"], f["end"]) for f in (fog0, fog1)], k,
                     NOMA_MII_HEAD_Y, head.mtx[1, 3], ava_scale, neck, NOMA_MII_HEIGHT, NOMA_MII_HEIGHT / k))
        self.inv["env"] = dict(lights=env_lights, light_sets=sets, fogs=[fog0, fog1],
                               k=dict(value=k, noma_head_y=NOMA_MII_HEAD_Y, noma_height=NOMA_MII_HEIGHT,
                                      wsr_head_set_y=float(head.mtx[1, 3]), wsr_spectator_scale=ava_scale,
                                      mii_height_island_units=NOMA_MII_HEIGHT / k),
                               world_bbox=self.world_bbox)

    # ------------------------------------------------------------------------------- markers
    def stage_markers(self, kcl):
        pkg, w = self.pkg, self.wsr
        tris, attrs, normals = kcl
        g = kcl_ground(tris, attrs, normals, SPAWN[0], SPAWN[2], SPAWN[1] + 60)
        # flat ground around it: the ground on a 9 x 9 grid 200 units across
        ys = []
        for dx in np.linspace(-100, 100, 9):
            for dz in np.linspace(-100, 100, 9):
                h = kcl_ground(tris, attrs, normals, SPAWN[0] + dx, SPAWN[2] + dz, SPAWN[1] + 60)
                ys.append((h[0], h[1], float(h[2][1])) if h else (None, None, None))
        hs = [y for y, _, _ in ys if y is not None]
        spawn_check = dict(ground_y=g[0], attribute="%04x" % g[1], normal_y=float(g[2][1]),
                           grid_200=dict(points=len(ys), on_ground=len(hs), y_min=min(hs), y_max=max(hs),
                                         attributes=sorted({"%04x" % a for _, a, _ in ys if a is not None}),
                                         min_normal_y=min(n for _, _, n in ys if n is not None)))
        self.say("spawn %s: ground %.2f attr %s normal.y %.4f; 200x200 around: y %.2f..%.2f, attrs %s" % (
            SPAWN, g[0], spawn_check["attribute"], g[2][1], min(hs), max(hs), spawn_check["grid_200"]["attributes"]))
        pkg.add("markers", name=pkg.str("spawn"), kind=0, position=[SPAWN[0], g[0], SPAWN[2]], dir=list(SPAWN_DIR))
        lms = read_landmarks(w.get("Stage/Shared/Activity.carc", "LandMarkData/LandMarkData.exbin"))
        used = [l for l in lms if l["kind"] != 6]
        fixed = [l for l in used if l["kind"] == 0]
        for l in fixed:
            name = l["name"].split("_", 1)[1] if "_" in l["name"] else l["name"]
            pkg.add("markers", name=pkg.str(name or "landmark_%d" % l["index"]), kind=1,
                    position=list(map(float, l["pos"])))
        for i, (eye, tgt) in enumerate(read_campar(w.get("Stage/Shared/SeqData.carc",
                                                         "Map/WS2_seq1_IslandCamParam.exbin"))):
            pkg.add("markers", name=pkg.str("seqcam_%d" % i), kind=2, position=list(map(float, eye)),
                    dir=list(map(float, tgt)))
        for name, (eye, tgt, fov) in VIEWS.items():
            pkg.add("markers", name=pkg.str("view_" + name), kind=2, position=list(eye), dir=list(tgt))
        self.say("markers: spawn, %d landmarks (of %d used: %d ride vehicles, positions relative to them), "
                 "15 seqcam, %d view" % (len(fixed), len(used), len(used) - len(fixed), len(VIEWS)))
        self.inv["markers"] = dict(spawn=dict(position=[SPAWN[0], g[0], SPAWN[2]], dir=SPAWN_DIR, check=spawn_check),
                                   landmarks=len(fixed),
                                   landmarks_on_vehicles=[l["name"] for l in used if l["kind"] == 3],
                                   seqcams=15, views=list(VIEWS))

    # ------------------------------------------------------------------------------- collision
    def stage_collision(self):
        pkg, w = self.pkg, self.wsr
        all_t, all_a, all_n = [], [], []
        files = []
        for i in range(1, 6):
            name = "WS2_common_island%d" % i
            d = w.get(SA, "Island/%s.kcl" % name)
            pkg.add("collisions", name=pkg.str(name))
            pkg.blobs.append(d)
            t, a, n = kcl_triangles(d)
            ok = np.isfinite(t.reshape(len(t), -1)).all(1)
            all_t.append(t[ok]), all_a.append(a[ok]), all_n.append(n[ok])
            files.append(dict(name=name, bytes=len(d), triangles=int(ok.sum())))
        kcl = (np.concatenate(all_t), np.concatenate(all_a), np.concatenate(all_n))
        # tree trunks: each tree class's trunk (attribute 0x1105 in Common/ArcScene's tree KCLs,
        # model space) as an upright cylinder through the placed matrix
        trunks = {}
        arc = "Common/ArcScene/common.carc"
        for cls, f in ((2, "WS2_common_tree.kcl"), (3, "WS2_common_tree2.kcl"), (4, "WS2_common_tree3.kcl")):
            t, a, _ = kcl_triangles(w.get(arc, f))
            v = t[a == 0x1105].reshape(-1, 3)
            cx, cz = (v[:, 0].min() + v[:, 0].max()) / 2, (v[:, 2].min() + v[:, 2].max()) / 2
            rad = float(np.max(np.hypot(v[:, 0] - cx, v[:, 2] - cz)))
            trunks[cls] = (cx, cz, float(v[:, 1].min()), float(v[:, 1].max()), rad)
        n = 0
        for o in self.objs:
            if o["cls"] not in trunks:
                continue
            cx, cz, y0, y1, rad = trunks[o["cls"]]
            M = o["mtx"]
            base = M[:3, :3] @ [cx, y0, cz] + M[:3, 3]
            top = M[:3, :3] @ [cx, y1, cz] + M[:3, 3]
            sxz = math.sqrt(abs(np.linalg.det(M[[0, 2]][:, [0, 2]])))
            pkg.add("cylinders", x=float(base[0]), z=float(base[2]), y0=float(min(base[1], top[1])),
                    y1=float(max(base[1], top[1])), radius=float(rad * sxz))
            n += 1
        self.say("collision: %s; %d trunk cylinders (model trunks %s)" % (
            ", ".join("%s %d B %d tris" % (f["name"], f["bytes"], f["triangles"]) for f in files), n,
            {CLASSES[c][0]: [round(x, 1) for x in v] for c, v in trunks.items()}))
        self.inv["collision"] = dict(files=files, cylinders=n,
                                     trunks={CLASSES[c][0]: dict(x=v[0], z=v[1], y0=v[2], y1=v[3], radius=v[4])
                                             for c, v in trunks.items()})
        return kcl


def sample_path(anim, node, step):
    """[(frame, x, y, z)] of a CHR0 node's translation, every `step` frames."""
    nd = anim.nodes[node]
    fr = np.arange(0.0, anim.frames + 1e-6, step)
    return np.array([[f] + [c(f) for c in nd["trans"]] for f in fr])


def path_length(p):
    return float(np.sum(np.linalg.norm(np.diff(p[:, 1:4], axis=0), axis=1)))


def nearest_frame(p, pos):
    return float(p[np.argmin(np.linalg.norm(p[:, 1:4] - np.asarray(pos), axis=1)), 0])


def add_orbit(ab, model, node, radius, period, n=16):
    """A node going round a circle of `radius` on x/z once in `period` frames, heading along it:
    Hermite keys with the circle's own slopes (16 of them keep it within 0.02% of round)."""
    w = 2 * math.pi / period
    kx, kz = [], []
    for i in range(n + 1):
        th = 2 * math.pi * i / n
        f = period * i / n
        kx.append((f, radius * math.sin(th), radius * math.cos(th) * w))
        kz.append((f, radius * math.cos(th), -radius * math.sin(th) * w))
    ab.add(ng3d.TT_NODE_TRANS_X, node, kx, ng3d.IP_HERMITE, model=model)
    ab.add(ng3d.TT_NODE_TRANS_Y, node, [(0.0, 0.0, 0.0)], ng3d.IP_LINEAR, model=model)
    ab.add(ng3d.TT_NODE_TRANS_Z, node, kz, ng3d.IP_HERMITE, model=model)
    # the direction of travel at angle th is (cos th, 0, -sin th): a Y rotation of th + 90 degrees
    ab.add(ng3d.TT_NODE_ROT_Y, node, [(0.0, 90.0, 0.0), (float(period), 450.0, 0.0)], ng3d.IP_LINEAR, model=model)
    for t in (ng3d.TT_NODE_ROT_X, ng3d.TT_NODE_ROT_Z):
        ab.add(t, node, [(0.0, 0.0, 0.0)], ng3d.IP_LINEAR, model=model)
    for t in (ng3d.TT_NODE_SCALE_X, ng3d.TT_NODE_SCALE_Y, ng3d.TT_NODE_SCALE_Z):
        ab.add(t, node, [(0.0, 1.0, 0.0)], ng3d.IP_LINEAR, model=model)


def texture_summary(pkg):
    names = {v: k for k, v in ng3d.ENUMS["TexFormat"].items()}
    out = Counter()
    n = Counter()
    for t, d in zip(pkg.textures, pkg.texture_data):
        out[names[t.format]] += len(d)
        n[names[t.format]] += 1
    return {k: dict(count=n[k], gx_bytes=out[k]) for k in sorted(out)}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("data", help="the port's data directory (the package goes to DATA/wuhu/island.ng3d)")
    ap.add_argument("--wsr", default=DEFAULT_WSR, help="the Wii Sports Resort extraction's root (holding files/)")
    ap.add_argument("--bake", help="the renderer's ng3d-bake: run as TOOL RAW OUT after writing")
    ap.add_argument("--keep-raw", action="store_true", help="with --bake: keep island.raw.ng3d")
    ap.add_argument("--json", help="where the inventory goes (default DATA/wuhu/island.json)")
    a = ap.parse_args(argv)
    t0 = time.time()
    st = Stager(Wsr(a.wsr))
    st.all_tag0 = read_pmp(st.wsr.get("Stage/PlnScene/PlnEnv0.carc", "WS2_pln_island_tag0.pmp"))
    st.stage_island()
    st.stage_props()
    st.build_cells()
    st.stage_instances()
    st.stage_anims()
    kcl = st.stage_collision()
    st.stage_env()
    st.stage_markers(kcl)
    pkg = st.pkg
    bad = pkg.check()
    if bad:
        for b in bad[:30]:
            print("BAD", b)
        raise SystemExit("the package has %d bad references" % len(bad))
    data = pkg.write()
    back = ng3d.Package.read(data)
    if back.write() != data:
        raise SystemExit("the package doesn't read back to the same bytes")
    out_dir = os.path.join(a.data, "wuhu")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, "island.ng3d")
    raw = os.path.join(out_dir, "island.raw.ng3d") if a.bake else out
    with open(raw, "wb") as f:
        f.write(data)
    if a.bake:
        subprocess.run([a.bake, raw, out], check=True)
        if not a.keep_raw:
            os.remove(raw)
    st.inv["package"] = dict(path=out, bytes=os.path.getsize(out), baked=bool(a.bake),
                             sections=[dict(tag=t, bytes=s, count=c) for t, s, c in pkg.summary()],
                             textures=texture_summary(pkg), stats=dict(st.cv.stats))
    st.inv["log"] = st.log
    with open(a.json or os.path.join(out_dir, "island.json"), "w") as f:
        json.dump(st.inv, f, ensure_ascii=False, indent=1,
                  default=lambda x: x.tolist() if hasattr(x, "tolist") else str(x))
    st.say("wrote %s: %d bytes in %.1f s (%s)" % (out, os.path.getsize(out), time.time() - t0,
                                                 ", ".join("%s %d" % (t, s) for t, s, _ in pkg.summary())))
    for k, v in sorted(st.cv.stats.items()):
        print("  %s: %s" % (k, v))
    return 0


if __name__ == "__main__":
    sys.exit(main())
