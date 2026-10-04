#!/usr/bin/env python3
"""The reference pictures NOMA_SELFTEST=g3drender compares the host's software backend with: every
camera marker named view_* of a package, drawn by preview.py with the conventions the test draws
with -- the same size, field of view, near and far planes and frame, and point lights lit as the
engine lights them (World turns a point light into a direction from its position toward the
origin: preview.py's point_as_directional).

    preview_views.py PACKAGE OUTDIR [--size 640x456] [--fov 45] [--frame 600] [--near 1.5]
                     [--far 80000] [--views view_town,view_coast]

Writes OUTDIR/<view>.png and OUTDIR/<view>.ppm (binary PPM: what the C++ test reads).
"""
import argparse
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ng3d  # noqa: E402
import preview  # noqa: E402


def main(argv=None):
    ap = argparse.ArgumentParser(description="reference renders of a package's view_* cameras")
    ap.add_argument("package")
    ap.add_argument("outdir")
    ap.add_argument("--size", default="640x456")
    ap.add_argument("--fov", type=float, default=45.0)
    ap.add_argument("--frame", type=float, default=600.0)
    ap.add_argument("--near", type=float, default=1.5)
    ap.add_argument("--far", type=float, default=80000.0)
    ap.add_argument("--views", help="comma-separated marker names (default: every view_*)")
    a = ap.parse_args(argv)
    from PIL import Image
    W, H = [int(x) for x in a.size.split("x")]
    pkg = ng3d.Package.read(open(a.package, "rb").read())
    want = a.views.split(",") if a.views else None
    os.makedirs(a.outdir, exist_ok=True)
    for m in pkg.markers:
        name = pkg.string(m.name)
        if not name.startswith("view_") or (want and name not in want):
            continue
        eye = np.array(m.position, np.float32)
        target = np.array(m.dir, np.float32) if m.kind == 2 else eye + np.array(m.dir, np.float32)
        t0 = time.time()
        sc = preview.Scene(pkg, point_as_directional=True)
        sc.apply_material_anims(a.frame)
        items = sc.gather(eye, frame=a.frame)
        view = preview.View(eye, target, a.fov, W, H, near=a.near, far=a.far)
        img, _ = preview.render(sc, view, items)
        Image.fromarray(img, "RGB").save(os.path.join(a.outdir, name + ".png"))
        with open(os.path.join(a.outdir, name + ".ppm"), "wb") as f:
            f.write(b"P6\n%d %d\n255\n" % (W, H))
            f.write(img.tobytes())
        print("%s: %d items, %.1f s" % (name, len(items), time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
