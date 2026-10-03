// The GPU ("GX", Flipper/Hollywood's graphics processor) as the decompiled GX library drives it.
//
// The CPU writes commands through the write-gather pipe (0xCC008000), which stores them in
// main memory at the PI FIFO write pointer. When that FIFO is also the GPU's (linked mode) the
// command processor executes them; otherwise they are a display list being recorded, executed
// later by a CALL_DL command. fifo.cpp moves the bytes, cp.cpp decodes the command stream and
// keeps the GPU's register state, a renderer (gx_render.h) draws.
#pragma once
#include "../hw.h"

namespace guest {

// ---- GPU register state, as the command stream sets it
struct GxState {
    u32 cp[256];      // command processor registers (vertex descriptors, attribute formats, arrays)
    u32 xf[0x1058];   // transform unit: matrix memory (0x0000-0x04FF), lights etc., registers at 0x1000
    u32 bp[256];      // blitting processor registers (TEV, textures, pixel engine, EFB copies)
    // TEV registers (BP 0xE0-0xE7 hold two kinds behind one address: the type bit picks the
    // color registers PREV, C0-C2 or the constants K0-K3), RGBA, 11-bit signed
    s32 tev_reg[4][4];
    s32 tev_konst[4][4];
};
extern GxState g_gx;

// CP register indices
enum : u32 {
    CP_VCD_LO = 0x50,
    CP_VCD_HI = 0x60,
    CP_VAT_A = 0x70,  // + vat
    CP_VAT_B = 0x80,
    CP_VAT_C = 0x90,
    CP_ARRAY_BASE = 0xA0,    // + attribute (0 position, 1 normal, 2-3 colors, 4-11 texcoords, 12-15 indexed loads)
    CP_ARRAY_STRIDE = 0xB0,
};

// Bytes one vertex takes in the stream for a vertex format (vat 0-7).
u32 gx_vertex_size(u32 vat);

// Called by the command decoder (cp.cpp) for what a renderer cares about.
struct GxRenderer {
    virtual ~GxRenderer() {}
    // a primitive: type (GX_QUADS 0x80 ... GX_POINTS 0xB8), vertex format, count, the raw
    // vertex data in the stream (count * gx_vertex_size(vat) bytes)
    virtual void draw(u32 prim, u32 vat, u32 count, const u8* data) {}
    virtual void bp_write(u32 reg, u32 value) {}
    virtual void xf_write(u32 addr, u32 value) {}
    virtual void cp_write(u32 reg, u32 value) {}
    // XF indexed loads (matrices/normals/lights from main memory)
    virtual void xf_indexed_load(u32 which, u32 index, u32 addr, u32 count) {}
    // BP 0x52: copy the EFB to texture memory or to the external frame buffer
    virtual void efb_copy(u32 value) {}
    virtual void frame_done() {}
};
void gx_set_renderer(GxRenderer* r);

// fifo.cpp
void gx_fifo_init();
void gx_pi_fifo_write(u32 off, u32 value, int size);  // PI 0x0C/0x10/0x14: CPU FIFO base/end/wptr
u32 gx_pi_fifo_read(u32 off);
void gx_run();  // execute whatever the GPU's FIFO holds now (also called from device polls)

// cp.cpp: decode commands; returns bytes used, or 0 if [p, p+n) ends in the middle of one
u32 gx_decode(const u8* p, u32 n, bool in_display_list);

// pe events
void pe_finish();               // BP 0x45 (draw done)
void pe_token(u16 value, bool interrupt);

}  // namespace guest
