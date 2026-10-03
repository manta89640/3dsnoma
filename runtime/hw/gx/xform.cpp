// The transform unit: positions and normals to eye space (matrix memory), the projection,
// the two color channels (material, ambient, up to eight lights each) and texture coordinate
// generation (a texture matrix, then optionally a normalised post-transform matrix).
#include <algorithm>
#include <cmath>
#include <cstdlib>
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

// attribute reading for the draw path: element pointer for an indexed attribute
inline const u8* attr_data(const GxAttrFormat& a, u32 attr, const u8* v) {
    if (a.mode == 1)
        return v + a.offset;
    return gx_array_ptr(attr, a.mode == 2 ? v[a.offset] : gx_be16(v + a.offset));
}

}  // namespace

void gx_transform_draw(u32 vat, u32 count, const u8* data, const GxDrawSetup& setup, GxDrawVertex* out) {
    const GxVertexFormat& f = gx_vertex_format(vat);
    const u32 mia = g_gx.xf[XF_MATINDEX_A], mib = g_gx.xf[XF_MATINDEX_B];
    const u32 num_texgens = g_gx.xf[XF_NUMTEXGENS];
    const bool dualtex = g_gx.xf[XF_DUALTEX] & 1;

    // projection
    f32 p[6];
    for (int i = 0; i < 6; i++)
        p[i] = xf_f32(XF_PROJECTION + i);
    const bool ortho = g_gx.xf[XF_PROJECTION + 6] != 0;

    // color channel 0, and 1 if a texgen reads it (planned again when the lighting changed)
    static ColorPlan col[2];
    static u32 s_planned[2] = {~0u, ~0u};
    if (s_planned[0] != g_gx.light_serial) {
        s_planned[0] = g_gx.light_serial;
        plan_color(0, col[0]);
    }

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
                const f32* s = stq[t];
                f32 q = s[2] != 0 ? s[2] : 1.0f;
                o.tc[k][0] = s[0] / q * setup.sx[k];
                o.tc[k][1] = s[1] / q * setup.sy[k];
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

void gx_check_transform_draw(u32 vat, u32 count, const u8* data, const GxOutVertex* ref) {
    static bool s_on = getenv("NOMA_CHECK_GX_DRAW") != nullptr;
    if (!s_on || !count)
        return;
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
