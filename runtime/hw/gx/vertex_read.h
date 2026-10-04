// Reading vertex attributes from the command stream or the vertex arrays (big-endian guest data);
// shared by the decoder (vertex.cpp) and the draw path (xform.cpp).
#pragma once
#include <cstring>

#include "regs.h"
#include "vertex.h"

namespace guest {

static inline u16 gx_be16(const u8* p) {
    u16 v;
    memcpy(&v, p, 2);
    return __builtin_bswap16(v);
}
static inline u32 gx_be32(const u8* p) {
    u32 v;
    memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}

static inline f32 gx_be_f32(const u8* p) {
    u32 b = gx_be32(p);
    f32 v;
    memcpy(&v, &b, 4);
    return v;
}

// bytes one component of a type takes in an array element
static inline u32 gx_comp_bytes(u32 type) { return type == 2 || type == 3 ? 2 : type == 4 ? 4 : 1; }

// array element `index` of attribute `attr` (0 position, 1 normal, 2-3 colors, 4-11 texture
// coordinates) in guest memory; zeros where there is no memory
static inline const u8* gx_array_ptr(u32 attr, u32 index) {
    static const u8 zero[64] = {};
    u32 base = g_gx.cp[CP_ARRAY_BASE + attr], stride = g_gx.cp[CP_ARRAY_STRIDE + attr];
    const u8* h = host_ptr(0x80000000 | ((base + index * stride) & 0x1FFFFFFF));
    return h ? h : zero;
}

// where an attribute's data is for the stream vertex at v: in the vertex (direct), or the
// array element its 8/16-bit index names
static inline const u8* gx_attr_data(const GxAttrFormat& a, u32 attr, const u8* v) {
    if (a.mode == 1)
        return v + a.offset;
    return gx_array_ptr(attr, a.mode == 2 ? v[a.offset] : gx_be16(v + a.offset));
}

// n components of a type (u8 s8 u16 s16 f32; others read as zero bytes) times the scale
static inline const u8* gx_read_comps(const u8* p, u32 type, u32 n, f32 scale, f32* out) {
    switch (type) {
    case 0:
        for (u32 i = 0; i < n; i++)
            out[i] = (f32)p[i] * scale;
        return p + n;
    case 1:
        for (u32 i = 0; i < n; i++)
            out[i] = (f32)(s8)p[i] * scale;
        return p + n;
    case 2:
        for (u32 i = 0; i < n; i++)
            out[i] = (f32)gx_be16(p + 2 * i) * scale;
        return p + 2 * n;
    case 3:
        for (u32 i = 0; i < n; i++)
            out[i] = (f32)(s16)gx_be16(p + 2 * i) * scale;
        return p + 2 * n;
    case 4:
        for (u32 i = 0; i < n; i++) {
            u32 b = gx_be32(p + 4 * i);
            memcpy(&out[i], &b, 4);
        }
        return p + 4 * n;
    default:
        for (u32 i = 0; i < n; i++)
            out[i] = 0;
        return p + n;
    }
}

// a color in one of the vertex color formats -> RGBA bytes
static inline void gx_read_color(const u8* p, u32 fmt, u8* out) {
    switch (fmt) {
    case 0: {  // RGB565
        u16 v = gx_be16(p);
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
    case 3:  // RGBA4444
        out[0] = (u8)((p[0] >> 4) * 17);
        out[1] = (u8)((p[0] & 15) * 17);
        out[2] = (u8)((p[1] >> 4) * 17);
        out[3] = (u8)((p[1] & 15) * 17);
        break;
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

}  // namespace guest
