// The sea that moves (sea.h).
#include "engine/g3d/sea.h"

#include <algorithm>
#include <cstring>

namespace engine::g3d {

using engine::math::Vec3;

void sea_waves(const fmt::Sea& s, double seconds, double cx, double cz, SeaWaves* out) {
    const double two_pi = 6.283185307179586;
    for (int i = 0; i < SEA_WAVES; i++) {
        const fmt::SeaWave& w = s.waves[i];
        const double k = two_pi / w.wavelength;
        const double kx = k * w.dir[0], kz = k * w.dir[1];
        // (the phase at the centre now, wrapped: what the GPU's floats add a vertex's few radians to)
        double p = kx * cx + kz * cz - k * w.speed * seconds + w.phase;
        p -= two_pi * std::floor(p / two_pi + 0.5);
        out->kx[i] = (f32)kx, out->kz[i] = (f32)kz;
        out->amp[i] = w.amplitude;
        out->phase[i] = (f32)p;
        const f32 span = w.fade[1] - w.fade[0];
        out->fade_a[i] = w.fade[1] / span;
        out->fade_b[i] = 1.0f / span;
    }
}

void sea_shade(const fmt::Sea& s, f32 x, f32 y, f32 z, f32 dhdx, f32 dhdz, const f32 eye[3], f32* shade,
               f32* bright) {
    f32 n[3] = {-dhdx, 1.0f, -dhdz};
    const f32 nl = 1.0f / std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    n[0] *= nl, n[1] *= nl, n[2] *= nl;
    f32 v[3] = {eye[0] - x, eye[1] - y, eye[2] - z};
    const f32 vl = 1.0f / std::sqrt(std::max(v[0] * v[0] + v[1] * v[1] + v[2] * v[2], 1e-12f));
    v[0] *= vl, v[1] *= vl, v[2] *= vl;
    const f32* L = s.sun;
    const f32 kd = s.shade[0], kf = s.shade[1], ks = s.shade[2], power = s.shade[3];
    // the sun's side of a swell lighter, its back darker (against the flat surface's N.L, L.y)
    const f32 diffuse = kd * (n[0] * L[0] + n[1] * L[1] + n[2] * L[2] - L[1]);
    // Schlick's Fresnel term (water's 0.02 head on) for the face and for the flat surface
    const f32 nv = n[0] * v[0] + n[1] * v[1] + n[2] * v[2];
    auto fresnel = [](f32 c) {
        const f32 t = 1.0f - std::max(c, 0.0f), t2 = t * t;
        return 0.02f + 0.98f * t2 * t2 * t;
    };
    const f32 df = fresnel(nv) - fresnel(v[1]);
    // the sun's reflection: the view reflected about the face's normal, and about the flat surface's
    const f32 r[3] = {2 * nv * n[0] - v[0], 2 * nv * n[1] - v[1], 2 * nv * n[2] - v[2]};
    const f32 rl = std::max(r[0] * L[0] + r[1] * L[1] + r[2] * L[2], 0.0f);
    const f32 rl0 = std::max(-v[0] * L[0] + v[1] * L[1] - v[2] * L[2], 0.0f);
    const f32 glint = std::pow(rl, power) - std::pow(rl0, power);
    *shade = 1.0f + diffuse + kf * std::min(df, 0.0f);
    *bright = kf * std::max(df, 0.0f) + ks * std::max(glint, 0.0f);
}

Sea::Sea(const Package& p) {
    s_ = p.sea();
    if (!s_)
        return;
    amp_ = p.sea_amplitude();
    if (s_->grid_batch != fmt::NONE16) {
        const fmt::Batch& b = p.batches()[s_->grid_batch];
        const fmt::VertexBuffer& vb = p.vertex_buffers()[b.vbuf];
        const u8* d = p.at(vb.data);
        grid_xz_.resize(2 * (size_t)vb.count);
        for (u32 i = 0; i < vb.count; i++) {
            f32 q[3];
            memcpy(q, d + (size_t)i * vb.stride, 12);
            grid_xz_[2 * i] = q[0], grid_xz_[2 * i + 1] = q[2];
        }
    }
}

f32 Sea::amplitude(f32 x, f32 z) const {
    const u32 w = s_->amp_size[0], h = s_->amp_size[1];
    const f32* b = s_->amp_box;
    if (!w || !h || !(x >= b[0] && x < b[2] && z >= b[1] && z < b[3]))
        return 1.0f;
    // (texel centres: between them bilinear, from the outer ones to the edge as they are)
    const f32 fx = (x - b[0]) / (b[2] - b[0]) * (f32)w - 0.5f, fz = (z - b[1]) / (b[3] - b[1]) * (f32)h - 0.5f;
    const int x0 = std::max(0, std::min((int)std::floor(fx), (int)w - 1)), z0 = std::max(0, std::min((int)std::floor(fz), (int)h - 1));
    const int x1 = std::min(x0 + 1, (int)w - 1), z1 = std::min(z0 + 1, (int)h - 1);
    const f32 tx = std::min(std::max(fx - (f32)x0, 0.0f), 1.0f), tz = std::min(std::max(fz - (f32)z0, 0.0f), 1.0f);
    const f32 a00 = amp_[z0 * w + x0], a10 = amp_[z0 * w + x1], a01 = amp_[z1 * w + x0], a11 = amp_[z1 * w + x1];
    return ((a00 * (1 - tx) + a10 * tx) * (1 - tz) + (a01 * (1 - tx) + a11 * tx) * tz) * (1.0f / 255.0f);
}

f32 Sea::height(f32 x, f32 z, double seconds, f32 cx, f32 cz, Vec3* normal) const {
    SeaWaves w;
    sea_waves(*s_, seconds, cx, cz, &w);
    f32 h, dx, dz;
    sea_wave(w, x - cx, z - cz, amplitude(x, z), &h, &dx, &dz);
    if (normal)
        *normal = engine::math::normalize(Vec3{-dx, 1.0f, -dz});
    return s_->level + h;
}

bool Sea::grid_amplitudes(f32 cx, f32 cz, u8* out) const {
    bool any = false;
    const u32 n = grid_vertices();
    for (u32 i = 0; i < n; i++) {
        const f32 a = amplitude(cx + grid_xz_[2 * i], cz + grid_xz_[2 * i + 1]);
        const u8 v = (u8)std::min(255.0f, a * 255.0f + 0.5f);
        out[4 * i] = v, out[4 * i + 1] = out[4 * i + 2] = out[4 * i + 3] = 0;
        any |= v != 0;
    }
    return any;
}

}  // namespace engine::g3d
