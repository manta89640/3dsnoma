// The GPU registers nw4r::g3d's Draw1Mat1ShpDirectly leaves for a draw, derived natively: its
// material setup (the TEV, the textures, the pixel state, the colour channels and their lights,
// the texgens and the texture matrices) and its shape setup's matrix registers. What the native
// g3d's backends draw a live model's draw with; NOMA_CHECK_G3D_LIVE compares it with the GPU's
// own registers draw by draw (engine/g3d/live_draw.cpp).
#pragma once

#include <vector>

#include "runtime/guest.h"

namespace engine::g3d::live {

// GX's register file as far as writes made it known: BP registers (with the bits a write set: a
// masked write sets only its mask's), the TEV colour and konst registers, XF registers
// (0x1000-0x105F) and XF memory (matrices 0x000-0x0FF, normal matrices 0x400-0x45F, post-transform
// matrices 0x500-0x5FF, lights 0x600-0x67F). Writes can also be logged (the check replays them).
struct GxRegs {
    u32 bp[256], bp_known[256];
    s32 tev_reg[4][4], tev_konst[4][4];
    u8 tev_reg_known[4], tev_konst_known[4];  // a bit per component (R G B A)
    u32 xf[0x60];
    bool xf_known[0x60];
    u32 xf_mem[0x700];
    bool xf_mem_known[0x700];
    u32 bp_mask;

    struct Write {
        u8 kind;  // 0 BP command word, 1 XF word
        u16 addr;
        u32 value;
    };
    std::vector<Write>* log = nullptr;

    void clear();
    void bp_write(u32 cmd);  // a BP load's word: register << 24 | value (0xFE: the next write's mask)
    void xf_write(u32 addr, u32 v);
    void apply(const Write& w) { w.kind == 0 ? bp_write(w.value) : xf_write(w.addr, w.value); }
    // a display list's BP and XF loads (CP loads and NOPs pass; anything else ends it: false)
    bool apply_dl(u32 addr, u32 size);
};

// where a material's parts come from: the resource's own, or a ScnMdl's per-instance copies (its
// DrawResMdlReplacement, by the material's id); 0 where it has none
struct MatSources {
    u32 tex_obj, tlut_obj, genmode, tev, pix_dl, tevcolor_dl, ind_dl, chan, texgen_dl, misc, tex_srt;
};
MatSources mat_sources(u32 mat, u32 repl);

// G3DState's light cache as the light sets change it within a pass (nw4r_g3d_G3DState_sLightCache): the light set
// in place, its masks (colour 0, alpha 0, colour 1, alpha 1) and the GX light slot -> light map
struct LightCache {
    s32 current;
    u32 masks[4];
    s8 slots[8];
};
LightCache light_cache_now();  // as G3DState holds it now
void light_cache_store(const LightCache& lc);

// what a material's setup leaves for the draws after it (G3DState's): its gen mode, the
// texcoords whose texture matrix the shape makes from the normal matrix (1) or the position
// matrix (2), the texture maps' sizes
struct MatShared {
    u8 texgens, chans, stages, ind;
    u32 cull;
    u8 needs[8];
    u16 map_w[8], map_h[8];
};
MatShared mat_shared_now();  // as G3DState holds it now

// Draw1Mat1ShpDirectly's material setup for material `m` (its sources), as `ctrl` asks (8:
// lighting forced off; 0x10, no material: nothing): into `r`, the light cache and `sh` moved on.
// False where it can't (fur, an indirect normal map: logged once).
bool derive_material(u32 mat, const MatSources& m, u32 ctrl, LightCache& lc, MatShared& sh, GxRegs& r);
// ... and its shape setup's matrix registers for shape `shp` drawn with the view arrays: a
// single-matrix shape's matrix index registers and the texture matrices made from its normal
// matrix; a skinned shape's defaults
void derive_shape(u32 shp, u32 vpos, u32 vnrm, const MatShared& sh, GxRegs& r);

}  // namespace engine::g3d::live
