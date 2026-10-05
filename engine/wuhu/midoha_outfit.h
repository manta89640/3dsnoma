// ミドハ's hanbok: a garment model of our own, skinned to his Mii body's skeleton and drawn through
// GX as the Mii body itself draws (docs/wuhu_design.md §9). The body's own draw is turned off; of
// the body only what shows is drawn: the hands (skin) and the shoes, from its own display lists.
//
// The garment (DATA/wuhu/midoha_outfit.bin: tools/mii/midoha_outfit_model.py makes the mesh,
// tools/mii/midoha_outfit_paint.py paints its texture from the concept, tools/mii/midoha_outfit.py
// packs both; the file's layout is in that tool) is ~3,800 triangles in its bind pose (the body's
// T-pose, model space), each vertex with at most 4 weights (eighths) over 13 of the body's nodes.
// The data, in the form a native skinned path (matrix palettes) can take as it is:
//   * bones: the Mii body's node names, each with its inverse bind matrix (model space -> the
//     node's space in the bind pose; the bind pose is the body's own, ResNodeData +0x70);
//   * weight sets: the distinct (bone, weight) lists, at most 4 each (81 of them);
//   * vertices: position, normal, texture coordinate (the bind pose), each with one set;
//   * batches: the triangles in groups using at most 4 sets each (what the 3DS GX path's matrix
//     palette holds whole beside the rest, so that the list cache keeps them), with the (slot, set) loads before
//     each (the slots keep what earlier batches loaded).
// Each frame (draw): for each bone its view matrix (the body's view matrix array, by its matrix id:
// ScnMdlSimple +0xF4, made by the root's calc), times its inverse bind; for each set the weighted
// sum of its bones' (the envelope), its normal matrix (the inverse transpose) and its texture
// matrix for the Mii's sheen; then each batch's loads (GXLoadPosMtxImm / GXLoadNrmMtxImm /
// GXLoadTexMtxImm into slot k: GX_PNMTX0 + 3k, GX_TEXMTX0 + 3k) and its display list, whose
// vertices name their slot (PNMTXIDX, TEX0MTXIDX).
//
// Lit as the Mii body is: unlit, the material colour the Mii light's tint (mii_ModelRes_sInstance
// +0x34) times our texture (its painted shading), plus the body's own sheen (its material's
// SpecMapI8, mapped from the view-space normal through the env camera projection, as the body's
// texture coordinate 0 is), at 60% of the body's. The texture has four levels (1024 down to 128
// square, GX RGB565); the renderers sample level 0 only (no mipmaps), so the level is chosen per
// frame from his size on screen (a level's texel to about a pixel).
#pragma once

#include <vector>

#include "engine/math/vecmath.h"
#include "runtime/runtime.h"

namespace engine::wuhu {

class MidohaOutfit {
public:
    MidohaOutfit() {}
    ~MidohaOutfit();
    MidohaOutfit(const MidohaOutfit&) = delete;
    MidohaOutfit& operator=(const MidohaOutfit&) = delete;

    // reads the garment (NOMA_MIDOHA_OUTFIT: another file) and splits the body `scn_mdl`'s (his
    // ScnMdl's) display lists for its hands and shoes; false if either can't be had (he then keeps
    // the body's own draw). Turns the body's own opaque draw off.
    bool make(u32 scn_mdl);
    bool made() const { return block_ != 0; }
    // the body dressed: after the root's opaque pass, under its camera's projection (the body's
    // view matrices are this frame's); fovy_deg the camera's, for the texture's level
    void draw(u32 scn_mdl, f32 fovy_deg);

private:
    struct Set {
        u8 bone[4];
        f32 w[4];
    };
    struct Batch {
        u32 dl, size;
        u16 n_loads, first_load;
    };
    struct Load {
        u8 slot;
        u16 set;
    };
    // a body shape's lists: its own vertex format list, the hands' and the shoes' triangles
    struct BodyList {
        u32 setup = 0, setup_size = 0;
        u32 dl[2] = {0, 0}, size[2] = {0, 0};
        s32 mtx_id = -1;  // a shape with one matrix (no matrix index per vertex)
        u32 pos = 0, pos_stride = 0, nrm = 0, nrm_stride = 0;
    };
    bool make_body_lists(u32 res_mdl, std::vector<std::vector<u8>>* dls);
    void draw_body(u32 view_pos, u32 n_mtx, u32 tint);
    void draw_garment(u32 tint, u32 level);

    std::vector<engine::math::Mtx34> inv_bind_;
    std::vector<s32> bone_mtx_;  // each bone's matrix id in the body's view matrix array
    s32 chest_mtx_ = -1;         // (the chest's: his distance, for the texture's level)
    std::vector<Set> sets_;
    std::vector<Batch> batches_;
    std::vector<Load> loads_;
    std::vector<engine::math::Mtx34> skin_;  // (per frame) each bone's view matrix x inverse bind
    u32 block_ = 0;                          // guest: arrays, lists, textures, objects, scratch
    u32 pos_ = 0, nrm_ = 0, uv_ = 0, dl_ = 0;
    u32 tex_obj_ = 0, n_levels_ = 0, tex_size_ = 0;
    f32 density_ = 0;                         // texels a Mii unit at level 0
    u32 set_mtx_ = 0;                         // scratch: per set position, normal, texture matrices
    u32 body_nrm_ = 0, body_tex_ = 0;         // scratch: the body's normal and sheen matrices by id
    u32 consts_ = 0;                          // scratch: the env post matrix, colours
    u32 spec_obj_ = 0;                        // the body material's sheen texture object (guest's)
    BodyList body_[4];
    u32 n_body_ = 0;
    u32 n_tris_ = 0;
    // the cost: host time of the draws, logged every LOG_EVERY frames
    u64 cost_us_ = 0, matrix_us_ = 0;
    u32 cost_frames_ = 0, level_ = 0;
};

}  // namespace engine::wuhu
