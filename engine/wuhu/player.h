// The player on Wuhu Island: the household's Mii, walked with the circle pad over the island's
// collision (engine/collision/kcl.h). Pure native gameplay: positions in island units (y up, the sea
// at y = 0), yaw in nw4r's degrees (0 faces +z, 90 faces +x); the scene turns the result into the
// Mii's guest position (times k) and its animation (state and stride phase).
//
// The walking numbers are the room's own (the Move state of room::BodyController, research port.md
// §4.4), converted with k, Wii no Ma's units per island unit (the package's Env::mii_units_per_unit);
// the body's measures (radius, knee and chest heights, step, wading depth) are fractions of the Mii's
// height in island units, so a different k (or a taller Mii) keeps the same proportions.
#pragma once

#include "engine/collision/kcl.h"

#include <cmath>
#include <utility>
#include <vector>

namespace engine::wuhu {

using engine::collision::f32;
using engine::collision::u16;
using engine::collision::u8;
using engine::math::Vec3;

// The room's walking and running speeds, in Wii no Ma units a frame at 60 Hz (a Mii is 158 units
// tall at height scale 1). room_BodyController_Move_Enter (the BodyController's Move state, made by
// room_BodyController_ChangeToMove) takes one pair or the other by the Mii's sex (room_BodyController_IsMale: its
// face's bit 31): A a man's, B a woman's.
struct Gait {
    f32 walk, run;
};
constexpr Gait GAIT_A = {1.593f, 4.31f};
constexpr Gait GAIT_B = {1.687f, 5.343f};

constexpr f32 MII_HEIGHT = 158.0f;  // Wii no Ma units, at height scale 1 (port.md §4.4)

struct PlayerParams {
    f32 height;                      // the Mii, feet to the top of its head (island units)
    f32 walk_speed, run_speed;       // island units a frame (60 Hz)
    f32 slide_speed;                 // down a slope too steep to stand on
    f32 speed_blend;                 // how much of the way to the wanted speed a frame goes
    f32 turn_rate_still, turn_rate_run;  // degrees a frame, standing and at run speed
    f32 dead_zone, run_tilt;         // stick tilt that starts walking, and running
    f32 radius, knee, chest;         // the body's two spheres: radius, centres above the feet
    f32 step_up, snap_down;          // highest step up; lowest step down before it is a fall
    f32 max_drop;                    // the deepest fall (or slide down a slope too steep to stand on)
                                     // a step may start: a deeper one is refused, as a wall would be
    f32 wade_depth;                  // deepest water the Mii walks into
    f32 walkable_ny;                 // ground at least this flat (normal y) is walked on; steeper slid down
    f32 wall_ny;                     // steeper than this (and overhangs) is a wall
    f32 gravity, max_fall_speed;     // island units / s^2 and / s
    f32 jump_height;                 // rise from a grounded jump, in island units
    f32 walk_stride, run_stride;     // distance a loop of the Walk / Run clip covers

    // for a Mii of height scale `scale` (1 = 158 units) and k = Wii no Ma units per island unit
    static PlayerParams make(f32 k, f32 scale = 1.0f, Gait gait = GAIT_A);
};

struct PlayerInput {
    // where to go: a direction on x/z, its length the stick's tilt (0..1), already turned by the
    // camera (FollowCamera::stick_to_world)
    f32 move_x = 0, move_z = 0;
    bool run = false;  // run at any tilt
    bool fast_run = false;  // faster sprint, for a running leap and retained air momentum
    bool jump = false;  // newly pressed, grounded jump
    bool jump_held = false;  // held jump button, for a higher arc
};

enum class PlayerState : u8 { WAIT, WALK, RUN, FALL, JUMP, SWIM };

// How the Mii came into deep water: off the ground falling, jumping, leaping at a sprint, under the
// parachute, sliding down a slope; or walking in down a shore (no fall at all)
enum class WaterCause : u8 { FALL, JUMP, LEAP, GLIDE, SLIDE, WADE };

// A swim's start, as it was going in (Player::water_entry): for the splash and the Mii's plunge
struct WaterEntry {
    Vec3 at;          // the swimmer's feet, where it began
    f32 surface;      // the water's surface there
    f32 vy;           // the vertical speed going in (island units a second, up: < 0 falling in; WADE 0)
    f32 speed;        // along the way it went, island units a frame, before the water slowed it
    f32 yaw;          // that way (degrees, as yaw())
    WaterCause cause;
};

// A swim's end on a shore or a quay (Player::water_exit): how far the feet rose from the swimmer's
// onto the ground, and how high that ground is over the water's surface (< 0: waded out, under it)
struct WaterExit {
    f32 climbed, over_surface;
};

// a box the body can't enter (a parked car or boat): centred at `centre` on x/z, its own x axis
// (cos_yaw, -sin_yaw) and z axis (sin_yaw, cos_yaw) on x/z, from `bottom` to `top`
struct Obstacle {
    Vec3 centre;
    f32 half_width, half_length;
    f32 cos_yaw, sin_yaw;
    f32 bottom, top;
};

class Player {
public:
    Player(const engine::collision::World& world, const PlayerParams& params);

    // puts the Mii on the ground at or below `feet` (up to a step above it, down to `lowest`), facing
    // `yaw_deg`; false (and the Mii left at `feet`) if there is no ground there
    bool place(Vec3 feet, f32 yaw_deg, f32 lowest = -1e9f);
    // whether the Mii could stand at ground under `feet` (a step above it, down to `lowest`): walkable,
    // not lava, no deeper than wading depth, the body free; where (`at`)
    bool can_stand(Vec3 feet, f32 lowest, Vec3* at) const;
    // puts the Mii swimming in deep water by `feet` (within its height above, two below), where the
    // body is free; false (nothing changed) if there is none
    bool place_swimming(Vec3 feet, f32 yaw_deg);
    // advances `dt` seconds, in steps of at most 1/60 s (the same input and dt give the same result)
    void update(const PlayerInput& in, f32 dt);

    Vec3 position() const { return pos_; }  // the feet
    f32 yaw() const { return yaw_; }
    f32 speed() const { return speed_; }    // island units a frame, along the facing
    PlayerState state() const;
    // where in its gait the Mii is: 0..1 over a loop of the Walk or Run clip, driven by the distance
    // walked (so the feet don't slide whatever the speed)
    f32 stride_phase() const { return phase_; }
    f32 distance() const { return distance_; }  // walked in all
    bool airborne() const { return airborne_; }  // falling or sliding down a steep slope
    bool sliding() const { return airborne_ && sliding_; }
    // gliding down from a sprint off a ledge (a Mii height and more below where it left the ground)
    bool gliding() const { return airborne_ && run_leap_ && last_ground_.y - pos_.y > p_.height; }
    f32 vertical_speed() const { return vy_; }  // island units a second, up
    // (for the Mii's animation) in the air from a jump; off a ledge at a sprint; ticks since the feet
    // left the ground; a jump's take-off speed (island units a second)
    bool jumping() const { return airborne_ && jumping_; }
    bool leaping() const { return airborne_ && run_leap_; }
    int air_ticks() const { return airborne_ ? air_ticks_ : 0; }
    f32 jump_speed() const { return std::sqrt(2.0f * p_.gravity * p_.jump_height); }
    bool swimming() const { return swimming_; }
    // a swim begun in the last update(), the first if more than one (warps and place_swimming give
    // none: nothing went into the water); a swim ended on a shore in it
    const WaterEntry* water_entry() const { return entry_valid_ ? &entry_ : nullptr; }
    const WaterExit* water_exit() const { return exit_valid_ ? &exit_ : nullptr; }
    void set_obstacles(std::vector<Obstacle> obstacles) { obstacles_ = std::move(obstacles); }
    u16 ground_attribute() const { return ground_attr_; }
    f32 water_depth() const { return depth_; }  // of the water over the feet (0: dry)
    const PlayerParams& params() const { return p_; }

    // what the body can't pass: walls (upright faces), invisible fences, overhangs, tree trunks
    bool solid(u16 attribute, Vec3 normal) const;
    // how far the body's spheres would be pushed out of what's solid with the feet at `feet`
    // (0: free)
    f32 overlap(Vec3 feet) const;

private:
    struct Ground {
        bool found = false;
        f32 y = 0;
        Vec3 n{0, 1, 0};
        u16 attr = 0;
    };
    Ground ground_at(f32 x, f32 z, f32 from_y, f32 to_y) const;
    f32 water_above(f32 x, f32 z, f32 ground_y, f32 top_y) const;
    bool water_surface(f32 x, f32 z, f32 from_y, f32 to_y, f32* y) const;
    bool deep_water(f32 x, f32 z, f32 water_top, f32 water_bottom, f32 floor_top, f32* surface) const;
    f32 swim_feet(f32 x, f32 z, f32 surface) const;
    void start_swim(Vec3 at, f32 surface);
    void start_swim(Vec3 at, f32 surface, WaterCause cause, f32 vy, f32 speed, f32 yaw);
    WaterCause air_cause() const;
    bool blocked(Vec3 feet) const;
    bool landing_ok(f32 x, f32 z, f32 from_y, f32 lowest) const;
    Vec3 push(Vec3 sphere) const;
    Vec3 slide_off_walls(Vec3 feet, bool* free) const;
    void tick(const PlayerInput& in, f32 frames);
    void walk(f32 frames);
    bool try_step(Vec3 d);
    void start_fall(bool slide, f32 lowest);
    void ride_slope(Vec3 n);
    void fall(f32 frames, bool jump_held);
    void swim(const PlayerInput& in, f32 frames);
    bool swim_step(Vec3 d, f32* surface);
    bool climb_out(Vec3 d, f32 surface);

    const engine::collision::World& world_;
    PlayerParams p_;
    Vec3 pos_{0, 0, 0}, last_ground_{0, 0, 0};
    f32 yaw_ = 0, speed_ = 0, vy_ = 0;
    f32 phase_ = 0, distance_ = 0, depth_ = 0;
    bool airborne_ = false, sliding_ = false, jumping_ = false, run_leap_ = false, swimming_ = false;
    f32 slide_x_ = 0, slide_z_ = 0;  // down the slope, on x/z
    f32 fall_lowest_ = 0;            // the lowest the fall may end (max_drop under where it began)
    int air_ticks_ = 0;
    u16 ground_attr_ = 0;
    Vec3 ground_n_{0, 1, 0};
    // what's solid near the body, gathered once a walking tick (push() checks it covers a sphere)
    mutable engine::collision::Nearby near_;
    std::vector<Obstacle> obstacles_;
    WaterEntry entry_{};
    WaterExit exit_{};
    bool entry_valid_ = false, exit_valid_ = false;
};

// angles in degrees, wrapped into (-180, 180]
f32 wrap_deg(f32 a);

}  // namespace engine::wuhu
