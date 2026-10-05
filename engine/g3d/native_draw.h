// Native draws inside the GX frame.
//
// The channel draws through GX: its commands go into the GX FIFO and a renderer (the 3DS GPU's,
// platform/3ds/gx_pica.cpp, on the GPU's own thread; the host's software one, runtime/hw/gx/raster.cpp)
// turns them into pixels later, in order. Native code that draws (the native g3d's World) has to land
// at the right point of that order, and share the frame and its depth with what the channel draws
// around it (the Mii, the layouts). So it doesn't draw: it fills a Packet and puts one GX command
// into the FIFO, opcode NATIVE_DRAW_OPCODE followed by the packet's id (big-endian u32). The command
// decoder (runtime/hw/gx/cp.cpp) hands the id to GxRenderer::native, which calls
// native_draw_execute: the backend registered here gets the packet at that point of the stream,
// with the GX projection, viewport and scissor current there (GxView: what a GX draw there would be
// placed with).
//
// Packets come from a ring of NATIVE_DRAW_PACKETS; one is free again once the renderer has executed
// it (on the 3DS the GPU's thread runs up to a frame behind). Everything a packet's draws need is
// in it or lives as long as the scene that emitted it: the package (vertex and texel data, until
// native_draw_release) and the runtime materials (the World's).
//
// The command is written from the guest CPU's thread, in a scene's draw (not while a display list is
// being recorded: it would end up in the list). If a renderer never executes it (no renderer, the
// FIFO not running), the packet is taken back when the ring comes round to it again, after a GX sync.
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "engine/g3d/material.h"
#include "engine/g3d/package.h"

namespace engine::g3d {

constexpr u8 NATIVE_DRAW_OPCODE = 0x50;  // not a GX command; the decoder knows it as ours
constexpr u32 NATIVE_DRAW_PACKETS = 8;

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
    DRAW_SKY = 1,      // the sky dome: follows the camera, no depth test or write, drawn first
};

// Overlays (material.h is_overlay: translucent, depth tested, writing no depth -- the island's Kiwa and
// _blend edge layers, the sea's Kiwa and Nami strips) lie 0.03-0.4 package units over what they cover,
// as meshes of their own whose triangles needn't follow a curved ground's, and that is lost at a
// distance: the 24-bit depth buffer's step is d^2 / (2^24 near) (near 15 Mii units: 0.036 package
// units 300 away, 3.6 units 3,000 away), and the 3DS GPU's 24-bit floats round a position to 16 bits.
// Both backends (g3d_pica.cpp, soft_backend.cpp) draw an overlay with its view-space position scaled
// by (1 - pull) about the eye: on the screen it stays where it was (x/w, y/w don't change), its depth
// comes nearer by that fraction of its distance. The pull is OVERLAY_DEPTH_PULL (0.03 units 30 away,
// 3 units 3,000 away: less than the pixel it covers), but never more than OVERLAY_PULL_LIMIT package
// units at the item's furthest corner: from the overview camera an unlimited pull let the volcano's
// inner blend show through its outer wall (NOMA_SELFTEST=g3drender's sweep). Nothing else is pulled.
constexpr float OVERLAY_DEPTH_PULL = 1.0f / 1024;
constexpr float OVERLAY_PULL_LIMIT = 4.0f;
// The pull of an overlay whose box (bbox, less `origin`: the space `mv` maps from) is under the
// model-view `mv` (guest units; k guest units a package unit). (pull and limit: the test hooks'.)
inline double overlay_pull(const double mv[3][4], const float bbox[6], const float origin[3], double k,
                           double pull = OVERLAY_DEPTH_PULL, double limit = OVERLAY_PULL_LIMIT) {
    double far2 = 0;
    for (int i = 0; i < 8; i++) {
        const double x = (double)bbox[i & 1 ? 3 : 0] - origin[0], y = (double)bbox[i & 2 ? 4 : 1] - origin[1],
                     z = (double)bbox[i & 4 ? 5 : 2] - origin[2];
        double d2 = 0;
        for (int r = 0; r < 3; r++) {
            const double v = mv[r][0] * x + mv[r][1] * y + mv[r][2] * z + mv[r][3];
            d2 += v * v;
        }
        far2 = d2 > far2 ? d2 : far2;
    }
    const double far = std::sqrt(far2) / (k > 0 ? k : 1.0);
    const double cap = far > 0 ? limit / far : pull;
    return cap < pull ? cap : pull;
}

// the animated parts of a material, sampled for this frame
struct MatDynamic {
    u16 material;
    u16 pad;
    s16 tev_reg[4][4];     // PREV, C0-C2
    s16 tev_konst[4][4];   // K0-K3
    u8 mat_color[4], amb_color[4];
    f32 tex_mtx[3][2][4];  // the material's first three texture matrices, 2x4 (S, T rows), final
                           // (scale/rotate/translate of its SRT0 track applied, effect matrix aside)
    engine::gx::Combiners comb;  // the combiners for these registers (luminance remap applied):
                                 // a colour animation can change what the translation folds
};

enum class PacketKind : u8 {
    DRAW,      // draw the items
    PREPARE,   // the backend uploads the package (at load time, under the loading screen)
    RELEASE,   // the backend frees what it made for the package
};

struct Packet {
    u32 id = 0;
    PacketKind kind = PacketKind::DRAW;
    const Package* package = nullptr;
    const Material* materials = nullptr;  // runtime materials, one per package material (the World's)
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
    std::vector<fmt::Fog> fogs;    // start/end in view depth, package units
    f32 time = 0;                  // frames (60 a second), for anything a backend animates itself
    std::vector<f32> matrices;     // 12 floats each (3x4), [0] = identity
    std::vector<MatDynamic> mat_states;
    std::vector<DrawItem> items;   // in draw order

    void reset();                  // empty, for the next frame (keeps the vectors' memory)
};

// Where a native draw lands: the GX state at its command, as a GX draw there would be transformed
// (runtime/hw/gx/xform.cpp, raster.cpp). GX's projection maps view space (guest units) to clip space
// with w = -z; the viewport maps x/w, y/w to EFB pixels (its offsets include GX's 342-pixel bias)
// and z/w (in [-1, 0]) to the 24-bit depth range.
struct GxView {
    f32 projection[4][4];          // XF 0x1020-0x1026, as a matrix
    bool orthographic;
    f32 viewport[6];               // XF 0x101A-0x101F: scale x, y, z; offset x, y, z
    u32 scissor_tl, scissor_br;    // BP 0x20, 0x21 (as set: x in bits 12-22, y in 0-10, 342 bias)
    u32 scissor_offset;            // BP 0x59
};
// the view the GX registers describe now (on the decoder's thread: where native_draw_execute runs)
GxView native_draw_view();

// A backend draws packets: the 3DS GPU's (platform/3ds/g3d_pica.cpp) or the host's software one
// (engine/g3d/soft_backend.cpp). All its calls come from the renderer's thread (the GPU thread on the
// 3DS), in the GX stream's order. prepare() comes once before a package's first draw (from a
// prepare packet, or before the first draw packet that uses it), release() once after its last.
class Backend {
public:
    virtual ~Backend() {}
    // upload what the package's draws need (textures, vertex and index buffers)
    virtual void prepare(Package& package) = 0;
    virtual void draw(const Packet& packet, const GxView& view) = 0;
    // free what prepare made
    virtual void release(Package& package) = 0;
};

// the platform's backend (set at start-up, before the guest runs)
void native_draw_set_backend(Backend* backend);
Backend* native_draw_backend();

// a free packet, reset (blocks while every packet of the ring is still in flight: the renderer's
// thread runs behind); fill it, then submit
Packet* native_draw_begin();
// put the packet's command into the GX FIFO (from the guest CPU's thread, in a scene's draw)
void native_draw_submit(Packet* packet);
// ask the backend to prepare the package now (its own command in the stream, so that uploads happen
// under a loading screen, not at the first draw)
void native_draw_prepare(Package& package);
// the package is about to be freed: waits until the renderer has executed every command so far,
// then has the backend release its resources (on the renderer's thread; on this one if the
// renderer runs none)
void native_draw_release(Package& package);

// For the renderers (GxRenderer::native): execute the command with this id. draw = false: a frame
// the renderer doesn't draw (the host skips most frames when it only saves some) -- the packet is
// retired, prepare and release packets still run.
void native_draw_execute(u32 id, bool draw = true);

// counts since start (for logs and tests)
struct NativeDrawStats {
    u32 submitted, executed, drawn, stale, lost, waits;
};
NativeDrawStats native_draw_stats();

}  // namespace engine::g3d
