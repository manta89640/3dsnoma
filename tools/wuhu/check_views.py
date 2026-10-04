#!/usr/bin/env python3
"""Render a staged island package from its view_* cameras and compare with reference pictures.

    tools/wuhu/check_views.py PACKAGE REFDIR OUTDIR [--terrain] [--views town,coast,...] [--size WxH]

REFDIR holds view_<name>.png (e.g. the research renders of the BRRES, made the same way at
1280x720). For each view OUTDIR gets pkg_view_<name>.png (tools/g3d/preview.py's render of the
package), diff_view_<name>.png (the absolute difference, amplified 4 times) and side_view_<name>.png
(package above, reference below); the table printed gives PSNR and the share of pixels that differ
by more than 16 levels. --terrain draws the cells' bare terrain (detail level 2) without instances:
the like-for-like comparison with renders of the island model alone.
"""
import argparse
import math
import os
import sys
import time

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools/g3d"))
sys.path.insert(0, os.path.join(ROOT, "tools/wuhu"))
import ng3d  # noqa: E402
import preview  # noqa: E402
from stage_wuhu import VIEWS  # noqa: E402


def psnr(a, b):
    mse = float(np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2))
    return float("inf") if mse == 0 else 10 * math.log10(255.0 ** 2 / mse)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("package")
    ap.add_argument("refdir")
    ap.add_argument("outdir")
    ap.add_argument("--terrain", action="store_true", help="bare terrain, no instances (cell level 2)")
    ap.add_argument("--views", default=",".join(VIEWS))
    ap.add_argument("--size", default="1280x720")
    ap.add_argument("--frame", type=float, default=0.0)
    ap.add_argument("--like-research", action="store_true",
                    help="the research renderer's conventions: no material animation (the materials' own "
                         "SRTs), the sky at the origin, fog 0 for every fogged material, the sun as a "
                         "directional light from the origin")
    a = ap.parse_args(argv)
    W, H = [int(x) for x in a.size.split("x")]
    pkg = ng3d.Package.read(open(a.package, "rb").read())
    os.makedirs(a.outdir, exist_ok=True)
    suffix = ("_terrain" if a.terrain else "") + ("_like" if a.like_research else "")
    like = dict(material_anims=False, sky_follows=False, fog_index=0, point_as_directional=True) \
        if a.like_research else {}
    print("%-9s %8s %8s %10s %7s" % ("view", "PSNR dB", ">16 lvl", "items", "secs"))
    for name in a.views.split(","):
        mk = [m for m in pkg.markers if pkg.string(m.name) == "view_" + name][0]
        fov = VIEWS[name][2]
        t0 = time.time()
        img, n = preview.render_package(pkg, mk.position, mk.dir, fov, W, H, frame=a.frame,
                                        lod=2 if a.terrain else None, instances=not a.terrain, **like)
        dt = time.time() - t0
        Image.fromarray(img).save(os.path.join(a.outdir, "pkg_view_%s%s.png" % (name, suffix)), optimize=True)
        ref_path = os.path.join(a.refdir, "view_%s.png" % name)
        if not os.path.exists(ref_path):
            print("%-9s %8s %8s %10d %7.1f" % (name, "-", "-", n, dt))
            continue
        ref = np.asarray(Image.open(ref_path).convert("RGB"))
        d = np.abs(img.astype(np.int16) - ref.astype(np.int16))
        Image.fromarray(np.clip(d * 4, 0, 255).astype(np.uint8)).save(
            os.path.join(a.outdir, "diff_view_%s%s.png" % (name, suffix)), optimize=True)
        side = np.concatenate([img, ref], 0)
        Image.fromarray(side).resize((W * 3 // 4, H * 3 // 2)).save(
            os.path.join(a.outdir, "side_view_%s%s.png" % (name, suffix)), optimize=True)
        print("%-9s %8.2f %7.2f%% %10d %7.1f" % (name, psnr(img, ref), 100.0 * float((d.max(2) > 16).mean()), n, dt))
    return 0


if __name__ == "__main__":
    sys.exit(main())
