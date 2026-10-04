// Collision against KCL meshes (kcl.h).
//
// A KCL file (big-endian; research tools/kcl.py has the layout and checks it against the island's
// files) holds a header of 0x3C bytes (the offsets of its positions, normals, prisms and octree, the
// octree's origin, masks and shifts), a pool of positions (f32 x3), a pool of unit normals (f32 x3)
// and the prisms, 0x10 bytes each: f32 height, u16 position, u16 face normal, u16 edge normals A, B
// and C, u16 attribute. A prism is a triangle: its first vertex is the position, and the other two
// lie where the planes through the first vertex across edges A and B (the edges' normals are
// in-plane, pointing out) meet the plane of edge C, `height` away from the first vertex along the
// triangle:
//     v2 = v1 + cross(nB, face) * height / dot(cross(nB, face), nC)
//     v3 = v1 + cross(nA, face) * height / dot(cross(nA, face), nC)
// (Mario Kart Wii's layout; research tools/kcl.py found the rebuilt triangles' normals agree with
// the stored face normals on all the island's 42 934 prisms.) The rebuild is done in double
// precision, once at load, and the triangles are kept as three vertices, the face normal and the
// attribute.
//
// The grid. A file gets an x/z grid of square columns over its triangles' footprint, CELL units a
// side; a column lists (u16, a file has at most 65535 prisms) every triangle whose footprint (its
// projection on x/z) touches the column, tested exactly, not by bounding box, so a long diagonal
// wall isn't listed in columns it only passes near. A sparse file (the island's sea is 140
// triangles over 100 000 units) gets bigger columns: never more than 4 a triangle. The island's
// files at CELL = 128: 50 629 columns, 123 980 entries (a triangle is in 2.9 columns on average),
// 232 at most in one; the ground under a point of the land looks at 10.4 triangles on average.
// Queries look a triangle up only through the grid, and then reject it cheaply (its vertices'
// heights, its plane) before any exact test, so the cost of a query is the size of its columns. In
// memory: 2.68 MB (2.23 MB of triangles, 0.45 MB of grid).
//
// Measured with NOMA_SELFTEST=walk (platform/host/selftest_walk.cpp) on the island: ground() agrees
// with the research tools' raster of the top surface at 12 000 of 12 000 points (within 0.0005);
// on the host a query takes 110-240 ns. On the 3DS (devkitARM, -O2) a candidate the plane rejects
// is 25 instructions, half of them VFP in a dependent multiply-add chain: an estimated 40-80 cycles
// with its two cache lines (not measured on hardware).
#include "engine/collision/kcl.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "runtime.h"

namespace engine::collision {

using engine::math::cross;
using engine::math::dot;

namespace {

// The columns' side, in the file's units. At 128 an island column holds about 13 triangles (the
// ground under a point tests one column, a walking body's sphere 1-4); 256 doubles that for half
// the grid's memory, 64 halves it for four times the columns (the island: 0.2, 0.5, 1.6 MB).
constexpr f32 CELL = 128.0f;
// a sparse file's columns grow until it has at most this many a triangle (and at least 64 in all)
constexpr u32 MAX_CELLS_PER_TRIANGLE = 4;
// how far outside a column a footprint may pass and still be listed in it (rounding of the exact
// test; the island's coordinates reach 50 000, where a float step is 0.004)
constexpr f32 MARGIN = 0.05f;

enum : u16 { TRI_VALID = 1 };

// the rebuilt triangle (52 bytes)
struct Tri {
    Vec3 a, b, c;  // vertices
    Vec3 n;        // unit face normal (the side the file calls the front)
    u16 attr;
    u16 flags;     // TRI_VALID
};
static_assert(sizeof(Tri) == 52, "Tri");

u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
u16 be16(const u8* p) { return (u16)(p[0] << 8 | p[1]); }
double bef(const u8* p) {
    u32 v = be32(p);
    f32 f;
    memcpy(&f, &v, 4);
    return f;
}

struct D3 {
    double x, y, z;
};
D3 operator+(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
D3 operator*(D3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 cross(D3 a, D3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec3 to_f(D3 v) { return {(f32)v.x, (f32)v.y, (f32)v.z}; }

inline f32 min3(f32 a, f32 b, f32 c) { return std::min(a, std::min(b, c)); }
inline f32 max3(f32 a, f32 b, f32 c) { return std::max(a, std::max(b, c)); }

// Does the triangle's footprint touch the rectangle [x0, x1] x [z0, z1]? (The caller has checked the
// bounding boxes.) Separating axes: each edge's line, the rectangle wholly on the far side of it from
// the third vertex; an upright triangle's footprint is a segment, and its line separates when the
// rectangle lies wholly on one side.
bool footprint_touches(const Tri& t, f32 x0, f32 z0, f32 x1, f32 z1) {
    const f32 px[3] = {t.a.x, t.b.x, t.c.x}, pz[3] = {t.a.z, t.b.z, t.c.z};
    for (int i = 0; i < 3; i++) {
        int j = i == 2 ? 0 : i + 1, k = j == 2 ? 0 : j + 1;
        f32 ex = px[j] - px[i], ez = pz[j] - pz[i];
        auto side = [&](f32 x, f32 z) { return ex * (z - pz[i]) - ez * (x - px[i]); };
        f32 s00 = side(x0, z0), s10 = side(x1, z0), s01 = side(x0, z1), s11 = side(x1, z1);
        f32 lo = std::min(std::min(s00, s10), std::min(s01, s11));
        f32 hi = std::max(std::max(s00, s10), std::max(s01, s11));
        f32 so = side(px[k], pz[k]);
        f32 eps = 1e-6f * (ex * ex + ez * ez);
        if (so > eps ? hi < 0 : so < -eps ? lo > 0 : hi < 0 || lo > 0)
            return false;
    }
    return true;
}

// the point of the triangle closest to p (Ericson, Real-Time Collision Detection 5.1.5)
Vec3 closest_point(Vec3 p, const Tri& t) {
    Vec3 ab = t.b - t.a, ac = t.c - t.a, ap = p - t.a;
    f32 d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0)
        return t.a;
    Vec3 bp = p - t.b;
    f32 d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3)
        return t.b;
    f32 vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0)
        return t.a + ab * (d1 / (d1 - d3));
    Vec3 cp = p - t.c;
    f32 d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6)
        return t.c;
    f32 vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0)
        return t.a + ac * (d2 / (d2 - d6));
    f32 va = d3 * d6 - d5 * d4;
    if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0)
        return t.b + (t.c - t.b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    f32 inv = 1.0f / (va + vb + vc);
    return t.a + ab * (vb * inv) + ac * (vc * inv);
}

// Liang-Barsky on one axis: narrows [t0, t1] to where p0 + dp * t is within [lo, hi]
bool clip(f32 p0, f32 dp, f32 lo, f32 hi, f32& t0, f32& t1) {
    if (dp == 0)
        return p0 >= lo && p0 <= hi;
    f32 ta = (lo - p0) / dp, tb = (hi - p0) / dp;
    if (ta > tb)
        std::swap(ta, tb);
    t0 = std::max(t0, ta), t1 = std::min(t1, tb);
    return t0 <= t1;
}

}  // namespace

struct World::Mesh {
    std::string name;
    std::vector<Tri> tris;  // by prism index - 1
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};  // the valid triangles' bounds
    // the grid: column (cx, cz) lists list[start[cz * nx + cx] .. start[cz * nx + cx + 1])
    f32 x0 = 0, z0 = 0, cell = CELL, inv_cell = 1.0f / CELL;
    u32 nx = 0, nz = 0;
    std::vector<u32> start;
    std::vector<u16> list;

    // the columns a range of x/z covers, clamped to the grid; false if it misses the grid
    bool columns(f32 xa, f32 za, f32 xb, f32 zb, u32* cx0, u32* cz0, u32* cx1, u32* cz1) const {
        if (!nx || xb < lo.x || xa > hi.x || zb < lo.z || za > hi.z)
            return false;
        auto col = [](f32 v, u32 n) { return v <= 0 ? 0u : v >= (f32)(n - 1) ? n - 1 : (u32)v; };
        *cx0 = col((xa - x0) * inv_cell, nx), *cx1 = col((xb - x0) * inv_cell, nx);
        *cz0 = col((za - z0) * inv_cell, nz), *cz1 = col((zb - z0) * inv_cell, nz);
        return true;
    }
    template <class F>
    void for_each_column(const Tri& t, F f) const {
        u32 cx0, cz0, cx1, cz1;
        if (!columns(min3(t.a.x, t.b.x, t.c.x) - MARGIN, min3(t.a.z, t.b.z, t.c.z) - MARGIN,
                     max3(t.a.x, t.b.x, t.c.x) + MARGIN, max3(t.a.z, t.b.z, t.c.z) + MARGIN, &cx0, &cz0, &cx1, &cz1))
            return;
        bool one = cx0 == cx1 && cz0 == cz1;
        for (u32 cz = cz0; cz <= cz1; cz++)
            for (u32 cx = cx0; cx <= cx1; cx++) {
                f32 xa = x0 + cx * cell, za = z0 + cz * cell;
                if (one || footprint_touches(t, xa - MARGIN, za - MARGIN, xa + cell + MARGIN, za + cell + MARGIN))
                    f(cz * nx + cx);
            }
    }
    void build_grid() {
        u32 n = 0;
        for (const Tri& t : tris)
            n += t.flags & TRI_VALID;
        start.clear(), list.clear(), nx = nz = 0;
        if (!n)
            return;
        f32 wx = hi.x - lo.x, wz = hi.z - lo.z;
        u32 most = std::max<u32>(64, MAX_CELLS_PER_TRIANGLE * n);
        cell = std::max(CELL, std::sqrt((wx + 1) * (wz + 1) / (f32)most));
        for (;;) {
            nx = (u32)(wx / cell) + 1, nz = (u32)(wz / cell) + 1;
            if (nx * nz <= most)
                break;
            cell *= 1.25f;
        }
        inv_cell = 1.0f / cell, x0 = lo.x, z0 = lo.z;
        start.assign(nx * nz + 1, 0);
        for (const Tri& t : tris)
            if (t.flags & TRI_VALID)
                for_each_column(t, [&](u32 i) { start[i + 1]++; });
        for (u32 i = 0; i < nx * nz; i++)
            start[i + 1] += start[i];
        list.resize(start[nx * nz]);
        std::vector<u32> fill(start.begin(), start.end() - 1);
        for (u32 k = 0; k < tris.size(); k++)
            if (tris[k].flags & TRI_VALID)
                for_each_column(tris[k], [&](u32 i) { list[fill[i]++] = (u16)k; });
    }
    u32 bytes() const {
        return (u32)(sizeof(*this) + name.capacity() + tris.capacity() * sizeof(Tri) + start.capacity() * 4 +
                     list.capacity() * 2);
    }
};

World::World() = default;
World::~World() {
    for (Mesh* m : meshes_)
        delete m;
}

bool World::add_kcl(const u8* data, u32 size, const char* name) {
    if (size < 0x3C) {
        guest::log("kcl: %s: %u bytes, not a KCL file", name, (unsigned)size);
        return false;
    }
    u32 o_pos = be32(data), o_nrm = be32(data + 4), o_prism = be32(data + 8) + 0x10, o_oct = be32(data + 12);
    if (o_pos < 0x3C || o_pos > o_nrm || o_nrm > o_prism || o_prism > o_oct || o_oct > size) {
        guest::log("kcl: %s: section offsets %X %X %X %X out of order or past the end (%u bytes)", name,
                   (unsigned)o_pos, (unsigned)o_nrm, (unsigned)o_prism, (unsigned)o_oct, (unsigned)size);
        return false;
    }
    // the pools run up to the next section (the prisms' own count is the space before the octree)
    u32 n_pos = (o_nrm - o_pos) / 12, n_nrm = (o_prism - o_nrm) / 12, n_prism = (o_oct - o_prism) / 0x10;
    if (n_prism > 0xFFFF) {
        guest::log("kcl: %s: %u prisms, more than a KCL can index", name, (unsigned)n_prism);
        return false;
    }
    auto pos = [&](u32 i) { const u8* p = data + o_pos + i * 12; return D3{bef(p), bef(p + 4), bef(p + 8)}; };
    auto nrm = [&](u32 i) { const u8* p = data + o_nrm + i * 12; return D3{bef(p), bef(p + 4), bef(p + 8)}; };

    Mesh* m = new Mesh;
    m->name = name;
    m->tris.resize(n_prism);
    u32 bad = 0;
    for (u32 k = 0; k < n_prism; k++) {
        const u8* r = data + o_prism + k * 0x10;
        double h = bef(r);
        u16 ip = be16(r + 4), ifn = be16(r + 6), ia = be16(r + 8), ib = be16(r + 10), ic = be16(r + 12);
        Tri& t = m->tris[k];
        memset(&t, 0, sizeof(t));
        t.attr = be16(r + 14);
        if (ip >= n_pos || ifn >= n_nrm || ia >= n_nrm || ib >= n_nrm || ic >= n_nrm) {
            bad++;
            continue;
        }
        D3 v1 = pos(ip), fn = nrm(ifn), nc = nrm(ic);
        D3 ca = cross(nrm(ia), fn), cb = cross(nrm(ib), fn);
        double dca = dot(ca, nc), dcb = dot(cb, nc), fl = std::sqrt(dot(fn, fn));
        if (dca == 0 || dcb == 0 || !(fl > 0)) {
            bad++;
            continue;
        }
        D3 v2 = v1 + cb * (h / dcb), v3 = v1 + ca * (h / dca);
        if (!std::isfinite(v2.x + v2.y + v2.z + v3.x + v3.y + v3.z + v1.x + v1.y + v1.z)) {
            bad++;
            continue;
        }
        t.a = to_f(v1), t.b = to_f(v2), t.c = to_f(v3), t.n = to_f(fn * (1.0 / fl));
        t.flags = TRI_VALID;
        for (Vec3 v : {t.a, t.b, t.c}) {
            m->lo = {std::min(m->lo.x, v.x), std::min(m->lo.y, v.y), std::min(m->lo.z, v.z)};
            m->hi = {std::max(m->hi.x, v.x), std::max(m->hi.y, v.y), std::max(m->hi.z, v.z)};
        }
    }
    if (bad)
        guest::log("kcl: %s: %u of %u prisms make no triangle (left out)", name, (unsigned)bad, (unsigned)n_prism);
    meshes_.push_back(m);
    return true;
}

void World::add_cylinder(const Cylinder& c) { cylinders_.push_back(c); }

void World::finish() {
    for (Mesh* m : meshes_)
        m->build_grid();
    // the cylinders: a grid of 256-unit columns over their footprints (a trunk is a few units wide)
    cyl_start_.clear(), cyl_list_.clear(), cyl_nx_ = cyl_nz_ = 0;
    if (cylinders_.empty() || cylinders_.size() > 0xFFFF)
        return;
    f32 x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
    for (const Cylinder& c : cylinders_) {
        x0 = std::min(x0, c.x - c.radius), x1 = std::max(x1, c.x + c.radius);
        z0 = std::min(z0, c.z - c.radius), z1 = std::max(z1, c.z + c.radius);
    }
    f32 cell = 256.0f;
    while (((u32)((x1 - x0) / cell) + 1) * ((u32)((z1 - z0) / cell) + 1) > 4 * cylinders_.size() + 64)
        cell *= 1.25f;
    cyl_x0_ = x0, cyl_z0_ = z0, cyl_inv_cell_ = 1.0f / cell;
    cyl_nx_ = (u32)((x1 - x0) / cell) + 1, cyl_nz_ = (u32)((z1 - z0) / cell) + 1;
    auto each = [&](const Cylinder& c, auto f) {
        u32 a = (u32)((c.x - c.radius - x0) * cyl_inv_cell_), b = (u32)((c.x + c.radius - x0) * cyl_inv_cell_);
        u32 d = (u32)((c.z - c.radius - z0) * cyl_inv_cell_), e = (u32)((c.z + c.radius - z0) * cyl_inv_cell_);
        for (u32 cz = d; cz <= std::min(e, cyl_nz_ - 1); cz++)
            for (u32 cx = a; cx <= std::min(b, cyl_nx_ - 1); cx++)
                f(cz * cyl_nx_ + cx);
    };
    cyl_start_.assign(cyl_nx_ * cyl_nz_ + 1, 0);
    for (const Cylinder& c : cylinders_)
        each(c, [&](u32 i) { cyl_start_[i + 1]++; });
    for (u32 i = 0; i < cyl_nx_ * cyl_nz_; i++)
        cyl_start_[i + 1] += cyl_start_[i];
    cyl_list_.resize(cyl_start_.back());
    std::vector<u32> fill(cyl_start_.begin(), cyl_start_.end() - 1);
    for (u32 k = 0; k < cylinders_.size(); k++)
        each(cylinders_[k], [&](u32 i) { cyl_list_[fill[i]++] = (u16)k; });
}

void World::cyl_columns(f32 xa, f32 za, f32 xb, f32 zb, u32* cx0, u32* cz0, u32* cx1, u32* cz1) const {
    auto col = [](f32 v, u32 n) { return v <= 0 ? 0u : v >= (f32)(n - 1) ? n - 1 : (u32)v; };
    *cx0 = col((xa - cyl_x0_) * cyl_inv_cell_, cyl_nx_), *cx1 = col((xb - cyl_x0_) * cyl_inv_cell_, cyl_nx_);
    *cz0 = col((za - cyl_z0_) * cyl_inv_cell_, cyl_nz_), *cz1 = col((zb - cyl_z0_) * cyl_inv_cell_, cyl_nz_);
}

u32 World::triangles() const {
    u32 n = 0;
    for (const Mesh* m : meshes_)
        n += (u32)m->tris.size();
    return n;
}

u32 World::memory_bytes() const {
    u32 n = (u32)(sizeof(*this) + meshes_.capacity() * sizeof(Mesh*) + cylinders_.capacity() * sizeof(Cylinder) +
                  cyl_start_.capacity() * 4 + cyl_list_.capacity() * 2);
    for (const Mesh* m : meshes_)
        n += m->bytes();
    return n;
}

void World::grid_info(u32* columns, u32* entries, u32* most) const {
    u32 c = 0, e = 0, m = 0;
    for (const Mesh* mesh : meshes_) {
        c += mesh->nx * mesh->nz;
        e += (u32)mesh->list.size();
        for (u32 i = 0; i + 1 < mesh->start.size(); i++)
            m = std::max(m, mesh->start[i + 1] - mesh->start[i]);
    }
    *columns = c, *entries = e, *most = m;
}

void World::bounds(Vec3* lo, Vec3* hi) const {
    Vec3 l{1e30f, 1e30f, 1e30f}, h{-1e30f, -1e30f, -1e30f};
    for (const Mesh* m : meshes_) {
        l = {std::min(l.x, m->lo.x), std::min(l.y, m->lo.y), std::min(l.z, m->lo.z)};
        h = {std::max(h.x, m->hi.x), std::max(h.y, m->hi.y), std::max(h.z, m->hi.z)};
    }
    *lo = l, *hi = h;
}

// ---- queries

bool World::ground(f32 x, f32 z, f32 from_y, f32 to_y, Hit* hit) const {
    return ground_(x, z, from_y, to_y, [](const void*, u16, Vec3) { return true; }, nullptr, hit);
}
bool World::cast(Vec3 a, Vec3 b, Hit* hit) const {
    return cast_(a, b, [](const void*, u16, Vec3) { return true; }, nullptr, hit);
}

// A vertical ray: one column per file. A triangle is skipped by its vertices' heights first (all
// above the ray's start, or all below the best hit so far), then by its footprint (the three edge
// functions on x/z, inclusive, either winding); only then is its height at (x, z) worked out.
bool World::ground_(f32 x, f32 z, f32 from_y, f32 to_y, Pred p, const void* ctx, Hit* hit) const {
    stats_.grounds++;
    f32 best = to_y;
    const Tri* found = nullptr;
    u32 found_mesh = 0;
    for (u32 mi = 0; mi < meshes_.size(); mi++) {
        const Mesh& m = *meshes_[mi];
        u32 cx, cz, cx1, cz1;
        if (m.lo.y > from_y || m.hi.y < best || !m.columns(x, z, x, z, &cx, &cz, &cx1, &cz1))
            continue;
        u32 i = cz * m.nx + cx;
        const u16* it = m.list.data() + m.start[i];
        const u16* end = m.list.data() + m.start[i + 1];
        stats_.triangle_tests += (u32)(end - it);
        for (; it != end; ++it) {
            const Tri& t = m.tris[*it];
            if ((t.a.y > from_y && t.b.y > from_y && t.c.y > from_y) || (t.a.y < best && t.b.y < best && t.c.y < best))
                continue;
            if (std::fabs(t.n.y) < 1e-3f)
                continue;  // upright: no height under a point
            f32 e0 = (t.b.x - t.a.x) * (z - t.a.z) - (t.b.z - t.a.z) * (x - t.a.x);
            f32 e1 = (t.c.x - t.b.x) * (z - t.b.z) - (t.c.z - t.b.z) * (x - t.b.x);
            if ((e0 < 0 && e1 > 0) || (e0 > 0 && e1 < 0))
                continue;
            f32 e2 = (t.a.x - t.c.x) * (z - t.c.z) - (t.a.z - t.c.z) * (x - t.c.x);
            if ((e2 < 0 && (e0 > 0 || e1 > 0)) || (e2 > 0 && (e0 < 0 || e1 < 0)))
                continue;
            // the plane's height here, kept within the triangle's own (a steep one's rounding)
            f32 y = t.a.y - (t.n.x * (x - t.a.x) + t.n.z * (z - t.a.z)) / t.n.y;
            y = std::min(std::max(y, min3(t.a.y, t.b.y, t.c.y)), max3(t.a.y, t.b.y, t.c.y));
            if (y > from_y || y < best || (found && y == best) || !p(ctx, t.attr, t.n))
                continue;
            best = y, found = &t, found_mesh = mi;
        }
    }
    if (!found)
        return false;
    if (hit) {
        hit->point = {x, best, z};
        hit->normal = found->n;
        hit->t = 0;
        hit->attribute = found->attr;
        hit->mesh = found_mesh;
        hit->triangle = (u32)(found - meshes_[found_mesh]->tris.data());
    }
    return true;
}

// A segment: in each file, the columns its footprint crosses, in order along it (Amanatides and
// Woo's grid walk), stopping once the next column starts beyond the nearest hit so far. A triangle
// is skipped when both ends are on one side of its plane; otherwise the crossing point is tested
// against its three edges (inclusive, either winding).
bool World::cast_(Vec3 a, Vec3 b, Pred p, const void* ctx, Hit* hit) const {
    stats_.casts++;
    const Vec3 d = b - a;
    f32 best = 1.0f;
    const Tri* found = nullptr;
    u32 found_mesh = 0;
    for (u32 mi = 0; mi < meshes_.size(); mi++) {
        const Mesh& m = *meshes_[mi];
        if (!m.nx || std::max(a.x, b.x) < m.lo.x || std::min(a.x, b.x) > m.hi.x || std::max(a.y, b.y) < m.lo.y ||
            std::min(a.y, b.y) > m.hi.y || std::max(a.z, b.z) < m.lo.z || std::min(a.z, b.z) > m.hi.z)
            continue;
        // in column units
        const f32 gx = (a.x - m.x0) * m.inv_cell, gz = (a.z - m.z0) * m.inv_cell;
        const f32 gdx = d.x * m.inv_cell, gdz = d.z * m.inv_cell;
        f32 t0 = 0, t1 = best;
        if (!clip(gx, gdx, 0, (f32)m.nx, t0, t1) || !clip(gz, gdz, 0, (f32)m.nz, t0, t1))
            continue;
        auto col = [](f32 v, u32 n) { return v <= 0 ? 0 : v >= (f32)(n - 1) ? (int)n - 1 : (int)v; };
        int cx = col(gx + gdx * t0, m.nx), cz = col(gz + gdz * t0, m.nz);
        const int sx = gdx > 0 ? 1 : -1, sz = gdz > 0 ? 1 : -1;
        const f32 INF = 1e30f;
        const f32 tdx = gdx != 0 ? std::fabs(1.0f / gdx) : INF, tdz = gdz != 0 ? std::fabs(1.0f / gdz) : INF;
        f32 tx = gdx > 0 ? (cx + 1 - gx) / gdx : gdx < 0 ? (cx - gx) / gdx : INF;
        f32 tz = gdz > 0 ? (cz + 1 - gz) / gdz : gdz < 0 ? (cz - gz) / gdz : INF;
        for (;;) {
            u32 i = (u32)cz * m.nx + (u32)cx;
            const u16* it = m.list.data() + m.start[i];
            const u16* end = m.list.data() + m.start[i + 1];
            stats_.triangle_tests += (u32)(end - it);
            for (; it != end; ++it) {
                const Tri& t = m.tris[*it];
                f32 da = dot(a - t.a, t.n), db = dot(b - t.a, t.n);
                if ((da > 0 && db > 0) || (da < 0 && db < 0) || da == db)
                    continue;
                f32 tt = da / (da - db);
                if (tt > best || (found && tt == best))
                    continue;
                Vec3 q = a + d * tt;
                f32 s0 = dot(cross(t.b - t.a, q - t.a), t.n);
                f32 s1 = dot(cross(t.c - t.b, q - t.b), t.n);
                f32 s2 = dot(cross(t.a - t.c, q - t.c), t.n);
                if (!((s0 >= 0 && s1 >= 0 && s2 >= 0) || (s0 <= 0 && s1 <= 0 && s2 <= 0)) || !p(ctx, t.attr, t.n))
                    continue;
                best = tt, found = &t, found_mesh = mi;
            }
            f32 next = std::min(tx, tz);
            if (next >= std::min(best, t1))
                break;
            if (tx < tz) {
                cx += sx, tx += tdx;
                if (cx < 0 || cx >= (int)m.nx)
                    break;
            } else {
                cz += sz, tz += tdz;
                if (cz < 0 || cz >= (int)m.nz)
                    break;
            }
        }
    }
    if (!found)
        return false;
    if (hit) {
        hit->point = a + d * best;
        hit->normal = found->n;
        hit->t = best;
        hit->attribute = found->attr;
        hit->mesh = found_mesh;
        hit->triangle = (u32)(found - meshes_[found_mesh]->tris.data());
    }
    return true;
}

namespace {

// A sphere being pushed out: a solid triangle within reach pushes it straight away from the
// triangle's closest point by the overlap, at once (each push changes the next triangle's overlap,
// so a corner of two walls settles in a pass or two); a cylinder pushes it away from its axis.
struct Sphere {
    Vec3 c;
    f32 r, r2;
    f32 deepest = 0;
    Vec3 deep_n{0, 1, 0};
    bool moved = false;

    Sphere(Vec3 centre, f32 radius) : c(centre), r(radius), r2(radius * radius) {}
    void push(Vec3 dir, f32 overlap) {
        c = c + dir * overlap;
        if (overlap > deepest)
            deepest = overlap, deep_n = dir;
        // (an overlap of a ten-thousandth of the radius is rounding: it's pushed out, but doesn't
        // call for another pass)
        moved |= overlap > 1e-4f * r;
    }
    // the triangle's distance from the centre along its normal (beyond the radius either way: out of
    // reach, and most of a column is)
    bool in_reach(const Tri& t, f32* plane) const {
        *plane = dot(c - t.a, t.n);
        return *plane < r && *plane > -r;
    }
    void against(const Tri& t, f32 plane) {
        Vec3 v = c - closest_point(c, t);
        f32 d2 = dot(v, v);
        if (d2 >= r2)
            return;
        f32 dist = std::sqrt(d2);
        // (a centre on the triangle leaves by the front)
        push(dist > 1e-5f ? v * (1.0f / dist) : plane < 0 ? -t.n : t.n, r - dist);
    }
    void against(const Cylinder& cy) {
        if (c.y + r < cy.y0 || c.y - r > cy.y1)
            return;
        f32 dx = c.x - cy.x, dz = c.z - cy.z, reach = cy.radius + r;
        f32 d2 = dx * dx + dz * dz;
        if (d2 >= reach * reach)
            return;
        f32 dist = std::sqrt(d2);
        push(dist > 1e-5f ? Vec3{dx / dist, 0, dz / dist} : Vec3{1, 0, 0}, reach - dist);
    }
};

}  // namespace

// A sphere against the grid: the triangles of the columns under it, the cylinders of the cylinder
// grid's. Passes repeat while anything pushed, at most four: by then a sphere wedged between walls
// closer than its diameter has been moved as far as it will go, and the caller sees what's left in
// the next query.
Vec3 World::push_out_(Vec3 centre, f32 r, Pred p, const void* ctx, Vec3* normal) const {
    stats_.push_outs++;
    Sphere s(centre, r);
    const bool trunks = cyl_nx_ && p(ctx, ATTR_CYLINDER, Vec3{1, 0, 0});
    for (int pass = 0; pass < 4; pass++) {
        s.moved = false;
        const Vec3 c = s.c;
        for (const Mesh* mp : meshes_) {
            const Mesh& m = *mp;
            u32 cx0, cz0, cx1, cz1;
            if (c.y - r > m.hi.y || c.y + r < m.lo.y ||
                !m.columns(c.x - r, c.z - r, c.x + r, c.z + r, &cx0, &cz0, &cx1, &cz1))
                continue;
            for (u32 cz = cz0; cz <= cz1; cz++)
                for (u32 cx = cx0; cx <= cx1; cx++) {
                    u32 i = cz * m.nx + cx;
                    const u16* it = m.list.data() + m.start[i];
                    const u16* end = m.list.data() + m.start[i + 1];
                    stats_.triangle_tests += (u32)(end - it);
                    for (; it != end; ++it) {
                        const Tri& t = m.tris[*it];
                        f32 plane;
                        if (s.in_reach(t, &plane) && p(ctx, t.attr, t.n))
                            s.against(t, plane);
                    }
                }
        }
        if (trunks) {
            u32 cx0, cz0, cx1, cz1;
            cyl_columns(c.x - r, c.z - r, c.x + r, c.z + r, &cx0, &cz0, &cx1, &cz1);
            for (u32 cz = cz0; cz <= cz1; cz++)
                for (u32 cx = cx0; cx <= cx1; cx++) {
                    u32 i = cz * cyl_nx_ + cx;
                    stats_.cylinder_tests += cyl_start_[i + 1] - cyl_start_[i];
                    for (u32 k = cyl_start_[i]; k < cyl_start_[i + 1]; k++)
                        s.against(cylinders_[cyl_list_[k]]);
                }
        }
        if (!s.moved)
            break;
    }
    if (normal)
        *normal = s.deep_n;
    return s.c - centre;
}

// The box's columns once: each triangle whose bounding box meets the box and that the predicate
// counts, once (a triangle is listed in every column its footprint touches); the same for the
// cylinders.
void World::gather_(Vec3 lo, Vec3 hi, Pred p, const void* ctx, Nearby* near) const {
    stats_.gathers++;
    near->lo = lo, near->hi = hi;
    near->count = near->cylinders = 0;
    near->complete = false;
    for (const Mesh* mp : meshes_) {
        const Mesh& m = *mp;
        u32 cx0, cz0, cx1, cz1;
        if (lo.y > m.hi.y || hi.y < m.lo.y || !m.columns(lo.x, lo.z, hi.x, hi.z, &cx0, &cz0, &cx1, &cz1))
            continue;
        for (u32 cz = cz0; cz <= cz1; cz++)
            for (u32 cx = cx0; cx <= cx1; cx++) {
                u32 i = cz * m.nx + cx;
                const u16* it = m.list.data() + m.start[i];
                const u16* end = m.list.data() + m.start[i + 1];
                stats_.triangle_tests += (u32)(end - it);
                const u32 before = near->count;  // (the ones from earlier columns may come again)
                for (; it != end; ++it) {
                    const Tri& t = m.tris[*it];
                    if (max3(t.a.y, t.b.y, t.c.y) < lo.y || min3(t.a.y, t.b.y, t.c.y) > hi.y ||
                        max3(t.a.x, t.b.x, t.c.x) < lo.x || min3(t.a.x, t.b.x, t.c.x) > hi.x ||
                        max3(t.a.z, t.b.z, t.c.z) < lo.z || min3(t.a.z, t.b.z, t.c.z) > hi.z ||
                        !p(ctx, t.attr, t.n))
                        continue;
                    if (std::find(near->tri, near->tri + before, (const void*)&t) != near->tri + before)
                        continue;
                    if (near->count == Nearby::MAX)
                        return;
                    near->tri[near->count++] = &t;
                }
            }
    }
    if (cyl_nx_ && p(ctx, ATTR_CYLINDER, Vec3{1, 0, 0})) {
        u32 cx0, cz0, cx1, cz1;
        cyl_columns(lo.x, lo.z, hi.x, hi.z, &cx0, &cz0, &cx1, &cz1);
        for (u32 cz = cz0; cz <= cz1; cz++)
            for (u32 cx = cx0; cx <= cx1; cx++) {
                u32 i = cz * cyl_nx_ + cx;
                for (u32 k = cyl_start_[i]; k < cyl_start_[i + 1]; k++) {
                    const Cylinder& cy = cylinders_[cyl_list_[k]];
                    stats_.cylinder_tests++;
                    if (cy.x + cy.radius < lo.x || cy.x - cy.radius > hi.x || cy.z + cy.radius < lo.z ||
                        cy.z - cy.radius > hi.z || cy.y1 < lo.y || cy.y0 > hi.y ||
                        std::find(near->cyl, near->cyl + near->cylinders, cyl_list_[k]) != near->cyl + near->cylinders)
                        continue;
                    if (near->cylinders == Nearby::MAX_CYLINDERS)
                        return;
                    near->cyl[near->cylinders++] = cyl_list_[k];
                }
            }
    }
    near->complete = true;
}

Vec3 World::push_out(Vec3 centre, f32 r, const Nearby& near, Vec3* normal) const {
    stats_.push_outs++;
    Sphere s(centre, r);
    for (int pass = 0; pass < 4; pass++) {
        s.moved = false;
        stats_.triangle_tests += near.count;
        for (u32 k = 0; k < near.count; k++) {
            const Tri& t = *(const Tri*)near.tri[k];
            f32 plane;
            if (s.in_reach(t, &plane))
                s.against(t, plane);
        }
        stats_.cylinder_tests += near.cylinders;
        for (u32 k = 0; k < near.cylinders; k++)
            s.against(cylinders_[near.cyl[k]]);
        if (!s.moved)
            break;
    }
    if (normal)
        *normal = s.deep_n;
    return s.c - centre;
}

}  // namespace engine::collision
