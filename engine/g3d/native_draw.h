// Native draws inside the GX frame.
//
// The channel draws through GX: its commands go into the GX FIFO and a renderer (the 3DS GPU's,
// platform/3ds/gx_pica.cpp, on the GPU's own thread; the host's software one, runtime/hw/gx/raster.cpp)
// turns them into pixels later, in order. Native code that draws (the native g3d's World) has to land
// at the right point of that order, and share the frame and its depth with what the channel draws
// around it (the Mii, the layouts). So it doesn't draw: it fills a Packet and puts one GX command
// into the FIFO, opcode NATIVE_DRAW_OPCODE followed by the packet's id (big-endian u32). The command
// decoder (runtime/hw/gx/cp.cpp) hands the id to GxRenderer::native, which calls the backend
// registered here with the packet, at that point of the stream, with the GX projection, viewport
// and scissor current there (the backend reads them as a GX draw would).
//
// Packets come from a ring; one is free again once the renderer has executed it (on the 3DS the GPU's
// thread runs up to a frame behind). Everything a packet's draws need is copied into it, except the
// package (vertex and texel data), which lives until native_draw_release().
#pragma once

#include <cstdint>
#include <vector>

#include "engine/g3d/package.h"

namespace engine::g3d {

constexpr u8 NATIVE_DRAW_OPCODE = 0x50;  // not a GX command; the decoder knows it as ours

enum class Pass : u8 { OPAQUE = 0, TRANSLUCENT = 1 };

// one draw: a batch of the package, with the matrix (model -> world, package units) and the
// material's dynamic state for this frame
struct DrawItem {
    u16 batch;         // fmt::Batch index
    u16 matrix;        // Packet::matrices index (0 = identity, the static world)
    u16 mat_state;     // Packet::mat_states index, or 0xFFFF: the material as packaged
    u16 flags;         // DRAW_* below
};
enum : u16 {
    DRAW_SKY = 1,      // the sky dome: no depth write, drawn first
};

// the animated parts of a material, sampled for this frame
struct MatDynamic {
    u16 material;
    u16 pad;
    s16 tev_reg[4][4];     // PREV, C0-C2
    s16 tev_konst[4][4];   // K0-K3
    u8 mat_color[4], amb_color[4];
    f32 tex_mtx[3][2][4];  // the material's first three texture matrices, 2x4 (S, T rows), final
                           // (scale/rotate/translate of its SRT0 track applied, effect matrix aside)
};

struct Packet {
    u32 id = 0;
    const Package* package = nullptr;
    Pass pass = Pass::OPAQUE;
    // the camera: the view matrix in the GUEST's units (Wii no Ma's, the Mii's), and the scale from
    // package units to guest units (fmt::Env::mii_units_per_unit): model-view = view * S(k) * matrix
    f32 view[3][4] = {};
    f32 world_scale = 1.0f;
    f32 eye[3] = {};               // package units (for fog/env effects that need it)
    // lighting for the materials' light sets (package units, world space)
    // (lights and ambients copied from the package's env each frame; a scene may change them)
    struct Light {
        f32 dir[3];                // direction the light travels (unit)
        u8 rgba[4];
    };
    std::vector<Light> lights;
    std::vector<fmt::LightSet> light_sets;
    std::vector<fmt::Fog> fogs;
    f32 time = 0;                  // frames (60 a second), for anything a backend animates itself
    std::vector<f32> matrices;     // 12 floats each (3x4), [0] = identity
    std::vector<MatDynamic> mat_states;
    std::vector<DrawItem> items;   // in draw order
};

// A backend draws packets: the 3DS GPU's (platform/3ds/g3d_pica.cpp) or the host's software one
// (engine/g3d/soft_backend.cpp). All its calls come from the renderer's thread (the GPU thread on the
// 3DS), in the GX stream's order.
class Backend {
public:
    virtual ~Backend() {}
    // upload what the package's draws need (textures, vertex and index buffers); called before its
    // first draw (a prepare packet: native_draw_prepare)
    virtual void prepare(Package& package) = 0;
    virtual void draw(const Packet& packet) = 0;
    // free what prepare made
    virtual void release(Package& package) = 0;
};

// the platform's backend (set at start-up, before the guest runs)
void native_draw_set_backend(Backend* backend);
Backend* native_draw_backend();

// a free packet (blocks while every packet of the ring is still in flight: the renderer's thread
// runs behind); fill it, then submit
Packet* native_draw_begin();
// put the packet's command into the GX FIFO (from the guest CPU's thread, in a scene's draw)
void native_draw_submit(Packet* packet);
// ask the backend to prepare the package now (its own command in the stream, so that uploads happen
// under a loading screen, not at the first draw)
void native_draw_prepare(Package& package);
// the package is about to be freed: waits until the renderer has executed every command so far,
// then has the backend release its resources (on the renderer's thread)
void native_draw_release(Package& package);

// For the renderers (GxRenderer::native): execute the command with this id
void native_draw_execute(u32 id);

}  // namespace engine::g3d
