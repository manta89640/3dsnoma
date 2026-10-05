// Animation curves of the native g3d (anim.h).
#include "engine/g3d/anim.h"

#include <cmath>

namespace engine::g3d {

namespace {
const f32 DEG = 3.14159265358979f / 180.0f;
}

f32 anim_frame(f32 time, f32 length, bool loop) {
    if (!(length > 0))
        return 0;
    if (!loop)
        return time < 0 ? 0 : time > length ? length : time;
    f32 f = std::fmod(time, length);
    if (f < 0)
        f += length;
    return f >= length ? 0 : f;  // (fmod of a value just below a multiple can round up to it)
}

f32 eval_track(const fmt::Track& t, const fmt::Key* keys, f32 frame) {
    const fmt::Key* k = keys + t.first_key;
    const u32 n = t.n_keys;
    if (n == 1 || frame <= k[0].frame)
        return k[0].value;
    if (frame >= k[n - 1].frame)
        return k[n - 1].value;
    // the last key at or before the frame (binary search: k[lo].frame <= frame < k[hi].frame)
    u32 lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        const u32 mid = (lo + hi) / 2;
        if (k[mid].frame <= frame)
            lo = mid;
        else
            hi = mid;
    }
    const fmt::Key& a = k[lo];
    const fmt::Key& b = k[hi];
    const f32 d = frame - a.frame, span = b.frame - a.frame;
    switch (t.interp) {
    case fmt::IP_STEP: return a.value;
    case fmt::IP_LINEAR: return a.value + (b.value - a.value) * (d / span);
    default: {
        const f32 u = d / span, u1 = u - 1.0f;
        return a.value + (b.value - a.value) * (3.0f - 2.0f * u) * u * u + d * u1 * (u1 * a.slope + u * b.slope);
    }
    }
}

Mtx34 srt_matrix(const f32 s[3], const f32 r[3], const f32 t[3]) {
    const f32 cx = std::cos(r[0] * DEG), sx = std::sin(r[0] * DEG);
    const f32 cy = std::cos(r[1] * DEG), sy = std::sin(r[1] * DEG);
    const f32 cz = std::cos(r[2] * DEG), sz = std::sin(r[2] * DEG);
    const f32 rot[3][3] = {{cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz},
                           {cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz},
                           {-sy, sx * cy, cx * cy}};
    Mtx34 m;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++)
            m.m[i][j] = rot[i][j] * s[j];
        m.m[i][3] = t[i];
    }
    return m;
}

void tex_srt_matrix(u8 mode, f32 sx, f32 sy, f32 rotate_deg, f32 tx, f32 ty, f32 out[2][4], bool* approximated) {
    if (mode != 0 && approximated)
        *approximated = true;
    const f32 c = std::cos(rotate_deg * DEG), s = std::sin(rotate_deg * DEG);
    out[0][0] = sx * c, out[0][1] = -sx * s, out[0][2] = 0;
    out[0][3] = sx * ((-0.5f * c) - (0.5f * s - 0.5f) - tx);
    out[1][0] = sy * s, out[1][1] = sy * c, out[1][2] = 0;
    out[1][3] = sy * ((-0.5f * c) + (0.5f * s - 0.5f) + ty) + 1.0f;
}

}  // namespace engine::g3d
