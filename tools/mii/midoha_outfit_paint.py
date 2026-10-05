#!/usr/bin/env python3
"""ミドハ's hanbok's texture, painted from the user's concept art.

    tools/mii/midoha_outfit_paint.py paint --concept CONCEPT.png [--out tools/mii/midoha_outfit.png]
    tools/mii/midoha_outfit_paint.py flat [--out OUT.png]     each piece one colour (a test)

(Needs the mesh, tools/mii/midoha_outfit_model.py build, and the Mii body for the modelling pose.)

The atlas is 1024 x 1024 (tools/mii/midoha_outfit.py template shows its islands). Each texel is
traced back to its place on the garment in the pose the concept shows him in (the Wait clip, frame
0) and painted there:
  * where the concept sees it (the front, facing the viewer, not hidden), it takes the concept's
    own pixels: the garment's front view is matched to the concept's piece by piece and row by row
    (each row of each piece -- the jeogori, a sleeve, the belt, the skirt -- spans the same part of
    the concept's row), and the pixel is read only from the concept's pixels of its own kind (the
    collar from the collar's colours, the jeogori from the green fabric's, ...), so that the
    concept's shading, its shadow and seam lines and its trim carry over while the edges stay where
    the model's are;
  * elsewhere (the sides, the back, the insides, the skirt below the concept's picture) it is
    painted in the concept's manner: its palette (sampled), a soft cel shading (light from above,
    darker under the arms, below the belt and the collar, in the folds), rim light on the outer
    edges, crisp anti-aliased lines (from exact distances, a texel's width of anti-aliasing): the
    trim's edges, the pleats' folds, the cuffs' bands, the back seam.
The two meet over a soft band where the surface turns away from the front.
"""
import argparse
import math
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import midoha_outfit_model as model  # noqa: E402
import midoha_raster as R  # noqa: E402

OUT = os.path.join(HERE, "midoha_outfit.png")


def hexrgb(h):
    h = h.lstrip("#")
    return np.array([int(h[i:i + 2], 16) for i in (0, 2, 4)], float)


# the concept's colours, sampled from it
PAL = {
    "green": hexrgb("#2A5F4B"), "green_lt": hexrgb("#3F7660"), "green_dk": hexrgb("#2A4E3F"),
    "green_sh": hexrgb("#1F4A39"), "crease": hexrgb("#194533"),
    "git": hexrgb("#1C4537"), "git_lt": hexrgb("#24503F"), "git_line": hexrgb("#0F2B21"),
    "white": hexrgb("#F0E8E0"), "white_sh": hexrgb("#D3C4BC"), "white_line": hexrgb("#A99A92"),
    "skin": hexrgb("#FBC090"), "skin_sh": hexrgb("#D19163"),
    "belt": hexrgb("#271B27"), "belt_lt": hexrgb("#33243A"), "belt_line": hexrgb("#120C12"),
    "skirt": hexrgb("#261A26"), "skirt_lt": hexrgb("#2F2131"), "pleat": hexrgb("#19111B"),
    "lining": hexrgb("#1A3A2E"), "skirt_lining": hexrgb("#170F18"),
}


class Bake:
    """the atlas's texels traced to the garment: per texel the piece, its position and normal in
    the modelling pose, its surface coordinates (st) and the piece's own attributes"""

    def __init__(self, pieces, size, ss=1, density=1.0):
        self.size = size * ss
        self.ss = ss
        self.density = density          # texels a Mii unit at `size`
        S = self.size
        buf = R.Buffer(S, S)
        self.pieces = pieces
        allS, allT, owner = [], [], []
        off = 0
        for pi, pc in enumerate(pieces):
            uv = pc.uv * S
            allS.append(np.stack([uv[:, 0], uv[:, 1], np.zeros(len(uv))], 1))
            allT.append(pc.T + off)
            owner.append(np.full(len(pc.T), pi))
            off += len(pc.P)
        self.Sv = np.concatenate(allS)
        T = np.concatenate(allT)
        self.T = T
        buf.draw(self.Sv, T)
        self.buf = buf
        self.tri_piece = np.concatenate(owner)
        self.mask = buf.tri >= 0
        self.piece = np.full((S, S), -1, int)
        self.piece[self.mask] = self.tri_piece[buf.tri[self.mask]]
        P = np.concatenate([pc.P for pc in pieces])
        N = np.concatenate([pc.N for pc in pieces])
        st = np.concatenate([pc.st for pc in pieces])
        self.pos = buf.interp(P, T)
        self.nrm = buf.interp(N, T)
        self.nrm /= np.maximum(np.linalg.norm(self.nrm, axis=-1, keepdims=True), 1e-9)
        self.st = buf.interp(st, T)

    def attr(self, name, piece_names, default=0.0):
        """a per-vertex attribute of the given pieces, interpolated (default elsewhere)"""
        vals = []
        for pc in self.pieces:
            a = pc.attrs.get(name)
            if pc.name in piece_names and a is not None and np.ndim(a) >= 1 and len(a) == len(pc.P):
                vals.append(np.asarray(a, float).reshape(len(pc.P), -1))
            else:
                vals.append(np.full((len(pc.P), 1), default))
        V = np.concatenate(vals)
        return self.buf.interp(V, self.T)[..., 0]

    def attr_vec(self, name, piece_names):
        """a per-vertex vector attribute of the given pieces, interpolated (zeros elsewhere)"""
        vals = []
        for pc in self.pieces:
            a = pc.attrs.get(name)
            if pc.name in piece_names and a is not None and np.ndim(a) == 2 and len(a) == len(pc.P):
                vals.append(np.asarray(a, float))
            else:
                vals.append(np.zeros((len(pc.P), 3)))
        return self.buf.interp(np.concatenate(vals), self.T)

    def where(self, *prefixes):
        ids = [i for i, pc in enumerate(self.pieces) if pc.name.startswith(prefixes)]
        return np.isin(self.piece, ids)


def dilate(img, mask, n):
    """spread the painted texels' colours n texels into the empty ones (for the lower levels)"""
    img = img.copy()
    m = mask.copy()
    for _ in range(n):
        acc = np.zeros_like(img)
        cnt = np.zeros(m.shape)
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0), (1, 1), (1, -1), (-1, 1), (-1, -1)):
            sm = np.roll(np.roll(m, dy, 0), dx, 1)
            si = np.roll(np.roll(img, dy, 0), dx, 1)
            acc += si * sm[..., None]
            cnt += sm
        grow = ~m & (cnt > 0)
        img[grow] = acc[grow] / cnt[grow][:, None]
        m = m | grow
    return img


FLAT = {"jeogori": "green", "sleeve": "green", "lining": "lining", "cuffrim": "git", "skirt_lining": "skirt_lining",
        "skirt_rim": "pleat", "skirt": "skirt", "neck": "skin", "belt": "belt", "buckle_frame": "belt_line",
        "buckle": "belt_lt", "git": "git", "dongjeong": "white"}


def paint_flat(bake):
    img = np.zeros((bake.size, bake.size, 3))
    for i, pc in enumerate(bake.pieces):
        key = next(k for k in sorted(FLAT, key=len, reverse=True) if pc.name.startswith(k))
        img[bake.piece == i] = PAL[FLAT[key]]
    # a light shading by the normal, to see the shapes
    L = np.array([-0.3, 0.75, 0.6])
    L /= np.linalg.norm(L)
    sh = 0.8 + 0.3 * np.clip((bake.nrm * L).sum(-1), -1, 1)
    img *= sh[..., None]
    return img


def finish(img, bake, out):
    img = dilate(img, bake.mask, 24 * bake.ss)
    im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))
    if bake.ss > 1:
        im = im.resize((bake.size // bake.ss,) * 2, Image.BOX)
    im.save(out)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("what", choices=["paint", "flat"])
    ap.add_argument("--concept")
    ap.add_argument("--out", default=OUT)
    a = ap.parse_args()
    bd, pieces, (layout, density) = model.build()
    bake = Bake(pieces, model.ATLAS_SIZE, ss=2 if a.what == "paint" else 1, density=density)
    if a.what == "flat":
        print(finish(paint_flat(bake), bake, a.out))
    else:
        import midoha_outfit_art as art
        print(finish(art.paint(bake, bd, pieces, a.concept), bake, a.out))


if __name__ == "__main__":
    main()
