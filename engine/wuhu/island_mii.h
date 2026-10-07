// The household's Mii on Wuhu Island (docs/wuhu_design.md §0.2, §5): drawn by the channel's own g3d
// (the room's actor manager: its body, its RFL face, the room's animation files, its shadow) in a
// ScnRoot of its own that holds nothing else -- the island is drawn natively, between the Mii's
// opaque and translucent passes, in the same depth buffer (engine/g3d/native_draw.h) -- and walked
// by native code (engine/wuhu/player.h): every frame it stands where the player is (times k: Wii no
// Ma's units), turned its way, and plays the room's Wait, Walk or Run (a woman's Female_Walk and
// Female_Run), or Seal.lex's Jump pose, through a SpecialAnim behaviour; a walking clip's frame is
// driven by the distance walked
// so that its feet keep to the ground whatever the speed. Its camera is the island's, scaled by k.
//
// Its guest objects are made a step a frame while the island loads (make_step) and freed in the
// reverse order by the destructor: the channel's heaps are stacks.
#pragma once

#include "engine/g3d/world.h"
#include "engine/wuhu/player.h"
#include "runtime/runtime.h"

namespace engine::wuhu {

class IslandMii {
public:
    IslandMii();
    ~IslandMii();
    IslandMii(const IslandMii&) = delete;
    IslandMii& operator=(const IslandMii&) = delete;

    // the next guest object (one a frame while loading: the actor manager's step reads the Mii's
    // files, which the guest waits for); false once they are all made
    bool make_step();
    f32 progress() const;  // 0..1, of the steps
    // all made (a root to draw in, whether or not there is a Mii in it)
    bool made() const { return block_ != 0 && step_ >= STEPS; }
    // the household's first Mii is there (in the database, spawned)
    bool present() const { return actor_ != 0; }
    // what the player's measures and speeds depend on (player.h's PlayerParams::make)
    f32 height_scale() const { return height_scale_; }
    bool female() const { return female_; }
    Gait gait() const { return female_ ? GAIT_B : GAIT_A; }
    // the TV's width over height (16:9 or 4:3), the camera's aspect
    f32 aspect() const { return aspect_; }

    // The frame: the camera (guest units: engine::g3d::scaled), the body where the player stands
    // (guest units) facing yaw (nw4r degrees), the clip its state asks for, `walked` guest units on
    // (this frame's walking: the walking clips' frames go by it); then the actor manager's update and
    // the root's calc.
    void update(const engine::g3d::Camera& camera, engine::math::Vec3 at, f32 yaw, PlayerState state, f32 walked,
                f32 run_pace_multiplier = 1.0f);
    // The draw. view() first: the camera's viewport, projection, scissor and scissor offset into GX
    // (engine/ui/g3d.h camera_load_view), which the native draws before the opaque pass take; then
    // the opaque pass, the translucent one.
    void view();
    void draw_opaque();
    void draw_translucent();

private:
    static constexpr u32 STEPS = 5;
    enum Clip { WAIT, WALK, RUN, JUMP, N_CLIPS };
    void set_clip(Clip c);
    void write_camera(const engine::g3d::Camera& c);

    u32 step_ = 0;
    u32 block_ = 0;
    // in the block: the 3D sound, the Mii shadow's resource, the 3D sound handles' list, the actor
    // manager, the root's word, the animation file list, the face's expression, the RFLCreateID, the
    // strings
    u32 sound3d_ = 0, shadow_res_ = 0, handles_ = 0, actors_ = 0, root_word_ = 0;
    u32 anim_list_ = 0, expression_ = 0, create_id_ = 0, strings_ = 0;
    u32 clip_file_[N_CLIPS] = {}, clip_anim_[N_CLIPS] = {};  // guest strings
    u32 longest_file_ = 0, longest_anim_ = 0, female_walk_ = 0, female_run_ = 0;
    bool have_id_ = false;
    u32 actor_ = 0, behavior_ = 0;
    u32 anim_[N_CLIPS] = {};  // the body's animation object of each clip
    f32 frames_[N_CLIPS] = {};  // each clip's loop
    f32 frame_[N_CLIPS] = {};   // where the walking clips are (Wait plays at its own pace)
    f32 speed_[N_CLIPS] = {};   // guest units a frame at which a clip plays at its own pace
    int clip_ = -1;
    bool female_ = false;
    f32 height_scale_ = 1, aspect_ = 16.0f / 9.0f;
    u32 frame_count_ = 0;
    u32 heap_before_ = 0;
};

}  // namespace engine::wuhu
