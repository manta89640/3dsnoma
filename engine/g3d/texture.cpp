// Textures of the native g3d (texture.h): PICA layout, GX -> PICA conversion, PICA -> RGBA8 decoding.
#include "engine/g3d/texture.h"

#include <algorithm>
#include <cstring>

#include "platform/3ds/pica_tex.h"

namespace engine::g3d {

namespace pt = guest::pica_tex;

bool is_pow2(u32 v) { return v && !(v & (v - 1)); }

bool pica_format_valid(u32 format) { return format <= fmt::TF_ETC1A4; }

u32 pica_texel_bits(u32 format) {
    switch (format) {
    case fmt::TF_RGBA8: return 32;
    case fmt::TF_RGB8: return 24;
    case fmt::TF_RGBA5551:
    case fmt::TF_RGB565:
    case fmt::TF_RGBA4:
    case fmt::TF_LA8:
    case fmt::TF_HILO8: return 16;
    case fmt::TF_L8:
    case fmt::TF_A8:
    case fmt::TF_LA4:
    case fmt::TF_ETC1A4: return 8;
    default: return 4;  // L4, A4, ETC1
    }
}

u32 pica_level_bytes(u32 format, u32 w, u32 h) { return w * h * pica_texel_bits(format) / 8; }

u32 pica_max_levels(u32 w, u32 h) {
    u32 n = 1;
    while ((w >> n) >= 8 && (h >> n) >= 8)
        n++;
    return n;
}

u32 pica_texture_bytes(u32 format, u32 w, u32 h, u32 levels) {
    u32 total = 0;
    for (u32 l = 0; l < levels; l++)
        total += pica_level_bytes(format, w >> l, h >> l);
    return total;
}

TexLevel pica_level(u32 format, u32 w, u32 h, u32 level) {
    TexLevel t;
    t.offset = pica_texture_bytes(format, w, h, level);
    t.width = w >> level, t.height = h >> level;
    t.size = pica_level_bytes(format, t.width, t.height);
    return t;
}

// ---------------------------------------------------------------- PICA -> RGBA8
namespace {

inline u32 rgba(u32 r, u32 g, u32 b, u32 a) { return r | g << 8 | b << 16 | a << 24; }

// the texel index of image texel (x, y) in a w x h texture: 8x8 tiles left to right then up from
// the bottom of the image, Z order within a tile (pica_tex.h's Places)
inline u32 tiled(u32 x, u32 y, u32 w, u32 h) {
    const u32 py = h - 1 - y;
    return ((py >> 3) * (w >> 3) + (x >> 3)) * 64 + ((x & 1) | ((py & 1) << 1) | ((x & 2) << 1) | ((py & 2) << 2) |
                                                      ((x & 4) << 2) | ((py & 4) << 3));
}

const int ETC1_MOD[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};

// an ETC1 block (little-endian 64-bit word) -> 16 texels px[4 * x + y] (block coordinates, y down
// in memory order), alpha 255
void etc1_block(uint64_t v, u32* px) {
    const bool flip = (v >> 32) & 1, diff = (v >> 33) & 1;
    for (u32 x = 0; x < 4; x++)
        for (u32 y = 0; y < 4; y++) {
            const u32 t = 4 * x + y;
            // which half: by x (flip 0) or by y (flip 1)
            const bool second = flip ? y >= 2 : x >= 2;
            const int table = second ? (int)((v >> 34) & 7) : (int)((v >> 37) & 7);
            int m = ETC1_MOD[table][(v >> t) & 1];
            if ((v >> (16 + t)) & 1)
                m = -m;
            int c[3];
            for (int k = 0; k < 3; k++) {
                const int at = 56 - 8 * k;  // R at bit 56, G at 48, B at 40
                int q8;
                if (diff) {
                    int base = (int)((v >> (at + 3)) & 31), d = (int)((v >> at) & 7);
                    if (d >= 4)
                        d -= 8;
                    const int q5 = second ? base + d : base;
                    q8 = (q5 << 3) | (q5 >> 2);
                } else {
                    q8 = (second ? (int)((v >> at) & 15) : (int)((v >> (at + 4)) & 15)) * 17;
                }
                const int s = q8 + m;
                c[k] = s < 0 ? 0 : s > 255 ? 255 : s;
            }
            px[t] = rgba((u32)c[0], (u32)c[1], (u32)c[2], 255);
        }
}

}  // namespace

void pica_decode(u32 format, const u8* p, u32 w, u32 h, u32* out) {
    if (format == fmt::TF_ETC1 || format == fmt::TF_ETC1A4) {
        // 8x8 tiles of four 4x4 blocks (left to right, then down in memory order)
        const u32 bytes = format == fmt::TF_ETC1A4 ? 16 : 8;
        u32 px[16];
        for (u32 my = 0; my < h; my += 4)
            for (u32 mx = 0; mx < w; mx += 4) {
                const u8* b = p + (((my / 8) * (w / 8) + mx / 8) * 4 + ((mx / 4) & 1) + 2 * ((my / 4) & 1)) * bytes;
                uint64_t alpha = ~0ull, color;
                if (bytes == 16)
                    memcpy(&alpha, b, 8), b += 8;
                memcpy(&color, b, 8);
                etc1_block(color, px);
                for (u32 x = 0; x < 4; x++)
                    for (u32 y = 0; y < 4; y++) {
                        const u32 t = 4 * x + y;
                        const u32 a = (u32)((alpha >> (4 * t)) & 15) * 17;
                        // memory row my + y is image row h - 1 - (my + y)
                        out[(h - 1 - (my + y)) * w + mx + x] = (px[t] & 0xFFFFFF) | a << 24;
                    }
            }
        return;
    }
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            const u32 i = tiled(x, y, w, h);
            u32 c;
            switch (format) {
            case fmt::TF_RGBA8: {
                u32 v;
                memcpy(&v, p + 4 * i, 4);
                c = rgba(v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255);
                break;
            }
            case fmt::TF_RGB8: c = rgba(p[3 * i + 2], p[3 * i + 1], p[3 * i], 255); break;
            case fmt::TF_RGBA5551: {
                const u32 v = (u32)p[2 * i] | (u32)p[2 * i + 1] << 8;
                c = rgba(pt::detail::x5(v >> 11), pt::detail::x5((v >> 6) & 31), pt::detail::x5((v >> 1) & 31),
                         (v & 1) ? 255 : 0);
                break;
            }
            case fmt::TF_RGB565: {
                const u32 v = (u32)p[2 * i] | (u32)p[2 * i + 1] << 8;
                c = rgba(pt::detail::x5(v >> 11), pt::detail::x6((v >> 5) & 63), pt::detail::x5(v & 31), 255);
                break;
            }
            case fmt::TF_RGBA4: {
                const u32 v = (u32)p[2 * i] | (u32)p[2 * i + 1] << 8;
                c = rgba((v >> 12) * 17, ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17);
                break;
            }
            case fmt::TF_LA8: c = rgba(p[2 * i + 1], p[2 * i + 1], p[2 * i + 1], p[2 * i]); break;
            case fmt::TF_HILO8: c = rgba(p[2 * i + 1], p[2 * i], 0, 255); break;
            case fmt::TF_L8: c = rgba(p[i], p[i], p[i], 255); break;
            case fmt::TF_A8: c = rgba(0, 0, 0, p[i]); break;
            case fmt::TF_LA4: {
                const u32 l = (p[i] >> 4) * 17;
                c = rgba(l, l, l, (p[i] & 15) * 17);
                break;
            }
            case fmt::TF_L4: {
                const u32 l = ((i & 1) ? p[i >> 1] >> 4 : p[i >> 1] & 15) * 17;
                c = rgba(l, l, l, 255);
                break;
            }
            case fmt::TF_A4: c = rgba(0, 0, 0, ((i & 1) ? p[i >> 1] >> 4 : p[i >> 1] & 15) * 17); break;
            default: c = 0; break;
            }
            out[y * w + x] = c;
        }
}

// ---------------------------------------------------------------- GX
u32 gx_level_bytes(u32 gx_format, u32 w, u32 h) {
    u32 bw, bh, bytes = 32;
    switch (gx_format) {
    case 0: case 8: case 14: bw = 8, bh = 8; break;
    case 1: case 2: case 9: bw = 8, bh = 4; break;
    case 6: bw = 4, bh = 4, bytes = 64; break;
    default: bw = 4, bh = 4; break;  // IA8, RGB565, RGB5A3, C14X2
    }
    return ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bytes;
}

u32 gx_texture_bytes(u32 gx_format, u32 w, u32 h, u32 levels) {
    u32 total = 0;
    for (u32 l = 0; l < levels; l++)
        total += gx_level_bytes(gx_format, w >> l ? w >> l : 1, h >> l ? h >> l : 1);
    return total;
}

bool gx_conversion_supported(u32 gx_format, u32 f) {
    switch (gx_format) {
    case 0: return f == fmt::TF_L4 || f == fmt::TF_LA4;
    case 1: return f == fmt::TF_L8 || f == fmt::TF_LA8;
    case 2: return f == fmt::TF_LA4;
    case 3: return f == fmt::TF_LA8;
    case 4: return f == fmt::TF_RGB565;
    case 5:
    case 6: return f == fmt::TF_RGBA8;
    case 14: return f == fmt::TF_RGBA5551 || f == fmt::TF_ETC1 || f == fmt::TF_ETC1A4;
    default: return false;
    }
}

namespace {

pt::Fmt pica_tex_format(u32 f) {
    switch (f) {
    case fmt::TF_LA4: return pt::LA4;
    case fmt::TF_LA8: return pt::LA8;
    case fmt::TF_RGB565: return pt::RGB565;
    case fmt::TF_RGBA5551: return pt::RGBA5551;
    case fmt::TF_L8: return pt::L8;
    case fmt::TF_L4: return pt::L4;
    case fmt::TF_ETC1: return pt::ETC1;
    case fmt::TF_ETC1A4: return pt::ETC1A4;
    default: return pt::RGBA8;
    }
}

u32 next_pow2(u32 v) {
    u32 p = 8;
    while (p < v && p < 1024)
        p <<= 1;
    return p;
}

}  // namespace

bool convert_gx_texture(const fmt::Texture& t, const u8* src, u32 src_size, ConvertedTexture* out, std::string* error) {
    auto fail = [&](const char* why) {
        if (error)
            *error = why;
        return false;
    };
    const u32 w = t.width, h = t.height;
    if (!w || !h || w > 1024 || h > 1024)
        return fail("size out of range");
    if (!gx_conversion_supported(t.gx_format, t.format))
        return fail("no conversion from its GX format to its PICA format");
    const u32 gx_levels = t.levels ? t.levels : 1;
    if (gx_texture_bytes(t.gx_format, w, h, gx_levels) > src_size)
        return fail("fewer texel bytes than its levels need");
    const u32 W = next_pow2(w), H = next_pow2(h);
    // levels beyond the first only for a texture already a power of two (GX allows mipmaps only
    // there), down to 8x8
    u32 levels = 1;
    if (W == w && H == h)
        levels = std::min(gx_levels, pica_max_levels(W, H));
    out->width = (u16)W, out->height = (u16)H, out->format = t.format, out->levels = (u8)levels;
    out->texels.assign(pica_texture_bytes(t.format, W, H, levels), 0);
    u32 at = 0;
    for (u32 l = 0; l < levels; l++) {
        const u32 lw = w >> l, lh = h >> l;
        const TexLevel pl = pica_level(t.format, W, H, l);
        if (!pt::from_gx(src + at, t.gx_format, lw, lh, nullptr, 0, 0, 0, out->texels.data() + pl.offset, pl.width,
                         pl.height, pica_tex_format(t.format)))
            return fail("pica_tex.h doesn't convert its GX format");
        at += gx_level_bytes(t.gx_format, lw, lh);
    }
    return true;
}

}  // namespace engine::g3d
