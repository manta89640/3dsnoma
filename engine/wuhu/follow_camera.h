// The island's third-person camera: it orbits the Mii's head at a distance and a pitch the player can
// change, swings slowly round behind the Mii while it walks (unless the player has just turned the
// camera), keeps clear of the terrain by pulling in along its line of sight, and never goes under
// the ground or the water; it looks a little over the Mii from afar (the Mii low in the picture, the
// island before it), at its middle from close by. It produces the native g3d's Camera
// (engine/g3d/world.h) in island units; the scene scales it by k for the Mii's guest camera
// (engine::g3d::scaled).
#pragma once

#include "engine/collision/kcl.h"
#include "engine/g3d/world.h"

namespace engine::wuhu {

using engine::collision::f32;
using engine::math::Vec3;

struct FollowCameraParams {
    f32 head;                                 // the point orbited, above the Mii's feet
    f32 distance, min_distance, max_distance; // from it (what the player chooses, and the limits)
    f32 aim_near, aim_far;                    // the point looked at, above the feet: from
                                              // min_distance, and from `distance` on (between: between)
    f32 pitch, min_pitch, max_pitch;          // degrees, looking down
    f32 orbit_rate, pitch_rate;               // degrees a second at full tilt
    f32 zoom_rate;                            // the distance's change a second at full tilt (x e)
    f32 manual_hold;                          // s after the player turned it before it swings again
    f32 swing_rate;                           // how fast it swings behind the Mii at run speed (1/s)
    f32 swing_limit;                          // no swing while the Mii faces the camera (degrees off)
    f32 target_time, angle_time, distance_time;  // critically damped smoothing times (s)
    f32 margin;                               // kept between the eye and the terrain
    f32 min_pull;                             // the closest the terrain may pull it in
    f32 fovy_deg, near_, far_, aspect;

    // for a Mii `mii_height` island units tall (PlayerParams::height)
    static FollowCameraParams make(f32 mii_height);
};

struct FollowCameraInput {
    f32 orbit_x = 0;   // -1..1: + turns the view right (the camera goes round to the Mii's left)
    f32 orbit_y = 0;   // -1..1: + raises the camera (looks further down)
    f32 zoom = 0;      // -1..1: + moves out
    bool behind = false;  // swing round behind the Mii now
};

class FollowCamera {
public:
    FollowCamera(const engine::collision::World& world, const FollowCameraParams& params);

    // behind the Mii at once (no smoothing): at the start and after a jump cut
    void reset(Vec3 feet, f32 mii_yaw);
    // speed: the Mii's speed over its run speed (0..1), how fast to swing behind it
    void update(const FollowCameraInput& in, Vec3 feet, f32 mii_yaw, f32 speed, f32 dt);

    const engine::g3d::Camera& camera() const { return cam_; }
    Vec3 pivot() const { return target_; }  // the point orbited (the terrain is kept off the line to it)
    f32 yaw() const { return yaw_; }      // the way it looks on x/z (nw4r degrees: 0 = +z)
    f32 pitch() const { return pitch_; }
    f32 distance() const { return dist_; }  // to the target now (pulled in or not)
    // the stick (x right, y up: away from the camera) as a direction on x/z: the player's input
    void stick_to_world(f32 sx, f32 sy, f32* x, f32* z) const;
    // what the camera can't see through (not the invisible fences)
    static bool blocks_view(engine::collision::u16 attribute, Vec3 normal);

private:
    void place_eye(f32 dt, bool snap);

    const engine::collision::World& world_;
    FollowCameraParams p_;
    engine::g3d::Camera cam_;
    Vec3 target_{0, 0, 0}, target_vel_{0, 0, 0}, head_{0, 0, 0};
    f32 yaw_ = 0, pitch_ = 0, dist_ = 0;            // as shown
    f32 want_yaw_ = 0, want_pitch_ = 0, want_dist_ = 0;  // as asked for
    f32 yaw_vel_ = 0, pitch_vel_ = 0, dist_vel_ = 0;
    f32 since_manual_ = 1e9f;
};

}  // namespace engine::wuhu
