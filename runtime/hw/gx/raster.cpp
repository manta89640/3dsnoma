// A software implementation of the GPU's back end, for the host build: clipping, triangle
// rasterization, the TEV (texture environment) stages, alpha test, depth and blending into an
// embedded frame buffer (EFB, 640x528), and EFB copies to the external frame buffer (YUYV in
// main memory, which the video interface shows) or to textures.
//
// It follows the hardware's arithmetic where that is cheap (TEV: 8-bit inputs, 11-bit signed
// registers, the lerp's rounding; 24-bit depth) and is the reference the 3DS renderer is
// compared with.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <sys/stat.h>

#include "raster.h"
#include "regs.h"
#include "texture.h"
#include "vertex.h"
#include "engine/g3d/native_draw.h"

namespace guest {

namespace {

const int EFB_W = 640, EFB_H = 528;
std::vector<u32> s_color(EFB_W * EFB_H);  // R in bits 0-7 ... A in bits 24-31
std::vector<u32> s_depth(EFB_W * EFB_H, 0xFFFFFF);

XfbCopy s_last_xfb;
XfbCopy s_regions[8];  // frame buffers, built up from the copies into them (often two halves)
bool s_warned_ind, s_warned_fog, s_warned_zcopy;

// ---------------------------------------------------------------- per-draw state
struct Stage {
    u32 cenv, aenv;
    bool tex_on;
    u32 texmap, texcoord, chan;
    u32 kcsel, kasel;
};

struct State {
    u32 num_stages, num_tex, num_colors;
    Stage stage[16];
    s32 reg[4][4];    // TEV registers PREV, C0, C1, C2 (RGBA, 11-bit signed)
    s32 konst[4][4];  // K0-K3
    u8 swap[4][4];    // swap tables: source channel of R, G, B, A
    const GxTexture* tex[8];
    u32 tex_mode0[8];
    u32 acmp;
    u32 zmode, cmode0, cmode1, pixfmt;
    bool early_z;
    s32 sc_l, sc_t, sc_r, sc_b;  // scissor, inclusive, EFB coordinates
    f32 xoff, yoff;              // viewport origin minus the scissor offset
    // on the bottom screen: the frame's rectangle that fills it (its corner, and the scale to the
    // whole bottom buffer), as gx_pica.cpp maps each draw there
    bool map;
    f32 mx, my, kx, ky;
};
State S;

// The bottom screen (SoftRenderer::screen): its buffers are swapped in while it is drawn on (s_color
// and s_depth are what every draw writes), and each draw there maps the frame's rectangle given
// with it onto the whole of them (a scene's region of its layouts, engine/ui/screen.h), so draws in
// different rectangles in one frame land as on the 3DS
bool s_on_bottom;
u32 s_bottom_rect[4];  // the current rectangle: x, y, w, h in the frame

s32 sext11(u32 v) { return (s32)(v << 21) >> 21; }
s32 to_int(f32 f);
s32 floor_int(f32 f);

void load_state() {
    const u32* bp = g_gx.bp;
    u32 gen = bp[BP_GENMODE];
    S.num_tex = bits(gen, 0, 4);
    S.num_colors = bits(gen, 4, 3);
    S.num_stages = bits(gen, 10, 4) + 1;
    for (u32 i = 0; i < S.num_stages; i++) {
        Stage& st = S.stage[i];
        st.cenv = bp[BP_TEV_COLOR_ENV + 2 * i];
        st.aenv = bp[BP_TEV_COLOR_ENV + 2 * i + 1];
        u32 tref = bp[BP_TREF + i / 2] >> ((i & 1) * 12);
        st.texmap = bits(tref, 0, 3);
        st.texcoord = bits(tref, 3, 3);
        st.tex_on = bits(tref, 6, 1);
        st.chan = bits(tref, 7, 3);
        u32 ksel = bp[BP_TEV_KSEL + i / 2] >> ((i & 1) * 10);
        st.kcsel = bits(ksel, 4, 5);
        st.kasel = bits(ksel, 9, 5);
        if (bp[BP_IND_CMD + i] != 0 && !s_warned_ind) {
            s_warned_ind = true;
            log("gx: indirect texturing used (stage %u: %06X) -- not emulated", i, bp[BP_IND_CMD + i]);
        }
    }
    for (int k = 0; k < 4; k++)
        for (int c = 0; c < 4; c++)
            S.swap[k][c] = (u8)bits(bp[BP_TEV_KSEL + 2 * k + (c >> 1)], (c & 1) * 2, 2);
    for (u32 m = 0; m < 8; m++) {
        S.tex[m] = nullptr;
        S.tex_mode0[m] = bp[bp_tex(BP_TX_SETMODE0, m)];
    }
    for (u32 i = 0; i < S.num_stages; i++)
        if (S.stage[i].tex_on && !S.tex[S.stage[i].texmap])
            S.tex[S.stage[i].texmap] = gx_texture(S.stage[i].texmap);
    memcpy(S.reg, g_gx.tev_reg, sizeof(S.reg));
    memcpy(S.konst, g_gx.tev_konst, sizeof(S.konst));
    S.acmp = bp[BP_ALPHACOMPARE];
    S.zmode = bp[BP_ZMODE];
    S.cmode0 = bp[BP_CMODE0];
    S.cmode1 = bp[BP_CMODE1];
    S.pixfmt = bits(bp[BP_ZCONTROL], 0, 3);
    S.early_z = bits(bp[BP_ZCONTROL], 6, 1);
    s32 sox = (s32)bits(bp[BP_SCISSOR_OFFSET], 0, 10) * 2, soy = (s32)bits(bp[BP_SCISSOR_OFFSET], 10, 10) * 2;
    S.sc_l = (s32)bits(bp[BP_SCISSOR_TL], 12, 11) - sox;
    S.sc_t = (s32)bits(bp[BP_SCISSOR_TL], 0, 11) - soy;
    S.sc_r = (s32)bits(bp[BP_SCISSOR_BR], 12, 11) - sox;
    S.sc_b = (s32)bits(bp[BP_SCISSOR_BR], 0, 11) - soy;
    S.sc_l = std::max<s32>(S.sc_l, 0), S.sc_t = std::max<s32>(S.sc_t, 0);
    S.sc_r = std::min<s32>(S.sc_r, EFB_W - 1), S.sc_b = std::min<s32>(S.sc_b, EFB_H - 1);
    S.xoff = (f32)sox;
    S.yoff = (f32)soy;
    S.map = s_on_bottom;
    if (S.map) {
        S.mx = (f32)s_bottom_rect[0], S.my = (f32)s_bottom_rect[1];
        S.kx = (f32)EFB_W / (f32)s_bottom_rect[2], S.ky = (f32)EFB_H / (f32)s_bottom_rect[3];
        S.sc_l = std::max<s32>(floor_int(((f32)S.sc_l - S.mx) * S.kx), 0);
        S.sc_t = std::max<s32>(floor_int(((f32)S.sc_t - S.my) * S.ky), 0);
        S.sc_r = std::min<s32>(to_int(std::ceil(((f32)S.sc_r + 1 - S.mx) * S.kx)) - 1, EFB_W - 1);
        S.sc_b = std::min<s32>(to_int(std::ceil(((f32)S.sc_b + 1 - S.my) * S.ky)) - 1, EFB_H - 1);
    }
    if (bits(bp[BP_FOG_PARAM3], 21, 3) && !s_warned_fog) {
        s_warned_fog = true;
        log("gx: fog used -- not emulated");
    }
}

// ---------------------------------------------------------------- TEV
const s32 k_konst[8] = {255, 223, 191, 159, 128, 96, 64, 32};

s32 clamp(s32 v, s32 lo, s32 hi) { return v < lo ? lo : v > hi ? hi : v; }

// float -> int without undefined behaviour for NaN, infinities and huge values
s32 to_int(f32 f) {
    if (!(f > -1e9f))
        return -1000000000;
    if (!(f < 1e9f))
        return 1000000000;
    return (s32)f;
}
s32 floor_int(f32 f) { return to_int(std::floor(f)); }

struct Pixel {
    s32 ras[2][4];        // rasterized color channels, 0-255
    f32 tc[8][2];         // texture coordinates (s/q, t/q)
};

void sample(u32 map, f32 s, f32 t, s32* out) {
    const GxTexture* tex = S.tex[map];
    if (!tex || tex->rgba.empty()) {
        out[0] = out[1] = out[2] = out[3] = 255;
        return;
    }
    u32 mode = S.tex_mode0[map];
    u32 wrap_s = bits(mode, 0, 2), wrap_t = bits(mode, 2, 2);
    bool linear = bits(mode, 4, 1);
    s32 w = (s32)tex->width, h = (s32)tex->height;
    auto wrap = [](s32 i, s32 n, u32 mode) {
        switch (mode) {
        case 1: return ((i % n) + n) % n;
        case 2: {
            s32 p = ((i % (2 * n)) + 2 * n) % (2 * n);
            return p < n ? p : 2 * n - 1 - p;
        }
        default: return clamp(i, 0, n - 1);
        }
    };
    auto texel = [&](s32 x, s32 y) {
        s32 ty = wrap(y, h, wrap_t), tx = wrap(x, w, wrap_s);
        size_t i = (size_t)ty * (size_t)w + (size_t)tx;
        if (ty < 0 || tx < 0 || i >= tex->rgba.size()) {
            static int n;
            if (n++ < 5)
                log("gx: texel (%d,%d) of %dx%d (%zu texels) from (%d,%d) modes %u/%u", tx, ty, w, h,
                    tex->rgba.size(), x, y, wrap_s, wrap_t);
            return (u32)0;
        }
        return (u32)tex->rgba[i];
    };
    f32 u = s * (f32)w, v = t * (f32)h;
    if (!linear) {
        u32 c = texel(floor_int(u), floor_int(v));
        out[0] = c & 255, out[1] = (c >> 8) & 255, out[2] = (c >> 16) & 255, out[3] = c >> 24;
        return;
    }
    u -= 0.5f, v -= 0.5f;
    s32 x0 = floor_int(u), y0 = floor_int(v);
    s32 fx = clamp(to_int((u - (f32)x0) * 128.0f), 0, 128), fy = clamp(to_int((v - (f32)y0) * 128.0f), 0, 128);
    u32 c00 = texel(x0, y0), c10 = texel(x0 + 1, y0), c01 = texel(x0, y0 + 1), c11 = texel(x0 + 1, y0 + 1);
    for (int i = 0; i < 4; i++) {
        s32 a = (c00 >> (8 * i)) & 255, b = (c10 >> (8 * i)) & 255, c = (c01 >> (8 * i)) & 255,
            d = (c11 >> (8 * i)) & 255;
        s32 top = a * (128 - fx) + b * fx, bot = c * (128 - fx) + d * fx;
        out[i] = (top * (128 - fy) + bot * fy) >> 14;
    }
}

void konst_color(u32 sel, s32* out) {
    if (sel < 8) {
        out[0] = out[1] = out[2] = k_konst[sel];
    } else if (sel >= 12 && sel < 16) {
        for (int i = 0; i < 3; i++)
            out[i] = S.konst[sel - 12][i];
    } else if (sel >= 16) {
        s32 v = S.konst[(sel - 16) & 3][(sel - 16) >> 2];
        out[0] = out[1] = out[2] = v;
    } else {
        out[0] = out[1] = out[2] = 0;
    }
}

s32 konst_alpha(u32 sel) {
    if (sel < 8)
        return k_konst[sel];
    if (sel >= 16)
        return S.konst[(sel - 16) & 3][(sel - 16) >> 2];
    return 0;
}

const int k_lshift[4] = {0, 1, 2, 0};
const int k_rshift[4] = {0, 0, 0, 1};
const s32 k_bias[4] = {0, 128, -128, 0};

s32 tev_regular(s32 a, s32 b, s32 c, s32 d, u32 env) {
    u32 bias = bits(env, 16, 2), sub = bits(env, 18, 1), scale = bits(env, 20, 2);
    a &= 255, b &= 255, c &= 255;
    c += c >> 7;
    s32 t = a * (256 - c) + b * c;
    t <<= k_lshift[scale];
    t += (scale == 3) ? 0 : sub ? 127 : 128;
    t >>= 8;
    if (sub)
        t = -t;
    s32 r = ((d + k_bias[bias]) << k_lshift[scale]) + t;
    return r >> k_rshift[scale];
}

// returns false when the alpha test discards the pixel
bool run_tev(const Pixel& px, s32* out) {
    s32 reg[4][4];
    memcpy(reg, S.reg, sizeof(reg));
    for (u32 si = 0; si < S.num_stages; si++) {
        const Stage& st = S.stage[si];
        // rasterized color and texture, through the swap tables
        s32 ras[4] = {0, 0, 0, 0}, tex[4] = {255, 255, 255, 255};
        u32 rswap = bits(st.aenv, 0, 2), tswap = bits(st.aenv, 2, 2);
        if (st.chan < 2) {
            for (int i = 0; i < 4; i++)
                ras[i] = px.ras[st.chan][S.swap[rswap][i]];
        }
        if (st.tex_on) {
            s32 t[4];
            sample(st.texmap, px.tc[st.texcoord][0], px.tc[st.texcoord][1], t);
            for (int i = 0; i < 4; i++)
                tex[i] = t[S.swap[tswap][i]];
        }
        s32 kc[3];
        konst_color(st.kcsel, kc);
        s32 ka = konst_alpha(st.kasel);

        auto color_in = [&](u32 arg, int ch) -> s32 {
            switch (arg) {
            case 0: return reg[0][ch];
            case 1: return reg[0][3];
            case 2: return reg[1][ch];
            case 3: return reg[1][3];
            case 4: return reg[2][ch];
            case 5: return reg[2][3];
            case 6: return reg[3][ch];
            case 7: return reg[3][3];
            case 8: return tex[ch];
            case 9: return tex[3];
            case 10: return ras[ch];
            case 11: return ras[3];
            case 12: return 255;
            case 13: return 128;
            case 14: return kc[ch];
            default: return 0;
            }
        };
        auto alpha_in = [&](u32 arg) -> s32 {
            switch (arg) {
            case 0: return reg[0][3];
            case 1: return reg[1][3];
            case 2: return reg[2][3];
            case 3: return reg[3][3];
            case 4: return tex[3];
            case 5: return ras[3];
            case 6: return ka;
            default: return 0;
            }
        };

        // color
        u32 ce = st.cenv;
        u32 cd = bits(ce, 0, 4), cc = bits(ce, 4, 4), cb = bits(ce, 8, 4), ca = bits(ce, 12, 4);
        s32 res[4];
        if (bits(ce, 16, 2) == 3) {  // compare
            u32 mode = bits(ce, 20, 2), eq = bits(ce, 18, 1);
            s32 A[3], B[3];
            for (int i = 0; i < 3; i++)
                A[i] = color_in(ca, i) & 255, B[i] = color_in(cb, i) & 255;
            for (int i = 0; i < 3; i++) {
                u32 x, y;
                switch (mode) {
                case 0: x = A[0], y = B[0]; break;
                case 1: x = A[1] << 8 | A[0], y = B[1] << 8 | B[0]; break;
                case 2: x = A[2] << 16 | A[1] << 8 | A[0], y = B[2] << 16 | B[1] << 8 | B[0]; break;
                default: x = A[i], y = B[i]; break;
                }
                bool pass = eq ? x == y : x > y;
                res[i] = sext11((u32)color_in(cd, i)) + (pass ? (color_in(cc, i) & 255) : 0);
            }
        } else {
            for (int i = 0; i < 3; i++)
                res[i] = tev_regular(color_in(ca, i), color_in(cb, i), color_in(cc, i),
                                     sext11((u32)color_in(cd, i)), ce);
        }
        // alpha
        u32 ae = st.aenv;
        u32 ad = bits(ae, 4, 3), ac = bits(ae, 7, 3), ab = bits(ae, 10, 3), aa = bits(ae, 13, 3);
        if (bits(ae, 16, 2) == 3) {
            // compare: A8 compares the alpha inputs; R8, GR16 and BGR24 compare the color
            // combiner's A and B inputs, as for the color channels
            u32 mode = bits(ae, 20, 2), eq = bits(ae, 18, 1);
            u32 x, y;
            if (mode == 3) {
                x = alpha_in(aa) & 255, y = alpha_in(ab) & 255;
            } else {
                s32 A[3], B[3];
                for (int i = 0; i < 3; i++)
                    A[i] = color_in(ca, i) & 255, B[i] = color_in(cb, i) & 255;
                if (mode == 0)
                    x = A[0], y = B[0];
                else if (mode == 1)
                    x = A[1] << 8 | A[0], y = B[1] << 8 | B[0];
                else
                    x = A[2] << 16 | A[1] << 8 | A[0], y = B[2] << 16 | B[1] << 8 | B[0];
            }
            bool pass = eq ? x == y : x > y;
            res[3] = sext11((u32)alpha_in(ad)) + (pass ? (alpha_in(ac) & 255) : 0);
        } else {
            res[3] = tev_regular(alpha_in(aa), alpha_in(ab), alpha_in(ac), sext11((u32)alpha_in(ad)), ae);
        }
        u32 cdest = bits(ce, 22, 2), adest = bits(ae, 22, 2);
        bool cclamp = bits(ce, 19, 1), aclamp = bits(ae, 19, 1);
        for (int i = 0; i < 3; i++)
            reg[cdest][i] = cclamp ? clamp(res[i], 0, 255) : clamp(res[i], -1024, 1023);
        reg[adest][3] = aclamp ? clamp(res[3], 0, 255) : clamp(res[3], -1024, 1023);
        if (si == S.num_stages - 1) {
            out[0] = reg[cdest][0], out[1] = reg[cdest][1], out[2] = reg[cdest][2];
            out[3] = reg[adest][3];
        }
    }
    for (int i = 0; i < 4; i++)
        out[i] = clamp(out[i], 0, 255);

    // alpha test
    auto cmp = [](u32 f, s32 a, s32 ref) {
        switch (f) {
        case 0: return false;
        case 1: return a < ref;
        case 2: return a == ref;
        case 3: return a <= ref;
        case 4: return a > ref;
        case 5: return a != ref;
        case 6: return a >= ref;
        default: return true;
        }
    };
    u32 ac0 = bits(S.acmp, 16, 3), ac1 = bits(S.acmp, 19, 3), op = bits(S.acmp, 22, 2);
    if (ac0 == 7 && ac1 == 7 && op <= 1)
        return true;
    bool p0 = cmp(ac0, out[3], (s32)bits(S.acmp, 0, 8)), p1 = cmp(ac1, out[3], (s32)bits(S.acmp, 8, 8));
    switch (op) {
    case 0: return p0 && p1;
    case 1: return p0 || p1;
    case 2: return p0 != p1;
    default: return p0 == p1;
    }
}

// ---------------------------------------------------------------- frame buffer writes
bool depth_test(u32 idx, u32 z) {
    if (!(S.zmode & 1))
        return true;
    u32 cur = s_depth[idx];
    bool pass;
    switch (bits(S.zmode, 1, 3)) {
    case 0: pass = false; break;
    case 1: pass = z < cur; break;
    case 2: pass = z == cur; break;
    case 3: pass = z <= cur; break;
    case 4: pass = z > cur; break;
    case 5: pass = z != cur; break;
    case 6: pass = z >= cur; break;
    default: pass = true; break;
    }
    if (pass && (S.zmode & 0x10))
        s_depth[idx] = z;
    return pass;
}

u32 quantize(const s32* c) {
    u32 r = c[0], g = c[1], b = c[2], a = c[3];
    switch (S.pixfmt) {
    case 1:  // RGBA6
        r = (r >> 2) * 255 / 63, g = (g >> 2) * 255 / 63, b = (b >> 2) * 255 / 63, a = (a >> 2) * 255 / 63;
        break;
    case 2:  // RGB565
        r = (r >> 3) * 255 / 31, g = (g >> 2) * 255 / 63, b = (b >> 3) * 255 / 31, a = 255;
        break;
    default:  // RGB8
        a = 255;
        break;
    }
    return r | g << 8 | b << 16 | a << 24;
}

void blend_write(u32 idx, const s32* src) {
    u32 dstc = s_color[idx];
    s32 dst[4] = {(s32)(dstc & 255), (s32)((dstc >> 8) & 255), (s32)((dstc >> 16) & 255), (s32)(dstc >> 24)};
    s32 out[4] = {src[0], src[1], src[2], src[3]};
    u32 m = S.cmode0;
    if (m & 1) {  // blending
        if (m & 0x800) {  // subtract
            for (int i = 0; i < 4; i++)
                out[i] = clamp(dst[i] - src[i], 0, 255);
        } else {
            auto factor = [&](u32 f, bool is_src, int ch) -> s32 {
                switch (f) {
                case 0: return 0;
                case 1: return 255;
                case 2: return is_src ? dst[ch] : src[ch];
                case 3: return 255 - (is_src ? dst[ch] : src[ch]);
                case 4: return src[3];
                case 5: return 255 - src[3];
                case 6: return dst[3];
                default: return 255 - dst[3];
                }
            };
            u32 sf = bits(m, 8, 3), df = bits(m, 5, 3);
            for (int i = 0; i < 4; i++) {
                s32 v = (src[i] * factor(sf, true, i) + dst[i] * factor(df, false, i) + 127) / 255;
                out[i] = clamp(v, 0, 255);
            }
        }
    } else if (m & 2) {  // logic op
        u32 op = bits(m, 12, 4);
        for (int i = 0; i < 4; i++) {
            u32 s = (u32)src[i], d = (u32)dst[i], r;
            switch (op) {
            case 0: r = 0; break;
            case 1: r = s & d; break;
            case 2: r = s & ~d; break;
            case 3: r = s; break;
            case 4: r = ~s & d; break;
            case 5: r = d; break;
            case 6: r = s ^ d; break;
            case 7: r = s | d; break;
            case 8: r = ~(s | d); break;
            case 9: r = ~(s ^ d); break;
            case 10: r = ~d; break;
            case 11: r = s | ~d; break;
            case 12: r = ~s; break;
            case 13: r = ~s | d; break;
            case 14: r = ~(s & d); break;
            default: r = 255; break;
            }
            out[i] = (s32)(r & 255);
        }
    }
    if (S.cmode1 & 0x100)  // constant alpha
        out[3] = (s32)bits(S.cmode1, 0, 8);
    u32 q = quantize(out);
    u32 keep = 0;
    if (!(m & 0x08))
        keep |= 0x00FFFFFF;
    if (!(m & 0x10) || S.pixfmt != 1)
        keep |= 0xFF000000;
    if (S.pixfmt != 1)
        q |= 0xFF000000;
    s_color[idx] = (dstc & keep) | (q & ~keep);
}

// ---------------------------------------------------------------- triangles
struct RV {        // a vertex in screen space
    f32 x, y, z;   // EFB pixels, depth 0-0xFFFFFF
    f32 iw;        // 1/w
    f32 col[2][4]; // divided by w
    f32 tex[8][3];
};

void to_screen(const GxOutVertex& v, RV& o) {
    f32 iw = 1.0f / v.clip[3];
    o.iw = iw;
    // the viewport origin and the scissor offset both include the GPU's 342-pixel bias
    o.x = v.clip[0] * iw * xf_f32(XF_VIEWPORT + 0) + xf_f32(XF_VIEWPORT + 3) - S.xoff;
    o.y = v.clip[1] * iw * xf_f32(XF_VIEWPORT + 1) + xf_f32(XF_VIEWPORT + 4) - S.yoff;
    o.z = v.clip[2] * iw * xf_f32(XF_VIEWPORT + 2) + xf_f32(XF_VIEWPORT + 5);
    if (S.map)
        o.x = (o.x - S.mx) * S.kx, o.y = (o.y - S.my) * S.ky;
    for (int c = 0; c < 2; c++)
        for (int i = 0; i < 4; i++)
            o.col[c][i] = v.color[c][i] * iw;
    for (u32 t = 0; t < S.num_tex && t < 8; t++)
        for (int i = 0; i < 3; i++)
            o.tex[t][i] = v.tex[t][i] * iw;
}

void raster(const RV& v0, const RV& v1, const RV& v2) {
    // edge functions; area > 0 after ordering
    f32 area = (v1.x - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (v1.y - v0.y);
    if (area == 0 || !std::isfinite(area))
        return;
    const RV* a = &v0;
    const RV* b = &v1;
    const RV* c = &v2;
    if (area < 0) {
        std::swap(b, c);
        area = -area;
    }
    s32 minx = floor_int(std::min({a->x, b->x, c->x})), maxx = to_int(std::ceil(std::max({a->x, b->x, c->x})));
    s32 miny = floor_int(std::min({a->y, b->y, c->y})), maxy = to_int(std::ceil(std::max({a->y, b->y, c->y})));
    minx = std::max(minx, S.sc_l), maxx = std::min(maxx, S.sc_r);
    miny = std::max(miny, S.sc_t), maxy = std::min(maxy, S.sc_b);
    if (minx > maxx || miny > maxy)
        return;
    f32 inv_area = 1.0f / area;
    auto edge = [](const RV* p, const RV* q, f32 x, f32 y) { return (q->x - p->x) * (y - p->y) - (q->y - p->y) * (x - p->x); };
    // top-left rule: pixels exactly on an edge belong to top and left edges only
    auto top_left = [](const RV* p, const RV* q) {
        f32 dy = q->y - p->y, dx = q->x - p->x;
        return (dy < 0) || (dy == 0 && dx > 0);
    };
    bool t0 = top_left(b, c), t1 = top_left(c, a), t2 = top_left(a, b);
    for (s32 y = miny; y <= maxy; y++) {
        f32 py = (f32)y + 0.5625f;
        for (s32 x = minx; x <= maxx; x++) {
            f32 px = (f32)x + 0.5625f;
            f32 w0 = edge(b, c, px, py), w1 = edge(c, a, px, py), w2 = edge(a, b, px, py);
            if (w0 < 0 || w1 < 0 || w2 < 0 || (w0 == 0 && !t0) || (w1 == 0 && !t1) || (w2 == 0 && !t2))
                continue;
            w0 *= inv_area, w1 *= inv_area, w2 *= inv_area;
            u32 idx = (u32)y * EFB_W + (u32)x;
            f32 zf = a->z * w0 + b->z * w1 + c->z * w2;
            u32 z = (u32)clamp(to_int(zf), 0, 0xFFFFFF);
            bool early = S.early_z;
            if (early && !depth_test(idx, z))
                continue;
            f32 iw = a->iw * w0 + b->iw * w1 + c->iw * w2;
            f32 wv = iw != 0 ? 1.0f / iw : 0.0f;
            Pixel pix;
            for (int ch = 0; ch < 2; ch++)
                for (int i = 0; i < 4; i++) {
                    f32 v = (a->col[ch][i] * w0 + b->col[ch][i] * w1 + c->col[ch][i] * w2) * wv;
                    pix.ras[ch][i] = clamp(to_int(v + 0.5f), 0, 255);
                }
            for (u32 t = 0; t < S.num_tex && t < 8; t++) {
                f32 s = (a->tex[t][0] * w0 + b->tex[t][0] * w1 + c->tex[t][0] * w2) * wv;
                f32 tt = (a->tex[t][1] * w0 + b->tex[t][1] * w1 + c->tex[t][1] * w2) * wv;
                f32 q = (a->tex[t][2] * w0 + b->tex[t][2] * w1 + c->tex[t][2] * w2) * wv;
                if (q != 0 && q != 1.0f)
                    s /= q, tt /= q;
                pix.tc[t][0] = s, pix.tc[t][1] = tt;
            }
            s32 color[4];
            if (!run_tev(pix, color))
                continue;
            if (!early && !depth_test(idx, z))
                continue;
            blend_write(idx, color);
        }
    }
}

// cull, clip against the near and far planes, rasterize
void triangle(const GxOutVertex& v0, const GxOutVertex& v1, const GxOutVertex& v2) {
    u32 cull = bits(g_gx.bp[BP_GENMODE], 14, 2);
    if (cull) {
        const f32 *p0 = v0.clip, *p1 = v1.clip, *p2 = v2.clip;
        f32 nz = (p0[0] * p2[3] - p2[0] * p0[3]) * p1[1] + (p2[0] * p0[1] - p0[0] * p2[1]) * p1[3] +
                 (p2[1] * p0[3] - p0[1] * p2[3]) * p1[0];
        bool back = nz <= 0;
        if (xf_f32(XF_VIEWPORT + 1) > 0)
            back = !back;
        if (xf_f32(XF_VIEWPORT + 0) < 0)
            back = !back;
        if (!back && (cull == 1 || cull == 3))
            return;
        if (back && (cull == 2 || cull == 3))
            return;
    }
    // Sutherland-Hodgman in clip space: z >= -w (near), z <= 0 (far)
    GxOutVertex buf[2][9];
    int n = 3;
    buf[0][0] = v0, buf[0][1] = v1, buf[0][2] = v2;
    int cur = 0;
    for (int plane = 0; plane < 2; plane++) {
        auto dist = [&](const GxOutVertex& v) { return plane == 0 ? v.clip[2] + v.clip[3] : -v.clip[2]; };
        bool all_in = true;
        for (int i = 0; i < n; i++)
            if (dist(buf[cur][i]) < 0)
                all_in = false;
        if (all_in)
            continue;
        int m = 0;
        for (int i = 0; i < n; i++) {
            const GxOutVertex& p = buf[cur][i];
            const GxOutVertex& q = buf[cur][(i + 1) % n];
            f32 dp = dist(p), dq = dist(q);
            if (dp >= 0)
                buf[cur ^ 1][m++] = p;
            if ((dp >= 0) != (dq >= 0) && m < 9) {
                f32 t = dp / (dp - dq);
                GxOutVertex& o = buf[cur ^ 1][m++];
                const f32* pa = (const f32*)&p;
                const f32* qa = (const f32*)&q;
                f32* oa = (f32*)&o;
                for (size_t k = 0; k < sizeof(GxOutVertex) / sizeof(f32); k++)
                    oa[k] = pa[k] + (qa[k] - pa[k]) * t;
            }
        }
        cur ^= 1;
        n = m;
        if (n < 3)
            return;
    }
    RV rv[9];
    for (int i = 0; i < n; i++) {
        if (buf[cur][i].clip[3] <= 0)
            return;  // behind the eye even after clipping: degenerate
        to_screen(buf[cur][i], rv[i]);
    }
    for (int i = 1; i + 1 < n; i++)
        raster(rv[0], rv[i], rv[i + 1]);
}

// lines and points as thin quads (rare in this program)
void line(const GxOutVertex& a, const GxOutVertex& b) {
    if (a.clip[3] <= 0 || b.clip[3] <= 0)
        return;
    RV ra, rb;
    to_screen(a, ra);
    to_screen(b, rb);
    f32 width = (f32)bits(g_gx.bp[BP_LINEPTWIDTH], 0, 8) / 6.0f;
    f32 dx = rb.x - ra.x, dy = rb.y - ra.y;
    f32 len = std::sqrt(dx * dx + dy * dy);
    if (len == 0)
        return;
    f32 nx = -dy / len * width / 2, ny = dx / len * width / 2;
    RV q[4] = {ra, ra, rb, rb};
    q[0].x += nx, q[0].y += ny, q[1].x -= nx, q[1].y -= ny;
    q[2].x -= nx, q[2].y -= ny, q[3].x += nx, q[3].y += ny;
    raster(q[0], q[1], q[2]);
    raster(q[0], q[2], q[3]);
}

void point(const GxOutVertex& a) {
    if (a.clip[3] <= 0)
        return;
    RV r;
    to_screen(a, r);
    f32 h = (f32)bits(g_gx.bp[BP_LINEPTWIDTH], 8, 8) / 12.0f;
    RV q[4] = {r, r, r, r};
    q[0].x -= h, q[0].y -= h, q[1].x += h, q[1].y -= h, q[2].x += h, q[2].y += h, q[3].x -= h, q[3].y += h;
    raster(q[0], q[1], q[2]);
    raster(q[0], q[2], q[3]);
}

// ---------------------------------------------------------------- EFB copies
void efb_rgba(u32 x, u32 y, s32* out) {
    x = std::min(x, (u32)EFB_W - 1), y = std::min(y, (u32)EFB_H - 1);
    u32 c = s_color[y * EFB_W + x];
    out[0] = c & 255, out[1] = (c >> 8) & 255, out[2] = (c >> 16) & 255, out[3] = c >> 24;
}

void copy_to_xfb(u32 sx, u32 sy, u32 w, u32 h, u32 dst, u32 stride) {
    u32 ys = g_gx.bp[BP_DISP_YSCALE] & 0x1FF;
    bool invert = bits(g_gx.bp[BP_TRIGGER_COPY], 10, 1);
    f32 yscale = (invert && ys) ? 256.0f / (f32)ys : (ys ? (f32)ys / 256.0f : 1.0f);
    u32 lines = (u32)(1 + (h - 1) * yscale);
    if (lines > 1024)
        lines = 1024;
    for (u32 ly = 0; ly < lines; ly++) {
        u32 y = sy + std::min(h - 1, (u32)((f32)ly / yscale));
        u8* row = host_ptr(0x80000000 | ((dst + ly * stride) & 0x1FFFFFFF));
        if (!row)
            continue;
        for (u32 x = 0; x + 1 < w + 1; x += 2) {
            s32 p0[4], p1[4];
            efb_rgba(sx + x, y, p0);
            efb_rgba(sx + x + 1, y, p1);
            auto Y = [](const s32* p) { return (s32)(0.257f * p[0] + 0.504f * p[1] + 0.098f * p[2] + 16.5f); };
            f32 r = (p0[0] + p1[0]) / 2.0f, g = (p0[1] + p1[1]) / 2.0f, b = (p0[2] + p1[2]) / 2.0f;
            s32 u = (s32)(-0.148f * r - 0.291f * g + 0.439f * b + 128.5f);
            s32 v = (s32)(0.439f * r - 0.368f * g - 0.071f * b + 128.5f);
            u8* o = row + x * 2;
            if (x * 2 + 3 >= stride)
                break;
            o[0] = (u8)clamp(Y(p0), 0, 255);
            o[1] = (u8)clamp(u, 0, 255);
            o[2] = (u8)clamp(Y(p1), 0, 255);
            o[3] = (u8)clamp(v, 0, 255);
        }
    }
    s_last_xfb = {0x80000000 | dst, w, lines, stride};
    u32 a = 0x80000000 | dst;
    XfbCopy* r = nullptr;
    for (XfbCopy& x : s_regions)
        if (x.width && x.stride == stride && a >= x.addr && a <= x.addr + x.height * stride) {
            r = &x;
            break;
        }
    if (!r) {
        static u32 next;
        r = &s_regions[next++ % 8];
        *r = {a, w, 0, stride};
    }
    r->width = std::max(r->width, w);
    r->height = std::max(r->height, (a - r->addr) / stride + lines);
    if (a == r->addr)
        gx_texture_frame();
}

}  // namespace

EfbCopy gx_efb_copy_params(u32 v) {
    const u32* bp = g_gx.bp;
    EfbCopy c;
    c.sx = bits(bp[BP_EFB_TL], 0, 10), c.sy = bits(bp[BP_EFB_TL], 10, 10);
    c.w = bits(bp[BP_EFB_WH], 0, 10) + 1, c.h = bits(bp[BP_EFB_WH], 10, 10) + 1;
    c.dst = (bp[BP_EFB_ADDR] & 0xFFFFFF) << 5;
    c.stride = bits(bp[BP_EFB_STRIDE], 0, 10) << 5;
    u32 f = bits(v, 3, 4);
    c.fmt = f / 2 + (f & 1) * 8;
    c.to_xfb = bits(v, 14, 1), c.half = bits(v, 9, 1), c.intensity = bits(v, 15, 1);
    c.clear = bits(v, 11, 1);
    c.depth = bits(bp[BP_ZCONTROL], 0, 3) == 3;
    return c;
}

// write an EFB copy in the tiled layout of `fmt` (from the rectangle's pixels, or the averages
// of 2x2 pixels with `half`)
void gx_copy_to_texture(const EfbCopy& c, EfbFetch efb_rgba, u8* out) {
    u32 sx = c.sx, sy = c.sy, w = c.w, h = c.h, dst = c.dst, fmt = c.fmt;
    bool half = c.half, intensity = c.intensity;
    if (half)
        w = std::max<u32>(1, w / 2), h = std::max<u32>(1, h / 2);
    // block geometry of the destination format
    u32 bw, bh, bbytes;
    switch (fmt) {
    case 0: bw = 8, bh = 8, bbytes = 32; break;                       // R4/I4
    case 1: case 7: case 8: case 9: case 10: bw = 8, bh = 4, bbytes = 32; break;  // 8-bit
    case 2: bw = 8, bh = 4, bbytes = 32; break;                       // RA4/IA4
    case 6: bw = 4, bh = 4, bbytes = 64; break;                       // RGBA8
    default: bw = 4, bh = 4, bbytes = 32; break;                      // 16-bit formats
    }
    u32 row_bytes = c.stride;
    auto src_px = [&](u32 x, u32 y, s32* p) {
        if (!half) {
            efb_rgba(sx + x, sy + y, p);
            return;
        }
        s32 a[4], b[4], c[4], d[4];
        efb_rgba(sx + 2 * x, sy + 2 * y, a);
        efb_rgba(sx + 2 * x + 1, sy + 2 * y, b);
        efb_rgba(sx + 2 * x, sy + 2 * y + 1, c);
        efb_rgba(sx + 2 * x + 1, sy + 2 * y + 1, d);
        for (int i = 0; i < 4; i++)
            p[i] = (a[i] + b[i] + c[i] + d[i]) / 4;
    };
    auto lum = [](const s32* p) { return clamp((s32)(0.257f * p[0] + 0.504f * p[1] + 0.098f * p[2] + 16.5f), 0, 255); };
    u32 blocks_x = (w + bw - 1) / bw, blocks_y = (h + bh - 1) / bh;
    for (u32 by = 0; by < blocks_y; by++) {
        for (u32 bx = 0; bx < blocks_x; bx++) {
            u32 at = by * row_bytes + bx * bbytes;
            u8* o = out ? out + at : host_ptr(0x80000000 | ((dst + at) & 0x1FFFFFFF));
            if (!o)
                continue;
            memset(o, 0, bbytes);
            for (u32 i = 0; i < bw * bh; i++) {
                u32 x = bx * bw + i % bw, y = by * bh + i / bw;
                s32 p[4];
                src_px(std::min(x, w - 1), std::min(y, h - 1), p);
                s32 I = intensity ? lum(p) : p[0];
                switch (fmt) {
                case 0: o[i / 2] |= (u8)((I >> 4) << ((i & 1) ? 0 : 4)); break;
                case 1: case 8: o[i] = (u8)I; break;
                case 7: o[i] = (u8)p[3]; break;
                case 9: o[i] = (u8)p[1]; break;
                case 10: o[i] = (u8)p[2]; break;
                case 2: o[i] = (u8)((p[3] >> 4) << 4 | (I >> 4)); break;
                case 3: o[2 * i] = (u8)p[3], o[2 * i + 1] = (u8)I; break;
                case 4: {
                    u16 v = (u16)((p[0] >> 3) << 11 | (p[1] >> 2) << 5 | (p[2] >> 3));
                    o[2 * i] = (u8)(v >> 8), o[2 * i + 1] = (u8)v;
                    break;
                }
                case 5: {
                    u16 v;
                    if (p[3] >= 224)
                        v = (u16)(0x8000 | (p[0] >> 3) << 10 | (p[1] >> 3) << 5 | (p[2] >> 3));
                    else
                        v = (u16)((p[3] >> 5) << 12 | (p[0] >> 4) << 8 | (p[1] >> 4) << 4 | (p[2] >> 4));
                    o[2 * i] = (u8)(v >> 8), o[2 * i + 1] = (u8)v;
                    break;
                }
                case 6:
                    o[2 * i] = (u8)p[3], o[2 * i + 1] = (u8)p[0];
                    o[32 + 2 * i] = (u8)p[1], o[32 + 2 * i + 1] = (u8)p[2];
                    break;
                case 11: o[2 * i] = (u8)p[0], o[2 * i + 1] = (u8)p[1]; break;  // RG8
                case 12: o[2 * i] = (u8)p[1], o[2 * i + 1] = (u8)p[2]; break;  // GB8
                default: break;
                }
            }
        }
    }
    if (!out)
        gx_texture_invalidate(dst, blocks_y * row_bytes);
}

namespace {
const u32* s_lk_pixels;
const u32 *s_lk_col, *s_lk_row;
void lookup_fetch(u32 x, u32 y, s32* out) {
    u32 p = s_lk_pixels[s_lk_col[x & 1023] + s_lk_row[y & 1023]];
    out[0] = (s32)(p >> 24), out[1] = (s32)((p >> 16) & 255), out[2] = (s32)((p >> 8) & 255), out[3] = (s32)(p & 255);
}
}  // namespace

void gx_copy_to_texture_lookup(const EfbCopy& c, const u32* pixels, const u32* col, const u32* row, u8* out) {
    if (c.half || c.intensity || (c.fmt != 4 && c.fmt != 5 && c.fmt != 6) || !c.w || !c.h) {
        s_lk_pixels = pixels, s_lk_col = col, s_lk_row = row;
        gx_copy_to_texture(c, lookup_fetch, out);
        return;
    }
    // as gx_copy_to_texture: 4x4 blocks (RGBA8: 64 bytes, its AR halves then its GB halves),
    // texels past the rectangle repeat its last row and column
    const u32 w = c.w, h = c.h, fmt = c.fmt, bbytes = fmt == 6 ? 64 : 32;
    const u32 blocks_x = (w + 3) / 4, blocks_y = (h + 3) / 4;
    for (u32 by = 0; by < blocks_y; by++) {
        u32 rows[4];
        for (u32 j = 0; j < 4; j++)
            rows[j] = row[(c.sy + std::min(by * 4 + j, h - 1)) & 1023];
        for (u32 bx = 0; bx < blocks_x; bx++) {
            const u32 at = by * c.stride + bx * bbytes;
            u8* o = out ? out + at : host_ptr(0x80000000 | ((c.dst + at) & 0x1FFFFFFF));
            if (!o)
                continue;
            u32 cols[4];
            for (u32 i = 0; i < 4; i++)
                cols[i] = col[(c.sx + std::min(bx * 4 + i, w - 1)) & 1023];
            for (u32 i = 0; i < 16; i++) {
                const u32 p = pixels[cols[i & 3] + rows[i >> 2]];
                const u32 r = p >> 24, g = (p >> 16) & 255, b = (p >> 8) & 255, a = p & 255;
                if (fmt == 6) {
                    o[2 * i] = (u8)a, o[2 * i + 1] = (u8)r, o[32 + 2 * i] = (u8)g, o[32 + 2 * i + 1] = (u8)b;
                    continue;
                }
                u32 v;
                if (fmt == 4)
                    v = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3);
                else if (a >= 224)
                    v = 0x8000 | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3);
                else
                    v = (a >> 5) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4);
                o[2 * i] = (u8)(v >> 8), o[2 * i + 1] = (u8)v;
            }
        }
    }
    if (!out)
        gx_texture_invalidate(c.dst, blocks_y * c.stride);
}

int gx_efb_copy_test_frame() {
    static int frame = getenv("NOMA_EFB_COPY_TEST") ? atoi(getenv("NOMA_EFB_COPY_TEST")) : -1;
    return frame;
}

void gx_efb_copy_test(EfbFetch fetch, const char* prefix) {
    const u32 W = 640, H = 456;
    struct T {
        u32 fmt;
        bool half;
        const char* name;
    };
    static const T tests[] = {{6, false, "rgba8"}, {4, true, "rgb565"}};
    for (const T& t : tests) {
        EfbCopy c;
        c.w = W, c.h = H, c.fmt = t.fmt, c.half = t.half;
        u32 w = t.half ? W / 2 : W, h = t.half ? H / 2 : H;
        u32 bw = 4, bbytes = t.fmt == 6 ? 64 : 32;
        c.stride = (w + bw - 1) / bw * bbytes;
        std::vector<u8> tex(gx_texture_bytes(t.fmt, w, h) + c.stride);
        gx_copy_to_texture(c, fetch, tex.data());
        std::vector<u32> rgba((size_t)w * h);
        gx_decode_texture(tex.data(), t.fmt, w, h, rgba.data());
        char path[256];
        snprintf(path, sizeof(path), "%s_%s.bmp", prefix, t.name);
        FILE* f = fopen(path, "wb");
        if (!f)
            continue;
        u32 row = (w * 3 + 3) & ~3u, size = 54 + row * h;
        u8 hdr[54] = {'B', 'M'};
        auto put32 = [&](int at, u32 v) { memcpy(hdr + at, &v, 4); };
        put32(2, size), put32(10, 54), put32(14, 40), put32(18, w), put32(22, h), put32(34, row * h);
        hdr[26] = 1, hdr[28] = 24;
        fwrite(hdr, 1, 54, f);
        std::vector<u8> line(row, 0);
        for (u32 y = h; y-- > 0;) {  // BMP rows from the bottom
            for (u32 x = 0; x < w; x++) {
                u32 p = rgba[y * w + x];  // R in bits 0-7
                line[x * 3 + 0] = (u8)(p >> 16), line[x * 3 + 1] = (u8)(p >> 8), line[x * 3 + 2] = (u8)p;
            }
            fwrite(line.data(), 1, row, f);
        }
        fclose(f);
        log("gx: EFB copy test: %s", path);
    }
}

namespace {

void clear_rect(u32 sx, u32 sy, u32 w, u32 h) {
    const u32* bp = g_gx.bp;
    u32 ar = bp[BP_CLEAR_AR], gb = bp[BP_CLEAR_GB];
    u32 color = (ar & 255) | ((gb >> 8) & 255) << 8 | (gb & 255) << 16 | ((ar >> 8) & 255) << 24;
    u32 z = bp[BP_CLEAR_Z] & 0xFFFFFF;
    u32 cm = bp[BP_CMODE0];
    u32 pixfmt = bits(bp[BP_ZCONTROL], 0, 3);
    u32 keep = 0;
    if (!(cm & 0x08))
        keep |= 0x00FFFFFF;
    if (!(cm & 0x10) || pixfmt != 1)
        keep |= 0xFF000000;
    if (pixfmt != 1)
        color |= 0xFF000000;
    bool zupd = bp[BP_ZMODE] & 0x10;
    for (u32 y = sy; y < sy + h && y < (u32)EFB_H; y++)
        for (u32 x = sx; x < sx + w && x < (u32)EFB_W; x++) {
            u32 i = y * EFB_W + x;
            s_color[i] = (s_color[i] & keep) | (color & ~keep);
            if (zupd)
                s_depth[i] = z;
        }
}

}  // namespace

// ---------------------------------------------------------------- the renderer


// NOMA_DEBUG_DRAWS=N: describe every draw of the Nth frame (counted in XFB copies)
static u32 s_copies;
// NOMA_DEBUG_DRAWS=tN instead: the draws between texture copies N-1 and N (NOMA_DUMP_TEX_COPIES)
static u32 s_tex_copies;
// NOMA_DEBUG_DRAWS=xADDR (hex): the first 40 draws that sample the texture image at ADDR;
// x alone: that sample an EFB copy
static std::vector<u32> s_copy_dsts;
static bool debug_draws() {
    static const char* e = getenv("NOMA_DEBUG_DRAWS");
    static char mode = e && (e[0] == 't' || e[0] == 'x') ? e[0] : 0;
    static long n = e ? strtol(e + (mode != 0), nullptr, mode == 'x' ? 16 : 10) : -1;
    if (mode == 't')
        return n >= 1 && s_tex_copies + 1 == (u32)n;
    if (mode == 'x') {
        static u32 s_hits;
        if (s_hits >= 40)
            return false;
        for (u32 m = 0; m < 8; m++)
            if (u32 img = (g_gx.bp[bp_tex(BP_TX_SETIMAGE3, m)] & 0xFFFFFF) << 5;
                n > 0 ? img == ((u32)n & 0x1FFFFFFF)
                      : std::find(s_copy_dsts.begin(), s_copy_dsts.end(), img) != s_copy_dsts.end() ||
                            bits(g_gx.bp[bp_tex(BP_TX_SETIMAGE1, m)], 21, 1)) {
                s_hits++;
                return true;
            }
        return false;
    }
    return n >= 0 && s_copies / 3 == (u32)n;
}
// once per draw (debug_draws counts its hits); what the other debug output follows
static bool s_debug_now;
// NOMA_DEBUG_DRAWS=rX0,Y0,X1,Y1,F[,N]: the first N (80) draws after game frame F whose screen box
// (640x456 frame) has its centre inside the rectangle
static bool debug_draws_rect(const GxOutVertex* out, u32 count);
static bool debug_draws_now(const GxOutVertex* out, u32 count) {
    return s_debug_now = debug_draws() || debug_draws_rect(out, count);
}
bool gx_debug_frame() { return s_debug_now; }

static bool debug_draws_rect(const GxOutVertex* out, u32 count) {
    static const char* e = getenv("NOMA_DEBUG_DRAWS");
    static float r[6] = {0, 0, 0, 0, 0, 80};
    static bool on = e && e[0] == 'r' &&
                     sscanf(e + 1, "%f,%f,%f,%f,%f,%f", &r[0], &r[1], &r[2], &r[3], &r[4], &r[5]) >= 5;
    static u32 s_hits;
    if (!on || s_hits >= (u32)r[5] || vi_game_frames() < (u32)r[4])
        return false;
    f32 x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (u32 i = 0; i < count; i++) {
        RV v;
        to_screen(out[i], v);
        x0 = std::min(x0, v.x), x1 = std::max(x1, v.x), y0 = std::min(y0, v.y), y1 = std::max(y1, v.y);
    }
    f32 cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    if (cx < r[0] || cx > r[2] || cy < r[1] || cy > r[3])
        return false;
    s_hits++;
    return true;
}

void SoftRenderer::draw(u32 prim, u32 vat, u32 count, const u8* data) {
    static std::vector<GxVertex> in;
    static std::vector<GxOutVertex> out;
    in.resize(count);
    out.resize(count);
    gx_decode_vertices(vat, count, data, in.data());
    gx_transform(in.data(), count, out.data());
    gx_check_transform_draw(vat, count, data, out.data());
    load_state();
    if (debug_draws_now(out.data(), count)) {
        f32 x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (u32 i = 0; i < count; i++) {
            RV r;
            to_screen(out[i], r);
            x0 = std::min(x0, r.x), x1 = std::max(x1, r.x), y0 = std::min(y0, r.y), y1 = std::max(y1, r.y);
        }
        const u32* bp = g_gx.bp;
        log("draw %02X x%u: screen (%.0f,%.0f)-(%.0f,%.0f) w %.2f scissor (%d,%d)-(%d,%d) cull %u stages %u tex %u "
            "z %03X blend %04X tev %06X/%06X tex0 %ux%u fmt %u c0 %02X%02X%02X%02X mtx %u",
            prim, count, x0, y0, x1, y1, out[0].clip[3], S.sc_l, S.sc_t, S.sc_r, S.sc_b, bits(bp[BP_GENMODE], 14, 2),
            S.num_stages, S.num_tex, S.zmode, S.cmode0, bp[BP_TEV_COLOR_ENV], bp[BP_TEV_COLOR_ENV + 1],
            bits(bp[BP_TX_SETIMAGE0], 0, 10) + 1, bits(bp[BP_TX_SETIMAGE0], 10, 10) + 1,
            bits(bp[BP_TX_SETIMAGE0], 20, 4), in[0].color[0][0], in[0].color[0][1], in[0].color[0][2],
            in[0].color[0][3], in[0].pos_mtx);
        log("     tev regs prev (%d %d %d %d) c0 (%d %d %d %d) c1 (%d %d %d %d) c2 (%d %d %d %d) env1 %06X/%06X tref %06X",
            S.reg[0][0], S.reg[0][1], S.reg[0][2], S.reg[0][3], S.reg[1][0], S.reg[1][1], S.reg[1][2], S.reg[1][3],
            S.reg[2][0], S.reg[2][1], S.reg[2][2], S.reg[2][3], S.reg[3][0], S.reg[3][1], S.reg[3][2], S.reg[3][3],
            bp[BP_TEV_COLOR_ENV + 2], bp[BP_TEV_COLOR_ENV + 3], bp[BP_TREF]);
        if (S.tex[0] && !S.tex[0]->rgba.empty()) {
            const GxTexture* t = S.tex[0];
            u32 nonzero = 0;
            for (u32 px : t->rgba)
                nonzero += px != 0;
            u32 img3 = g_gx.bp[BP_TX_SETIMAGE3];
            const u8* raw = host_ptr(0x80000000 | ((img3 & 0xFFFFFF) << 5));
            log("     tex0 texels: %08X %08X %08X %08X (corner, mid, ...), %u of %zu not zero; memory %02X%02X%02X%02X%02X%02X%02X%02X",
                t->rgba[0], t->rgba[t->rgba.size() / 2], t->rgba[t->width / 2], t->rgba[t->rgba.size() - 1], nonzero,
                t->rgba.size(), raw ? raw[0] : 0, raw ? raw[1] : 0, raw ? raw[2] : 0, raw ? raw[3] : 0, raw ? raw[32] : 0,
                raw ? raw[33] : 0, raw ? raw[64] : 0, raw ? raw[65] : 0);
        }
        for (u32 i = 0; i < count && i < 4; i++)
            log("     vtx%u in tex (%.3f %.3f) out tex (%.3f %.3f %.3f) col %02X%02X%02X%02X", i, in[i].tex[0][0],
                in[i].tex[0][1], out[i].tex[0][0], out[i].tex[0][1], out[i].tex[0][2], in[i].color[0][0],
                in[i].color[0][1], in[i].color[0][2], in[i].color[0][3]);
        log("     texgen0 %08X post %08X tx mode0 %06X mode1 %06X img0 %06X img3 %06X suf %06X/%06X",
            g_gx.xf[0x1040], g_gx.xf[0x1050], bp[0x80], bp[0x84], bp[0x88], bp[0x94], bp[0x30], bp[0x31]);
        {
            u32 tm = in[0].tex_mtx[0] * 4u;
            log("     texmtx idx %u: %08X %08X %08X %08X | %08X %08X %08X %08X (%g %g %g %g | %g %g %g %g)", tm,
                g_gx.xf[tm], g_gx.xf[tm + 1], g_gx.xf[tm + 2], g_gx.xf[tm + 3], g_gx.xf[tm + 4], g_gx.xf[tm + 5],
                g_gx.xf[tm + 6], g_gx.xf[tm + 7], xf_f32(tm), xf_f32(tm + 1), xf_f32(tm + 2), xf_f32(tm + 3),
                xf_f32(tm + 4), xf_f32(tm + 5), xf_f32(tm + 6), xf_f32(tm + 7));
        }
        u32 m = in[0].pos_mtx * 4u;
        log("     v0 (%.1f %.1f %.1f) v2 (%.1f %.1f %.1f) mtx [%.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f]"
            " proj %s %.4f %.4f %.4f %.4f %.4f %.4f vp %.1f %.1f %.1f %.1f",
            in[0].pos[0], in[0].pos[1], in[0].pos[2], in[count > 2 ? 2 : 0].pos[0], in[count > 2 ? 2 : 0].pos[1],
            in[count > 2 ? 2 : 0].pos[2], xf_f32(m), xf_f32(m + 1), xf_f32(m + 2), xf_f32(m + 3), xf_f32(m + 4),
            xf_f32(m + 5), xf_f32(m + 6), xf_f32(m + 7), xf_f32(m + 8), xf_f32(m + 9), xf_f32(m + 10), xf_f32(m + 11),
            g_gx.xf[XF_PROJECTION + 6] ? "ortho" : "persp", xf_f32(XF_PROJECTION), xf_f32(XF_PROJECTION + 1),
            xf_f32(XF_PROJECTION + 2), xf_f32(XF_PROJECTION + 3), xf_f32(XF_PROJECTION + 4), xf_f32(XF_PROJECTION + 5),
            xf_f32(XF_VIEWPORT), xf_f32(XF_VIEWPORT + 1), xf_f32(XF_VIEWPORT + 3), xf_f32(XF_VIEWPORT + 4));
    }
    switch (prim) {
    case 0x80:  // quads
    case 0x88:
        for (u32 i = 0; i + 3 < count; i += 4) {
            triangle(out[i], out[i + 1], out[i + 2]);
            triangle(out[i], out[i + 2], out[i + 3]);
        }
        break;
    case 0x90:  // triangles
        for (u32 i = 0; i + 2 < count; i += 3)
            triangle(out[i], out[i + 1], out[i + 2]);
        break;
    case 0x98:  // strip
        for (u32 i = 0; i + 2 < count; i++) {
            if (i & 1)
                triangle(out[i + 1], out[i], out[i + 2]);
            else
                triangle(out[i], out[i + 1], out[i + 2]);
        }
        break;
    case 0xA0:  // fan
        for (u32 i = 1; i + 1 < count; i++)
            triangle(out[0], out[i], out[i + 1]);
        break;
    case 0xA8:  // lines
        for (u32 i = 0; i + 1 < count; i += 2)
            line(out[i], out[i + 1]);
        break;
    case 0xB0:  // line strip
        for (u32 i = 0; i + 1 < count; i++)
            line(out[i], out[i + 1]);
        break;
    case 0xB8:  // points
        for (u32 i = 0; i < count; i++)
            point(out[i]);
        break;
    }
}

void SoftRenderer::native(u32 id) { engine::g3d::native_draw_execute(id); }

// The bottom screen's buffers (see State::map), swapped in while it is drawn on.
namespace {
std::vector<u32> s_bottom_color(EFB_W * EFB_H, 0xFFFFFFFFu), s_bottom_depth(EFB_W * EFB_H, 0xFFFFFF);
bool s_bottom_drawn, s_bottom_shown;
}  // namespace

void SoftRenderer::screen(u32 screen, u32 x, u32 y, u32 w, u32 h) {
    const bool bottom = screen == SCREEN_BOTTOM;
    if (bottom && !s_bottom_drawn) {  // (the frame's first draw there: a white screen, depth far)
        std::fill(s_bottom_color.begin(), s_bottom_color.end(), 0xFFFFFFFFu);
        std::fill(s_bottom_depth.begin(), s_bottom_depth.end(), 0xFFFFFFu);
        s_bottom_drawn = true;
    }
    if (bottom) {
        s_bottom_rect[0] = x, s_bottom_rect[1] = y, s_bottom_rect[2] = std::max((u32)1, w), s_bottom_rect[3] = std::max((u32)1, h);
    }
    if (bottom != s_on_bottom) {
        std::swap(s_color, s_bottom_color);
        std::swap(s_depth, s_bottom_depth);
        s_on_bottom = bottom;
    }
}

void SoftRenderer::draw_done() {
    screen(SCREEN_TOP, 0, 0, 0, 0);
    s_bottom_shown = s_bottom_drawn;
    s_bottom_drawn = false;
}

bool gx_host_bottom_screen(u32* out) {
    if (!s_bottom_shown)
        return false;
    const std::vector<u32>& c = s_on_bottom ? s_color : s_bottom_color;
    const u32 rx = 0, ry = 0, rw = EFB_W, rh = EFB_H;  // (every draw mapped onto the whole buffer)
    for (u32 y = 0; y < 240; y++) {
        const u32 y0 = ry + y * rh / 240, y1 = std::max(y0 + 1, ry + (y + 1) * rh / 240);
        for (u32 x = 0; x < 320; x++) {
            const u32 x0 = rx + x * rw / 320, x1 = std::max(x0 + 1, rx + (x + 1) * rw / 320);
            u32 sum[4] = {0, 0, 0, 0}, n = 0;
            for (u32 sy = y0; sy < y1 && sy < (u32)EFB_H; sy++)
                for (u32 sx = x0; sx < x1 && sx < (u32)EFB_W; sx++, n++)
                    for (int k = 0; k < 4; k++)
                        sum[k] += c[sy * EFB_W + sx] >> (8 * k) & 0xFF;
            u32 p = 0;
            for (int k = 0; k < 3; k++)
                p |= (n ? sum[k] / n : 0) << (8 * k);
            out[y * 320 + x] = p | 0xFF000000u;
        }
    }
    return true;
}

HostEfb gx_host_efb() {
    const u32* bp = g_gx.bp;
    HostEfb e;
    e.color = s_color.data(), e.depth = s_depth.data();
    e.width = EFB_W, e.height = EFB_H;
    // (as load_state)
    s32 sox = (s32)bits(bp[BP_SCISSOR_OFFSET], 0, 10) * 2, soy = (s32)bits(bp[BP_SCISSOR_OFFSET], 10, 10) * 2;
    e.scissor_left = std::max<s32>((s32)bits(bp[BP_SCISSOR_TL], 12, 11) - sox, 0);
    e.scissor_top = std::max<s32>((s32)bits(bp[BP_SCISSOR_TL], 0, 11) - soy, 0);
    e.scissor_right = std::min<s32>((s32)bits(bp[BP_SCISSOR_BR], 12, 11) - sox, EFB_W - 1);
    e.scissor_bottom = std::min<s32>((s32)bits(bp[BP_SCISSOR_BR], 0, 11) - soy, EFB_H - 1);
    e.x_offset = (f32)sox, e.y_offset = (f32)soy;
    e.pixel_format = bits(bp[BP_ZCONTROL], 0, 3);
    return e;
}

// NOMA_DUMP_TEX_COPIES=N: log the first N EFB copies to textures and save each as it lands in
// memory, decoded (color left, alpha right), to tex_copies/NNNN.bmp
static void dump_texture_copy(const EfbCopy& c, u32 v) {
    static int s_left = getenv("NOMA_DUMP_TEX_COPIES") ? atoi(getenv("NOMA_DUMP_TEX_COPIES")) : 0;
    static u32 s_n;
    if (s_left <= 0)
        return;
    s_left--;
    u32 w = c.half ? std::max<u32>(1, c.w / 2) : c.w, h = c.half ? std::max<u32>(1, c.h / 2) : c.h;
    const u32* bp = g_gx.bp;
    log("gx: texture copy %u: %06X (%u,%u) %ux%u%s fmt %u%s -> %08X stride %u; pixel format %u, cmode0 %04X "
        "cmode1 %04X, clear %02X%02X%02X%02X",
        s_n, v, c.sx, c.sy, c.w, c.h, c.half ? " halved" : "", c.fmt, c.intensity ? " (intensity)" : "", c.dst,
        c.stride, bits(bp[BP_ZCONTROL], 0, 3), bp[BP_CMODE0], bp[BP_CMODE1], bp[BP_CLEAR_AR] & 255,
        bp[BP_CLEAR_GB] >> 8, bp[BP_CLEAR_GB] & 255, bp[BP_CLEAR_AR] >> 8);
    const u8* src = host_ptr(0x80000000 | (c.dst & 0x1FFFFFFF));
    if (!src)
        return;
    std::vector<u32> rgba((size_t)w * h);
    gx_decode_texture(src, c.fmt, w, h, rgba.data());
    mkdir("tex_copies", 0777);
    char path[64];
    snprintf(path, sizeof(path), "tex_copies/%04u.bmp", s_n++);
    FILE* f = fopen(path, "wb");
    if (!f)
        return;
    u32 W = 2 * w, row = (W * 3 + 3) & ~3u, size = 54 + row * h;
    u8 hdr[54] = {'B', 'M'};
    auto put32 = [&](int at, u32 x) { memcpy(hdr + at, &x, 4); };
    put32(2, size), put32(10, 54), put32(14, 40), put32(18, W), put32(22, h), put32(34, row * h);
    hdr[26] = 1, hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    std::vector<u8> line(row, 0);
    for (u32 y = h; y-- > 0;) {
        for (u32 x = 0; x < w; x++) {
            u32 p = rgba[y * w + x];  // R in bits 0-7
            line[x * 3 + 0] = (u8)(p >> 16), line[x * 3 + 1] = (u8)(p >> 8), line[x * 3 + 2] = (u8)p;
            u8 a = (u8)(p >> 24);
            line[(w + x) * 3 + 0] = line[(w + x) * 3 + 1] = line[(w + x) * 3 + 2] = a;
        }
        fwrite(line.data(), 1, row, f);
    }
    fclose(f);
}

void SoftRenderer::efb_copy(u32 v) {
    if (s_debug_now)
        log("efb copy %06X (clear color %04X%04X, z %06X)", v, g_gx.bp[BP_CLEAR_AR], g_gx.bp[BP_CLEAR_GB],
            g_gx.bp[BP_CLEAR_Z]);
    s_copies++;
    const u32* bp = g_gx.bp;
    u32 sx = bits(bp[BP_EFB_TL], 0, 10), sy = bits(bp[BP_EFB_TL], 10, 10);
    u32 w = bits(bp[BP_EFB_WH], 0, 10) + 1, h = bits(bp[BP_EFB_WH], 10, 10) + 1;
    u32 dst = (bp[BP_EFB_ADDR] & 0xFFFFFF) << 5;
    u32 stride = bits(bp[BP_EFB_STRIDE], 0, 10) << 5;
    static int s_logged;
    if (s_logged < 12 && getenv("NOMA_DEBUG_GX")) {
        s_logged++;
        log("gx: EFB copy %08X: (%u,%u) %ux%u -> %08X stride %u, yscale %03X", v, sx, sy, w, h, dst, stride,
            bp[BP_DISP_YSCALE]);
    }
    if (bits(v, 14, 1)) {
        copy_to_xfb(sx, sy, w, h, dst, stride);
    } else {
        EfbCopy c = gx_efb_copy_params(v);
        if (c.depth) {
            if (!s_warned_zcopy) {
                s_warned_zcopy = true;
                log("gx: depth copy to a texture -- not emulated");
            }
        } else {
            gx_copy_to_texture(c, efb_rgba);
            dump_texture_copy(c, v);
            s_tex_copies++;
            if (std::find(s_copy_dsts.begin(), s_copy_dsts.end(), c.dst & 0x1FFFFFFF) == s_copy_dsts.end())
                s_copy_dsts.push_back(c.dst & 0x1FFFFFFF);
        }
    }
    if (bits(v, 11, 1))
        clear_rect(sx, sy, w, h);
}

const XfbCopy& gx_last_xfb_copy() { return s_last_xfb; }

const XfbCopy* gx_xfb_region(u32 addr) {
    for (const XfbCopy& x : s_regions)
        if (x.width && x.addr == addr)
            return &x;
    return nullptr;
}

}  // namespace guest
