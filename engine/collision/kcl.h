// Collision against Nintendo's KCL meshes (the format Wii Sports Resort's island ships in, as Mario
// Kart Wii's): triangles stored as prisms, an octree over them, an attribute per triangle. Loaded
// from the raw big-endian files into native memory; queries for walking: the ground under a point,
// a sphere pushed out of walls, a segment cast (camera), and the attribute of what was hit.
//
// The files' own octree is not used. Its leaves list only the triangles within its build radius of
// the cell (2 units for the island's StageArc copies), so a walking body (a few units across) or a
// camera segment would have to visit every neighbouring leaf as well, through up to five levels of
// big-endian offsets. Instead each file gets a uniform x/z grid of columns at load (kcl.cpp): a
// column lists every triangle whose footprint touches it, at any height. The ground under a point
// is then one column, a sphere one to four, a short segment the few its footprint crosses.
//
// Memory, Wuhu Island's five files (42 934 triangles, 2.96 MB as shipped): 52 bytes a triangle
// (2.23 MB) and 0.45 MB of grids, 2.68 MB in all; the files themselves are not kept.
// Queries allocate nothing; they keep counters (stats()), so a World is used from one thread at a
// time.
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
    Vec3 normal{0, 1, 0};  // the triangle's front
    f32 t = 0;             // along the cast, 0..1
    u16 attribute = 0;     // the triangle's KCL attribute (e.g. 0x1101 grass, 0x1106 water)
    u32 mesh = 0, triangle = 0;  // the file (in add order) and the triangle (its prism index - 1)
};

// an upright cylinder (tree trunks): collides with spheres
struct Cylinder {
    f32 x, z, y0, y1, radius;
};

// what the queries did since reset_stats(): their counts and the triangles and cylinders they
// looked at (the grid's candidates, each test that followed), for frame budgets
struct Stats {
    u32 grounds = 0, casts = 0, push_outs = 0, gathers = 0;
    u32 triangle_tests = 0, cylinder_tests = 0;
};

// The solid triangles and cylinders in a box, picked out of the grid once (World::gather) for many
// push_outs: a walking body pushes its two spheres out several times a step, against the same few
// walls, while the grid's columns round it hold mostly floor. Small enough for the stack (about
// 0.6 KB on the 3DS).
struct Nearby {
    static constexpr u32 MAX = 128, MAX_CYLINDERS = 16;
    Vec3 lo{0, 0, 0}, hi{0, 0, 0};  // the box
    u32 count = 0, cylinders = 0;
    bool complete = false;          // false: more than fit (or never gathered); use the grid
    const void* tri[MAX];           // (the world's triangles)
    u16 cyl[MAX_CYLINDERS];
    // does it hold everything solid a sphere here could touch
    bool covers(Vec3 c, f32 r) const {
        return complete && c.x - r >= lo.x && c.x + r <= hi.x && c.y - r >= lo.y && c.y + r <= hi.y &&
               c.z - r >= lo.z && c.z + r <= hi.z;
    }
};

// the attribute push_out's predicate is asked about for a cylinder (no KCL triangle of the island
// has it; a predicate that wants trunks solid says yes to it)
constexpr u16 ATTR_CYLINDER = 0xFFFF;

class World {
public:
    World();
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // Adds a KCL file: its triangles are rebuilt from the prisms into native memory (the data isn't
    // kept). False if it isn't one (the reason logged).
    bool add_kcl(const u8* data, u32 size, const char* name);
    void add_cylinder(const Cylinder& c);
    // after the last add: builds the grids the queries use
    void finish();

    // The highest triangle under (x, z) whose surface there is at most `from_y` and at least `to_y`
    // (a ray straight down from from_y to to_y), counting only triangles accept(attribute, normal)
    // says yes to; upright triangles (no height under a point) never count. False if none.
    template <class Accept>
    bool ground(f32 x, f32 z, f32 from_y, f32 to_y, Accept accept, Hit* hit) const;
    bool ground(f32 x, f32 z, f32 from_y, f32 to_y, Hit* hit) const;  // any triangle

    // The first triangle a segment from a to b crosses (front or back), false if none; with a
    // predicate, the first that accept(attribute, normal) says yes to. Cylinders aren't cast against.
    template <class Accept>
    bool cast(Vec3 a, Vec3 b, Accept accept, Hit* hit) const;
    bool cast(Vec3 a, Vec3 b, Hit* hit) const;

    // A sphere at `centre` of `radius`: pushes it out of every triangle that `solid(attribute,
    // normal)` counts (walls, fences) and of the cylinders `solid(ATTR_CYLINDER, ...)` counts;
    // returns the total push (0 if none) and the deepest contact's direction. Triangles are
    // two-sided: the sphere leaves on the side its centre is on.
    template <class Solid>
    Vec3 push_out(Vec3 centre, f32 radius, Solid solid, Vec3* normal = nullptr) const;

    // The triangles and cylinders in the box lo..hi that `solid` counts, for push_out(near): the
    // same as push_out(solid) for a sphere the box covers (Nearby::covers), at the cost of the
    // grid's columns once.
    template <class Solid>
    void gather(Vec3 lo, Vec3 hi, Solid solid, Nearby* near) const;
    Vec3 push_out(Vec3 centre, f32 radius, const Nearby& near, Vec3* normal = nullptr) const;

    u32 meshes() const { return (u32)meshes_.size(); }
    u32 triangles() const;
    u32 cylinders() const { return (u32)cylinders_.size(); }
    u32 memory_bytes() const;
    // the grids: columns in all, the triangles they list (a triangle once per column it touches), the
    // most in one column
    void grid_info(u32* columns, u32* entries, u32* most) const;
    void bounds(Vec3* lo, Vec3* hi) const;

    const Stats& stats() const { return stats_; }
    void reset_stats() const { stats_ = Stats(); }

    // (the template queries call these with a type-erased predicate)
    using Pred = bool (*)(const void* ctx, u16 attribute, Vec3 normal);
    bool ground_(f32 x, f32 z, f32 from_y, f32 to_y, Pred p, const void* ctx, Hit* hit) const;
    bool cast_(Vec3 a, Vec3 b, Pred p, const void* ctx, Hit* hit) const;
    Vec3 push_out_(Vec3 centre, f32 radius, Pred p, const void* ctx, Vec3* normal) const;
    void gather_(Vec3 lo, Vec3 hi, Pred p, const void* ctx, Nearby* near) const;

private:
    struct Mesh;  // a file's triangles and grid (kcl.cpp)
    std::vector<Mesh*> meshes_;
    std::vector<Cylinder> cylinders_;
    // the cylinders' x/z grid: cell i lists cyl_list_[cyl_start_[i] .. cyl_start_[i + 1])
    f32 cyl_x0_ = 0, cyl_z0_ = 0, cyl_inv_cell_ = 0;
    u32 cyl_nx_ = 0, cyl_nz_ = 0;
    std::vector<u32> cyl_start_;
    std::vector<u16> cyl_list_;
    mutable Stats stats_;
    void cyl_columns(f32 xa, f32 za, f32 xb, f32 zb, u32* cx0, u32* cz0, u32* cx1, u32* cz1) const;
};

template <class Accept>
bool World::ground(f32 x, f32 z, f32 from_y, f32 to_y, Accept accept, Hit* hit) const {
    return ground_(x, z, from_y, to_y,
                   [](const void* c, u16 a, Vec3 n) { return (*(const Accept*)c)(a, n); }, &accept, hit);
}
template <class Accept>
bool World::cast(Vec3 a, Vec3 b, Accept accept, Hit* hit) const {
    return cast_(a, b, [](const void* c, u16 at, Vec3 n) { return (*(const Accept*)c)(at, n); }, &accept, hit);
}
template <class Solid>
Vec3 World::push_out(Vec3 centre, f32 radius, Solid solid, Vec3* normal) const {
    return push_out_(centre, radius,
                     [](const void* c, u16 a, Vec3 n) { return (*(const Solid*)c)(a, n); }, &solid, normal);
}

template <class Solid>
void World::gather(Vec3 lo, Vec3 hi, Solid solid, Nearby* near) const {
    gather_(lo, hi, [](const void* c, u16 a, Vec3 n) { return (*(const Solid*)c)(a, n); }, &solid, near);
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
