// The household's Mii on Wuhu Island (docs/wuhu_design.md §0.2, §5): drawn by the channel's own g3d
// (the room's actor manager: its body, its RFL face, the room's animation files, its shadow) in a
// ScnRoot of its own that holds nothing else -- the island is drawn natively, between the Mii's
// opaque and translucent passes, in the same depth buffer (engine/g3d/native_draw.h) -- and moved
// by native code (engine/wuhu/player.h, vehicles.h): every frame it stands where the player is (times
// k: Wii no Ma's units) or sits where the vehicle's driver does, turned its way, and plays the clip
// what it is doing asks for through a SpecialAnim behaviour, the clip's frame set natively: a gait's
// by the distance gone (the planted foot keeps to the ground at any speed), a jump's by its rise and
// fall, a steering pose's by the stick, the rest's by the time the frame took. Its camera is the
// island's, scaled by k.
//
// The clips (island_mii.cpp's table) are the room's own -- Move, Wait, Vote (the landings),
// Send_DSi (a jump) and Sit_Sofa from the channel's content -- or, where the island's clip archive
// is staged (DATA/wuhu/mii_anim.brres, made by tools/wuhu/mii_anims.py from your own Wii no Ma and
// Wii Sports Resort: WSR's parachute, fall, pilot and jet-ski poses moved onto Wii no Ma's body, the
// jump and landings without their own rise, a swim stroke and treading water), the archive's, given
// to the actor manager as one more animation file ("Island") in place of Vote, Send_DSi and
// Sit_Sofa.
//
// Its guest objects are made a step a frame while the island loads (make_step) and freed in the
// reverse order by the destructor: the channel's heaps are stacks.
#pragma once

#include <vector>

#include "engine/g3d/world.h"
#include "engine/wuhu/player.h"
#include "runtime/runtime.h"

namespace engine::wuhu {

// What the Mii is doing this frame, for its animation: island.cpp makes it from the player or from
// the vehicle driven. Distances are guest units (Wii no Ma's: a Mii of height scale 1 is 158 tall).
struct MiiMotion {
    enum Kind : u8 {
        GROUND,  // standing, walking or running (a step down short of a fall too)
        AIR,     // a jump, a fall off a ledge, a leap off one at a sprint
        SLIDE,   // down a slope too steep to stand on
        GLIDE,   // under the parachute (Player::gliding)
        SWIM,
        CAR,     // in a car's driving seat
        BOAT,    // at a boat's helm
    };
    Kind kind = GROUND;
    u32 frames = 1;          // the sixtieths of a second this update covers (the island's FrameClock)
    f32 walked = 0;          // along the ground or the water this update
    f32 turned = 0;          // the facing's turn this update (degrees)
    bool sprint = false;     // the sprint button held
    bool jump = false;       // AIR: a jump (else a fall or a leap)
    bool leap = false;       // AIR: off a ledge at a sprint (it may drop any height)
    f32 rise = 0;            // AIR: vertical speed over a jump's take-off speed (1 taking off, < 0 down)
    f32 air_time = 0;        // AIR: seconds since the feet left the ground
    f32 depth = 0;           // SWIM: the water's depth over the feet
    f32 steer = 0;           // CAR, BOAT: the stick's x, -1 (left) .. 1 (right)
};

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

    // The frame: the camera (guest units: engine::g3d::scaled), the body's feet where the player
    // stands or the driver sits (guest units) facing yaw (nw4r degrees), doing `m`; the actor
    // manager's update, the clip's frame, the root's calc.
    void update(const engine::g3d::Camera& camera, engine::math::Vec3 at, f32 yaw, const MiiMotion& m);
    // The draw. view() first: the camera's viewport, projection, scissor and scissor offset into GX
    // (engine/ui/g3d.h camera_load_view), which the native draws before the opaque pass take; then
    // the opaque pass, the translucent one.
    void view();
    void draw_opaque();
    void draw_translucent();

    // The clips: what each is and how its frame goes is island_mii.cpp's table.
    enum Clip : u8 {
        IDLE, FIDGET_A, FIDGET_B, FIDGET_C, FIDGET_D,  // Wait, and a pass of Wait_A..D now and then
        TURN,                                          // turning on the spot
        WALK, WALK_FAST, RUN, SPRINT,                  // the gaits, by speed
        JUMP,                                          // a jump: take-off, rise, apex, fall, landing
        DROP,                                          // a fall off a ledge, then its landing
        FALL,                                          // a long fall (or a leap)
        SLIDE, GLIDE, SWIM, TREAD, DRIVE, HELM,
        N_CLIPS
    };
    struct Source;

private:
    // the block, the root, the Mii shadow, the actor manager, the island's archive, the Mii
    static constexpr u32 STEPS = 6;
    void resolve_clips();
    void set_clip(Clip c, f32 fade);
    void choose(const MiiMotion& m);
    Clip gait_for(f32 speed, bool sprint) const;
    void move_gait(Clip c, f32 walked, u32 frames);
    void add_archive();
    void write_camera(const engine::g3d::Camera& c);
    void measure_feet(engine::math::Vec3 at, f32 walked);

    u32 step_ = 0;
    u32 block_ = 0;
    // in the block: the 3D sound, the Mii shadow's resource, the 3D sound handles' list, the actor
    // manager, the root's word, the animation file list, the face's expression, the RFLCreateID, the
    // strings
    u32 sound3d_ = 0, shadow_res_ = 0, handles_ = 0, actors_ = 0, root_word_ = 0;
    u32 anim_list_ = 0, expression_ = 0, create_id_ = 0, strings_ = 0;
    std::vector<u32> name_at_;    // the clip names' guest strings (Strings indices: island_mii.cpp)
    bool have_id_ = false;
    std::vector<u8> archive_;     // DATA/wuhu/mii_anim.brres as read at step 0 (none: not staged)
    u32 archive_file_ = 0;        // its AnimFile, once the actor manager has it
    u32 actor_ = 0, behavior_ = 0;
    u32 longest_file_ = 0, longest_anim_ = 0;  // the names' std::strings are first made this long
    // each clip as resolved at spawn: its source (the archive's or the content's), names and the
    // body's animation object (0: none of its sources is loaded; IDLE's stands in)
    const Source* source_[N_CLIPS] = {};
    u32 clip_file_[N_CLIPS] = {}, clip_anim_[N_CLIPS] = {}, anim_[N_CLIPS] = {};
    // the clip playing, its frame (its own: from 0) and how long it has played (sixtieths)
    int clip_ = -1;
    f32 frame_ = 0, played_ = 0;
    f32 fade_ = -1;               // the crossfade to write after the actor manager's update (sixtieths)
    f32 phase_ = 0;               // the gaits' common phase: 0..1 from the left foot's strike
    f32 idle_ = 0, turning_ = 1e6f;  // sixtieths standing still since the last fidget; since the last turn
    u32 fidgets_ = 0;
    bool landed_ = false;         // JUMP or DROP playing on from its touchdown
    f32 steer_ = 0;               // the stick's x, eased (the steering poses' axis)
    bool female_ = false;
    f32 height_scale_ = 1, aspect_ = 16.0f / 9.0f;
    u32 ticks_ = 0;               // sixtieths of a second since the Mii came (its blinks)
    u32 heap_before_ = 0;
    // NOMA_WUHU_FEET=1: the planted foot's slide over the ground, measured on the posed body (its
    // ankles' world matrices) and logged a second at a time
    bool feet_log_ = false;
    u32 ankle_[2] = {};
    bool feet_had_ = false;
    engine::math::Vec3 ankle_was_[2];
    engine::math::Vec3 at_was_;
    f32 frame_was_ = 0;
    f32 feet_slid_ = 0, feet_walked_ = 0, feet_top_ = 0, feet_body_ = 0, feet_frames_ = 0;
    u32 feet_samples_ = 0, feet_updates_ = 0;
};

}  // namespace engine::wuhu
