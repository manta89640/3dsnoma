// The GPU ("GX", Flipper/Hollywood's graphics processor) as the decompiled GX library drives it.
//
// The CPU writes commands through the write-gather pipe (0xCC008000), which stores them in
// main memory at the PI FIFO write pointer. When that FIFO is also the GPU's (linked mode) the
// command processor executes them; otherwise they are a display list being recorded, executed
// later by a CALL_DL command. fifo.cpp moves the bytes, cp.cpp decodes the command stream and
// keeps the GPU's register state, a renderer (GxRenderer below) draws. One command is ours, not
// GX's: 0x50 + a big-endian u32, a native draw (engine/g3d/native_draw.h).
#pragma once
#include <algorithm>

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
    // moves on when the lighting setup may have changed (light memory 0x600-0x7FF, the color
    // channel registers 0x1009-0x1011): the draw path keeps its light plans until then
    u32 light_serial;
    // move on when XF memory (matrices, lights: below 0x1000), the post-transform matrices
    // (0x500-0x5FF) or an XF register changed: the draw path keeps its plan for a draw until then
    u32 xf_mem_serial, xf_post_serial, xf_reg_serial;
    u32 xf_viewport_serial;  // ... when the viewport (0x101A-0x101F) changed
    // the matrix rows' write generations (xf_gen when a row last got new values): position and
    // texture matrix memory in rows of 4 words (0x000-0x10F), normal matrix memory in rows of 3
    // (0x400-0x465): what the draw path copied of them is current while their generation is older
    u32 xf_gen;
    u32 xf_row_gen[68];
    u32 xf_nrm_gen[34];
    // the parts of a renderer's state whose BP registers changed since it last looked (GX_BP_*):
    // the decoder sets the bits as it writes the registers, the renderer clears them
    u32 bp_dirty;
};
enum : u32 {
    GX_BP_TEV = 1,      // combiners: GENMODE, TREF, the TEV stages, registers and constants, KSEL
    GX_BP_TEX = 2,      // textures (modes, images, palettes, TMEM loads)
    GX_BP_PIXEL = 4,    // depth, blending, logic op, write masks, alpha test
    GX_BP_SCISSOR = 8,  // the scissor box
};
// the XF words [addr, addr + count) were written with new values
static inline void gx_xf_written(GxState& s, u32 addr, u32 count) {
    if ((addr < 0x800 && addr + count > 0x600) || (addr < 0x1012 && addr + count > 0x1009))
        s.light_serial++;
    if (addr < 0x1000) {
        s.xf_mem_serial++;
        const u32 end = addr + count;
        if (addr < 0x110) {
            u32 g = ++s.xf_gen;
            for (u32 r = addr >> 2, last = (std::min<u32>(end, 0x110) - 1) >> 2; r <= last; r++)
                s.xf_row_gen[r] = g;
        }
        if (addr < 0x466 && end > 0x400) {
            u32 g = ++s.xf_gen;
            u32 lo = addr > 0x400 ? addr - 0x400 : 0;
            for (u32 r = lo / 3, last = (std::min<u32>(end, 0x466) - 0x400 - 1) / 3; r <= last; r++)
                s.xf_nrm_gen[r] = g;
        }
    }
    if (addr < 0x600 && addr + count > 0x500)
        s.xf_post_serial++;
    if (addr + count > 0x1000)
        s.xf_reg_serial++;
    if (addr < 0x1020 && addr + count > 0x101A)
        s.xf_viewport_serial++;
}
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

// Called by the command decoder (cp.cpp) for what a renderer cares about.
struct GxRenderer {
    virtual ~GxRenderer() {}
    // a primitive: type (GX_QUADS 0x80 ... GX_POINTS 0xB8), vertex format, count, the raw
    // vertex data in the stream (count * gx_vertex_format(vat).size bytes)
    virtual void draw(u32 prim, u32 vat, u32 count, const u8* data) {}
    // BP 0x45 (draw done): the frame is complete (the other registers only set g_gx.bp_dirty)
    virtual void draw_done() {}
    // XF indexed loads (matrices/normals/lights from main memory)
    virtual void xf_indexed_load(u32 which, u32 index, u32 addr, u32 count) {}
    // BP 0x52: copy the EFB to texture memory or to the external frame buffer
    virtual void efb_copy(u32 value) {}
    virtual void frame_done() {}
    // the platform hands its GPU to the system for a while (an applet such as the keyboard):
    // what was drawn so far goes to the GPU, and nothing is left open
    virtual void suspend() {}
    // our own command 0x50 (not GX's): a native draw lands here, at its place in the stream
    // (engine/g3d/native_draw.h): the renderer brings its state up to date, calls
    // engine::g3d::native_draw_execute(id), and takes nothing it cached about the GPU's state as
    // still true afterwards. Without a renderer the packet is never executed (native_draw takes it
    // back later).
    virtual void native(u32 id) {}
    // our own command 0x51 (not GX's): what is drawn from here on goes onto `screen` (SCREEN_TOP,
    // SCREEN_BOTTOM: the 3DS's two), until the next such command or the frame's end. For the bottom
    // screen, the frame's rectangle x, y, w, h (EFB pixels) is what fills it, scaled to its 320x240
    // (engine/ui/screen.h). Each frame starts on the top screen.
    virtual void screen(u32 screen, u32 x, u32 y, u32 w, u32 h) {}
};
enum : u32 { SCREEN_TOP = 0, SCREEN_BOTTOM = 1 };
constexpr u8 SCREEN_OPCODE = 0x51;
void gx_set_renderer(GxRenderer* r);
// Before the platform runs an applet that takes over the GPU and the screens (the 3DS keyboard):
// the GPU's thread finishes what it was given and the renderer closes the frame it is drawing
// (not shown; the frame goes on after the applet). Called from the CPU's thread; nothing else
// for the renderer to do on the host.
void gx_end_frame_for_applet();

// fifo.cpp
void gx_fifo_init();
void gx_pi_fifo_write(u32 off, u32 value, int size);  // PI 0x0C/0x10/0x14: CPU FIFO base/end/wptr
u32 gx_pi_fifo_read(u32 off);
void gx_run();  // execute whatever the GPU's FIFO holds now (also called from device polls)
// let the GPU's thread finish what it was given (no-op where the decoder runs inline)
void gx_sync();
// the app ends: the GPU's thread finishes what it was given and then sleeps for good; false if it
// didn't within timeout_us (true where the decoder runs inline)
bool gx_park(u64 timeout_us);

// cp.cpp: decode the commands in [p, p+n) in a row; returns bytes used (less than n when the
// last command is cut off: the rest waits for its tail)
u32 gx_decode(const u8* p, u32 n, bool in_display_list);
// while a renderer's draw() runs: whether the draw came from a display list, and its primitive
bool gx_in_display_list();
u32 gx_current_prim();
// ... and the list (guest address, size) and the draw command's offset in it
u32 gx_display_list_addr();
u32 gx_display_list_size();
u32 gx_display_list_offset();
// what the decoder has executed so far (totals; a renderer's statistics take differences)
struct GxDecodeCounts {
    u32 draws, vertices, bp, cp, xf, dl;
    u32 native;  // native draw commands (0x50)
};
GxDecodeCounts gx_decode_counts();

// pe events
void pe_finish();               // BP 0x45 (draw done)
void pe_token(u16 value, bool interrupt);

}  // namespace guest
