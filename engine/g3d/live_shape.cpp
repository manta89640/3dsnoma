// A ResShp's display lists decoded natively (see live_shape.h). Written apart from the GX
// decoder (runtime/hw/gx/cp.cpp, vertex.cpp) on purpose: NOMA_CHECK_G3D_LIVE compares the two.
// The component readers (runtime/hw/gx/vertex_read.h) are shared: they only read bytes.
#include "engine/g3d/live_shape.h"

#include <cstring>

#include "engine/g3d/live_res.h"
#include "runtime/hw/gx/vertex_read.h"

namespace engine::g3d::live {

namespace {

u32 bits(u32 v, u32 at, u32 n) { return (v >> at) & ((1u << n) - 1); }
u16 be16(const u8* p) { return (u16)(p[0] << 8 | p[1]); }
u32 be32(const u8* p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// bytes of one component of a type in the stream (u8 s8 u16 s16 f32), and of a colour format
// (RGB565 RGB8 RGBX8 RGBA4 RGBA6 RGBA8)
u32 comp_bytes(u32 type) { return type == 2 || type == 3 ? 2 : type == 4 ? 4 : 1; }
const u8 k_color_bytes[8] = {2, 3, 4, 2, 3, 4, 4, 4};

// one attribute's format: mode (0 none, 1 direct, 2 8-bit index, 3 16-bit index), component type
// (or colour format), component count, scale
struct Attr {
    u32 mode = 0, type = 0, n = 0;
    f32 scale = 1;
};
struct Format {
    bool pnmtx = false, texmtx[8] = {};
    Attr pos, nrm, clr[2], tex[8];
    bool nbt = false, nrm3 = false;  // normal, binormal, tangent; with an index each
};

Format format(u32 lo, u32 hi, u32 a, u32 b, u32 c) {
    Format f;
    f.pnmtx = lo & 1;
    for (int i = 0; i < 8; i++)
        f.texmtx[i] = lo & (2u << i);
    f.pos = {bits(lo, 9, 2), bits(a, 1, 3), bits(a, 0, 1) ? 3u : 2u, 1};
    f.pos.scale = f.pos.type == 4 ? 1.0f : 1.0f / (f32)(1u << bits(a, 4, 5));
    f.nbt = bits(a, 9, 1), f.nrm3 = bits(a, 31, 1);
    f.nrm = {bits(lo, 11, 2), bits(a, 10, 3), f.nbt ? 9u : 3u, 1};
    // (normals have fixed scales: s8 6 fraction bits, s16 14)
    f.nrm.scale = f.nrm.type == 1 ? 1.0f / 64 : f.nrm.type == 3 ? 1.0f / 16384 : 1.0f;
    for (int ch = 0; ch < 2; ch++)
        f.clr[ch] = {bits(lo, 13 + 2 * ch, 2), bits(a, 14 + 4 * ch, 3), 4, 1};
    const u32 tc[8][3] = {
        {bits(a, 21, 1), bits(a, 22, 3), bits(a, 25, 5)}, {bits(b, 0, 1), bits(b, 1, 3), bits(b, 4, 5)},
        {bits(b, 9, 1), bits(b, 10, 3), bits(b, 13, 5)},  {bits(b, 18, 1), bits(b, 19, 3), bits(b, 22, 5)},
        {bits(b, 27, 1), bits(b, 28, 3), bits(c, 0, 5)},  {bits(c, 5, 1), bits(c, 6, 3), bits(c, 9, 5)},
        {bits(c, 14, 1), bits(c, 15, 3), bits(c, 18, 5)}, {bits(c, 23, 1), bits(c, 24, 3), bits(c, 27, 5)},
    };
    for (int i = 0; i < 8; i++) {
        f.tex[i] = {bits(hi, 2 * i, 2), tc[i][1], tc[i][0] ? 2u : 1u, 1};
        f.tex[i].scale = tc[i][1] == 4 ? 1.0f : 1.0f / (f32)(1u << tc[i][2]);
    }
    return f;
}

// the CP registers the pre-primitive list sets (and the primitive list, should it)
struct Cp {
    u32 r[256] = {};
    bool set[256] = {};
};

// array element `index` of attribute `attr` (0 position, 1 normal, 2-3 colours, 4-11 texcoords)
const u8* element(const Cp& cp, u32 attr, u32 index) {
    static const u8 zero[64] = {};
    const u32 base = cp.r[0xA0 + attr], stride = cp.r[0xB0 + attr];
    const u8* h = ::guest::host_ptr(0x80000000u | ((base + index * stride) & 0x1FFFFFFF));
    return h ? h : zero;
}

// where an attribute's data is for the stream vertex at `v` (offset `off` in it), and how many
// stream bytes the attribute takes
const u8* attr_data(const Cp& cp, const Attr& a, u32 attr, const u8* v, u32 off) {
    if (a.mode == 1)
        return v + off;
    return element(cp, attr, a.mode == 2 ? v[off] : be16(v + off));
}
u32 attr_stream_bytes(const Attr& a, u32 direct_bytes) {
    return a.mode == 0 ? 0 : a.mode == 1 ? direct_bytes : a.mode == 2 ? 1 : 2;
}

// one stream vertex at `v`; returns its size
u32 read_vertex(const Cp& cp, const Format& f, const u8* v, ShapeVertex& o) {
    memset(&o, 0, sizeof(o));
    o.pnmtx = 0xFF;
    memset(o.texmtx, 0xFF, sizeof(o.texmtx));
    u32 off = 0;
    if (f.pnmtx)
        o.pnmtx = v[off++];
    for (int i = 0; i < 8; i++)
        if (f.texmtx[i])
            o.texmtx[i] = v[off++];
    if (f.pos.mode) {
        ::guest::gx_read_comps(attr_data(cp, f.pos, 0, v, off), f.pos.type, f.pos.n, f.pos.scale, o.pos);
        off += attr_stream_bytes(f.pos, f.pos.n * comp_bytes(f.pos.type));
    }
    if (f.nrm.mode) {
        f32 nbt[9];
        ::guest::gx_read_comps(attr_data(cp, f.nrm, 1, v, off), f.nrm.type, 3, f.nrm.scale, nbt);
        memcpy(o.nrm, nbt, sizeof(o.nrm));
        off += f.nrm.mode == 1 ? f.nrm.n * comp_bytes(f.nrm.type)
                               : attr_stream_bytes(f.nrm, 0) * ((f.nbt && f.nrm3) ? 3 : 1);
    }
    for (int ch = 0; ch < 2; ch++)
        if (f.clr[ch].mode) {
            ::guest::gx_read_color(attr_data(cp, f.clr[ch], 2 + ch, v, off), f.clr[ch].type, o.clr[ch]);
            off += attr_stream_bytes(f.clr[ch], k_color_bytes[f.clr[ch].type & 7]);
        }
    for (int i = 0; i < 8; i++)
        if (f.tex[i].mode) {
            ::guest::gx_read_comps(attr_data(cp, f.tex[i], 4 + i, v, off), f.tex[i].type, f.tex[i].n, f.tex[i].scale,
                                   o.uv[i]);
            off += attr_stream_bytes(f.tex[i], f.tex[i].n * comp_bytes(f.tex[i].type));
        }
    return off;
}

// a display list as bytes on the host (guest memory is one block a page: a list lies in one)
const u8* list_bytes(u32 addr, u32 size) {
    const u8* p = ::guest::host_ptr(addr);
    return p && ::guest::host_ptr(addr + size - 1) == p + size - 1 ? p : nullptr;
}

}  // namespace

DecodedShape decode_shape(u32 s, const ArrayOverride* arrays) {
    DecodedShape out;
    Cp cp;
    // the pre-primitive list: CP (VCD, VAT, the arrays' bases and strides), XF 0x1008
    const u32 pre = shp::dl_addr(s, shp::PRE_PRIM), pre_n = shp::dl_size(s, shp::PRE_PRIM);
    const u8* p = pre ? list_bytes(pre, pre_n) : nullptr;
    if (!p)
        return out.error = "no pre-primitive list", out;
    for (u32 at = 0; at < pre_n;) {
        const u8 cmd = p[at];
        if (cmd == 0x00) {
            at++;
        } else if (cmd == 0x08 && at + 6 <= pre_n) {
            cp.r[p[at + 1]] = be32(p + at + 2), cp.set[p[at + 1]] = true;
            at += 6;
        } else if (cmd == 0x10 && at + 5 <= pre_n) {
            const u32 n = bits(be32(p + at + 1), 16, 4) + 1;
            at += 5 + 4 * n;
        } else {
            return out.error = "an unexpected command in the pre-primitive list", out;
        }
    }
    if (!cp.set[0x50] || !cp.set[0x60])
        return out.error = "no vertex descriptor", out;
    for (u32 a = 0; arrays && a < 4; a++)
        if (arrays[a].base)
            cp.r[0xA0 + a] = arrays[a].base, cp.r[0xB0 + a] = arrays[a].stride;
    out.vcd_lo = cp.r[0x50], out.vcd_hi = cp.r[0x60];

    // the primitive list: draws, and matrices loaded by index
    const u32 prim = shp::dl_addr(s, shp::PRIM), prim_n = shp::dl_size(s, shp::PRIM);
    p = prim ? list_bytes(prim, prim_n) : nullptr;
    if (!p)
        return out.error = "no primitive list", out;
    Format fmt[8];
    bool fmt_made[8] = {};
    u32 loads_since = 0;
    for (u32 at = 0; at < prim_n;) {
        const u8 cmd = p[at];
        if (cmd == 0x00 || cmd == 0x44 || cmd == 0x48) {
            at++;
        } else if (cmd == 0x08 && at + 6 <= prim_n) {
            cp.r[p[at + 1]] = be32(p + at + 2), cp.set[p[at + 1]] = true;
            for (bool& m : fmt_made)
                m = false;
            at += 6;
        } else if (cmd == 0x10 && at + 5 <= prim_n) {
            at += 5 + 4 * (bits(be32(p + at + 1), 16, 4) + 1);
        } else if (cmd >= 0x20 && cmd <= 0x38 && (cmd & 7) == 0 && at + 5 <= prim_n) {
            const u32 v = be32(p + at + 1);
            if (cmd == 0x38)
                return out.error = "a light loaded by index", out;
            out.loads.push_back({(u16)bits(v, 0, 12), (u8)(bits(v, 12, 4) + 1), (u8)((cmd - 0x20) / 8), (u16)(v >> 16)});
            loads_since++;
            at += 5;
        } else if (cmd >= 0x80 && cmd < 0xC0 && at + 3 <= prim_n) {
            const u32 vat = cmd & 7, count = be16(p + at + 1);
            at += 3;
            if (!cp.set[0x70 + vat])
                return out.error = "a draw with a vertex format the shape didn't set", out;
            if (!fmt_made[vat])
                fmt[vat] = format(cp.r[0x50], cp.r[0x60], cp.r[0x70 + vat], cp.r[0x80 + vat], cp.r[0x90 + vat]),
                fmt_made[vat] = true;
            ShapePrim pr;
            pr.type = cmd & 0xF8, pr.vat = (u8)vat, pr.first = (u32)out.verts.size(), pr.count = count;
            pr.loads_first = (u32)out.loads.size() - loads_since, pr.n_loads = loads_since;
            loads_since = 0;
            out.verts.resize(out.verts.size() + count);
            for (u32 i = 0; i < count; i++) {
                if (at >= prim_n)
                    return out.error = "a draw runs past its list", out;
                at += read_vertex(cp, fmt[vat], p + at, out.verts[pr.first + i]);
            }
            if (at > prim_n)
                return out.error = "a draw runs past its list", out;
            out.prims.push_back(pr);
            const Format& f = fmt[vat];
            out.attrs |= (f.pnmtx ? 1u : 0) | (f.pos.mode ? 1u << 9 : 0) | (f.nrm.mode ? 1u << 10 : 0);
            for (int i = 0; i < 8; i++)
                out.attrs |= (f.texmtx[i] ? 2u << i : 0) | (f.tex[i].mode ? 1u << (13 + i) : 0);
            for (int ch = 0; ch < 2; ch++)
                out.attrs |= f.clr[ch].mode ? 1u << (11 + ch) : 0;
            out.nbt |= f.nbt;
        } else {
            return out.error = "an unexpected command in the primitive list", out;
        }
    }
    return out;
}

}  // namespace engine::g3d::live
