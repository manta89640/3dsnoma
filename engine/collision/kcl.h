// Collision against Nintendo's KCL meshes (the format Wii Sports Resort's island ships in, as Mario
// Kart Wii's): triangles stored as prisms, an octree over them, an attribute per triangle. Loaded
// from the raw big-endian files into native memory; queries for walking: the ground under a point,
// a sphere pushed out of walls, a segment cast (camera), and the attribute at a point.
#pragma once

#include <cstdint>
#include <vector>

#include "engine/math/vecmath.h"

namespace engine::collision {

using engine::math::Vec3;
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using f32 = float;

struct Hit {
    Vec3 point{0, 0, 0};
    Vec3 normal{0, 1, 0};
    f32 t = 0;          // along the cast, 0..1
    u16 attribute = 0;  // the triangle's KCL attribute (e.g. 0x1101 grass, 0x1106 water)
    u32 mesh = 0, triangle = 0;
};

// an upright cylinder (tree trunks): collides with spheres
struct Cylinder {
    f32 x, z, y0, y1, radius;
};

class World {
public:
    // add a KCL file (copied); false if it isn't one (the reason logged)
    bool add_kcl(const u8* data, u32 size, const char* name);
    void add_cylinder(const Cylinder& c);
    // after the last add: builds whatever the queries need
    void finish();

    // The highest triangle under (x, z) whose top is at most `from_y` (a ray straight down from
    // from_y to to_y), counting only triangles accept(attribute, normal) says yes to. False if none.
    template <class Accept>
    bool ground(f32 x, f32 z, f32 from_y, f32 to_y, Accept accept, Hit* hit) const;
    bool ground(f32 x, f32 z, f32 from_y, f32 to_y, Hit* hit) const;  // any triangle

    // The first triangle a segment from a to b crosses (front or back), false if none.
    bool cast(Vec3 a, Vec3 b, Hit* hit) const;

    // A sphere at `centre` of `radius`: pushes it out of every triangle that `solid(attribute,
    // normal)` counts (walls, fences) and of the cylinders; returns the total push (0 if none) and
    // the deepest contact's normal.
    template <class Solid>
    Vec3 push_out(Vec3 centre, f32 radius, Solid solid, Vec3* normal = nullptr) const;

    u32 triangles() const;
    u32 memory_bytes() const;
    void bounds(Vec3* lo, Vec3* hi) const;

private:
    struct Mesh;
    std::vector<Mesh*> meshes_;
    std::vector<Cylinder> cylinders_;
    // the implementation's own (grid, ...): see kcl.cpp
    struct Index;
    Index* index_ = nullptr;

public:
    World() = default;
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    // (the template queries call these with a type-erased predicate)
    using Pred = bool (*)(const void* ctx, u16 attribute, Vec3 normal);
    bool ground_(f32 x, f32 z, f32 from_y, f32 to_y, Pred p, const void* ctx, Hit* hit) const;
    Vec3 push_out_(Vec3 centre, f32 radius, Pred p, const void* ctx, Vec3* normal) const;
};

template <class Accept>
bool World::ground(f32 x, f32 z, f32 from_y, f32 to_y, Accept accept, Hit* hit) const {
    return ground_(x, z, from_y, to_y,
                   [](const void* c, u16 a, Vec3 n) { return (*(const Accept*)c)(a, n); }, &accept, hit);
}
template <class Solid>
Vec3 World::push_out(Vec3 centre, f32 radius, Solid solid, Vec3* normal) const {
    return push_out_(centre, radius,
                     [](const void* c, u16 a, Vec3 n) { return (*(const Solid*)c)(a, n); }, &solid, normal);
}

// the island's attributes (research placement.md §3.3)
enum : u16 {
    KCL_GRASS = 0x1101, KCL_DIRT = 0x1102, KCL_HARD = 0x1103, KCL_SAND = 0x1104, KCL_WOOD = 0x1105,
    KCL_WATER = 0x1106, KCL_STRUCTURE = 0x1108, KCL_LAVA = 0x110B,
};
inline bool kcl_is_water(u16 a) { return a == KCL_WATER; }
// invisible fences and plain walls (high byte 0x08 or 0x00)
inline bool kcl_is_fence(u16 a) { return (a >> 8) == 0x08 || (a >> 8) == 0x00; }

}  // namespace engine::collision
