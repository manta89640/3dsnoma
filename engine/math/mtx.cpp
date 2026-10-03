// The SDK's matrix, vector and quaternion library (MTX: mtx.c, mtxvec.c, mtx44.c, vec.c,
// quat.c), native: each call loads its operands from guest memory once, works on native floats
// (engine/math/vecmath.h) and stores the result. The paired-single originals approximate 1/x and
// 1/sqrt(x) with fres/frsqrte and a Newton step; here they are exact, and sin/cos/tan/acos are
// the single-precision ones (the decompiled callers went through MSL's double-precision ones).
//
// Signatures follow decl.h (what the decompiled callers pass). Where a function returns f64
// although the SDK's is void, the value is what was left in f1 -- a few callers pass it on, to
// parameters nobody reads -- and is kept as the decompiled version had it.
#include "noma_decomp.h"
#include "vecmath.h"

namespace g {

using namespace engine::math;

// ---- Mtx (3x4)

f64 PSMTXIdentity(u32 m, u32) {
    store(m, identity34());
    return 0.0;
}

// (src, dst): the words move as they are
f64 PSMTXCopy(u32 src, u32 dst) {
    if (src != dst)
        guest::memcpy_g(dst, src, sizeof(Mtx34));
    return load_f32(src + 0x8);
}

// ab = a * b
f64 PSMTXConcat(u32 a, u32 b, u32 ab) {
    Mtx34 ma = load<Mtx34>(a);
    store(ab, concat(ma, load<Mtx34>(b)));
    return ma.m[0][2];
}

// dst[i] = a * src[i], i < count
void PSMTXConcatArray(u32 a, u32 src, u32 dst, u32 count) {
    Mtx34 ma = load<Mtx34>(a);
    for (u32 i = 0; i < count; i++)
        store(dst + i * sizeof(Mtx34), concat(ma, load<Mtx34>(src + i * sizeof(Mtx34))));
}

// 0 (dst untouched) when src is singular
u32 PSMTXInverse(u32 src, u32 dst) {
    Mtx34 r;
    if (!inverse(load<Mtx34>(src), r))
        return 0;
    store(dst, r);
    return 1;
}

u32 PSMTXInvXpose(u32 src, u32 dst) {
    Mtx34 r;
    if (!inverse_transpose(load<Mtx34>(src), r))
        return 0;
    store(dst, r);
    return 1;
}

// a rotation about the x, y or z axis (axis: 'x' 'y' 'z', either case); other axes leave m
void PSMTXRotTrig(u32 m, u32 axis, f64 sin_a, f64 cos_a) {
    f32 s = (f32)sin_a, c = (f32)cos_a;
    Mtx34 r;
    switch (axis | 0x20) {
    case 'x': r = {{{1, 0, 0, 0}, {0, c, -s, 0}, {0, s, c, 0}}}; break;
    case 'y': r = {{{c, 0, s, 0}, {0, 1, 0, 0}, {-s, 0, c, 0}}}; break;
    case 'z': r = {{{c, -s, 0, 0}, {s, c, 0, 0}, {0, 0, 1, 0}}}; break;
    default: return;
    }
    store(m, r);
}

void PSMTXRotRad(u32 m, u32 axis, f64 rad) {
    f32 a = (f32)rad;
    PSMTXRotTrig(m, (u32)(s32)(s8)axis, sinf(a), cosf(a));
}

// a rotation about any axis (normalized here)
void __PSMTXRotAxisRadInternal(u32 m, u32 axis, f64 sin_a, f64 cos_a) {
    store(m, rotation(normalize(load<Vec3>(axis)), (f32)sin_a, (f32)cos_a));
}

void PSMTXRotAxisRad(u32 m, u32 axis, f64 rad) {
    f32 a = (f32)rad;
    store(m, rotation(normalize(load<Vec3>(axis)), sinf(a), cosf(a)));
}

void PSMTXTrans(u32 m, f64 x, f64 y, f64 z) {
    store(m, Mtx34{{{1, 0, 0, (f32)x}, {0, 1, 0, (f32)y}, {0, 0, 1, (f32)z}}});
}

// dst = translation(x, y, z) * src
f64 PSMTXTransApply(u32 src, u32 dst, f64 x, f64 y, f64 z) {
    Mtx34 r = load<Mtx34>(src);
    r.m[0][3] += (f32)x;
    r.m[1][3] += (f32)y;
    r.m[2][3] += (f32)z;
    store(dst, r);
    return (f32)x;
}

void PSMTXScale(u32 m, f64 x, f64 y, f64 z) {
    store(m, Mtx34{{{(f32)x, 0, 0, 0}, {0, (f32)y, 0, 0}, {0, 0, (f32)z, 0}}});
}

// dst = scale(x, y, z) * src: row i scaled by the i-th factor
void PSMTXScaleApply(u32 src, u32 dst, f64 x, f64 y, f64 z) {
    Mtx34 r = load<Mtx34>(src);
    const f32 s[3] = {(f32)x, (f32)y, (f32)z};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            r.m[i][j] *= s[i];
    store(dst, r);
}

void PSMTXQuat(u32 m, u32 q) {
    store(m, rotation(load<Quat>(q)));
}

// (the SDK's returns nothing; r3 held a stack address)
u32 C_MTXLookAt(u32 m, u32 cam_pos, u32 cam_up, u32 target) {
    Vec3 pos = load<Vec3>(cam_pos);
    Vec3 look = normalize(pos - load<Vec3>(target));
    Vec3 right = normalize(cross(load<Vec3>(cam_up), look));
    Vec3 up = cross(look, right);
    store(m, Mtx34{{{right.x, right.y, right.z, -dot(pos, right)},
                    {up.x, up.y, up.z, -dot(pos, up)},
                    {look.x, look.y, look.z, -dot(pos, look)}}});
    return 0;
}

// dst = m * src (a point: with the translation)
void PSMTXMultVec(u32 m, u32 src, u32 dst, f64, f64, f64, f64, f64, f64) {
    store(dst, transform(load<Mtx34>(m), load<Vec3>(src)));
}

// ---- texture projection matrices (3x4): the light's view volume mapped to texture
// coordinates, scaled by (scale_s, scale_t) and moved by (trans_s, trans_t)

// trans_t, the ninth float argument, is passed on the caller's stack
void C_MTXLightFrustum(u32 m, f64 t, f64 b, f64 l, f64 r, f64 n, f64 scale_s, f64 scale_t, f64 trans_s) {
    f32 trans_t = load_f32(guest::g_sp + 0x8);
    f32 tl = 1.0f / ((f32)r - (f32)l), tb = 1.0f / ((f32)t - (f32)b), n2 = 2.0f * (f32)n;
    store(m, Mtx34{{{(f32)scale_s * (n2 * tl), 0, (f32)scale_s * (tl * ((f32)r + (f32)l)) - (f32)trans_s, 0},
                    {0, (f32)scale_t * (n2 * tb), (f32)scale_t * (tb * ((f32)t + (f32)b)) - trans_t, 0},
                    {0, 0, -1, 0}}});
}

void C_MTXLightPerspective(u32 m, f64 fov_y, f64 aspect, f64 scale_s, f64 scale_t, f64 trans_s, f64 trans_t) {
    f32 cot = 1.0f / tanf(0.017453292f * (0.5f * (f32)fov_y));
    store(m, Mtx34{{{(f32)scale_s * (cot / (f32)aspect), 0, -(f32)trans_s, 0},
                    {0, cot * (f32)scale_t, -(f32)trans_t, 0},
                    {0, 0, -1, 0}}});
}

void C_MTXLightOrtho(u32 m, f64 t, f64 b, f64 l, f64 r, f64 scale_s, f64 scale_t, f64 trans_s, f64 trans_t) {
    f32 tl = 1.0f / ((f32)r - (f32)l), tb = 1.0f / ((f32)t - (f32)b);
    store(m, Mtx34{{{2.0f * tl * (f32)scale_s, 0, 0, (f32)trans_s + (f32)scale_s * (tl * -((f32)r + (f32)l))},
                    {0, 2.0f * tb * (f32)scale_t, 0, (f32)trans_t + (f32)scale_t * (tb * -((f32)t + (f32)b))},
                    {0, 0, 0, 1}}});
}

// ---- projection matrices (4x4, GX's conventions: clip z in [-w, 0])

void C_MTXFrustum(u32 m, f64 t, f64 b, f64 l, f64 r, f64 n, f64 f) {
    f32 tl = 1.0f / ((f32)r - (f32)l), tb = 1.0f / ((f32)t - (f32)b), tf = 1.0f / ((f32)f - (f32)n);
    f32 n2 = 2.0f * (f32)n;
    store(m, Mtx44{{{n2 * tl, 0, tl * ((f32)r + (f32)l), 0},
                    {0, n2 * tb, tb * ((f32)t + (f32)b), 0},
                    {0, 0, -(f32)n * tf, tf * -((f32)f * (f32)n)},
                    {0, 0, -1, 0}}});
}

void C_MTXPerspective(u32 m, f64 fov_y, f64 aspect, f64 n, f64 f) {
    f32 cot = 1.0f / tanf(0.017453292f * (0.5f * (f32)fov_y));
    f32 tf = 1.0f / ((f32)f - (f32)n);
    store(m, Mtx44{{{cot / (f32)aspect, 0, 0, 0},
                    {0, cot, 0, 0},
                    {0, 0, -(f32)n * tf, tf * -((f32)f * (f32)n)},
                    {0, 0, -1, 0}}});
}

void C_MTXOrtho(u32 m, f64 t, f64 b, f64 l, f64 r, f64 n, f64 f) {
    f32 tl = 1.0f / ((f32)r - (f32)l), tb = 1.0f / ((f32)t - (f32)b), tf = 1.0f / ((f32)f - (f32)n);
    store(m, Mtx44{{{2.0f * tl, 0, 0, tl * -((f32)r + (f32)l)},
                    {0, 2.0f * tb, 0, tb * -((f32)t + (f32)b)},
                    {0, 0, -1.0f * tf, -(f32)f * tf},
                    {0, 0, 0, 1}}});
}

// ---- Vec

void PSVECAdd(u32 a, u32 b, u32 ab) {
    store(ab, load<Vec3>(a) + load<Vec3>(b));
}

// (src, dst); f1 was left holding the Newton step's 3.0
f64 PSVECNormalize(u32 src, u32 dst) {
    store(dst, normalize(load<Vec3>(src)));
    return 3.0;
}

f64 PSVECMag(u32 v, u32) {
    return length(load<Vec3>(v));
}

f64 PSVECDotProduct(u32 a, u32 b) {
    return dot(load<Vec3>(a), load<Vec3>(b));
}

void PSVECCrossProduct(u32 a, u32 b, u32 axb) {
    store(axb, cross(load<Vec3>(a), load<Vec3>(b)));
}

// the half-angle vector of two directions (lighting): normalize(-a/|a| - b/|b|)
void C_VECHalfAngle(u32 a, u32 b, u32 half) {
    Vec3 h = normalize(-load<Vec3>(a)) + normalize(-load<Vec3>(b));
    store(half, dot(h, h) > 0.0f ? normalize(h) : h);
}

f64 PSVECSquareDistance(u32 a, u32 b) {
    Vec3 d = load<Vec3>(a) - load<Vec3>(b);
    return dot(d, d);
}

// ---- Quat

// the rotation of the 3x3 part of m (Shoemake's method)
u32 C_QUATMtx(u32 q, u32 m) {
    Mtx34 a = load<Mtx34>(m);
    f32 tr = a.m[0][0] + a.m[1][1] + a.m[2][2];
    Quat r;
    if (tr > 0.0f) {
        f32 s = sqrtf(tr + 1.0f);
        r.w = 0.5f * s;
        s = 0.5f / s;
        r.x = s * (a.m[2][1] - a.m[1][2]);
        r.y = s * (a.m[0][2] - a.m[2][0]);
        r.z = s * (a.m[1][0] - a.m[0][1]);
    } else {
        static const int next[3] = {1, 2, 0};
        int i = a.m[1][1] > a.m[0][0] ? 1 : 0;
        if (a.m[2][2] > a.m[i][i])
            i = 2;
        int j = next[i], k = next[j];
        f32 s = sqrtf(a.m[i][i] - (a.m[j][j] + a.m[k][k]) + 1.0f);
        f32 v[3];
        v[i] = 0.5f * s;
        if (s != 0.0f)
            s = 0.5f / s;
        v[j] = s * (a.m[i][j] + a.m[j][i]);
        v[k] = s * (a.m[i][k] + a.m[k][i]);
        r = {v[0], v[1], v[2], s * (a.m[k][j] - a.m[j][k])};
    }
    store(q, r);
    return 0;
}

// spherical interpolation from p (t = 0) to q (t = 1), the short way round
f64 C_QUATSlerp(u32 p, u32 q, u32 out, f64 t) {
    Quat a = load<Quat>(p), b = load<Quat>(q);
    f32 ft = (f32)t;
    f32 cosom = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    f32 sign = 1.0f;
    if (cosom < 0.0f) {
        cosom = -cosom;
        sign = -1.0f;
    }
    f32 s0, s1;
    if (cosom <= 0.99999f) {
        f32 omega = acosf(cosom), sinom = sinf(omega);
        s0 = sinf((1.0f - ft) * omega) / sinom;
        s1 = sign * (sinf(ft * omega) / sinom);
    } else {  // nearly the same: linear
        s0 = 1.0f - ft;
        s1 = sign * ft;
    }
    store(out, Quat{s0 * a.x + s1 * b.x, s0 * a.y + s1 * b.y, s0 * a.z + s1 * b.z, s0 * a.w + s1 * b.w});
    return s0 * a.w;
}

}  // namespace g
