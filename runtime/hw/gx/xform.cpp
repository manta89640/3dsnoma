// The transform unit: positions and normals to eye space (matrix memory), the projection,
// the two color channels (material, ambient, up to eight lights each) and texture coordinate
// generation (a texture matrix, then optionally a normalised post-transform matrix).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "vertex.h"
#include "vertex_read.h"

namespace guest {

namespace {

void mul34(u32 addr, const f32* v, f32* out) {  // 3x4 matrix at XF addr times (v, 1)
    for (int r = 0; r < 3; r++) {
        u32 m = addr + 4 * r;
        out[r] = xf_f32(m) * v[0] + xf_f32(m + 1) * v[1] + xf_f32(m + 2) * v[2] + xf_f32(m + 3) * v[3];
    }
}

void mul33(u32 addr, const f32* v, f32* out) {
    for (int r = 0; r < 3; r++) {
        u32 m = addr + 3 * r;
        out[r] = xf_f32(m) * v[0] + xf_f32(m + 1) * v[1] + xf_f32(m + 2) * v[2];
    }
}

f32 dot3(const f32* a, const f32* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void normalize3(f32* v) {
    f32 l = std::sqrt(dot3(v, v));
    if (l > 0) {
        v[0] /= l, v[1] /= l, v[2] /= l;
    }
}

void unpack_rgba(u32 c, f32* out) {
    out[0] = (f32)(c >> 24), out[1] = (f32)((c >> 16) & 255), out[2] = (f32)((c >> 8) & 255),
    out[3] = (f32)(c & 255);
}

f32 clamp255(f32 v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// one color channel's lighting: the sum of the enabled lights' contributions (0-255 scale)
void light_channel(u32 ctrl, const f32* pos, const f32* nrm, bool alpha, f32* acc) {
    u32 mask = bits(ctrl, 2, 4) | (bits(ctrl, 11, 4) << 4);
    u32 diffuse = bits(ctrl, 7, 2), attn = bits(ctrl, 9, 2);
    for (int l = 0; l < 8; l++) {
        if (!(mask & (1u << l)))
            continue;
        u32 base = XF_MEM_LIGHT + 0x10 * l;
        f32 color[4];
        unpack_rgba(g_gx.xf[base + 3], color);
        f32 cosatt[3] = {xf_f32(base + 4), xf_f32(base + 5), xf_f32(base + 6)};
        f32 distatt[3] = {xf_f32(base + 7), xf_f32(base + 8), xf_f32(base + 9)};
        f32 lpos[3] = {xf_f32(base + 10), xf_f32(base + 11), xf_f32(base + 12)};
        f32 ldir[3] = {xf_f32(base + 13), xf_f32(base + 14), xf_f32(base + 15)};

        f32 dir[3] = {lpos[0] - pos[0], lpos[1] - pos[1], lpos[2] - pos[2]};
        f32 a = 1.0f;
        if (attn == 1) {  // specular
            normalize3(dir);
            f32 ndl = dot3(nrm, dir);
            f32 t = ndl >= 0 ? std::max(0.0f, dot3(nrm, ldir)) : 0.0f;
            f32 da[3] = {distatt[0], distatt[1], distatt[2]};
            if (diffuse == 0)
                ;
            else
                normalize3(da);
            f32 num = cosatt[0] + cosatt[1] * t + cosatt[2] * t * t;
            f32 den = da[0] + da[1] * t + da[2] * t * t;
            a = std::max(0.0f, num) / (den != 0 ? den : 1e-6f);
        } else if (attn == 3) {  // spot
            f32 d2 = dot3(dir, dir), d = std::sqrt(d2);
            if (d > 0)
                dir[0] /= d, dir[1] /= d, dir[2] /= d;
            f32 t = std::max(0.0f, dot3(dir, ldir));
            f32 num = cosatt[0] + cosatt[1] * t + cosatt[2] * t * t;
            f32 den = distatt[0] + distatt[1] * d + distatt[2] * d2;
            a = std::max(0.0f, num) / (den != 0 ? den : 1e-6f);
        } else {
            normalize3(dir);
        }
        f32 d = 1.0f;
        if (diffuse == 1)
            d = dot3(dir, nrm);
        else if (diffuse == 2)
            d = std::max(0.0f, dot3(dir, nrm));
        if (alpha)
            acc[3] += a * d * color[3];
        else
            for (int i = 0; i < 3; i++)
                acc[i] += a * d * color[i];
    }
}

}  // namespace

void gx_transform(const GxVertex* in, u32 count, GxOutVertex* out) {
    // projection
    f32 p[6];
    for (int i = 0; i < 6; i++)
        p[i] = xf_f32(XF_PROJECTION + i);
    bool ortho = g_gx.xf[XF_PROJECTION + 6] != 0;
    u32 num_colors = g_gx.xf[XF_NUMCOLORS];
    u32 num_texgens = g_gx.xf[XF_NUMTEXGENS];
    bool dualtex = g_gx.xf[XF_DUALTEX] & 1;

    for (u32 v = 0; v < count; v++) {
        const GxVertex& iv = in[v];
        GxOutVertex& o = out[v];
        f32 mpos[4] = {iv.pos[0], iv.pos[1], iv.pos[2], 1.0f};
        f32 epos[3], enrm[3][3];
        mul34(iv.pos_mtx * 4u, mpos, epos);
        u32 nmtx = XF_MEM_NORMAL + (iv.pos_mtx & 31u) * 3;
        for (int k = 0; k < 3; k++) {
            mul33(nmtx, iv.nrm[k], enrm[k]);
            if (k == 0)
                normalize3(enrm[k]);
        }

        if (ortho) {
            o.clip[0] = p[0] * epos[0] + p[1];
            o.clip[1] = p[2] * epos[1] + p[3];
            o.clip[2] = p[4] * epos[2] + p[5];
            o.clip[3] = 1.0f;
        } else {
            o.clip[0] = p[0] * epos[0] + p[1] * epos[2];
            o.clip[1] = p[2] * epos[1] + p[3] * epos[2];
            o.clip[2] = p[4] * epos[2] + p[5];
            o.clip[3] = -epos[2];
        }

        // color channels
        for (u32 ch = 0; ch < 2; ch++) {
            f32 vc[4];
            for (int i = 0; i < 4; i++)
                vc[i] = iv.color[ch][i];
            u32 cctrl = g_gx.xf[XF_COLOR0CNTRL + ch], actrl = g_gx.xf[XF_ALPHA0CNTRL + ch];
            f32 mat[4], amb[4], res[4];
            unpack_rgba(g_gx.xf[XF_MATERIAL0 + ch], mat);
            unpack_rgba(g_gx.xf[XF_AMBIENT0 + ch], amb);
            // rgb
            {
                const f32* m = (cctrl & 1) ? vc : mat;
                if (cctrl & 2) {
                    const f32* am = (cctrl & 0x40) ? vc : amb;
                    f32 acc[4] = {am[0], am[1], am[2], 0};
                    light_channel(cctrl, epos, enrm[0], false, acc);
                    for (int i = 0; i < 3; i++)
                        res[i] = m[i] * clamp255(acc[i]) / 255.0f;
                } else {
                    for (int i = 0; i < 3; i++)
                        res[i] = m[i];
                }
            }
            // alpha
            {
                const f32* m = (actrl & 1) ? vc : mat;
                if (actrl & 2) {
                    const f32* am = (actrl & 0x40) ? vc : amb;
                    f32 acc[4] = {0, 0, 0, am[3]};
                    light_channel(actrl, epos, enrm[0], true, acc);
                    res[3] = m[3] * clamp255(acc[3]) / 255.0f;
                } else {
                    res[3] = m[3];
                }
            }
            for (int i = 0; i < 4; i++)
                o.color[ch][i] = ch < num_colors ? clamp255(res[i]) : 0.0f;
        }

        // texture coordinates
        for (u32 t = 0; t < 8; t++) {
            f32* tc = o.tex[t];
            if (t >= num_texgens) {
                tc[0] = tc[1] = 0, tc[2] = 1;
                continue;
            }
            u32 info = g_gx.xf[XF_TEXMTXINFO + t];
            bool stq = bits(info, 1, 1);
            bool abc1 = bits(info, 2, 1);
            u32 type = bits(info, 4, 3), row = bits(info, 7, 5);
            f32 src[4] = {0, 0, 1, 1};
            switch (row) {
            case 0: src[0] = mpos[0], src[1] = mpos[1], src[2] = mpos[2]; break;
            case 1: src[0] = iv.nrm[0][0], src[1] = iv.nrm[0][1], src[2] = iv.nrm[0][2]; break;
            case 2: src[0] = iv.color[0][0] / 255.0f, src[1] = iv.color[0][1] / 255.0f, src[2] = iv.color[0][2] / 255.0f; break;
            case 3: src[0] = iv.nrm[2][0], src[1] = iv.nrm[2][1], src[2] = iv.nrm[2][2]; break;
            case 4: src[0] = iv.nrm[1][0], src[1] = iv.nrm[1][1], src[2] = iv.nrm[1][2]; break;
            default:
                if (row >= 5 && row <= 12)
                    src[0] = iv.tex[row - 5][0], src[1] = iv.tex[row - 5][1];
                break;
            }
            if (!abc1)
                src[2] = 1.0f;
            if (type == 2 || type == 3) {  // texture coordinates from a lit color channel
                const f32* c = o.color[type - 2];
                tc[0] = c[0] / 255.0f, tc[1] = c[1] / 255.0f, tc[2] = 1.0f;
            } else if (type == 1) {  // emboss: the source coordinates (bump offset not emulated)
                u32 srcrow = bits(info, 12, 3);
                tc[0] = out[v].tex[srcrow][0], tc[1] = out[v].tex[srcrow][1], tc[2] = 1.0f;
            } else {
                u32 m = iv.tex_mtx[t] * 4u;
                f32 r[3];
                for (int i = 0; i < 2; i++) {
                    u32 a = m + 4 * i;
                    r[i] = xf_f32(a) * src[0] + xf_f32(a + 1) * src[1] + xf_f32(a + 2) * src[2] + xf_f32(a + 3) * src[3];
                }
                if (stq) {
                    u32 a = m + 8;
                    r[2] = xf_f32(a) * src[0] + xf_f32(a + 1) * src[1] + xf_f32(a + 2) * src[2] + xf_f32(a + 3) * src[3];
                } else {
                    r[2] = 1.0f;
                }
                if (dualtex) {
                    u32 post = g_gx.xf[XF_POSTMTXINFO + t];
                    if (bits(post, 8, 1))
                        normalize3(r);
                    f32 in4[4] = {r[0], r[1], r[2], 1.0f};
                    mul34(XF_MEM_POST + bits(post, 0, 6) * 4u, in4, r);
                }
                tc[0] = r[0], tc[1] = r[1], tc[2] = r[2];
            }
        }
    }
}

}  // namespace guest

// ---------------------------------------------------------------- the draw path
// gx_transform's work for the outputs a draw uses, with everything that is the same for all its
// vertices worked out once: matrices (reloaded only when a vertex's index changes), lights,
// texgen descriptions.
namespace guest {

namespace {

struct Mtx34 {
    f32 m[12];
};
struct Mtx33 {
    f32 m[9];
};

inline void load_mtx34(u32 addr, Mtx34& out) {
    memcpy(out.m, &g_gx.xf[addr], sizeof(out.m));
}
inline void load_mtx33(u32 addr, Mtx33& out) {
    memcpy(out.m, &g_gx.xf[addr], sizeof(out.m));
}

// a light as one channel uses it, with its parameters as floats
struct LightParams {
    f32 color[4];
    f32 cosatt[3], distatt[3];  // distatt normalised for specular lights with diffuse on
    f32 pos[3], dir[3];
};

// one half (rgb or alpha) of a color channel
struct ChannelPlan {
    bool lit = false;
    bool mat_vtx = false, amb_vtx = false;  // material / ambient from the vertex color
    u32 diffuse = 0, attn = 0;
    u32 nlights = 0;
    LightParams lights[8];
};

void plan_channel(u32 ctrl, ChannelPlan& p) {
    p.lit = ctrl & 2;
    p.mat_vtx = ctrl & 1;
    p.amb_vtx = ctrl & 0x40;
    p.diffuse = bits(ctrl, 7, 2), p.attn = bits(ctrl, 9, 2);
    p.nlights = 0;
    if (!p.lit)
        return;
    u32 mask = bits(ctrl, 2, 4) | (bits(ctrl, 11, 4) << 4);
    for (int l = 0; l < 8; l++) {
        if (!(mask & (1u << l)))
            continue;
        u32 base = XF_MEM_LIGHT + 0x10 * l;
        LightParams& L = p.lights[p.nlights++];
        unpack_rgba(g_gx.xf[base + 3], L.color);
        for (int i = 0; i < 3; i++) {
            L.cosatt[i] = xf_f32(base + 4 + i);
            L.distatt[i] = xf_f32(base + 7 + i);
            L.pos[i] = xf_f32(base + 10 + i);
            L.dir[i] = xf_f32(base + 13 + i);
        }
        if (p.attn == 1 && p.diffuse != 0)
            normalize3(L.distatt);
    }
}

// light_channel's sum over the planned lights; component 0-2 (rgb) or 3 (alpha)
template <bool ALPHA>
void light_sum(const ChannelPlan& p, const f32* pos, const f32* nrm, f32* acc) {
    for (u32 l = 0; l < p.nlights; l++) {
        const LightParams& L = p.lights[l];
        f32 dir[3] = {L.pos[0] - pos[0], L.pos[1] - pos[1], L.pos[2] - pos[2]};
        f32 a = 1.0f;
        if (p.attn == 1) {  // specular
            normalize3(dir);
            f32 ndl = dot3(nrm, dir);
            f32 t = ndl >= 0 ? std::max(0.0f, dot3(nrm, L.dir)) : 0.0f;
            f32 num = L.cosatt[0] + L.cosatt[1] * t + L.cosatt[2] * t * t;
            f32 den = L.distatt[0] + L.distatt[1] * t + L.distatt[2] * t * t;
            a = std::max(0.0f, num) / (den != 0 ? den : 1e-6f);
        } else if (p.attn == 3) {  // spot
            f32 d2 = dot3(dir, dir), d = std::sqrt(d2);
            if (d > 0)
                dir[0] /= d, dir[1] /= d, dir[2] /= d;
            f32 t = std::max(0.0f, dot3(dir, L.dir));
            f32 num = L.cosatt[0] + L.cosatt[1] * t + L.cosatt[2] * t * t;
            f32 den = L.distatt[0] + L.distatt[1] * d + L.distatt[2] * d2;
            a = std::max(0.0f, num) / (den != 0 ? den : 1e-6f);
        } else {
            normalize3(dir);
        }
        f32 d = 1.0f;
        if (p.diffuse == 1)
            d = dot3(dir, nrm);
        else if (p.diffuse == 2)
            d = std::max(0.0f, dot3(dir, nrm));
        if (ALPHA)
            acc[3] += a * d * L.color[3];
        else
            for (int i = 0; i < 3; i++)
                acc[i] += a * d * L.color[i];
    }
}

// a color channel: material, ambient and lights
struct ColorPlan {
    bool on = false;  // channel < XF_NUMCOLORS (else its output is zero)
    f32 mat[4], amb[4];
    ChannelPlan rgb, alpha;
};

void plan_color(u32 ch, ColorPlan& c) {
    c.on = ch < g_gx.xf[XF_NUMCOLORS];
    unpack_rgba(g_gx.xf[XF_MATERIAL0 + ch], c.mat);
    unpack_rgba(g_gx.xf[XF_AMBIENT0 + ch], c.amb);
    plan_channel(g_gx.xf[XF_COLOR0CNTRL + ch], c.rgb);
    plan_channel(g_gx.xf[XF_ALPHA0CNTRL + ch], c.alpha);
}

// gx_transform's color channel result (0-255 floats)
void color_out(const ColorPlan& c, const f32* vc, const f32* epos, const f32* enrm, f32* res) {
    if (!c.on) {
        res[0] = res[1] = res[2] = res[3] = 0;
        return;
    }
    const f32* m = c.rgb.mat_vtx ? vc : c.mat;
    if (c.rgb.lit) {
        const f32* am = c.rgb.amb_vtx ? vc : c.amb;
        f32 acc[4] = {am[0], am[1], am[2], 0};
        light_sum<false>(c.rgb, epos, enrm, acc);
        for (int i = 0; i < 3; i++)
            res[i] = m[i] * clamp255(acc[i]) / 255.0f;
    } else {
        res[0] = m[0], res[1] = m[1], res[2] = m[2];
    }
    m = c.alpha.mat_vtx ? vc : c.mat;
    if (c.alpha.lit) {
        const f32* am = c.alpha.amb_vtx ? vc : c.amb;
        f32 acc[4] = {0, 0, 0, am[3]};
        light_sum<true>(c.alpha, epos, enrm, acc);
        res[3] = m[3] * clamp255(acc[3]) / 255.0f;
    } else {
        res[3] = m[3];
    }
    for (int i = 0; i < 4; i++)
        res[i] = clamp255(res[i]);
}

// one texgen: its source and matrices
struct TexgenPlan {
    u32 type = 0;    // 0/other regular, 1 emboss, 2-3 from color channel 0/1
    u32 row = 0;     // source row
    bool stq = false, abc1 = false;
    u32 emboss_src = 0;
    bool post = false, post_norm = false;
    Mtx34 post_mtx;
    s8 mtx_off = -1;  // stream offset of its matrix index, -1: the default
    u32 cur_index = ~0u;
    Mtx34 mtx;       // rows 0-1 (and 2 when stq)
};

void plan_texgen(u32 t, const GxVertexFormat& f, u32 def_index, bool dualtex, TexgenPlan& g) {
    u32 info = g_gx.xf[XF_TEXMTXINFO + t];
    g.stq = bits(info, 1, 1), g.abc1 = bits(info, 2, 1);
    g.type = bits(info, 4, 3), g.row = bits(info, 7, 5);
    g.emboss_src = bits(info, 12, 3);
    g.post = false;
    if (dualtex) {
        u32 post = g_gx.xf[XF_POSTMTXINFO + t];
        g.post = true;
        g.post_norm = bits(post, 8, 1);
        load_mtx34(XF_MEM_POST + bits(post, 0, 6) * 4u, g.post_mtx);
        // the identity (the usual post-transform matrix) changes nothing but the sign of a zero
        static const f32 k_identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        if (!g.post_norm && memcmp(g.post_mtx.m, k_identity, sizeof(k_identity)) == 0)
            g.post = false;
    }
    g.mtx_off = f.tex_mtx[t];
    g.cur_index = def_index;
    load_mtx34(def_index * 4u, g.mtx);
}

// the decoded attributes of one vertex that texgens may read
struct VtxIn {
    f32 pos[3];
    f32 nrm[3][3];
    f32 color[2][4];
    f32 tex[8][2];
};

// a regular texgen's s, t, q from its source row (x, y, z, 1)
inline void texgen_apply(const TexgenPlan& g, f32* src, f32* out) {
    if (!g.abc1)
        src[2] = 1.0f;
    const f32* m = g.mtx.m;
    f32 r[3];
    r[0] = m[0] * src[0] + m[1] * src[1] + m[2] * src[2] + m[3] * src[3];
    r[1] = m[4] * src[0] + m[5] * src[1] + m[6] * src[2] + m[7] * src[3];
    r[2] = g.stq ? m[8] * src[0] + m[9] * src[1] + m[10] * src[2] + m[11] * src[3] : 1.0f;
    if (g.post) {
        if (g.post_norm)
            normalize3(r);
        const f32* p = g.post_mtx.m;
        for (int i = 0; i < 3; i++)
            out[i] = p[4 * i] * r[0] + p[4 * i + 1] * r[1] + p[4 * i + 2] * r[2] + p[4 * i + 3];
    } else {
        out[0] = r[0], out[1] = r[1], out[2] = r[2];
    }
}

// s and t with q divided out (q = 0 counts as 1; a division by 1 is skipped: it is exact), scaled
inline void project_tc(const f32* s, f32 sx, f32 sy, f32* tc) {
    f32 q = s[2] != 0 ? s[2] : 1.0f;
    if (q == 1.0f) {
        tc[0] = s[0] * sx;
        tc[1] = s[1] * sy;
    } else {
        tc[0] = s[0] / q * sx;
        tc[1] = s[1] / q * sy;
    }
}

// a regular texgen's s, t, q
void texgen_regular(TexgenPlan& g, const VtxIn& v, f32* out) {
    f32 src[4] = {0, 0, 1, 1};
    switch (g.row) {
    case 0: src[0] = v.pos[0], src[1] = v.pos[1], src[2] = v.pos[2]; break;
    case 1: src[0] = v.nrm[0][0], src[1] = v.nrm[0][1], src[2] = v.nrm[0][2]; break;
    case 2: src[0] = v.color[0][0] / 255.0f, src[1] = v.color[0][1] / 255.0f, src[2] = v.color[0][2] / 255.0f; break;
    case 3: src[0] = v.nrm[2][0], src[1] = v.nrm[2][1], src[2] = v.nrm[2][2]; break;
    case 4: src[0] = v.nrm[1][0], src[1] = v.nrm[1][1], src[2] = v.nrm[1][2]; break;
    default:
        if (g.row >= 5 && g.row <= 12)
            src[0] = v.tex[g.row - 5][0], src[1] = v.tex[g.row - 5][1];
        break;
    }
    texgen_apply(g, src, out);
}

// attribute reading for the draw path: element pointer for an indexed attribute
inline const u8* attr_data(const GxAttrFormat& a, u32 attr, const u8* v) {
    if (a.mode == 1)
        return v + a.offset;
    return gx_array_ptr(attr, a.mode == 2 ? v[a.offset] : gx_be16(v + a.offset));
}

inline f32 be_f32(const u8* p) {
    u32 b = gx_be32(p);
    f32 v;
    memcpy(&v, &b, 4);
    return v;
}

// An output texture coordinate in transform_simple: the texgen and where its source row is
struct SimpleTc {
    enum Src : s8 { POS = -1, NONE = -2, ZERO = -3 };  // or >= 0: stream offset of float s(, t)
    const TexgenPlan* g;
    s16 src;
    u8 n;  // the coordinate's components (1: s only)
    u8 k;  // the output
};

// The draw path for the commonest draws, the layouts' quads and glyphs: float positions in the
// stream, no matrix indices, no normals, an unlit color 0 and regular texgens from the position
// or a float texture coordinate in the stream. The same arithmetic as the general loop, without
// its per-vertex dispatch.
template <u32 NPOS, bool ORTHO>
void transform_simple(u32 count, const u8* data, const GxVertexFormat& f, const Mtx34& pm, const f32* p,
                      const ColorPlan& c0, const SimpleTc* tcs, u32 ntc, const GxDrawSetup& setup,
                      GxDrawVertex* out) {
    // color 0: each byte from the vertex color or a constant (the material, or 0)
    u8 cconst[4] = {0, 0, 0, 0};
    bool vtx[4] = {false, false, false, false};
    const GxAttrFormat& cf = f.color[0];
    if (c0.on) {
        for (int k = 0; k < 4; k++) {
            bool from_vtx = k < 3 ? c0.rgb.mat_vtx : c0.alpha.mat_vtx;
            if (from_vtx)
                vtx[k] = cf.mode != 0;  // (no vertex color: 0)
            else
                cconst[k] = (u8)clamp255(c0.mat[k]);
        }
    }
    const bool any_vtx = vtx[0] || vtx[1] || vtx[2] || vtx[3];
    const f32* m = pm.m;
    for (u32 i = 0; i < count; i++, data += f.size) {
        GxDrawVertex& o = out[i];
        const u8* d = data + f.pos.offset;
        f32 pos[3] = {be_f32(d), be_f32(d + 4), NPOS == 3 ? be_f32(d + 8) : 0.0f};
        f32 epos[3];
        for (int r = 0; r < 3; r++)
            epos[r] = m[4 * r] * pos[0] + m[4 * r + 1] * pos[1] + m[4 * r + 2] * pos[2] + m[4 * r + 3];
        if (ORTHO) {
            o.pos[0] = p[0] * epos[0] + p[1];
            o.pos[1] = p[2] * epos[1] + p[3];
            o.pos[2] = p[4] * epos[2] + p[5];
            o.pos[3] = 1.0f;
        } else {
            o.pos[0] = p[0] * epos[0] + p[1] * epos[2];
            o.pos[1] = p[2] * epos[1] + p[3] * epos[2];
            o.pos[2] = p[4] * epos[2] + p[5];
            o.pos[3] = -epos[2];
        }
        if (any_vtx) {
            u8 rgba[4];
            gx_read_color(attr_data(cf, 2, data), cf.type, rgba);
            for (int k = 0; k < 4; k++)
                o.color[k] = vtx[k] ? rgba[k] : cconst[k];
        } else {
            memcpy(o.color, cconst, 4);
        }
        for (u32 j = 0; j < ntc; j++) {
            const SimpleTc& t = tcs[j];
            f32* tc = o.tc[t.k];
            if (t.src == SimpleTc::ZERO) {
                tc[0] = tc[1] = 0;
                continue;
            }
            f32 src[4] = {0, 0, 1, 1};
            if (t.src == SimpleTc::POS) {
                src[0] = pos[0], src[1] = pos[1], src[2] = pos[2];
            } else if (t.src >= 0) {
                src[0] = be_f32(data + t.src);
                if (t.n == 2)
                    src[1] = be_f32(data + t.src + 4);
            }
            f32 stq[3];
            texgen_apply(*t.g, src, stq);
            project_tc(stq, setup.sx[t.k], setup.sy[t.k], tc);
        }
    }
}

// NOMA_CHECK_GX_DRAW=bench: the general loop for every draw (to time and compare the short ways)
bool s_force_general;

// A simple draw's plan, kept while the vertex format, the XF registers, the lighting and the
// texgens the outputs use stay the same; its matrices are loaded again when XF memory changed.
struct SimplePlan {
    bool valid = false;
    u32 fmt_serial = 0, reg_serial = 0, mem_serial = 0, light_serial = 0;
    s8 texgen[3] = {-1, -1, -1};
    bool ortho = false, dualtex = false;
    f32 p[6];
    u32 pos_index = 0;
    Mtx34 pm;
    u32 tg_used = 0;
    u8 def_index[8];
    TexgenPlan tg[8];
    SimpleTc tcs[3];
    u32 ntc = 0;
};

void run_simple(const SimplePlan& sp, u32 count, const u8* data, const GxVertexFormat& f, const ColorPlan& c0,
                const GxDrawSetup& setup, GxDrawVertex* out) {
    if (f.pos.n == 3)
        sp.ortho ? transform_simple<3, true>(count, data, f, sp.pm, sp.p, c0, sp.tcs, sp.ntc, setup, out)
                 : transform_simple<3, false>(count, data, f, sp.pm, sp.p, c0, sp.tcs, sp.ntc, setup, out);
    else
        sp.ortho ? transform_simple<2, true>(count, data, f, sp.pm, sp.p, c0, sp.tcs, sp.ntc, setup, out)
                 : transform_simple<2, false>(count, data, f, sp.pm, sp.p, c0, sp.tcs, sp.ntc, setup, out);
}

}  // namespace

void gx_transform_draw(u32 vat, u32 count, const u8* data, const GxDrawSetup& setup, GxDrawVertex* out) {
    const GxVertexFormat& f = gx_vertex_format(vat);

    // color channel 0, and 1 if a texgen reads it (planned again when the lighting changed)
    static ColorPlan col[2];
    static u32 s_planned[2] = {~0u, ~0u};
    if (s_planned[0] != g_gx.light_serial) {
        s_planned[0] = g_gx.light_serial;
        plan_color(0, col[0]);
    }

    // the draw before was simple, and nothing it was planned from changed but maybe a matrix
    static SimplePlan s_simple;
    SimplePlan& sp = s_simple;
    if (sp.valid && !s_force_general && sp.fmt_serial == f.serial && sp.reg_serial == g_gx.xf_reg_serial &&
        sp.light_serial == g_gx.light_serial && sp.texgen[0] == setup.texgen[0] &&
        sp.texgen[1] == setup.texgen[1] && sp.texgen[2] == setup.texgen[2]) {
        if (sp.mem_serial != g_gx.xf_mem_serial) {
            sp.mem_serial = g_gx.xf_mem_serial;
            load_mtx34(sp.pos_index * 4u, sp.pm);
            for (u32 t = 0; t < 8; t++)
                if (sp.tg_used & (1u << t))
                    plan_texgen(t, f, sp.def_index[t], sp.dualtex, sp.tg[t]);
        }
        run_simple(sp, count, data, f, col[0], setup, out);
        return;
    }

    const u32 mia = g_gx.xf[XF_MATINDEX_A], mib = g_gx.xf[XF_MATINDEX_B];
    const u32 num_texgens = g_gx.xf[XF_NUMTEXGENS];
    const bool dualtex = g_gx.xf[XF_DUALTEX] & 1;

    // projection
    f32 p[6];
    for (int i = 0; i < 6; i++)
        p[i] = xf_f32(XF_PROJECTION + i);
    const bool ortho = g_gx.xf[XF_PROJECTION + 6] != 0;

    // the texgens the outputs use (and an emboss texgen's source)
    TexgenPlan tg[8];
    u32 tg_used = 0;  // bit per texgen
    bool need_ch1 = false;
    for (int o = 0; o < 3; o++) {
        s8 t = setup.texgen[o];
        if (t < 0 || (u32)t >= num_texgens)
            continue;
        tg_used |= 1u << t;
        u32 type = bits(g_gx.xf[XF_TEXMTXINFO + t], 4, 3);
        if (type == 1) {
            u32 src = bits(g_gx.xf[XF_TEXMTXINFO + t], 12, 3);
            if (src < num_texgens)
                tg_used |= 1u << src;
        }
        need_ch1 |= type == 3;
    }
    for (u32 t = 0; t < 8; t++)
        if (tg_used & (1u << t))
            plan_texgen(t, f, t < 4 ? bits(mia, 6 + 6 * t, 6) : bits(mib, 6 * (t - 4), 6), dualtex, tg[t]);
    if (need_ch1 && s_planned[1] != g_gx.light_serial) {
        s_planned[1] = g_gx.light_serial;
        plan_color(1, col[1]);
    }

    // which attributes are read
    bool need_nrm_eye = col[0].on && (col[0].rgb.lit || col[0].alpha.lit);
    need_nrm_eye |= need_ch1 && col[1].on && (col[1].rgb.lit || col[1].alpha.lit);
    bool need_nrm_raw = false, need_tex[8] = {};
    for (u32 t = 0; t < 8; t++) {
        if (!(tg_used & (1u << t)))
            continue;
        u32 row = tg[t].row;
        need_nrm_raw |= row == 1 || row == 3 || row == 4;
        if (row >= 5 && row <= 12)
            need_tex[row - 5] = true;
    }
    const bool read_nrm = f.nrm.mode && (need_nrm_eye || need_nrm_raw);
    const u32 nrm_vectors = f.nbt ? 3 : 1;

    // the position (and normal) matrix, reloaded when the index changes
    u32 pos_index = bits(mia, 0, 6);
    Mtx34 pm;
    Mtx33 nm;
    load_mtx34(pos_index * 4u, pm);
    if (need_nrm_eye)
        load_mtx33(XF_MEM_NORMAL + (pos_index & 31u) * 3, nm);

    // the commonest draws take the short way (planned once for the draws after with the same setup)
    bool simple = !s_force_general && f.pos.mode == 1 && f.pos.type == 4 && f.pos_mtx < 0 && !read_nrm && !need_nrm_eye &&
                  !need_ch1 && !(col[0].on && (col[0].rgb.lit || col[0].alpha.lit));
    SimpleTc tcs[3];
    u32 ntc = 0;
    for (int k = 0; k < 3 && simple; k++) {
        s8 t = setup.texgen[k];
        if (t < 0)
            continue;
        SimpleTc& st = tcs[ntc++];
        st.k = (u8)k, st.g = nullptr, st.n = 0;
        if ((u32)t >= num_texgens) {
            st.src = SimpleTc::ZERO;
            continue;
        }
        const TexgenPlan& g = tg[t];
        st.g = &g;
        if ((g.type >= 1 && g.type <= 3) || g.mtx_off >= 0 || (g.row >= 1 && g.row <= 4)) {
            simple = false;
        } else if (g.row == 0) {
            st.src = SimpleTc::POS;
        } else if (g.row >= 5 && g.row <= 12) {
            const GxAttrFormat& a = f.tex[g.row - 5];
            if (!a.mode)
                st.src = SimpleTc::NONE;
            else if (a.mode == 1 && a.type == 4)
                st.src = a.offset, st.n = a.n;
            else
                simple = false;
        } else {
            st.src = SimpleTc::NONE;
        }
    }
    if (simple) {
        sp.valid = true;
        sp.fmt_serial = f.serial, sp.reg_serial = g_gx.xf_reg_serial, sp.mem_serial = g_gx.xf_mem_serial;
        sp.light_serial = g_gx.light_serial;
        memcpy(sp.texgen, setup.texgen, sizeof(sp.texgen));
        sp.ortho = ortho, sp.dualtex = dualtex;
        memcpy(sp.p, p, sizeof(p));
        sp.pos_index = pos_index, sp.pm = pm;
        sp.tg_used = tg_used;
        for (u32 t = 0; t < 8; t++) {
            sp.def_index[t] = (u8)(t < 4 ? bits(mia, 6 + 6 * t, 6) : bits(mib, 6 * (t - 4), 6));
            if (tg_used & (1u << t))
                sp.tg[t] = tg[t];
        }
        sp.ntc = ntc;
        for (u32 j = 0; j < ntc; j++) {
            sp.tcs[j] = tcs[j];
            if (tcs[j].g)
                sp.tcs[j].g = &sp.tg[tcs[j].g - tg];
        }
        run_simple(sp, count, data, f, col[0], setup, out);
        return;
    }

    VtxIn v;
    memset(&v, 0, sizeof(v));
    for (u32 i = 0; i < count; i++, data += f.size) {
        GxDrawVertex& o = out[i];
        if (f.pos_mtx >= 0) {
            u32 idx = data[f.pos_mtx] & 63;
            if (idx != pos_index) {
                pos_index = idx;
                load_mtx34(idx * 4u, pm);
                if (need_nrm_eye)
                    load_mtx33(XF_MEM_NORMAL + (idx & 31u) * 3, nm);
            }
        }

        // position -> eye space -> clip space
        v.pos[2] = 0;
        if (f.pos.mode)
            gx_read_comps(attr_data(f.pos, 0, data), f.pos.type, f.pos.n, f.pos.scale, v.pos);
        const f32* m = pm.m;
        f32 epos[3];
        for (int r = 0; r < 3; r++)
            epos[r] = m[4 * r] * v.pos[0] + m[4 * r + 1] * v.pos[1] + m[4 * r + 2] * v.pos[2] + m[4 * r + 3];
        if (ortho) {
            o.pos[0] = p[0] * epos[0] + p[1];
            o.pos[1] = p[2] * epos[1] + p[3];
            o.pos[2] = p[4] * epos[2] + p[5];
            o.pos[3] = 1.0f;
        } else {
            o.pos[0] = p[0] * epos[0] + p[1] * epos[2];
            o.pos[1] = p[2] * epos[1] + p[3] * epos[2];
            o.pos[2] = p[4] * epos[2] + p[5];
            o.pos[3] = -epos[2];
        }

        // normals
        f32 enrm[3] = {0, 0, 0};
        if (read_nrm) {
            if (f.nrm.mode == 1) {
                gx_read_comps(data + f.nrm.offset, f.nrm.type, 3 * nrm_vectors, f.nrm.scale, v.nrm[0]);
            } else if (f.nbt && f.nrm_index3) {
                u32 isz = f.nrm.mode == 2 ? 1 : 2;
                for (u32 k = 0; k < 3; k++) {
                    const u8* ip = data + f.nrm.offset + k * isz;
                    const u8* src = gx_array_ptr(1, isz == 1 ? *ip : gx_be16(ip)) + k * 3 * gx_comp_bytes(f.nrm.type);
                    gx_read_comps(src, f.nrm.type, 3, f.nrm.scale, v.nrm[k]);
                }
            } else {
                gx_read_comps(attr_data(f.nrm, 1, data), f.nrm.type, 3 * nrm_vectors, f.nrm.scale, v.nrm[0]);
            }
            if (need_nrm_eye) {
                const f32* n = nm.m;
                for (int r = 0; r < 3; r++)
                    enrm[r] = n[3 * r] * v.nrm[0][0] + n[3 * r + 1] * v.nrm[0][1] + n[3 * r + 2] * v.nrm[0][2];
                normalize3(enrm);
            }
        }

        // colors
        for (int ch = 0; ch < 2; ch++) {
            const GxAttrFormat& c = f.color[ch];
            if (!c.mode)
                continue;
            u8 rgba[4];
            gx_read_color(attr_data(c, 2 + ch, data), c.type, rgba);
            for (int k = 0; k < 4; k++)
                v.color[ch][k] = (f32)rgba[k];
        }
        f32 res[2][4];
        color_out(col[0], v.color[0], epos, enrm, res[0]);
        for (int k = 0; k < 4; k++)
            o.color[k] = (u8)res[0][k];
        if (need_ch1)
            color_out(col[1], v.color[1], epos, enrm, res[1]);

        // texture coordinates
        if (tg_used) {
            for (int t = 0; t < 8; t++) {
                if (!need_tex[t] || !f.tex[t].mode)
                    continue;
                gx_read_comps(attr_data(f.tex[t], 4 + t, data), f.tex[t].type, f.tex[t].n, f.tex[t].scale, v.tex[t]);
            }
            f32 stq[8][3];
            // regular texgens first (an emboss texgen reads one)
            for (u32 t = 0; t < 8; t++) {
                if (!(tg_used & (1u << t)))
                    continue;
                TexgenPlan& g = tg[t];
                if (g.type == 2 || g.type == 3 || g.type == 1)
                    continue;
                if (g.mtx_off >= 0) {
                    u32 idx = data[g.mtx_off] & 63;
                    if (idx != g.cur_index) {
                        g.cur_index = idx;
                        load_mtx34(idx * 4u, g.mtx);
                    }
                }
                texgen_regular(g, v, stq[t]);
            }
            for (u32 t = 0; t < 8; t++) {
                if (!(tg_used & (1u << t)))
                    continue;
                TexgenPlan& g = tg[t];
                if (g.type == 2 || g.type == 3) {
                    const f32* c = res[g.type - 2];
                    stq[t][0] = c[0] / 255.0f, stq[t][1] = c[1] / 255.0f, stq[t][2] = 1.0f;
                } else if (g.type == 1) {
                    bool have = g.emboss_src < num_texgens && (tg_used & (1u << g.emboss_src));
                    stq[t][0] = have ? stq[g.emboss_src][0] : 0, stq[t][1] = have ? stq[g.emboss_src][1] : 0;
                    stq[t][2] = 1.0f;
                }
            }
            for (int k = 0; k < 3; k++) {
                s8 t = setup.texgen[k];
                if (t < 0)
                    continue;
                if ((u32)t >= num_texgens) {
                    o.tc[k][0] = o.tc[k][1] = 0;
                    continue;
                }
                project_tc(stq[t], setup.sx[k], setup.sy[k], o.tc[k]);
            }
        } else {
            for (int k = 0; k < 3; k++)
                if (setup.texgen[k] >= 0)
                    o.tc[k][0] = o.tc[k][1] = 0;
        }
    }
}

}  // namespace guest

// ---------------------------------------------------------------- self-check
// NOMA_CHECK_GX_DRAW=1 (host renderer): run every draw through gx_transform_draw as well and
// compare with gx_transform's results; logs mismatches and what the draws use.
namespace guest {

// NOMA_CHECK_GX_DRAW=bench: each draw's time through gx_transform_draw against the general loop
// alone (both repeated), by kind of draw, and whether their results are the same bytes
static void bench_transform_draw(u32 vat, u32 count, const u8* data) {
    using clock = std::chrono::steady_clock;
    static std::vector<GxDrawVertex> a, b;
    a.resize(count), b.resize(count);
    GxDrawSetup su;
    for (u32 k = 0; k < 3; k++)
        su.texgen[k] = k < g_gx.xf[XF_NUMTEXGENS] ? (s8)k : -1, su.sx[k] = 1.0f + k, su.sy[k] = 0.5f;
    const int REP = 4;
    auto time = [&](GxDrawVertex* out) {
        auto t0 = clock::now();
        for (int r = 0; r < REP; r++)
            gx_transform_draw(vat, count, data, su, out);
        return (double)std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count() / REP;
    };
    memset(a.data(), 0xA5, count * sizeof(GxDrawVertex));
    memset(b.data(), 0xA5, count * sizeof(GxDrawVertex));
    double fast = time(a.data());
    s_force_general = true;
    double general = time(b.data());
    s_force_general = false;
    struct Kind {
        u64 draws = 0, verts = 0, differ = 0;
        double fast = 0, general = 0;
    };
    static Kind s_kinds[8];  // lit, matrix indices in the stream, indexed positions
    const GxVertexFormat& f = gx_vertex_format(vat);
    bool lit = (g_gx.xf[XF_COLOR0CNTRL] & 2) || (g_gx.xf[XF_ALPHA0CNTRL] & 2);
    Kind& k = s_kinds[lit | (f.pos_mtx >= 0) << 1 | (f.pos.mode >= 2) << 2];
    k.draws++, k.verts += count, k.fast += fast, k.general += general;
    if (memcmp(a.data(), b.data(), count * sizeof(GxDrawVertex)) != 0 && k.differ++ < 4)
        log("gx bench: vat %u, %u vertices: the short way differs from the general loop", vat, count);
    static u64 s_n;
    if (++s_n % 50000)
        return;
    for (int i = 0; i < 8; i++) {
        Kind& x = s_kinds[i];
        if (x.draws)
            log("gx bench: %s%s%s: %llu draws, %.1f vertices each: %.1f ns a vertex (general loop %.1f), %llu differ",
                i & 1 ? "lit" : "unlit", i & 2 ? ", matrix indices" : "", i & 4 ? ", indexed positions" : "",
                (unsigned long long)x.draws, (double)x.verts / x.draws, x.fast / x.verts, x.general / x.verts,
                (unsigned long long)x.differ);
        x = Kind();
    }
}

// NOMA_CHECK_GX_DRAW=census: what the draws use, by kind (vertex format, lighting, texgens, culling),
// with draws and vertices per kind and how many matrices a draw with matrix indices refers to;
// logged every 20000 draws
static void census_draw(u32 vat, u32 count, const u8* data) {
    const GxVertexFormat& f = gx_vertex_format(vat);
    char key[512];
    int n = 0;
    auto put = [&](const char* fmt, auto... args) {
        if (n < (int)sizeof(key) - 1)
            n += snprintf(key + n, sizeof(key) - n, fmt, args...);
    };
    u32 prim = gx_current_prim();
    put("%s %s", gx_in_display_list() ? "dl" : "im",
        prim == 0x80 ? "quads" : prim == 0x88 ? "quads2" : prim == 0x90 ? "tris" : prim == 0x98 ? "strip" : prim == 0xA0 ? "fan" : prim == 0xA8 ? "lines" : prim == 0xB0 ? "lstrip" : "points");
    auto attr = [&](const char* name, const GxAttrFormat& a) {
        if (a.mode)
            put(" %s%u%c%u", name, a.mode, "ubUSf???"[a.type & 7], a.n);
    };
    attr("p", f.pos);
    // distinct position matrices (and texture matrices) in the stream
    u32 npm = 0, ntm = 0;
    if (f.pos_mtx >= 0) {
        u64 seen = 0;
        for (u32 i = 0; i < count; i++)
            seen |= 1ull << (data[i * f.size + f.pos_mtx] & 63);
        npm = __builtin_popcountll(seen);
        put(" pm%u", npm);
    }
    for (u32 t = 0; t < 8; t++) {
        if (f.tex_mtx[t] < 0)
            continue;
        u64 seen = 0;
        for (u32 i = 0; i < count; i++)
            seen |= 1ull << (data[i * f.size + f.tex_mtx[t]] & 63);
        ntm = std::max(ntm, (u32)__builtin_popcountll(seen));
        put(" tm%u:%u", t, __builtin_popcountll(seen));
    }
    if (f.nrm.mode)
        put(" n%u%c%s", f.nrm.mode, "ubUSf???"[f.nrm.type & 7], f.nbt ? (f.nrm_index3 ? "nbt3" : "nbt") : "");
    attr("c", f.color[0]);
    attr("C", f.color[1]);
    for (u32 t = 0; t < 8; t++)
        if (f.tex[t].mode)
            put(" t%u:%u%c%u", t, f.tex[t].mode, "ubUSf???"[f.tex[t].type & 7], f.tex[t].n);
    // lighting
    u32 nc = g_gx.xf[XF_NUMCOLORS];
    put(" | nc%u", nc);
    for (u32 ch = 0; ch < nc && ch < 2; ch++) {
        for (int a = 0; a < 2; a++) {
            u32 ctrl = g_gx.xf[(a ? XF_ALPHA0CNTRL : XF_COLOR0CNTRL) + ch];
            u32 mask = bits(ctrl, 2, 4) | (bits(ctrl, 11, 4) << 4);
            put(" %c%u:%s%s%s", a ? 'A' : 'L', ch, ctrl & 1 ? "mv" : "mr", ctrl & 2 ? " lit" : "", (ctrl & 2) && (ctrl & 0x40) ? " av" : "");
            if (ctrl & 2)
                put(" d%u at%u l%02X", bits(ctrl, 7, 2), bits(ctrl, 9, 2), mask);
        }
    }
    // texgens
    u32 ntg = g_gx.xf[XF_NUMTEXGENS];
    bool dual = g_gx.xf[XF_DUALTEX] & 1;
    put(" | tg%u%s", ntg, dual ? " dual" : "");
    for (u32 t = 0; t < ntg && t < 8; t++) {
        u32 info = g_gx.xf[XF_TEXMTXINFO + t];
        u32 type = bits(info, 4, 3), row = bits(info, 7, 5);
        put(" g%u:%s%s%s", t, type == 0 ? "" : type == 1 ? "emboss" : type == 2 ? "col0" : type == 3 ? "col1" : "?",
            row == 0 ? "pos" : row == 1 ? "nrm" : row == 2 ? "col" : row == 3 ? "bin" : row == 4 ? "tan" : "tc",
            bits(info, 1, 1) ? "q" : "");
        if (row >= 5)
            put("%u", row - 5);
        if (bits(info, 2, 1))
            put("z");
        if (f.tex_mtx[t] >= 0)
            put("*");
        if (dual) {
            u32 post = g_gx.xf[XF_POSTMTXINFO + t];
            u32 m = bits(post, 0, 6) * 4u;
            static const u32 k_id[12] = {0x3F800000, 0, 0, 0, 0, 0x3F800000, 0, 0, 0, 0, 0x3F800000, 0};
            bool ident = memcmp(&g_gx.xf[XF_MEM_POST + m], k_id, sizeof(k_id)) == 0;
            put("%s%s", ident ? "" : "P", bits(post, 8, 1) ? "N" : "");
        }
    }
    put(" | cull%u vp%c%c proj%c", bits(g_gx.bp[BP_GENMODE], 14, 2), xf_f32(XF_VIEWPORT) < 0 ? '-' : '+',
        xf_f32(XF_VIEWPORT + 1) < 0 ? '-' : '+', g_gx.xf[XF_PROJECTION + 6] ? 'o' : 'p');

    struct Agg {
        u64 draws = 0, verts = 0;
        u32 max_pm = 0, max_tm = 0;
    };
    static std::unordered_map<std::string, Agg> s_agg;
    static u64 s_draws, s_verts, s_dl_draws;
    Agg& g = s_agg[key];
    g.draws++, g.verts += count;
    g.max_pm = std::max(g.max_pm, npm), g.max_tm = std::max(g.max_tm, ntm);
    s_draws++, s_verts += count, s_dl_draws += gx_in_display_list();
    if (s_draws % 20000)
        return;
    log("census: %llu draws (%llu in display lists), %llu vertices, %zu kinds", (unsigned long long)s_draws,
        (unsigned long long)s_dl_draws, (unsigned long long)s_verts, s_agg.size());
    std::vector<std::pair<u64, const std::string*>> v;
    for (auto& kv : s_agg)
        v.push_back({kv.second.verts, &kv.first});
    std::sort(v.rbegin(), v.rend());
    for (size_t i = 0; i < v.size() && i < 80; i++) {
        const Agg& a = s_agg[*v[i].second];
        log("census: %5.1f%% verts %7llu draws %5.1f v/draw maxpm %u maxtm %u | %s", 100.0 * a.verts / s_verts,
            (unsigned long long)a.draws, (double)a.verts / a.draws, a.max_pm, a.max_tm, v[i].second->c_str());
    }
}

void gx_check_transform_draw(u32 vat, u32 count, const u8* data, const GxOutVertex* ref) {
    static const char* s_env = getenv("NOMA_CHECK_GX_DRAW");
    if (!s_env || !count)
        return;
    if (strcmp(s_env, "bench") == 0) {
        bench_transform_draw(vat, count, data);
        return;
    }
    if (strcmp(s_env, "census") == 0) {
        census_draw(vat, count, data);
        return;
    }
    static u64 s_draws, s_vertices, s_bad_draws, s_bad_values, s_lit, s_mtx_stream;
    static std::vector<GxDrawVertex> out;
    out.resize(count);
    GxDrawSetup setup;
    u32 first = (u32)(s_draws % 6);
    for (int k = 0; k < 3; k++)
        setup.texgen[k] = (s8)(first + k), setup.sx[k] = 1.0f + k, setup.sy[k] = 1.0f / (1 + k);
    gx_transform_draw(vat, count, data, setup, out.data());

    auto close = [](f32 a, f32 b) {
        if (a == b || (std::isnan(a) && std::isnan(b)))
            return true;
        f32 d = std::fabs(a - b), m = std::max(std::fabs(a), std::fabs(b));
        return d <= 1e-4f * m || d <= 1e-5f;
    };
    u32 bad = 0;
    for (u32 i = 0; i < count; i++) {
        const GxOutVertex& r = ref[i];
        const GxDrawVertex& o = out[i];
        bool ok = true;
        for (int k = 0; k < 4; k++)
            ok &= close(r.clip[k], o.pos[k]);
        for (int k = 0; k < 4; k++) {
            f32 c = r.color[0][k];
            u8 want = (u8)(c < 0 ? 0 : c > 255 ? 255 : c);
            ok &= std::abs((int)want - (int)o.color[k]) <= 1;
        }
        for (int k = 0; k < 3; k++) {
            const f32* t = r.tex[setup.texgen[k]];
            f32 q = t[2] != 0 ? t[2] : 1.0f;
            ok &= close(t[0] / q * setup.sx[k], o.tc[k][0]) && close(t[1] / q * setup.sy[k], o.tc[k][1]);
        }
        if (!ok) {
            if (s_bad_values++ < 20)
                log("gx check: vat %u vertex %u/%u: ref (%g %g %g %g) c %g %g %g %g t0 %g %g | draw (%g %g %g %g) "
                    "c %u %u %u %u t0 %g %g",
                    vat, i, count, r.clip[0], r.clip[1], r.clip[2], r.clip[3], r.color[0][0], r.color[0][1],
                    r.color[0][2], r.color[0][3], r.tex[first][0], r.tex[first][1], o.pos[0], o.pos[1], o.pos[2],
                    o.pos[3], o.color[0], o.color[1], o.color[2], o.color[3], o.tc[0][0], o.tc[0][1]);
            bad++;
        }
    }
    s_draws++;
    s_vertices += count;
    s_bad_draws += bad != 0;
    s_lit += (g_gx.xf[XF_COLOR0CNTRL] & 2) || (g_gx.xf[XF_ALPHA0CNTRL] & 2);
    s_mtx_stream += g_gx.cp[CP_VCD_LO] & 1;
    if (s_draws % 20000 == 0)
        log("gx check: %llu draws (%llu vertices): %llu differ (%llu vertices); %llu lit, %llu with matrix indices",
            (unsigned long long)s_draws, (unsigned long long)s_vertices, (unsigned long long)s_bad_draws,
            (unsigned long long)s_bad_values, (unsigned long long)s_lit, (unsigned long long)s_mtx_stream);
}

}  // namespace guest
