// A ResShp of the channel's models decoded natively (engine/g3d/live_draw.cpp): the vertex format
// its pre-primitive display list sets (VCD, VAT, the arrays), its primitive display list's draws
// with their vertices as the arrays give them, and the matrices the list loads by index between
// draws (a skinned shape takes its matrices from the view arrays that way).
#pragma once

#include <vector>

#include "runtime/guest.h"

namespace engine::g3d::live {

// a vertex as the primitive list gives it, in model space (the arrays' values, scaled)
struct ShapeVertex {
    f32 pos[3];
    f32 nrm[3];
    u8 clr[2][4];  // RGBA
    f32 uv[8][2];
    u8 pnmtx;      // the position matrix's row (PNMTXIDX), or 0xFF: the default (XF's)
    u8 texmtx[8];  // TEXnMTXIDX, or 0xFF: the default
};

// a matrix the primitive list loads from a view array by index (LOAD_INDX_A/B/C): the XF address
// and words it fills, the array (0 the view position matrices, 1 the normal ones, 2 the texture
// ones) and the matrix's index in it
struct ShapeMtxLoad {
    u16 xf_addr;
    u8 words;
    u8 array;
    u16 index;
};

struct ShapePrim {
    u8 type;  // GX primitive: 0x80 quads, 0x90 triangles, 0x98 strip, 0xA0 fan, 0xA8 lines, 0xB0 line strip, 0xB8 points
    u8 vat;
    u32 first, count;          // its vertices in DecodedShape::verts
    u32 loads_first, n_loads;  // the matrix loads since the draw before it
};

struct DecodedShape {
    const char* error = nullptr;  // why it couldn't be decoded (nullptr: decoded)
    u32 vcd_lo = 0, vcd_hi = 0;
    u32 attrs = 0;  // GX attributes present: bit 0 PNMTXIDX, 1-8 TEXnMTXIDX, 9 position, 10 normal, 11-12 colours, 13-20 texcoords
    bool nbt = false;
    std::vector<ShapeVertex> verts;
    std::vector<ShapePrim> prims;
    std::vector<ShapeMtxLoad> loads;
};

// an array a draw reads in place of the shape's own (the GXSetArray before its draw): a fur
// layer's positions, a ScnMdl's own vertex buffers. base 0: the shape's own.
struct ArrayOverride {
    u32 base = 0, stride = 0;
};
// decode ResShp `shp` (guest address): its two display lists and the arrays they read, with
// `arrays` (position, normal, colour 0, colour 1) in place of its own where given
DecodedShape decode_shape(u32 shp, const ArrayOverride* arrays = nullptr);

}  // namespace engine::g3d::live
