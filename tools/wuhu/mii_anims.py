#!/usr/bin/env python3
"""Wuhu Island's Mii clip archive: DATA/wuhu/mii_anim.brres, from your own Wii no Ma and Wii Sports Resort.

    tools/wuhu/mii_anims.py DATA [--noma SRC] [--wsr ROOT] [--out FILE]

The island plays the household's Mii with the channel's own body and animation files (engine/wuhu/
island_mii.h); this archive gives it the clips those files lack, as one more animation file (its
short name "Island"), in place of the content's Vote, Send_DSi and Sit_Sofa files the island loads
when it isn't staged. SRC is Wii no Ma's content 26 -- tools/unpack_all.py's extracted/fs/00000026
directory, or the content's U8 archive as tools/stage_data.py copies it into DATA/content (the one
holding g3d/Mii/Model.lex; found there by default) -- and ROOT your Wii Sports Resort extraction
(tools/wuhu/stage_wuhu.py's --wsr). Nothing of either is copied anywhere else, and the archive (made
of them) doesn't belong in the repository.

The clips, all on Wii no Ma's body (Male_Body and Female_Body have one skeleton), 60 frames a second:
  Jump     Send_DSi's Send_DSi_Jump, frames 12..47: the take-off, rise, apex, fall, touchdown (at 26)
           and the landing squat, without the clip's own rise (the island's physics jumps the body):
           the root lowered by the lowest ankle's height over its rest while the feet are off the
           ground.
  Land_M   Vote's Male_Land (falling with the arms up, the touchdown at 14, the squat and the stand)
  Land_F   and Female_Land (touchdown at 10), likewise without their fall.
  Slide    Male_Land's frame 17: a crouch with the arms forward, for sliding down a slope.
  Fall     WSR's swf_ava_fall (Swordplay: off the platform): the arms windmilling, the legs
           bicycling, 20 frames looped.
  Glide    WSR's omk_ava_parachute (Skydiving: hanging in the harness, hands up at the toggles), 99
           frames looped, turned to face the way the Mii does (the clip faces its x).
  Drive    the driving seat: WSR's pln_ava_wait (Island Flyover's pilot) seated, the arms from
  Helm     jsk_ava_drivepose (Jet Ski: a steering axis, frame 0 full left, 10 straight, 20 full
           right) in the body's frame; Helm the same arms on Wait's standing pose (a boat's wheel).
  Swim     made here: a front crawl, the body pitched face down, the back at the water's surface (98
           units over the feet, the swimmer's 0.62 of the height: engine/wuhu/player.cpp SWIM_SINK),
           the arms a stroke each in 48 frames, the flutter kick three beats a stroke.
  Tread    made here: upright, the arms sculling, the legs cycling, 60 frames looped.
WSR's clips are moved onto Wii no Ma's body as research found both skeletons alike (the same T pose):
each mapped bone's world rotation is the WSR bone's times a constant rest offset (the identity for
the root, chest and head, a half turn about the bone for the limbs), every other bone keeps its rest
pose against its parent, the root's translation is WSR's times the legs' length ratio, and the knees'
helpers (Knee_L, Knee_R: in every Noma clip half way between the thigh and the shin, as the bind pose
has them) are made so.

The archive is tools/g3d/chr0_write.py's BRRES of CHR0 version 5 clips; it is read back (brres.py)
and every clip's pose checked against the one it was made from (FK) before it is written.
"""
import argparse
import glob
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tools/g3d"))
sys.path.insert(0, HERE)
import brres  # noqa: E402
import chr0_write  # noqa: E402
import nintendo_formats as nf  # noqa: E402

DEFAULT_NOMA = os.path.join(ROOT, "extracted/fs/00000026")
OUT = "wuhu/mii_anim.brres"
SURFACE = 98.0           # the water's surface over a swimmer's feet (0.62 x 158)
srt_matrix = brres.MDL0.srt_matrix


# ------------------------------------------------------------------ sources

class Noma:
    """Wii no Ma's content 26: a directory (extracted/fs/00000026) or its U8 archive."""

    def __init__(self, src):
        self.src = src
        self.files = None
        if os.path.isfile(src):
            data, _ = nf.maybe_decompress(open(src, "rb").read())
            self.files = {p: data[o:o + s] for p, o, s in nf.u8_list(data) if o is not None}
        elif not os.path.isdir(src):
            raise FileNotFoundError(src)

    def get(self, rel):
        data = self.files[rel] if self.files is not None else open(os.path.join(self.src, rel), "rb").read()
        return nf.lz77_decompress(data) if data[:1] in (b"\x10", b"\x11") else data

    def brres(self, rel):
        return brres.BRRES(self.get(rel), rel)

    @staticmethod
    def find(data_dir):
        """the content archive in DATA/content holding Wii no Ma's Mii model, else the extraction"""
        for p in sorted(glob.glob(os.path.join(data_dir, "content", "*.app"))):
            try:
                data, _ = nf.maybe_decompress(open(p, "rb").read())
                if any(name == "g3d/Mii/Model.lex" for name, _, _ in nf.u8_list(data)):
                    return p
            except Exception:
                continue
        return DEFAULT_NOMA


class Rig:
    """FK over nw4r CHR0 clips (scale tracks dropped: neither body scales a bone)."""

    def __init__(self, mdl):
        self.nodes = mdl.nodes
        self.by_name = {n.name: n for n in mdl.nodes}

    def local(self, n, anim=None, f=0.0):
        s, r, t = [1, 1, 1], list(n.rot), list(n.trans)
        nd = anim.nodes.get(n.name) if anim is not None else None
        if nd is not None:
            fl = nd["flags"]

            def ev(part, rest):
                v = nd[part]
                if v == "model":
                    return rest
                if v is None:
                    return [0, 0, 0] if fl & 6 else rest
                return [c(f) for c in v]
            r, t = ev("rot", r), ev("trans", t)
        m = np.eye(4)
        m[:3] = srt_matrix(s, r, t)
        return m

    def pose(self, anim=None, f=0.0, override=None):
        """{name: world 4x4}"""
        W = {}
        for n in self.nodes:
            m = override[n.name] if override and n.name in override else self.local(n, anim, f)
            W[n.name] = W[self.nodes[n.parent].name] @ m if n.parent >= 0 else m
        return W


def orthonormal(M):
    U, _, Vt = np.linalg.svd(M)
    R = U @ Vt
    if np.linalg.det(R) < 0:
        U[:, -1] *= -1
        R = U @ Vt
    return R


def quat(m):
    m = np.asarray(m)[:3, :3]
    t = np.trace(m)
    if t > 0:
        s = math.sqrt(t + 1) * 2
        return np.array([0.25 * s, (m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s])
    i = int(np.argmax(np.diag(m)))
    if i == 0:
        s = math.sqrt(1 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        return np.array([(m[2, 1] - m[1, 2]) / s, 0.25 * s, (m[0, 1] + m[1, 0]) / s, (m[0, 2] + m[2, 0]) / s])
    if i == 1:
        s = math.sqrt(1 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        return np.array([(m[0, 2] - m[2, 0]) / s, (m[0, 1] + m[1, 0]) / s, 0.25 * s, (m[1, 2] + m[2, 1]) / s])
    s = math.sqrt(1 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
    return np.array([(m[1, 0] - m[0, 1]) / s, (m[0, 2] + m[2, 0]) / s, (m[1, 2] + m[2, 1]) / s, 0.25 * s])


def quat_matrix(q):
    w, x, y, z = q / np.linalg.norm(q)
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def slerp(a, b, t):
    d = float(a @ b)
    if d < 0:
        b, d = -b, -d
    if d > 0.9995:
        q = a + (b - a) * t
        return q / np.linalg.norm(q)
    th = math.acos(d)
    return (a * math.sin((1 - t) * th) + b * math.sin(t * th)) / math.sin(th)


def rot(axis, deg):
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    if axis == "x":
        return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])
    if axis == "y":
        return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])


def euler(R):
    """nw4r's node rotation (degrees, R = Rz Ry Rx) of a rotation matrix"""
    ry = -math.asin(max(-1.0, min(1.0, R[2, 0])))
    rx, rz = math.atan2(R[2, 1], R[2, 2]), math.atan2(R[1, 0], R[0, 0])
    return [math.degrees(rx), math.degrees(ry), math.degrees(rz)]


def unwrap(values):
    """a rotation's values with no jump over 180 degrees from one frame to the next (the frame is
    set fractionally: a wrap would spin the bone through the frames between)"""
    out = [values[0]]
    for v in values[1:]:
        while v - out[-1] > 180:
            v -= 360
        while v - out[-1] < -180:
            v += 360
        out.append(v)
    return out


# ------------------------------------------------------------------ the body

# WSR's avatar bones for Wii no Ma's (research anim report §3)
WSR_MAP = {"Skl_Root": "skl_root", "Chest": "chest", "Arm_L1": "arm_l1", "Arm_L2": "arm_l2", "Wrist_L": "wrist_l",
           "Arm_R1": "arm_r1", "Arm_R2": "arm_r2", "Wrist_R": "wrist_r", "Head": "head", "Hip": "hip",
           "Foot_L1": "leg_l1", "Foot_L2": "leg_l2", "Ankle_L": "ankle_l", "Foot_R1": "leg_r1", "Foot_R2": "leg_r2",
           "Ankle_R": "ankle_r"}
ARMS = ("Arm_L1", "Arm_L2", "Wrist_L", "Arm_R1", "Arm_R2", "Wrist_R")


class Body:
    """Wii no Ma's body: poses as world rotations (and the root's position) and the clip nodes of them."""

    def __init__(self, noma):
        mdl = noma.brres("g3d/Mii/Model.lex").models["Male_Body"]
        self.rig = Rig(mdl)
        self.nodes = mdl.nodes
        self.rest = self.rig.pose()
        self.rest_local = {n.name: self.rig.local(n) for n in self.nodes}
        R = self.rest
        self.knee = {s: (R["Foot_%s1" % s][:3, :3].T @ R["Knee_%s" % s][:3, :3],
                         R["Foot_%s2" % s][:3, :3].T @ R["Knee_%s" % s][:3, :3]) for s in "LR"}

    def G0(self, name):
        return self.rest[name][:3, :3]

    def locals(self, G, root_t):
        """{name: local 4x4} for world rotations G (any bone missing keeps its rest pose against its
        parent; the knees' helpers half way between thigh and shin) and the root at root_t"""
        L, W = {}, {}
        for n in self.nodes:
            par = self.nodes[n.parent].name if n.parent >= 0 else None
            rest = self.rest_local[n.name]
            if n.name in G:
                R = G[n.name]
            elif n.name.startswith("Knee_"):
                s = n.name[-1]
                c1, c2 = self.knee[s]
                R = quat_matrix(slerp(quat(W["Foot_%s1" % s] @ c1), quat(W["Foot_%s2" % s] @ c2), 0.5))
            else:
                R = W[par] @ rest[:3, :3] if par else rest[:3, :3]
            W[n.name] = R
            m = np.eye(4)
            m[:3, :3] = W[par].T @ R if par else R
            m[:3, 3] = root_t if n.name == "Skl_Root" else rest[:3, 3]
            L[n.name] = m
        return L

    def world(self, anim, f):
        """(world rotations, root position) of a Noma clip at frame f"""
        W = self.rig.pose(anim, f)
        return {n: W[n][:3, :3] for n in W}, W["Skl_Root"][:3, 3].copy()

    def lift(self, G, root_t):
        """the lowest ankle over its rest height (10) for this pose"""
        W = self.rig.pose(override=self.locals(G, root_t))
        return min(W["Ankle_L"][1, 3], W["Ankle_R"][1, 3]) - 10.0


CLIP_NODES = ["Skl_Root", "Chest", "Arm_L1", "Arm_L2", "Wrist_L", "Arm_R1", "Arm_R2", "Wrist_R", "Head", "Hip",
              "Foot_L1", "Foot_L2", "Ankle_L", "Knee_L", "Foot_R1", "Foot_R2", "Ankle_R", "Knee_R"]


def make_clip(body, name, poses, loop):
    """A chr0_write.Clip of poses [(G, root_t)], frames 0..n-1 (a loop's frame n its frame 0, else
    the last again)"""
    n = len(poses)
    ends = poses + [poses[0] if loop else poses[-1]]
    locs = [body.locals(G, t) for G, t in ends]
    clip = chr0_write.Clip(name, n, loop)
    for node in CLIP_NODES:
        r = [euler(L[node][:3, :3]) for L in locs]
        rx, ry, rz = (unwrap([e[i] for e in r]) for i in range(3))
        # (every node's translation: one a clip doesn't give is 0 to nw4r, not the model's)
        trans = [[L[node][i, 3] for L in locs] for i in range(3)]
        # (and the model node's segment scale compensation, as the channel's clips carry it: the
        # body scales its bones by the Mii's height, and a node without it gets the scale twice)
        flags = body.rig.by_name[node].flags
        ssc = (0x400 if flags & 0x20 else 0) | (0x800 if flags & 0x40 else 0)
        clip.node(node, [rx, ry, rz], trans, ssc)
    return clip, locs


# ------------------------------------------------------------------ WSR's clips on the body

class Retarget:
    def __init__(self, body, wsr, anim):
        nm = "WS2_avaA1" if ("clavicle_l" in anim.targets or "all_root" in anim.targets) else "WS2_avaA2"
        self.src = Rig(wsr.brres("Common/Kokeshi/common.carc", "G3D/%s.brres" % nm).models[nm])
        self.anim, self.body = anim, body
        S0, D0 = self.src.pose(), body.rest
        self.C = {b: S0[a][:3, :3].T @ D0[b][:3, :3] for b, a in WSR_MAP.items()}
        self.scale = (D0["Skl_Root"][1, 3] - D0["Ankle_L"][1, 3]) / (S0["skl_root"][1, 3] - S0["ankle_l"][1, 3])

    def world(self, f):
        S = self.src.pose(self.anim, f)
        G = {b: orthonormal(S[a][:3, :3]) @ self.C[b] for b, a in WSR_MAP.items()}
        return G, S["skl_root"][:3, 3] * self.scale


def turned(G, R):
    """every world rotation of a pose turned by R (about the origin)"""
    return {k: R @ v for k, v in G.items()}


# ------------------------------------------------------------------ the clips

def noma_clip(body, noma, file, anim, first, n, touchdown, name):
    """frames first..first + n - 1 of a Noma clip, the root lowered by the clip's lift before its
    touchdown (the frame from first at which the feet are down again)"""
    a = noma.brres("g3d/Mii/Animation/%s.lex" % file).anims["CHR0"][anim]
    poses = []
    for k in range(n):
        G, t = body.world(a, first + k)
        if touchdown is not None and k < touchdown:
            t = t.copy()
            t[1] -= max(body.lift(G, t), 0.0)
        poses.append((G, t))
    return poses


def wsr_clip(body, wsr, rel, member, anim, frames=None, yaw=0.0, centre=True):
    a = wsr.brres(rel, member).anims["CHR0"][anim]
    rt = Retarget(body, wsr, a)
    n = frames or int(a.frames)
    R = rot("y", yaw)
    poses = []
    for f in range(n):
        G, t = rt.world(f)
        G, t = turned(G, R), R @ t
        poses.append((G, t))
    if centre:
        # the root over the origin (its sway kept), its height its mean plus what keeps the clip's
        # own up and down
        mx = np.mean([t[0] for _, t in poses])
        mz = np.mean([t[2] for _, t in poses])
        poses = [(G, np.array([t[0] - mx, t[1], t[2] - mz])) for G, t in poses]
    return poses


def steering(body, wsr, base_G, base_t):
    """the 21 frames of jsk_ava_drivepose's arms (in the body's frame) on a pose"""
    a = wsr.brres("Stage/JskScene/Jsk.carc", "G3D/WS2_jsk_ava.brres").anims["CHR0"]["WS2_jsk_ava_drivepose"]
    rt = Retarget(body, wsr, a)
    poses = []
    for f in range(21):
        G, _ = rt.world(f)
        # (the arms as they are against the jet ski's chest, on the pose's chest, the shoulders' and
        # chest's lean taken out: the hands forward at a wheel)
        to_base = base_G["Chest"] @ G["Chest"].T
        P = dict(base_G)
        for b in ARMS:
            P[b] = to_base @ G[b]
        poses.append((P, base_t))
    return poses


def swim(body, n=48):
    """a front crawl: n frames a cycle (two arm strokes)"""
    poses = []
    for f in range(n):
        u = f / n
        roll = 22.0 * math.sin(2 * math.pi * u)
        W = rot("x", 78.0) @ rot("y", roll)          # face down, rolling with the strokes
        G = {"Skl_Root": W @ body.G0("Skl_Root"), "Chest": W @ body.G0("Chest"),
             "Hip": W @ body.G0("Hip"), "Head": W @ rot("x", -35.0) @ rot("y", -0.6 * roll) @ body.G0("Head")}
        for side, phase, down in (("L", 0.0, -90.0), ("R", 0.5, 90.0)):
            v = (u + phase) % 1.0
            # the arm's swing in the body: reach over the head (-180), the pull under the body to the
            # hip (0) over 60 % of the stroke, the recovery out of the water over the back
            if v < 0.6:
                th = -180.0 + 180.0 * (v / 0.6)
                bend = 25.0 * math.sin(math.pi * v / 0.6)
            else:
                w = (v - 0.6) / 0.4
                th = 180.0 * w
                bend = 95.0 * math.sin(math.pi * w)
            A = rot("x", th) @ rot("z", down)
            B = rot("x", th - bend) @ rot("z", down)
            G["Arm_%s1" % side] = W @ A @ body.G0("Arm_%s1" % side)
            G["Arm_%s2" % side] = W @ B @ body.G0("Arm_%s2" % side)
            G["Wrist_%s" % side] = W @ B @ body.G0("Wrist_%s" % side)
            # the flutter kick: three beats a stroke, the legs opposite
            k = math.sin(2 * math.pi * (3 * u + (0.5 if side == "R" else 0.0)))
            hip, knee = -8.0 + 14.0 * k, 12.0 + 10.0 * max(-k, 0.0)
            G["Foot_%s1" % side] = W @ rot("x", -hip) @ body.G0("Foot_%s1" % side)
            G["Foot_%s2" % side] = W @ rot("x", -hip + knee) @ body.G0("Foot_%s2" % side)
            G["Ankle_%s" % side] = W @ rot("x", -hip + knee + 40.0) @ body.G0("Ankle_%s" % side)
        # the hips 12 under the surface, a little behind the feet's point: the swimmer lies over it
        poses.append((G, np.array([0.0, SURFACE - 12.0, -18.0])))
    return poses


def tread(body, n=60):
    """treading water: upright, sculling, the legs cycling; n frames a loop"""
    poses = []
    for f in range(n):
        u = f / n
        s = math.sin(2 * math.pi * u)
        G = {"Head": rot("x", 4.0 * math.sin(2 * math.pi * u + 1.0)) @ body.G0("Head")}
        for side, down in (("L", -1.0), ("R", 1.0)):
            # the upper arms out and down, the forearms forward, sweeping in and out
            A = rot("y", down * (-10.0)) @ rot("z", down * 55.0)
            sweep = 30.0 * s * (-down)
            B = rot("y", sweep) @ rot("x", -70.0) @ rot("z", down * 90.0)
            G["Arm_%s1" % side] = A @ body.G0("Arm_%s1" % side)
            G["Arm_%s2" % side] = B @ body.G0("Arm_%s2" % side)
            G["Wrist_%s" % side] = B @ rot("z", down * -20.0) @ body.G0("Wrist_%s" % side)
            c = math.sin(2 * math.pi * u + (math.pi if side == "R" else 0.0))
            hip, knee = 35.0 + 20.0 * c, 55.0 + 25.0 * c
            G["Foot_%s1" % side] = rot("x", -hip) @ body.G0("Foot_%s1" % side)
            G["Foot_%s2" % side] = rot("x", -hip + knee) @ body.G0("Foot_%s2" % side)
            G["Ankle_%s" % side] = rot("x", -hip + knee + 20.0) @ body.G0("Ankle_%s" % side)
        poses.append((G, np.array([0.0, 70.0 + 2.0 * math.sin(4 * math.pi * u), 0.0])))
    return poses


def build(noma, wsr, say=print):
    body = Body(noma)
    clips = []
    want = []
    want.append(("Jump", noma_clip(body, noma, "Send_DSi", "Send_DSi_Jump", 12, 36, 27, "Jump"), False))
    want.append(("Land_M", noma_clip(body, noma, "Vote", "Male_Land", 0, 31, 14, "Land_M"), False))
    want.append(("Land_F", noma_clip(body, noma, "Vote", "Female_Land", 0, 31, 10, "Land_F"), False))
    want.append(("Slide", noma_clip(body, noma, "Vote", "Male_Land", 17, 2, None, "Slide"), False))
    want.append(("Fall", wsr_clip(body, wsr, "Stage/SwfScene/Common.carc", "G3D/WS2_swf_ava.brres",
                                  "WS2_swf_ava_fall"), True))
    want.append(("Glide", wsr_clip(body, wsr, "Common/OmkScene/common.carc", "G3D/WS2_omk_ava.brres",
                                   "WS2_omk_ava_parachute", yaw=90.0), True))
    pilot = wsr_clip(body, wsr, "Common/PlnScene/common.carc", "G3D/WS2_pln_ava.brres", "WS2_pln_ava_wait",
                     frames=1)[0]
    want.append(("Drive", steering(body, wsr, pilot[0], pilot[1]), False))
    wait = noma.brres("g3d/Mii/Animation/Wait.lex").anims["CHR0"]["Wait"]
    standing = body.world(wait, 0)
    want.append(("Helm", steering(body, wsr, standing[0], standing[1]), False))
    want.append(("Swim", swim(body), True))
    want.append(("Tread", tread(body), True))
    made = []
    for name, poses, loop in want:
        clip, locs = make_clip(body, name, poses, loop)
        clips.append(clip)
        made.append((name, locs))
    data = chr0_write.brres(clips)
    check(body, data, made, say)
    return data, clips


def check(body, data, made, say):
    """the archive read back: every clip's every frame posed (FK) as the poses it was made of"""
    back = brres.BRRES(data)
    worst = 0.0
    for name, locs in made:
        a = back.anims["CHR0"][name]
        assert a.frames == len(locs) - 1, name
        for f, L in enumerate(locs[:-1]):
            want = body.rig.pose(override=L)
            got = body.rig.pose(a, f)
            for n in CLIP_NODES:
                worst = max(worst, float(np.abs(want[n][:3, 3] - got[n][:3, 3]).max()))
    if worst > 0.5:
        raise SystemExit("the archive doesn't read back as made: a joint %.2f units off" % worst)
    say("mii_anims: read back, every joint within %.3f units of its pose" % worst)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("data", help="the port's data directory (the archive goes to DATA/wuhu/mii_anim.brres)")
    ap.add_argument("--noma", help="Wii no Ma's content 26: extracted/fs/00000026 or its U8 archive (default: the "
                                   "one in DATA/content, else %s)" % os.path.relpath(DEFAULT_NOMA, ROOT))
    ap.add_argument("--wsr", help="the Wii Sports Resort extraction's root (stage_wuhu.py's default)")
    ap.add_argument("--out", help="where the archive goes (default DATA/%s)" % OUT)
    a = ap.parse_args(argv)
    import stage_wuhu
    noma = Noma(a.noma or Noma.find(a.data))
    data, clips = build(noma, stage_wuhu.Wsr(a.wsr or stage_wuhu.DEFAULT_WSR))
    out = a.out or os.path.join(a.data, OUT)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        f.write(data)
    print("mii_anims: wrote %s: %d bytes, %d clips (%s)" % (out, len(data), len(clips),
                                                          ", ".join("%s %d" % (c.name, c.frames) for c in clips)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
