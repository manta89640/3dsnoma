// A world drawn by the native g3d (world.h): culling (the frustum, distances, the visibility rows,
// detail batches), detail levels, animation (at a rate by size; combiners translated when their
// registers change), the draw order, the packets.
#include "engine/g3d/world.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "engine/g3d/anim.h"

namespace engine::g3d {

using namespace engine::math;

Mtx34 look_at(const Camera& c) {
    // nw4r's (and the SDK's) MTXLookAt: the view's z axis points from the target back to the eye
    Vec3 look = c.eye - c.target;
    if (dot(look, look) == 0)
        look = {0, 0, 1};
    look = normalize(look);
    Vec3 right = cross(c.up, look);
    right = dot(right, right) == 0 ? Vec3{1, 0, 0} : normalize(right);
    const Vec3 up = cross(look, right);
    return {{{right.x, right.y, right.z, -dot(right, c.eye)},
             {up.x, up.y, up.z, -dot(up, c.eye)},
             {look.x, look.y, look.z, -dot(look, c.eye)}}};
}

Camera scaled(const Camera& c, f32 k) {
    Camera s = c;
    s.eye = c.eye * k, s.target = c.target * k;
    s.near_ = c.near_ * k, s.far_ = c.far_ * k;
    return s;
}

namespace {

const u16 NONE = fmt::NONE16;

// a plane: n.x + d >= 0 on the inside (n unit)
struct Plane {
    Vec3 n;
    f32 d;
};
struct Frustum {
    Plane p[6];
};

Plane plane(Vec3 n, Vec3 through) {
    n = normalize(n);
    return {n, -dot(n, through)};
}

// the camera's frustum, in its own units
Frustum frustum(const Camera& c) {
    Vec3 f = c.target - c.eye;
    f = dot(f, f) == 0 ? Vec3{0, 0, -1} : normalize(f);
    Vec3 r = cross(f, c.up);
    r = dot(r, r) == 0 ? Vec3{1, 0, 0} : normalize(r);
    const Vec3 u = cross(r, f);
    const f32 tv = std::tan(c.fovy_deg * 0.5f * 3.14159265f / 180.0f), th = tv * c.aspect;
    Frustum fr;
    fr.p[0] = plane(f, c.eye + f * c.near_);
    fr.p[1] = plane(-f, c.eye + f * c.far_);
    fr.p[2] = plane(r + f * th, c.eye);   // left
    fr.p[3] = plane(-r + f * th, c.eye);  // right
    fr.p[4] = plane(u + f * tv, c.eye);   // bottom
    fr.p[5] = plane(-u + f * tv, c.eye);  // top
    return fr;
}

// a box (min xyz, max xyz) against the frustum: out if wholly behind one plane (its corner furthest
// along the plane's normal is behind it)
bool box_visible(const Frustum& fr, const f32* b) {
    for (const Plane& p : fr.p) {
        const Vec3 v = {p.n.x >= 0 ? b[3] : b[0], p.n.y >= 0 ? b[4] : b[1], p.n.z >= 0 ? b[5] : b[2]};
        if (dot(p.n, v) + p.d < 0)
            return false;
    }
    return true;
}

bool sphere_visible(const Frustum& fr, Vec3 c, f32 r) {
    for (const Plane& p : fr.p)
        if (dot(p.n, c) + p.d < -r)
            return false;
    return true;
}

// the distance from a point to a box (0 inside)
f32 box_distance(const f32* b, Vec3 p) {
    const f32 dx = std::max(std::max(b[0] - p.x, p.x - b[3]), 0.0f);
    const f32 dy = std::max(std::max(b[1] - p.y, p.y - b[4]), 0.0f);
    const f32 dz = std::max(std::max(b[2] - p.z, p.z - b[5]), 0.0f);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

Vec3 box_centre(const f32* b) { return {(b[0] + b[3]) * 0.5f, (b[1] + b[4]) * 0.5f, (b[2] + b[5]) * 0.5f}; }

// the box around a box transformed
void transform_box(const Mtx34& m, const f32* in, f32* out) {
    for (int i = 0; i < 3; i++) {
        f32 lo = m.m[i][3], hi = m.m[i][3];
        for (int j = 0; j < 3; j++) {
            const f32 a = m.m[i][j] * in[j], b = m.m[i][j] * in[3 + j];
            lo += std::min(a, b), hi += std::max(a, b);
        }
        out[i] = lo, out[3 + i] = hi;
    }
}

f32 max_scale(const Mtx34& m) {
    f32 s = 0;
    for (int j = 0; j < 3; j++)
        s = std::max(s, m.m[0][j] * m.m[0][j] + m.m[1][j] * m.m[1][j] + m.m[2][j] * m.m[2][j]);
    return std::sqrt(s);
}

Mtx34 to_mtx(const f32 (*m)[4]) {
    Mtx34 r;
    memcpy(r.m, m, sizeof(r.m));
    return r;
}

Mtx34 translation(Vec3 t) { return {{{1, 0, 0, t.x}, {0, 1, 0, t.y}, {0, 0, 1, t.z}}}; }

s16 clamp_reg(f32 v) {
    const long i = std::lround(v);
    return (s16)(i < -1024 ? -1024 : i > 1023 ? 1023 : i);
}
u8 clamp_u8(f32 v) {
    const long i = std::lround(v);
    return (u8)(i < 0 ? 0 : i > 255 ? 255 : i);
}

// the 3DS's top screen: 240 lines (half of them: what a frustum's half height covers)
constexpr f32 SCREEN_HALF_PX = 120;
// the animation rate (pose_instance): every second update under ANIM_HALF_PX pixels across, every
// fourth under ANIM_QUARTER_PX, while the nodes move under ANIM_STEP_PX (a third of it) a frame
constexpr f32 ANIM_HALF_PX = 20, ANIM_QUARTER_PX = 6, ANIM_STEP_PX = 1.5f;
// an animated prop under a pixel across isn't drawn (WF_DETAIL)
constexpr f32 SIZE_CULL_PX = 1.0f;

// The trails' ribbons (the trail batch's vertices, DRAW_DYNAMIC): position (f32 x 3, from the item's
// origin), colour (u8 x 4: white, the alpha the trail's strength), texture coordinates (f32 x 2: across
// the ribbon 0-1, along it the path's length over TRAIL_REPEAT, so that the foam stays on the water as
// the boat goes); a ribbon's vertices TRAIL_LIFT over the waves under them (an overlay, drawn pulled
// toward the eye as the others); its first stretch fades in over TRAIL_FADE_IN seconds behind the stern
// (where the boat's own wake model lies); the trails of boats further than TRAIL_RANGE from the eye
// aren't drawn
constexpr u32 TRAIL_STRIDE = 24;
constexpr f32 TRAIL_REPEAT = 160.0f, TRAIL_LIFT = 0.4f, TRAIL_FADE_IN = 0.8f, TRAIL_RANGE = 4000.0f;

// depth tested (less, less or equal, or their opposites) and written, colour written, neither blended
// nor a logic op: where it comes among its kind changes nothing but the state the GPU is given (the
// PICA tests depth after the combiners: no early test that front to back would feed). (ALWAYS, EQUAL
// or a depth-only material would make the order show.)
bool is_plain(const Material& m) {
    const u8 f = m.pixel.depth_func;  // GPU_TESTFUNC: 4 less, 5 lequal, 6 greater, 7 gequal
    return m.pixel.depth_test && m.pixel.depth_write && f >= 4 && f <= 7 && m.pixel.color_write && !m.pixel.blend &&
           !m.pixel.logic;
}

}  // namespace

struct World::Impl {
    const Package& pkg;
    f32 k = 1;
    std::vector<Material> mats;
    u32 features = WF_ALL;

    // ---- material animations: the environment's (world, sea, sky), each material they drive with
    // a dynamic block (its current registers, colours, texture SRTs and what they make)
    std::vector<u16> mat_anims;
    // a translation of a material's combiners for one set of its registers (what the translation
    // folds into the stages' constants), kept for when the registers come back to it
    struct CombEntry {
        s16 reg[4][4], konst[4][4];
        engine::gx::Combiners comb;
        u32 used = 0;  // the update it was last wanted at (the least recent one makes way)
        bool valid = false;
    };
    static constexpr u32 COMB_CACHE = 4;
    struct Dyn {
        MatDynamic d;
        f32 srt[3][5];         // the first three texture matrices' scale s, t, rotation, translation s, t
        bool comb_stale = false;  // the registers changed since d.comb was made
        CombEntry cache[COMB_CACHE];
    };
    std::vector<Dyn> dyn;
    std::vector<int> dyn_of;                    // per material: index into dyn, -1
    std::vector<std::vector<u32>> tmtx_users;   // per TMTX record: (material << 8 | its index there)

    // ---- instances
    struct NodeRot {
        u16 node;
        f32 r[3];  // degrees, added to its rest rotation
    };
    struct Inst {
        Mtx34 mtx;          // model -> world
        Vec3 centre;        // bounding sphere of its model, world
        f32 radius, scale;  // (scale: the matrix's largest)
        u32 slot = 0;       // its nodes' world matrices: finals[slot..] (room for its LOD chain's largest)
        u16 posed_model = NONE;  // the model they were posed for (NONE: not yet)
        u32 posed_tick = 0;      // the update they were posed at
        bool animated = false;   // node tracks play on it
        Vec3 at{0, 0, 0};        // of its nodes' places at the last pose the nearest the eye (where it
                                 // is: an animation can carry it far from its matrix, along a path
                                 // its sphere spans)
        f32 speed = 0;           // the most a node's place moved an update, between its last two poses
        Vec3 mesh_centre{0, 0, 0};  // its meshes' bounding sphere as last posed (world)
        f32 mesh_r = 0;
        std::vector<NodeRot> rots;  // nodes of its own model turned at run time (set_instance_node_rotation)
    };
    std::vector<Inst> insts;
    // node tracks per animation, by (model, node): track indices; and per (animation, model) each
    // node's entry in that list (-1: none)
    struct NodeTracks {
        u16 model, node;
        std::vector<u32> tracks;
    };
    std::vector<std::vector<NodeTracks>> anim_nodes;  // per animation (filled for instance animations)
    struct NodeMap {
        u16 anim, model;
        std::vector<int> of_node;
    };
    std::vector<NodeMap> node_maps;
    std::vector<u8> model_billboards;  // per model: it has billboard nodes
    // per model: the radius of its meshes about its origin (its batches' boxes where their nodes rest):
    // what the screen shows of it, which its bounding sphere (an animated prop's spans its path: a
    // bird 5 units long flies a circle 500 across) doesn't say
    std::vector<f32> mesh_radius;
    u32 sea_slot = 0, sky_slot = 0;
    bool sea_posed = false;

    // ---- what the scene set
    std::vector<u8> cell_hidden, model_hidden;
    f32 cell_distance = 0, inst_distance = 0, lod_scale = 1;
    f32 frame = 0;
    u32 tick = 0;       // updates so far (the animation rates count them)
    Camera cam;
    f32 eye_ground = NAN;
    Frustum fr;
    Mtx34 view_guest;
    bool dirty = true;  // camera, time or settings changed since the last cull
    bool grid = false;  // the cells are the Env's grid, in order (visibility rows usable)

    // ---- this frame's visible set (cull())
    struct VisCell {
        u32 cell;
        u8 lod;
        f32 distance;
    };
    struct VisInst {
        u32 inst;           // instance index, or ~0u: the sea, ~1u: the sky
        u16 model;          // the model of its LOD chain drawn
        f32 distance;
        u32 first_final;    // its nodes' world matrices in `finals`
    };
    std::vector<VisCell> vis_cells;
    std::vector<VisInst> vis_insts;
    std::vector<Mtx34> finals;  // every instance's slot, the sea's and the sky's
    WorldStats* stats = nullptr;
    // the triangles each pass's last packet holds (stats->triangles is their sum: a world that isn't
    // culled again -- frozen, the camera still -- must not count a pass twice)
    u32 pass_tris[2] = {0, 0};

    // ---- the passes' candidates (collected with the cull, sorted at their first build)
    struct Cand {
        s32 priority;
        f32 distance;
        u32 order;
        u32 final;  // its matrix: a finals index, ~0u: the identity (the static world)
        u16 material;
        u16 coords;  // its vertex buffer's texture coordinate scale (an index: same scale, same index)
        bool plain;  // depth tested and written, not blended: its place among them doesn't matter
        DrawItem item;
        u32 tris;
    };
    std::vector<Cand> cands[2];
    u32 n_sky = 0;               // the opaque pass's first candidates: the sky's, in order
    bool sorted[2] = {false, false};
    std::vector<int> matrix_of;  // per finals entry: index in the packet's matrices, -1
    std::vector<int> state_of;   // per material: index in the packet's mat_states, -1
    // per vertex buffer: its texture coordinates' scale as an index (buffers that share one share it):
    // a material's items grouped by it want the backend's texture rows made fewer times
    std::vector<u16> coords_of;
    u32 n_coords = 1;            // (the scales' count)
    // the sort's keys, and the candidates gathered in their order (sort_pass)
    std::vector<u64> keys;
    std::vector<Cand> gathered;

    // ---- the sea (sea.h): its waves; the grid -- whether this frame draws it, its centre on the
    // lattice, its vertices' amplitudes there (made again when the centre moves), the stencil value of
    // the frame --; the flat surface's materials (fmt::MAT_SEA); the boats' trails
    Sea sea;
    bool grid_on = false, grid_any = false;
    f32 grid_c[2] = {0, 0}, grid_amp_c[2] = {NAN, NAN};
    std::vector<u8> grid_amp;
    u8 stencil = 0;
    std::vector<u8> mat_sea;
    struct TrailSample {
        f32 x, z;        // the stern where it was
        f32 dx, dz;      // its heading (unit)
        f32 t;           // when (seconds)
        f32 dist;        // the path's length there
        f32 strength;    // the boat's speed then, of Sea::trail_speed (0-1)
        f32 half;        // half its width
    };
    struct Trail {
        u32 inst;
        f32 half;        // half the boat's width
        f32 back;        // its stern from its origin, along its heading (model space times its scale)
        std::vector<TrailSample> samples;  // the oldest first
        TrailSample head{};                // the stern now
        bool started = false;
        f32 last_t = 0;
        f32 speed = 0;   // units a second, smoothed
    };
    std::vector<Trail> trails;
    f32 trail_time = 0;  // the clock the trails were last sampled at (seconds)
    std::vector<std::pair<f32, u32>> trail_pick;  // (trail_vertices' choice: distance, trail)
    bool trails_ok = false;  // the trail batch's layout is the one the ribbons are written in

    explicit Impl(const Package& p) : pkg(p), sea(p) {}

    void init();
    const NodeMap* node_map(u16 anim, u16 model) const;
    void pose(u16 model, const Mtx34& mtx, u16 anim, f32 anim_offset, u32 base,
              const std::vector<NodeRot>* rots = nullptr);
    void billboard(u16 model, u32 base);
    void pose_instance(u32 i, u16 model, f32 distance);
    int eye_cell() const;
    void cull();
    void collect();
    void add_model_items(const VisInst& v, f32 detail);
    void sort_pass(u32 pi);
    void build(Pass pass, Packet* p);
    void refresh_combiners(Dyn& d);
    Mtx34 root_now(u32 i) const;
    void sample_trails();
    u32 trail_vertices(Packet* p, const f32 origin[3]);
    Dyn& dyn_for(u32 m);
};

void World::Impl::init() {
    const fmt::Env& env = pkg.env();
    k = env.mii_units_per_unit;
    mats.resize(pkg.materials().count);
    for (u32 i = 0; i < mats.size(); i++)
        build_material(pkg, i, &mats[i]);
    cell_hidden.assign(pkg.cells().count, 0);
    model_hidden.assign(pkg.models().count, 0);
    grid = env.pvs_row_bytes && env.cell_size > 0 &&
           (uint64_t)env.n_cells_x * env.n_cells_z == (uint64_t)pkg.cells().count;
    // who uses each texture matrix record
    tmtx_users.assign(pkg.tex_mtx().count, {});
    for (u32 m = 0; m < pkg.materials().count; m++) {
        const fmt::Material& fm = pkg.materials()[m];
        for (u32 i = 0; i < fm.n_tex_mtx; i++)
            tmtx_users[fm.tex_mtx_first + i].push_back(m << 8 | i);
    }
    // the material animations and the materials they drive
    for (u16 a : {env.world_anim_srt, env.world_anim_clr, env.sea_anim_srt, env.sea_anim_clr, env.sky_anim})
        if (a != NONE && std::find(mat_anims.begin(), mat_anims.end(), a) == mat_anims.end())
            mat_anims.push_back(a);
    dyn_of.assign(mats.size(), -1);
    // (and every material the scene sets registers of: fmt::MAT_RUNTIME)
    dyn.reserve(mats.size());
    for (u32 m = 0; m < mats.size(); m++)
        if (pkg.materials()[m].flags & fmt::MAT_RUNTIME)
            dyn_for(m);

    for (u16 a : mat_anims) {
        const fmt::Anim& an = pkg.anims()[a];
        for (u32 t = an.first_track; t < an.first_track + an.n_tracks; t++) {
            const fmt::Track& tr = pkg.tracks()[t];
            if (tr.target >= fmt::TT_TEXMTX_SCALE_S && tr.target <= fmt::TT_TEXMTX_TRANS_T) {
                for (u32 user : tmtx_users[tr.index])
                    if ((user & 255) < 3) {
                        dyn_for(user >> 8);
                        mats[user >> 8].animated_tex_mtx |= 1u << (user & 255);
                    }
            } else if (tr.target >= fmt::TT_MAT_REG_R && tr.target <= fmt::TT_MAT_REG_A) {
                dyn_for(tr.index);
                mats[tr.index].animated_color = true;
            }
        }
    }
    // models with billboard nodes (turned every frame, whether posed again or not)
    model_billboards.assign(pkg.models().count, 0);
    for (u32 m = 0; m < pkg.models().count; m++) {
        const fmt::Model& md = pkg.models()[m];
        for (u32 n = 0; n < md.n_nodes; n++) {
            const fmt::Node& nd = pkg.nodes()[md.first_node + n];
            if ((nd.flags & fmt::NODE_BILLBOARD) && nd.billboard)
                model_billboards[m] = 1;
        }
    }
    mesh_radius.assign(pkg.models().count, 0);
    for (u32 m = 0; m < pkg.models().count; m++) {
        const fmt::Model& md = pkg.models()[m];
        for (u32 b = md.first_batch; b < md.first_batch + md.n_batches; b++) {
            const fmt::Batch& bt = pkg.batches()[b];
            Vec3 at{0, 0, 0};
            if (bt.node < md.n_nodes) {
                const fmt::Node& nd = pkg.nodes()[md.first_node + bt.node];
                at = {nd.bind[0][3], nd.bind[1][3], nd.bind[2][3]};
            }
            const Vec3 c = box_centre(bt.bbox), h = {bt.bbox[3] - c.x, bt.bbox[4] - c.y, bt.bbox[5] - c.z};
            mesh_radius[m] = std::max(mesh_radius[m], length(at + c) + length(h));
        }
    }
    // the most nodes a model of a chain has (a slot's size)
    auto chain_nodes = [&](u16 model) {
        u32 most = 1;
        for (u32 steps = 0; model != NONE && steps <= pkg.models().count; steps++) {
            most = std::max<u32>(most, pkg.models()[model].n_nodes);
            model = pkg.models()[model].lod_next;
        }
        return most;
    };
    // instances: their bounding spheres, their slots; their animations' node tracks
    anim_nodes.assign(pkg.anims().count, {});
    insts.resize(pkg.instances().count);
    u32 slots = 0;
    for (u32 i = 0; i < insts.size(); i++) {
        const fmt::Instance& in = pkg.instances()[i];
        const fmt::Model& m = pkg.models()[in.model];
        Inst& n = insts[i];
        n.mtx = to_mtx(in.mtx);
        n.scale = max_scale(n.mtx);
        n.centre = transform(n.mtx, box_centre(m.bbox));
        n.radius = m.radius * n.scale;
        n.slot = slots;
        slots += chain_nodes(in.model);
        n.animated = in.anim != NONE && (in.flags & fmt::INST_ANIMATED);
        if (n.animated && anim_nodes[in.anim].empty()) {
            const fmt::Anim& an = pkg.anims()[in.anim];
            for (u32 t = an.first_track; t < an.first_track + an.n_tracks; t++) {
                const fmt::Track& tr = pkg.tracks()[t];
                if (tr.target > fmt::TT_NODE_TRANS_Z)
                    continue;
                auto& list = anim_nodes[in.anim];
                auto it = std::find_if(list.begin(), list.end(),
                                       [&](const NodeTracks& nt) { return nt.model == tr.model && nt.node == tr.index; });
                if (it == list.end())
                    list.push_back({tr.model, tr.index, {}}), it = list.end() - 1;
                it->tracks.push_back(t);
            }
            // each node's tracks, by model
            for (u32 e = 0; e < anim_nodes[in.anim].size(); e++) {
                const NodeTracks& nt = anim_nodes[in.anim][e];
                auto mp = std::find_if(node_maps.begin(), node_maps.end(),
                                       [&](const NodeMap& x) { return x.anim == in.anim && x.model == nt.model; });
                if (mp == node_maps.end()) {
                    node_maps.push_back({in.anim, nt.model, std::vector<int>(pkg.models()[nt.model].n_nodes, -1)});
                    mp = node_maps.end() - 1;
                }
                if (nt.node < mp->of_node.size())
                    mp->of_node[nt.node] = (int)e;
            }
        }
        n.animated = n.animated && !anim_nodes[in.anim].empty();
    }
    if (env.sea_model != NONE)
        sea_slot = slots, slots += std::max<u32>(1, pkg.models()[env.sea_model].n_nodes);
    if (env.sky_model != NONE)
        sky_slot = slots, slots += std::max<u32>(1, pkg.models()[env.sky_model].n_nodes);
    finals.assign(slots, identity34());
    // the vertex buffers' coordinate scales, numbered
    std::vector<std::pair<f32, f32>> scales;
    coords_of.resize(pkg.vertex_buffers().count);
    for (u32 v = 0; v < coords_of.size(); v++) {
        const fmt::VertexBuffer& vb = pkg.vertex_buffers()[v];
        const std::pair<f32, f32> sc = (vb.flags & fmt::VB_UV_FLOAT) ? std::make_pair(1.0f, 1.0f)
                                                                     : std::make_pair(vb.uv_scale[0], vb.uv_scale[1]);
        auto it = std::find(scales.begin(), scales.end(), sc);
        coords_of[v] = (u16)(it - scales.begin());
        if (it == scales.end())
            scales.push_back(sc);
    }
    n_coords = std::max<u32>(1, (u32)scales.size());
    // the sea: the flat surface's materials, the grid's amplitudes, a trail for each boat that leaves one
    // (its width and stern from its model's box), if the trail batch has the ribbons' layout
    mat_sea.assign(mats.size(), 0);
    for (u32 m = 0; m < mats.size(); m++)
        mat_sea[m] = (pkg.materials()[m].flags & fmt::MAT_SEA) != 0;
    if (sea.valid()) {
        const fmt::Sea& sp = sea.params();
        grid_amp.resize(4 * (size_t)sea.grid_vertices());
        if (sp.trail_batch != NONE) {
            const fmt::VertexBuffer& vb = pkg.vertex_buffers()[pkg.batches()[sp.trail_batch].vbuf];
            trails_ok = vb.attrs == (fmt::VA_POS | fmt::VA_CLR | fmt::VA_UV0) && (vb.flags & fmt::VB_UV_FLOAT) &&
                        vb.stride == TRAIL_STRIDE;
        }
        for (u32 i = 0; i < insts.size() && trails_ok; i++) {
            if (!(pkg.instances()[i].flags & fmt::INST_TRAIL))
                continue;
            const fmt::Model& m = pkg.models()[pkg.instances()[i].model];
            Trail t;
            t.inst = i;
            // (model space: times the scale its root has as it goes)
            t.half = 0.5f * (m.bbox[3] - m.bbox[0]);
            t.back = m.bbox[2];
            trails.push_back(t);
        }
    }
    matrix_of.reserve(slots);
    state_of.reserve(mats.size());
    vis_cells.reserve(pkg.cells().count);
    vis_insts.reserve(insts.size() + 2);
}

// a material's dynamic block (made at its first want: at init only, while the blocks' vector may grow)
World::Impl::Dyn& World::Impl::dyn_for(u32 m) {
    if (dyn_of[m] < 0) {
        const fmt::Material& fm = pkg.materials()[m];
        Dyn d;
        d.d = MatDynamic();
        d.d.material = (u16)m;
        memcpy(d.d.tev_reg, fm.tev_reg, sizeof(d.d.tev_reg));
        memcpy(d.d.tev_konst, fm.tev_konst, sizeof(d.d.tev_konst));
        memcpy(d.d.mat_color, fm.mat_color, 4);
        memcpy(d.d.amb_color, fm.amb_color, 4);
        memcpy(d.d.tex_mtx, mats[m].tex_mtx, sizeof(d.d.tex_mtx));
        d.d.comb = mats[m].comb;
        for (u32 i = 0; i < 3; i++) {
            if (i < fm.n_tex_mtx) {
                const fmt::TexMtx& t = pkg.tex_mtx()[fm.tex_mtx_first + i];
                const f32 v[5] = {t.scale[0], t.scale[1], t.rotate, t.translate[0], t.translate[1]};
                memcpy(d.srt[i], v, sizeof(v));
            } else {
                const f32 v[5] = {1, 1, 0, 0, 0};
                memcpy(d.srt[i], v, sizeof(v));
            }
        }
        // (the packaged registers' translation is the first one kept)
        CombEntry& e = d.cache[0];
        memcpy(e.reg, d.d.tev_reg, sizeof(e.reg));
        memcpy(e.konst, d.d.tev_konst, sizeof(e.konst));
        e.comb = d.d.comb;
        e.valid = true;
        dyn_of[m] = (int)dyn.size();
        dyn.push_back(d);
    }
    return dyn[(u32)dyn_of[m]];
}

const World::Impl::NodeMap* World::Impl::node_map(u16 anim, u16 model) const {
    for (const NodeMap& m : node_maps)
        if (m.anim == anim && m.model == model)
            return &m;
    return nullptr;
}

// The world matrices of a model's nodes (into finals[base..]): at rest (the bind matrices) or posed
// by an animation's node tracks at the current frame, under `mtx` (billboards: billboard()).
void World::Impl::pose(u16 model, const Mtx34& mtx, u16 anim, f32 anim_offset, u32 base,
                       const std::vector<NodeRot>* rots) {
    const fmt::Model& m = pkg.models()[model];
    // (a model without nodes draws its batches with the matrix it is placed with)
    finals[base] = mtx;
    const NodeMap* map = anim != NONE ? node_map(anim, model) : nullptr;
    const std::vector<NodeTracks>* tracks = map ? &anim_nodes[anim] : nullptr;
    f32 f = 0;
    if (tracks) {
        const fmt::Anim& an = pkg.anims()[anim];
        f = anim_frame(frame + anim_offset, an.frames, an.loop);
    }
    for (u32 k = 0; k < m.n_nodes; k++) {
        const fmt::Node& nd = pkg.nodes()[m.first_node + k];
        Mtx34 local_world;  // model space
        const NodeTracks* nt = map && k < map->of_node.size() && map->of_node[k] >= 0 ? &(*tracks)[(u32)map->of_node[k]]
                                                                                       : nullptr;
        const NodeRot* turn = nullptr;
        if (rots)
            for (const NodeRot& nr : *rots)
                if (nr.node == k)
                    turn = &nr;
        if (nt || turn) {
            f32 s[3], r[3], t[3];
            memcpy(s, nd.scale, sizeof(s)), memcpy(r, nd.rotate, sizeof(r)), memcpy(t, nd.translate, sizeof(t));
            if (nt)
                for (u32 ti : nt->tracks) {
                    const fmt::Track& tr = pkg.tracks()[ti];
                    const f32 v = eval_track(tr, pkg.keys().data, f);
                    const u32 c = tr.target;
                    (c < 3 ? s : c < 6 ? r : t)[c % 3] = v;
                }
            if (turn)
                r[0] += turn->r[0], r[1] += turn->r[1], r[2] += turn->r[2];
            const Mtx34 local = srt_matrix(s, r, t);
            local_world = nd.parent == NONE ? local : concat(finals[base + nd.parent], local);
            // (finals holds model space until the instance matrix goes on below)
        } else if ((tracks || rots) && nd.parent != NONE) {
            // a node at rest under an animated parent: its rest pose relative to the parent
            const f32 s[3] = {nd.scale[0], nd.scale[1], nd.scale[2]};
            local_world = concat(finals[base + nd.parent], srt_matrix(s, nd.rotate, nd.translate));
        } else {
            local_world = to_mtx(nd.bind);
        }
        finals[base + k] = local_world;
    }
    for (u32 k = 0; k < m.n_nodes; k++)
        finals[base + k] = concat(mtx, finals[base + k]);
}

// A model's billboard nodes (in finals[base..]) turned to the camera, keeping their place and scale:
// Y billboards (nw4r modes 5, 6) about the world's y axis, the others wholly; the perspective modes
// (2, 4, 6) toward the eye, the rest along the view direction. (It reads only a matrix's place and
// its axes' lengths, which it keeps: turning a turned matrix again is turning the one it came from.)
void World::Impl::billboard(u16 model, u32 base) {
    const fmt::Model& m = pkg.models()[model];
    for (u32 k = 0; k < m.n_nodes; k++) {
        const fmt::Node& nd = pkg.nodes()[m.first_node + k];
        if (!(nd.flags & fmt::NODE_BILLBOARD) || !nd.billboard)
            continue;
        Mtx34& w = finals[base + k];
        const Vec3 pos = {w.m[0][3], w.m[1][3], w.m[2][3]};
        f32 sc[3];
        for (int j = 0; j < 3; j++)
            sc[j] = std::sqrt(w.m[0][j] * w.m[0][j] + w.m[1][j] * w.m[1][j] + w.m[2][j] * w.m[2][j]);
        const bool persp = nd.billboard == 2 || nd.billboard == 4 || nd.billboard == 6;
        Vec3 back = persp ? cam.eye - pos : cam.eye - cam.target;
        Vec3 x, y, z;
        if (nd.billboard >= 5) {
            back.y = 0;
            z = dot(back, back) == 0 ? Vec3{0, 0, 1} : normalize(back);
            y = {0, 1, 0};
            x = cross(y, z);
        } else {
            z = dot(back, back) == 0 ? Vec3{0, 0, 1} : normalize(back);
            x = cross(cam.up, z);
            x = dot(x, x) == 0 ? Vec3{1, 0, 0} : normalize(x);
            y = cross(z, x);
        }
        const Vec3 axes[3] = {x, y, z};
        for (int j = 0; j < 3; j++) {
            w.m[0][j] = axes[j].x * sc[j], w.m[1][j] = axes[j].y * sc[j], w.m[2][j] = axes[j].z * sc[j];
        }
        w.m[0][3] = pos.x, w.m[1][3] = pos.y, w.m[2][3] = pos.z;
    }
}

// An instance drawn this frame as `model`: posed again if its model changed, or if it is animated
// and due: every update, or with WF_ANIM_LOD every second one while its meshes are under
// ANIM_HALF_PX pixels across on the 3DS's screen and its nodes move under ANIM_STEP_PX a frame there,
// every fourth under ANIM_QUARTER_PX and a third of ANIM_STEP_PX (what it lags behind stays within a
// pixel and a half); its billboards turned.
void World::Impl::pose_instance(u32 i, u16 model, f32 distance) {
    Inst& n = insts[i];
    const fmt::Instance& in = pkg.instances()[i];
    u32 rate = 1;
    if (n.animated && (features & WF_ANIM_LOD) && n.posed_model == model) {
        // (pixels a unit is at its distance; its size and speed in pixels)
        const f32 tv = std::tan(cam.fovy_deg * 0.5f * 3.14159265f / 180.0f);
        const f32 px = SCREEN_HALF_PX / (std::max(length(n.at - cam.eye), 1e-3f) * tv);
        const f32 size = 2 * mesh_radius[model] * n.scale * px, step = n.speed * px;
        rate = size < ANIM_QUARTER_PX && 3 * step <= ANIM_STEP_PX ? 4 : size < ANIM_HALF_PX && step <= ANIM_STEP_PX ? 2 : 1;
    }
    (void)distance;
    const bool due = n.posed_model != model || (n.animated && tick - n.posed_tick >= rate);
    if (due) {
        // (its nodes' places before, for how fast they move)
        const fmt::Model& m = pkg.models()[model];
        const u32 nn = std::max<u32>(m.n_nodes, 1);
        Vec3 was[64];
        const bool track = n.animated && n.posed_model == model && nn <= 64;
        if (track)
            for (u32 k = 0; k < nn; k++)
                was[k] = {finals[n.slot + k].m[0][3], finals[n.slot + k].m[1][3], finals[n.slot + k].m[2][3]};
        pose(model, n.mtx, n.animated ? in.anim : NONE, in.anim_offset, n.slot,
             model == in.model && !n.rots.empty() ? &n.rots : nullptr);
        if (track) {
            f32 most = 0;
            for (u32 k = 0; k < nn; k++)
                most = std::max(most, length(Vec3{finals[n.slot + k].m[0][3], finals[n.slot + k].m[1][3],
                                                  finals[n.slot + k].m[2][3]} - was[k]));
            n.speed = most / (f32)std::max<u32>(1, tick - n.posed_tick);
        } else {
            n.speed = 0;
        }
        // (where it is: of its nodes' places, the nearest the eye)
        f32 nearest = 1e30f;
        for (u32 k = 0; k < nn; k++) {
            const Vec3 p = {finals[n.slot + k].m[0][3], finals[n.slot + k].m[1][3], finals[n.slot + k].m[2][3]};
            const f32 d = length(p - cam.eye);
            if (d < nearest)
                nearest = d, n.at = p;
        }
        // its meshes' sphere as posed: about their boxes' centres' box, out to the furthest box's corner
        if (n.animated) {
            Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
            for (u32 b = m.first_batch; b < m.first_batch + m.n_batches; b++) {
                const fmt::Batch& bt = pkg.batches()[b];
                const Vec3 c = transform(finals[n.slot + (bt.node < m.n_nodes ? bt.node : 0)], box_centre(bt.bbox));
                lo = {std::min(lo.x, c.x), std::min(lo.y, c.y), std::min(lo.z, c.z)};
                hi = {std::max(hi.x, c.x), std::max(hi.y, c.y), std::max(hi.z, c.z)};
            }
            n.mesh_centre = (lo + hi) * 0.5f;
            n.mesh_r = 0;
            for (u32 b = m.first_batch; b < m.first_batch + m.n_batches; b++) {
                const fmt::Batch& bt = pkg.batches()[b];
                const Mtx34& w = finals[n.slot + (bt.node < m.n_nodes ? bt.node : 0)];
                const Vec3 c = transform(w, box_centre(bt.bbox));
                const Vec3 h = {bt.bbox[3] - bt.bbox[0], bt.bbox[4] - bt.bbox[1], bt.bbox[5] - bt.bbox[2]};
                n.mesh_r = std::max(n.mesh_r, length(c - n.mesh_centre) + 0.5f * length(h) * max_scale(w));
            }
        }
        n.posed_tick = tick;
        n.posed_model = model;
        stats->poses++;
    }
    if (model_billboards[model])
        billboard(model, n.slot);
}

// the cell whose visibility row applies to the eye, -1 if none does
int World::Impl::eye_cell() const {
    if (!grid)
        return -1;
    const fmt::Env& e = pkg.env();
    const f32 fx = (cam.eye.x - e.grid_origin[0]) / e.cell_size, fz = (cam.eye.z - e.grid_origin[1]) / e.cell_size;
    if (!(fx >= 0 && fx < (f32)e.n_cells_x && fz >= 0 && fz < (f32)e.n_cells_z))
        return -1;
    const u32 i = std::min((u32)fz, e.n_cells_z - 1) * e.n_cells_x + std::min((u32)fx, e.n_cells_x - 1);
    const f32 ground = std::isfinite(eye_ground) ? eye_ground : pkg.cells()[i].bbox[1];
    return cam.eye.y - ground <= e.pvs_max_height ? (int)i : -1;
}

// this frame's visible cells and instances, with their detail levels and poses; then the passes'
// candidates
void World::Impl::cull() {
    vis_cells.clear();
    vis_insts.clear();
    // (the combiners' translations are counted after this, as the packets are built: until the next
    // cull the stats say what the last frame did, as a scene reading them before its draws sees them)
    *stats = WorldStats();
    pass_tris[0] = pass_tris[1] = 0;
    stats->cells_total = pkg.cells().count;
    stats->instances_total = pkg.instances().count;
    const Vec3 eye = cam.eye;
    const f32 tv = std::tan(cam.fovy_deg * 0.5f * 3.14159265f / 180.0f);
    const u8* row = nullptr;
    if (features & WF_VISIBILITY) {
        stats->pvs_cell = eye_cell();
        if (stats->pvs_cell >= 0)
            row = pkg.visibility_row((u32)stats->pvs_cell);
    }
    for (u32 i = 0; i < pkg.cells().count; i++) {
        const fmt::Cell& c = pkg.cells()[i];
        if (cell_hidden[i] || !box_visible(fr, c.bbox))
            continue;
        const f32 d = box_distance(c.bbox, eye);
        if (cell_distance > 0 && d > cell_distance)
            continue;
        if (row && !(row[i >> 3] >> (i & 7) & 1)) {
            stats->cells_pvs_hidden++;
            continue;
        }
        u8 lod = 0;
        while (lod + 1 < c.n_lods && d >= c.lod_distance[lod + 1] * lod_scale)
            lod++;
        vis_cells.push_back({i, lod, d});
    }
    stats->cells_visible = (u32)vis_cells.size();
    for (u32 i = 0; i < insts.size(); i++) {
        const fmt::Instance& in = pkg.instances()[i];
        const Inst& n = insts[i];
        if (model_hidden[in.model] || !sphere_visible(fr, n.centre, n.radius))
            continue;
        const f32 d = length(n.centre - eye);
        const f32 near_d = std::max(d - n.radius, 0.0f);
        if ((in.cull_distance > 0 && near_d > in.cull_distance) || (inst_distance > 0 && near_d > inst_distance))
            continue;
        // its LOD chain: the next model from that model's distance on
        u16 model = in.model;
        for (u32 steps = 0; steps < pkg.models().count; steps++) {
            const fmt::Model& m = pkg.models()[model];
            if (m.lod_next == NONE || d < m.lod_distance * lod_scale)
                break;
            model = m.lod_next;
        }
        pose_instance(i, model, d);
        // an animated one: where its meshes are now (its sphere spans the path they move on), and
        // whether they are a pixel across there
        if (n.animated && (features & WF_DETAIL)) {
            const f32 dm = std::max(length(n.mesh_centre - eye) - n.mesh_r, 1e-3f);
            if (!sphere_visible(fr, n.mesh_centre, n.mesh_r) || 2 * n.mesh_r * SCREEN_HALF_PX < SIZE_CULL_PX * dm * tv) {
                stats->instances_small++;
                continue;
            }
        }
        vis_insts.push_back({i, model, d, n.slot});
    }
    stats->instances_visible = (u32)vis_insts.size();
    const fmt::Env& env = pkg.env();
    if (env.sea_model != NONE) {
        if (!sea_posed)
            pose(env.sea_model, identity34(), NONE, 0, sea_slot), sea_posed = true;
        if (model_billboards[env.sea_model])
            billboard(env.sea_model, sea_slot);
        vis_insts.push_back({~0u, env.sea_model, 0, sea_slot});
    }
    if (env.sky_model != NONE) {
        pose(env.sky_model, translation(eye), NONE, 0, sky_slot);
        vis_insts.push_back({~1u, env.sky_model, 0, sky_slot});
    }
    // the sea's wave grid: centred under the eye on its lattice; drawn while the eye is low enough
    // over the sea, its box (its waves' reach up and down) in view, and with waves somewhere in it (its
    // vertices' amplitudes, made again when the centre moves)
    grid_on = false;
    if (sea.valid() && sea.params().grid_batch != NONE) {
        const fmt::Sea& sp = sea.params();
        grid_c[0] = std::round(eye.x / sp.grid_snap) * sp.grid_snap;
        grid_c[1] = std::round(eye.z / sp.grid_snap) * sp.grid_snap;
        if (eye.y - sp.level <= sp.grid_height) {
            if (grid_c[0] != grid_amp_c[0] || grid_c[1] != grid_amp_c[1]) {
                grid_amp_c[0] = grid_c[0], grid_amp_c[1] = grid_c[1];
                grid_any = sea.grid_amplitudes(grid_c[0], grid_c[1], grid_amp.data());
            }
            f32 reach = 0;
            for (const fmt::SeaWave& wv : sp.waves)
                reach += std::fabs(wv.amplitude);
            const f32 r = sp.grid_radius;
            const f32 box[6] = {grid_c[0] - r, sp.level - reach, grid_c[1] - r, grid_c[0] + r, sp.level + reach, grid_c[1] + r};
            grid_on = grid_any && box_visible(fr, box);
        }
    }
    stats->sea_grid = grid_on;
    collect();
}

// an instance's root node where it is now (its matrix; an animated one's root posed at this frame)
Mtx34 World::Impl::root_now(u32 i) const {
    const Inst& n = insts[i];
    const fmt::Instance& in = pkg.instances()[i];
    const fmt::Model& m = pkg.models()[in.model];
    const NodeMap* map = n.animated ? node_map(in.anim, in.model) : nullptr;
    if (!map || !m.n_nodes || map->of_node.empty() || map->of_node[0] < 0)
        return m.n_nodes ? concat(n.mtx, to_mtx(pkg.nodes()[m.first_node].bind)) : n.mtx;
    const fmt::Node& nd = pkg.nodes()[m.first_node];
    f32 sv[3], rv[3], tv[3];
    memcpy(sv, nd.scale, sizeof(sv)), memcpy(rv, nd.rotate, sizeof(rv)), memcpy(tv, nd.translate, sizeof(tv));
    const fmt::Anim& an = pkg.anims()[in.anim];
    const f32 f = anim_frame(frame + in.anim_offset, an.frames, an.loop);
    for (u32 ti : anim_nodes[in.anim][(u32)map->of_node[0]].tracks) {
        const fmt::Track& tr = pkg.tracks()[ti];
        const u32 c = tr.target;
        (c < 3 ? sv : c < 6 ? rv : tv)[c % 3] = eval_track(tr, pkg.keys().data, f);
    }
    return concat(n.mtx, srt_matrix(sv, rv, tv));
}

// The boats' trails at this update: each one's stern now (its root's place less its model's back along
// its heading), its speed (smoothed over a quarter second), a sample each Sea::trail_spacing of its path
// while it moves, the samples older than Sea::trail_life or beyond its ribbon's length dropped. A jump
// (a warp, a boat put somewhere) starts its path again.
void World::Impl::sample_trails() {
    if (trails.empty())
        return;
    const fmt::Sea& sp = sea.params();
    const f32 now = frame / 60.0f, dt = now - trail_time;
    trail_time = now;
    for (Trail& t : trails) {
        const Mtx34 M = root_now(t.inst);
        const f32 fx = M.m[0][2], fz = M.m[2][2], fl = std::sqrt(fx * fx + fz * fz);
        if (!(fl > 1e-6f))
            continue;
        const f32 sc = max_scale(M);
        TrailSample h;
        h.dx = fx / fl, h.dz = fz / fl;
        h.x = M.m[0][3] + h.dx * t.back * sc, h.z = M.m[2][3] + h.dz * t.back * sc;
        h.t = now;
        h.half = t.half * sc;
        const f32 d = t.started ? std::sqrt((h.x - t.head.x) * (h.x - t.head.x) + (h.z - t.head.z) * (h.z - t.head.z)) : 0.0f;
        if (!t.started || d > 10 * sp.trail_spacing + 400.0f * std::max(dt, 0.0f)) {
            t.samples.clear();
            t.speed = 0;
            h.dist = 0;
        } else {
            h.dist = t.head.dist + d;
            if (dt > 0)
                t.speed += (d / dt - t.speed) * std::min(1.0f, dt / 0.25f);
        }
        t.started = true;
        h.strength = std::min(1.0f, t.speed / std::max(sp.trail_speed, 1e-3f));
        t.head = h;
        if (h.strength > 0.02f && (t.samples.empty() || h.dist - t.samples.back().dist >= sp.trail_spacing))
            t.samples.push_back(h);
        while (!t.samples.empty() && (now - t.samples.front().t > sp.trail_life || t.samples.size() + 1 > sp.trail_samples))
            t.samples.erase(t.samples.begin());
    }
}

// The trails' ribbons into the packet's vertices (the trail batch's layout and order: ribbon k's point
// j's two sides at vertices 2 (k samples + j) and + 1), about `origin`: the nearest Sea::trail_boats
// trails with samples, each from its stern now back along its samples, half the boat's width wider a
// little more each second, fading with age (and in behind the stern), on the waves under them. The
// rest of a ribbon's vertices repeat its last point, unseen (no area); a ribbon not used is all at
// the origin. The ribbons drawn, 0 if none.
u32 World::Impl::trail_vertices(Packet* p, const f32 origin[3]) {
    const fmt::Sea& sp = sea.params();
    const u32 nb = sp.trail_boats, ns = sp.trail_samples;
    // (the nearest, by their sterns)
    trail_pick.clear();
    for (u32 i = 0; i < trails.size(); i++) {
        const Trail& t = trails[i];
        const f32 dx = t.head.x - cam.eye.x, dz = t.head.z - cam.eye.z, d = std::sqrt(dx * dx + dz * dz);
        if (!t.samples.empty() && d <= TRAIL_RANGE)
            trail_pick.push_back({d, i});
    }
    const u32 n = std::min<u32>((u32)trail_pick.size(), nb);
    if (!n)
        return 0;
    std::partial_sort(trail_pick.begin(), trail_pick.begin() + n, trail_pick.end());
    p->dynamic.assign((size_t)2 * nb * ns * TRAIL_STRIDE, 0);
    const f32 now = frame / 60.0f;
    SeaWaves w;
    sea_waves(sp, now, grid_c[0], grid_c[1], &w);
    u8* out = p->dynamic.data();
    auto put = [&](u32 v, f32 x, f32 y, f32 z, f32 a, f32 s, f32 tt) {
        u8* q = out + (size_t)v * TRAIL_STRIDE;
        const f32 pos[3] = {x - origin[0], y - origin[1], z - origin[2]};
        memcpy(q, pos, 12);
        q[12] = q[13] = q[14] = 255;
        q[15] = (u8)std::min(255.0f, std::max(0.0f, a * 255.0f + 0.5f));
        const f32 uv[2] = {s, tt};
        memcpy(q + 16, uv, 8);
    };
    for (u32 k = 0; k < n; k++) {
        const Trail& t = trails[trail_pick[k].second];
        u32 j = 0;
        const u32 base = 2 * k * ns;
        auto point = [&](const TrailSample& q) {
            const f32 age = now - q.t;
            const f32 a = q.strength * std::max(0.0f, 1.0f - age / sp.trail_life) * std::min(1.0f, age / TRAIL_FADE_IN);
            const f32 half = q.half + sp.trail_spread * age;
            const f32 sx = q.dz * half, sz = -q.dx * half;
            for (int e = 0; e < 2; e++) {
                const f32 x = q.x + (e ? -sx : sx), z = q.z + (e ? -sz : sz);
                f32 h, ddx, ddz;
                sea_wave(w, x - grid_c[0], z - grid_c[1], sea.amplitude(x, z), &h, &ddx, &ddz);
                put(base + 2 * j + e, x, sp.level + h + TRAIL_LIFT, z, a, (f32)e, std::fmod(q.dist / TRAIL_REPEAT, 4096.0f));
            }
            j++;
        };
        point(t.head);
        for (size_t i = t.samples.size(); i-- > 0 && j < ns;)
            point(t.samples[i]);
        // (the rest: the last point again, unseen)
        for (; j < ns; j++)
            for (int e = 0; e < 2; e++) {
                memcpy(out + (size_t)(base + 2 * j + e) * TRAIL_STRIDE, out + (size_t)(base + 2 * (j - 1) + e) * TRAIL_STRIDE,
                       TRAIL_STRIDE);
                out[(size_t)(base + 2 * j + e) * TRAIL_STRIDE + 15] = 0;
            }
    }
    return n;
}

// a model's batches as candidates of their passes (the sky's all in the opaque pass, first)
void World::Impl::add_model_items(const VisInst& v, f32 detail) {
    const fmt::Model& m = pkg.models()[v.model];
    const bool sky = v.inst == ~1u, sea = v.inst == ~0u;
    for (u32 b = m.first_batch; b < m.first_batch + m.n_batches; b++) {
        const fmt::Batch& bt = pkg.batches()[b];
        const Material& mat = mats[bt.material];
        if (mat.pixel.cull_gx == 3)
            continue;  // (GX culls all of it)
        const u32 pass = !sky && mat.translucent;
        const u32 fin = v.first_final + (bt.node < m.n_nodes ? bt.node : 0);
        f32 box[6];
        transform_box(finals[fin], bt.bbox, box);
        // (the sea's batches are culled one by one: it spans the world)
        if (sea && !box_visible(fr, box))
            continue;
        if (!sky && detail > 0 && (bt.flags & fmt::BATCH_DETAIL) && box_distance(box, cam.eye) > detail) {
            stats->details_culled++;
            continue;
        }
        std::vector<Cand>& out = cands[pass];
        Cand c;
        c.priority = sky ? -1000 : mat.priority;
        c.distance = sky ? 0 : pass ? length(box_centre(box) - cam.eye) : sea ? box_distance(box, cam.eye) : v.distance;
        c.order = (u32)out.size();
        c.final = fin;
        c.material = bt.material;
        c.coords = coords_of[bt.vbuf];
        c.plain = !sky && is_plain(mat);
        c.item = {(u16)b, 0, 0xFFFF, (u16)(sky ? DRAW_SKY : grid_on && mat_sea[bt.material] ? DRAW_SEA_MASKED : 0)};
        c.tris = bt.n_indices / 3;
        out.push_back(c);
    }
}

// the candidates of both passes: the sky's first, the cells' batches at their level, the models'
// (instances, sea); detail batches beyond the detail distance left out
void World::Impl::collect() {
    cands[0].clear(), cands[1].clear();
    sorted[0] = sorted[1] = false;
    const f32 detail = (features & WF_DETAIL) ? pkg.env().detail_distance * lod_scale : 0.0f;
    for (const VisInst& v : vis_insts)
        if (v.inst == ~1u)
            add_model_items(v, detail);
    n_sky = (u32)cands[0].size();
    for (const VisCell& vc : vis_cells) {
        const fmt::Cell& c = pkg.cells()[vc.cell];
        for (u32 i = 0; i < c.count[vc.lod]; i++) {
            const u16 b = pkg.cell_batches()[c.first[vc.lod] + i];
            const fmt::Batch& bt = pkg.batches()[b];
            const Material& mat = mats[bt.material];
            if (mat.pixel.cull_gx == 3)
                continue;
            // (the cell's box meets the view; each of its batches is tested by its own, in world space)
            if (!box_visible(fr, bt.bbox))
                continue;
            if (detail > 0 && (bt.flags & fmt::BATCH_DETAIL) && box_distance(bt.bbox, cam.eye) > detail) {
                stats->details_culled++;
                continue;
            }
            const u32 pass = mat.translucent;
            std::vector<Cand>& out = cands[pass];
            Cand cd;
            cd.priority = mat.priority;
            cd.distance = pass ? length(box_centre(bt.bbox) - cam.eye) : vc.distance;
            cd.order = (u32)out.size();
            cd.final = ~0u;
            cd.material = bt.material;
            cd.coords = coords_of[bt.vbuf];
            cd.plain = is_plain(mat);
            cd.item = {b, 0, 0xFFFF, (u16)(grid_on && mat_sea[bt.material] ? DRAW_SEA_MASKED : 0)};
            cd.tris = bt.n_indices / 3;
            out.push_back(cd);
        }
    }
    for (const VisInst& v : vis_insts)
        if (v.inst != ~1u)
            add_model_items(v, detail);
}

// a material's combiners for its current registers: one of the last translations, or a new one
void World::Impl::refresh_combiners(Dyn& d) {
    d.comb_stale = false;
    CombEntry* oldest = &d.cache[0];
    for (CombEntry& e : d.cache) {
        if (e.valid && !memcmp(e.reg, d.d.tev_reg, sizeof(e.reg)) && !memcmp(e.konst, d.d.tev_konst, sizeof(e.konst))) {
            e.used = tick;
            d.d.comb = e.comb;
            stats->translations_cached++;
            return;
        }
        if (!e.valid || (oldest->valid && e.used < oldest->used))
            oldest = &e;
    }
    const fmt::Material& fm = pkg.materials()[d.d.material];
    engine::gx::tev_to_combiners(tev_regs(fm, d.d.tev_reg, d.d.tev_konst), d.d.comb);
    if (const u32 lum = mats[d.d.material].lum_units)
        engine::gx::remap_luminance(d.d.comb.stage, lum);
    memcpy(oldest->reg, d.d.tev_reg, sizeof(oldest->reg));
    memcpy(oldest->konst, d.d.tev_konst, sizeof(oldest->konst));
    oldest->comb = d.d.comb;
    oldest->used = tick;
    oldest->valid = true;
    stats->translations++;
}

// A pass's candidates in their order (once a cull): the sky's as they come (the opaque pass's first
// n_sky); then by priority; opaque: the plain ones by material and their buffers' coordinate scale
// (WF_MATERIAL_SORT), then front to back; translucent: back to front; then as they came. Sorted as one
// 64-bit key a candidate (priority, the plain ones' material and scale, the distance's bits -- a
// non-negative float orders as its bits; back to front: complemented --, the place it came in), the
// candidates then gathered in the keys' order: the same order as comparing the candidates (the keys
// are unique and hold every field the comparison reads), for a sort of 8-byte keys instead of the
// candidates. Where the fields don't fit (more candidates or materials than the bits left) or a
// distance is negative or NaN, the candidates are sorted by comparing them.
void World::Impl::sort_pass(u32 pi) {
    std::vector<Cand>& cs = cands[pi];
    const u32 first = pi ? 0 : n_sky, n = (u32)cs.size() - first;
    if (n < 2)
        return;
    const bool by_material = !pi && (features & WF_MATERIAL_SORT);
    auto bits = [](u64 v) {  // (bits that hold 0..v)
        u32 b = 0;
        while (v >> b)
            b++;
        return b;
    };
    const u64 n_groups = (u64)mats.size() * n_coords;  // (the plain ones' (material, scale); the rest after)
    const u32 order_bits = bits(n - 1), group_bits = by_material ? bits(n_groups) : 0;
    bool keyed = 8 + group_bits + 32 + order_bits <= 64;
    if (keyed) {
        keys.resize(n);
        for (u32 i = 0; i < n && keyed; i++) {
            const Cand& c = cs[first + i];
            const f32 d = c.distance + 0.0f;  // (-0 as 0: they compare equal)
            u32 db;
            memcpy(&db, &d, 4);
            if (db > 0x7F800000u) {  // (negative or NaN)
                keyed = false;
                break;
            }
            const u64 group = !by_material ? 0 : c.plain ? (u64)c.material * n_coords + c.coords : n_groups;
            keys[i] = ((u64)(u8)(c.priority + 128) << (group_bits + 32 + order_bits)) |
                      (group << (32 + order_bits)) | ((u64)(pi ? ~db : db) << order_bits) | i;
        }
    }
    if (keyed) {
        std::sort(keys.begin(), keys.end());
        const u64 mask = ((u64)1 << order_bits) - 1;
        gathered.resize(cs.size());
        std::copy(cs.begin(), cs.begin() + first, gathered.begin());
        for (u32 i = 0; i < n; i++)
            gathered[first + i] = cs[first + (u32)(keys[i] & mask)];
        cs.swap(gathered);
        return;
    }
    if (pi)
        std::sort(cs.begin(), cs.end(), [](const Cand& a, const Cand& b) {
            if (a.priority != b.priority)
                return a.priority < b.priority;
            if (a.distance != b.distance)
                return a.distance > b.distance;
            return a.order < b.order;
        });
    else
        std::sort(cs.begin() + n_sky, cs.end(), [by_material](const Cand& a, const Cand& b) {
            if (a.priority != b.priority)
                return a.priority < b.priority;
            if (by_material) {
                if (a.plain != b.plain)
                    return a.plain;
                if (a.plain && a.material != b.material)
                    return a.material < b.material;
                if (a.plain && a.coords != b.coords)
                    return a.coords < b.coords;
            }
            if (a.distance != b.distance)
                return a.distance < b.distance;
            return a.order < b.order;
        });
}

void World::Impl::build(Pass pass, Packet* p) {
    if (dirty) {
        cull();
        dirty = false;
    }
    p->kind = PacketKind::DRAW;
    p->package = &pkg;
    p->materials = mats.data();
    p->pass = pass;
    memcpy(p->view, view_guest.m, sizeof(p->view));
    p->world_scale = k;
    p->eye[0] = cam.eye.x, p->eye[1] = cam.eye.y, p->eye[2] = cam.eye.z;
    p->time = frame;
    // the environment: lights (a point light shines toward the origin: the sun's position), sets, fogs
    p->lights.clear();
    for (const fmt::Light& l : pkg.lights()) {
        Vec3 d = l.kind == fmt::LK_DIRECTIONAL ? Vec3{l.direction[0], l.direction[1], l.direction[2]}
                                               : Vec3{-l.position[0], -l.position[1], -l.position[2]};
        d = dot(d, d) == 0 ? Vec3{0, -1, 0} : normalize(d);
        Packet::Light pl = {{d.x, d.y, d.z}, {l.rgba[0], l.rgba[1], l.rgba[2], l.rgba[3]}};
        p->lights.push_back(pl);
    }
    p->light_sets.assign(pkg.light_sets().begin(), pkg.light_sets().end());
    p->fogs.assign(pkg.fogs().begin(), pkg.fogs().end());

    // the order (once a cull: sort_pass)
    const u32 pi = pass == Pass::TRANSLUCENT;
    std::vector<Cand>& cs = cands[pi];
    if (!sorted[pi]) {
        sorted[pi] = true;
        sort_pass(pi);
    }
    // the items, with their matrices and material states put in the packet as they are first used
    matrix_of.assign(finals.size(), -1);
    state_of.assign(mats.size(), -1);
    static const f32 k_identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    p->matrices.assign(k_identity, k_identity + 12);
    p->mat_states.clear();
    p->items.clear();
    p->items.reserve(cs.size() + 2);
    p->sea.amp.clear();
    p->dynamic.clear();
    u32 tris = 0;
    // (a material's dynamic state into the packet at its first use)
    auto state = [&](u16 material) -> u16 {
        if (dyn_of[material] < 0)
            return 0xFFFF;
        int& si = state_of[material];
        if (si < 0) {
            Dyn& d = dyn[(u32)dyn_of[material]];
            if (d.comb_stale)
                refresh_combiners(d);
            si = (int)p->mat_states.size();
            p->mat_states.push_back(d.d);
        }
        return (u16)si;
    };
    auto translated = [&](f32 x, f32 y, f32 z) {
        const f32 m[12] = {1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z};
        p->matrices.insert(p->matrices.end(), m, m + 12);
        return (u16)(p->matrices.size() / 12 - 1);
    };
    // the sea's wave grid (after the sky: before the flat surface it masks), and its frame's waves
    const fmt::Sea* sp = sea.valid() ? &sea.params() : nullptr;
    auto add_grid = [&] {
        const fmt::Batch& bt = pkg.batches()[sp->grid_batch];
        Packet::SeaDraw& sd = p->sea;
        sd.origin[0] = grid_c[0], sd.origin[1] = sp->level, sd.origin[2] = grid_c[1];
        sea_waves(*sp, frame / 60.0, grid_c[0], grid_c[1], &sd.waves);
        sd.eye[0] = cam.eye.x - grid_c[0], sd.eye[1] = cam.eye.y - sp->level, sd.eye[2] = cam.eye.z - grid_c[1];
        stencil = (u8)(stencil % 255 + 1);
        sd.stencil = stencil;
        sd.amp.assign(grid_amp.begin(), grid_amp.end());
        p->items.push_back({sp->grid_batch, translated(grid_c[0], sp->level, grid_c[1]), state(bt.material), DRAW_SEA_GRID});
        tris += bt.n_indices / 3;
    };
    if (pi == 0 && grid_on && n_sky == 0)
        add_grid();
    // the boats' trails (first in the translucent pass: on the water, under whatever is over it)
    stats->trails = 0;
    if (pi == 1 && sp && trails_ok) {
        const f32 o[3] = {grid_c[0], sp->level, grid_c[1]};
        if (const u32 n = trail_vertices(p, o)) {
            const fmt::Batch& bt = pkg.batches()[sp->trail_batch];
            p->items.push_back({sp->trail_batch, translated(o[0], o[1], o[2]), state(bt.material), DRAW_DYNAMIC});
            tris += n * 2 * (sp->trail_samples - 1u);
            stats->trails = n;
        }
    }
    for (u32 ci = 0; ci < cs.size(); ci++) {
        const Cand& c = cs[ci];
        DrawItem it = c.item;
        if (c.final != ~0u) {
            int& mi = matrix_of[c.final];
            if (mi < 0) {
                mi = (int)(p->matrices.size() / 12);
                const f32* f = &finals[c.final].m[0][0];
                p->matrices.insert(p->matrices.end(), f, f + 12);
            }
            it.matrix = (u16)mi;
        }
        it.mat_state = state(c.material);
        p->items.push_back(it);
        tris += c.tris;
        if (pi == 0 && grid_on && ci + 1 == n_sky)
            add_grid();
    }
    (pass == Pass::OPAQUE ? stats->items_opaque : stats->items_translucent) = (u32)p->items.size();
    pass_tris[pi] = tris;
    stats->triangles = pass_tris[0] + pass_tris[1];
}

// ---------------------------------------------------------------- World
World::World(const Package& package) : impl_(new Impl(package)), package_(package) {
    impl_->stats = &stats_;
    impl_->init();
    set_camera(Camera());
}

World::~World() { delete impl_; }

f32 World::world_scale() const { return impl_->k; }
void World::set_world_scale(f32 k) {
    if (k > 0)
        impl_->k = k;
}
const std::vector<Material>& World::materials() const { return impl_->mats; }

void World::set_group_visible(const char* group, bool visible) {
    for (u32 i = 0; i < package_.cells().count; i++)
        if (!strcmp(package_.str(package_.cells()[i].group), group))
            impl_->cell_hidden[i] = !visible;
    const int m = package_.find_model(group);
    for (u32 i = 0; i < package_.models().count; i++)
        if (!strcmp(package_.str(package_.models()[i].name), group) || (int)i == m)
            impl_->model_hidden[i] = !visible;
    impl_->dirty = true;
}

void World::set_draw_distance(f32 cells, f32 instances) {
    impl_->cell_distance = cells, impl_->inst_distance = instances;
    impl_->dirty = true;
}

u32 World::instance_count() const { return (u32)impl_->insts.size(); }

u16 World::instance_class(u32 instance) const {
    return instance < package_.instances().count ? package_.instances()[instance].class_id : NONE;
}

bool World::instance_animated(u32 instance) const {
    if (instance >= package_.instances().count)
        return false;
    const fmt::Instance& in = package_.instances()[instance];
    return in.anim != NONE && (in.flags & fmt::INST_ANIMATED);
}

Mtx34 World::instance_transform(u32 instance) const {
    return instance < impl_->insts.size() ? impl_->insts[instance].mtx : identity34();
}

bool World::set_instance_transform(u32 instance, const Mtx34& matrix) {
    if (instance >= impl_->insts.size())
        return false;
    Impl::Inst& n = impl_->insts[instance];
    const fmt::Instance& in = package_.instances()[instance];
    const fmt::Model& model = package_.models()[in.model];
    n.mtx = matrix;
    n.scale = max_scale(n.mtx);
    n.centre = transform(n.mtx, box_centre(model.bbox));
    n.radius = model.radius * n.scale;
    n.posed_model = NONE;
    n.posed_tick = 0;
    n.at = n.centre;
    n.speed = 0;
    impl_->dirty = true;
    return true;
}

int World::instance_node(u32 instance, const char* name) const {
    if (instance >= impl_->insts.size())
        return -1;
    const fmt::Model& m = package_.models()[package_.instances()[instance].model];
    for (u32 k = 0; k < m.n_nodes; k++)
        if (!std::strcmp(package_.str(package_.nodes()[m.first_node + k].name), name))
            return (int)k;
    return -1;
}

void World::set_instance_node_rotation(u32 instance, u16 node, const f32 degrees[3]) {
    if (instance >= impl_->insts.size())
        return;
    Impl::Inst& n = impl_->insts[instance];
    auto it = std::find_if(n.rots.begin(), n.rots.end(), [&](const Impl::NodeRot& r) { return r.node == node; });
    if (it == n.rots.end()) {
        n.rots.push_back({node, {0, 0, 0}});
        it = n.rots.end() - 1;
    } else if (!std::memcmp(it->r, degrees, sizeof(it->r))) {
        return;
    }
    std::memcpy(it->r, degrees, sizeof(it->r));
    n.posed_model = NONE;  // (posed again when next drawn)
    impl_->dirty = true;
}

void World::set_material_register(u16 material, u8 reg, const s16 rgba[4]) {
    Impl& w = *impl_;
    if (material >= w.mats.size() || w.dyn_of[material] < 0)
        return;
    Impl::Dyn& dy = w.dyn[(u32)w.dyn_of[material]];
    MatDynamic& d = dy.d;
    for (int c = 0; c < 4; c++) {
        switch (reg) {
        case fmt::CR_MAT0: d.mat_color[c] = clamp_u8(rgba[c]); break;
        case fmt::CR_AMB0: d.amb_color[c] = clamp_u8(rgba[c]); break;
        case fmt::CR_TEV0:
        case fmt::CR_TEV1:
        case fmt::CR_TEV2: {
            s16& r = d.tev_reg[1 + reg - fmt::CR_TEV0][c];
            dy.comb_stale |= r != rgba[c];
            r = rgba[c];
            break;
        }
        case fmt::CR_KONST0:
        case fmt::CR_KONST1:
        case fmt::CR_KONST2:
        case fmt::CR_KONST3: {
            s16& r = d.tev_konst[reg - fmt::CR_KONST0][c];
            dy.comb_stale |= r != rgba[c];
            r = rgba[c];
            break;
        }
        default: break;
        }
    }
}

const Sea& World::sea() const { return impl_->sea; }

f32 World::sea_height(f32 x, f32 z, double seconds, Vec3* normal) const {
    const Impl& w = *impl_;
    if (!w.sea.valid()) {
        if (normal)
            *normal = Vec3{0, 1, 0};
        return 0;
    }
    // (about the grid's centre for the camera's last eye: where the waves' fades are measured from)
    const fmt::Sea& sp = w.sea.params();
    const f32 cx = std::round(w.cam.eye.x / sp.grid_snap) * sp.grid_snap;
    const f32 cz = std::round(w.cam.eye.z / sp.grid_snap) * sp.grid_snap;
    return w.sea.height(x, z, seconds, cx, cz, normal);
}

void World::set_lod_scale(f32 scale) {
    impl_->lod_scale = scale;
    impl_->dirty = true;
}

void World::set_features(u32 features) {
    impl_->features = features & WF_ALL;
    impl_->dirty = true;
}
u32 World::features() const { return impl_->features; }

void World::update(f32 frame) {
    Impl& w = *impl_;
    w.frame = frame;
    w.tick++;
    w.dirty = true;  // (instance poses)
    w.sample_trails();
    if (w.dyn.empty())
        return;
    const Package& pkg = package_;
    for (u16 a : w.mat_anims) {
        const fmt::Anim& an = pkg.anims()[a];
        const f32 f = anim_frame(frame, an.frames, an.loop);
        for (u32 t = an.first_track; t < an.first_track + an.n_tracks; t++) {
            const fmt::Track& tr = pkg.tracks()[t];
            if (tr.target >= fmt::TT_TEXMTX_SCALE_S && tr.target <= fmt::TT_TEXMTX_TRANS_T) {
                const f32 v = eval_track(tr, pkg.keys().data, f);
                for (u32 user : w.tmtx_users[tr.index]) {
                    const int di = w.dyn_of[user >> 8];
                    if ((user & 255) >= 3 || di < 0)
                        continue;
                    Impl::Dyn& d = w.dyn[(u32)di];
                    f32& slot = d.srt[user & 255][tr.target - fmt::TT_TEXMTX_SCALE_S];
                    if (slot == v)
                        continue;
                    slot = v;
                    // (the matrix again, now: a few multiplies)
                    const fmt::Material& fm = pkg.materials()[d.d.material];
                    const u32 k = user & 255;
                    if (k < fm.n_tex_mtx) {
                        const fmt::TexMtx& tm = pkg.tex_mtx()[fm.tex_mtx_first + k];
                        tex_srt_matrix(tm.mode, d.srt[k][0], d.srt[k][1], d.srt[k][2], d.srt[k][3], d.srt[k][4],
                                       d.d.tex_mtx[k]);
                    }
                }
            } else if (tr.target >= fmt::TT_MAT_REG_R && tr.target <= fmt::TT_MAT_REG_A) {
                const int di = w.dyn_of[tr.index];
                if (di < 0)
                    continue;
                Impl::Dyn& dy = w.dyn[(u32)di];
                MatDynamic& d = dy.d;
                const f32 v = eval_track(tr, pkg.keys().data, f);
                const u32 c = tr.target - fmt::TT_MAT_REG_R;
                switch (tr.reg) {
                case fmt::CR_MAT0: d.mat_color[c] = clamp_u8(v); break;
                case fmt::CR_AMB0: d.amb_color[c] = clamp_u8(v); break;
                case fmt::CR_TEV0:
                case fmt::CR_TEV1:
                case fmt::CR_TEV2: {
                    // (the registers are folded into the combiners' constants: translated again when
                    // the material is next drawn, if they changed)
                    s16& r = d.tev_reg[1 + tr.reg - fmt::CR_TEV0][c];
                    const s16 nv = clamp_reg(v);
                    dy.comb_stale |= r != nv;
                    r = nv;
                    break;
                }
                case fmt::CR_KONST0:
                case fmt::CR_KONST1:
                case fmt::CR_KONST2:
                case fmt::CR_KONST3: {
                    s16& r = d.tev_konst[tr.reg - fmt::CR_KONST0][c];
                    const s16 nv = clamp_reg(v);
                    dy.comb_stale |= r != nv;
                    r = nv;
                    break;
                }
                default: break;  // (channel 1's colours: not used)
                }
            }
        }
    }
}

void World::set_camera(const Camera& camera, f32 eye_ground) {
    Impl& w = *impl_;
    w.cam = camera;
    w.eye_ground = eye_ground;
    w.fr = frustum(camera);
    w.view_guest = look_at(scaled(camera, w.k));
    w.dirty = true;
}

void World::build_packet(Pass pass, Packet* packet) { impl_->build(pass, packet); }

void World::emit(Pass pass) {
    Packet* p = native_draw_begin();
    impl_->build(pass, p);
    native_draw_submit(p);
}

}  // namespace engine::g3d
