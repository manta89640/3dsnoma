// ミドハ (Midoha), the island's first NPC: a Mii whose face is Wii Mii parts and whose head and dress
// are the port's own (docs/wuhu_design.md §9). Shown only on request while his design is being
// approved: NOMA_WUHU_MIDOHA=A|B|C|D (a candidate face, engine/wuhu/midoha_data.h; 1 = D, the
// recommended one).
//
// His three layers:
//   * the Mii (island_mii.h): his RFLCharData given to RFL through a middle database of one of its own
//     (RFLInitMiddleDB, type 4 "user set": no Mii database, no household), spawned by the island's
//     actor manager next to the player's Mii, its Wait clip playing; bald (RFL hair 30);
//   * his head (MidohaLook): his hair, pointed ears, hair band and bead cord, a native g3d package of
//     our own (DATA/wuhu/midoha.ng3d, tools/mii/midoha_parts.py) in RFL head units, drawn by a World
//     of its own whose one instance follows his head: the matrix RFL draws his face with (his body's
//     Head_Only node, its view matrix: the head's 0.69 scale in it), times RFL's axes;
//   * his dress (MidohaOutfit, engine/wuhu/midoha_outfit.h): a garment model of our own, skinned to
//     his body's skeleton, the body's own draw off but for its hands and shoes.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "engine/g3d/world.h"
#include "engine/wuhu/midoha_data.h"
#include "runtime/runtime.h"

namespace engine::wuhu {

// the candidate NOMA_WUHU_MIDOHA asks for, or nullptr (he isn't shown)
const midoha::Candidate* midoha_candidate();

// His head: the package and its World.
class MidohaLook {
public:
    MidohaLook() {}
    ~MidohaLook();
    MidohaLook(const MidohaLook&) = delete;
    MidohaLook& operator=(const MidohaLook&) = delete;

    // reads DATA/wuhu/midoha.ng3d (NOMA_MIDOHA_PACKAGE: another file); false and the reason logged if
    // it can't be had
    bool load();
    bool loaded() const { return world_ != nullptr; }
    // the frame: `head` = RFL head units -> world (guest units); `camera` the island's, in guest units
    void update(const engine::math::Mtx34& head, const engine::g3d::Camera& camera, f32 frame);
    void emit(engine::g3d::Pass pass);

private:
    std::unique_ptr<engine::g3d::Package> package_;
    std::unique_ptr<engine::g3d::World> world_;
    bool prepared_ = false, translucent_ = false;  // (no translucent pieces: no packet for that pass)
};

// His face's parts, painted after the concept (tools/mii/midoha_face.py -> DATA/wuhu/midoha_face.bin:
// eyes, brows and mouth as a Mii's face texture holds them, our own art): written over the face
// textures RFL made for him from his Wii parts, so that RFL draws them on his face as it draws any
// Mii's (the same mask, the same blending over the skin). RFL keeps a face texture for each of its
// seven expressions in his char model's buffer (RFLiInitCharModel, fn_804C83F0: the char model at
// his actor +0x44, its +0x6C..+0x84 the expressions' GXTexObj*, RGB5A3 at the actor manager's face
// resolution); the file has an image for each (the blink's eyes closed). RFL makes its textures
// through the EFB a frame or more after his actor is made (an EFB copy, which the 3DS's renderer
// reads back from its 400x240 target: a 256 face would come out at about half that), so the images
// are checked every frame (a few words) and written again whenever RFL's have come in over them.
// NOMA_MIDOHA_FACE=0: RFL's own face (his Wii parts); =path: another file.
// NOMA_MIDOHA_FACE_DUMP=dir: RFL's own textures written there once (midoha_face_rfl_<expression>.bin:
// "MDFD", u16 width, height, format, then the texels as GX lays them out).
class MidohaFace {
public:
    bool load();
    bool loaded() const { return !images_.empty(); }
    // the frame: his actor's face textures checked, ours written over any that aren't ours
    void apply(u32 actor);
    // RFL's own textures (before apply) into `dir`, once they are made (false: not yet)
    static bool dump(u32 actor, const std::string& dir);

private:
    static constexpr u32 N_EXPRESSIONS = 7;
    u16 width_ = 0, height_ = 0;
    u32 image_bytes_ = 0;
    std::vector<u8> images_;           // GX RGB5A3, image_bytes_ each
    u8 image_of_[N_EXPRESSIONS] = {};  // expression -> image
    u32 writes_ = 0;
    bool warned_ = false;
};

}  // namespace engine::wuhu
