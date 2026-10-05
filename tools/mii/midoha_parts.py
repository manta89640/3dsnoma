#!/usr/bin/env python3
"""ミドハ's head pieces -- his whole hair, his pointed ears, the hair band at his ponytail's base and the
cord of three jade beads -- as a native g3d package: smooth shapes with their shading painted into one
texture (tools/mii/midoha_hair.py says what they are and how the concept is read; the machinery is
tools/mii/midoha_shapes.py).

    tools/mii/midoha_parts.py DATA [--band A|B] [--out PATH] [--png ATLAS.png] [--obj OUT.obj]
        -> DATA/wuhu/midoha.ng3d (and DATA/wuhu/midoha_outfit.bin: tools/mii/midoha_outfit.py)

All of it is ours (the user's concept, our geometry and painting): nothing of Nintendo's is read or
written. tools/mii/midoha_check.py, run where RFL_Res.dat is at hand, measures the hair's clearance
over RFL's own head.

Space: RFL's head units (origin at the chin, +y up, +z toward the face, +x the Mii's left).
engine/wuhu/midoha.cpp draws the package with the matrix RFL draws his face with (his body's Head_Only
node), so the pieces sit on his head wherever it turns.

The package: two models of the same pieces (LOD: "midoha_head", and "midoha_head_far" with about half
the vertices from FAR guest units off, where his head is a few dozen pixels high), one node each; one
material (the texture times the vertex colour, unlit; depth tested and written, both faces drawn: some
thin pieces are seen from behind); one vertex buffer (position, normal, colour, texture coordinates);
one texture, the atlas (512 x 256, GX RGB565 and its mip levels down to 8 x 8: the painting is flat
tones, which 16 bits keep without banding); one instance (the head's matrix goes on it every frame); no
environment (k = 1: the package's units are the guest's).
"""
import argparse
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "g3d"))
import ng3d  # noqa: E402

sys.path.insert(0, HERE)
import midoha_hair  # noqa: E402
import midoha_outfit  # noqa: E402
from midoha_shapes import Atlas, gx_levels  # noqa: E402

ATLAS = (512, 256)
GX_RGB565 = 4
FAR = 450.0          # guest units (a Mii is about 150 tall): the far model from there on
FAR_DETAIL = 0.55    # of each piece's grid knots


def build(band="B"):
    """the pieces, the atlas and its image (RGB)"""
    midoha_hair.BAND_OPTION = band
    pieces = midoha_hair.build()
    atlas = Atlas(*ATLAS)
    atlas.place(list(pieces.values()))
    image = atlas.bake(list(pieces.values()))
    return pieces, atlas, image


def material(p, texture, levels):
    """the texture times the vertex colour, unlit: TEV stage 0 = TEXC * RASC (alpha: the vertex's);
    texture coordinates from the vertices' (texgen 0: TEX0, no matrix); trilinear"""
    col_env = 15 | 10 << 4 | 8 << 8 | 15 << 12 | 1 << 19   # a 0, b TEXC, c RASC, d 0; clamp
    alpha_env = (7 << 13) | (7 << 10) | (7 << 7) | (5 << 4) | (1 << 19)  # d = RASA
    tref = 0 | 0 << 3 | 1 << 6 | 0 << 7                    # map 0, coordinate 0, enabled, colour channel 0
    maps = [ng3d.Rec("TexMap", texture=ng3d.NONE16) for _ in range(8)]
    maps[0] = ng3d.Rec("TexMap", texture=texture, wrap_s=0, wrap_t=0, min_filter=5, mag_filter=1, max_aniso=0,
                       flags=0, lod_bias=0.0, min_lod=0.0, max_lod=float(levels - 1))
    gens = [ng3d.Rec("TexGen", matrix=ng3d.NONE16, post_mtx=ng3d.NONE16) for _ in range(8)]
    gens[0] = ng3d.Rec("TexGen", type=1, source=4, normalize=0, matrix=ng3d.NONE16, post_mtx=ng3d.NONE16)
    return p.add("materials", name=p.str("midoha_painted"), flags=0, n_stages=1, n_texgens=1, n_chans=1,
                 maps=maps, texgens=gens,
                 cull=0, color_env=[col_env] + [0] * 15, alpha_env=[alpha_env] + [0] * 15,
                 tref=[tref] + [0] * 7, ksel=[0] * 8,
                 alpha_compare=7 << 16 | 7 << 19,  # always, always
                 zmode=1 | 3 << 1 | 1 << 4,          # test, LEQUAL, write
                 blend=0x18,                         # no blending, colour and alpha written
                 chan_color=1, chan_alpha=1,         # material colour from the vertex, unlit
                 mat_color=[255, 255, 255, 255], amb_color=[0, 0, 0, 255])


UV_SCALE = 1.0 / 32768.0


def package(pieces, atlas, image):
    p = ng3d.Package()
    data, levels = gx_levels(image, GX_RGB565)
    p.add("textures", name=p.str("midoha_atlas"), width=atlas.w, height=atlas.h, format=ng3d.TF_RGB565,
          levels=levels, flags=ng3d.TEX_GX_RAW, gx_format=GX_RGB565, src_width=atlas.w, src_height=atlas.h)
    p.texture_data.append(data)
    mat = material(p, 0, levels)
    vdata = bytearray()
    indices = []
    n = 0
    ident = [[1.0, 0, 0, 0], [0, 1.0, 0, 0], [0, 0, 1.0, 0]]
    stats = {}
    for mi, (model, detail) in enumerate((("midoha_head", 1.0), ("midoha_head_far", FAR_DETAIL))):
        first_batch = len(p.batches)
        lo, hi = np.full(3, 1e9), np.full(3, -1e9)
        tris = 0
        for name, piece in pieces.items():
            V, N, ST, F = piece.mesh(detail)
            UV = piece.uv(ST, atlas)
            colour = piece.colour if piece.colour is not None else (255, 255, 255)
            base = n
            for v, nn, uv in zip(V, N, UV):
                vdata += struct.pack("<3f", *v) + struct.pack("<3bx", *[int(round(x * 127)) for x in nn])
                vdata += bytes([int(c) for c in colour] + [255])
                vdata += struct.pack("<2h", *[int(round(min(max(x, 0.0), 0.99997) / UV_SCALE)) for x in uv])
            n += len(V)
            first = len(indices)
            # (GX's front faces wind clockwise: ours are counter-clockwise seen from outside, so they
            # go in reversed; both faces are drawn anyway)
            for a, b, c in F:
                indices += [base + int(a), base + int(c), base + int(b)]
            tris += len(F)
            lo, hi = np.minimum(lo, V.min(0)), np.maximum(hi, V.max(0))
            p.add("batches", material=mat, vbuf=0, first_index=first, n_indices=len(indices) - first,
                  bbox=[float(x) for x in list(V.min(0)) + list(V.max(0))], node=0, flags=0)
        stats[model] = tris
        p.add("nodes", name=p.str(model), parent=ng3d.NONE16, flags=ng3d.NODE_VISIBLE, scale=[1.0, 1.0, 1.0],
              rotate=[0.0, 0.0, 0.0], translate=[0.0, 0.0, 0.0], bind=ident, first_batch=first_batch,
              n_batches=len(p.batches) - first_batch)
        centre = (lo + hi) / 2
        p.add("models", name=p.str(model), first_node=mi, n_nodes=1, first_batch=first_batch,
              n_batches=len(p.batches) - first_batch, bbox=[float(x) for x in list(lo) + list(hi)],
              radius=float(np.linalg.norm(hi - centre)), lod_next=1 if mi == 0 else ng3d.NONE16,
              lod_distance=FAR if mi == 0 else 0.0)
    if n > 0xFFFF:
        raise ValueError("more than 65535 vertices")
    p.add("vertex_buffers", data=0, count=n, attrs=ng3d.VA_POS | ng3d.VA_NRM | ng3d.VA_CLR | ng3d.VA_UV0,
          stride=24, flags=0, uv_scale=[UV_SCALE, UV_SCALE])
    p.vertex_data.append(bytes(vdata))
    p.indices = indices
    p.add("instances", model=0, flags=0, anim=ng3d.NONE16, class_id=0, mtx=ident, anim_offset=0.0,
          cull_distance=0.0)
    return p, stats


def write_obj(pieces, path):
    out = ["# ミドハ's head pieces (tools/mii/midoha_parts.py): RFL head units"]
    k = 1
    for name, piece in pieces.items():
        V, N, ST, F = piece.mesh()
        out.append("o " + name)
        out += ["v %.3f %.3f %.3f" % tuple(v) for v in V]
        out += ["f %d %d %d" % (k + a, k + b, k + c) for a, b, c in F]
        k += len(V)
    open(path, "w").write("\n".join(out) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("data", help="the port's data directory (the package goes to DATA/wuhu/midoha.ng3d)")
    ap.add_argument("--band", default="B", choices="AB", help="the hair band (B: red with gold edges; A: dark)")
    ap.add_argument("--out", help="the package's path (default DATA/wuhu/midoha.ng3d)")
    ap.add_argument("--png", help="also the atlas as a PNG")
    ap.add_argument("--obj", help="also an OBJ of the pieces")
    a = ap.parse_args()
    pieces, atlas, image = build(a.band)
    p, stats = package(pieces, atlas, image)
    problems = p.check()
    if problems:
        raise SystemExit("the package doesn't check: " + "; ".join(problems[:5]))
    out = a.out or os.path.join(a.data, "wuhu", "midoha.ng3d")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    data = p.write()
    open(out, "wb").write(data)
    if a.png:
        from PIL import Image
        Image.fromarray(image).save(a.png)
    if a.obj:
        write_obj(pieces, a.obj)
    print("%s: %d bytes; %d pieces; triangles: %s" % (out, len(data), len(pieces),
                                                      ", ".join("%s %d" % kv for kv in stats.items())))
    if not a.out:
        print("%s: %d bytes" % midoha_outfit.write(a.data))


if __name__ == "__main__":
    main()
