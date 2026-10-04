// Textures of the native g3d: the PICA's formats and layout (what a package holds and the 3DS GPU
// samples), GX's formats where a package still has a texture as Nintendo shipped it, and the
// conversions between them.
//
// The PICA keeps a W x H texture (powers of two, 8 to 1024) in 8x8 tiles, the texels of a tile in
// Z order, the image upside down (image row y is memory row H-1-y), mip levels one after the other,
// each half the size of the one before and at least 8x8 (citro3d's C3D_TexInitMipmap). The formats
// are numbered as citro3d's GPU_TEXCOLOR (fmt::TexFormat). GX textures are converted with
// platform/3ds/pica_tex.h, the GX renderer's own conversion (so a texture comes out as the GX path
// would have it), level by level.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/g3d/ng3d_format.h"

namespace engine::g3d {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;

// ---- the PICA's formats
bool pica_format_valid(u32 format);
u32 pica_texel_bits(u32 format);
// bytes of one level of w x h texels (w, h multiples of 8)
u32 pica_level_bytes(u32 format, u32 w, u32 h);
// how many levels a w x h texture can have (each at least 8x8)
u32 pica_max_levels(u32 w, u32 h);
// bytes of `levels` levels from w x h down
u32 pica_texture_bytes(u32 format, u32 w, u32 h, u32 levels);
bool is_pow2(u32 v);

// a texture's level: where its texels are (from the texture's first byte) and its size
struct TexLevel {
    u32 offset, size, width, height;
};
TexLevel pica_level(u32 format, u32 w, u32 h, u32 level);

// Level `level` of a PICA texture (w x h is that level's size) as RGBA8 (R in bits 0-7 ... A in
// 24-31, as runtime/hw/gx/texture.cpp decodes GX textures), rows from the image's first (GX's t = 0)
// down. What the PICA samples: luminance formats have alpha 255 (the combiners read their red where
// GX would read an intensity texture's alpha: engine/gx/tev_combiner's remap_luminance), A8/A4 have
// colour 0, ETC1 alpha 255.
void pica_decode(u32 format, const u8* texels, u32 w, u32 h, u32* rgba);

// ---- GX's formats (GXTexFmt: 0 I4, 1 I8, 2 IA4, 3 IA8, 4 RGB565, 5 RGB5A3, 6 RGBA8, 8-10 paletted,
// 14 CMPR)
// bytes of one level of w x h texels (whole blocks)
u32 gx_level_bytes(u32 gx_format, u32 w, u32 h);
// bytes of `levels` levels from w x h down (GX levels halve down to 1x1)
u32 gx_texture_bytes(u32 gx_format, u32 w, u32 h, u32 levels);
// whether pica_tex.h converts this GX format to this PICA format exactly as the GX path keeps it:
// I4 -> L4 or LA4, I8 -> L8 or LA8, IA4 -> LA4, IA8 -> LA8, RGB565 -> RGB565, RGB5A3 and RGBA8 ->
// RGBA8, CMPR -> RGBA5551, ETC1 or ETC1A4 (lossy). Paletted textures are not supported (a package
// has no palettes).
bool gx_conversion_supported(u32 gx_format, u32 pica_format);

// A texture converted from GX (fmt::TEX_GX_RAW) to its PICA format: every level the PICA can take
// (down to 8x8, at most the source's), padded to powers of two with the edge texels (as the GX path
// does for a clamped texture; UVs stay those of the source image, which a backend scales by
// src/size). Not thread-safe against the GX path's own conversions: pica_tex.h keeps its work
// tables in statics, so a package with GX textures must be loaded while the GX renderer uploads
// none (better: baked, tools/ng3d_bake).
struct ConvertedTexture {
    u16 width = 0, height = 0;  // of level 0, powers of two
    u8 format = 0, levels = 0;
    std::vector<u8> texels;     // all levels
};
// false (and why in error) if the texture can't be converted
bool convert_gx_texture(const fmt::Texture& t, const u8* gx_texels, u32 gx_size, ConvertedTexture* out,
                        std::string* error);

}  // namespace engine::g3d
