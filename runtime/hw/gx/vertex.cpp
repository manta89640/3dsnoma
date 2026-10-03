// Vertex decoding: the attributes of each vertex in stream order, direct or as 8/16-bit indices
// into the arrays the CP registers describe (base 0xA0 + attribute, stride 0xB0 + attribute).
//
//   matrix indices (position, texture 0-7: one byte each), position, normal (or normal,
//   binormal, tangent), color 0, color 1, texture coordinates 0-7
//
// Formats come from the vertex attribute tables (CP 0x70/0x80/0x90 + vat): component type
// (u8 s8 u16 s16 f32), element count and, for integers, a fixed-point shift.
#include "vertex.h"
#include "vertex_read.h"

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

const u32 k_comp_bytes[8] = {1, 1, 2, 2, 4, 1, 1, 1};

const u32 k_color_bytes[8] = {2, 3, 4, 2, 3, 4, 4, 4};

// bytes a component or a direct color takes in the stream (a vertex's size in the stream)
const u8 k_stream_comp[8] = {1, 1, 2, 2, 4, 0, 0, 0};
const u8 k_stream_color[8] = {2, 3, 4, 2, 3, 4, 0, 0};

GxVertexFormat compile_format(u32 lo, u32 hi, u32 a, u32 b, u32 c) {
    GxVertexFormat f;
    u32 off = 0;
    if (lo & 1)
        f.pos_mtx = (s8)off++;
    for (int i = 0; i < 8; i++)
        if (lo & (2u << i))
            f.tex_mtx[i] = (s8)off++;
    auto index_bytes = [](u32 mode) { return mode == 2 ? 1u : 2u; };

    GxAttrFormat& p = f.pos;
    p.mode = (u8)bits(lo, 9, 2), p.n = bits(a, 0, 1) ? 3 : 2, p.type = (u8)bits(a, 1, 3);
    p.scale = p.type == 4 ? 1.0f : 1.0f / (f32)(1u << bits(a, 4, 5));
    p.offset = (u8)off;
    if (p.mode)
        off += p.mode == 1 ? p.n * k_stream_comp[p.type] : index_bytes(p.mode);

    GxAttrFormat& n = f.nrm;
    f.nbt = bits(a, 9, 1), f.nrm_index3 = bits(a, 31, 1);
    n.mode = (u8)bits(lo, 11, 2), n.n = f.nbt ? 9 : 3, n.type = (u8)bits(a, 10, 3);
    n.scale = n.type == 1 ? 1.0f / 64 : n.type == 3 ? 1.0f / 16384 : 1.0f;
    n.offset = (u8)off;
    if (n.mode)
        off += n.mode == 1 ? n.n * k_stream_comp[n.type] : index_bytes(n.mode) * ((f.nbt && f.nrm_index3) ? 3 : 1);

    for (int ch = 0; ch < 2; ch++) {
        GxAttrFormat& col = f.color[ch];
        col.mode = (u8)bits(lo, 13 + 2 * ch, 2), col.type = (u8)bits(a, 14 + 4 * ch, 3);
        col.offset = (u8)off;
        if (col.mode)
            off += col.mode == 1 ? k_stream_color[col.type] : index_bytes(col.mode);
    }

    const u32 tc_fields[8][3] = {
        {bits(a, 21, 1), bits(a, 22, 3), bits(a, 25, 5)}, {bits(b, 0, 1), bits(b, 1, 3), bits(b, 4, 5)},
        {bits(b, 9, 1), bits(b, 10, 3), bits(b, 13, 5)},  {bits(b, 18, 1), bits(b, 19, 3), bits(b, 22, 5)},
        {bits(b, 27, 1), bits(b, 28, 3), bits(c, 0, 5)},  {bits(c, 5, 1), bits(c, 6, 3), bits(c, 9, 5)},
        {bits(c, 14, 1), bits(c, 15, 3), bits(c, 18, 5)}, {bits(c, 23, 1), bits(c, 24, 3), bits(c, 27, 5)},
    };
    for (int i = 0; i < 8; i++) {
        GxAttrFormat& t = f.tex[i];
        t.mode = (u8)bits(hi, 2 * i, 2), t.n = tc_fields[i][0] ? 2 : 1, t.type = (u8)tc_fields[i][1];
        t.scale = t.type == 4 ? 1.0f : 1.0f / (f32)(1u << tc_fields[i][2]);
        t.offset = (u8)off;
        if (t.mode)
            off += t.mode == 1 ? t.n * k_stream_comp[t.type] : index_bytes(t.mode);
    }
    f.size = off;
    return f;
}

}  // namespace

const GxVertexFormat& gx_vertex_format(u32 vat) {
    // the formats in use change rarely: one compiled format per vat, kept until its registers change
    struct Slot {
        u32 regs[5] = {~0u, ~0u, ~0u, ~0u, ~0u};
        GxVertexFormat f;
    };
    static Slot s_slots[8];
    vat &= 7;
    const u32 regs[5] = {g_gx.cp[CP_VCD_LO], g_gx.cp[CP_VCD_HI], g_gx.cp[CP_VAT_A + vat], g_gx.cp[CP_VAT_B + vat],
                         g_gx.cp[CP_VAT_C + vat]};
    Slot& s = s_slots[vat];
    if (s.regs[0] != regs[0] || s.regs[1] != regs[1] || s.regs[2] != regs[2] || s.regs[3] != regs[3] ||
        s.regs[4] != regs[4]) {
        static u32 s_serial;
        memcpy(s.regs, regs, sizeof(regs));
        s.f = compile_format(regs[0], regs[1], regs[2], regs[3], regs[4]);
        s.f.serial = ++s_serial;
    }
    return s.f;
}

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
                p = gx_array_ptr(0, pos_mode == 2 ? r.u8_() : r.u16_());
            p = gx_read_comps(p, pos_type, pos_n, pos_scale, o.pos);
            if (pos_mode == 1)
                r.p = p;
        }
        // normals
        if (nrm_mode) {
            u32 vectors = nbt ? 3 : 1;
            if (nrm_mode == 1) {
                const u8* p = r.p;
                r.p = gx_read_comps(p, nrm_type, 3 * vectors, nrm_scale, o.nrm[0]);
            } else if (nbt && nrm_index3) {
                for (u32 k = 0; k < 3; k++) {
                    u32 idx = nrm_mode == 2 ? r.u8_() : r.u16_();
                    const u8* p = gx_array_ptr(1, idx) + k * 3 * k_comp_bytes[nrm_type];
                    gx_read_comps(p, nrm_type, 3, nrm_scale, o.nrm[k]);
                }
            } else {
                const u8* p = gx_array_ptr(1, nrm_mode == 2 ? r.u8_() : r.u16_());
                gx_read_comps(p, nrm_type, 3 * vectors, nrm_scale, o.nrm[0]);
            }
        }
        // colors
        for (int ch = 0; ch < 2; ch++) {
            if (!col_mode[ch])
                continue;
            if (col_mode[ch] == 1) {
                gx_read_color(r.p, col_fmt[ch], o.color[ch]);
                r.p += k_color_bytes[col_fmt[ch]];
            } else {
                u32 idx = col_mode[ch] == 2 ? r.u8_() : r.u16_();
                gx_read_color(gx_array_ptr(2 + ch, idx), col_fmt[ch], o.color[ch]);
            }
        }
        // texture coordinates
        for (int t = 0; t < 8; t++) {
            if (!tc_mode[t])
                continue;
            const u8* p = r.p;
            if (tc_mode[t] != 1)
                p = gx_array_ptr(4 + t, tc_mode[t] == 2 ? r.u8_() : r.u16_());
            p = gx_read_comps(p, tc_type[t], tc_n[t], tc_scale[t], o.tex[t]);
            if (tc_mode[t] == 1)
                r.p = p;
        }
    }
}

}  // namespace guest
