// ng3d-pvs: the cells' visibility rows of an NG3D package (fmt::SEC_CVIS), from cube maps rendered at
// eye points: bit j of row i is set when some triangle of cell j shows from some eye of cell i (cells
// next to each other always see each other). tools/wuhu/stage_wuhu.py writes the input (the cells'
// boxes and detail distances, their triangles, the eyes) and reads the rows back; this program
// knows nothing of packages.
//
//   ng3d-pvs in.pvsi out.bin
//
// Conservative in what it hides, as far as sampling allows:
//   * occluders are opaque triangles that write depth and have no alpha test, and are drawn at any
//     distance (not BATCH_DETAIL: the input says which), drawn as their material culls them (a
//     back-face-culled triangle seen from behind hides nothing);
//   * a cell draws one of its detail levels, picked by the eye's distance to its box: from anywhere
//     a cell's eye may be (its eyes' box, and the region the input gives: the cell's square of the
//     grid, the gaps between the eyes' columns and its edges included), all the levels its
//     distances allow are possible, so a cell occludes a pixel only as far as the farthest of them
//     (a level that doesn't cover the pixel: not at all);
//   * the occluders' depth is eroded (the farthest of each pixel's 3x3 neighbourhood) before targets
//     are tested, so a gap narrower than a pixel lets them through; faces are a little wider than
//     90 degrees and overlap;
//   * a target is every triangle of a cell at any of its levels, translucent and cut-out ones too,
//     counted if a pixel it covers, or the pixel of one of its corners, isn't behind the occluders.
// The eyes must cover where the camera can be: a grid over each cell at heights from the ground up
// to the table's height (the stager makes them from the collision).
//
// Input (little-endian): "PVSI", u32 version (1 or 2), u32 n_cells, u32 face_res, u32 threads,
//   per cell: f32 bbox[6], f32 lod_distance[3], u32 n_lods, (version 2) f32 eye_region[6] (min xyz,
//     max xyz: where the cell's eye may be, the levels' distances measured from it),
//   u32 n_sets, per set: u32 cell (~0: drawn always, not a cell's), u32 kind (0 occluder, 1 target),
//     u32 level mask (bit l: drawn at level l), u32 cull (GX: 0 none, 1 front, 2 back), u32 n_tris,
//     f32 xyz[n_tris][3][3],
//   u32 n_eyes, per eye: u32 cell, f32 xyz[3].
// Output: n_cells rows of (n_cells + 7) / 8 bytes, bit j of row i at byte j >> 3, bit j & 7.
// Built with the host build (CMake target ng3d-pvs).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

using u8 = uint8_t;
using u32 = uint32_t;

struct V3 {
    float x, y, z;
};
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

struct Tri {
    V3 p[3];
    u8 cull, mask;
};
struct Cell {
    float box[6], lod[3];
    u32 n_lods;
    float region[6] = {INFINITY, INFINITY, INFINITY, -INFINITY, -INFINITY, -INFINITY};  // where its eye may be
    std::vector<Tri> occ, tgt;  // occluders (with their level masks), targets (all levels)
};

// a cube face: looking along f, x along r, y along u; half-angle tangent t
struct Face {
    V3 f, r, u;
};
const Face k_faces[6] = {
    {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}}, {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},
    {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}, {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
};
const float k_tan = std::tan((45.0f + 2.0f) * 3.14159265f / 180.0f);  // 94 degrees a face: they overlap
const float k_near = 0.25f;

struct SV {
    float x, y, iz;  // screen pixels (y down), 1 / view depth
};

struct View {
    V3 eye;
    const Face* face;
    int res;
};

// a triangle in view space, clipped at the near plane, projected: emit(a, b, c) per piece
template <class F>
void project(const View& v, const V3* p, F emit) {
    V3 q[3];
    for (int i = 0; i < 3; i++) {
        const V3 d = p[i] - v.eye;
        q[i] = {dot(d, v.face->r), dot(d, v.face->u), dot(d, v.face->f)};
    }
    // outside one side of the frustum: nothing
    for (int s = 0; s < 4; s++) {
        int out = 0;
        for (int i = 0; i < 3; i++) {
            const float c = s == 0 ? q[i].x : s == 1 ? -q[i].x : s == 2 ? q[i].y : -q[i].y;
            out += c > q[i].z * k_tan;
        }
        if (out == 3)
            return;
    }
    V3 poly[4];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const V3 &a = q[i], &b = q[(i + 1) % 3];
        const bool ain = a.z >= k_near, bin = b.z >= k_near;
        if (ain)
            poly[n++] = a;
        if (ain != bin) {
            const float t = (k_near - a.z) / (b.z - a.z);
            poly[n++] = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, k_near};
        }
    }
    if (n < 3)
        return;
    SV s[4];
    const float h = v.res * 0.5f;
    for (int i = 0; i < n; i++) {
        const float iz = 1.0f / poly[i].z;
        s[i] = {h + poly[i].x * iz / k_tan * h, h - poly[i].y * iz / k_tan * h, iz};
    }
    emit(s[0], s[1], s[2]);
    if (n == 4)
        emit(s[0], s[2], s[3]);
}

// the pixel centres a projected triangle covers: fn(x, y, iz); returns whether any was
template <class F>
bool raster(int res, SV a, SV b, SV c, F fn) {
    const float minx = std::min({a.x, b.x, c.x}), maxx = std::max({a.x, b.x, c.x});
    const float miny = std::min({a.y, b.y, c.y}), maxy = std::max({a.y, b.y, c.y});
    const int x0 = std::max(0, (int)std::ceil(minx - 0.5f)), x1 = std::min(res - 1, (int)std::floor(maxx - 0.5f));
    const int y0 = std::max(0, (int)std::ceil(miny - 0.5f)), y1 = std::min(res - 1, (int)std::floor(maxy - 0.5f));
    if (x0 > x1 || y0 > y1)
        return false;
    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (area == 0)
        return false;
    const float ia = 1.0f / area;
    bool any = false;
    for (int y = y0; y <= y1; y++) {
        const float py = y + 0.5f;
        for (int x = x0; x <= x1; x++) {
            const float px = x + 0.5f;
            const float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) * ia;
            const float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) * ia;
            const float w2 = 1.0f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0)
                continue;
            any = true;
            if (fn(x, y, w0 * a.iz + w1 * b.iz + w2 * c.iz))
                return true;
        }
    }
    return any;
}

// GX culls by winding: front faces wind clockwise on the screen, i.e. their normal (b - a) x (c - a)
// points away from the eye (tools/g3d/preview.py's rule)
bool drawn(const Tri& t, V3 eye) {
    if (!t.cull)
        return true;
    const bool front = dot(cross(t.p[1] - t.p[0], t.p[2] - t.p[0]), eye - t.p[0]) < 0;
    return t.cull == 2 ? front : t.cull == 1 ? !front : false;
}

float box_dist(const float* b, float x, float y, float z) {
    const float dx = std::max(std::max(b[0] - x, x - b[3]), 0.0f), dy = std::max(std::max(b[1] - y, y - b[4]), 0.0f),
                dz = std::max(std::max(b[2] - z, z - b[5]), 0.0f);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
// the distance range from any point of box a to box b
void box_range(const float* a, const float* b, float* lo, float* hi) {
    float l = 0, h = 0;
    for (int i = 0; i < 3; i++) {
        const float gap = std::max(std::max(b[i] - a[3 + i], a[i] - b[3 + i]), 0.0f);
        const float far = std::max(std::fabs(b[3 + i] - a[i]), std::fabs(a[3 + i] - b[i]));
        l += gap * gap, h += far * far;
    }
    *lo = std::sqrt(l), *hi = std::sqrt(h);
}
// a box against a face's frustum (its corners all outside one side: out)
bool box_in_face(const View& v, const float* b) {
    for (int s = 0; s < 5; s++) {
        int out = 0;
        for (int k = 0; k < 8; k++) {
            const V3 p = {b[k & 1 ? 3 : 0], b[k & 2 ? 4 : 1], b[k & 4 ? 5 : 2]};
            const V3 d = p - v.eye;
            const float x = dot(d, v.face->r), y = dot(d, v.face->u), z = dot(d, v.face->f);
            const float c = s == 0 ? x - z * k_tan : s == 1 ? -x - z * k_tan : s == 2 ? y - z * k_tan
                                                                         : s == 3 ? -y - z * k_tan : k_near - z;
            out += c > 0;
        }
        if (out == 8)
            return false;
    }
    return true;
}

struct Input {
    u32 n_cells = 0, res = 128, threads = 0;
    std::vector<Cell> cells;
    std::vector<Tri> always;                      // occluders drawn whatever the cells' levels
    std::vector<std::vector<V3>> eyes;            // per cell
};

bool read_input(const char* path, Input* in) {
    FILE* f = fopen(path, "rb");
    if (!f)
        return false;
    auto u = [&]() {
        u32 v = 0;
        if (fread(&v, 4, 1, f) != 1)
            throw 1;
        return v;
    };
    auto fl = [&](float* p, size_t n) {
        if (fread(p, 4, n, f) != n)
            throw 1;
    };
    try {
        char magic[4];
        if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "PVSI", 4))
            throw 1;
        const u32 version = u();
        if (version != 1 && version != 2)
            throw 1;
        in->n_cells = u(), in->res = u(), in->threads = u();
        in->cells.resize(in->n_cells);
        for (Cell& c : in->cells) {
            fl(c.box, 6), fl(c.lod, 3);
            c.n_lods = u();
            if (version >= 2)
                fl(c.region, 6);
        }
        const u32 n_sets = u();
        for (u32 s = 0; s < n_sets; s++) {
            const u32 cell = u(), kind = u(), mask = u(), cull = u(), n = u();
            std::vector<float> xyz((size_t)n * 9);
            fl(xyz.data(), xyz.size());
            std::vector<Tri>* dst = cell == ~0u ? &in->always : kind ? &in->cells.at(cell).tgt : &in->cells.at(cell).occ;
            for (u32 t = 0; t < n; t++) {
                Tri tr;
                for (int k = 0; k < 3; k++)
                    tr.p[k] = {xyz[t * 9 + k * 3], xyz[t * 9 + k * 3 + 1], xyz[t * 9 + k * 3 + 2]};
                tr.cull = (u8)cull, tr.mask = (u8)mask;
                dst->push_back(tr);
            }
        }
        const u32 n_eyes = u();
        in->eyes.assign(in->n_cells, {});
        for (u32 e = 0; e < n_eyes; e++) {
            const u32 cell = u();
            float p[3];
            fl(p, 3);
            in->eyes.at(cell).push_back({p[0], p[1], p[2]});
        }
    } catch (int) {
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

// the levels cell `c` may show from anywhere in box `region` (bit l)
u32 possible_levels(const Cell& c, const float* region) {
    float lo, hi;
    box_range(region, c.box, &lo, &hi);
    u32 m = 0;
    for (u32 l = 0; l < c.n_lods; l++) {
        const float from = c.lod[l], to = l + 1 < c.n_lods ? c.lod[l + 1] : INFINITY;
        if (hi >= from && lo < to)
            m |= 1u << l;
    }
    return m ? m : 1;
}

struct Stats {
    std::atomic<u32> eyes{0}, faces{0};
};

// row i: the cells seen from cell i's eyes
void row_of(const Input& in, u32 i, std::vector<u8>* row, Stats* st) {
    const u32 n = in.n_cells;
    const int res = (int)in.res;
    std::vector<u8> vis(n, 0);
    // the cells next to it (and itself): always
    const Cell& ci = in.cells[i];
    for (u32 j = 0; j < n; j++) {
        const Cell& cj = in.cells[j];
        bool touch = true;
        for (int a : {0, 2})
            touch &= cj.box[a] <= ci.box[3 + a] + 1.0f && ci.box[a] <= cj.box[3 + a] + 1.0f;
        if (touch || cj.tgt.empty())
            vis[j] = 1;
    }
    const std::vector<V3>& eyes = in.eyes[i];
    if (eyes.empty()) {  // (nowhere to stand: the row doesn't hide anything)
        std::fill(vis.begin(), vis.end(), 1);
    } else {
        float region[6];
        memcpy(region, ci.region, sizeof region);
        for (const V3& e : eyes) {
            const float p[3] = {e.x, e.y, e.z};
            for (int a = 0; a < 3; a++)
                region[a] = std::min(region[a], p[a]), region[3 + a] = std::max(region[3 + a], p[a]);
        }
        std::vector<u32> levels(n);
        for (u32 k = 0; k < n; k++)
            levels[k] = possible_levels(in.cells[k], region);
        std::vector<float> D((size_t)res * res), E((size_t)res * res), T[3];
        for (auto& t : T)
            t.assign((size_t)res * res, 0.0f);
        for (const V3& eye : eyes) {
            bool left = false;
            for (u32 j = 0; j < n; j++)
                left |= !vis[j];
            if (!left)
                break;
            st->eyes++;
            for (const Face& face : k_faces) {
                const View v = {eye, &face, res};
                // targets still hidden that this face might show
                std::vector<u32> todo;
                for (u32 j = 0; j < n; j++)
                    if (!vis[j] && box_in_face(v, in.cells[j].box))
                        todo.push_back(j);
                if (todo.empty())
                    continue;
                st->faces++;
                std::fill(D.begin(), D.end(), 0.0f);  // 1/z of the nearest occluder (0: none)
                auto occlude = [&](std::vector<float>& buf, const Tri& t) {
                    if (!drawn(t, eye))
                        return;
                    project(v, t.p, [&](SV a, SV b, SV c) {
                        raster(res, a, b, c, [&](int x, int y, float iz) {
                            float& d = buf[(size_t)y * res + x];
                            d = std::max(d, iz);
                            return false;
                        });
                    });
                };
                for (const Tri& t : in.always)
                    occlude(D, t);
                for (u32 k = 0; k < n; k++) {
                    const Cell& ck = in.cells[k];
                    if (ck.occ.empty() || !box_in_face(v, ck.box))
                        continue;
                    const u32 lm = levels[k];
                    const int nl = __builtin_popcount(lm);
                    if (nl == 1) {
                        for (const Tri& t : ck.occ)
                            if (t.mask & lm)
                                occlude(D, t);
                        continue;
                    }
                    // each possible level apart; the pixel is hidden as far as the farthest of them
                    int used = 0;
                    for (u32 l = 0; l < 3; l++)
                        if (lm >> l & 1) {
                            std::fill(T[used].begin(), T[used].end(), 0.0f);
                            for (const Tri& t : ck.occ)
                                if (t.mask >> l & 1)
                                    occlude(T[used], t);
                            used++;
                        }
                    for (size_t p = 0; p < D.size(); p++) {
                        float m = T[0][p];
                        for (int k2 = 1; k2 < used; k2++)
                            m = std::min(m, T[k2][p]);
                        D[p] = std::max(D[p], m);
                    }
                }
                // eroded: the farthest of each 3x3 neighbourhood
                for (int y = 0; y < res; y++)
                    for (int x = 0; x < res; x++) {
                        float m = D[(size_t)y * res + x];
                        for (int dy = -1; dy <= 1; dy++)
                            for (int dx = -1; dx <= 1; dx++) {
                                const int xx = x + dx, yy = y + dy;
                                if (xx >= 0 && yy >= 0 && xx < res && yy < res)
                                    m = std::min(m, D[(size_t)yy * res + xx]);
                            }
                        E[(size_t)y * res + x] = m;
                    }
                auto shows = [&](int x, int y, float iz) { return iz >= E[(size_t)y * res + x] * (1.0f - 1e-3f); };
                for (u32 j : todo) {
                    for (const Tri& t : in.cells[j].tgt) {
                        bool seen = false;
                        project(v, t.p, [&](SV a, SV b, SV c) {
                            if (seen)
                                return;
                            bool hit = false;
                            raster(res, a, b, c, [&](int x, int y, float iz) { return hit = shows(x, y, iz); });
                            // its corners' pixels (a triangle smaller than a pixel)
                            for (const SV& s : {a, b, c})
                                if (!hit && s.x >= 0 && s.y >= 0 && s.x < res && s.y < res)
                                    hit = shows((int)s.x, (int)s.y, s.iz);
                            seen = hit;
                        });
                        if (seen) {
                            vis[j] = 1;
                            break;
                        }
                    }
                }
            }
        }
    }
    row->assign((n + 7) / 8, 0);
    for (u32 j = 0; j < n; j++)
        if (vis[j])
            (*row)[j >> 3] |= (u8)(1u << (j & 7));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: ng3d-pvs in.pvsi out.bin\n");
        return 2;
    }
    Input in;
    if (!read_input(argv[1], &in)) {
        fprintf(stderr, "ng3d-pvs: can't read %s\n", argv[1]);
        return 1;
    }
    const u32 n = in.n_cells;
    std::vector<std::vector<u8>> rows(n);
    Stats st;
    std::atomic<u32> next{0};
    u32 threads = in.threads ? in.threads : std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (u32 t = 0; t < threads; t++)
        pool.emplace_back([&]() {
            for (u32 i; (i = next++) < n;)
                row_of(in, i, &rows[i], &st);
        });
    for (std::thread& t : pool)
        t.join();
    FILE* f = fopen(argv[2], "wb");
    if (!f)
        return 1;
    u32 ones = 0, cells_with_eyes = 0;
    for (u32 i = 0; i < n; i++) {
        fwrite(rows[i].data(), 1, rows[i].size(), f);
        cells_with_eyes += !in.eyes[i].empty();
        for (u8 b : rows[i])
            ones += (u32)__builtin_popcount(b);
    }
    fclose(f);
    printf("ng3d-pvs: %u cells (%u with eyes), %u eyes and %u faces rendered (%dx%d), %u of %u bits set\n", n,
           cells_with_eyes, st.eyes.load(), st.faces.load(), (int)in.res, (int)in.res, ones, n * n);
    return 0;
}
