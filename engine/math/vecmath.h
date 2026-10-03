// The engine's vector and matrix math: native types in the host's float layout and the
// operations on them, written for the 3DS's ARM11 (VFPv2: single precision throughout -- its
// double-precision operations take twice as long, and there is no NEON).
//
// Guest code keeps its vectors and matrices in guest memory as big-endian words; load() and
// store() move a whole object between guest memory and these types in one pass (one page
// lookup, a byte reverse per word), so the arithmetic in between runs on native floats.
//
// Layouts match the SDK's and nw4r's: Mtx34 is row-major 3x4 (Mtx), Mtx44 4x4 (Mtx44), Mtx33
// 3x3 (nw4r::math::MTX33), Quat x y z w.
#pragma once

#include <cmath>
#include <cstring>

#include "guest.h"

namespace engine::math {

struct Vec2 {
    f32 x, y;
};
struct Vec3 {
    f32 x, y, z;
};
struct Quat {
    f32 x, y, z, w;
};
struct Mtx33 {
    f32 m[3][3];
};
struct Mtx34 {
    f32 m[3][4];
};
struct Mtx44 {
    f32 m[4][4];
};

// ---- guest memory: an object's words, byte-reversed (MEM1 and MEM2 are each one block of
// host memory, so an object is contiguous from its first byte). Objects elsewhere go word by
// word through the device accessors: the locked cache (0xE0000000, where nw4r::g3d keeps the
// matrices it is working on) is emulated as a device.

// word by word, unrolled: each one an ldr, a rev and a move into a VFP register (copying the
// whole object and reversing it in place makes GCC go through the stack twice)
template <class T>
inline T load(u32 a) {
    static_assert(sizeof(T) % 4 == 0, "words only");
    const u8* p = guest::host_ptr(a);
    T v;
    u8* d = reinterpret_cast<u8*>(&v);
    if (__builtin_expect(p == nullptr, 0)) {
        for (u32 i = 0; i < sizeof(T); i += 4) {
            u32 w = guest::load32(a + i);
            memcpy(d + i, &w, 4);
        }
        return v;
    }
#pragma GCC unroll 16
    for (u32 i = 0; i < sizeof(T); i += 4) {
        u32 w;
        memcpy(&w, p + i, 4);
        w = __builtin_bswap32(w);
        memcpy(d + i, &w, 4);
    }
    return v;
}

template <class T>
inline void store(u32 a, const T& v) {
    static_assert(sizeof(T) % 4 == 0, "words only");
    u8* p = guest::host_ptr(a);
    const u8* s = reinterpret_cast<const u8*>(&v);
    if (__builtin_expect(p == nullptr, 0)) {
        for (u32 i = 0; i < sizeof(T); i += 4) {
            u32 w;
            memcpy(&w, s + i, 4);
            guest::store32(a + i, w);
        }
        return;
    }
#pragma GCC unroll 16
    for (u32 i = 0; i < sizeof(T); i += 4) {
        u32 w;
        memcpy(&w, s + i, 4);
        w = __builtin_bswap32(w);
        memcpy(p + i, &w, 4);
    }
}

inline f32 load_f32(u32 a) { return load<f32>(a); }
inline void store_f32(u32 a, f32 v) { store(a, v); }

// ---- scalars
inline f32 rsqrt(f32 x) { return 1.0f / sqrtf(x); }

// ---- vectors
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator-(Vec3 a) { return {-a.x, -a.y, -a.z}; }
inline Vec3 operator*(Vec3 a, f32 s) { return {a.x * s, a.y * s, a.z * s}; }

inline f32 dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
// a zero vector comes out as NaNs, as the SDK's does
inline Vec3 normalize(Vec3 v) { return v * rsqrt(dot(v, v)); }
inline f32 length(Vec3 v) {
    f32 sq = dot(v, v);
    return sq == 0.0f ? 0.0f : sqrtf(sq);
}

inline Vec2 normalize(Vec2 v) {
    f32 s = rsqrt(v.x * v.x + v.y * v.y);
    return {v.x * s, v.y * s};
}

// ---- 3x4 matrices (an affine transform: a 3x3 part and a translation column)
inline Mtx34 identity34() {
    return {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
}

// a * b (b applied first)
inline Mtx34 concat(const Mtx34& a, const Mtx34& b) {
    Mtx34 r;
    for (int i = 0; i < 3; i++) {
        const f32 x = a.m[i][0], y = a.m[i][1], z = a.m[i][2];
        r.m[i][0] = x * b.m[0][0] + y * b.m[1][0] + z * b.m[2][0];
        r.m[i][1] = x * b.m[0][1] + y * b.m[1][1] + z * b.m[2][1];
        r.m[i][2] = x * b.m[0][2] + y * b.m[1][2] + z * b.m[2][2];
        r.m[i][3] = x * b.m[0][3] + y * b.m[1][3] + z * b.m[2][3] + a.m[i][3];
    }
    return r;
}

inline Vec3 transform(const Mtx34& m, Vec3 v) {
    return {m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z + m.m[0][3],
            m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z + m.m[1][3],
            m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z + m.m[2][3]};
}

// the 3x3 part only (directions, normals)
inline Vec3 transform_normal(const Mtx34& m, Vec3 v) {
    return {m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z,
            m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z,
            m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z};
}

// false (and out untouched) when the 3x3 part is singular
inline bool inverse(const Mtx34& a, Mtx34& out) {
    const f32(*m)[4] = a.m;
    // the cofactors of the 3x3 part, transposed
    f32 c00 = m[1][1] * m[2][2] - m[2][1] * m[1][2];
    f32 c01 = m[2][1] * m[0][2] - m[0][1] * m[2][2];
    f32 c02 = m[0][1] * m[1][2] - m[1][1] * m[0][2];
    f32 c10 = m[1][2] * m[2][0] - m[2][2] * m[1][0];
    f32 c11 = m[2][2] * m[0][0] - m[0][2] * m[2][0];
    f32 c12 = m[0][2] * m[1][0] - m[1][2] * m[0][0];
    f32 c20 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    f32 c21 = m[0][1] * m[2][0] - m[0][0] * m[2][1];
    f32 c22 = m[0][0] * m[1][1] - m[0][1] * m[1][0];
    f32 det = m[0][0] * c00 + m[1][0] * c01 + m[2][0] * c02;
    if (det == 0.0f)
        return false;
    f32 inv = 1.0f / det;
    Mtx34 r = {{{c00 * inv, c01 * inv, c02 * inv, 0},
                {c10 * inv, c11 * inv, c12 * inv, 0},
                {c20 * inv, c21 * inv, c22 * inv, 0}}};
    for (int i = 0; i < 3; i++)
        r.m[i][3] = -(r.m[i][0] * m[0][3] + r.m[i][1] * m[1][3] + r.m[i][2] * m[2][3]);
    out = r;
    return true;
}

// the inverse transpose of the 3x3 part (for normals), no translation; false when singular
inline bool inverse_transpose(const Mtx34& a, Mtx34& out) {
    const f32(*m)[4] = a.m;
    f32 c00 = m[1][1] * m[2][2] - m[2][1] * m[1][2];
    f32 c01 = m[1][2] * m[2][0] - m[2][2] * m[1][0];
    f32 c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    f32 c10 = m[2][1] * m[0][2] - m[0][1] * m[2][2];
    f32 c11 = m[2][2] * m[0][0] - m[0][2] * m[2][0];
    f32 c12 = m[0][1] * m[2][0] - m[0][0] * m[2][1];
    f32 c20 = m[0][1] * m[1][2] - m[1][1] * m[0][2];
    f32 c21 = m[0][2] * m[1][0] - m[1][2] * m[0][0];
    f32 c22 = m[0][0] * m[1][1] - m[0][1] * m[1][0];
    f32 det = m[0][0] * c00 + m[1][0] * c10 + m[2][0] * c20;
    if (det == 0.0f)
        return false;
    f32 inv = 1.0f / det;
    out = {{{c00 * inv, c01 * inv, c02 * inv, 0},
            {c10 * inv, c11 * inv, c12 * inv, 0},
            {c20 * inv, c21 * inv, c22 * inv, 0}}};
    return true;
}

// a rotation by an angle (given as its sine and cosine) about a unit axis
inline Mtx34 rotation(Vec3 n, f32 s, f32 c) {
    f32 t = 1.0f - c;
    f32 tx = t * n.x, ty = t * n.y, tz = t * n.z;
    return {{{tx * n.x + c, tx * n.y - s * n.z, tx * n.z + s * n.y, 0},
             {tx * n.y + s * n.z, ty * n.y + c, ty * n.z - s * n.x, 0},
             {tx * n.z - s * n.y, ty * n.z + s * n.x, tz * n.z + c, 0}}};
}

// the rotation of a quaternion (of any length)
inline Mtx34 rotation(Quat q) {
    f32 s = 2.0f / (q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    f32 xs = q.x * s, ys = q.y * s, zs = q.z * s;
    f32 wx = q.w * xs, wy = q.w * ys, wz = q.w * zs;
    f32 xx = q.x * xs, xy = q.x * ys, xz = q.x * zs;
    f32 yy = q.y * ys, yz = q.y * zs, zz = q.z * zs;
    return {{{1.0f - (yy + zz), xy - wz, xz + wy, 0},
             {xy + wz, 1.0f - (xx + zz), yz - wx, 0},
             {xz - wy, yz + wx, 1.0f - (xx + yy), 0}}};
}

}  // namespace engine::math
