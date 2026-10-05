#!/usr/bin/env python3
"""ミドハ's hanbok checked in the Mii's clips: does anything that is drawn come through it?

    tools/mii/midoha_outfit_check.py [--clips wait,walk,run,sit,seiza] [--every 4] [--sheet OUT.png --png TEX.png]

The body's own torso, arms and legs aren't drawn under the garment (engine/wuhu/midoha_outfit.h):
only its hands and shoes are. So, for every few frames of each clip (posed as the channel poses the
body, the garment skinned by its weights as the game skins it):
  * the shoes against the skirt: a shoe's vertex above the hem (its closest point on the skirt more
    than HEM_FREE above the hem's edge) must be inside the skirt;
  * the hands against the sleeves: a hand's vertex whose closest point on its sleeve is short of the
    cuff (more than CUFF_FREE from its end) must be inside the sleeve;
  * the layers: the collar, the belt and the buckle must stay outside the surface under them, and
    the jeogori's hem and the skirt's top inside the belt;
  * tearing: no triangle stretched to more than STRETCH times its length in the modelling pose.
It prints the worst of each per clip, and with --sheet renders the poses (textured with --png).
Reads the user's extracted Mii files (tools/mii/midoha_body.py); writes nothing but the sheet.
"""
import argparse
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import midoha_body as body  # noqa: E402
import midoha_outfit_model as model  # noqa: E402

HEM_FREE = 1.0
CUFF_FREE = 1.2
STRETCH = 2.5


def posed(bd, pc, S):
    return body.skin_dense(pc.Pb, pc.W, S)


def normals(P, T):
    fn = np.cross(P[T[:, 1]] - P[T[:, 0]], P[T[:, 2]] - P[T[:, 0]])
    acc = np.zeros_like(P)
    for c in range(3):
        np.add.at(acc, T[:, c], fn)
    return model.unit(acc)


def body_parts(bd, S):
    """the drawn body parts posed: {'hand_L', 'hand_R', 'shoe_L', 'shoe_R': (n,3) vertices}"""
    tp, tw, _ = body.triangles(bd.m)
    groups = {"hand_L": "Wrist_L", "hand_R": "Wrist_R", "shoe_L": "Ankle_L", "shoe_R": "Ankle_R"}
    out = {}
    for key, node in groups.items():
        ni = bd.idx[node]
        pts, wts = [], []
        for k in range(len(tp)):
            if any(ni in c for c in tw[k]):
                for c in range(3):
                    pts.append(tp[k][c])
                    w = np.zeros(bd.n)
                    for n_, x in tw[k][c].items():
                        w[n_] += x
                    wts.append(w)
        out[key] = body.skin_dense(np.array(pts), np.array(wts), S)
    return out


def inside_check(V, P, T, N, free_mask):
    """vertices V against the surface (P, T, normals N): those outside it (on its normals' side)
    whose closest point isn't in the free region (free_mask per surface vertex); their depth out"""
    cp, tri, bary, d = model.closest_on_mesh(P, T, V)
    n = (N[T[tri]] * bary[:, :, None]).sum(1)
    side = ((V - cp) * n).sum(1)
    free = (free_mask[T[tri]] * bary).sum(1) > 0.5
    bad = (side > 0.05) & ~free
    return bad, np.where(bad, d, 0.0)


def check_frame(bd, pieces, S, ref_len):
    by = {pc.name: pc for pc in pieces}
    res = {}
    parts = body_parts(bd, S)
    # shoes against the skirt (and its rim)
    sk = by["skirt"]
    P = posed(bd, sk, S)
    N = normals(P, sk.T)
    hem_y0 = model.SKIRT_ROWS[0][0]
    free = sk.attrs["y"] < hem_y0 + HEM_FREE + 0.01
    worst = 0.0
    nbad = 0
    for key in ("shoe_L", "shoe_R"):
        bad, depth = inside_check(parts[key], P, sk.T, N, free)
        nbad += int(bad.sum())
        worst = max(worst, float(depth.max()) if len(depth) else 0.0)
    res["shoes out of the skirt"] = (nbad, worst)
    # hands against their sleeves
    nbad, worst = 0, 0.0
    for side, key in (("L", "hand_L"), ("R", "hand_R")):
        sl = by["sleeve_" + side]
        P = posed(bd, sl, S)
        N = normals(P, sl.T)
        free = sl.attrs["t"] * sl.attrs["L"] > sl.attrs["L"] - CUFF_FREE
        bad, depth = inside_check(parts[key], P, sl.T, N, free)
        nbad += int(bad.sum())
        worst = max(worst, float(depth.max()))
    res["hands through the sleeves"] = (nbad, worst)
    # layers: the collar's and the belt's vertices must stay outside the shell (by their offset)
    sh = by["jeogori"]
    Ps = posed(bd, sh, S)
    Ns = normals(Ps, sh.T)
    worst, nbad = 0.0, 0
    for name in ("git", "dongjeong", "belt", "buckle", "buckle_frame"):
        pc = by[name]
        V = posed(bd, pc, S)
        if name in ("git", "dongjeong"):
            acr = pc.attrs["across"]
            V = V[(acr >= 0) & (acr <= 1)]
        cp, tri, bary, d = model.closest_on_mesh(Ps, sh.T, V)
        n = (Ns[sh.T[tri]] * bary[:, :, None]).sum(1)
        side = ((V - cp) * n).sum(1)
        b = side < 0.05
        nbad += int(b.sum())
        worst = max(worst, float((-side[b]).max()) if b.any() else 0.0)
    res["trim under the jeogori"] = (nbad, worst)
    # the jeogori's hem and the skirt's top inside the belt
    bt = by["belt"]
    Pb = posed(bd, bt, S)
    Nb = normals(Pb, bt.T)
    worst, nbad = 0.0, 0
    for name, ylim in (("jeogori", (model.BELT_Y0 + 0.4, model.BELT_Y1 - 0.4)), ("skirt", (model.BELT_Y0 + 0.4, 80))):
        pc = by[name]
        sel = (pc.P[:, 1] > ylim[0]) & (pc.P[:, 1] < ylim[1])
        V = posed(bd, pc, S)[sel]
        out_mask = np.zeros(len(Pb), bool)
        bad, depth = inside_check(V, Pb, bt.T, Nb, out_mask)
        nbad += int(bad.sum())
        worst = max(worst, float(depth.max()) if len(depth) else 0.0)
    res["hem or skirt through the belt"] = (nbad, worst)
    # stretch
    worst = 0.0
    for pc in pieces:
        P = posed(bd, pc, S)
        for a, b in ((0, 1), (1, 2), (2, 0)):
            l = np.linalg.norm(P[pc.T[:, a]] - P[pc.T[:, b]], axis=1)
            worst = max(worst, float((l / np.maximum(ref_len[pc.name][(a, b)], 1e-6)).max()))
    res["stretch"] = (int(worst > STRETCH), worst)
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--clips", default="wait,walk,run,sit,seiza")
    ap.add_argument("--every", type=int, default=4)
    a = ap.parse_args()
    bd, pieces, _ = model.build()
    ref_len = {pc.name: {(x, y): np.linalg.norm(pc.P[pc.T[:, x]] - pc.P[pc.T[:, y]], axis=1)
                         for x, y in ((0, 1), (1, 2), (2, 0))} for pc in pieces}
    for clip in a.clips.split(","):
        n = int(body.clip_frames(clip))
        worst = {}
        for f in range(0, n, a.every):
            W = body.pose(bd.m, clip, float(f))
            S = body.skin_matrices(W, bd.Wb, bd.n)
            r = check_frame(bd, pieces, S, ref_len)
            for k, (cnt, d) in r.items():
                c0, d0, f0 = worst.get(k, (0, 0.0, -1))
                if d > d0 or (cnt > c0 and d >= d0):
                    worst[k] = (max(cnt, c0), d, f)
                else:
                    worst[k] = (max(cnt, c0), d0, f0)
        print("%s (%d frames, every %d):" % (clip, n, a.every))
        for k, (cnt, d, f) in worst.items():
            print("   %-32s %s" % (k, ("worst %.2f (frame %d), %d vertices at most" % (d, f, cnt)) if k != "stretch"
                                    else "worst %.2fx (frame %d)" % (d, f)))


if __name__ == "__main__":
    main()
