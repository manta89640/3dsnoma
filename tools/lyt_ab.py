#!/usr/bin/env python3
"""Compare two sets of frame pictures, picture by picture (the layouts drawn natively against the
decompiled draw, NOMA_CHECK_LYT_DRAW=orig, or any other A/B of the same moments).

    tools/lyt_ab.py A_DIR B_DIR OUT_DIR [--threshold DB] [--off PERCENT]

A_DIR and B_DIR hold pictures of the same names: the host's frames/NNNNN.png and NNNNN_3ds.png
(NOMA_DUMP_FRAMES, NOMA_DUMP_3DS) or the 3DS's shots/NNNNN.bmp and NNNNN_bottom.bmp
(NOMA_SHOT_EVERY). For each name in both, OUT_DIR gets diff_<name>.png (the absolute difference,
amplified 4 times) and side_<name>.png (A | B | diff); index.html lists them worst first with the
PSNR, the share of pixels whose worst channel differs by more than 16 levels (as
platform/host/selftest_g3drender.cpp's compare) and the box the differences lie in. The exit status
is 1 when any picture is under the thresholds (default 40 dB, 0.5%).

Two runs differ where the game itself does from run to run (the room's date banner, the Sound
Player's dance, anything timed by the clock): look at the pictures, not only the numbers.
"""
import argparse
import html
import math
import os
import sys

import numpy as np
from PIL import Image

EXTENSIONS = (".png", ".bmp")


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


def compare(a, b):
    """PSNR over R, G, B (99 dB when equal), the share of pixels off by more than 16, and their box"""
    d = np.abs(a - b)
    mse = float(np.mean(d.astype(np.float64) ** 2))
    psnr = 99.0 if mse == 0 else min(99.0, 10 * math.log10(255.0 ** 2 / mse))
    worst = d.max(axis=2)
    off = worst > 16
    box = None
    if off.any():
        ys, xs = np.nonzero(off)
        box = (int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max()))
    return psnr, 100.0 * float(off.mean()), box, d


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("out")
    ap.add_argument("--threshold", type=float, default=40.0, help="lowest acceptable PSNR (dB)")
    ap.add_argument("--off", type=float, default=0.5, help="highest acceptable share of pixels off (%%)")
    args = ap.parse_args(argv)

    names = sorted(n for n in os.listdir(args.a)
                   if n.lower().endswith(EXTENSIONS) and os.path.exists(os.path.join(args.b, n)))
    if not names:
        sys.exit("no pictures of the same name in %s and %s" % (args.a, args.b))
    os.makedirs(args.out, exist_ok=True)

    rows, bad = [], 0
    for n in names:
        a, b = load(os.path.join(args.a, n)), load(os.path.join(args.b, n))
        if a.shape != b.shape:
            rows.append((0.0, 100.0, None, n, "sizes differ: %s, %s" % (a.shape[1::-1], b.shape[1::-1])))
            bad += 1
            continue
        psnr, off, box, d = compare(a, b)
        stem = os.path.splitext(n)[0]
        diff = np.clip(d * 4, 0, 255).astype(np.uint8)
        Image.fromarray(diff).save(os.path.join(args.out, "diff_%s.png" % stem))
        side = np.concatenate([a.astype(np.uint8), b.astype(np.uint8), diff], axis=1)
        Image.fromarray(side).save(os.path.join(args.out, "side_%s.png" % stem))
        failed = psnr < args.threshold or off > args.off
        bad += failed
        rows.append((psnr, off, box, n, "FAIL" if failed else ""))

    rows.sort(key=lambda r: (r[0], -r[1]))
    for psnr, off, box, n, note in rows:
        print("%-24s %6.2f dB %7.3f%% off  box %-22s %s" % (n, psnr, off, box or "-", note))
    with open(os.path.join(args.out, "index.html"), "w") as f:
        f.write("<!doctype html><meta charset=utf-8><title>lyt A/B</title>"
                "<style>body{font:14px sans-serif;margin:16px}img{max-width:100%%}"
                "td{padding:2px 8px}</style><h1>%s | %s</h1><table>" %
                (html.escape(args.a), html.escape(args.b)))
        for psnr, off, box, n, note in rows:
            stem = os.path.splitext(n)[0]
            f.write("<tr><td>%s</td><td>%.2f dB</td><td>%.3f%% off</td><td>%s</td><td>%s</td></tr>"
                    "<tr><td colspan=5><img src='side_%s.png'></td></tr>" %
                    (html.escape(n), psnr, off, box or "-", note, html.escape(stem)))
        f.write("</table>")
    print("%d pictures, %d under the thresholds; %s" % (len(rows), bad, os.path.join(args.out, "index.html")))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
