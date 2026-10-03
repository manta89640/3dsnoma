// The transform unit: positions and normals to eye space (matrix memory), the projection,
// the two color channels (material, ambient, up to eight lights each) and texture coordinate
// generation (a texture matrix, then optionally a normalised post-transform matrix).
#include <cmath>

#include "regs.h"
#include "vertex.h"

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
