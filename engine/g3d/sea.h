// The sea that moves (fmt::Sea, a package's SEAW section): a sum of travelling waves over the open
// water, which a grid of vertices that follows the camera draws (world.h: the grid's item; the PICA
// backend's vertex program platform/3ds/shaders/g3d_sea.v.pica, the host's soft_backend.cpp with the
// functions here) and which boats ride (the same waves on the CPU: Sea::height).
//
// A wave i at a point p (x, z) at time t is amp_i sin(k_i . p - w_i t + phase_i), k_i its wave vector
// (2 pi / wavelength along its direction), w_i = k_i x its speed. Its amplitude is scaled by the
// amplitude map at p (the stager's: flat by the shores, whole over the open sea) and by a fade with
// the distance r from the grid's centre (whole within fade[0], none beyond fade[1]: where the grid's
// vertices are too far apart to draw a wave, it is left out, the long ones last), so the grid's rim is
// flat and meets the flat sea (MAT_SEA) beyond it without a seam. A frame's phases are folded about the
// grid's centre on the CPU in double precision and wrapped (SeaWaves): the GPU's 24-bit floats then
// only see the few radians a vertex is from it.
//
// The grid's shading, relative to the flat sea's (a vertex whose waves have faded is the flat sea's
// colour exactly): its colour channel's rgb is half a shade -- 1 + the change of N.L against a flat
// surface's (the sun's side of a swell lighter, its back darker) + the loss of the sky's Fresnel
// reflection where a face turns toward the eye --, the combiners double it; its alpha the brightening:
// the gain of that reflection where a face turns away (the grazing sky on the far side of the crests)
// and the sun's glint, the highlight of the sun's reflection beyond what the flat sea's would have. The
// material's combiners (the stager's: the flat sea's own, then times the shade, then plus a sky colour
// times the brightening) do the rest.
#pragma once

#include <cmath>
#include <vector>

#include "engine/g3d/package.h"
#include "engine/math/vecmath.h"

namespace engine::g3d {

constexpr int SEA_WAVES = fmt::SEA_WAVES;

// A frame's waves about a grid's centre, as the grid's vertex program takes them (a lane a wave):
// wave vectors, amplitudes, the phases at the centre (in [-pi, pi)), and the fade with the distance r
// from the centre: the amplitude's factor clamp(fade_a - fade_b r, 0, 1).
struct SeaWaves {
    f32 kx[4] = {}, kz[4] = {};
    f32 amp[4] = {};
    f32 phase[4] = {};
    f32 fade_a[4] = {}, fade_b[4] = {};
};

// the waves of the sea `s` at `seconds`, about (cx, cz)
void sea_waves(const fmt::Sea& s, double seconds, double cx, double cz, SeaWaves* out);

// The waves' height and slopes at (x, z) from the centre, where the amplitude map's factor is `a`
// (0-1): what the vertex program computes for a grid vertex (with its polynomial sine).
inline void sea_wave(const SeaWaves& w, f32 x, f32 z, f32 a, f32* h, f32* dhdx, f32* dhdz) {
    const f32 r = std::sqrt(x * x + z * z);
    f32 hh = 0, dx = 0, dz = 0;
    for (int i = 0; i < SEA_WAVES; i++) {
        f32 f = w.fade_a[i] - w.fade_b[i] * r;
        f = f < 0 ? 0 : f > 1 ? 1 : f;
        const f32 amp = w.amp[i] * f * a;
        const f32 p = w.kx[i] * x + w.kz[i] * z + w.phase[i];
        const f32 s = std::sin(p), c = std::cos(p);
        hh += amp * s;
        dx += amp * w.kx[i] * c;
        dz += amp * w.kz[i] * c;
    }
    *h = hh, *dhdx = dx, *dhdz = dz;
}

// A grid vertex's shading (the vertex program's, the module comment's): at (x, y, z) from the centre
// with the slopes there, the eye at `eye` from the centre: the shade (1: as the flat sea) and the
// brightening (0: none), each before the colour channel's clamp.
void sea_shade(const fmt::Sea& s, f32 x, f32 y, f32 z, f32 dhdx, f32 dhdz, const f32 eye[3], f32* shade,
               f32* bright);

// A package's sea at run time: its waves at any time and place, its amplitude map, its grid's vertices.
// Immutable once made (any thread may ask it).
class Sea {
public:
    explicit Sea(const Package& package);

    bool valid() const { return s_ != nullptr; }
    const fmt::Sea& params() const { return *s_; }

    // the amplitude map's factor at (x, z) (bilinear between its texels' centres; 1 outside the map)
    f32 amplitude(f32 x, f32 z) const;
    // The surface at (x, z) at `seconds`, its waves faded about a grid centred at (cx, cz): its
    // height (the level and the waves), and its normal if asked.
    f32 height(f32 x, f32 z, double seconds, f32 cx, f32 cz, engine::math::Vec3* normal = nullptr) const;
    // ... with the waves made already (sea_waves for that time, about (cx, cz): many places at once)
    f32 height(f32 x, f32 z, const SeaWaves& w, f32 cx, f32 cz, engine::math::Vec3* normal = nullptr) const;

    // the grid's vertices: their count, and each one's (x, z) from the centre
    u32 grid_vertices() const { return (u32)grid_xz_.size() / 2; }
    const f32* grid_xz() const { return grid_xz_.data(); }
    // the amplitude map's factor at each grid vertex for a grid centred at (cx, cz): 4 bytes a vertex
    // (the factor 0-255, then three of padding: the vertex program's attribute); false if all are 0
    bool grid_amplitudes(f32 cx, f32 cz, u8* out) const;

private:
    const fmt::Sea* s_ = nullptr;
    const u8* amp_ = nullptr;
    std::vector<f32> grid_xz_;
};

}  // namespace engine::g3d
