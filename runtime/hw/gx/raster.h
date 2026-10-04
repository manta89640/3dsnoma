// The software renderer (raster.cpp): the GPU's back end on the host.
#pragma once
#include "gx.h"

namespace guest {

class SoftRenderer : public GxRenderer {
public:
    void draw(u32 prim, u32 vat, u32 count, const u8* data) override;
    void efb_copy(u32 value) override;
    // a native draw: the registered backend draws into the EFB (gx_host_efb), at this point of the
    // stream (nothing is cached here between draws: every draw reads the registers again)
    void native(u32 id) override;
};

// The EFB for native draws (engine/g3d's software backend) drawing beside GX's, into the same
// pixels and depth: colour (R in bits 0-7 ... A in 24-31), depth (24 bits, 0 near ... 0xFFFFFF far,
// as GX writes it), the size, and what the registers say now: the scissor (inclusive EFB pixels,
// the scissor offset applied, clamped to the EFB), the offset to subtract from viewport-transformed
// x and y (the scissor offset: raster.cpp's to_screen), and the pixel format (BP 0x43 bits 0-2:
// 0 RGB8 (alpha not kept), 1 RGBA6, 2 RGB565).
struct HostEfb {
    u32* color;
    u32* depth;
    u32 width, height;
    s32 scissor_left, scissor_top, scissor_right, scissor_bottom;
    f32 x_offset, y_offset;
    u32 pixel_format;
};
HostEfb gx_host_efb();

// an EFB copy as the BP registers describe it when the copy command (BP 0x52) is written
struct EfbCopy {
    u32 sx = 0, sy = 0, w = 0, h = 0;  // source rectangle in the EFB
    u32 dst = 0, stride = 0;            // destination and its stride (bytes per row of blocks)
    u32 fmt = 0;                        // texture format of a copy to a texture
    bool to_xfb = false, half = false, intensity = false, clear = false, depth = false;
};
EfbCopy gx_efb_copy_params(u32 command);
// an EFB pixel (x, y) as R, G, B, A (0-255)
using EfbFetch = void (*)(u32 x, u32 y, s32* rgba);
// write a copy of the EFB rectangle to the texture at c.dst, or to `out` (laid out the same way)
void gx_copy_to_texture(const EfbCopy& c, EfbFetch fetch, u8* out = nullptr);
// the same from a read-back buffer: EFB pixel (x, y) is pixels[col[x & 1023] + row[y & 1023]], as
// 0xRRGGBBAA. The common copies (RGB565, RGB5A3, RGBA8 at full size, not intensity) go straight
// from it; the others through gx_copy_to_texture.
void gx_copy_to_texture_lookup(const EfbCopy& c, const u32* pixels, const u32* col, const u32* row, u8* out = nullptr);
// NOMA_EFB_COPY_TEST=N (3DS): at frame N the whole frame goes through the EFB-to-texture path
// (RGBA8, and RGB565 at half size) and is saved decoded as <prefix>_rgba8.bmp / _rgb565.bmp,
// to compare with the host's frame dumps
int gx_efb_copy_test_frame();
void gx_efb_copy_test(EfbFetch fetch, const char* prefix);

// the last EFB copy to an external frame buffer
struct XfbCopy {
    u32 addr = 0, width = 0, height = 0, stride = 0;
};
const XfbCopy& gx_last_xfb_copy();
// the frame buffer starting at addr, as far as copies have filled it (null if none)
const XfbCopy* gx_xfb_region(u32 addr);

}  // namespace guest

namespace guest {
// an external frame buffer (YUYV) as RGBA (R in bits 0-7)
void gx_xfb_to_rgba(u32 addr, u32 width, u32 height, u32 stride, u32* out);
}  // namespace guest
