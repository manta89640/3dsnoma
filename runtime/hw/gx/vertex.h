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

// ---- the draw path: decoding and the transform unit in one pass, for a renderer that
// rasterizes with one color and up to three projected texture coordinates (the 3DS GPU)

// one attribute in the stream: where its data (or index) is and how to read the components
struct GxAttrFormat {
    u8 mode = 0;    // 0 not present, 1 direct, 2 8-bit index, 3 16-bit index
    u8 type = 0;    // component type (u8 s8 u16 s16 f32), or the color format
    u8 n = 0;       // components
    u8 offset = 0;  // byte offset in the stream vertex
    f32 scale = 1;  // integer components: 1 / 2^shift
};

// a vertex format (CP VCD + VAT) compiled for decoding
struct GxVertexFormat {
    u32 size = 0;                 // stream bytes per vertex
    s8 pos_mtx = -1;              // stream offsets of the matrix indices, -1: XF default
    s8 tex_mtx[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    GxAttrFormat pos, nrm, color[2], tex[8];
    bool nbt = false;             // normal, binormal and tangent
    bool nrm_index3 = false;      // ... with an index each
    u32 serial = 0;               // a new number each time a format is compiled
};
const GxVertexFormat& gx_vertex_format(u32 vat);

// what the renderer draws: a clip-space position (as GxOutVertex), color 0 as bytes, and the
// texture coordinates of up to three texgens with q divided out and a scale applied
struct GxDrawVertex {
    f32 pos[4];
    u8 color[4];
    f32 tc[3][2];
};
struct GxDrawSetup {
    s8 texgen[3] = {-1, -1, -1};  // the texgen each output coordinate comes from; -1: none
    f32 sx[3] = {1, 1, 1}, sy[3] = {1, 1, 1};
};
// Decode and transform `count` vertices of format `vat`: the same results as
// gx_decode_vertices + gx_transform (to rounding), but only the outputs the setup asks for.
void gx_transform_draw(u32 vat, u32 count, const u8* data, const GxDrawSetup& setup, GxDrawVertex* out);
// NOMA_CHECK_GX_DRAW: compare gx_transform_draw with gx_transform's results for a draw
void gx_check_transform_draw(u32 vat, u32 count, const u8* data, const GxOutVertex* ref);

}  // namespace guest
