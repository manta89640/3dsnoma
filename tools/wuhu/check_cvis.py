#!/usr/bin/env python3
"""Check a package's cell visibility rows (CVIS) by pictures: from random walkable points, the island's
follow camera at rest behind the Mii (tools/g3d/preview.py), each frame drawn with and without the
row of the eye's cell; they must be the same, pixel for pixel.

    tools/wuhu/check_cvis.py PACKAGE OUTDIR [--points 20] [--seed 1] [--cells 19,20,...] [--size 800x450]

PACKAGE is an unbaked package (preview.py reads GX texels). Each point gets three frames: yaws 120
degrees apart, or with --cells (points in those cells: the ones whose rows hide the most, say)
toward the middle of what its row hides and 30 degrees either side. A frame whose eye is higher
over the ground under it than Env.pvs_max_height uses no row (as a renderer does). OUTDIR gets each
pair side by side. Prints a line a frame and how many were identical; exits 1 if any differ.
"""
import argparse
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "g3d"))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import ng3d  # noqa: E402
import preview  # noqa: E402
import stage_wuhu as S  # noqa: E402
import viewstats  # noqa: E402

# the follow camera at rest (engine/wuhu/follow_camera.cpp FollowCameraParams::make) for the island's
# Mii (158 Wii no Ma units at k / 1.7: engine/wuhu/island.cpp MII_SCALE)
MII = 158.0 / (10.0 / 1.7)
HEAD, DISTANCE, PITCH, AIM = 0.85 * MII, 4.5 * MII, 15.0, 1.6 * MII


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("package")
    ap.add_argument("outdir")
    ap.add_argument("--points", type=int, default=20)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--cells")
    ap.add_argument("--size", default="800x450")
    a = ap.parse_args(argv)
    from PIL import Image
    W, H = (int(x) for x in a.size.split("x"))
    pkg = ng3d.Package.read(open(a.package, "rb").read())
    e = pkg.env
    if not e.pvs_row_bytes:
        raise SystemExit("%s has no visibility rows" % a.package)
    os.makedirs(a.outdir, exist_ok=True)
    stats = viewstats.Stats(pkg, viewstats.kcl_of(pkg))
    kcl = stats.kcl
    rng = np.random.default_rng(a.seed)
    rb = e.pvs_row_bytes
    geo = [any(c.count[:c.n_lods]) for c in pkg.cells]

    def row_of(x, z):
        i = int(math.floor((z - e.grid_origin[1]) / e.cell_size)) * e.n_cells_x + \
            int(math.floor((x - e.grid_origin[0]) / e.cell_size))
        return i, pkg.cvis[i * rb:(i + 1) * rb]

    def hidden(row):
        return [j for j in range(len(pkg.cells)) if geo[j] and not row[j >> 3] >> (j & 7) & 1]

    def walkable(x, z):
        g = S.kcl_ground(*kcl, x, z, 1e5)
        if g is None:
            return None
        y, at, n = g
        ok = n[1] >= math.cos(math.radians(50)) and at not in (0x1106, 0x110B) and (at >> 8) not in (0x00, 0x08)
        return y if ok and y > 1.0 else None

    cells = [int(c) for c in a.cells.split(",")] if a.cells else None
    lo, hi = np.array(e.world_bbox[:3]), np.array(e.world_bbox[3:])
    pts, tries = [], 0
    while len(pts) < a.points and tries < 200000:
        tries += 1
        if cells:
            c = cells[len(pts) % len(cells)]
            x0, z0 = e.grid_origin[0] + (c % e.n_cells_x) * e.cell_size, e.grid_origin[1] + (c // e.n_cells_x) * e.cell_size
            x, z = rng.uniform(x0, x0 + e.cell_size), rng.uniform(z0, z0 + e.cell_size)
        else:
            x, z = rng.uniform(lo[0], hi[0]), rng.uniform(lo[2], hi[2])
        y = walkable(x, z)
        if y is not None:
            pts.append((x, y, z))
    same = total = 0
    shares = []
    print("%-4s %24s %5s %7s %5s %5s %s" % ("pt", "feet", "yaw", "eye_h", "cell", "hide", "pictures"))
    for k, (x, y, z) in enumerate(pts):
        yaw, spread = float(rng.uniform(0, 360)), (0.0, 120.0, 240.0)
        if cells:
            hid = hidden(row_of(x, z)[1])
            if hid:
                c = np.mean([[(pkg.cells[j].bbox[0] + pkg.cells[j].bbox[3]) / 2,
                              (pkg.cells[j].bbox[2] + pkg.cells[j].bbox[5]) / 2] for j in hid], 0)
                yaw = math.degrees(math.atan2(c[0] - x, c[1] - z))
            spread = (0.0, -30.0, 30.0)
        for dy in spread:
            yw, pt = math.radians(yaw + dy), math.radians(PITCH)
            fwd = np.array([math.sin(yw) * math.cos(pt), -math.sin(pt), math.cos(yw) * math.cos(pt)])
            pivot = np.array([x, y + HEAD, z])
            eye, target = pivot - fwd * DISTANCE, pivot + np.array([0.0, AIM - HEAD, 0.0])
            g = stats.ground(eye[0], eye[2], eye[1])
            if g is None or g > eye[1] - 2.1:          # (lifted over the ground, as the camera is)
                g2 = S.kcl_ground(*kcl, eye[0], eye[2], 1e5)
                eye[1] = (g2[0] if g2 else 0.0) + 2.1
                g = stats.ground(eye[0], eye[2], eye[1])
            eh = eye[1] - g if g is not None else float("inf")
            ci, row = row_of(eye[0], eye[2])
            use = eh <= e.pvs_max_height
            hid = hidden(row) if use else []
            shares.append(len(hid) / sum(geo))
            view = preview.View(eye, target, 45.0, W, H, near=1.5, far=80000.0)
            sc = preview.Scene(pkg)
            sc.apply_material_anims(0.0)
            img = preview.render(sc, view, sc.gather(eye))[0]
            img2 = img
            if hid:
                sc = preview.Scene(pkg)
                sc.apply_material_anims(0.0)
                img2 = preview.render(sc, view, sc.gather(eye, cell_filter=lambda j: bool(row[j >> 3] >> (j & 7) & 1)))[0]
            d = int(np.abs(img.astype(int) - img2.astype(int)).max())
            same += d == 0
            total += 1
            Image.fromarray(np.concatenate([img, img2], 1)).save(
                os.path.join(a.outdir, "p%02d_%03d.png" % (k, int(yaw + dy) % 360)))
            print("%-4d %24s %5.0f %7.1f %5d %5s %s" % (k, "(%.0f, %.0f, %.0f)" % (x, y, z), (yaw + dy) % 360, eh, ci,
                                                      len(hid) if use else "-", "same" if d == 0 else "DIFFER by %d" % d))
    print("%d of %d frames the same with and without the rows; the rows hide %.1f%% of the cells with geometry "
          "at these eyes on average" % (same, total, 100 * float(np.mean(shares)) if shares else 0.0))
    return 0 if same == total else 1


if __name__ == "__main__":
    sys.exit(main())
