// The channel's own nw4r::g3d resources in guest memory, as its draw path reads them (NW4R G3D,
// Dec 22 2009): the model (MDL0) with its dictionaries, nodes, materials, TEV and shapes, and the
// scene objects' per-instance replacement buffers. Big-endian; offsets in bytes. A handle is the
// guest address of the resource's data (0: none), as nw4r's ResXxx classes hold one pointer.
// The draw path these come from: docs and names in engine/g3d/live_draw.cpp.
#pragma once

#include "engine/math/vecmath.h"
#include "runtime/guest.h"

namespace engine::g3d::live {

using ::guest::load16;
using ::guest::load32;
using ::guest::load8;
inline f32 loadf(u32 a) { return engine::math::load_f32(a); }

// an s32 offset field at `field`, relative to `base` (0: none)
inline u32 rel(u32 base, u32 field) {
    const s32 o = (s32)load32(field);
    return o ? base + (u32)o : 0;
}

// ResDic at `dic`: entry i's data (i from 0; the root entry comes first, then 16 bytes an entry:
// ref, flag, left, right, the name's and the data's offsets from `dic`)
inline u32 dic_count(u32 dic) { return dic ? load32(dic + 4) : 0; }
inline u32 dic_data(u32 dic, u32 i) { return dic ? rel(dic, dic + (i + 1) * 16 + 0x14) : 0; }

// ---- ResMdl (MDL0)
namespace mdl {
const u32 BYTECODES = 0x10, NODES = 0x14, MATS = 0x30, SHAPES = 0x38, INFO = 0x4C;
inline u32 dic(u32 m, u32 which) { return rel(m, m + which); }
inline u32 node(u32 m, u32 i) { return dic_data(dic(m, NODES), i); }
inline u32 material(u32 m, u32 i) { return dic_data(dic(m, MATS), i); }
inline u32 shape(u32 m, u32 i) { return dic_data(dic(m, SHAPES), i); }
inline u32 n_nodes(u32 m) { return dic_count(dic(m, NODES)); }
inline u32 n_materials(u32 m) { return dic_count(dic(m, MATS)); }
inline u32 n_shapes(u32 m) { return dic_count(dic(m, SHAPES)); }
// the model info: the number of view matrices (the CalcView arrays' length)
inline u32 n_view_mtx(u32 m) { return load32(m + INFO + 0x1C); }
}  // namespace mdl

// ---- ResNode
namespace node {
const u32 VISIBLE = 0x100;  // flags
inline u32 id(u32 n) { return load32(n + 0xC); }
inline u32 mtx_id(u32 n) { return load32(n + 0x10); }
inline u32 flags(u32 n) { return load32(n + 0x14); }
}  // namespace node

// ---- the draw byte code (DrawOpa / DrawXlu): 8-byte entries until op 1
namespace bc {
const u8 END = 1, DRAW = 4;
struct Draw {
    u16 mat, shp, node;
    u8 priority;
};
inline Draw draw(u32 p) {
    return {(u16)(load8(p + 1) << 8 | load8(p + 2)), (u16)(load8(p + 3) << 8 | load8(p + 4)),
            (u16)(load8(p + 5) << 8 | load8(p + 6)), load8(p + 7)};
}
}  // namespace bc

// ---- ResShp
namespace shp {
const u32 INVISIBLE = 2;  // flags
// the current matrix (a view matrix id), or -1: the vertices name theirs (PNMTXIDX), loaded by
// the primitive list
inline s32 cur_mtx(u32 s) { return (s32)load32(s + 0x8); }
inline u32 vcd_lo(u32 s) { return load32(s + 0xC); }
inline u32 vcd_hi(u32 s) { return load32(s + 0x10); }
inline u32 flags(u32 s) { return load32(s + 0x34); }
inline u32 attrs(u32 s) { return load32(s + 0x30); }  // bit n: GX attribute n (0x400 normal)
// a display list's tag at `tag` (0x18 the pre-primitive list, 0x24 the primitive list): buffer
// size, the commands' size, the list's offset from the tag
inline u32 dl_addr(u32 s, u32 tag) { return rel(s + tag, s + tag + 8); }
inline u32 dl_size(u32 s, u32 tag) { return load32(s + tag + 4); }
const u32 PRE_PRIM = 0x18, PRIM = 0x24;
inline u32 id(u32 s) { return load32(s + 0x3C); }
}  // namespace shp

// ---- ResMat
namespace mat {
const u32 XLU = 0x80000000u;  // flag
inline u32 id(u32 m) { return load32(m + 0xC); }
inline u32 flags(u32 m) { return load32(m + 0x10); }
inline u32 genmode(u32 m) { return m + 0x14; }  // ResGenMode: u8 texgens, chans, tev stages, ind; u32 cull
inline u32 misc(u32 m) { return m + 0x1C; }     // ResMatMisc: u8 zcomploc, s8 light set, s8 fog, ...
inline u32 tev(u32 m) { return rel(m, m + 0x28); }
inline u32 fur(u32 m) { return rel(m, m + 0x34); }
inline u32 dls(u32 m) { return rel(m, m + 0x3C); }  // pixel 0x00, TEV colour 0x20, ind 0xA0, texgen 0xE0
inline u32 tex_obj(u32 m) { return m + 0x40; }      // ResTexObj: u32 used maps, GXTexObj[8] (32 bytes each)
inline u32 tlut_obj(u32 m) { return m + 0x144; }    // ResTlutObj: u32 flags, GXTlutObj[8] (12 bytes each)
inline u32 tex_srt(u32 m) { return m + 0x1A8; }     // ResTexSrt
inline u32 chan(u32 m) { return m + 0x3F0; }        // ResChan[2]
}  // namespace mat

// ---- ResTev: the stage count, the texcoord -> texmap map, its display list (BP writes)
namespace tev {
inline u32 n_stages(u32 t) { return load8(t + 0xC); }
inline u8 texmap_of(u32 t, u32 texcoord) { return load8(t + 0x10 + texcoord); }
inline u32 dl(u32 t) { return t + 0x20; }
}  // namespace tev

// ---- DrawResMdlReplacement (ScnMdl +0x144): per-instance copies, indexed by ResMat id (by node
// id for the visibility); a null pointer: the resource's own
namespace repl {
const u32 VIS = 0x4, TEX_OBJ = 0x8, TLUT_OBJ = 0xC, TEX_SRT = 0x10, CHAN = 0x14, GENMODE = 0x18, MISC = 0x1C,
          PIX_DL = 0x20, TEVCOLOR_DL = 0x24, IND_DL = 0x28, TEXGEN_DL = 0x2C, TEV = 0x30;
// sizes of one material's entry in each buffer
const u32 SZ_TEX_OBJ = 0x104, SZ_TLUT_OBJ = 0x64, SZ_TEX_SRT = 0x248, SZ_CHAN = 0x28, SZ_GENMODE = 8, SZ_MISC = 0xC,
          SZ_PIX_DL = 0x20, SZ_TEVCOLOR_DL = 0x80, SZ_IND_DL = 0x40, SZ_TEXGEN_DL = 0xA0, SZ_TEV = 0x200;
}  // namespace repl

}  // namespace engine::g3d::live
