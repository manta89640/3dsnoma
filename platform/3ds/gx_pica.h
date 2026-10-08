// The GX renderer on the 3DS GPU (gx_pica.cpp).
#pragma once
#include <3ds.h>
#include <citro3d.h>

#include "runtime/hw/gx/gx.h"

namespace guest {

class PicaRenderer : public GxRenderer {
public:
    bool init();
    void draw(u32 prim, u32 vat, u32 count, const u8* data) override;
    void draw_done() override;
    void efb_copy(u32 value) override;
    void suspend() override;
    // a native draw (engine/g3d/native_draw.h): the GX draws before it go to the GPU first; the
    // native g3d's PICA backend (g3d_pica.cpp) then draws between pica_native_begin and _end
    void native(u32 id) override;
};

// a wait for the GPU that hasn't ended in 3 s, logged (the I/O thread, every 100 ms)
void pica_watchdog();

// ---- For the native g3d's PICA backend (g3d_pica.cpp), on the GPU's thread. It writes the GPU's
// registers directly, its own shader program included, between these two:
//   * begin: the frame started if this is its first draw, the GX draws so far sent, the scissor
//     and the clip matrix of the GX state at this point of the stream (GX clip space -> the PICA's:
//     the viewport in the 640x456 frame, scaled to 400x240, rotated for the 240x400 target, the GX
//     depth range already complemented for depth map scale 1, offset 1), and what citro3d still
//     had to write (a frame's frame buffer and viewport) written;
//   * end: nothing the GX path keeps about the GPU's state is taken as still true (its shader
//     program, attribute layout, uniforms, combiners, textures, pixel state, culling); `used` is
//     the program the backend ran (citro3d only notices a program that differs from the last).
void pica_native_begin(f32 clip[4][4]);
void pica_native_end(shaderProgram_s* used);
// Between begin and end: the native draws go into another frame buffer (a texture's: the shadow map),
// whole (its viewport, no scissor); nullptr: back into the frame's, with the GX state's viewport and
// scissor. What was drawn before the switch is flushed to memory first.
void pica_native_target(C3D_FrameBuf* fb);
// a native draw goes into the command list (NOMA_CHECK_PICA_STATE pairs its snapshots with the
// list's draws: a native one is skipped)
void pica_native_draw_noted();
// linear memory the GPU may still read (a released package's buffers and textures): freed once
// every command list so far has finished
void pica_free_after_frame(void* linear);
// room in the linear heap for `bytes` more (a package's upload): the GX path's textures that no
// recent frame drew are dropped (made again if drawn again), oldest first, as their own uploads do
void pica_make_room(u32 bytes);

}  // namespace guest
