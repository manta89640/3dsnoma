// GX textures into PICA texture memory (platform/3ds/gx_pica.cpp), header-only so that a host test and
// the ARM11 bench (tools/armbench --gx) build the same code.
//
// The PICA keeps a W x H texture (powers of two) in 8x8 tiles, the texels of a tile in Z order, the
// image upside down (its t runs from the last row up: row y is at H-1-y), little-endian: RGBA8 as
// 0xRRGGBBAA, LA8 0xLLAA, LA4 0xLA, RGB565, RGBA5551 (R in the top bits). An image smaller than W x H
// is padded with its edge texels (what GX's clamp samples beyond the edge); a repeating one is
// resampled to W x H instead.
//
// from_rgba: an image decoded to RGBA8 (runtime/hw/gx/texture.cpp), resampled or padded.
// from_gx: the GX texture's own bytes, tile by tile, straight into the PICA format: no RGBA8 image
// (half the passes over the texels, no temporary of w*h*4 bytes); for any format but the few that
// have to be resampled the same texture memory as from_rgba of the decoded image, byte for byte.
#pragma once
#include <cstring>
#include <vector>

#include "runtime/hw/gx/gx.h"

namespace guest::pica_tex {

enum Fmt : u8 { LA4, LA8, RGB565, RGBA5551, RGBA8 };

inline u32 texel_bytes(Fmt f) { return f == LA4 ? 1 : f == RGBA8 ? 4 : 2; }

// a texel's place: column part + row part (the row part of image row y, stored upside down)
struct Places {
    std::vector<u32> col, row;
    void make(u32 W, u32 H) {
        col.resize(W), row.resize(H);
        for (u32 x = 0; x < W; x++)
            col[x] = (x >> 3) * 64 + ((x & 1) | ((x & 2) << 1) | ((x & 4) << 2));
        for (u32 y = 0; y < H; y++) {
            u32 py = H - 1 - y;
            row[y] = (py >> 3) * (W >> 3) * 64 + (((py & 1) << 1) | ((py & 2) << 2) | ((py & 4) << 3));
        }
    }
};

// RGBA8 as texture.cpp decodes it (R in bits 0-7 ... A in 24-31) -> the PICA format's texel
inline u32 pack(Fmt f, u32 c) {
    switch (f) {
    case LA4: return (c & 0xF0) | (c >> 28);
    case LA8: return (c & 0xFF) << 8 | c >> 24;
    case RGB565: return (c & 0xF8) << 8 | (c >> 5 & 0x7E0) | (c >> 19 & 0x1F);
    case RGBA5551: return (c & 0xF8) << 8 | (c >> 5 & 0x7C0) | (c >> 18 & 0x3E) | c >> 31;
    default: return __builtin_bswap32(c);
    }
}

template <class T>
inline void put(u8* out, u32 i, u32 v) {
    ((T*)out)[i] = (T)v;
}

inline void from_rgba(const u32* rgba, u32 w, u32 h, u8* out, u32 W, u32 H, Fmt fmt, bool repeat) {
    static Places pl;
    pl.make(W, H);
    static std::vector<u32> col_src;
    col_src.resize(W);
    for (u32 x = 0; x < W; x++)
        col_src[x] = repeat ? x * w / W : (x < w - 1 ? x : w - 1);
    auto rows = [&](auto store) {
        for (u32 y = 0; y < H; y++) {
            u32 sy = repeat ? y * h / H : (y < h - 1 ? y : h - 1);
            const u32* src = rgba + sy * w;
            for (u32 x = 0; x < W; x++)
                store(pl.row[y] + pl.col[x], pack(fmt, src[col_src[x]]));
        }
    };
    switch (texel_bytes(fmt)) {
    case 1: rows([&](u32 i, u32 v) { put<u8>(out, i, v); }); break;
    case 2: rows([&](u32 i, u32 v) { put<u16>(out, i, v); }); break;
    default: rows([&](u32 i, u32 v) { put<u32>(out, i, v); }); break;
    }
}

namespace detail {

inline u32 rgba(u32 r, u32 g, u32 b, u32 a) { return r | g << 8 | b << 16 | a << 24; }
inline u32 x5(u32 v) { return (v << 3) | (v >> 2); }
inline u32 x6(u32 v) { return (v << 2) | (v >> 4); }
inline u32 x4(u32 v) { return v * 17; }
inline u32 x3(u32 v) { return (v << 5) | (v << 2) | (v >> 1); }
inline u32 be16(const u8* p) { return (u32)p[0] << 8 | p[1]; }
inline u32 rgb5a3(u32 v) {
    if (v & 0x8000)
        return rgba(x5((v >> 10) & 31), x5((v >> 5) & 31), x5(v & 31), 255);
    return rgba(x4((v >> 8) & 15), x4((v >> 4) & 15), x4(v & 15), x3((v >> 12) & 7));
}
// a palette entry (TMEM, big-endian) in the PICA format its texture is kept in
inline u32 tlut_texel(u32 v, u32 tlut_fmt) {
    switch (tlut_fmt) {
    case 0: return (v & 255) << 8 | v >> 8;  // IA8 -> LA8
    case 1: return v;                        // RGB565
    default: return __builtin_bswap32(rgb5a3(v));
    }
}

// the texels of one GX block, a row at a time: value(i) for texel i of the block (bw x bh)
template <class T, class Value>
inline void block(u8* out, const Places& pl, u32 x0, u32 y0, u32 bw, u32 bh, u32 w, u32 h, Value value) {
    const u32 nx = x0 + bw <= w ? bw : w - x0, ny = y0 + bh <= h ? bh : h - y0;
    for (u32 j = 0; j < ny; j++) {
        const u32 r = pl.row[y0 + j];
        const u32* c = &pl.col[x0];
        for (u32 i = 0; i < nx; i++)
            ((T*)out)[r + c[i]] = (T)value(j * bw + i);
    }
}

// the padding: x >= w gets the row's last texel, y >= h the last row
template <class T>
inline void pad(u8* out, const Places& pl, u32 w, u32 h, u32 W, u32 H) {
    T* o = (T*)out;
    if (w < W)
        for (u32 y = 0; y < h; y++) {
            const T v = o[pl.row[y] + pl.col[w - 1]];
            for (u32 x = w; x < W; x++)
                o[pl.row[y] + pl.col[x]] = v;
        }
    for (u32 y = h; y < H; y++)
        for (u32 x = 0; x < W; x++)
            o[pl.row[y] + pl.col[x]] = o[pl.row[h - 1] + pl.col[x]];
}

}  // namespace detail

// GX format gxfmt (w x h at src; palettes in tmem, at tlut_addr, mask tmem_mask) -> PICA format fmt
// (W x H, padded). false for a format this doesn't convert (the caller decodes it).
inline bool from_gx(const u8* src, u32 gxfmt, u32 w, u32 h, const u8* tmem, u32 tmem_mask, u32 tlut_addr,
                    u32 tlut_fmt, u8* out, u32 W, u32 H, Fmt fmt) {
    using namespace detail;
    static Places pl;
    pl.make(W, H);
    u32 bw, bh, bytes;
    switch (gxfmt) {
    case 0: case 8: case 14: bw = 8, bh = 8, bytes = 32; break;
    case 1: case 2: case 9: bw = 8, bh = 4, bytes = 32; break;
    case 6: bw = 4, bh = 4, bytes = 64; break;
    case 3: case 4: case 5: case 10: bw = 4, bh = 4, bytes = 32; break;
    default: return false;
    }
    // the palette in the texture's format (C4, C8; C14X2's is looked up per texel)
    static u32 pal[256];
    auto tlut_entry = [&](u32 index) {
        const u8* p = &tmem[(tlut_addr + 2 * index) & tmem_mask];
        return tlut_texel(be16(p), tlut_fmt);
    };
    if (gxfmt == 8 || gxfmt == 9)
        for (u32 i = 0; i < (gxfmt == 8 ? 16u : 256u); i++)
            pal[i] = tlut_entry(i);
    const u32 bx_n = (w + bw - 1) / bw, by_n = (h + bh - 1) / bh;
    for (u32 by = 0; by < by_n; by++) {
        for (u32 bx = 0; bx < bx_n; bx++, src += bytes) {
            const u32 x0 = bx * bw, y0 = by * bh;
            const u8* s = src;
            switch (gxfmt) {
            case 0:  // I4 -> LA4
                block<u8>(out, pl, x0, y0, 8, 8, w, h, [&](u32 i) {
                    u32 n = (s[i >> 1] >> ((i & 1) ? 0 : 4)) & 15;
                    return n << 4 | n;
                });
                break;
            case 1:  // I8 -> LA8
                block<u16>(out, pl, x0, y0, 8, 4, w, h, [&](u32 i) { return (u32)s[i] << 8 | s[i]; });
                break;
            case 2:  // IA4 -> LA4 (GX: alpha in the high nibble)
                block<u8>(out, pl, x0, y0, 8, 4, w, h, [&](u32 i) { return (u32)((s[i] << 4) | (s[i] >> 4)) & 0xFF; });
                break;
            case 3:  // IA8 -> LA8 (GX: alpha first)
                block<u16>(out, pl, x0, y0, 4, 4, w, h, [&](u32 i) { return (u32)s[2 * i + 1] << 8 | s[2 * i]; });
                break;
            case 4:  // RGB565
                block<u16>(out, pl, x0, y0, 4, 4, w, h, [&](u32 i) { return be16(s + 2 * i); });
                break;
            case 5:  // RGB5A3 -> RGBA8
                block<u32>(out, pl, x0, y0, 4, 4, w, h, [&](u32 i) { return __builtin_bswap32(rgb5a3(be16(s + 2 * i))); });
                break;
            case 6:  // RGBA8: AR pairs, then GB pairs
                block<u32>(out, pl, x0, y0, 4, 4, w, h, [&](u32 i) {
                    return (u32)s[2 * i] | (u32)s[32 + 2 * i + 1] << 8 | (u32)s[32 + 2 * i] << 16 | (u32)s[2 * i + 1] << 24;
                });
                break;
            case 8:  // C4
            case 9:  // C8
            case 10: {  // C14X2
                auto idx = [&](u32 i) -> u32 {
                    if (gxfmt == 8)
                        return pal[(s[i >> 1] >> ((i & 1) ? 0 : 4)) & 15];
                    if (gxfmt == 9)
                        return pal[s[i]];
                    return tlut_entry(be16(s + 2 * i) & 0x3FFF);
                };
                if (fmt == RGBA8)
                    block<u32>(out, pl, x0, y0, bw, bh, w, h, idx);
                else
                    block<u16>(out, pl, x0, y0, bw, bh, w, h, idx);
                break;
            }
            case 14:  // CMPR -> RGBA5551: four 4x4 sub-blocks, each a two-color palette and 2-bit indices
                for (u32 sb = 0; sb < 4; sb++) {
                    const u8* q = s + 8 * sb;
                    const u32 sx = x0 + (sb & 1) * 4, sy = y0 + (sb >> 1) * 4;
                    if (sx >= w || sy >= h)
                        continue;
                    u32 c0 = be16(q), c1 = be16(q + 2);
                    u32 r0 = x5(c0 >> 11), g0 = x6((c0 >> 5) & 63), b0 = x5(c0 & 31);
                    u32 r1 = x5(c1 >> 11), g1 = x6((c1 >> 5) & 63), b1 = x5(c1 & 31);
                    u32 p[4];
                    p[0] = rgba(r0, g0, b0, 255), p[1] = rgba(r1, g1, b1, 255);
                    if (c0 > c1) {
                        p[2] = rgba((r1 * 3 + r0 * 5) >> 3, (g1 * 3 + g0 * 5) >> 3, (b1 * 3 + b0 * 5) >> 3, 255);
                        p[3] = rgba((r0 * 3 + r1 * 5) >> 3, (g0 * 3 + g1 * 5) >> 3, (b0 * 3 + b1 * 5) >> 3, 255);
                    } else {
                        p[2] = rgba((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 255);
                        p[3] = rgba((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 0);
                    }
                    for (u32 k = 0; k < 4; k++)
                        p[k] = pack(RGBA5551, p[k]);
                    block<u16>(out, pl, sx, sy, 4, 4, w, h, [&](u32 i) { return p[(q[4 + (i >> 2)] >> (6 - 2 * (i & 3))) & 3]; });
                }
                break;
            }
        }
    }
    switch (texel_bytes(fmt)) {
    case 1: pad<u8>(out, pl, w, h, W, H); break;
    case 2: pad<u16>(out, pl, w, h, W, H); break;
    default: pad<u32>(out, pl, w, h, W, H); break;
    }
    return true;
}

}  // namespace guest::pica_tex
