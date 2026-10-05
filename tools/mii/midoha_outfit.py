#!/usr/bin/env python3
"""ミドハ's hanbok for the game: his garment mesh and its texture packed into one file.

    tools/mii/midoha_outfit.py DATA [--png IN.png] [--mesh MESH.json]
        tools/mii/midoha_outfit_mesh.json + tools/mii/midoha_outfit.png -> DATA/wuhu/midoha_outfit.bin
    tools/mii/midoha_outfit.py template OUT.png
        the atlas's layout (each piece's island outlined and named), to paint on

The garment (tools/mii/midoha_outfit_model.py makes the mesh; tools/mii/midoha_outfit_paint.py
paints the texture from the concept): ~3,800 triangles skinned to the Mii body's nodes, at most 4
weights a vertex in eighths. To repaint it: paint over tools/mii/midoha_outfit.png (1024 x 1024,
the template shows where each piece is; keep the islands' places, colour a little past their edges)
and run this again; a new mesh needs a new texture (the islands move): run the paint tool after it.

The file (engine/wuhu/midoha.cpp reads it), little-endian but for the GX data, which is the GPU's
big-endian, ready to be copied into guest memory:
  0x00 "MDG1", u32 version 1
  0x08 u32 bones, sets, vertices, batches
  0x18 u16 texture width, height (level 0); u32 levels; f32 texels a Mii unit at level 0
  0x24 u32 offsets of: bones, sets, GX arrays, batches, loads; display lists, their size; texture
       levels, their size; u32 loads
  bones: 64 bytes each: the Mii node's name (16 bytes, zero padded), its inverse bind matrix (3x4 f32)
  sets: 20 bytes each: 4 bone numbers (0xFF none), 4 f32 weights
  GX arrays (32-byte aligned): positions f32 x3, normals f32 x3, texture coordinates f32 x2 (the
       bind pose, model space)
  batches: 12 bytes each: u32 display list offset (from the lists' start), u32 size, u16 loads, u16
       first load. Before a batch's list, its loads: (u8 slot 0-3, u8 0, u16 set) each: the set's
       matrices into position/normal matrix slot `slot` (GX_PNMTX0 + 3 slot) and texture matrix
       GX_TEXMTX0 + 3 slot; the slots keep what earlier batches loaded
  display lists: GX_DRAW_TRIANGLES on vertex format 6, each vertex u8 PNMTXIDX (3 slot), u8
       TEX0MTXIDX (30 + 3 slot), u16 position, normal and texture coordinate indices (the same),
       wound as GX's front faces (clockwise), padded with NOPs to 32 bytes
  texture levels: GX RGB565 (4x4 tiles), 1024, 512, 256, 128 square, one after another
"""
import argparse
import json
import os
import struct

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
MESH = os.path.join(HERE, "midoha_outfit_mesh.json")
DEFAULT_PNG = os.path.join(HERE, "midoha_outfit.png")
# Matrix slots a batch may use. A set takes two of the 3DS GX path's palette entries (its position and
# its texture matrix, 3 of the palette's 42 rows each), and a draw has to fit the palette whole to be
# kept in its list cache; else its vertices are made again every frame, on the CPU (tools/armbench's
# GX path: 1,388 ARM11 cycles a vertex made, 82 a cached one's, plus 4,556 a cached draw): with 7 sets
# a batch (the palette exactly) none ever fitted, and the outfit's 11,970 vertices cost the 3DS's GPU
# thread about 21 ms a frame; with 4 (69 batches) about 1.6 ms. (Triangle strips would make fewer
# vertices but many more draws, which cost more.)
SLOTS = 4
LEVELS = 4              # 1024, 512, 256, 128
VTXFMT = 6
GX_DRAW_TRIANGLES = 0x90


def load_mesh(path=MESH):
    with open(path) as f:
        return json.load(f)


def flatten(mesh):
    """all pieces' vertices and triangles as arrays: pos, nrm, uv (n,), weights [(bone, w)], tris,
    the piece of each triangle"""
    pos, nrm, uv, wts, tris, piece = [], [], [], [], [], []
    for pi, pc in enumerate(mesh["pieces"]):
        base = len(pos)
        for p, n, t, w in pc["vertices"]:
            pos.append(p)
            nrm.append(n)
            uv.append(t)
            wts.append(tuple((int(b), float(x)) for b, x in w))
        for a, b, c in pc["triangles"]:
            tris.append((base + a, base + b, base + c))
            piece.append(pi)
    return np.array(pos, float), np.array(nrm, float), np.array(uv, float), wts, np.array(tris), np.array(piece)


def sets_of(wts):
    """the distinct weight sets and each vertex's set"""
    key = {}
    vset = []
    for w in wts:
        k = tuple(sorted(((b, round(x * 8)) for b, x in w), key=lambda e: (-e[1], e[0])))
        if k not in key:
            key[k] = len(key)
        vset.append(key[k])
    sets = [None] * len(key)
    for k, i in key.items():
        sets[i] = [(b, q / 8.0) for b, q in k]
    return sets, np.array(vset)


def batches(tris, vset, piece):
    """group the triangles (in their pieces' order) into batches of at most SLOTS sets, keeping the
    sets already in the slots where it can: [(loads [(slot, set)], triangles, {set: slot})]"""
    slots = [None] * SLOTS            # the set in each slot now
    last_use = [-1] * SLOTS
    out = []
    order = np.lexsort((np.arange(len(tris)), piece))
    pending = list(order)
    step = 0
    while pending:
        # start from the sets the slots hold: triangles needing only those first
        held = {s for s in slots if s is not None}
        cur_sets = set()
        cur, rest = [], []
        for t in pending:
            need = {int(vset[v]) for v in tris[t]}
            if len(cur_sets | need) <= SLOTS:
                cur_sets |= need
                cur.append(t)
            else:
                rest.append(t)
        holding = {s: i for i, s in enumerate(slots) if s is not None}
        free = [i for i in range(SLOTS) if slots[i] is None or slots[i] not in cur_sets]
        free.sort(key=lambda i: last_use[i])
        slot_of = {s: holding[s] for s in cur_sets if s in holding}
        loads = []
        for s in sorted(cur_sets):
            if s not in slot_of:
                i = free.pop(0)
                slots[i] = s
                slot_of[s] = i
                loads.append((i, s))
        for i in slot_of.values():
            last_use[i] = step
        out.append((loads, cur, dict(slot_of)))
        pending = rest
        step += 1
        del held
    return out


def display_list(tris, vset, slot_of):
    d = bytearray()
    d += struct.pack(">BH", GX_DRAW_TRIANGLES | VTXFMT, 3 * len(tris))
    for a, b, c in tris:
        for v in (a, c, b):           # GX's front faces wind the other way
            s = slot_of[int(vset[v])]
            d += struct.pack(">BBHHH", 3 * s, 30 + 3 * s, v, v, v)
    while len(d) % 32:
        d += b"\0"
    return bytes(d)


def gx_rgb565(img):
    a = np.asarray(img.convert("RGB"), np.uint16)
    v = (a[..., 0] >> 3) << 11 | (a[..., 1] >> 2) << 5 | (a[..., 2] >> 3)
    h, w = v.shape
    t = v.reshape(h // 4, 4, w // 4, 4).transpose(0, 2, 1, 3).reshape(-1)
    return t.astype(">u2").tobytes()


def levels(png, n=LEVELS):
    im = Image.open(png).convert("RGB")
    size = im.size[0]
    out = []
    for k in range(n):
        s = size >> k
        lv = im if k == 0 else im.resize((s, s), Image.BOX)
        out.append(gx_rgb565(lv))
    return size, out


def align(b, n=32):
    return b + b"\0" * (-len(b) % n)


def pack(mesh, png):
    pos, nrm, uv, wts, tris, piece = flatten(mesh)
    sets, vset = sets_of(wts)
    bt = batches(tris, vset, piece)
    bones = mesh["bones"]
    hdr_size = 0x60
    b_bones = b"".join(struct.pack("<16s12f", nm.encode()[:16], *mesh["inverse_bind"][i]) for i, nm in enumerate(bones))
    b_sets = b""
    for s in sets:
        bs = [b for b, _ in s] + [0xFF] * (4 - len(s))
        ws = [w for _, w in s] + [0.0] * (4 - len(s))
        b_sets += struct.pack("<4B4f", *bs, *ws)
    b_arrays = align(pos.astype(">f4").tobytes()) + align(nrm.astype(">f4").tobytes()) + align(uv.astype(">f4").tobytes())
    dls, b_batches, b_loads = b"", b"", b""
    n_loads = 0
    for loads, ts, slot_of in bt:
        dl = display_list(tris[ts], vset, slot_of)
        b_batches += struct.pack("<IIHH", len(dls), len(dl), len(loads), n_loads)
        for s, st in loads:
            b_loads += struct.pack("<BBH", s, 0, st)
        n_loads += len(loads)
        dls += dl
    size, lv = levels(png)
    b_tex = b"".join(lv)
    parts = [b_bones, b_sets, b_arrays, b_batches, b_loads, dls, b_tex]
    offs = []
    at = hdr_size
    body = b""
    for p in parts:
        at_al = (at + 31) & ~31
        body += b"\0" * (at_al - at)
        offs.append(at_al)
        body += p
        at = at_al + len(p)
    hdr = b"MDG1" + struct.pack("<IIIII", 1, len(bones), len(sets), len(pos), len(bt))
    hdr += struct.pack("<HHIf", size, size, len(lv), float(mesh["atlas"]["density"]))
    hdr += struct.pack("<5I", *offs[:5]) + struct.pack("<II", offs[5], len(dls)) + struct.pack("<II", offs[6], len(b_tex))
    hdr += struct.pack("<I", n_loads)
    hdr = hdr.ljust(hdr_size, b"\0")
    stats = dict(bones=len(bones), sets=len(sets), vertices=len(pos), triangles=len(tris), batches=len(bt),
                 loads=n_loads, dl_bytes=len(dls), array_bytes=len(b_arrays), texture_bytes=len(b_tex),
                 texture=size)
    return hdr + body, stats


def write(data_dir, png=DEFAULT_PNG, mesh_path=MESH):
    """(midoha_parts.py stages the outfit with this)"""
    out = os.path.join(data_dir, "wuhu", "midoha_outfit.bin")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    data, _ = pack(load_mesh(mesh_path), png)
    with open(out, "wb") as f:
        f.write(data)
    return out, len(data)


def template(out, mesh_path=MESH):
    mesh = load_mesh(mesh_path)
    size = mesh["atlas"]["size"]
    im = Image.new("RGB", (size, size), (250, 250, 250))
    d = ImageDraw.Draw(im)
    for pc in mesh["pieces"]:
        uv = np.array([v[2] for v in pc["vertices"]]) * size
        for a, b, c in pc["triangles"]:
            d.polygon([tuple(uv[a]), tuple(uv[b]), tuple(uv[c])], outline=(205, 205, 210))
    for name, (x, y, w, h) in mesh["atlas"]["islands"].items():
        d.rectangle([x, y, x + w, y + h], outline=(60, 60, 200))
        d.text((x + 3, y + 2), name, fill=(20, 20, 120))
    im.save(out)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("what", help="the data directory, or 'template'")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--png", default=DEFAULT_PNG)
    ap.add_argument("--mesh", default=MESH)
    a = ap.parse_args()
    if a.what == "template":
        print(template(a.out or "midoha_outfit_template.png", a.mesh))
        return
    out = os.path.join(a.what, "wuhu", "midoha_outfit.bin")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    data, stats = pack(load_mesh(a.mesh), a.png)
    with open(out, "wb") as f:
        f.write(data)
    print("%s: %d bytes; %s" % (out, len(data), ", ".join("%s %s" % kv for kv in stats.items())))


if __name__ == "__main__":
    main()
