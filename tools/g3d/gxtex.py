"""GX (Wii/GameCube) texture formats: sizes and decoding to RGBA8, with numpy.

    rgba = decode(data, width, height, fmt, palette=None)   # (h, w, 4) uint8, row 0 at the top
    pal = decode_palette(raw, plt_fmt)                       # (n, 4) uint8 (0 IA8, 1 RGB565, 2 RGB5A3)
    n = level_bytes(fmt, w, h)                               # bytes of one level as GX stores it

Formats (GX's numbers): 0 I4, 1 I8, 2 IA4, 3 IA8, 4 RGB565, 5 RGB5A3, 6 RGBA8, 8 C4, 9 C8,
10 C14X2 (paletted: give the palette), 14 CMPR (S3TC/DXT1 blocks, four to an 8x8 tile). GX keeps a
texture in tiles of BLOCK[fmt] texels, row by row, the texels of a tile row by row; a level's size is
padded up to whole tiles. Intensity formats are returned as GX's TEV sees them: I4/I8 with alpha =
intensity, IA4/IA8 with their own alpha.

Checked against the 2,479 textures of Wii Sports Resort's Stage and Common archives (every one decodes;
the renders of tools/g3d/preview.py use it) and Wii no Ma's own.
"""
import numpy as np

FORMATS = {0: "I4", 1: "I8", 2: "IA4", 3: "IA8", 4: "RGB565", 5: "RGB5A3", 6: "RGBA8",
           8: "C4", 9: "C8", 10: "C14X2", 14: "CMPR"}
# tile size (w, h) in texels, and bits a texel
BLOCK = {0: (8, 8), 1: (8, 4), 2: (8, 4), 3: (4, 4), 4: (4, 4), 5: (4, 4),
         6: (4, 4), 8: (8, 8), 9: (8, 4), 10: (4, 4), 14: (8, 8)}
BPP = {0: 4, 1: 8, 2: 8, 3: 16, 4: 16, 5: 16, 6: 32, 8: 4, 9: 8, 10: 16, 14: 4}
PALETTED = (8, 9, 10)


def level_bytes(fmt, w, h):
    """Bytes of one w x h level in GX format `fmt` (padded to whole tiles)."""
    bw, bh = BLOCK[fmt]
    pw, ph = -(-w // bw) * bw, -(-h // bh) * bh
    return pw * ph * BPP[fmt] // 8


def _untile(px, w, h, bw, bh):
    """px (tiles, bh*bw, C) in tile order -> (h, w, C), the padding cropped."""
    pw, ph = -(-w // bw) * bw, -(-h // bh) * bh
    nbx, nby = pw // bw, ph // bh
    c = px.shape[-1]
    img = px.reshape(nby, nbx, bh, bw, c).transpose(0, 2, 1, 3, 4).reshape(ph, pw, c)
    return img[:h, :w]


def _rgb565(v):
    v = v.astype(np.uint32)
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.uint8)


def _rgb5a3(v):
    """RGB5A3: top bit set = RGB555 opaque, clear = ARGB3444."""
    v = v.astype(np.uint32)
    out = np.empty(v.shape + (4,), np.uint8)
    op = (v & 0x8000) != 0
    r5, g5, b5 = (v >> 10) & 31, (v >> 5) & 31, v & 31
    r4, g4, b4, a3 = (v >> 8) & 15, (v >> 4) & 15, v & 15, (v >> 12) & 7
    out[..., 0] = np.where(op, (r5 << 3) | (r5 >> 2), r4 * 17)
    out[..., 1] = np.where(op, (g5 << 3) | (g5 >> 2), g4 * 17)
    out[..., 2] = np.where(op, (b5 << 3) | (b5 >> 2), b4 * 17)
    out[..., 3] = np.where(op, 255, (a3 << 5) | (a3 << 2) | (a3 >> 1))
    return out


def decode_palette(raw, fmt):
    """A PLT0/TLUT palette (big-endian u16 entries) -> (n, 4) RGBA8."""
    v = np.frombuffer(raw, ">u2")
    if fmt == 0:      # IA8: high byte alpha, low byte intensity
        i = (v & 0xFF).astype(np.uint8)
        a = (v >> 8).astype(np.uint8)
        return np.stack([i, i, i, a], -1)
    if fmt == 1:
        rgb = _rgb565(v)
        return np.concatenate([rgb, np.full(rgb.shape[:-1] + (1,), 255, np.uint8)], -1)
    return _rgb5a3(v)


def _pal(idx, palette):
    if palette is None:            # no palette: the indices as grey
        i = (idx.astype(np.float32) * (255.0 / max(1, int(idx.max())))).astype(np.uint8)
        return np.stack([i, i, i, np.full_like(i, 255)], -1)
    return palette[np.minimum(idx, len(palette) - 1)]


def decode(data, w, h, fmt, palette=None):
    """One level of GX texels -> (h, w, 4) uint8 RGBA."""
    bw, bh = BLOCK[fmt]
    pw, ph = -(-w // bw) * bw, -(-h // bh) * bh
    nb = (pw // bw) * (ph // bh)
    raw = np.frombuffer(data, np.uint8, level_bytes(fmt, w, h))
    if fmt == 14:
        return _cmpr(raw, w, h)
    if fmt in (0, 8):                                  # 4 bits a texel, high nibble first
        v = np.stack([raw >> 4, raw & 15], -1).reshape(nb, bw * bh)
        if fmt == 0:
            i = (v * 17).astype(np.uint8)
            px = np.stack([i, i, i, i], -1)
        else:
            px = _pal(v, palette)
    elif fmt in (1, 2, 9):                             # 8 bits
        v = raw.reshape(nb, bw * bh)
        if fmt == 1:
            px = np.stack([v, v, v, v], -1)
        elif fmt == 2:                                 # IA4: alpha in the high nibble
            i = ((v & 15) * 17).astype(np.uint8)
            a = ((v >> 4) * 17).astype(np.uint8)
            px = np.stack([i, i, i, a], -1)
        else:
            px = _pal(v, palette)
    elif fmt == 3:                                     # IA8: alpha byte first
        v = raw.reshape(nb, bw * bh, 2)
        a, i = v[..., 0], v[..., 1]
        px = np.stack([i, i, i, a], -1)
    elif fmt in (4, 5, 10):
        v = raw.view(">u2").reshape(nb, bw * bh)
        if fmt == 4:
            rgb = _rgb565(v)
            px = np.concatenate([rgb, np.full(rgb.shape[:-1] + (1,), 255, np.uint8)], -1)
        elif fmt == 5:
            px = _rgb5a3(v)
        else:
            px = _pal(v & 0x3FFF, palette)
    elif fmt == 6:                                     # RGBA8: a tile's AR pairs, then its GB pairs
        v = raw.reshape(nb, 2, 16, 2)
        a, r = v[:, 0, :, 0], v[:, 0, :, 1]
        g, b = v[:, 1, :, 0], v[:, 1, :, 1]
        px = np.stack([r, g, b, a], -1)
    else:
        raise ValueError("GX texture format %d" % fmt)
    return np.ascontiguousarray(_untile(px, w, h, bw, bh))


def _cmpr_blocks(raw, w, h):
    """CMPR as (tile rows, tile cols, 2, 2, 8 bytes): four DXT1 blocks to an 8x8 tile."""
    pw, ph = -(-w // 8) * 8, -(-h // 8) * 8
    return raw.reshape(ph // 8, pw // 8, 2, 2, 8)


def _cmpr(raw, w, h):
    b = _cmpr_blocks(raw, w, h)
    nby, nbx = b.shape[:2]
    c0 = (b[..., 0].astype(np.uint32) << 8) | b[..., 1]
    c1 = (b[..., 2].astype(np.uint32) << 8) | b[..., 3]
    p0 = _rgb565(c0).astype(np.int32)
    p1 = _rgb565(c1).astype(np.int32)
    gt = (c0 > c1)[..., None]
    # the in-between colours at GX's 5:3 / 3:5 weights (runtime/hw/gx/texture.cpp, pica_tex.h
    # cmpr_palette); three-colour blocks (c0 <= c1) have the average and transparent black
    p2 = np.where(gt, (p0 * 5 + p1 * 3) >> 3, (p0 + p1) // 2)
    p3 = np.where(gt, (p0 * 3 + p1 * 5) >> 3, (p0 + p1) // 2)
    pal = np.stack([p0, p1, p2, p3], -2)
    alpha = np.full(pal.shape[:-1] + (1,), 255, np.int32)
    alpha[..., 3, 0] = np.where(gt[..., 0], 255, 0)    # c0 <= c1: index 3 is transparent black
    pal = np.concatenate([pal, alpha], -1).astype(np.uint8)
    rows = b[..., 4:8]
    sh = np.array([6, 4, 2, 0], np.uint8)
    idx = ((rows[..., :, None] >> sh) & 3).reshape(nby, nbx, 2, 2, 16)
    px = np.take_along_axis(pal, idx[..., None].astype(np.intp), axis=-2)
    img = px.reshape(nby, nbx, 2, 2, 4, 4, 4).transpose(0, 2, 4, 1, 3, 5, 6).reshape(nby * 8, nbx * 8, 4)
    return np.ascontiguousarray(img[:h, :w])


def cmpr_has_alpha(data, w, h):
    """True if a CMPR level has a transparent texel: a block in three-colour mode (c0 <= c1) that
    uses index 3. Whole blocks count, padding included, exactly as platform/3ds/pica_tex.h
    cmpr_has_alpha decides between ETC1A4 and ETC1."""
    b = _cmpr_blocks(np.frombuffer(data, np.uint8, level_bytes(14, w, h)), w, h)
    c0 = (b[..., 0].astype(np.uint32) << 8) | b[..., 1]
    c1 = (b[..., 2].astype(np.uint32) << 8) | b[..., 3]
    idx = (b[..., 4:8][..., :, None] >> np.array([6, 4, 2, 0], np.uint8)) & 3
    return bool(((c0 <= c1) & (idx == 3).any(axis=(-1, -2))).any())
