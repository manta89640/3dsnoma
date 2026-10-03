// nw4r::lyt's animation and pane matrices (lyt_animation.cpp, lyt_pane.cpp), native: every
// frame each layout's animations evaluate their curves (Hermite for values, steps for switches)
// into panes and materials, then every pane's matrix is rebuilt from its scale, rotation and
// translation under its parent's. The matrices go through engine/math in the order the SDK's
// PSMTX calls do, so the results are the same as from the decompiled code.
#include "noma_decomp.h"
#include "../math/vecmath.h"
#include "../snd/guest_obj.h"

namespace engine::lyt {

using engine::snd::GuestObj;
using engine::snd::fctiwz;
using namespace engine::math;

namespace {

// ---- curves: keys in guest memory
// Hermite keys: f32 frame, f32 value, f32 slope; step keys: f32 frame, u16 value, u16

// the key segment holding `frame`: lo, hi (binary search, as nw4r's)
template <class Before>
void find_segment(u32 keys, u32 stride, s32 n, Before before, s32& lo, s32& hi) {
    lo = 0;
    hi = n - 1;
    while (lo != hi - 1 && lo != hi) {
        s32 mid = (lo + hi) / 2;
        if (before(GuestObj(keys + (u32)mid * stride).getf(0)))
            hi = mid;
        else
            lo = mid;
    }
}

f32 hermite(u32 keys, u32 n, f32 frame) {
    GuestObj k(keys);
    if (n == 1 || frame <= k.getf(0))
        return k.getf(4);
    GuestObj last(keys + (n - 1) * 12);
    if (frame >= last.getf(0))
        return last.getf(4);
    s32 lo, hi;
    find_segment(keys, 12, (s32)n, [&](f32 f) { return frame <= f; }, lo, hi);
    GuestObj a(keys + (u32)lo * 12), b(keys + (u32)hi * 12);
    f32 d = frame - b.getf(0);
    if (-0.001f < d && d < 0.001f) {  // on a key: a jump takes the later value
        if ((u32)hi < n - 1) {
            GuestObj c(keys + (u32)(hi + 1) * 12);
            if (b.getf(0) == c.getf(0))
                return c.getf(4);
        }
        return b.getf(4);
    }
    const f32 t0 = a.getf(0);
    const f32 dt = frame - t0;
    const f32 inv = 1.0f / (b.getf(0) - t0);
    const f32 s2 = inv * (dt * dt);      // t^2 * len
    const f32 s2n = s2 * inv;            // t^2
    const f32 s3 = dt * s2n;             // t^3 * len
    const f32 t2x3 = 3.0f * s2n;         // 3 t^2
    const f32 t3 = s3 * inv;             // t^3
    return (b.getf(8) * (s3 - s2)) +
           ((a.getf(8) * (dt + (s3 - (2.0f * s2)))) +
            ((a.getf(4) * (1.0f + ((2.0f * t3) - t2x3))) + (b.getf(4) * ((-2.0f * t3) + t2x3))));
}

u16 step(u32 keys, u32 n, f32 frame) {
    GuestObj k(keys);
    if (n == 1 || frame <= k.getf(0))
        return k.get16(4);
    GuestObj last(keys + (n - 1) * 8);
    if (frame >= last.getf(0))
        return last.get16(4);
    s32 lo, hi;
    find_segment(keys, 8, (s32)n, [&](f32 f) { return frame < f; }, lo, hi);
    f32 d = frame - GuestObj(keys + (u32)hi * 8).getf(0);
    if (-0.001f < d && d < 0.001f)
        return GuestObj(keys + (u32)hi * 8).get16(4);
    return GuestObj(keys + (u32)lo * 8).get16(4);
}

// the paired-single stores through GQR2 (u8) and GQR5 (s16), unscaled: truncated, saturated
u8 quantize_u8(f32 x) { return (u8)(x < 0.0f ? 0 : x > 255.0f ? 255 : (s32)x); }
s16 quantize_s16(f32 x) { return (s16)(x < -32768.0f ? -32768 : x > 32767.0f ? 32767 : (s32)x); }

// ---- AnimTransformBasic (an RLAN resource and a frame)
enum : u32 {
    AT_RESOURCE = 0x0C,  // the RLAN data
    AT_FRAME = 0x10,     // f32
    AT_TEXTURES = 0x14,  // TPL pointers of a texture pattern animation
};
enum : u32 {  // animation kinds (the info's magic)
    RLPA = 0x524C5041, RLVI = 0x524C5649, RLVC = 0x524C5643,  // pane: SRT, visibility, vertex color
    RLMC = 0x524C4D43, RLTS = 0x524C5453, RLTP = 0x524C5450, RLIM = 0x524C494D,  // material
};

// each animation info of content `index`: f(info, number of targets, first target offset)
template <class F>
void for_each_info(GuestObj at, u32 index, F f) {
    u32 res = at.get32(AT_RESOURCE);
    u32 content = res + guest::load32(res + guest::load32(res + 0x10) + index * 4);
    u32 infos = guest::load8(content + 0x14);
    for (u32 i = 0; i < infos; i++) {
        u32 info = content + guest::load32(content + 0x18 + i * 4);
        f(info, (u32)guest::load8(info + 4));
    }
}
u32 target_at(u32 info, u32 i) { return info + guest::load32(info + 8 + i * 4); }
u32 target_keys(u32 target) { return target + guest::load32(target + 8); }
u32 target_count(u32 target) { return guest::load16(target + 4); }

void animate_pane(u32 at_addr, u32 index, u32 pane) {
    GuestObj at(at_addr), p(pane);
    const f32 frame = at.getf(AT_FRAME);
    for_each_info(at, index, [&](u32 info, u32 targets) {
        switch (guest::load32(info)) {
        case RLPA:  // translate, rotate, scale, size
            for (u32 i = 0; i < targets; i++) {
                u32 t = target_at(info, i);
                p.setf(0x2C + guest::load8(t + 1) * 4, hermite(target_keys(t), target_count(t), frame));
            }
            break;
        case RLVI:  // visible
            for (u32 i = 0; i < targets; i++) {
                u32 t = target_at(info, i);
                u32 v = step(target_keys(t), target_count(t), frame);
                p.set8(0xBB, (u8)((p.get8(0xBB) & ~1u) | ((-(s32)v | (s32)v) >> 31 & 1)));
            }
            break;
        case RLVC:  // vertex colors and alpha: Pane::SetColorElement (virtual)
            for (u32 i = 0; i < targets; i++) {
                u32 t = target_at(info, i);
                u8 v = quantize_u8(hermite(target_keys(t), target_count(t), frame) + 0.5f);
                u32 fn = guest::load32(guest::load32(pane) + 0x30);
                gcall<u32>(fn, pane, (u32)guest::load8(t + 1), (u32)v);
            }
            break;
        }
    });
}

// Material's arrays after its colors (the counts are bit fields of its 0x38 word)
u32 tex_srt_array(GuestObj m) { return m.get32(0x40) + (m.get32(0x38) >> 28) * 0x1C; }
u32 ind_srt_array(GuestObj m) {
    u32 b = m.get32(0x38);
    return ((b >> 12) & 4) + ((b >> 18) & 0x3C) + (((b >> 4) & 4) + (((b >> 5) & 4) + ((b >> 6) & 4))) +
           ((((b >> 24) & 0xF) * 0x14) + ((b >> 28) * 0x1C)) + (m.get32(0x40) + (((b >> 13) & 0x1C) + ((b >> 3) & 4)));
}

void animate_material(u32 at_addr, u32 index, u32 material) {
    GuestObj at(at_addr), m(material);
    const f32 frame = at.getf(AT_FRAME);
    for_each_info(at, index, [&](u32 info, u32 targets) {
        switch (guest::load32(info)) {
        case RLMC:  // colors: Material::SetColorElement
            for (u32 i = 0; i < targets; i++) {
                u32 t = target_at(info, i);
                s32 v = quantize_s16(hermite(target_keys(t), target_count(t), frame) + 0.5f);
                v = v < -0x400 ? -0x400 : v;
                v = (s16)v > 0x3FF ? 0x3FF : v;
                g::nw4r_lyt_Material_SetColorElement(material, guest::load8(t + 1), (u32)(s32)(s16)v);
            }
            break;
        case RLTS:  // texture SRT
            for (u32 i = 0; i < targets; i++) {
                u32 t = target_at(info, i);
                if (guest::load8(t) < ((m.get32(0x38) >> 24) & 0xF)) {
                    f32 v = hermite(target_keys(t), target_count(t), frame);
                    GuestObj(tex_srt_array(m) + guest::load8(t) * 0x14).setf(guest::load8(t + 1) * 4, v);
                }
            }
            break;
        case RLTP: {  // texture pattern
            u32 textures = at.get32(AT_TEXTURES);
            if (!textures)
                break;
            for (u32 i = 0; i < targets; i++) {
                u32 t = target_at(info, i);
                if (guest::load8(t) < (m.get32(0x3C) >> 28)) {
                    u32 n = step(target_keys(t), target_count(t), frame);
                    u32 tpl = guest::load32(textures + ((n * 4) & 0x3FFFC));
                    if (tpl)
                        g::nw4r_lyt_TexMap_Set(m.get32(0x40) + guest::load8(t) * 0x1C, tpl, 0);
                }
            }
            break;
        }
        case RLIM:  // indirect texture SRT
            for (u32 i = 0; i < targets; i++) {
                u32 t = target_at(info, i);
                if (guest::load8(t) < ((m.get32(0x38) >> 18) & 3)) {
                    f32 v = hermite(target_keys(t), target_count(t), frame);
                    GuestObj(ind_srt_array(m) + guest::load8(t) * 0x14).setf(guest::load8(t + 1) * 4, v);
                }
            }
            break;
        }
    });
}

// ---- Pane::CalculateMtx
enum : u32 {
    P_PARENT = 0x0C,
    P_CHILDREN = 0x14,      // list head; a child's node is at child + 4
    P_TRANSLATE = 0x2C,     // f32 x3
    P_ROTATE = 0x38,        // f32 x3, degrees
    P_SCALE = 0x44,         // f32 x2
    P_MTX = 0x54,           // Mtx34: local
    P_GLOBAL_MTX = 0x84,    // Mtx34
    P_ALPHA = 0xB8,
    P_GLOBAL_ALPHA = 0xB9,
    P_FLAGS = 0xBB,         // 1 visible, 2 influenced alpha, 4 location adjust
};
enum : u32 {
    DI_VIEW_MTX = 0x04,
    DI_LOCATION_SCALE = 0x44,  // f32 x2
    DI_GLOBAL_ALPHA = 0x4C,
    DI_FLAGS = 0x50,           // u8: 0x10 invisible panes too, 0x20 location adjust, 0x40 influenced alpha, 0x80 local only
};
const u32 PANE_CALCULATE_MTX = 0x80458D90;

// PSMTXRotRad(m, axis, rad) as engine/math computes it (sinf, cosf of the f32 angle)
Mtx34 rot_axis(char axis, f32 rad) {
    f32 s = sinf(rad), c = cosf(rad);
    switch (axis) {
    case 'x': return {{{1, 0, 0, 0}, {0, c, -s, 0}, {0, s, c, 0}}};
    case 'y': return {{{c, 0, s, 0}, {0, 1, 0, 0}, {-s, 0, c, 0}}};
    default: return {{{c, -s, 0, 0}, {s, c, 0, 0}, {0, 0, 1, 0}}};
    }
}

void calculate_mtx(u32 pane, u32 draw_info) {
    GuestObj p(pane), di(draw_info);
    const u8 flags = p.get8(P_FLAGS);
    if (!(flags & 1) && !((di.get8(DI_FLAGS) >> 4) & 1))
        return;
    f32 sx = p.getf(P_SCALE), sy = p.getf(P_SCALE + 4);
    if (((di.get8(DI_FLAGS) >> 5) & 1) && (flags & 4)) {
        sx = sx * di.getf(DI_LOCATION_SCALE);
        sy = sy * di.getf(DI_LOCATION_SCALE + 4);
    }
    const Mtx34 scale = {{{sx, 0, 0, 0}, {0, sy, 0, 0}, {0, 0, 1.0f, 0}}};
    const f32 deg = 0.017453292f;
    Mtx34 m = concat(rot_axis('x', deg * p.getf(P_ROTATE)), scale);
    m = concat(rot_axis('y', deg * p.getf(P_ROTATE + 4)), m);
    m = concat(rot_axis('z', deg * p.getf(P_ROTATE + 8)), m);
    for (int i = 0; i < 3; i++)  // PSMTXTransApply
        m.m[i][3] += p.getf(P_TRANSLATE + 4 * i);
    store(pane + P_MTX, m);
    if (u32 parent = p.get32(P_PARENT))
        store(pane + P_GLOBAL_MTX, concat(load<Mtx34>(parent + P_GLOBAL_MTX), m));
    else if ((di.get8(DI_FLAGS) >> 7) & 1)
        store(pane + P_GLOBAL_MTX, m);
    else
        store(pane + P_GLOBAL_MTX, concat(load<Mtx34>(draw_info + DI_VIEW_MTX), m));

    if (((di.get8(DI_FLAGS) >> 6) & 1) && p.get32(P_PARENT))
        p.set8(P_GLOBAL_ALPHA, (u8)fctiwz((f32)p.get8(P_ALPHA) * di.getf(DI_GLOBAL_ALPHA)));
    else
        p.set8(P_GLOBAL_ALPHA, p.get8(P_ALPHA));

    // children: under an alpha that is not opaque, with it applied to them
    const f32 saved_alpha = di.getf(DI_GLOBAL_ALPHA);
    const bool saved_influence = (di.get8(DI_FLAGS) >> 6) & 1;
    const bool influence = (p.get8(P_FLAGS) & 2) && p.get8(P_ALPHA) != 0xFF;
    if (influence) {
        di.set8(DI_FLAGS, di.get8(DI_FLAGS) | 0x40);
        di.setf(DI_GLOBAL_ALPHA, 0.003921569f * (saved_alpha * (f32)p.get8(P_ALPHA)));
    }
    for (u32 node = p.get32(P_CHILDREN); node != pane + P_CHILDREN; node = guest::load32(node)) {
        u32 child = node - 4;
        u32 fn = guest::load32(guest::load32(child) + 0x10);
        if (fn == PANE_CALCULATE_MTX)
            calculate_mtx(child, draw_info);
        else
            gcall<u32>(fn, child, draw_info);
    }
    if (influence) {
        di.setf(DI_GLOBAL_ALPHA, saved_alpha);
        di.set8(DI_FLAGS, (u8)((di.get8(DI_FLAGS) & ~0x40u) | (saved_influence ? 0x40 : 0)));
    }
}

}  // namespace

}  // namespace engine::lyt

// ---- the guest ABI (decl.h)
namespace g {

using namespace engine::lyt;

f64 nw4r_lyt_GetHermiteCurveValue(u32 keys, u32 n, f64 frame) {
    return hermite(keys, n, (f32)frame);
}
u32 nw4r_lyt_GetStepCurveValue(u32 keys, u32 n, f64 frame) {
    return step(keys, n, (f32)frame);
}
u32 nw4r_lyt_AnimTransformBasic_AnimatePane(u32 at, u32 index, u32 pane) {
    NOMA_PROF_SCOPE("(lyt animate)");
    animate_pane(at, index, pane);
    return 0;
}
u32 nw4r_lyt_AnimTransformBasic_AnimateMaterial(u32 at, u32 index, u32 material) {
    NOMA_PROF_SCOPE("(lyt animate)");
    animate_material(at, index, material);
    return 0;
}
u32 nw4r_lyt_Pane_CalculateMtx(u32 pane, u32 draw_info) {
    NOMA_PROF_SCOPE("(lyt matrices)");
    calculate_mtx(pane, draw_info);
    return 0;
}

}  // namespace g
