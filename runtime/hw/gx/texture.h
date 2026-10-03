// Textures: GX formats decoded to RGBA8, cached by address, size, format, palette and contents.
#pragma once
#include <vector>

#include "gx.h"

namespace guest {

struct GxTexture {
    u32 width = 0, height = 0;
    std::vector<u32> rgba;  // R in bits 0-7 ... A in bits 24-31 (host order); empty once released
    u32 version = 0;        // changes whenever the contents are decoded again
    u32 id = 0;             // unique per cache entry
    u32 fmt = 0;            // the GX format it came from (paletted: and its palette's), for
    u32 tlut_fmt = 0;       // renderers that keep a texture in less memory than RGBA8
};

// texture map 0-7 as the BP registers describe it now (null if it has no image)
const GxTexture* gx_texture(u32 map);
// A renderer that keeps the pixels itself (the 3DS: in a PICA texture) frees the RGBA8 copy
// once it has it; gx_texture_pixels decodes it again if it is needed later (same version
// semantics: a new decode is a new version).
void gx_texture_release(const GxTexture* t);
const GxTexture* gx_texture_pixels(u32 map);
// the texture cache forgets textures not used for a while; called once per frame
void gx_texture_frame();
// TMEM (palettes): BP 0x65 copies main memory into it
void gx_load_tlut(u32 bp64, u32 bp65);
// TMEM (preloaded textures): BP 0x63 copies main memory into it (BP 0x60-0x62: source and banks)
void gx_preload(u32 bp60, u32 bp61, u32 bp62, u32 bp63);
// bytes a texture of this format and size takes in memory
u32 gx_texture_bytes(u32 fmt, u32 w, u32 h);
// memory that textures may come from changed (an EFB copy): check those again when used
void gx_texture_invalidate(u32 addr, u32 size);
// a texture in GX format (not paletted) -> RGBA8 as in GxTexture
void gx_decode_texture(const u8* src, u32 fmt, u32 w, u32 h, u32* rgba);

}  // namespace guest
