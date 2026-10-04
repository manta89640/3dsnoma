// The transform unit's work for a GPU that runs it as a vertex shader (platform/3ds/shaders/gx.v.pica,
// drawn by platform/3ds/gx_pica.cpp): the CPU decodes the stream and follows the indices into the
// vertex arrays, and hands the shader model-space vertices, the matrix rows they index (a palette
// of uniforms), and the rest of the GX state a draw needs as uniforms. xform.cpp's gx_transform
// stays the reference: NOMA_CHECK_GX_DRAW=pica runs the shader itself on the host against it.
#pragma once
#include <cstddef>
#include <cstring>

#include "vertex.h"

namespace guest {

// a vertex as the shader takes it (one attribute buffer's layout: what stays the same for a
// vertex of a display list from one frame to the next)
struct GxTnlVertex {
    f32 pos[3];     // v0: model space
    f32 nrm[3];     // v1: model space (zeros when the draw has no use for it)
    u8 color[4];    // v2: color 0, RGBA 0-255 (zeros when the vertex has none)
    f32 tc[3][3];   // v4-v6: the texgen sources of texture units 0-2: (s, t, 1), position or normal (x, y, z or 1)
};
static_assert(sizeof(GxTnlVertex) == 64, "GxTnlVertex layout");
// its palette slots (v3, a second attribute buffer): position matrix (its normal matrix at +3),
// texgen 0-2 matrices; written again when the palette gives the matrices other slots
struct GxTnlSlots {
    u8 s[4];
};
// the XF rows its matrices come from as the stream names them (per vertex: a draw with matrix
// indices in its vertices); the draw's defaults otherwise
struct GxTnlRows {
    u8 r[4];
};

// the shader's bool uniforms (b0-b13)
enum : u32 {
    TNL_LIT = 1u << 0,        // a channel is lit
    TNL_HASLIGHTS = 1u << 1,  // ... with lights (else ambient only)
    TNL_SPOT = 1u << 2,       // spot attenuation (else none)
    TNL_STQ0 = 1u << 3,       // divide texgen k by q
    TNL_POST0 = 1u << 6,      // texgen k has a post matrix
    TNL_PNRM0 = 1u << 9,      // ... normalised before it
    TNL_TG1 = 1u << 12,       // texture units 1 and 2 are used
    TNL_TG2 = 1u << 13,
};

// the shader's uniforms but the palette, as a draw sets them (float uniform registers c0-c52)
struct GxTnlConsts {
    f32 proj[4][4];          // c0-c3: GX projection x the platform's clip-space transform
    f32 matc[4], ambc[4];    // c4-c5: material and ambient color 0 from the registers (0 where the vertex's)
    f32 selmat[4], selamb[4], sellit[4];  // c6-c8: 1 per component: from the vertex, from the vertex, lit
    f32 lcfg[4];             // c9: diffuse floor, diffuse on, 1 - on, alpha light weight
    f32 tcscale[2][4];       // c10-c11: (s, t) scales of texture units 0, 1 | 2
    f32 lights[8][4][4];     // c12-c43: the lights the draw uses, 4 rows each
    f32 post[3][3][4];       // c44-c52: post matrices of texture units 0-2
    u32 nlights;             // i0: lights (TNL_HASLIGHTS)
    u32 bools;               // TNL_*
};
const u32 TNL_CONST_VECTORS = 53;  // c0-c52 as the struct lays them out
static_assert(offsetof(GxTnlConsts, nlights) == TNL_CONST_VECTORS * 16, "GxTnlConsts layout");

// The palette: matrix rows copied from XF memory into uniform registers c53 on, found again by the
// rows they came from. Slots a pending batch's vertices refer to (used) are kept until the batch
// is submitted (flushed); the renderer uploads the slots written since it last did (dirty).
const u32 TNL_PAL_BASE = 53, TNL_PAL_SLOTS = 42;
struct GxTnlPalette {
    f32 rows[TNL_PAL_SLOTS][4] = {};
    u64 used = 0, dirty = 0;  // bit per slot
    // entries: runs of slots with the same source
    struct Entry {
        u8 first = 0, n = 0;   // slots
        u8 kind = 0;           // 0 free; else TNL_PAL_* below
        u8 xf_row = 0;         // XF matrix memory row (position/texture matrices), normal matrix index
        u32 mem_serial = 0;    // g_gx.xf_mem_serial when the rows were checked against XF memory
        u32 age = 0;           // allocation order (the oldest unused entry gives way first)
    };
    Entry entries[TNL_PAL_SLOTS / 3];
    u8 slot_entry[TNL_PAL_SLOTS];  // slot -> entry index, 0xFF: none
    u8 by_row[3][64];              // (kind - 1, XF row) -> entry index, 0xFF: none
    u32 clock = 0;
    GxTnlPalette() {
        memset(slot_entry, 0xFF, sizeof(slot_entry));
        memset(by_row, 0xFF, sizeof(by_row));
    }
    // the pending batch was submitted: every slot may be reused
    void flushed() { used = 0; }
};

// A draw as planned for the shader: the constants, and what the vertices need.
struct GxTnlPlan {
    GxTnlConsts consts;
    const GxVertexFormat* fmt = nullptr;
    u8 def_pos_index = 0;           // position matrix row, when the vertex has no index
    bool need_nrm = false;          // the normal is read (lit with lights, or a texgen's source)
    bool nrm_eye = false;           // ... the normal matrix too (lit with lights)
    // texture units 0-2: their texgen's source and matrix
    struct Tc {
        u8 src = 0;         // TNL_SRC_*
        u8 tex = 0;         // ... texture coordinate (src TNL_SRC_TEX)
        bool z = false;     // the source's third component (else 1)
        bool stq = false;   // 3x4 matrix (else 2x4)
        s8 mtx_off = -1;    // stream offset of the matrix index, -1: the default
        u8 def_index = 0;   // default matrix row
    } tc[3];
    u32 approximated = 0;  // what the shader doesn't do (counted; the draw is still made)
    u32 serial = 0;        // a new number each time a plan is made: the same serial, the same constants
};
enum : u8 { TNL_SRC_ZERO = 0, TNL_SRC_POS, TNL_SRC_NRM, TNL_SRC_TEX };

// Plan a draw of format `vat` for a renderer sampling `setup`'s texgens; `clip` maps GX clip
// space to the platform's (row-major 4x4; the identity for a check against gx_transform).
// setup_serial and clip_serial are the caller's: moved on whenever its setup or clip changed, so
// that a plan is found again by numbers alone. The plan is kept (and returned again) while the
// state it was made from stays; it is valid until the next call.
const GxTnlPlan& gx_tnl_plan(u32 vat, const GxDrawSetup& setup, u32 setup_serial, const f32 clip[4][4],
                             u32 clip_serial);
u32 gx_tnl_plan_misses();  // plans made so far (the rest were found again)

// Build the vertices [first, first + n) of a draw at `data` into `out` and `slots` (and their
// matrix rows into `rows`, when given), allocating palette slots as the vertices need them.
// Returns how many were built: fewer than n when the palette has no room for the next vertex's
// matrices (the renderer submits what it has, calls pal.flushed() and goes on from there; 0 after
// a flush means the vertex alone needs more than the palette holds, which no GX format does).
u32 gx_tnl_vertices(const GxTnlPlan& plan, const u8* data, u32 first, u32 n, GxTnlPalette& pal, GxTnlVertex* out,
                    GxTnlSlots* slots, GxTnlRows* rows);
// The slots of vertices built before (a display list's), from their rows: the same returns.
u32 gx_tnl_slots(const GxTnlPlan& plan, const GxTnlRows* rows, u32 n, GxTnlPalette& pal, GxTnlSlots* slots);
// whether the plan's vertices name their matrices (else gx_tnl_fixed_slots gives every vertex's)
bool gx_tnl_rows_vary(const GxTnlPlan& plan);
bool gx_tnl_fixed_slots(const GxTnlPlan& plan, GxTnlPalette& pal, const GxTnlSlots* hint, GxTnlSlots* out);

// the guest memory a draw's indexed attributes read: for a renderer keeping its vertices
struct GxTnlArrayRange {
    u32 attr, addr, size;
};
u32 gx_tnl_array_ranges(const GxVertexFormat& f, const u8* data, u32 count, GxTnlArrayRange out[12]);

// the number of draws planned with something the shader doesn't do (specular attenuation, a
// differently lit alpha channel, emboss/color/binormal texgens), for a log line
u32 gx_tnl_approximated();

// NOMA_CHECK_GX_DRAW=pica (the host's reference renderer): the draw through the shader binary
// against gx_transform's results
void gx_check_tnl_draw(u32 vat, u32 count, const u8* data, const GxOutVertex* ref);

}  // namespace guest
