// A world drawn by the native g3d: a package's static cells (terrain, baked props), its instances
// (models placed with a matrix, some animated), its sky and sea, its environment (lights, fog) and
// its animations, seen from a camera. Each frame: update(), set_camera(), then emit() for each pass
// at the point of the scene's draw where that pass belongs (native_draw.h).
//
// What it draws, in a frame:
//   * cells (fmt::Cell): those whose box meets the camera's frustum and lies within the cells' draw
//     distance, at the detail level their distance picks (Cell::lod_distance, times the LOD scale);
//     a group (Cell::group) can be hidden;
//   * instances (fmt::Instance): those whose bounding sphere meets the frustum within their own and
//     the instances' draw distance, each as the model of its LOD chain its distance picks
//     (Model::lod_next from Model::lod_distance on), its nodes posed by its animation (looping, from
//     its own offset) or at rest, billboard nodes turned to the camera;
//   * the sea (Env::sea_model) as an instance at the origin, each batch culled by its box;
//   * the sky (Env::sky_model) first, centred on the eye (it follows the camera), every batch with
//     DRAW_SKY (no depth test or write).
// Order: the OPAQUE pass draws the sky, then the opaque batches by material priority, then front to
// back (cells by their distance, instances by theirs); the TRANSLUCENT pass draws the translucent
// batches by priority, then back to front by the distance of their box's centre.
// Animations: the environment's material animations (the world's, the sea's, the sky's: SRT0 texture
// matrices and CLR0 colour registers) drive a per-material dynamic block (MatDynamic) that the
// packets carry; instance animations pose node trees.
#pragma once

#include <vector>

#include "engine/g3d/material.h"
#include "engine/g3d/native_draw.h"
#include "engine/g3d/package.h"
#include "engine/math/vecmath.h"

namespace engine::g3d {

using engine::math::Mtx34;
using engine::math::Vec3;

// a perspective camera, in package units
struct Camera {
    Vec3 eye{0, 0, 0}, target{0, 0, -1}, up{0, 1, 0};
    f32 fovy_deg = 45.0f;  // vertical field of view
    f32 aspect = 16.0f / 9.0f;
    f32 near_ = 1.0f, far_ = 80000.0f;
};

// The view matrix of a camera (nw4r's look-at: world -> view, -z forward), in its own units; and the
// same camera in the guest's units (positions and near/far times k), whose view matrix is what
// GX-side code (the Mii's ScnRoot camera) uses.
Mtx34 look_at(const Camera& c);
Camera scaled(const Camera& c, f32 k);

struct WorldStats {
    u32 cells_visible = 0, cells_total = 0;
    u32 instances_visible = 0, instances_total = 0;
    u32 items_opaque = 0, items_translucent = 0;
    u32 triangles = 0;  // in what was emitted this frame
};

class World {
public:
    explicit World(const Package& package);
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    const Package& package() const { return package_; }
    f32 world_scale() const;  // k (fmt::Env::mii_units_per_unit)
    // the runtime materials, one per package material (what packets point at)
    const std::vector<Material>& materials() const;

    // show or hide the cells of a group (fmt::Cell::group) and the instances of a model, by name
    void set_group_visible(const char* group, bool visible);
    // distances beyond which cells/instances are not drawn (0: no limit), and the LOD bias
    // (multiplies the package's LOD distances)
    void set_draw_distance(f32 cells, f32 instances);
    void set_lod_scale(f32 scale);
    // an instance placed anew (model -> world, package units), from the next cull on: something the
    // scene moves (an NPC's hair on his head) rather than the package's placement
    void set_instance_matrix(u32 instance, const Mtx34& mtx);

    // animations to `frame` (60 a second, from when the world was made)
    void update(f32 frame);
    // the camera for the next emits (package units)
    void set_camera(const Camera& camera);
    // queue this pass's draws: OPAQUE (sky, then opaque cells/instances front to back), TRANSLUCENT
    // (translucent ones back to front). The GX projection must be the camera's (scaled(camera, k))
    // when the command is executed: load it (GXSetProjection) before emitting.
    void emit(Pass pass);
    // what emit() submits, into a packet of one's own (tests, tools)
    void build_packet(Pass pass, Packet* packet);

    const WorldStats& stats() const { return stats_; }

private:
    struct Impl;
    Impl* impl_;
    const Package& package_;
    WorldStats stats_;
};

}  // namespace engine::g3d
