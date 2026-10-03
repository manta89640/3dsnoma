// Vertices as the GPU sees them: decoded from the command stream (vertex.cpp), then transformed,
// lit and given texture coordinates by the transform unit (xform.cpp).
#pragma once
#include "gx.h"

namespace guest {

// one vertex in model space, as the stream (or the arrays it indexes) gives it
struct GxVertex {
    f32 pos[3];
    f32 nrm[3][3];  // normal, binormal, tangent
    u8 color[2][4];  // RGBA
    f32 tex[8][2];
    u8 pos_mtx;      // matrix indices (from the vertex, or the defaults in XF)
    u8 tex_mtx[8];
};

// a vertex after the transform unit: clip-space position, lit colors, texture coordinates
struct GxOutVertex {
    f32 clip[4];      // x y z w (z in [-w, 0] between the near and far planes)
    f32 color[2][4];  // 0-255
    f32 tex[8][3];    // s t q
};

// decode `count` vertices of format `vat` from the stream at `data`
void gx_decode_vertices(u32 vat, u32 count, const u8* data, GxVertex* out);
// the transform unit
void gx_transform(const GxVertex* in, u32 count, GxOutVertex* out);

}  // namespace guest
