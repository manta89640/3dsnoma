"""Wii no Ma's Mii body (g3d/Mii/Model.lex: Male_Body) posed as the channel poses it: what ミドハ's
garment is built around (tools/mii/midoha_outfit_model.py) and checked against
(tools/mii/midoha_outfit_check.py). Reads the user's extracted files only (nothing of them is
written anywhere): NOMA_MII_DIR, default /Users/user/Projects/3dsnoma/extracted/fs/00000026/g3d/Mii
(Model.lex.dec and Animation/*.lex.dec, the decompressed BRRES).

Units are Wii no Ma's Mii units (the body 158 tall in its bind pose, the T-pose; +y up, +z the
front, +x the Mii's left). The pose is CalcWorld's at height and weight 1: each node's SRT from the
clip's tracks (a node without one keeps its rest SRT), parents first, and the head's node
(Head_Only) scaled by the head scale (mii::ModelRes +0x198, 0.69: BodyCallback ExecCallbackB).
"""
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "g3d"))
import brres  # noqa: E402

MII_DIR = os.environ.get("NOMA_MII_DIR", "/Users/user/Projects/3dsnoma/extracted/fs/00000026/g3d/Mii")
HEAD_SCALE = 0.69
# the clips the garment is checked in: (file, clip) by short name
CLIPS = {
    "wait": ("Wait.lex.dec", "Wait"),
    "walk": ("Move.lex.dec", "Walk"),
    "run": ("Move.lex.dec", "Run"),
    "sit": ("Sit_Sofa.lex.dec", "Sit_Sofa"),
    "seiza": ("Event_Play_Dsi.lex.dec", "Dsi_Seiza_Wait"),
}

_res = {}


def res(name):
    if name not in _res:
        _res[name] = brres.BRRES.load(os.path.join(MII_DIR, name))
    return _res[name]


def model(female=False):
    return res("Model.lex.dec").models["Female_Body" if female else "Male_Body"]


def rot_xyz(r):
    rx, ry, rz = [math.radians(a) for a in r]
    cx, sx, cy, sy, cz, sz = math.cos(rx), math.sin(rx), math.cos(ry), math.sin(ry), math.cos(rz), math.sin(rz)
    return np.array([[cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz],
                     [cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz],
                     [-sy, sx * cy, cx * cy]])


def clip(name):
    f, a = CLIPS.get(name, (None, None)) if name in CLIPS else name
    return res(os.path.join("Animation", f)).anims["CHR0"][a]


def clip_frames(name):
    return clip(name).frames


def pose(m, clip_name="wait", frame=0.0, head_scale=HEAD_SCALE):
    """{node index: 4x4 world}; clip_name None: the bind pose (with the head scale)"""
    a = clip(clip_name) if clip_name else None
    W = {}
    for n in m.nodes:
        s, r, t = list(n.scale), list(n.rot), list(n.trans)
        if a is not None and n.name in a.nodes:
            tr = a.nodes[n.name]
            for part, dst in (("scale", s), ("rot", r), ("trans", t)):
                cv = tr.get(part)
                if isinstance(cv, list):
                    for i in range(3):
                        dst[i] = cv[i](frame)
        L = np.eye(4)
        L[:3, :3] = rot_xyz(r) * np.array(s)[None, :] if a is not None else rot_xyz(r) * np.array(s)[None, :]
        L[:3, 3] = t
        W[n.index] = (W[n.parent] if n.parent >= 0 else np.eye(4)) @ L
    for n in m.nodes:
        if n.name == "Head_Only":
            W[n.index] = W[n.index] @ np.diag([head_scale, head_scale, head_scale, 1.0])
    return W


def bind(m):
    """{node index: 4x4 bind world} (ResNodeData +0x70, the T-pose, no head scale)"""
    return m.world_matrices()


def node_index(m, name):
    for n in m.nodes:
        if n.name == name:
            return n.index
    raise KeyError(name)


def vertices(m):
    """The body's vertices once each, in the bind pose: (pos (n,3) model space, weights: list of
    {node index: w}, shape names, the material of each)"""
    Wb = bind(m)
    mats = {d.shape: m.materials[d.mat].name for d in m.draws()}
    out_p, out_w, out_mat = [], [], []
    for si, p in enumerate(m.shapes):
        t = m.shape_triangles(p, world=False, want=("pos",))
        P = t["pos"].astype(float)
        mt = t["mtx"]
        seen = set()
        for k in range(len(P)):
            key = (round(P[k][0], 4), round(P[k][1], 4), round(P[k][2], 4), int(mt[k]))
            if key in seen:
                continue
            seen.add(key)
            mid = int(mt[k])
            nd = m.matrix_node(mid)
            if nd >= 0:
                B = Wb[nd]
                out_p.append(P[k] @ B[:3, :3].T + B[:3, 3])
                out_w.append({nd: 1.0})
            else:
                out_p.append(P[k].copy())
                out_w.append({m.matrix_node(i): w for i, w in m.envelopes[mid]})
            out_mat.append(mats.get(si, "?"))
    return np.array(out_p), out_w, out_mat


def triangles(m):
    """The body's triangles in the bind pose: (pos (k,3,3), weights per corner [[{node: w}]*3],
    material per triangle)"""
    Wb = bind(m)
    mats = {d.shape: m.materials[d.mat].name for d in m.draws()}
    P_all, W_all, M_all = [], [], []
    for si, p in enumerate(m.shapes):
        t = m.shape_triangles(p, world=False, want=("pos",))
        P = t["pos"].astype(float)
        mt = t["mtx"]
        Q = np.empty_like(P)
        ws = []
        for k in range(len(P)):
            mid = int(mt[k])
            nd = m.matrix_node(mid)
            if nd >= 0:
                B = Wb[nd]
                Q[k] = P[k] @ B[:3, :3].T + B[:3, 3]
                ws.append({nd: 1.0})
            else:
                Q[k] = P[k]
                ws.append({m.matrix_node(i): w for i, w in m.envelopes[mid]})
        n = len(P) // 3
        P_all.append(Q.reshape(n, 3, 3))
        W_all += [ws[3 * i:3 * i + 3] for i in range(n)]
        M_all += [mats.get(si, "?")] * n
    return np.concatenate(P_all), W_all, M_all


def skin(points, weights, W, Wb):
    """points (n,3) in the bind pose, weights list of {node: w} -> posed by W (linear blend)"""
    out = np.zeros_like(points, dtype=float)
    inv = {}
    for i, (p, wd) in enumerate(zip(points, weights)):
        acc = np.zeros(3)
        for nd, w in wd.items():
            if nd not in inv:
                inv[nd] = W[nd] @ np.linalg.inv(Wb[nd])
            M = inv[nd]
            acc += w * (M[:3, :3] @ p + M[:3, 3])
        out[i] = acc
    return out


def skin_matrices(W, Wb, n_nodes):
    """(n_nodes, 4, 4): W[i] @ inverse(bind[i]), the matrices a bind-pose point is skinned by"""
    S = np.zeros((n_nodes, 4, 4))
    for i in range(n_nodes):
        S[i] = W[i] @ np.linalg.inv(Wb[i])
    return S


def skin_dense(points, wmat, S):
    """points (n,3) bind pose; wmat (n, n_nodes) dense weights; S from skin_matrices -> (n,3)"""
    M = np.einsum("nk,kij->nij", wmat, S)          # (n,4,4) blended
    return np.einsum("nij,nj->ni", M[:, :3, :3], points) + M[:, :3, 3]


def blend_matrices(wmat, S):
    return np.einsum("nk,kij->nij", wmat, S)
