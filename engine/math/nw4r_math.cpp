// nw4r::math (math_triangular.cpp, math_types.cpp), native. Angles in the FIdx functions are
// 256 to a turn and come from nw4r's tables (engine/math/tables.cpp), interpolated as nw4r
// does: the table index is the angle as a u16 (the paired-single store through GQR3, set up
// as u16 with no scale: truncated, clamped to 0..65535), the remainder the fraction.
//
// Signatures follow decl.h (what the decompiled callers pass).
#include "noma_decomp.h"
#include "vecmath.h"
#include "tables.h"

namespace g {

using namespace engine::math;

namespace {

inline u16 quantize_u16(f32 x) {
    return (u16)(x < 0.0f ? 0 : x > 65535.0f ? 65535 : (s32)x);
}

// |fidx| brought into (0, 65536], as nw4r's loop of subtractions leaves it (for angles past
// 2^24, where the subtractions stop being exact or never end, fmodf instead)
inline f32 wrap_fidx(f32 a) {
    if (a > 65536.0f) {
        if (a >= 16777216.0f)
            return fmodf(a, 65536.0f);
        do
            a -= 65536.0f;
        while (a > 65536.0f);
    }
    return a;
}

inline const SinCos& sincos_entry(f32 a, f32& frac) {
    u16 i = quantize_u16(a);
    frac = a - (f32)i;
    return kSinCosTable[i & 0xFF];
}

// sin and cos of an FIdx angle
inline void sincos_fidx(f32 fidx, f32& s, f32& c) {
    f32 r;
    const SinCos& e = sincos_entry(wrap_fidx(fabsf(fidx)), r);
    s = e.sin_val + r * e.sin_delta;
    c = e.cos_val + r * e.cos_delta;
    if (fidx < 0.0f)
        s = -s;
}

}  // namespace

f64 nw4r_math_SinFIdx(f64 fidx) {
    f32 r;
    const SinCos& e = sincos_entry(wrap_fidx(fabsf((f32)fidx)), r);
    f32 s = e.sin_val + r * e.sin_delta;
    return fidx < 0.0 ? -s : s;
}

f64 nw4r_math_CosFIdx(u32, u32, f64 fidx) {
    f32 r;
    const SinCos& e = sincos_entry(wrap_fidx(fabsf((f32)fidx)), r);
    return e.cos_val + r * e.cos_delta;
}

void nw4r_math_SinCosFIdx(u32 sin_out, u32 cos_out, f64 fidx) {
    f32 s, c;
    sincos_fidx((f32)fidx, s, c);
    store_f32(cos_out, c);
    store_f32(sin_out, s);
}

// the angle of (x, y) in FIdx (-128..128); from atan in the octant, by the table
f64 nw4r_math_Atan2FIdx(f64 y64, f64 x64) {
    f32 y = (f32)y64, x = (f32)x64;
    if (x == 0.0f && y == 0.0f)
        return 0.0;
    f32 num, den, base;
    bool subtract;
    if (x >= 0.0f) {
        if (y >= 0.0f) {
            if (x >= y)
                num = y, den = x, base = 0.0f, subtract = false;
            else
                num = x, den = y, base = 64.0f, subtract = true;
        } else {
            y = -y;
            if (x >= y)
                num = y, den = x, base = 0.0f, subtract = true;
            else
                num = x, den = y, base = -64.0f, subtract = false;
        }
    } else {
        x = -x;
        if (y >= 0.0f) {
            if (x >= y)
                num = y, den = x, base = 128.0f, subtract = true;
            else
                num = x, den = y, base = 64.0f, subtract = false;
        } else {
            y = -y;
            if (x >= y)
                num = y, den = x, base = -128.0f, subtract = false;
            else
                num = x, den = y, base = -64.0f, subtract = true;
        }
    }
    f32 t = (num / den) * 32.0f;
    u16 i = quantize_u16(t);
    if (i > 32)  // (num <= den: only for values the original would read past the table with)
        i = 32;
    const ArcTan& e = kArcTanTable[i];
    f32 a = e.atan_val + (t - (f32)i) * e.atan_delta;
    return subtract ? base - a : base + a;
}

// (dst, src)
void nw4r_math_VEC2Normalize(u32 dst, u32 src) {
    store(dst, normalize(load<Vec2>(src)));
}

// (dst, src)
void nw4r_math_MTX33Copy(u32 dst, u32 src) {
    if (dst != src)
        guest::memcpy_g(dst, src, sizeof(Mtx33));
}

void nw4r_math_MTX33Identity(u32 m) {
    store(m, Mtx33{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}});
}

// (dst, src): the 3x3 part
void nw4r_math_MTX34ToMTX33(u32 dst, u32 src) {
    Mtx34 a = load<Mtx34>(src);
    store(dst, Mtx33{{{a.m[0][0], a.m[0][1], a.m[0][2]},
                      {a.m[1][0], a.m[1][1], a.m[1][2]},
                      {a.m[2][0], a.m[2][1], a.m[2][2]}}});
}

// (dst, src): into the 3x3 part; dst's translation stays
void nw4r_math_MTX33ToMTX34(u32 dst, u32 src) {
    Mtx33 a = load<Mtx33>(src);
    for (int i = 0; i < 3; i++)
        store(dst + i * 0x10, a.m[i]);
}

void nw4r_math_MTX34Zero(u32 m) {
    guest::memset_g(m, 0, sizeof(Mtx34));
}

// out = in * scale(s): column j scaled by s[j]; f1 was left holding s.z
f64 nw4r_math_MTX34Scale(u32 out, u32 in, u32 scale) {
    Mtx34 a = load<Mtx34>(in);
    Vec3 s = load<Vec3>(scale);
    for (int i = 0; i < 3; i++) {
        a.m[i][0] *= s.x;
        a.m[i][1] *= s.y;
        a.m[i][2] *= s.z;
    }
    store(out, a);
    return s.z;
}

// out = in * translation(t)
void nw4r_math_MTX34Trans(u32 out, u32 in, u32 t) {
    Mtx34 a = load<Mtx34>(in);
    Vec3 p = transform(a, load<Vec3>(t));
    a.m[0][3] = p.x;
    a.m[1][3] = p.y;
    a.m[2][3] = p.z;
    store(out, a);
}

void nw4r_math_MTX34RotAxisFIdx(u32 m, u32 axis, f64 fidx) {
    f32 a = 0.024543693f * (f32)fidx;  // 2 pi / 256
    store(m, rotation(normalize(load<Vec3>(axis)), sinf(a), cosf(a)));
}

// rotations about x, then y, then z
void nw4r_math_MTX34RotXYZFIdx(u32 m, f64 fx, f64 fy, f64 fz) {
    f32 sx, cx, sy, cy, sz, cz;
    sincos_fidx((f32)fx, sx, cx);
    sincos_fidx((f32)fy, sy, cy);
    sincos_fidx((f32)fz, sz, cz);
    f32 cxcz = cx * cz, sxsy = sx * sy, cxsz = cx * sz;
    store(m, Mtx34{{{cz * cy, sxsy * cz - cxsz, cxcz * sy + sx * sz, 0},
                    {sz * cy, sxsy * sz + cxcz, cxsz * sy - sx * cz, 0},
                    {-sy, sx * cy, cx * cy, 0}}});
}

// (out, m, v): the 3x3 part only
void nw4r_math_VEC3TransformNormal(u32 out, u32 m, u32 v) {
    store(out, transform_normal(load<Mtx34>(m), load<Vec3>(v)));
}

void nw4r_math_MTX44Identity(u32 m, u32) {
    store(m, Mtx44{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}});
}

// (dst, src)
void nw4r_math_MTX44Copy(u32 dst, u32 src) {
    if (dst != src)
        guest::memcpy_g(dst, src, sizeof(Mtx44));
}

}  // namespace g
