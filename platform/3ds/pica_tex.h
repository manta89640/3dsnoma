// GX textures into PICA texture memory (platform/3ds/gx_pica.cpp), header-only so that a host test and
// the ARM11 bench (tools/armbench --gx) build the same code.
//
// The PICA keeps a W x H texture (powers of two) in 8x8 tiles, the texels of a tile in Z order, the
// image upside down (its t runs from the last row up: row y is at H-1-y), little-endian: RGBA8 as
// 0xRRGGBBAA, LA8 0xLLAA, LA4 0xLA, RGB565, RGBA5551 (R in the top bits), L8, and L4 two texels to a
// byte (the first in the low nibble). An image smaller than W x H
// is padded with its edge texels (what GX's clamp samples beyond the edge); a repeating one is
// resampled to W x H instead.
//
// from_rgba: an image decoded to RGBA8 (runtime/hw/gx/texture.cpp), resampled or padded.
// from_gx: the GX texture's own bytes, tile by tile, straight into the PICA format: no RGBA8 image
// (half the passes over the texels, no temporary of w*h*4 bytes); for any format but the few that
// have to be resampled the same texture memory as from_rgba of the decoded image, byte for byte.
#pragma once
#include <algorithm>
#include <cstring>

#include "runtime/hw/gx/gx.h"

namespace guest::pica_tex {

enum Fmt : u8 { LA4, LA8, RGB565, RGBA5551, RGBA8, L8, L4, ETC1, ETC1A4 };

inline u32 texel_bits(Fmt f) { return f == L4 || f == ETC1 ? 4 : f == LA4 || f == L8 || f == ETC1A4 ? 8 : f == RGBA8 ? 32 : 16; }

// a 4-bit texel's store: two to a byte
struct Nib {};
template <class T>
inline void put(u8* out, u32 i, u32 v) {
    ((T*)out)[i] = (T)v;
}
template <>
inline void put<Nib>(u8* out, u32 i, u32 v) {
    u8& b = out[i >> 1];
    b = (i & 1) ? (u8)((b & 0x0F) | (v << 4)) : (u8)((b & 0xF0) | (v & 0xF));
}
template <class T>
inline u32 get(const u8* out, u32 i) {
    return ((const T*)out)[i];
}
template <>
inline u32 get<Nib>(const u8* out, u32 i) {
    return (i & 1) ? out[i >> 1] >> 4 : out[i >> 1] & 0xF;
}

// a texel's place: column part + row part (the row part of image row y, stored upside down);
// textures are at most 1024 x 1024
struct Places {
    u32 col[1024], row[1024];
    void make(u32 W, u32 H) {
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
    case L8: return c & 0xFF;
    case L4: return (c & 0xF0) >> 4;
    case LA8: return (c & 0xFF) << 8 | c >> 24;
    case RGB565: return (c & 0xF8) << 8 | (c >> 5 & 0x7E0) | (c >> 19 & 0x1F);
    case RGBA5551: return (c & 0xF8) << 8 | (c >> 5 & 0x7C0) | (c >> 18 & 0x3E) | c >> 31;
    default: return __builtin_bswap32(c);
    }
}

inline void from_rgba(const u32* rgba, u32 w, u32 h, u8* out, u32 W, u32 H, Fmt fmt, bool repeat) {
    static Places pl;
    pl.make(W, H);
    static u32 col_src[1024];
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
    switch (texel_bits(fmt)) {
    case 4: rows([&](u32 i, u32 v) { put<Nib>(out, i, v); }); break;
    case 8: rows([&](u32 i, u32 v) { put<u8>(out, i, v); }); break;
    case 16: rows([&](u32 i, u32 v) { put<u16>(out, i, v); }); break;
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
            put<T>(out, r + c[i], value(j * bw + i));
    }
}

// the padding: x >= w gets the row's last texel, y >= h the last row
template <class T>
inline void pad(u8* out, const Places& pl, u32 w, u32 h, u32 W, u32 H) {
    if (w < W)
        for (u32 y = 0; y < h; y++) {
            const u32 v = get<T>(out, pl.row[y] + pl.col[w - 1]);
            for (u32 x = w; x < W; x++)
                put<T>(out, pl.row[y] + pl.col[x], v);
        }
    for (u32 y = h; y < H; y++)
        for (u32 x = 0; x < W; x++)
            put<T>(out, pl.row[y] + pl.col[x], get<T>(out, pl.row[h - 1] + pl.col[x]));
}

}  // namespace detail

// ---- CMPR (GX's DXT1) as ETC1 or ETC1A4: a quarter (half with alpha) of RGBA5551, lossy. Each 8x8
// tile holds four 4x4 blocks (left to right, then down), each a little-endian 64-bit ETC1 word
// (ETC1A4: after a 64-bit word of 4-bit alphas); texel i = 4 * x + y within the block, its index
// bits at i (which of the table's two magnitudes) and 16 + i (negated). Encoded per block: the
// flip that splits its colors best, base colors differential where they are close enough (5 bits)
// else individual (4 bits), and for each half the modifier table with the least error over its
// (few: DXT1 blocks have four colors) distinct colors.
namespace etc1 {

const int MOD[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};
inline int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
// an 8-bit channel to 5 and 4 bits, rounded (tables: the ARM11 has no divide)
struct Quant {
    u8 q5[256], q4[256];
    constexpr Quant() : q5(), q4() {
        for (int v = 0; v < 256; v++)
            q5[v] = (u8)((v * 31 + 127) / 255), q4[v] = (u8)((v * 15 + 127) / 255);
    }
};
constexpr Quant QUANT;
inline int ch(u32 c, int k) { return (int)((c >> (8 * k)) & 255); }

// a half's texels (indices into the block) around base b[3]: the table, and per texel its code (bit
// 0: the larger magnitude, bit 1: negated). The modifiers add one value to all three channels, so a
// texel is matched by its summed offset from the base (clamping left out); only the tables around
// the one whose range fits the half's largest offset are tried
inline void fit_half(const u32* px, const u8* texels, const int* b, int* table, u8* code) {
    int sv[8], nd = 0, which[8], cnt[8], maxs = 0;
    for (int i = 0; i < 8; i++) {
        const u32 c = px[texels[i]];
        const int v = ch(c, 0) + ch(c, 1) + ch(c, 2) - b[0] - b[1] - b[2];
        int d = 0;
        while (d < nd && sv[d] != v)
            d++;
        if (d == nd)
            sv[nd] = v, cnt[nd++] = 0;
        cnt[d]++, which[i] = d;
        maxs = std::max(maxs, v < 0 ? -v : v);
    }
    int t0 = 0;
    while (t0 < 7 && 3 * MOD[t0][1] < maxs)
        t0++;
    u32 best = ~0u;
    u8 dcode[8], best_code[8] = {};
    for (int t = std::max(0, t0 - 1); t <= std::min(7, t0 + 1); t++) {
        const int a3 = 3 * MOD[t][0], b3 = 3 * MOD[t][1];
        u32 err = 0;
        for (int d = 0; d < nd; d++) {
            const int m = sv[d] < 0 ? -sv[d] : sv[d];
            const int ea = m - a3, eb = m - b3;
            const bool large = eb * eb < ea * ea;
            dcode[d] = (u8)((large ? 1 : 0) | (sv[d] < 0 ? 2 : 0));
            err += (u32)(large ? eb * eb : ea * ea) * (u32)cnt[d];
        }
        if (err < best) {
            best = err, *table = t;
            for (int d = 0; d < nd; d++)
                best_code[d] = dcode[d];
        }
    }
    for (int i = 0; i < 8; i++)
        code[i] = best_code[which[i]];
}

// one block: px[16] (0xAABBGGRR, texel i = 4 * x + y) -> the ETC1 word
inline u64 encode(const u32* px) {
    // the halves by x (flip 0) or by y (flip 1): the split whose halves' averages differ the most
    u8 half[2][2][8];
    int sum[2][2][3] = {};
    for (int f = 0; f < 2; f++) {
        int n[2] = {0, 0};
        for (int i = 0; i < 16; i++) {
            const int hh = ((f ? (i & 3) : (i >> 2)) >= 2);
            half[f][hh][n[hh]++] = (u8)i;
            for (int c = 0; c < 3; c++)
                sum[f][hh][c] += ch(px[i], c);
        }
    }
    int sep[2];
    for (int f = 0; f < 2; f++) {
        sep[f] = 0;
        for (int c = 0; c < 3; c++) {
            const int d = sum[f][1][c] - sum[f][0][c];
            sep[f] += d < 0 ? -d : d;
        }
    }
    const int flip = sep[1] > sep[0] ? 1 : 0;
    // base colors: 5-bit with a 3-bit difference when it fits, else 4-bit each
    int q5[2][3], q4[2][3];
    bool diff = true;
    for (int hh = 0; hh < 2; hh++)
        for (int c = 0; c < 3; c++) {
            const int avg = (sum[flip][hh][c] + 4) >> 3;
            q5[hh][c] = QUANT.q5[avg];
            q4[hh][c] = QUANT.q4[avg];
        }
    for (int c = 0; c < 3; c++) {
        const int d = q5[1][c] - q5[0][c];
        diff &= d >= -4 && d <= 3;
    }
    int base[2][3];
    for (int hh = 0; hh < 2; hh++)
        for (int c = 0; c < 3; c++)
            base[hh][c] = diff ? (q5[hh][c] << 3 | q5[hh][c] >> 2) : q4[hh][c] * 17;
    int table[2];
    u8 code[2][8];
    for (int hh = 0; hh < 2; hh++)
        fit_half(px, half[flip][hh], base[hh], &table[hh], code[hh]);
    u64 v = (u64)flip << 32 | (u64)diff << 33 | (u64)table[1] << 34 | (u64)table[0] << 37;
    for (int hh = 0; hh < 2; hh++)
        for (int i = 0; i < 8; i++) {
            const int t = half[flip][hh][i];
            v |= (u64)(code[hh][i] & 1) << t | (u64)(code[hh][i] >> 1) << (16 + t);
        }
    if (diff) {
        for (int c = 0; c < 3; c++) {
            const int at = 40 + 8 * c;  // b at 40, g at 48, r at 56
            v |= (u64)q5[0][2 - c] << (at + 3) | (u64)((q5[1][2 - c] - q5[0][2 - c]) & 7) << at;
        }
    } else {
        for (int c = 0; c < 3; c++) {
            const int at = 40 + 8 * c;
            v |= (u64)q4[0][2 - c] << (at + 4) | (u64)q4[1][2 - c] << at;
        }
    }
    return v;
}

// one block whose texels are four palette colors (a CMPR sub-block): mask[k] the texels (bit i, i =
// 4 * x + y) with palette entry k; the same choices as encode() made per entry instead of per texel
struct Popcount {
    u8 n[256];
    constexpr Popcount() : n() {
        for (int v = 0; v < 256; v++)
            n[v] = (u8)((v & 1) + ((v >> 1) & 1) + ((v >> 2) & 1) + ((v >> 3) & 1) + ((v >> 4) & 1) + ((v >> 5) & 1) +
                        ((v >> 6) & 1) + ((v >> 7) & 1));
    }
};
constexpr Popcount POPCOUNT;
inline int pop16(u32 m) { return POPCOUNT.n[m & 255] + POPCOUNT.n[(m >> 8) & 255]; }

inline u64 encode_palette(const u32* pal, const u16* mask) {
    // the halves as texel masks: flip 0 by x (texels 0-7 | 8-15), flip 1 by y (y < 2 | y >= 2)
    static const u16 HALF[2][2] = {{0x00FF, 0xFF00}, {0x3333, 0xCCCC}};
    int pc[4][3];
    for (int k = 0; k < 4; k++)
        pc[k][0] = (int)(pal[k] & 255), pc[k][1] = (int)((pal[k] >> 8) & 255), pc[k][2] = (int)((pal[k] >> 16) & 255);
    int cnt[2][2][4], sum[2][2][3];
    for (int f = 0; f < 2; f++)
        for (int hh = 0; hh < 2; hh++) {
            int r = 0, g = 0, b = 0;
            for (int k = 0; k < 4; k++) {
                const int n = pop16(mask[k] & HALF[f][hh]);
                cnt[f][hh][k] = n;
                r += n * pc[k][0], g += n * pc[k][1], b += n * pc[k][2];
            }
            sum[f][hh][0] = r, sum[f][hh][1] = g, sum[f][hh][2] = b;
        }
    int sep[2];
    for (int f = 0; f < 2; f++) {
        sep[f] = 0;
        for (int c = 0; c < 3; c++) {
            const int d = sum[f][1][c] - sum[f][0][c];
            sep[f] += d < 0 ? -d : d;
        }
    }
    const int flip = sep[1] > sep[0] ? 1 : 0;
    int q5[2][3], q4[2][3];
    bool diff = true;
    for (int hh = 0; hh < 2; hh++)
        for (int c = 0; c < 3; c++) {
            const int avg = (sum[flip][hh][c] + 4) >> 3;
            q5[hh][c] = QUANT.q5[avg];
            q4[hh][c] = QUANT.q4[avg];
        }
    for (int c = 0; c < 3; c++) {
        const int d = q5[1][c] - q5[0][c];
        diff &= d >= -4 && d <= 3;
    }
    int table[2];
    u32 lsb = 0, msb = 0;
    for (int hh = 0; hh < 2; hh++) {
        int bsum = 0;
        for (int c = 0; c < 3; c++)
            bsum += diff ? (q5[hh][c] << 3 | q5[hh][c] >> 2) : q4[hh][c] * 17;
        int sv[4], maxs = 0;
        for (int k = 0; k < 4; k++) {
            sv[k] = pc[k][0] + pc[k][1] + pc[k][2] - bsum;
            if (cnt[flip][hh][k])
                maxs = std::max(maxs, sv[k] < 0 ? -sv[k] : sv[k]);
        }
        int t0 = 0;
        while (t0 < 7 && 3 * MOD[t0][1] < maxs)
            t0++;
        u32 best = ~0u;
        u32 large_best = 0;
        for (int t = t0 > 0 ? t0 - 1 : 0; t <= t0; t++) {
            const int a3 = 3 * MOD[t][0], b3 = 3 * MOD[t][1];
            u32 err = 0, large_set = 0;
            for (int k = 0; k < 4; k++) {
                const int m = sv[k] < 0 ? -sv[k] : sv[k];
                const int ea = m - a3, eb = m - b3;
                const bool large = eb * eb < ea * ea;
                large_set |= (u32)large << k;
                err += (u32)(large ? eb * eb : ea * ea) * (u32)cnt[flip][hh][k];
            }
            if (err < best)
                best = err, table[hh] = t, large_best = large_set;
        }
        const u32 half = HALF[flip][hh];
        for (int k = 0; k < 4; k++) {
            const u32 texels = mask[k] & half;
            if (large_best >> k & 1)
                lsb |= texels;
            if (sv[k] < 0)
                msb |= texels;
        }
    }
    u64 v = (u64)flip << 32 | (u64)diff << 33 | (u64)table[1] << 34 | (u64)table[0] << 37 | (u64)lsb |
            (u64)msb << 16;
    if (diff) {
        for (int c = 0; c < 3; c++) {
            const int at = 40 + 8 * c;
            v |= (u64)q5[0][2 - c] << (at + 3) | (u64)((q5[1][2 - c] - q5[0][2 - c]) & 7) << at;
        }
    } else {
        for (int c = 0; c < 3; c++) {
            const int at = 40 + 8 * c;
            v |= (u64)q4[0][2 - c] << (at + 4) | (u64)q4[1][2 - c] << at;
        }
    }
    return v;
}

}  // namespace etc1

// a CMPR sub-block's (4x4) palette, as texture.cpp decodes it (0xAABBGGRR)
inline void cmpr_palette(const u8* q, u32 p[4]) {
    const u32 c0 = (u32)q[0] << 8 | q[1], c1 = (u32)q[2] << 8 | q[3];
    auto x5 = [](u32 v) { return (v << 3) | (v >> 2); };
    auto x6 = [](u32 v) { return (v << 2) | (v >> 4); };
    const u32 r0 = x5(c0 >> 11), g0 = x6((c0 >> 5) & 63), b0 = x5(c0 & 31);
    const u32 r1 = x5(c1 >> 11), g1 = x6((c1 >> 5) & 63), b1 = x5(c1 & 31);
    auto rgba = [](u32 r, u32 g, u32 b, u32 a) { return r | g << 8 | b << 16 | a << 24; };
    p[0] = rgba(r0, g0, b0, 255), p[1] = rgba(r1, g1, b1, 255);
    if (c0 > c1) {
        p[2] = rgba((r1 * 3 + r0 * 5) >> 3, (g1 * 3 + g0 * 5) >> 3, (b1 * 3 + b0 * 5) >> 3, 255);
        p[3] = rgba((r0 * 3 + r1 * 5) >> 3, (g0 * 3 + g1 * 5) >> 3, (b0 * 3 + b1 * 5) >> 3, 255);
    } else {
        p[2] = rgba((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 255);
        p[3] = rgba((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 0);
    }
}
// the CMPR sub-block holding image texel (x, y) of a w-wide texture
inline const u8* cmpr_sub_block(const u8* src, u32 w, u32 x, u32 y) {
    return src + ((y / 8) * ((w + 7) / 8) + x / 8) * 32 + 8 * (((x / 4) & 1) + 2 * ((y / 4) & 1));
}
inline u32 cmpr_index(const u8* q, u32 x, u32 y) { return (q[4 + (y & 3)] >> (6 - 2 * (x & 3))) & 3; }

// whether a CMPR texture has transparent texels (a block in three-color mode using index 3)
inline bool cmpr_has_alpha(const u8* src, u32 w, u32 h) {
    const u32 blocks = ((w + 7) / 8) * ((h + 7) / 8) * 4;
    for (u32 b = 0; b < blocks; b++, src += 8) {
        if (((u32)src[0] << 8 | src[1]) > ((u32)src[2] << 8 | src[3]))
            continue;
        for (int r = 0; r < 4; r++)
            for (int k = 0; k < 4; k++)
                if (((src[4 + r] >> (6 - 2 * k)) & 3) == 3)
                    return true;
    }
    return false;
}

// CMPR (w x h) as ETC1 / ETC1A4 of W x H, padded with its edge texels (a block inside the image is
// one CMPR sub-block: the image is stored upside down, and 4-row blocks stay aligned). Padding
// blocks repeat the edge: past the right edge a row's blocks are all alike, past the bottom every
// row is the last one's, so those are encoded once.
inline void etc1_from_cmpr(const u8* src, u32 w, u32 h, u8* out, u32 W, u32 H, bool alpha) {
    const u32 block_bytes = alpha ? 16 : 8, tiles_x = W / 8;
    static u64 below[256][2];  // the blocks of a row wholly below the image, by block column
    bool below_made = false;
    for (u32 my = 0; my < H; my += 4) {
        const u32 iy_top = H - 4 - my;  // the image row of the block's last memory row
        const bool all_below = H - 1 - my >= h && H - 1 - (my + 3) >= h;  // (every row clamps to h - 1)
        u64 right[2];
        bool right_made = false;
        for (u32 mx = 0; mx < W; mx += 4) {
            const u32 tile = (my / 8) * tiles_x + mx / 8, sub = ((mx / 4) & 1) + 2 * ((my / 4) & 1);
            u8* o = out + (tile * 4 + sub) * block_bytes;
            u64 words[2];  // alpha (ETC1A4), color
            if (all_below && below_made) {
                memcpy(words, below[mx / 4], 16);
            } else if (mx >= w && right_made) {
                memcpy(words, right, 16);
            } else if (mx + 3 < w && iy_top + 3 < h) {
                const u8* q = cmpr_sub_block(src, w, mx, iy_top);
                u32 p[4];
                cmpr_palette(q, p);
                // the texels of each palette entry: CMPR rows are 4 2-bit indices, x from the top
                // bits; block row y (memory) is image row 3 - y
                u16 mask[4] = {0, 0, 0, 0};
                for (u32 y = 0; y < 4; y++) {
                    const u32 bits = q[4 + 3 - y];
                    for (u32 x = 0; x < 4; x++)
                        mask[(bits >> (6 - 2 * x)) & 3] |= (u16)(1u << (4 * x + y));
                }
                words[0] = 0;
                if (alpha)
                    for (u32 k = 0; k < 4; k++)
                        if (p[k] >> 28)
                            for (u32 i = 0; i < 16; i++)
                                if (mask[k] >> i & 1)
                                    words[0] |= (u64)(p[k] >> 28) << (4 * i);
                words[1] = etc1::encode_palette(p, mask);
            } else {
                u32 px[16];
                for (u32 x = 0; x < 4; x++)
                    for (u32 y = 0; y < 4; y++) {
                        u32 iy = H - 1 - (my + y), ix = mx + x;
                        iy = iy < h ? iy : h - 1, ix = ix < w ? ix : w - 1;
                        const u8* q = cmpr_sub_block(src, w, ix, iy);
                        u32 p[4];
                        cmpr_palette(q, p);
                        px[4 * x + y] = p[cmpr_index(q, ix, iy)];
                    }
                words[0] = 0;
                if (alpha)
                    for (u32 i = 0; i < 16; i++)
                        words[0] |= (u64)(px[i] >> 28) << (4 * i);
                words[1] = etc1::encode(px);
            }
            if (mx >= w && !right_made)
                memcpy(right, words, 16), right_made = true;
            if (all_below && !below_made)
                memcpy(below[mx / 4], words, 16);
            if (alpha)
                memcpy(o, &words[0], 8), o += 8;
            memcpy(o, &words[1], 8);
        }
        if (all_below)
            below_made = true;
    }
}

// GX format gxfmt (w x h at src; palettes in tmem, at tlut_addr, mask tmem_mask) -> PICA format fmt
// (W x H, padded). false for a format this doesn't convert (the caller decodes it).
inline bool from_gx(const u8* src, u32 gxfmt, u32 w, u32 h, const u8* tmem, u32 tmem_mask, u32 tlut_addr,
                    u32 tlut_fmt, u8* out, u32 W, u32 H, Fmt fmt) {
    using namespace detail;
    if (gxfmt == 14 && (fmt == ETC1 || fmt == ETC1A4)) {
        etc1_from_cmpr(src, w, h, out, W, H, fmt == ETC1A4);
        return true;
    }
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
            case 0:  // I4 -> L4 (the combiners read its alpha from red) or LA4
                if (fmt == L4)
                    block<Nib>(out, pl, x0, y0, 8, 8, w, h, [&](u32 i) { return (u32)(s[i >> 1] >> ((i & 1) ? 0 : 4)) & 15; });
                else
                    block<u8>(out, pl, x0, y0, 8, 8, w, h, [&](u32 i) {
                        u32 n = (s[i >> 1] >> ((i & 1) ? 0 : 4)) & 15;
                        return n << 4 | n;
                    });
                break;
            case 1:  // I8 -> L8 or LA8
                if (fmt == L8)
                    block<u8>(out, pl, x0, y0, 8, 4, w, h, [&](u32 i) { return (u32)s[i]; });
                else
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
    switch (texel_bits(fmt)) {
    case 4: pad<Nib>(out, pl, w, h, W, H); break;
    case 8: pad<u8>(out, pl, w, h, W, H); break;
    case 16: pad<u16>(out, pl, w, h, W, H); break;
    default: pad<u32>(out, pl, w, h, W, H); break;
    }
    return true;
}

}  // namespace guest::pica_tex
