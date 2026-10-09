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
    u32 tlut_addr = 0;      // (paletted: the palette's TMEM address)
};

// texture map 0-7 as the BP registers describe it now (null if it has no image)
const GxTexture* gx_texture(u32 map);
// A renderer that keeps the pixels itself (the 3DS: in a PICA texture) frees the RGBA8 copy
// once it has it; gx_texture_pixels decodes it again if it is needed later (same version
// semantics: a new decode is a new version).
void gx_texture_release(const GxTexture* t);
const GxTexture* gx_texture_pixels(u32 map);
// A renderer that converts the GX bytes itself (the 3DS): the same texture and versions, never
// decoded to RGBA8; *src gets its bytes in GX format (valid until the next call), gx_tmem() has the
// palettes (TMEM_MASK: a 2-byte entry's offset)
const GxTexture* gx_texture_raw(u32 map, const u8** src);
// gx_texture and gx_texture_raw from a map's register values themselves (SETIMAGE0-3 and SETTLUT,
// 24-bit, as GXLoadTexObj would have sent them), for a draw whose textures never went through the
// registers (a layout drawn natively: engine/lyt/draw.h): the same cache entries and versions as
// the registers holding those values would give.
const GxTexture* gx_texture_regs(u32 image0, u32 image1, u32 image2, u32 image3, u32 tlut);
const GxTexture* gx_texture_raw_regs(u32 image0, u32 image1, u32 image2, u32 image3, u32 tlut, const u8** src);
const u8* gx_tmem();
const u32 GX_TMEM_MASK = (1u << 20) - 2;
// A frame has ended: textures are checked against their memory again at their next use (once a
// frame), and those not used for a while are forgotten. Every renderer calls it once per frame (the
// host's at its XFB copy, the 3DS's at the frame's end): without it a texture keeps the texels it was
// first seen with, whatever the guest writes there later.
void gx_texture_frame();
// what the cache's checks did so far: textures checked (once a frame each), images hashed and
// their bytes, textures found changed (decoded again: a new version); for a renderer's log line
struct GxTextureStats {
    u32 checks = 0, hashes = 0, changed = 0;
    u64 hashed_bytes = 0;
};
GxTextureStats gx_texture_stats();
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
// ... paletted too (the palette in TMEM at tlut_addr)
void gx_decode_texture_tlut(const u8* src, u32 fmt, u32 w, u32 h, u32 tlut_addr, u32 tlut_fmt, u32* rgba);
u8* gx_tmem_for_tests();

}  // namespace guest
