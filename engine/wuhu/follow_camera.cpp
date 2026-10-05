// The island's follow camera (follow_camera.h).
//
// Three numbers place it: yaw (the way it looks on x/z), pitch (down from level) and distance from the
// target, the Mii's head. The player changes them with the camera stick (or L/R and the D-pad: the
// scene maps those); otherwise the camera swings round behind the Mii while it moves, faster the
// faster the Mii goes, starting manual_hold seconds after the player last turned it, and not while
// the Mii walks toward the camera (it would whip round). What is shown follows what is asked through
// critically damped springs (no overshoot), so the view never jerks: the target follows the head
// (smoothing the steps of the ground), the angles their wanted values.
//
// Terrain. The line from the target to where the eye wants to be is cast against the collision (not
// the invisible fences, which nobody sees); something in the way pulls the eye in to a margin short
// of it at once (a wall must never come between), and the eye eases back out once it clears. The
// eye is then pushed off any surface within the margin (the near plane's corners are 1.3 x near from
// the eye at 45 degrees and 16:9) and lifted over the ground or the water under it. The water's
// surface is solid for the camera: it never looks from under the sea.
#include "engine/wuhu/follow_camera.h"

#include <algorithm>
#include <cmath>

#include "engine/wuhu/player.h"

namespace engine::wuhu {

using namespace engine::collision;

namespace {

constexpr f32 DEG = 3.14159265f / 180.0f;

f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : v > hi ? hi : v; }

// One step of a critically damped spring taking `cur` to `target` in about `time` seconds, carrying
// its velocity in `vel` (Game Programming Gems 4, 1.10: the exact solution with exp approximated by
// 1 / (1 + x + 0.48 x^2 + 0.235 x^3), stable for any dt).
f32 smooth(f32 cur, f32 target, f32& vel, f32 time, f32 dt) {
    f32 omega = 2.0f / std::max(time, 1e-4f);
    f32 x = omega * dt;
    f32 e = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    f32 change = cur - target;
    f32 temp = (vel + omega * change) * dt;
    vel = (vel - omega * temp) * e;
    return target + (change + temp) * e;
}

}  // namespace

FollowCameraParams FollowCameraParams::make(f32 h) {
    FollowCameraParams p;
    // The framing: the Mii about a quarter of the picture tall, low in its middle, the island before
    // it: 4.5 heights from the head (64 units at k = 10; first 7, a sixth of the picture: the user
    // asked for it closer), 15 degrees down, looking at a point 1.6 heights up (at 45 degrees' field
    // of view). Pulled in (the terrain, the player), the point looked at comes down to the Mii's
    // middle, so that the whole Mii stays in the picture.
    p.head = 0.85f * h;
    p.distance = 4.5f * h;
    p.min_distance = 1.2f * h;
    p.max_distance = 12.0f * h;
    p.aim_near = 0.55f * h;
    p.aim_far = 1.6f * h;
    p.pitch = 15.0f;
    p.min_pitch = -15.0f;
    p.max_pitch = 70.0f;
    p.orbit_rate = 120.0f;
    p.pitch_rate = 60.0f;
    p.zoom_rate = 1.0f;
    p.manual_hold = 2.0f;
    p.swing_rate = 1.2f;
    p.swing_limit = 135.0f;
    p.target_time = 0.12f;
    p.angle_time = 0.2f;
    p.distance_time = 0.35f;
    p.fovy_deg = 45.0f;
    p.near_ = 1.5f;
    p.far_ = 80000.0f;
    p.aspect = 16.0f / 9.0f;
    p.margin = 1.4f * p.near_;
    p.min_pull = p.near_;
    return p;
}

FollowCamera::FollowCamera(const World& world, const FollowCameraParams& params) : world_(world), p_(params) {
    cam_.fovy_deg = p_.fovy_deg, cam_.aspect = p_.aspect, cam_.near_ = p_.near_, cam_.far_ = p_.far_;
}

bool FollowCamera::blocks_view(u16 a, Vec3) { return !kcl_is_fence(a); }

void FollowCamera::reset(Vec3 feet, f32 mii_yaw) {
    head_ = target_ = feet + Vec3{0, p_.head, 0};
    target_vel_ = {0, 0, 0};
    yaw_ = want_yaw_ = wrap_deg(mii_yaw);
    pitch_ = want_pitch_ = p_.pitch;
    dist_ = want_dist_ = p_.distance;
    yaw_vel_ = pitch_vel_ = dist_vel_ = 0;
    since_manual_ = 1e9f;
    place_eye(0, true);
}

void FollowCamera::update(const FollowCameraInput& in, Vec3 feet, f32 mii_yaw, f32 speed, f32 dt) {
    if (!(dt > 0))
        return;
    head_ = feet + Vec3{0, p_.head, 0};
    bool manual = std::fabs(in.orbit_x) > 0.05f || std::fabs(in.orbit_y) > 0.05f;
    since_manual_ = manual ? 0 : since_manual_ + dt;
    want_yaw_ = wrap_deg(want_yaw_ - clampf(in.orbit_x, -1, 1) * p_.orbit_rate * dt);
    want_pitch_ = clampf(want_pitch_ + clampf(in.orbit_y, -1, 1) * p_.pitch_rate * dt, p_.min_pitch, p_.max_pitch);
    want_dist_ = clampf(want_dist_ * std::exp(clampf(in.zoom, -1, 1) * p_.zoom_rate * dt), p_.min_distance,
                        p_.max_distance);
    if (in.behind) {
        want_yaw_ = wrap_deg(mii_yaw);
        since_manual_ = p_.manual_hold;
    } else if (since_manual_ >= p_.manual_hold && speed > 0.02f) {
        f32 d = wrap_deg(mii_yaw - want_yaw_);
        if (std::fabs(d) < p_.swing_limit)
            want_yaw_ = wrap_deg(want_yaw_ + d * (1.0f - std::exp(-p_.swing_rate * std::min(speed, 1.0f) * dt)));
    }
    yaw_ = wrap_deg(smooth(yaw_, yaw_ + wrap_deg(want_yaw_ - yaw_), yaw_vel_, p_.angle_time, dt));
    pitch_ = smooth(pitch_, want_pitch_, pitch_vel_, p_.angle_time, dt);
    target_.x = smooth(target_.x, head_.x, target_vel_.x, p_.target_time, dt);
    target_.y = smooth(target_.y, head_.y, target_vel_.y, p_.target_time, dt);
    target_.z = smooth(target_.z, head_.z, target_vel_.z, p_.target_time, dt);
    place_eye(dt, false);
}

void FollowCamera::place_eye(f32 dt, bool snap) {
    const f32 cy = std::cos(yaw_ * DEG), sy = std::sin(yaw_ * DEG);
    const f32 cp = std::cos(pitch_ * DEG), sp = std::sin(pitch_ * DEG);
    const Vec3 fwd{sy * cp, -sp, cy * cp};
    // pulled in to a margin short of what's in the way
    const f32 reach = want_dist_ + p_.margin;
    f32 allowed = want_dist_;
    Hit hit;
    if (world_.cast(target_, target_ - fwd * reach, blocks_view, &hit))
        allowed = std::max(p_.min_pull, hit.t * reach - p_.margin);
    if (snap || allowed < dist_)
        dist_ = allowed, dist_vel_ = 0;
    else
        dist_ = smooth(dist_, allowed, dist_vel_, p_.distance_time, dt);
    Vec3 eye = target_ - fwd * dist_;
    // off the surfaces beside it (as long as that doesn't put one between it and the target)
    Vec3 push = world_.push_out(eye, p_.margin, blocks_view);
    if (dot(push, push) > 0 && !world_.cast(target_, eye + push, blocks_view, &hit))
        eye = eye + push;
    // over the ground and the water
    auto floor = [](u16 a, Vec3 n) { return n.y > 0.05f && !kcl_is_fence(a); };
    if (world_.ground(eye.x, eye.z, eye.y + p_.margin, eye.y - p_.margin, floor, &hit))
        eye.y = hit.point.y + p_.margin;
    cam_.eye = eye;
    const f32 s = clampf((dist_ - p_.min_distance) / std::max(p_.distance - p_.min_distance, 1e-3f), 0, 1);
    cam_.target = target_ + Vec3{0, p_.aim_near + (p_.aim_far - p_.aim_near) * s - p_.head, 0};
    cam_.up = {0, 1, 0};
}

void FollowCamera::stick_to_world(f32 sx, f32 sy, f32* x, f32* z) const {
    // forward (sin yaw, cos yaw); right, a quarter turn clockwise from above: (-cos yaw, sin yaw)
    const f32 c = std::cos(yaw_ * DEG), s = std::sin(yaw_ * DEG);
    *x = s * sy - c * sx;
    *z = c * sy + s * sx;
}

}  // namespace engine::wuhu
