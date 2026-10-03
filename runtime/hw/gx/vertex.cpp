// Vertex decoding: the attributes of each vertex in stream order, direct or as 8/16-bit indices
// into the arrays the CP registers describe (base 0xA0 + attribute, stride 0xB0 + attribute).
//
//   matrix indices (position, texture 0-7: one byte each), position, normal (or normal,
//   binormal, tangent), color 0, color 1, texture coordinates 0-7
//
// Formats come from the vertex attribute tables (CP 0x70/0x80/0x90 + vat): component type
// (u8 s8 u16 s16 f32), element count and, for integers, a fixed-point shift.
#include "regs.h"
#include "vertex.h"

namespace guest {

namespace {

struct Reader {
    const u8* p;
    u8 u8_() { return *p++; }
    u16 u16_() {
        u16 v = (u16)(p[0] << 8 | p[1]);
        p += 2;
        return v;
    }
};

// array element `index` of attribute `attr` in guest memory
const u8* array_ptr(u32 attr, u32 index) {
    u32 base = g_gx.cp[CP_ARRAY_BASE + attr], stride = g_gx.cp[CP_ARRAY_STRIDE + attr];
    u32 a = 0x80000000 | ((base + index * stride) & 0x1FFFFFFF);
    const u8* h = host_ptr(a);
    static const u8 zero[64] = {};
    return h ? h : zero;
}

const u32 k_comp_bytes[8] = {1, 1, 2, 2, 4, 1, 1, 1};

f32 read_comp(const u8*& p, u32 type, f32 scale) {
    switch (type) {
    case 0: return (f32)*p++ * scale;
    case 1: return (f32)(s8)*p++ * scale;
    case 2: {
        u16 v = (u16)(p[0] << 8 | p[1]);
        p += 2;
        return (f32)v * scale;
    }
    case 3: {
        s16 v = (s16)(p[0] << 8 | p[1]);
        p += 2;
        return (f32)v * scale;
    }
    case 4: {
        u32 b = (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
        p += 4;
        f32 f;
        memcpy(&f, &b, 4);
        return f;
    }
    default:
        p++;
        return 0;
    }
}

void read_color(const u8* p, u32 fmt, u8* out) {
    switch (fmt) {
    case 0: {  // RGB565
        u16 v = (u16)(p[0] << 8 | p[1]);
        out[0] = (u8)((v >> 11) * 255 / 31);
        out[1] = (u8)(((v >> 5) & 63) * 255 / 63);
        out[2] = (u8)((v & 31) * 255 / 31);
        out[3] = 255;
        break;
    }
    case 1:  // RGB888
    case 2:  // RGB888x
        out[0] = p[0], out[1] = p[1], out[2] = p[2], out[3] = 255;
        break;
    case 3: {  // RGBA4444
        out[0] = (u8)((p[0] >> 4) * 17);
        out[1] = (u8)((p[0] & 15) * 17);
        out[2] = (u8)((p[1] >> 4) * 17);
        out[3] = (u8)((p[1] & 15) * 17);
        break;
    }
    case 4: {  // RGBA6666
        u32 v = (u32)p[0] << 16 | (u32)p[1] << 8 | p[2];
        u32 c[4] = {(v >> 18) & 63, (v >> 12) & 63, (v >> 6) & 63, v & 63};
        for (int i = 0; i < 4; i++)
            out[i] = (u8)((c[i] << 2) | (c[i] >> 4));
        break;
    }
    default:  // RGBA8888
        out[0] = p[0], out[1] = p[1], out[2] = p[2], out[3] = p[3];
        break;
    }
}

const u32 k_color_bytes[8] = {2, 3, 4, 2, 3, 4, 4, 4};

}  // namespace

void gx_decode_vertices(u32 vat, u32 count, const u8* data, GxVertex* out) {
    const u32 lo = g_gx.cp[CP_VCD_LO], hi = g_gx.cp[CP_VCD_HI];
    const u32 a = g_gx.cp[CP_VAT_A + vat], b = g_gx.cp[CP_VAT_B + vat], c = g_gx.cp[CP_VAT_C + vat];
    const u32 mia = g_gx.xf[XF_MATINDEX_A], mib = g_gx.xf[XF_MATINDEX_B];

    // formats
    const u32 pos_mode = bits(lo, 9, 2), nrm_mode = bits(lo, 11, 2);
    const u32 col_mode[2] = {bits(lo, 13, 2), bits(lo, 15, 2)};
    const u32 pos_n = bits(a, 0, 1) ? 3 : 2, pos_type = bits(a, 1, 3);
    const f32 pos_scale = pos_type == 4 ? 1.0f : 1.0f / (f32)(1u << bits(a, 4, 5));
    const bool nbt = bits(a, 9, 1), nrm_index3 = bits(a, 31, 1);
    const u32 nrm_type = bits(a, 10, 3);
    const f32 nrm_scale = nrm_type == 1 ? 1.0f / 64 : nrm_type == 3 ? 1.0f / 16384 : 1.0f;
    const u32 col_fmt[2] = {bits(a, 14, 3), bits(a, 18, 3)};
    u32 tc_mode[8], tc_n[8], tc_type[8];
    f32 tc_scale[8];
    const u32 tc_fields[8][3] = {
        {bits(a, 21, 1), bits(a, 22, 3), bits(a, 25, 5)}, {bits(b, 0, 1), bits(b, 1, 3), bits(b, 4, 5)},
        {bits(b, 9, 1), bits(b, 10, 3), bits(b, 13, 5)},  {bits(b, 18, 1), bits(b, 19, 3), bits(b, 22, 5)},
        {bits(b, 27, 1), bits(b, 28, 3), bits(c, 0, 5)},  {bits(c, 5, 1), bits(c, 6, 3), bits(c, 9, 5)},
        {bits(c, 14, 1), bits(c, 15, 3), bits(c, 18, 5)}, {bits(c, 23, 1), bits(c, 24, 3), bits(c, 27, 5)},
    };
    for (int i = 0; i < 8; i++) {
        tc_mode[i] = bits(hi, 2 * i, 2);
        tc_n[i] = tc_fields[i][0] ? 2 : 1;
        tc_type[i] = tc_fields[i][1];
        tc_scale[i] = tc_type[i] == 4 ? 1.0f : 1.0f / (f32)(1u << tc_fields[i][2]);
    }

    Reader r{data};
    for (u32 v = 0; v < count; v++) {
        GxVertex& o = out[v];
        memset(&o, 0, sizeof(o));
        o.pos_mtx = (u8)bits(mia, 0, 6);
        for (int i = 0; i < 8; i++)
            o.tex_mtx[i] = (u8)(i < 4 ? bits(mia, 6 + 6 * i, 6) : bits(mib, 6 * (i - 4), 6));
        if (lo & 1)
            o.pos_mtx = r.u8_() & 63;
        for (int i = 0; i < 8; i++)
            if (lo & (2u << i))
                o.tex_mtx[i] = r.u8_() & 63;

        // position
        if (pos_mode) {
            const u8* p = r.p;
            if (pos_mode != 1)
                p = array_ptr(0, pos_mode == 2 ? r.u8_() : r.u16_());
            for (u32 i = 0; i < pos_n; i++)
                o.pos[i] = read_comp(p, pos_type, pos_scale);
            if (pos_mode == 1)
                r.p = p;
        }
        // normals
        if (nrm_mode) {
            u32 vectors = nbt ? 3 : 1;
            if (nrm_mode == 1) {
                const u8* p = r.p;
                for (u32 k = 0; k < vectors; k++)
                    for (int i = 0; i < 3; i++)
                        o.nrm[k][i] = read_comp(p, nrm_type, nrm_scale);
                r.p = p;
            } else if (nbt && nrm_index3) {
                for (u32 k = 0; k < 3; k++) {
                    u32 idx = nrm_mode == 2 ? r.u8_() : r.u16_();
                    const u8* p = array_ptr(1, idx) + k * 3 * k_comp_bytes[nrm_type];
                    for (int i = 0; i < 3; i++)
                        o.nrm[k][i] = read_comp(p, nrm_type, nrm_scale);
                }
            } else {
                const u8* p = array_ptr(1, nrm_mode == 2 ? r.u8_() : r.u16_());
                for (u32 k = 0; k < vectors; k++)
                    for (int i = 0; i < 3; i++)
                        o.nrm[k][i] = read_comp(p, nrm_type, nrm_scale);
            }
        }
        // colors
        for (int ch = 0; ch < 2; ch++) {
            if (!col_mode[ch])
                continue;
            if (col_mode[ch] == 1) {
                read_color(r.p, col_fmt[ch], o.color[ch]);
                r.p += k_color_bytes[col_fmt[ch]];
            } else {
                u32 idx = col_mode[ch] == 2 ? r.u8_() : r.u16_();
                read_color(array_ptr(2 + ch, idx), col_fmt[ch], o.color[ch]);
            }
        }
        // texture coordinates
        for (int t = 0; t < 8; t++) {
            if (!tc_mode[t])
                continue;
            const u8* p = r.p;
            if (tc_mode[t] != 1)
                p = array_ptr(4 + t, tc_mode[t] == 2 ? r.u8_() : r.u16_());
            for (u32 i = 0; i < tc_n[t]; i++)
                o.tex[t][i] = read_comp(p, tc_type[t], tc_scale[t]);
            if (tc_mode[t] == 1)
                r.p = p;
        }
    }
}

}  // namespace guest
