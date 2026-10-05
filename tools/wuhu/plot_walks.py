#!/usr/bin/env python3
"""Plot the walk selftest's paths over the island's height map.

    tools/wuhu/plot_walks.py PATHS.csv HEIGHT.png OUT.png

PATHS.csv is what NOMA_SELFTEST=walk writes with NOMA_WALK_OUT (walk,t,x,y,z,state,eye_x,eye_y,eye_z:
ten points a second; walks at the second k are prefixed "k2_"). HEIGHT.png is a top-down map at 25
units a pixel with its corner at (-36000, -16000), +x right and +z down (the research kcl_height.png,
made from your own extraction). The output: the whole island with a box round each place a walk
went, and each place enlarged beside it with its walks drawn (the second k's dashed), each walk's
start a ring, its end a dot.
"""
import csv
import sys
from collections import defaultdict

from PIL import Image, ImageDraw

X0, Z0, RES = -36000.0, -16000.0, 25.0
COLOURS = [(255, 64, 64), (255, 220, 0), (0, 230, 255), (255, 128, 255), (128, 255, 128), (255, 160, 32),
           (160, 160, 255), (255, 255, 255), (0, 160, 96), (255, 96, 160), (96, 200, 255)]


def main(paths_csv, height_png, out_png):
    walks = defaultdict(list)
    with open(paths_csv) as f:
        for row in csv.DictReader(f):
            walks[row["walk"]].append((float(row["x"]), float(row["z"])))
    base = Image.open(height_png).convert("RGB")
    names = sorted({w[3:] if w.startswith("k2_") else w for w in walks})
    colour = {n: COLOURS[i % len(COLOURS)] for i, n in enumerate(names)}

    # places: walks whose boxes are near each other share one inset
    boxes = []
    for n in names:
        pts = walks.get(n, []) + walks.get("k2_" + n, [])
        xs, zs = [p[0] for p in pts], [p[1] for p in pts]
        box = [min(xs), min(zs), max(xs), max(zs)]
        for b in boxes:
            if box[0] < b[2] + 300 and box[2] > b[0] - 300 and box[1] < b[3] + 300 and box[3] > b[1] - 300:
                b[0], b[1], b[2], b[3] = min(b[0], box[0]), min(b[1], box[1]), max(b[2], box[2]), max(b[3], box[3])
                b[4].append(n)
                break
        else:
            boxes.append(box + [[n]])

    inset = 420
    cols = 3
    rows = (len(boxes) + cols - 1) // cols
    W, H = base.size
    scale = 0.5
    full = base.resize((int(W * scale), int(H * scale)), Image.BILINEAR)
    out = Image.new("RGB", (full.width + cols * (inset + 8) + 8, max(full.height, rows * (inset + 30) + 8)), (20, 20, 20))
    out.paste(full, (0, 0))
    d = ImageDraw.Draw(out)
    for i, b in enumerate(boxes):
        # the place: a square round the walks with a margin
        cx, cz = (b[0] + b[2]) / 2, (b[1] + b[3]) / 2
        half = max(b[2] - b[0], b[3] - b[1]) / 2 + 60
        px0, pz0 = (cx - half - X0) / RES, (cz - half - Z0) / RES
        px1, pz1 = (cx + half - X0) / RES, (cz + half - Z0) / RES
        d.rectangle([px0 * scale, pz0 * scale, px1 * scale, pz1 * scale], outline=(255, 255, 255))
        d.text((px1 * scale + 2, pz0 * scale), str(i + 1), fill=(255, 255, 255))
        crop = base.crop((int(px0), int(pz0), int(px1) + 1, int(pz1) + 1)).resize((inset, inset), Image.BILINEAR)
        ox = full.width + 8 + (i % cols) * (inset + 8)
        oz = 8 + (i // cols) * (inset + 30)
        out.paste(crop, (ox, oz))
        k = inset / ((int(px1) + 1 - int(px0)) * RES)

        def to(p):
            return (ox + (p[0] - (X0 + int(px0) * RES)) * k, oz + (p[1] - (Z0 + int(pz0) * RES)) * k)
        for n in b[4]:
            for name, dashed in ((n, False), ("k2_" + n, True)):
                pts = [to(p) for p in walks.get(name, [])]
                if len(pts) < 2:
                    continue
                c = colour[n] if not dashed else tuple(v // 2 + 64 for v in colour[n])
                if dashed:
                    for j in range(0, len(pts) - 1, 2):
                        d.line([pts[j], pts[j + 1]], fill=c, width=2)
                else:
                    d.line(pts, fill=c, width=2)
                s, e = pts[0], pts[-1]
                d.ellipse([s[0] - 5, s[1] - 5, s[0] + 5, s[1] + 5], outline=c, width=2)
                d.ellipse([e[0] - 3, e[1] - 3, e[0] + 3, e[1] + 3], fill=c)
        d.text((ox + 4, oz + inset + 4), "%d: %s  (%.0f units across)" % (i + 1, ", ".join(b[4]), 2 * half),
               fill=(255, 255, 255))
        for j, n in enumerate(b[4]):
            d.text((ox + 4, oz + 4 + 12 * j), n, fill=colour[n])
    out.save(out_png)


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
