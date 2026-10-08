// Footprints and tyre tracks in the island's sand (island.cpp): marks stamped where the Mii walks and
// where a car drives on sand (collision attribute KCL_SAND), each fading out over MARK_SECONDS. A
// mark's corners take the sand's height under them as it is stamped (a mark that doesn't lie wholly
// on sand isn't stamped), so it neither floats over a dune nor runs onto grass; the frame's marks
// are drawn as one list of triangles through GX, depth tested, not written, blended over the opaque
// world and the Mii (draw()), pulled toward the eye as the native draws pull overlays
// (native_draw.h overlay_pull) so the depth buffer keeps them over the sand.
#pragma once

#include <vector>

#include "engine/collision/kcl.h"
#include "engine/g3d/world.h"

namespace engine::wuhu {

using engine::collision::f32;
using engine::collision::u32;
using engine::math::Vec3;

class SandTracks {
public:
    explicit SandTracks(const engine::collision::World& collision) : collision_(collision) {}

    // the Mii walked from `before` to `now` (`walked` along the way), facing `yaw`, `height` tall;
    // `on_foot` false (in the air, swimming, driving, a warp) ends the trail: the next starts afresh
    void walk(Vec3 before, Vec3 now, f32 walked, f32 yaw, f32 height, bool on_foot, u32 frame);
    // a car's middle moved from `before` to `now` on the ground, facing `yaw`; its wheels `track`
    // apart, a tyre `tyre` wide
    void drive(Vec3 before, Vec3 now, f32 yaw, f32 track, f32 tyre, u32 frame);
    void end_trail() { trail_ = false, car_trail_ = false; }
    // the marks still showing at `frame`, seen from `camera` (package units; the guest's are k times them)
    void draw(const engine::g3d::Camera& camera, f32 k, u32 frame);

private:
    static constexpr u32 MAX_MARKS = 256;
    struct Mark {
        u32 frame;
        u8 n;        // vertices (three a triangle)
        u8 shade;    // 0: a footprint, 1: a tyre's
        Vec3 v[12];
    };
    // a quad or a fan of corners (x across, z along the mark) about `at` facing `yaw`, on the sand
    bool stamp(Vec3 at, f32 yaw, const f32 (*corners)[2], u32 n, Mark* m) const;
    void add(const Mark& m);

    const engine::collision::World& collision_;
    std::vector<Mark> marks_;
    bool trail_ = false, car_trail_ = false;
    f32 walked_ = 0, driven_ = 0;  // along the trail so far (where the next mark falls)
};

}  // namespace engine::wuhu
