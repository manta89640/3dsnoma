// ミドハ (Midoha), the island's first NPC: a Mii whose face is Wii Mii parts and whose head and dress
// are the port's own (docs/wuhu_design.md §9). Shown only on request while his design is being
// approved: NOMA_WUHU_MIDOHA=A|B|C (a candidate face, engine/wuhu/midoha_data.h; 1 = A).
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

}  // namespace engine::wuhu
