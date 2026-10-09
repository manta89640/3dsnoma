// The water effects (water_fx.h): Wii Sports Resort's water emitters as layers of particles, a splash
// and a swimmer made of them, the particles stepped a sixtieth at a time and written as quads and
// patches.
#include "engine/g3d/water_fx.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "runtime/runtime.h"

namespace engine::g3d {

using namespace engine::math;

namespace {

constexpr f32 PI = 3.14159265f;
constexpr f32 WSR_MII = 15.24f;   // WSR's Mii's height, its units (WaterSplash::scale's 1)
constexpr u32 STRIDE = 24;        // a vertex (the trails' layout): f32 position, u8 RGBA, f32 texture coordinates
constexpr u32 MAX_STEPS = 6;      // sixtieths a step takes at most (a longer frame, a pause: the rest dropped)
constexpr u32 MAX_PARTICLES = 384;
constexpr u32 MAX_ASKED = 8;      // splashes a frame at most
constexpr f32 LIFT = 0.4f;        // a patch over the water (world.cpp's TRAIL_LIFT: an overlay, pulled to the eye)
constexpr f32 SCREEN_CAP = 0.3f;  // a billboard at most this of the screen's height at its distance (fill rate)
constexpr u32 CROWN_SEGMENTS = 16;
// how far a crown's wall leans out, of its emitter's rotation (WSR's stripe crowns turn from 60 and 90
// degrees to none as they live: as a lean, two fifths of that flares the rim as WSR's crowns look)
constexpr f32 CROWN_LEAN = 0.4f;
// a crown's foot this far (of its height) under the water, so that the water's edge hides it
constexpr f32 CROWN_FOOT = 0.1f;

// the swimmer (Mii heights, a second): it strokes from SWIM_START, treads water under SWIM_STOP
// (island_mii.cpp's thresholds: 0.8 and 0.4 guest units a sixtieth for a Mii 158 tall); its feet hang
// SWIM_SINK under the water (player.cpp's)
constexpr f32 SWIM_START = 0.30f, SWIM_STOP = 0.15f, SWIM_SINK = 0.62f;
constexpr f32 SWIM_FULL = 1.4f;        // its full swimming speed (55% of a run): the bow spray's whole
constexpr f32 RING_EVERY = 0.4f;       // a ring each this many Mii heights swum ...
constexpr u32 RING_TREAD = 48;         // ... or sixtieths treading water
constexpr u32 RINGS_ALIVE = 6;
constexpr f32 RING_AHEAD = 0.3f;       // a swimming ring's middle ahead of the feet (Mii heights)
constexpr u32 BOW_EVERY = 3;           // sixtieths between a pair of bow spray
constexpr f32 BOW_AHEAD = 0.55f, BOW_SIDE = 0.08f;  // where it rises: at the head, either side (Mii heights)
constexpr f32 BOW_ANGLE = 55.0f * PI / 180.0f;     // thrown back this far from straight behind
// the collar, foam where the body meets the water (hiding the hard edge): COLLAR_RADIUS of the Mii's
// height about it (COLLAR_AHEAD ahead of the feet swimming), eaten away to COLLAR_ALPHA at most, faded
// in or out over COLLAR_FADE sixtieths
constexpr f32 COLLAR_RADIUS = 0.45f, COLLAR_AHEAD = 0.35f, COLLAR_ALPHA = 0.3f;
constexpr u32 COLLAR_FADE = 15;
constexpr u32 DRIP_TIME = 120, DRIP_EVERY = 6;  // out of the water: a drop each DRIP_EVERY sixtieths for DRIP_TIME
constexpr u32 TEST_EVERY = 180;        // NOMA_WUHU_WATER_FX_TEST: a splash every 3 s treading water

// WS2_swf_watcolumn01's root scale, which its children's sizes and speeds take (WSR's 0.775: a Mii thrown
// off Swordplay's high platform), here 0.6 of it: at WSR's whole a big entry's crowns stood twice the
// Mii's height and filled the follow camera's view (host screenshots)
constexpr f32 WATCOL = 0.775f * 0.6f;

enum Shape : u8 {
    BILLBOARD,  // a quad facing the camera, turned by its rotation
    SHEET,      // a quad standing up (the world's y), facing the camera, about its middle
    BOW,        // ... standing on its foot (spray rising off the water)
    CROWN,      // an open cylinder of CROWN_SEGMENTS quads, the whole picture each around, its foot under
                // the water: WSR's crown, a ring of stripe particles
    PATCH,      // 3 x 3 vertices lying on the water
};

struct Key {
    f32 t, x, y;  // t of its life (0-1); its value (and a sheet's height's scale)
};
constexpr int MAX_KEYS = 7;

// A layer: one of WSR's emitters (or two, mirrored), as its particles behave and are drawn.
struct Layer {
    u8 shape, batch;   // Shape, WaterFxBatch
    u8 cell, cells;    // its picture (a flipbook: the first of `cells`)
    f32 flip;          // sixtieths each picture of the flipbook shows
    u16 life;          // sixtieths
    f32 life_rnd;      // up to this much of it shorter
    f32 size_x, size_y;  // WSR units (a crown: its wall's height; a patch: across)
    f32 gravity, drag; // WSR units a sixtieth each sixtieth; the velocity's factor each sixtieth
    f32 spin;          // radians turned over its life, from a random start
    u32 rgb;           // its primary colour: the materials colour a texel from their K0 (the secondary,
                       // WSR's c0sec) to it by the picture's intensity
    bool sink;         // gone once it falls back through the water
    u8 n_scale, n_alpha, n_lean;
    Key scale[MAX_KEYS];
    Key alpha[MAX_KEYS];  // multiply: its alpha (0-1); erode: what is eaten of the picture's alpha
    Key lean[MAX_KEYS];   // a crown's: radians (times CROWN_LEAN)
};

enum LayerId : u8 {
    L_SPRAY, L_SHEETS, L_DROPS, L_PUFFS, L_RING, L_WAVES1, L_WAVES2, L_CROWN1, L_CROWN2, L_BUBBLES, L_SWIM_RING,
    L_BOW, L_HAND, L_DRIP, N_LAYERS
};

// The layers. Their numbers are the emitters' (scratch dumps of the .breff: emitter, particle and curve
// records; baked curves read at a few keys); what isn't WSR's is said.
const Layer k_layers[N_LAYERS] = {
    // L_SPRAY, Effect/JskScene: WS2_jsk_pocha_splash (the jet ski's plop): 3 a sixtieth for 5, on a ring of
    // radius 4, 1.05 up and 0.125 out (21% random), the ws2f_waterSp flipbook (17 pictures over 33
    // sixtieths: our six every third); its alpha by erosion, the picture appearing in 4 sixtieths and
    // eaten away from 10 on
    {BILLBOARD, WFX_AIR_ERODE, WFX_SPRAY, WFX_SPRAY_FRAMES, 33.0f * 3 / 17, 45, 0.4f, 4.06f, 4.06f, 0.0375f, 0.98f,
     0.873f, 0xFFFFFF, false, 3, 6, 0,
     {{0, 0.25f, 0.25f}, {0.136f, 1, 1}, {1, 1.5f, 1.5f}},
     {{0, 1}, {0.09f, 0}, {0.23f, 0}, {0.5f, 0.51f}, {0.75f, 0.86f}, {1, 1}},
     {}},
    // L_SHEETS, WS2_jsk_pocha_crown1: upright sheets of foam (directional along y), 2 a sixtieth for 4 on
    // a ring of radius 6, from 1 under the water, 0.4 up, 0.1 out, slowed 0.9; 6.2 x 3.56, stretching
    // from 0.4 x 0.5 to 1 x 3.8; erosion. (Its picture ws2f_waterSand0IA8: the foam puff's cell, the
    // same picture.)
    {SHEET, WFX_AIR_ERODE, WFX_PUFF, 1, 0, 30, 0.44f, 6.2f, 3.56f, 0, 0.9f, 0, 0xFFFFFF, false, 2, 5, 0,
     {{0, 0.4f, 0.5f}, {1, 1, 3.8f}},
     {{0, 1}, {0.14f, 0.2f}, {0.38f, 0.2f}, {0.7f, 0.8f}, {1, 1}},
     {}},
    // L_DROPS, WS2_jsk_pocha_drop: glassy drops (rk_mizutama1), on a ring of radius 3 times its emitter's
    // 1.8, 1.25 up and 0.4 out (41% random; a big entry's up to 2.5 and 0.9: the plan's), falling at
    // 0.0543, slowed 0.95; 0.75 across times 1.8, shrinking to a tenth
    {BILLBOARD, WFX_AIR_MUL, WFX_DROP, 1, 0, 40, 0.22f, 1.35f, 1.35f, 0.0543f, 0.95f, 0, 0xFFFFFF, true, 5, 2, 0,
     {{0, 1, 1}, {0.25f, 0.92f, 0.92f}, {0.5f, 0.71f, 0.71f}, {0.75f, 0.41f, 0.41f}, {1, 0.1f, 0.1f}},
     {{0, 1}, {1, 1}},
     {}},
    // L_PUFFS, WS2_jsk_pocha_awa: foam puffs (ws2f_waterSand0), here lying on the water as patches (WSR's
    // are billboards, 12 a sixtieth for 5: up to 6 here), drifting out from a ring of radius 5 (not
    // WSR's), slowed 0.96; 5.8 across, from a quarter to 2.31; fading in and out, turning half a radian
    {PATCH, WFX_SURF_MUL, WFX_PUFF, 1, 0, 100, 0.35f, 5.8f, 5.8f, 0, 0.96f, 0.524f, 0xFFFFFF, false, 3, 5, 0,
     {{0, 0.25f, 0.25f}, {0.15f, 1.15f, 1.15f}, {1, 2.31f, 2.31f}},
     {{0, 0}, {0.2f, 1}, {0.5f, 0.67f}, {0.75f, 0.22f}, {1, 0}},
     {}},
    // L_RING, Effect/GlfScene: WS2_glf_ike01_hamon (WS2_glf_ikepocha's ring, a ball into a pond): a thin
    // ring (pc_glf_ripple_bl02) 2.7 across, from half to 8 times that in 80 sixtieths, fading by 57
    {PATCH, WFX_SURF_MUL, WFX_RING, 1, 0, 80, 0, 2.7f, 2.7f, 0, 1, 0, 0xFFFFFF, false, 2, 3, 0,
     {{0, 0.5f, 0.5f}, {1, 8, 8}},
     {{0, 1}, {0.71f, 0}, {1, 0}},
     {}},
    // L_WAVES1, Effect/SwfScene: WS2_swf_watcol_wave1 (WS2_swf_watcolumn01, a Mii falling into the sea in
    // Swordplay; its root scale WATCOL): concentric rings (ws2n_drop_wave) 20 to 70 across in 40
    {PATCH, WFX_SURF_MUL, WFX_WAVES, 1, 0, 40, 0, WATCOL, WATCOL, 0, 1, 0, 0xFFFFFF, false, 2, 6, 0,
     {{0, 20, 20}, {1, 70, 70}},
     {{0, 0}, {0.23f, 1}, {0.44f, 0.32f}, {0.67f, 0.11f}, {0.82f, 0.03f}, {1, 0}},
     {}},
    // L_WAVES2, WS2_swf_watcol_wave2: the second, 10 to 70 in 125
    {PATCH, WFX_SURF_MUL, WFX_WAVES, 1, 0, 125, 0, WATCOL, WATCOL, 0, 1, 0, 0xFFFFFF, false, 2, 6, 0,
     {{0, 10, 10}, {1, 70, 70}},
     {{0, 0}, {0.25f, 0.78f}, {0.46f, 0.32f}, {0.63f, 0.11f}, {0.81f, 0.03f}, {1, 0}},
     {}},
    // L_CROWN1, WS2_swf_watcol_crown1: a ring of 20 stripe particles (ws2n_water01) from radius 1.5
    // going out at 1.2 a sixtieth, a wall 15 high times 0.7 rising to 1.4 and falling to nothing in 40
    // (times WATCOL), fading from 26; turning 60 degrees to none (here a lean)
    {CROWN, WFX_AIR_MUL, WFX_CROWN, 1, 0, 40, 0, 0, 15 * WATCOL, 0, 1, 0, 0xFFFFFF, false, 6, 5, 7,
     {{0, 0.7f, 0.7f}, {0.125f, 1.3f, 1.3f}, {0.275f, 1.4f, 1.4f}, {0.45f, 1.24f, 1.24f}, {0.55f, 1, 1}, {1, 0, 0}},
     {{0, 1}, {0.65f, 1}, {0.73f, 0.25f}, {0.85f, 0.06f}, {1, 0}},
     {{0, 1.05f}, {0.275f, 0.99f}, {0.55f, 0.82f}, {0.675f, 0.56f}, {0.775f, 0.17f}, {0.875f, 0.06f}, {1, 0}}},
    // L_CROWN2, WS2_swf_watcol_crown2: the inner one, 6 sixtieths later, from radius 0.75 at 0.7, half
    // as high rising to 2.2 in 14 and falling by 59; turning 90 degrees to none
    {CROWN, WFX_AIR_MUL, WFX_CROWN, 1, 0, 60, 0, 0, 15 * WATCOL, 0, 1, 0, 0xFFFFFF, false, 7, 5, 6,
     {{0, 0.5f, 0.5f}, {0.117f, 2.1f, 2.1f}, {0.233f, 2.2f, 2.2f}, {0.417f, 1.81f, 1.81f}, {0.6f, 0.95f, 0.95f},
      {0.783f, 0.09f, 0.09f}, {1, 0, 0}},
     {{0, 1}, {0.63f, 1}, {0.72f, 0.24f}, {0.82f, 0.06f}, {1, 0}},
     {{0, 1.57f}, {0.3f, 1.43f}, {0.6f, 1.04f}, {0.72f, 0.36f}, {0.82f, 0.1f}, {1, 0}}},
    // L_BUBBLES, WS2_swf_watcol_bubble: a column of bubbles (ws2n_bubble01), 5 from a disc of radius 1.5
    // (from 48% out), 2.5 up (30% random), falling at 0.06; 3.75 x 6.25 times 1 x 0.5 swelling to 1.79 x
    // 2.15; seen 18 sixtieths (WSR's particles live 135, its alpha curve gone by 18: a life of 20 here)
    {BILLBOARD, WFX_AIR_MUL, WFX_BUBBLES, 1, 0, 20, 0.2f, 3.75f * WATCOL, 6.25f * WATCOL, 0.06f * WATCOL, 1, 0, 0xFFFFFF,
     true, 4, 5, 0,
     {{0, 1, 0.5f}, {0.45f, 1.79f, 2.15f}, {0.75f, 1.6f, 2}, {1, 1, 0.9f}},
     {{0, 0.78f}, {0.5f, 0.78f}, {0.6f, 0.39f}, {0.72f, 0.08f}, {0.9f, 0}},
     {}},
    // L_SWIM_RING: the swimmer's rings, the plan's (WSR has no swimmer): pc_glf_ripple_bl02 from 0.3 to
    // 1.3 Mii heights in radius over 1.6 s, alpha 0.55 to none
    {PATCH, WFX_SURF_MUL, WFX_RING, 1, 0, 96, 0, 2 * WSR_MII, 2 * WSR_MII, 0, 1, 0, 0xFFFFFF, false, 2, 3, 0,
     {{0, 0.3f, 0.3f}, {1, 1.3f, 1.3f}},
     {{0, 0.55f}, {0.5f, 0.22f}, {1, 0}},
     {}},
    // L_BOW, Effect/CanScene: WS2_can_water_line_duck_L/R (the canoe race's swimming ducks): upright
    // spray standing on the water (ws2n_water_splash1-5 over 40 sixtieths: our three), thrown back at 0.5
    // a sixtieth, slowed 0.96, falling at 0.015, shrinking to nothing; colour ebffff. WSR's are 1 across
    // (the ducks' size): 0.14 of a Mii's height here (the plan's 0.22 a row of white bursts on the host)
    {BOW, WFX_AIR_MUL, WFX_BOW, WFX_BOW_FRAMES, 40.0f / 3, 40, 0, 0.14f * WSR_MII, 0.14f * WSR_MII, 0.015f, 0.96f, 0,
     0xEBFFFF, true, 2, 2, 0,
     {{0, 1, 1}, {1, 0, 0}},
     {{0, 1}, {1, 1}},
     {}},
    // L_HAND: a hand going in, a small burst of the ducks' spray (not WSR's: its flipbook faster)
    {BOW, WFX_AIR_MUL, WFX_BOW, WFX_BOW_FRAMES, 8, 24, 0.2f, 0.15f * WSR_MII, 0.15f * WSR_MII, 0.03f, 0.96f, 0,
     0xEBFFFF, true, 3, 3, 0,
     {{0, 0.6f, 0.6f}, {0.3f, 1, 1}, {1, 0.4f, 0.4f}},
     {{0, 1}, {0.6f, 1}, {1, 0}},
     {}},
    // L_DRIP, Effect/SwfScene: WS2_swf_water_drop_walk (a Mii walking out of the sea): drops off the body
    // (its ws2n_drop01 not in the atlas: the glassy drop), d9fffc, swelling to 0.44 and gone in 45; here
    // falling at 0.06 (WSR's hang, carried by the Mii) over 30, 1.5 across
    {BILLBOARD, WFX_AIR_MUL, WFX_DROP, 1, 0, 30, 0.2f, 1.5f, 1.5f, 0.06f, 0.98f, 0, 0xD9FFFC, true, 6, 2, 0,
     {{0, 0, 0}, {0.11f, 0.4f, 0.4f}, {0.33f, 0.44f, 0.44f}, {0.58f, 0.4f, 0.4f}, {0.82f, 0.34f, 0.34f}, {1, 0, 0}},
     {{0, 1}, {1, 1}},
     {}},
};

// a curve at t: linear between its keys, its ends held
f32 curve(const Key* k, u32 n, f32 t, bool y = false) {
    if (!n)
        return 1;
    if (t <= k[0].t)
        return y ? k[0].y : k[0].x;
    for (u32 i = 1; i < n; i++)
        if (t <= k[i].t) {
            const f32 a = (t - k[i - 1].t) / std::max(k[i].t - k[i - 1].t, 1e-6f);
            const f32 v0 = y ? k[i - 1].y : k[i - 1].x, v1 = y ? k[i].y : k[i].x;
            return v0 + (v1 - v0) * a;
        }
    return y ? k[n - 1].y : k[n - 1].x;
}

// a small random number generator (a linear congruential one): 0 <= next() < 1
struct Rng {
    u32 s;
    f32 next() {
        s = s * 1664525u + 1013904223u;
        return (f32)(s >> 8) * (1.0f / 16777216.0f);
    }
    f32 sym() { return 2 * next() - 1; }  // -1 <= sym() < 1
};

u32 bits(f32 v) {
    u32 b;
    memcpy(&b, &v, 4);
    return b;
}

// a seed from a time and a place (each splash its own numbers, the same in every run)
u32 seed(u32 t, f32 x, f32 z, u32 salt) {
    u32 h = 2166136261u;
    for (u32 v : {t, bits(x), bits(z), salt})
        h = (h ^ v) * 16777619u;
    return h;
}

u8 clamp_u8(f32 v) { return (u8)std::lround(std::min(255.0f, std::max(0.0f, v * 255.0f))); }

// the crowns' ring: (cos, sin) of each segment's edge, round once (CROWN_SEGMENTS of them)
constexpr f32 k_ring[CROWN_SEGMENTS + 1][2] = {
    {1, 0}, {0.92387953f, 0.38268343f}, {0.70710678f, 0.70710678f}, {0.38268343f, 0.92387953f},
    {0, 1}, {-0.38268343f, 0.92387953f}, {-0.70710678f, 0.70710678f}, {-0.92387953f, 0.38268343f},
    {-1, 0}, {-0.92387953f, -0.38268343f}, {-0.70710678f, -0.70710678f}, {-0.38268343f, -0.92387953f},
    {0, -1}, {0.38268343f, -0.92387953f}, {0.70710678f, -0.70710678f}, {0.92387953f, -0.38268343f},
    {1, 0}};
static_assert(CROWN_SEGMENTS == 16, "k_ring's edges");

}  // namespace

// a particle: x, z where it is, y over the water at its anchor (its splash's middle, a bow spray's pair's
// head, or where it was made); package units, velocities a sixtieth
struct WaterFx::Particle {
    f32 x, y, z;
    f32 vx, vy, vz;
    f32 ax, az;          // its anchor: where the water under it is measured
    f32 y0;              // the water's height there when it was made (what it lies on off the sea)
    f32 g;               // package units a WSR unit (its splash's size)
    f32 radius, grow;    // a crown's radius, and its growth a sixtieth
    f32 rot;             // radians (its spin added over its life)
    f32 fade;            // its alpha's factor
    u16 age, life;       // sixtieths
    u16 delay;           // sixtieths before it begins
    u8 layer;
    bool on_sea;         // the sea's water (else a lake's or a pool's, or none: the drips)
};

// the swimmer: this frame's state and the last's, what its rings, spray, collar and drips have done
struct WaterFx::Swim {
    Swimmer now{}, was{};
    bool known = false;      // `was` is a frame's
    Vec3 last{0, 0, 0};      // where it was at the last sixtieth
    f32 swum = 0;            // units since the last ring
    u32 still = 0, bow = 0;  // sixtieths treading since the last ring; since the last bow spray
    f32 collar = 0;          // its opacity (0-1)
    u32 drips = 0;           // sixtieths of dripping left
    u32 test = 0, test_n = 0;
    Rng rng{0x2545F491u};
};

WaterFx::WaterFx(const Package& package) : rec_(*package.water_fx()), swim_(new Swim) {
    parts_.reserve(MAX_PARTICLES);
    asked_.reserve(MAX_ASKED);
}

WaterFx::~WaterFx() {}

void WaterFx::splash(const WaterSplash& s) {
    if (asked_.size() < MAX_ASKED && std::isfinite(s.at.x) && std::isfinite(s.at.y) && std::isfinite(s.at.z))
        asked_.push_back(s);
}

void WaterFx::set_swimmer(const Swimmer& s) { swim_->now = s; }

// a particle into the pool: the oldest make way when it's full
void WaterFx::add(const Particle& q) {
    if (parts_.size() >= MAX_PARTICLES)
        parts_.erase(parts_.begin(), parts_.begin() + MAX_PARTICLES / 8);
    parts_.push_back(q);
}

// A splash's particles (water_fx.h WaterSplash). g: package units a WSR unit, the Mii's size times 0.7
// to 1 by the strength (and a little more past 1): a small one smaller as well as fewer.
void WaterFx::spawn_splash(const WaterSplash& s) {
    Rng r{seed(steps_, s.at.x, s.at.z, s.kind)};
    const f32 st = std::max(0.0f, std::min(s.strength, 2.0f)), sm = std::min(st, 1.0f);
    const f32 big = std::max(0.0f, std::min((st - 0.5f) / 1.0f, 1.0f));
    const f32 scale = s.scale > 0 ? s.scale : 1.0f;
    const f32 g = scale * (0.7f + 0.3f * sm) * (1 + 0.25f * std::max(st - 1, 0.0f));
    f32 dl = std::sqrt(s.dir_x * s.dir_x + s.dir_z * s.dir_z);
    const f32 dx = dl > 1e-6f ? s.dir_x / dl : 0, dz = dl > 1e-6f ? s.dir_z / dl : 0;
    // (the way it moved carries the spray and drops a little: at a run's speed a tenth of their rise)
    const f32 carry = 0.12f * std::min(std::max(s.speed, 0.0f) / 60.0f, 1.0f) * g;
    auto make = [&](u8 layer, f32 gg) {
        const Layer& L = k_layers[layer];
        Particle q = {};
        q.x = s.at.x, q.z = s.at.z, q.ax = s.at.x, q.az = s.at.z, q.y0 = s.at.y;
        q.g = gg;
        q.rot = L.spin != 0 || L.shape == BILLBOARD || L.shape == PATCH || L.shape == CROWN ? 2 * PI * r.next() : 0;
        q.fade = 1;
        q.life = (u16)std::max(1.0f, std::round(L.life * (1 - L.life_rnd * r.next())));
        q.layer = layer;
        q.on_sea = s.on_sea;
        return q;
    };
    // n of a layer (at g's size, or gg's) as an emitter makes them (WSR units): from a ring of `radius`
    // (`inner` of it out: 1 on it), `up` and `out` a sixtieth (`rnd` of that random), `y` over the
    // water; the first `start` sixtieths after the splash, the rest each `per` after the last
    struct Emit {
        f32 radius, inner, up, out, rnd, per, start, y;
    };
    auto emit = [&](u8 layer, u32 n, const Emit& e, f32 gg = 0) {
        gg = gg > 0 ? gg : g;
        for (u32 i = 0; i < n; i++) {
            Particle q = make(layer, gg);
            const f32 a = 2 * PI * r.next(), rr = e.radius * (e.inner + (1 - e.inner) * r.next()) * gg;
            const f32 cx = std::cos(a), cz = std::sin(a);
            q.x += cx * rr, q.z += cz * rr;
            q.y = e.y * gg;
            const f32 k = (1 - e.rnd * r.next()) * gg;
            q.vx = cx * e.out * k + dx * carry, q.vz = cz * e.out * k + dz * carry, q.vy = e.up * k;
            q.delay = (u16)(e.start + e.per * (f32)i);
            add(q);
        }
    };
    if (s.kind == WaterSplash::RING) {
        emit(L_RING, 1, {0, 0, 0, 0, 0, 0, 0, 0}, scale);
        return;
    }
    if (s.kind == WaterSplash::HAND) {
        emit(L_HAND, 3, {0.4f, 0, 0.5f, 0.25f, 0.4f, 1, 0, 0}, scale);
        emit(L_DROPS, 2, {0.3f, 0, 0.8f, 0.3f, 0.4f, 0, 0, 0}, scale * 0.7f);
        return;
    }
    // a big entry's crowns first (when the batch is full, what came first is drawn)
    if (st >= 0.6f) {
        for (int c = 0; c < 2; c++) {
            Particle q = make(c ? L_CROWN2 : L_CROWN1, g);
            q.radius = (c ? 0.75f : 1.5f) * WATCOL * g;
            q.grow = (c ? 0.7f : 1.2f) * WATCOL * g;
            q.delay = c ? 6 : 0;
            add(q);
        }
    }
    emit(L_SPRAY, (u32)std::lround(9 + 6 * sm), {4, 1, 1.05f, 0.125f, 0.21f, 1.0f / 3, 0, 0});
    emit(L_SHEETS, 8, {6, 1, 0.4f, 0.1f, 0.22f, 0.5f, 0, -1});
    // (the drops: more and faster in a big entry)
    emit(L_DROPS, (u32)std::lround(8 + 16 * big), {3 * 1.8f, 1, 1.25f + 1.25f * big, 0.4f + 0.5f * big, 0.41f, 0.5f, 0, 0});
    emit(L_PUFFS, (u32)std::lround(3 + 3 * sm), {5, 0.3f, 0, 0.25f, 0.5f, 2, 0, 0});
    if (st < 0.6f) {
        emit(L_RING, 1, {0, 0, 0, 0, 0, 0, 0, 0});
    } else {
        // (WSR's watcolumn01 starts its crown at 4 sixtieths, its waves at 13 and 17, its bubbles at 20:
        // here the crown at once, the rest as much sooner)
        emit(L_WAVES1, 1, {0, 0, 0, 0, 0, 0, 9, 0});
        emit(L_WAVES2, 1, {0, 0, 0, 0, 0, 0, 13, 0});
    }
    if (st >= 1.0f)
        emit(L_BUBBLES, 5, {1.5f * WATCOL, 0.48f, 2.5f * WATCOL, 0, 0.3f, 0, 16, 0});
}

// The swimmer's particles at its place a fraction t of the way through this frame: a ring each
// RING_EVERY of its height swum or RING_TREAD sixtieths treading; bow spray either side of its head
// while it swims; drips for a while after it leaves the water; the test's splashes.
void WaterFx::spawn_swim(f32 t) {
    Swim& w = *swim_;
    const Swimmer& s = w.now;
    const f32 scale = s.scale > 0 ? s.scale : 1.0f, h = WSR_MII * scale;
    const Vec3 at = w.was.at + (s.at - w.was.at) * t;
    const f32 moved = std::sqrt((at.x - w.last.x) * (at.x - w.last.x) + (at.z - w.last.z) * (at.z - w.last.z));
    w.last = at;
    const f32 surface = std::isfinite(s.surface) ? s.surface : at.y + SWIM_SINK * h;
    const f32 yr = s.yaw_deg * PI / 180.0f;
    const f32 fx = std::sin(yr), fz = std::cos(yr);  // ahead
    const f32 sx = fz, sz = -fx;                     // to its right
    const f32 speed = std::max(0.0f, s.speed);
    const bool moving = speed > SWIM_STOP * h;
    const f32 pace = std::min(1.0f, speed / (SWIM_START * h));  // (treading 0, swimming 1)
    if (!s.on) {
        w.swum = 0, w.still = 0, w.bow = 0;
        w.test = TEST_EVERY - 60;
        if (w.drips && w.drips-- % DRIP_EVERY == 0) {
            Particle q = {};
            const f32 a = 2 * PI * w.rng.next(), rr = 2.5f * scale * std::sqrt(w.rng.next());
            q.x = at.x + std::cos(a) * rr, q.z = at.z + std::sin(a) * rr;
            q.ax = q.x, q.az = q.z, q.y0 = at.y;
            q.y = (0.3f + 0.25f * w.rng.next()) * h;
            q.g = scale, q.fade = 1, q.life = k_layers[L_DRIP].life, q.layer = L_DRIP;
            add(q);
        }
        return;
    }
    w.drips = DRIP_TIME;
    auto particle = [&](u8 layer, f32 x, f32 z) {
        Particle q = {};
        q.x = x, q.z = z, q.ax = x, q.az = z, q.y0 = surface;
        q.g = scale, q.fade = 1, q.layer = layer, q.on_sea = s.on_sea;
        q.life = k_layers[layer].life;
        return q;
    };
    // rings: by the distance swum, or the time treading; RINGS_ALIVE at most
    bool ring = false;
    if (moving) {
        w.still = 0;
        // (a jump -- a warp, the first frame -- isn't swimming)
        w.swum += moved < h ? moved : 0;
        ring = w.swum >= RING_EVERY * h;
    } else {
        ring = ++w.still >= RING_TREAD;
    }
    if (ring) {
        w.swum = 0, w.still = 0;
        u32 alive = 0;
        for (const Particle& q : parts_)
            alive += q.layer == L_SWIM_RING;
        if (alive < RINGS_ALIVE) {
            Particle q = particle(L_SWIM_RING, at.x + fx * RING_AHEAD * h * pace, at.z + fz * RING_AHEAD * h * pace);
            q.rot = 2 * PI * w.rng.next();
            add(q);
        }
    }
    // the bow spray: a pair each BOW_EVERY sixtieths, thrown back either side, as fast as it swims
    if (moving && ++w.bow >= BOW_EVERY) {
        w.bow = 0;
        const f32 f = std::max(0.3f, std::min(1.0f, speed / (SWIM_FULL * h)));
        // (the pair rides the water where they rose: one anchor, the head)
        const f32 ax = at.x + fx * BOW_AHEAD * h, az = at.z + fz * BOW_AHEAD * h;
        for (int side = -1; side <= 1; side += 2) {
            Particle q = particle(L_BOW, ax, az);
            q.x += sx * side * BOW_SIDE * h, q.z += sz * side * BOW_SIDE * h;
            // (55 degrees from straight behind, 20 either way at random; 22 degrees up: WSR's 0.5 a
            // sixtieth as 0.46 out and 0.19 up)
            const f32 a = BOW_ANGLE + 0.35f * w.rng.sym();
            const f32 bx = -fx * std::cos(a) + sx * side * std::sin(a), bz = -fz * std::cos(a) + sz * side * std::sin(a);
            const f32 v = 0.46f * scale * f;
            q.vx = bx * v, q.vz = bz * v, q.vy = 0.19f * scale * f;
            q.fade = 0.45f + 0.3f * f;
            add(q);
        }
    }
    // NOMA_WUHU_WATER_FX_TEST=1: treading water, a splash every TEST_EVERY sixtieths (strengths 0.3, 1.0,
    // 1.5 in turn) just ahead of it, for screenshots
    static const bool test = getenv("NOMA_WUHU_WATER_FX_TEST") && atoi(getenv("NOMA_WUHU_WATER_FX_TEST"));
    if (test && !moving && ++w.test >= TEST_EVERY) {
        static const f32 k_strength[3] = {0.3f, 1.0f, 1.5f};
        w.test = 0;
        WaterSplash sp = {{at.x + fx * 0.2f * h, surface, at.z + fz * 0.2f * h}, k_strength[w.test_n++ % 3], 0, 0, 0,
                          scale, WaterSplash::ENTRY, s.on_sea};
        guest::log("wuhu: water: test splash, strength %.1f", (double)sp.strength);
        spawn_splash(sp);
    }
}

// a sixtieth: every particle aged and moved (gravity and drag as nw4r::ef's fields: the velocity slowed,
// then pulled down), the dead dropped
void WaterFx::advance() {
    for (Particle& q : parts_) {
        if (q.delay) {
            q.delay--;
            continue;
        }
        if (++q.age >= q.life) {
            q.life = 0;
            continue;
        }
        const Layer& L = k_layers[q.layer];
        q.vx *= L.drag, q.vy *= L.drag, q.vz *= L.drag;
        q.vy -= L.gravity * q.g;
        q.x += q.vx, q.y += q.vy, q.z += q.vz;
        q.radius += q.grow;
        // (fallen back through the water: a drop's middle, a spray's foot half its height under)
        const f32 under = L.shape == BOW ? -0.5f * L.size_y * q.g * curve(L.scale, L.n_scale, (f32)q.age / q.life, true)
                                         : 0.0f;
        if (L.sink && q.vy < 0 && q.y < under)
            q.life = 0;
    }
    parts_.erase(std::remove_if(parts_.begin(), parts_.end(), [](const Particle& q) { return q.life == 0; }),
                 parts_.end());
}

void WaterFx::step(f32 frame) {
    Swim& w = *swim_;
    if (!std::isfinite(last_) || frame < last_)
        last_ = frame;
    const f32 d = std::floor(frame - last_);
    const u32 n = (u32)std::min<f32>(d, (f32)MAX_STEPS);
    last_ += d;
    for (const WaterSplash& s : asked_)
        spawn_splash(s);
    asked_.clear();
    if (!w.known)
        w.was = w.now, w.last = w.now.at, w.known = true;
    for (u32 i = 0; i < n; i++) {
        spawn_swim((f32)(i + 1) / (f32)n);
        w.collar = std::max(0.0f, std::min(1.0f, w.collar + (w.now.on ? 1.0f : -1.0f) / COLLAR_FADE));
        advance();
        steps_++;
    }
    w.was = w.now;
    // (the vertices' origin: the swimmer, else the newest effect)
    const Vec3 o = w.now.on || parts_.empty() ? w.now.at : Vec3{parts_.back().ax, parts_.back().y0, parts_.back().az};
    if (std::isfinite(o.x) && std::isfinite(o.y) && std::isfinite(o.z))
        origin_[0] = std::round(o.x), origin_[1] = std::round(o.y), origin_[2] = std::round(o.z);
}

// The water's height at (x, z) now: on the sea, its level and the waves the grid draws there (none where
// the grid isn't drawn: the flat sea); elsewhere what it was when the particle was made.
f32 WaterFx::surface_at(f32 x, f32 z, f32 y0, bool on_sea) {
    if (!on_sea || !sea_)
        return y0;
    const f32 level = sea_->params().level;
    if (!grid_on_)
        return level;
    f32 h, dx, dz;
    sea_wave(waves_, x - waves_c_[0], z - waves_c_[1], sea_->amplitude(x, z), &h, &dx, &dz);
    return level + h;
}

// The water's height at a patch's 3 x 3 vertices (about (cx, cz), its columns `ax` along x and z,
// its rows the same turned a right angle: (ax, az) is half its side along its own x), as surface_at
// gives it on the sea with the grid drawn (sea.h sea_wave: each wave's amplitude, faded with the
// distance from the grid's centre and by the amplitude map there, times the sine of its phase): each
// wave's phase is its phase in the middle plus a column's and a row's step, so its sines come from
// three by the angles' sums, not nine.
void WaterFx::patch_waves(f32 cx, f32 cz, f32 ax, f32 az, f32 out[9]) {
    const SeaWaves& w = waves_;
    f32 x[9], z[9], fade_r[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            const int k = 3 * i + j;
            x[k] = cx + ax * (f32)(j - 1) - az * (f32)(i - 1);
            z[k] = cz + az * (f32)(j - 1) + ax * (f32)(i - 1);
            const f32 dx = x[k] - waves_c_[0], dz = z[k] - waves_c_[1];
            fade_r[k] = std::sqrt(dx * dx + dz * dz);
            out[k] = 0;
        }
    for (int i = 0; i < SEA_WAVES; i++) {
        if (w.amp[i] == 0)
            continue;
        const f32 p0 = w.kx[i] * (cx - waves_c_[0]) + w.kz[i] * (cz - waves_c_[1]) + w.phase[i];
        const f32 pj = w.kx[i] * ax + w.kz[i] * az, pi = -w.kx[i] * az + w.kz[i] * ax;  // a column's, a row's step
        const f32 s0 = std::sin(p0), c0 = std::cos(p0), sj = std::sin(pj), cj = std::cos(pj), si = std::sin(pi),
                  ci = std::cos(pi);
        // (sin and cos of p0 - pi, p0, p0 + pi; then each of those -pj, 0, +pj)
        const f32 rs[3] = {s0 * ci - c0 * si, s0, s0 * ci + c0 * si}, rc[3] = {c0 * ci + s0 * si, c0, c0 * ci - s0 * si};
        for (int r = 0; r < 3; r++) {
            const f32 sn[3] = {rs[r] * cj - rc[r] * sj, rs[r], rs[r] * cj + rc[r] * sj};
            for (int j = 0; j < 3; j++) {
                const int k = 3 * r + j;
                f32 f = w.fade_a[i] - w.fade_b[i] * fade_r[k];
                f = f < 0 ? 0 : f > 1 ? 1 : f;
                out[k] += w.amp[i] * f * sn[j];
            }
        }
    }
    const f32 level = sea_->params().level;
    for (int k = 0; k < 9; k++)
        out[k] = level + out[k] * sea_->amplitude(x[k], z[k]);
}

// One batch's quads or patches into `out` (at most `cap`): its particles alive, in the order they came
// (a crown is drawn whole or not at all), and the swimmer's collar. How many.
u32 WaterFx::write_batch(u8 batch, u8* out, u32 cap, const WaterFxView& v) {
    const fmt::WaterFx& R = rec_;
    u8* q = out;
    u32 n = 0;
    auto put = [&](f32 x, f32 y, f32 z, const u8 c[4], f32 s, f32 t) {
        const f32 pos[3] = {x - origin_[0], y - origin_[1], z - origin_[2]};
        memcpy(q, pos, 12);
        memcpy(q + 12, c, 4);
        const f32 uv[2] = {s, t};
        memcpy(q + 16, uv, 8);
        q += STRIDE;
    };
    // a patch: 3 x 3 vertices row by row about (cx, cz), `half` its half side, turned by `rot`, on the water
    auto patch = [&](f32 cx, f32 cz, f32 half, f32 rot, f32 base, bool on_sea, const f32* cell, const u8 c[4]) {
        const f32 co = std::cos(rot), si = std::sin(rot);
        f32 y[9];
        if (on_sea && grid_on_) {
            patch_waves(cx, cz, half * co, half * si, y);
        } else {
            for (f32& h : y)
                h = base;
        }
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) {
                const f32 lx = (f32)(j - 1) * half, lz = (f32)(i - 1) * half;
                put(cx + co * lx - si * lz, y[3 * i + j] + LIFT, cz + si * lx + co * lz, c,
                    cell[0] + (cell[2] - cell[0]) * 0.5f * (f32)j, cell[1] + (cell[3] - cell[1]) * 0.5f * (f32)i);
            }
        n++;
    };
    const Vec3 flat = [&] {
        const Vec3 r = {v.right.x, 0, v.right.z};
        return dot(r, r) > 1e-12f ? normalize(r) : Vec3{1, 0, 0};
    }();
    // the collar: foam about the swimmer's body at the water line, eaten away to COLLAR_ALPHA
    const Swim& w = *swim_;
    if (batch == WFX_SURF_ERODE && w.collar > 0 && n < cap) {
        const Swimmer& s = w.now;
        const f32 scale = s.scale > 0 ? s.scale : 1.0f, h = WSR_MII * scale;
        const f32 yr = s.yaw_deg * PI / 180.0f, pace = std::min(1.0f, std::max(0.0f, s.speed) / (SWIM_START * h));
        const f32 cx = s.at.x + std::sin(yr) * COLLAR_AHEAD * h * pace, cz = s.at.z + std::cos(yr) * COLLAR_AHEAD * h * pace;
        const f32 y0 = std::isfinite(s.surface) ? s.surface : s.at.y + SWIM_SINK * h;
        const u8 c[4] = {255, 255, 255, clamp_u8(1 - COLLAR_ALPHA * w.collar)};
        patch(cx, cz, COLLAR_RADIUS * h, (f32)steps_ * 0.004f, surface_at(cx, cz, y0, s.on_sea), s.on_sea,
              R.cell[WFX_FOAM], c);
    }
    const bool erode = batch == WFX_AIR_ERODE || batch == WFX_SURF_ERODE;
    // (the last anchor's water: a splash's particles share their middle's)
    f32 last_ax = NAN, last_az = NAN, last_y0 = NAN, last_base = 0;
    bool last_sea = false;
    for (const Particle& p : parts_) {
        const Layer& L = k_layers[p.layer];
        if (L.batch != batch || p.delay || !p.life || n >= cap)
            continue;
        const u32 need = L.shape == CROWN ? CROWN_SEGMENTS : 1;
        if (n + need > cap)
            continue;
        const f32 t = std::min((f32)p.age / (f32)p.life, 1.0f);
        f32 a = curve(L.alpha, L.n_alpha, t);
        a = erode ? 1 - (1 - a) * p.fade : a * p.fade;
        const u8 c[4] = {(u8)(L.rgb >> 16), (u8)(L.rgb >> 8), (u8)L.rgb, clamp_u8(a)};
        const u32 frame = L.cells > 1 && L.flip > 0 ? std::min<u32>(L.cells - 1, (u32)((f32)p.age / L.flip)) : 0;
        const f32* cell = R.cell[std::min<u32>(L.cell + frame, WFX_CELLS - 1)];
        if (!(p.ax == last_ax && p.az == last_az && p.y0 == last_y0 && p.on_sea == last_sea)) {
            last_ax = p.ax, last_az = p.az, last_y0 = p.y0, last_sea = p.on_sea;
            last_base = surface_at(p.ax, p.az, p.y0, p.on_sea);
        }
        const f32 base = last_base;
        const f32 sx = curve(L.scale, L.n_scale, t), sy = curve(L.scale, L.n_scale, t, true);
        const f32 rot = p.rot + L.spin * t;
        if (L.shape == PATCH) {
            patch(p.x, p.z, 0.5f * L.size_x * p.g * sx, rot, base, p.on_sea, cell, c);
            continue;
        }
        if (L.shape == CROWN) {
            const f32 H = L.size_y * p.g * sy;
            if (!(H > 1e-3f) || !(p.radius > 0))
                continue;
            const f32 lean = CROWN_LEAN * curve(L.lean, L.n_lean, t);
            const f32 r0 = p.radius, r1 = p.radius + H * std::sin(lean);
            const f32 y0 = base - CROWN_FOOT * H, y1 = base + H * std::cos(lean);
            // (its segments' directions: the ring's turned by its own angle)
            const f32 co = std::cos(p.rot), si = std::sin(p.rot);
            for (u32 k = 0; k < CROWN_SEGMENTS; k++) {
                const f32 c0 = co * k_ring[k][0] - si * k_ring[k][1], s0 = si * k_ring[k][0] + co * k_ring[k][1];
                const f32 c1 = co * k_ring[k + 1][0] - si * k_ring[k + 1][1], s1 = si * k_ring[k + 1][0] + co * k_ring[k + 1][1];
                // (top left, top right, bottom left, bottom right: the rim at u1, the foot at u0)
                put(p.x + c0 * r1, y1, p.z + s0 * r1, c, cell[2], cell[1]);
                put(p.x + c1 * r1, y1, p.z + s1 * r1, c, cell[2], cell[3]);
                put(p.x + c0 * r0, y0, p.z + s0 * r0, c, cell[0], cell[1]);
                put(p.x + c1 * r0, y0, p.z + s1 * r0, c, cell[0], cell[3]);
            }
            n += CROWN_SEGMENTS;
            continue;
        }
        // a quad: its middle (a bow spray's foot), its axes, half its width and height (at most
        // SCREEN_CAP of the screen at its distance)
        Vec3 m = {p.x, base + p.y, p.z};
        f32 hw = 0.5f * L.size_x * p.g * sx, hh = 0.5f * L.size_y * p.g * sy;
        if (!(hw > 1e-4f && hh > 1e-4f))
            continue;
        const f32 most = SCREEN_CAP * std::max(length(m - v.eye), 1e-3f) * v.tan_half_fovy;
        if (std::max(hw, hh) > most) {
            const f32 k = most / std::max(hw, hh);
            hw *= k, hh *= k;
        }
        Vec3 X, Y;
        if (L.shape == BILLBOARD) {
            const f32 co = std::cos(rot), si = std::sin(rot);
            X = v.right * co + v.up * si;
            Y = v.up * co - v.right * si;
        } else {
            X = flat, Y = {0, 1, 0};
            if (L.shape == BOW)
                m = m + Y * hh;
        }
        const Vec3 tl = m - X * hw + Y * hh, tr = m + X * hw + Y * hh, bl = m - X * hw - Y * hh, br = m + X * hw - Y * hh;
        put(tl.x, tl.y, tl.z, c, cell[0], cell[1]);
        put(tr.x, tr.y, tr.z, c, cell[2], cell[1]);
        put(bl.x, bl.y, bl.z, c, cell[0], cell[3]);
        put(br.x, br.y, br.z, c, cell[2], cell[3]);
        n++;
    }
    return n;
}

u32 WaterFx::build(Packet* p, bool surface, const WaterFxView& v, WaterFxDraw out[2]) {
    sea_ = v.sea && v.sea->valid() ? v.sea : nullptr;
    grid_on_ = sea_ && v.grid_on;
    if (grid_on_ && (v.seconds != waves_t_ || v.grid_c[0] != waves_c_[0] || v.grid_c[1] != waves_c_[1])) {
        sea_waves(sea_->params(), v.seconds, v.grid_c[0], v.grid_c[1], &waves_);
        waves_t_ = v.seconds, waves_c_[0] = v.grid_c[0], waves_c_[1] = v.grid_c[1];
    }
    const u8 first = surface ? WFX_SURF_MUL : WFX_AIR_MUL;
    const u32 cap = surface ? rec_.surf_patches : rec_.air_quads;
    const u32 per_vertices = surface ? 9 : 4, per_indices = surface ? 24 : 6;
    u32& count = surface ? patches_ : quads_;
    count = 0;
    u32 n = 0;
    for (u8 k = first; k < first + 2; k++) {
        const size_t was = p->dynamic.size(), at = (was + 15) & ~(size_t)15;
        p->dynamic.resize(at + (size_t)cap * per_vertices * STRIDE);
        const u32 m = write_batch(k, p->dynamic.data() + at, cap, v);
        if (!m) {
            p->dynamic.resize(was);
            continue;
        }
        const u32 bytes = m * per_vertices * STRIDE;
        p->dynamic.resize(at + bytes);
        out[n++] = {rec_.batch[k], {(u32)at, bytes, m * per_indices}, m * per_indices / 3};
        count += m;
    }
    return n;
}

}  // namespace engine::g3d
