// The water effects of a World (world.h): the splash where the Mii falls, jumps or wades into the water,
// and a swimmer's rings, bow spray and foam, drawn natively in the translucent pass from the package's
// water effects (fmt::WaterFx, the WAFX section: an atlas of Wii Sports Resort's effect pictures, two
// materials and four dynamic batches; ng3d_format.h has their layout). WSR has no swimming Mii: its
// splashes are particles of its effect library (nw4r::ef), whose emitters the layers here copy -- each
// layer's numbers are an emitter's in WSR's Effect/<Scene>/effect.carc, named beside it in water_fx.cpp
// (sizes in WSR's units, a Mii 15.24 tall, scaled to the island's Mii; times in sixtieths of a second).
//
// How it runs: the scene asks for a splash (water_splash) and says where the swimmer is (set_swimmer)
// on the game's thread, before the frame's World job; the job's update steps the particles in fixed
// sixtieths of a second (at most six a frame: a host run frame by frame repeats), and its translucent
// build writes their vertices into the packet: the patches lying on the water first (after the trails,
// under the shore's foam strips), the quads in the air last (after every other translucent item, so
// that nothing of the world paints over a splash in front of it; the Mii is opaque, drawn before:
// depth sorts it against a splash). Each batch with something alive is one item, its vertices a range
// of the packet's (native_draw.h DynamicRange). A patch on the sea lies on the waves the grid draws
// (sea.h: its vertices raised by them, as the trails' are); a splash in the air rides them at its
// middle; off the sea (a lake, a pool) or where the grid isn't drawn they lie flat on the water.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "engine/g3d/native_draw.h"
#include "engine/g3d/package.h"
#include "engine/g3d/sea.h"
#include "engine/math/vecmath.h"

namespace engine::g3d {

using engine::math::Vec3;

// The atlas's cells (fmt::WaterFx::cell), in this order (the stager's, tools/wuhu/stage_wuhu.py)
enum WaterFxCell : u8 {
    WFX_SPRAY = 0,     // 0-5: the spray's flipbook (ws2f_waterSp00-15, every third)
    WFX_SPRAY_FRAMES = 6,
    WFX_DROP = 6,      // a glassy drop (rk_mizutama1)
    WFX_PUFF = 7,      // a foam puff (ws2f_waterSand0)
    WFX_CROWN = 8,     // a crown's wall, its foot at u0 and its jagged rim at u1 (ws2n_water01)
    WFX_WAVES = 9,     // a big entry's concentric rings (ws2n_drop_wave)
    WFX_RING = 10,     // a thin ring (pc_glf_ripple_bl02)
    WFX_BUBBLES = 11,  // bubbles (ws2n_bubble01)
    WFX_BOW = 12,      // 12-14: the bow spray's flipbook (ws2n_water_splash1, 3, 5)
    WFX_BOW_FRAMES = 3,
    WFX_FOAM = 15,     // a foam disc (rk_hamon2)
    WFX_CELLS = 16,
};
// its batches (fmt::WaterFx::batch): quads in the air and patches on the water, each with the material
// that multiplies the picture's alpha by the vertices' or the one that subtracts the vertices' from it
enum WaterFxBatch : u8 { WFX_AIR_MUL, WFX_AIR_ERODE, WFX_SURF_MUL, WFX_SURF_ERODE, WFX_BATCHES };

// A splash where something entered the water (package units).
struct WaterSplash {
    enum Kind : u8 {
        ENTRY,    // a fall, jump, glide or slide in: spray, drops, foam and a ring; from strength 0.6 the
                  // crowns and big rings, from 1.0 a column of bubbles too
        DROP_IN,  // out of a boat over its side: an entry (its strength small)
        HAND,     // a hand going in as the swimmer strokes: a little bow spray
        RING,     // a ring only (walking in off a beach)
    };
    Vec3 at;            // the water's surface where it went in
    f32 strength;       // 0.15 (a glide) to 1.5 (a jump off the plaza's sea front); 1: a fall from the
                        // deepest drop allowed
    f32 dir_x, dir_z;   // the way it moved (unit; 0, 0: straight down)
    f32 speed;          // its speed that way (package units a second)
    f32 scale;          // the Mii's height over WSR's (15.24): what WSR's sizes are multiplied by
    u8 kind;            // Kind
    bool on_sea;        // the sea's water (sea.h waves under it), not a lake's or a pool's
};

// The swimmer, every frame (on false: not in the water).
struct Swimmer {
    bool on;
    Vec3 at;            // its feet (as drawn: on the sea, moved with the waves)
    f32 yaw_deg;        // its heading: (sin, cos) of it along x, z
    f32 speed;          // package units a second
    f32 scale;          // the Mii's height over WSR's (15.24)
    bool on_sea;
    f32 surface = __builtin_nanf("");  // the water's height over it (NaN: SWIM_SINK of the Mii's height
                                       // over its feet, where player.cpp hangs a swimmer)
};

// what a frame's effects are seen with (package units)
struct WaterFxView {
    Vec3 eye, right, up;        // the camera, its right and up (unit)
    f32 tan_half_fovy;
    const Sea* sea;             // the package's sea (nullptr: none)
    bool grid_on;               // the wave grid is drawn this frame, centred at grid_c (world.h)
    f32 grid_c[2];
    double seconds;             // the World's clock
};

// one batch's draw: its batch, its range of the packet's vertices, its triangles
struct WaterFxDraw {
    u16 batch;
    DynamicRange range;
    u32 triangles;
};

class WaterFx {
public:
    // the package's water effects (fmt::WaterFx; Package::load checked its batches' layout and indices)
    explicit WaterFx(const Package& package);
    ~WaterFx();
    WaterFx(const WaterFx&) = delete;
    WaterFx& operator=(const WaterFx&) = delete;

    // (the game's thread, before the frame's job: what the next step takes)
    void splash(const WaterSplash& s);
    void set_swimmer(const Swimmer& s);
    // the effects at `frame` (sixtieths of a second, the World's clock): the splashes asked for made,
    // the swimmer's rings and spray, every particle moved on
    void step(f32 frame);
    // The frame's patches (surface) or quads into the packet: their vertices appended to `dynamic`
    // (each batch's 16-byte aligned), about origin() (the items' matrix: a translation); a WaterFxDraw
    // for each batch with something to draw (at most two). How many.
    u32 build(Packet* p, bool surface, const WaterFxView& view, WaterFxDraw out[2]);
    // where the vertices are written from (near the newest effect: small numbers for the 3DS GPU's
    // 24-bit floats)
    const f32* origin() const { return origin_; }
    // the last build's quads and patches
    u32 quads() const { return quads_; }
    u32 patches() const { return patches_; }

private:
    struct Particle;
    struct Swim;
    void spawn_splash(const WaterSplash& s);
    void spawn_swim(f32 t);
    void advance();
    f32 surface_at(f32 x, f32 z, f32 y0, bool on_sea);
    void patch_waves(f32 cx, f32 cz, f32 ax, f32 az, f32 out[9]);
    u32 write_batch(u8 batch, u8* out, u32 cap, const WaterFxView& v);
    void add(const Particle& q);

    const fmt::WaterFx& rec_;
    std::vector<Particle> parts_;
    std::vector<WaterSplash> asked_;
    std::unique_ptr<Swim> swim_;
    f32 last_ = __builtin_nanf("");  // the frame the last step reached
    u32 steps_ = 0;                  // sixtieths stepped so far
    f32 origin_[3] = {0, 0, 0};
    u32 quads_ = 0, patches_ = 0;
    // (a build's waves: made once for a frame's two builds)
    SeaWaves waves_;
    double waves_t_ = -1;
    f32 waves_c_[2] = {0, 0};
    bool grid_on_ = false;
    const Sea* sea_ = nullptr;
};

}  // namespace engine::g3d
