// The island's cars and boats (vehicles.h).
#include "engine/wuhu/vehicles.h"

#include <algorithm>
#include <cmath>

namespace engine::wuhu {

namespace {

namespace fmt = engine::g3d::fmt;
using engine::collision::Hit;
using engine::collision::kcl_is_fence;
using engine::collision::kcl_is_water;
using engine::math::Mtx34;

constexpr f32 DEG = 3.14159265f / 180.0f;
constexpr u16 CLASS_CAR = 15, CLASS_CRUISER = 7, CLASS_YACHT = 8;
// island units a second (a car is about 55 long, a cruiser 160), and a second's
constexpr f32 CAR_MAX_SPEED = 180.0f, CAR_REVERSE_SPEED = 55.0f;
constexpr f32 BOAT_MAX_SPEED = 85.0f, BOAT_REVERSE_SPEED = 30.0f;
constexpr f32 ACCEL = 115.0f, BRAKE = 190.0f, STEER = 115.0f;  // (degrees a second at full speed)
constexpr f32 STICK_DEAD = 0.12f;
constexpr f32 CAR_STEP = 0.3f;      // of its height: the most its corners climb or drop at once
constexpr f32 CAR_SLOPE_NY = 0.76f;  // the steepest ground it drives on (40 degrees)
constexpr f32 BOAT_DRAFT = 0.15f;    // of its height under the surface

f32 col_length(const Mtx34& m, int c) {
    return std::sqrt(m.m[0][c] * m.m[0][c] + m.m[1][c] * m.m[1][c] + m.m[2][c] * m.m[2][c]);
}

bool floor_of(u16 a, Vec3 n) { return n.y > 0.05f && !kcl_is_water(a) && !kcl_is_fence(a); }
bool water_of(u16 a, Vec3 n) { return n.y > 0.05f && kcl_is_water(a); }
// what stops a car or a boat side on: invisible fences and anything upright or steeper than it drives
bool wall_of(u16 a, Vec3 n) { return kcl_is_fence(a) || (!kcl_is_water(a) && n.y < 0.65f); }

bool overlap(const Obstacle& a, const Obstacle& b) {
    if (a.bottom >= b.top || b.bottom >= a.top)
        return false;
    const f32 dx = b.centre.x - a.centre.x, dz = b.centre.z - a.centre.z;
    const f32 axes[4][2] = {{a.cos_yaw, -a.sin_yaw}, {a.sin_yaw, a.cos_yaw}, {b.cos_yaw, -b.sin_yaw}, {b.sin_yaw, b.cos_yaw}};
    for (const auto& u : axes) {
        auto reach = [&](const Obstacle& o) {
            return o.half_width * std::fabs(o.cos_yaw * u[0] - o.sin_yaw * u[1]) +
                   o.half_length * std::fabs(o.sin_yaw * u[0] + o.cos_yaw * u[1]);
        };
        if (std::fabs(dx * u[0] + dz * u[1]) > reach(a) + reach(b))
            return false;
    }
    return true;
}

f32 dist2(const Obstacle& a, const Obstacle& b) {
    const f32 dx = b.centre.x - a.centre.x, dz = b.centre.z - a.centre.z;
    return dx * dx + dz * dz;
}

}  // namespace

bool boxes_overlap(const Obstacle& a, const Obstacle& b) { return overlap(a, b); }

Vehicles::Vehicles(engine::g3d::World& world, const engine::g3d::Package& package,
                   const engine::collision::World& collision)
    : world_(world), package_(package), collision_(collision) {
    for (u32 i = 0; i < world_.instance_count(); i++) {
        const u16 cls = world_.instance_class(i);
        if ((cls != CLASS_CAR && cls != CLASS_CRUISER && cls != CLASS_YACHT) || world_.instance_animated(i))
            continue;
        Vehicle v;
        v.instance = i;
        v.boat = cls != CLASS_CAR;
        const fmt::Model& m = package_.models()[package_.instances()[i].model];
        std::copy(m.bbox, m.bbox + 6, v.bbox);
        const Mtx34 t = world_.instance_transform(i);
        v.scale = {col_length(t, 0), col_length(t, 1), col_length(t, 2)};
        list_.push_back(v);
    }
}

Obstacle Vehicles::box_at(const Vehicle& v, const Mtx34& m) const {
    Obstacle o;
    o.centre = transform(m, Vec3{0.5f * (v.bbox[0] + v.bbox[3]), 0.5f * (v.bbox[1] + v.bbox[4]),
                                 0.5f * (v.bbox[2] + v.bbox[5])});
    o.half_width = 0.5f * (v.bbox[3] - v.bbox[0]) * v.scale.x;
    o.half_length = 0.5f * (v.bbox[5] - v.bbox[2]) * v.scale.z;
    // (its x axis on x/z: (cos, -sin) of its yaw)
    const f32 xx = m.m[0][0], xz = m.m[2][0], l = std::sqrt(xx * xx + xz * xz);
    o.cos_yaw = l > 1e-6f ? xx / l : 1.0f;
    o.sin_yaw = l > 1e-6f ? -xz / l : 0.0f;
    const f32 half_h = 0.5f * (v.bbox[4] - v.bbox[1]) * v.scale.y;
    o.bottom = o.centre.y - half_h, o.top = o.centre.y + half_h;
    return o;
}

Obstacle Vehicles::box(u32 v) const { return box_at(list_[v], world_.instance_transform(list_[v].instance)); }

f32 Vehicles::track_width(u32 v) const { return 0.8f * (list_[v].bbox[3] - list_[v].bbox[0]) * list_[v].scale.x; }

std::vector<Obstacle> Vehicles::obstacles(u32 except) const {
    std::vector<Obstacle> out;
    out.reserve(list_.size());
    for (u32 v = 0; v < list_.size(); v++)
        if (v != except)
            out.push_back(box(v));
    return out;
}

u32 Vehicles::nearest(Vec3 feet, f32 reach, bool swimming) const {
    u32 best = NONE;
    f32 best_d = reach;
    for (u32 v = 0; v < list_.size(); v++) {
        if (swimming && !list_[v].boat)
            continue;
        const Obstacle o = box(v);
        if (feet.y > o.top + reach || feet.y < o.bottom - 2 * reach)
            continue;
        // (to its box on x/z)
        const f32 dx = feet.x - o.centre.x, dz = feet.z - o.centre.z;
        const f32 x = o.cos_yaw * dx - o.sin_yaw * dz, z = o.sin_yaw * dx + o.cos_yaw * dz;
        const f32 qx = std::max(std::fabs(x) - o.half_width, 0.0f), qz = std::max(std::fabs(z) - o.half_length, 0.0f);
        const f32 d = std::sqrt(qx * qx + qz * qz);
        if (d <= best_d)
            best_d = d, best = v;
    }
    return best;
}

void Vehicles::enter(u32 v) {
    driven_ = v < list_.size() ? v : NONE;
    speed_ = 0;
    slope_f_ = slope_s_ = 0;
    if (driven_ == NONE)
        return;
    const Obstacle o = box(v);
    yaw_ = std::atan2(o.sin_yaw, o.cos_yaw) / DEG;
    // (its tilt as parked: its z and x axes' rise)
    const Mtx34 m = world_.instance_transform(list_[v].instance);
    const f32 zh = std::hypot(m.m[0][2], m.m[2][2]), xh = std::hypot(m.m[0][0], m.m[2][0]);
    slope_f_ = zh > 1e-6f ? m.m[1][2] / zh : 0;
    slope_s_ = xh > 1e-6f ? m.m[1][0] / xh : 0;
}

Vec3 Vehicles::position() const {
    if (driven_ == NONE)
        return Vec3{0, 0, 0};
    const Mtx34 m = world_.instance_transform(list_[driven_].instance);
    return Vec3{m.m[0][3], m.m[1][3], m.m[2][3]};
}

f32 Vehicles::speed_fraction() const {
    if (driven_ == NONE)
        return 0;
    return std::min(std::fabs(speed_) / (list_[driven_].boat ? BOAT_MAX_SPEED : CAR_MAX_SPEED), 1.0f);
}

std::vector<Vec3> Vehicles::exits(f32 clearance) const {
    std::vector<Vec3> out;
    if (driven_ == NONE)
        return out;
    const Obstacle o = box(driven_);
    const Vec3 x{o.cos_yaw, 0, -o.sin_yaw}, z{o.sin_yaw, 0, o.cos_yaw};
    // (at a car's bottom; at a boat's waterline)
    const f32 y = list_[driven_].boat ? o.bottom + BOAT_DRAFT * (o.top - o.bottom) : o.bottom;
    const Vec3 c{o.centre.x, y, o.centre.z};
    const f32 side = o.half_width + clearance, end = o.half_length + clearance;
    // (its sides, then behind it and before it)
    out.push_back(c - x * side);
    out.push_back(c + x * side);
    out.push_back(c - z * end);
    out.push_back(c + z * end);
    return out;
}

Mtx34 Vehicles::matrix(const Vehicle& v, Vec3 x, Vec3 y, Vec3 z, Vec3 origin) const {
    Mtx34 m;
    const Vec3 cols[3] = {x * v.scale.x, y * v.scale.y, z * v.scale.z};
    for (int r = 0; r < 3; r++) {
        m.m[r][0] = (&cols[0].x)[r];
        m.m[r][1] = (&cols[1].x)[r];
        m.m[r][2] = (&cols[2].x)[r];
    }
    m.m[0][3] = origin.x, m.m[1][3] = origin.y, m.m[2][3] = origin.z;
    return m;
}

// another vehicle's box the move runs into (one it already overlapped: only while it comes closer)
bool Vehicles::hits_other(const Obstacle& was, const Obstacle& now) const {
    for (u32 v = 0; v < list_.size(); v++) {
        if (v == driven_)
            continue;
        const Obstacle o = box(v);
        if (overlap(now, o) && (!overlap(was, o) || dist2(now, o) < dist2(was, o)))
            return true;
    }
    return false;
}

// a car with its origin moved to `next` on x/z: on the ground under its corners, or false
bool Vehicles::car_move(const Vehicle& v, Vec3 next, Mtx34* out) {
    const Mtx34 old = world_.instance_transform(v.instance);
    const f32 height = (v.bbox[4] - v.bbox[1]) * v.scale.y;
    const f32 step = CAR_STEP * height;
    const f32 bottom = old.m[1][3] + v.bbox[1] * v.scale.y;
    const Vec3 fwd{std::sin(yaw_ * DEG), 0, std::cos(yaw_ * DEG)}, side{fwd.z, 0, -fwd.x};
    const f32 hw = 0.4f * (v.bbox[3] - v.bbox[0]) * v.scale.x, hl = 0.4f * (v.bbox[5] - v.bbox[2]) * v.scale.z;
    // (the box's middle on x/z, which its origin needn't be at)
    const Vec3 mid_off = side * (0.5f * (v.bbox[0] + v.bbox[3]) * v.scale.x) + fwd * (0.5f * (v.bbox[2] + v.bbox[5]) * v.scale.z);
    const Vec3 c = next + mid_off;
    const Vec3 at[4] = {c + fwd * hl, c - fwd * hl, c + side * hw, c - side * hw};
    // (where each was, with the tilt it had)
    const f32 was[4] = {bottom + slope_f_ * hl, bottom - slope_f_ * hl, bottom + slope_s_ * hw, bottom - slope_s_ * hw};
    f32 y[4];
    for (int i = 0; i < 4; i++) {
        Hit h;
        if (!collision_.ground(at[i].x, at[i].z, was[i] + step, was[i] - step, floor_of, &h) ||
            h.normal.y < CAR_SLOPE_NY)
            return false;
        y[i] = h.point.y;
    }
    // no wall between where its corners were and will be, at half its height
    const Vec3 c0 = Vec3{old.m[0][3], 0, old.m[2][3]} + mid_off;
    const f32 mid_y = 0.25f * (y[0] + y[1] + y[2] + y[3]) + 0.5f * height;
    for (f32 sx : {-1.0f, 1.0f})
        for (f32 sz : {-1.0f, 1.0f}) {
            const Vec3 o = side * (sx * hw * 1.2f) + fwd * (sz * hl * 1.2f);
            Hit h;
            if (collision_.cast(Vec3{c0.x + o.x, mid_y, c0.z + o.z}, Vec3{c.x + o.x, mid_y, c.z + o.z}, wall_of, &h))
                return false;
        }
    slope_f_ = (y[0] - y[1]) / (2 * hl), slope_s_ = (y[2] - y[3]) / (2 * hw);
    if (std::fabs(slope_f_) > 0.85f || std::fabs(slope_s_) > 0.85f)  // (steeper than 40 degrees between them)
        return false;
    const Vec3 z = normalize(fwd + Vec3{0, slope_f_, 0});
    const Vec3 x0 = side + Vec3{0, slope_s_, 0};
    const Vec3 yax = normalize(cross(z, x0));
    const Vec3 x = normalize(cross(yax, z));
    const f32 ground = 0.25f * (y[0] + y[1] + y[2] + y[3]);
    *out = matrix(v, x, yax, z, Vec3{next.x, ground - v.bbox[1] * v.scale.y, next.z});
    return true;
}

// a boat with its origin moved to `next` on x/z: on deep water under its corners, or false; rocking
// with `time`
bool Vehicles::boat_move(const Vehicle& v, Vec3 next, f32 time, Mtx34* out) {
    const Mtx34 old = world_.instance_transform(v.instance);
    const f32 height = (v.bbox[4] - v.bbox[1]) * v.scale.y;
    const f32 draft = BOAT_DRAFT * height;
    const f32 surface_was = old.m[1][3] + v.bbox[1] * v.scale.y + draft;
    const Vec3 fwd{std::sin(yaw_ * DEG), 0, std::cos(yaw_ * DEG)}, side{fwd.z, 0, -fwd.x};
    const f32 hw = 0.4f * (v.bbox[3] - v.bbox[0]) * v.scale.x, hl = 0.4f * (v.bbox[5] - v.bbox[2]) * v.scale.z;
    const Vec3 mid_off = side * (0.5f * (v.bbox[0] + v.bbox[3]) * v.scale.x) + fwd * (0.5f * (v.bbox[2] + v.bbox[5]) * v.scale.z);
    const Vec3 c = next + mid_off;
    const Vec3 at[5] = {c, c + fwd * hl, c - fwd * hl, c + side * hw, c - side * hw};
    f32 surface = surface_was;
    for (int i = 0; i < 5; i++) {
        Hit w, g;
        if (!collision_.ground(at[i].x, at[i].z, surface_was + height, surface_was - height, water_of, &w))
            return false;
        // ground over the surface (land: the sea's plane goes on under it) or within its draft under it
        if (collision_.ground(at[i].x, at[i].z, w.point.y + height, w.point.y - draft, floor_of, &g))
            return false;
        if (i == 0)
            surface = w.point.y;
    }
    const Vec3 c0 = Vec3{old.m[0][3], 0, old.m[2][3]} + mid_off;
    const f32 mid_y = surface + 0.3f * height;
    for (f32 sx : {-1.0f, 1.0f})
        for (f32 sz : {-1.0f, 1.0f}) {
            const Vec3 o = side * (sx * hw * 1.2f) + fwd * (sz * hl * 1.2f);
            Hit h;
            if (collision_.cast(Vec3{c0.x + o.x, mid_y, c0.z + o.z}, Vec3{c.x + o.x, mid_y, c.z + o.z}, wall_of, &h))
                return false;
        }
    // the swell: a slow pitch and roll and a little heave, the bow up with speed
    const f32 pitch = 0.025f * std::sin(time * 1.3f) + 0.05f * speed_fraction();
    const f32 roll = 0.03f * std::sin(time * 0.9f + 1.0f);
    const f32 heave = 0.03f * height * std::sin(time * 1.7f + 0.4f);
    const Vec3 z = normalize(fwd + Vec3{0, pitch, 0});
    const Vec3 yax = normalize(cross(z, side + Vec3{0, roll, 0}));
    const Vec3 x = normalize(cross(yax, z));
    *out = matrix(v, x, yax, z, Vec3{next.x, surface - draft - v.bbox[1] * v.scale.y + heave, next.z});
    return true;
}

void Vehicles::drive(f32 throttle, f32 steer, f32 dt, f32 time) {
    if (driven_ == NONE || !(dt > 0))
        return;
    const Vehicle& v = list_[driven_];
    const f32 top = v.boat ? BOAT_MAX_SPEED : CAR_MAX_SPEED, back = v.boat ? BOAT_REVERSE_SPEED : CAR_REVERSE_SPEED;
    throttle = std::fabs(throttle) < STICK_DEAD ? 0.0f : throttle;
    steer = std::fabs(steer) < STICK_DEAD ? 0.0f : steer;
    if (throttle > 0)
        speed_ = std::min(top, speed_ + throttle * (speed_ < 0 ? BRAKE : ACCEL) * dt);
    else if (throttle < 0)
        speed_ = std::max(-back, speed_ + throttle * (speed_ > 0 ? BRAKE : ACCEL) * dt);
    else
        speed_ = speed_ > 0 ? std::max(0.0f, speed_ - BRAKE * 0.5f * dt) : std::min(0.0f, speed_ + BRAKE * 0.5f * dt);
    const f32 yaw_was = yaw_;
    const f32 frac = std::min(std::fabs(speed_) / top, 1.0f);
    if (frac > 0.01f)
        yaw_ = wrap_deg(yaw_ + steer * STEER * (0.15f + 0.85f * std::sqrt(frac)) * (speed_ < 0 ? 1.0f : -1.0f) * dt);

    const Mtx34 old = world_.instance_transform(v.instance);
    const Obstacle was = box_at(v, old);
    // in steps no longer than a quarter of its width, so it can't pass through a wall or a vehicle
    const f32 dist = std::fabs(speed_) * dt;
    const int steps = std::max(1, (int)std::ceil(dist / std::max(0.5f * was.half_width, 1.0f)));
    const Vec3 fwd{std::sin(yaw_ * DEG), 0, std::cos(yaw_ * DEG)};
    Vec3 origin{old.m[0][3], old.m[1][3], old.m[2][3]};
    Mtx34 m = old;
    bool moved = false;
    for (int i = 0; i < steps; i++) {
        const Vec3 next = origin + fwd * (speed_ * dt / (f32)steps);
        Mtx34 t;
        const Obstacle from = box_at(v, m);
        if (!(v.boat ? boat_move(v, next, time, &t) : car_move(v, next, &t)) || hits_other(from, box_at(v, t))) {
            speed_ = 0;
            if (!moved)
                yaw_ = yaw_was;
            break;
        }
        m = t, origin = next, moved = true;
    }
    // (at rest a boat still rocks)
    if (!moved && v.boat) {
        Mtx34 t;
        if (boat_move(v, Vec3{old.m[0][3], old.m[1][3], old.m[2][3]}, time, &t) && !hits_other(was, box_at(v, t)))
            m = t, moved = true;
    }
    if (moved)
        world_.set_instance_transform(v.instance, m);
}

}  // namespace engine::wuhu
