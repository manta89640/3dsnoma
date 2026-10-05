// The player on Wuhu Island (player.h).
//
// Walking. The stick gives a direction and a tilt; the tilt picks the wanted speed (a slow to full
// walk up to run_tilt, then a run, or a run at any tilt with the run button) and the Mii turns toward
// the direction at the room's turn rate (fn_801ED5E4, the behaviours' default: 5 degrees a frame
// down to 3.3 by a fraction it is given, here the speed over the run speed) while its speed eases
// toward the wanted one by a tenth of the difference a frame (the default of
// room_BodyController_Behavior_GetSpeedBlend). It moves along its facing, not the stick
// (room_BodyController_Move_Steer moves the body the same way), so a turn is an arc; a turn of more
// than about 80 degrees slows it while it comes round.
//
// The ground has three kinds of slope. Walkable (normal y at least walkable_ny, 50 degrees): the
// Mii walks on it, up and down, following it within a step up and a little more down. Steep: it
// can't be climbed, and the Mii slides down it rather than walking (a step onto it from above is a
// fall). Upright (normal y under wall_ny, 72.5 degrees) and overhanging: a wall. On the island this
// is the rock's split (research placement.md §3.3: a fifth of 0x1103 by area is steep), cliffs and
// buildings are upright, and every path, beach and lawn is walkable (no dirt or sand triangle is
// steeper than 45 degrees, under 1 % of the grass); 50 rather than 45 degrees adds the grass hills
// and rock shelves WSR's cycling routes pass, not the 50-58 degree rock faces that would read as
// climbing (research reach.md §8: 73 % of the main island is then walkable, every course on it).
//
// The body. Two spheres: the lower one rests just above the step height (so curbs and stairs are the
// ground's business, not the walls'), the upper one reaches the top of the head. Solid for them:
// walls, invisible fences, overhangs, tree trunks; not steep ground, which the rules below handle
// (as a wall, the slope under a Mii sliding down it would hold it fast); never water. A move is a
// step of at most half the radius:
//   1. the spheres at the new place are pushed sideways out of what's solid (that is what slides
//      the Mii along a wall it walks into);
//   2. the ground there is the highest non-water surface between a step above and a step below the
//      feet: steep and higher, lava, or water deeper than knee-deep (and deeper than where the Mii
//      stands now) refuse the step; no ground in reach, or steep ground below, is a drop, taken only
//      where the fall (and the slide after it) ends on ground fit to stand on (walkable, dry or
//      shallow water, not lava) no more than max_drop (about two Mii heights) below: a deeper drop
//      is refused as a wall would be, so that the Mii never falls into a cove or a pit it can't
//      climb out of (research reach.md §6: ten such traps on the island, the deepest 700 units); a
//      refused step is tried along x and along z alone, so the Mii slides along a shore or a ledge;
//   3. the spheres at the new height must be free, or the step is refused (a low ceiling).
// Falling and sliding: gravity (9.8 m/s^2: Wii no Ma's units are about centimetres) until walkable
// ground is reached; on steep ground the Mii slides down the slope at run speed, keeping to it (a face
// upright enough to be a wall it drops away from instead, as off a ledge); in the air it keeps
// going forward, but only over somewhere it can land. A fall that hasn't ended in 10 s (a hole in the
// collision, a pit of steep slopes) puts the Mii back where it last stood.
//
// Everything is in island units; a tick is 1/60 s (update() divides longer times evenly), so a run
// is the same for the same input on any machine.
#include "engine/wuhu/player.h"

#include <algorithm>
#include <cmath>

namespace engine::wuhu {

using namespace engine::collision;

namespace {

constexpr f32 DEG = 3.14159265f / 180.0f;
constexpr int MAX_AIR_TICKS = 600;  // 10 s

f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : v > hi ? hi : v; }

}  // namespace

f32 wrap_deg(f32 a) {
    a = std::fmod(a, 360.0f);
    if (a > 180.0f)
        a -= 360.0f;
    else if (a <= -180.0f)
        a += 360.0f;
    return a;
}

PlayerParams PlayerParams::make(f32 k, f32 scale, Gait gait) {
    PlayerParams p;
    const f32 h = MII_HEIGHT * scale / k;
    p.height = h;
    // (the room's speeds don't depend on the Mii's height)
    p.walk_speed = gait.walk / k;
    p.run_speed = gait.run / k;
    p.slide_speed = p.run_speed;
    p.speed_blend = 0.1f;
    p.turn_rate_still = 5.0f;
    p.turn_rate_run = 3.3f;
    p.dead_zone = 0.15f;
    p.run_tilt = 0.6f;
    p.radius = 0.2f * h;
    // a 50 cm step for a 1.58 m Mii (5 units at k = 10: the town's kerbs, the gangways' lips, the
    // ruins' rubble; research reach.md §8)
    p.step_up = 0.32f * h;
    p.knee = p.step_up + p.radius;
    p.chest = 0.75f * h;    // the upper sphere's top at 0.95 h
    p.snap_down = 0.3f * h;
    p.max_drop = 1.9f * h;  // 30 units at k = 10
    p.wade_depth = 0.3f * h;  // about knee-deep
    p.walkable_ny = 0.643f;   // 50 degrees (research reach.md §8)
    p.wall_ny = 0.3f;         // 72.5 degrees
    p.gravity = 980.0f / k;
    p.max_fall_speed = 5000.0f / k;
    // a loop of the clip at the gait's speed: Walk loops in 60 frames, Run in 40 (Move.lex, research
    // mii_animations.txt)
    p.walk_stride = gait.walk * 60.0f / k;
    p.run_stride = gait.run * 40.0f / k;
    return p;
}

Player::Player(const World& world, const PlayerParams& params) : world_(world), p_(params) {}

bool Player::solid(u16 a, Vec3 n) const {
    if (a == ATTR_CYLINDER)
        return true;
    if (kcl_is_water(a))
        return false;
    return kcl_is_fence(a) || n.y < p_.wall_ny;
}

// a sphere of the body pushed out of what's solid: against the walls gathered for this tick when
// they cover it, else the grid
Vec3 Player::push(Vec3 c) const {
    if (near_.covers(c, p_.radius))
        return world_.push_out(c, p_.radius, near_);
    return world_.push_out(c, p_.radius, [this](u16 a, Vec3 n) { return solid(a, n); });
}

f32 Player::overlap(Vec3 feet) const {
    Vec3 k = push(Vec3{feet.x, feet.y + p_.knee, feet.z});
    Vec3 c = push(Vec3{feet.x, feet.y + p_.chest, feet.z});
    return std::sqrt(std::max(dot(k, k), dot(c, c)));
}

// the ground: upward-facing, not water, not an invisible wall
Player::Ground Player::ground_at(f32 x, f32 z, f32 from_y, f32 to_y) const {
    Ground g;
    Hit h;
    auto floor = [](u16 a, Vec3 n) { return n.y > 0.05f && !kcl_is_water(a) && !kcl_is_fence(a); };
    if (world_.ground(x, z, from_y, to_y, floor, &h))
        g.found = true, g.y = h.point.y, g.n = h.normal, g.attr = h.attribute;
    return g;
}

// how deep the water over ground at `ground_y` is, counting surfaces up to `top_y` (0: none)
f32 Player::water_above(f32 x, f32 z, f32 ground_y, f32 top_y) const {
    Hit h;
    auto water = [](u16 a, Vec3 n) { return kcl_is_water(a) && n.y > 0.05f; };
    if (top_y > ground_y && world_.ground(x, z, top_y, ground_y, water, &h))
        return h.point.y - ground_y;
    return 0;
}

// Can a fall from `from_y` end well here: the ground below (and, where that is steep, where sliding
// down it leads, in steps of the radius) walkable, not lava, under no more than knee-deep water, and
// no lower than `lowest`.
bool Player::landing_ok(f32 x, f32 z, f32 from_y, f32 lowest) const {
    f32 y = from_y;
    for (int i = 0; i < 64; i++) {
        Ground g = ground_at(x, z, i ? y + p_.step_up : y, lowest);
        if (!g.found || g.attr == KCL_LAVA || water_above(x, z, g.y, y) > p_.wade_depth)
            return false;
        if (g.n.y >= p_.walkable_ny)
            return true;
        f32 l = std::sqrt(g.n.x * g.n.x + g.n.z * g.n.z);
        if (l < 1e-4f)
            return false;
        x += g.n.x / l * p_.radius, z += g.n.z / l * p_.radius, y = g.y;
    }
    return false;
}

// the spheres pushed sideways out of what's solid; `free` says whether they ended clear
Vec3 Player::slide_off_walls(Vec3 feet, bool* free) const {
    const f32 tiny = 1e-6f * p_.radius * p_.radius;
    Vec3 p = feet;
    bool clear = false;
    for (int i = 0; i < 4 && !clear; i++) {
        Vec3 k = push(Vec3{p.x, p.y + p_.knee, p.z});
        p.x += k.x, p.z += k.z;
        Vec3 c = push(Vec3{p.x, p.y + p_.chest, p.z});
        p.x += c.x, p.z += c.z;
        clear = dot(k, k) < tiny && dot(c, c) < tiny;
    }
    if (free)
        *free = clear;
    return p;
}

bool Player::place(Vec3 feet, f32 yaw_deg) {
    yaw_ = wrap_deg(yaw_deg);
    speed_ = vy_ = phase_ = 0;
    airborne_ = sliding_ = false;
    air_ticks_ = 0;
    Ground g = ground_at(feet.x, feet.z, feet.y + p_.step_up, -1e9f);
    if (!g.found) {
        pos_ = feet, ground_attr_ = 0, depth_ = 0, ground_n_ = {0, 1, 0};
        return false;
    }
    pos_ = last_ground_ = {feet.x, g.y, feet.z};
    ground_attr_ = g.attr, ground_n_ = g.n;
    depth_ = water_above(feet.x, feet.z, g.y, g.y + p_.height);
    return true;
}

PlayerState Player::state() const {
    if (airborne_)
        return PlayerState::FALL;
    if (speed_ < 0.1f * p_.walk_speed)
        return PlayerState::WAIT;
    return speed_ < 0.5f * (p_.walk_speed + p_.run_speed) ? PlayerState::WALK : PlayerState::RUN;
}

void Player::update(const PlayerInput& in, f32 dt) {
    if (!(dt > 0))
        return;
    int n = std::max(1, (int)std::ceil(dt * 60.0f - 1e-3f));
    f32 frames = dt * 60.0f / (f32)n;
    for (int i = 0; i < n; i++)
        tick(in, frames);
}

void Player::tick(const PlayerInput& in, f32 f) {
    // (placed on steep ground: it slides, however far down that takes it)
    if (!airborne_ && ground_n_.y < p_.walkable_ny)
        start_fall(true, -1e30f);
    if (airborne_) {
        fall(f);
        return;
    }
    f32 tilt = std::sqrt(in.move_x * in.move_x + in.move_z * in.move_z);
    f32 target = 0;
    if (tilt > p_.dead_zone) {
        tilt = std::min(tilt, 1.0f);
        f32 diff = wrap_deg(std::atan2(in.move_x, in.move_z) / DEG - yaw_);
        bool run = in.run || tilt >= p_.run_tilt;
        f32 walk = clampf((tilt - p_.dead_zone) / (p_.run_tilt - p_.dead_zone), 0, 1);
        target = run ? p_.run_speed : p_.walk_speed * (0.5f + 0.5f * walk);
        target *= clampf(std::cos(diff * DEG), 0.2f, 1.0f);
        f32 rate = p_.turn_rate_still + (p_.turn_rate_run - p_.turn_rate_still) * clampf(speed_ / p_.run_speed, 0, 1);
        yaw_ = wrap_deg(yaw_ + clampf(diff, -rate * f, rate * f));
    }
    speed_ += (target - speed_) * (1.0f - std::pow(1.0f - p_.speed_blend, f));
    if (std::fabs(target - speed_) < 0.01f * p_.walk_speed)
        speed_ = target;
    walk(f);
}

void Player::walk(f32 f) {
    f32 dist = speed_ * f;
    if (dist <= 0)
        return;
    const Vec3 fwd{std::sin(yaw_ * DEG), 0, std::cos(yaw_ * DEG)};
    int steps = std::max(1, (int)std::ceil(dist / (0.5f * p_.radius)));
    const Vec3 d = fwd * (dist / (f32)steps);
    // the walls this tick's steps can reach, out of the grid once: as far as the steps go and the
    // walls push, as low as they can step down and as high as they can climb
    const f32 reach = dist + 3 * p_.radius;
    world_.gather(Vec3{pos_.x - reach, pos_.y - steps * p_.snap_down + p_.step_up - 0.01f, pos_.z - reach},
                  Vec3{pos_.x + reach, pos_.y + steps * p_.step_up + p_.chest + p_.radius + 0.01f, pos_.z + reach},
                  [this](u16 a, Vec3 n) { return solid(a, n); }, &near_);
    f32 moved = 0;
    for (int i = 0; i < steps && !airborne_; i++) {
        Vec3 before = pos_;
        if (!try_step(d) && !try_step(Vec3{d.x, 0, 0}) && !try_step(Vec3{0, 0, d.z}))
            break;
        moved += std::sqrt((pos_.x - before.x) * (pos_.x - before.x) + (pos_.z - before.z) * (pos_.z - before.z));
    }
    // held back (a wall, the shore): the Mii goes as fast as it got
    if (moved < 0.98f * dist)
        speed_ = moved / f;
    f32 w = clampf((speed_ - p_.walk_speed) / (p_.run_speed - p_.walk_speed), 0, 1);
    phase_ += std::min(moved, dist) / (p_.walk_stride + (p_.run_stride - p_.walk_stride) * w);
    phase_ -= std::floor(phase_);
    distance_ += moved;
}

bool Player::try_step(Vec3 d) {
    const f32 tol = 0.05f * p_.radius;
    Vec3 p = pos_ + d;
    // the walls, at the height the ground there would put the body
    Ground g = ground_at(p.x, p.z, pos_.y + p_.step_up, pos_.y - p_.snap_down);
    p.y = g.found && g.n.y >= p_.walkable_ny ? g.y : pos_.y;
    bool free;
    p = slide_off_walls(p, &free);
    const f32 slid_at = p.y;
    g = ground_at(p.x, p.z, pos_.y + p_.step_up, pos_.y - p_.snap_down);
    const bool steep = g.found && g.n.y < p_.walkable_ny;
    if (steep && g.y > pos_.y + 1e-3f)
        return false;  // too steep to climb
    if (!g.found || steep) {
        // a drop, or a slope to slide down (from its surface, within a step down, unless it is
        // upright enough to be a wall to the body)
        p.y = steep && g.n.y >= p_.wall_ny ? g.y : pos_.y;
        const f32 lowest = pos_.y - p_.max_drop;
        if (!landing_ok(p.x, p.z, pos_.y, lowest) || ((p.y != slid_at || !free) && overlap(p) > tol))
            return false;
        last_ground_ = pos_;
        pos_ = p;
        start_fall(false, lowest);
        if (steep)
            ride_slope(g.n);
        return true;
    }
    if (g.attr == KCL_LAVA)
        return false;
    f32 depth = water_above(p.x, p.z, g.y, std::max(g.y + p_.height, pos_.y + p_.step_up));
    if (depth > p_.wade_depth && depth > depth_ + 1e-3f)
        return false;
    p.y = g.y;
    if ((p.y != slid_at || !free) && overlap(p) > tol)
        return false;
    pos_ = p, ground_attr_ = g.attr, ground_n_ = g.n, depth_ = depth;
    return true;
}

void Player::start_fall(bool slide, f32 lowest) {
    airborne_ = true;
    sliding_ = false;
    vy_ = 0;
    air_ticks_ = 0;
    fall_lowest_ = lowest;
    if (slide)
        ride_slope(ground_n_);
}

// Down the slope of normal n: on x/z straight down it at the slide speed, and down as fast as that
// takes the slope's surface, so that the next tick's fall reaches the slope again (from rest, gravity
// takes most of a second to catch up with a 60-degree slope: the Mii hopped down it, floating most of
// its height over it). A face upright enough to be a wall to the body (under wall_ny) isn't followed:
// the knee sphere would be in it; the Mii drops away from it from rest, as off a ledge.
void Player::ride_slope(Vec3 n) {
    sliding_ = true;
    f32 l = std::sqrt(n.x * n.x + n.z * n.z);
    slide_x_ = l > 1e-4f ? n.x / l : 0, slide_z_ = l > 1e-4f ? n.z / l : 0;
    vy_ = n.y >= p_.wall_ny ? std::max(-p_.slide_speed * 60.0f * l / n.y, -p_.max_fall_speed) : 0;
}

void Player::fall(f32 f) {
    const f32 secs = f / 60.0f;
    vy_ = std::max(vy_ - p_.gravity * secs, -p_.max_fall_speed);
    Vec3 p = pos_;
    // forward, or down the slope, over somewhere to land
    Vec3 dir = sliding_ ? Vec3{slide_x_, 0, slide_z_} : Vec3{std::sin(yaw_ * DEG), 0, std::cos(yaw_ * DEG)};
    f32 sp = sliding_ ? p_.slide_speed : speed_;
    if (sp > 0) {
        Vec3 q = slide_off_walls(pos_ + dir * (sp * f), nullptr);
        if (landing_ok(q.x, q.z, pos_.y, fall_lowest_))
            p.x = q.x, p.z = q.z;
        else if (!sliding_)
            speed_ = 0;
    }
    f32 y = pos_.y + vy_ * secs;
    Ground g = ground_at(p.x, p.z, pos_.y + p_.step_up, y);
    if (g.found && g.n.y >= p_.walkable_ny) {
        p.y = g.y;
        airborne_ = sliding_ = false, vy_ = 0;
        ground_attr_ = g.attr, ground_n_ = g.n;
        depth_ = water_above(p.x, p.z, g.y, g.y + p_.height);
    } else if (g.found) {
        // on a steep slope: down it
        p.y = g.y;
        ride_slope(g.n);
    } else {
        p.y = y;
    }
    // (a fall only starts over somewhere to land: one that goes on is a hole in the collision or a
    // pit of steep slopes, so back to where the Mii last stood rather than falling for ever)
    if (airborne_ && ++air_ticks_ > MAX_AIR_TICKS) {
        p = last_ground_;
        airborne_ = sliding_ = false, vy_ = 0, speed_ = 0;
        ground_n_ = {0, 1, 0};
    }
    pos_ = p;
}

}  // namespace engine::wuhu
