// A world drawn by the native g3d (world.h): culling, detail levels, animation, the draw order, the
// packets.
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

}  // namespace

struct World::Impl {
    const Package& pkg;
    f32 k = 1;
    std::vector<Material> mats;

    // ---- material animations: the environment's (world, sea, sky), each material they drive with
    // a dynamic block (its current registers, colours, texture SRTs and what they make)
    std::vector<u16> mat_anims;
    struct Dyn {
        MatDynamic d;
        f32 srt[3][5];   // the first three texture matrices' scale s, t, rotation, translation s, t
        bool tev_animated = false;
    };
    std::vector<Dyn> dyn;
    std::vector<int> dyn_of;                    // per material: index into dyn, -1
    std::vector<std::vector<u32>> tmtx_users;   // per TMTX record: (material << 8 | its index there)

    // ---- instances
    struct Inst {
        Mtx34 mtx;          // model -> world
        Vec3 centre;        // bounding sphere of its model, world
        f32 radius, scale;  // (scale: the matrix's largest)
    };
    std::vector<Inst> insts;
    // node tracks per animation, by (model, node): track indices
    struct NodeTracks {
        u16 model, node;
        std::vector<u32> tracks;
    };
    std::vector<std::vector<NodeTracks>> anim_nodes;  // per animation (filled for instance animations)

    // ---- what the scene set
    std::vector<u8> cell_hidden, model_hidden;
    f32 cell_distance = 0, inst_distance = 0, lod_scale = 1;
    f32 frame = 0;
    Camera cam;
    Frustum fr;
    Mtx34 view_guest;
    bool dirty = true;  // camera, time or settings changed since the last cull

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
    std::vector<Mtx34> finals;
    WorldStats* stats = nullptr;

    // ---- packet building scratch
    struct Cand {
        s32 priority;
        f32 distance;
        u32 order;
        u32 final;  // its matrix: a finals index, ~0u: the identity (the static world)
        DrawItem item;
        u32 tris;
    };
    std::vector<Cand> cands;
    std::vector<int> matrix_of;  // per finals entry: index in the packet's matrices, -1
    std::vector<int> state_of;   // per material: index in the packet's mat_states, -1
    std::vector<u8> srt_changed, tev_changed;  // per dynamic block (update())

    explicit Impl(const Package& p) : pkg(p) {}

    void init();
    void pose(u16 model, const Mtx34& mtx, u16 anim, f32 anim_offset, bool sky);
    void cull();
    void build(Pass pass, Packet* p);
    void add_model_items(const VisInst& v, Pass pass, u32 flags);
};

void World::Impl::init() {
    const fmt::Env& env = pkg.env();
    k = env.mii_units_per_unit;
    mats.resize(pkg.materials().count);
    for (u32 i = 0; i < mats.size(); i++)
        build_material(pkg, i, &mats[i]);
    cell_hidden.assign(pkg.cells().count, 0);
    model_hidden.assign(pkg.models().count, 0);
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
    auto dyn_for = [&](u32 m) -> Dyn& {
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
            dyn_of[m] = (int)dyn.size();
            dyn.push_back(d);
        }
        return dyn[(u32)dyn_of[m]];
    };
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
                Dyn& d = dyn_for(tr.index);
                mats[tr.index].animated_color = true;
                d.tev_animated |= tr.reg >= fmt::CR_TEV0;
            }
        }
    }
    // instances: their bounding spheres; their animations' node tracks
    anim_nodes.assign(pkg.anims().count, {});
    insts.resize(pkg.instances().count);
    for (u32 i = 0; i < insts.size(); i++) {
        const fmt::Instance& in = pkg.instances()[i];
        const fmt::Model& m = pkg.models()[in.model];
        Inst& n = insts[i];
        n.mtx = to_mtx(in.mtx);
        n.scale = max_scale(n.mtx);
        n.centre = transform(n.mtx, box_centre(m.bbox));
        n.radius = m.radius * n.scale;
        if (in.anim != NONE && (in.flags & fmt::INST_ANIMATED) && anim_nodes[in.anim].empty()) {
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
        }
    }
}

// The world matrices of a model's nodes (into finals): at rest (the bind matrices) or posed by an
// animation's node tracks at the current frame, under `mtx`; billboard nodes turned to the camera.
void World::Impl::pose(u16 model, const Mtx34& mtx, u16 anim, f32 anim_offset, bool sky) {
    const fmt::Model& m = pkg.models()[model];
    const u32 base = (u32)finals.size();
    // (a model without nodes draws its batches with the matrix it is placed with)
    finals.resize(base + std::max<u32>(m.n_nodes, 1));
    finals[base] = mtx;
    const std::vector<NodeTracks>* tracks = anim != NONE ? &anim_nodes[anim] : nullptr;
    f32 f = 0;
    if (tracks && !tracks->empty()) {
        const fmt::Anim& an = pkg.anims()[anim];
        f = anim_frame(frame + anim_offset, an.frames, an.loop);
    }
    for (u32 k = 0; k < m.n_nodes; k++) {
        const fmt::Node& nd = pkg.nodes()[m.first_node + k];
        Mtx34 local_world;  // model space
        const NodeTracks* nt = nullptr;
        if (tracks)
            for (const NodeTracks& x : *tracks)
                if (x.model == model && x.node == k)
                    nt = &x;
        if (nt) {
            f32 s[3], r[3], t[3];
            memcpy(s, nd.scale, sizeof(s)), memcpy(r, nd.rotate, sizeof(r)), memcpy(t, nd.translate, sizeof(t));
            for (u32 ti : nt->tracks) {
                const fmt::Track& tr = pkg.tracks()[ti];
                const f32 v = eval_track(tr, pkg.keys().data, f);
                const u32 c = tr.target;
                (c < 3 ? s : c < 6 ? r : t)[c % 3] = v;
            }
            const Mtx34 local = srt_matrix(s, r, t);
            local_world = nd.parent == NONE ? local : concat(finals[base + nd.parent], local);
            // (finals holds model space until the instance matrix goes on below)
        } else if (tracks && nd.parent != NONE) {
            // a node at rest under an animated parent: its rest pose relative to the parent
            const f32 s[3] = {nd.scale[0], nd.scale[1], nd.scale[2]};
            local_world = concat(finals[base + nd.parent], srt_matrix(s, nd.rotate, nd.translate));
        } else {
            local_world = to_mtx(nd.bind);
        }
        finals[base + k] = local_world;
    }
    for (u32 k = 0; k < m.n_nodes; k++) {
        Mtx34& w = finals[base + k];
        w = concat(mtx, w);
        const fmt::Node& nd = pkg.nodes()[m.first_node + k];
        if (!(nd.flags & fmt::NODE_BILLBOARD) || !nd.billboard || sky)
            continue;
        // turned to the camera, keeping its place and scale: Y billboards (nw4r modes 5, 6) about
        // the world's y axis, the others wholly; the perspective modes (2, 4, 6) toward the eye,
        // the rest along the view direction
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

// this frame's visible cells and instances, with their detail levels and poses
void World::Impl::cull() {
    vis_cells.clear();
    vis_insts.clear();
    finals.clear();
    *stats = WorldStats();
    stats->cells_total = pkg.cells().count;
    stats->instances_total = pkg.instances().count;
    const Vec3 eye = cam.eye;
    for (u32 i = 0; i < pkg.cells().count; i++) {
        const fmt::Cell& c = pkg.cells()[i];
        if (cell_hidden[i] || !box_visible(fr, c.bbox))
            continue;
        const f32 d = box_distance(c.bbox, eye);
        if (cell_distance > 0 && d > cell_distance)
            continue;
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
        VisInst v = {i, model, d, (u32)finals.size()};
        pose(model, n.mtx, (in.flags & fmt::INST_ANIMATED) ? in.anim : NONE, in.anim_offset, false);
        vis_insts.push_back(v);
    }
    stats->instances_visible = (u32)vis_insts.size();
    const fmt::Env& env = pkg.env();
    if (env.sea_model != NONE) {
        VisInst v = {~0u, env.sea_model, 0, (u32)finals.size()};
        pose(env.sea_model, identity34(), NONE, 0, false);
        vis_insts.push_back(v);
    }
    if (env.sky_model != NONE) {
        VisInst v = {~1u, env.sky_model, 0, (u32)finals.size()};
        pose(env.sky_model, translation(eye), NONE, 0, true);
        vis_insts.push_back(v);
    }
}

void World::Impl::add_model_items(const VisInst& v, Pass pass, u32 flags) {
    const fmt::Model& m = pkg.models()[v.model];
    const bool sky = v.inst == ~1u, sea = v.inst == ~0u;
    for (u32 b = m.first_batch; b < m.first_batch + m.n_batches; b++) {
        const fmt::Batch& bt = pkg.batches()[b];
        const Material& mat = mats[bt.material];
        if (!sky && mat.translucent != (pass == Pass::TRANSLUCENT))
            continue;
        if (mat.pixel.cull_gx == 3)
            continue;  // (GX culls all of it)
        const u32 fin = v.first_final + (bt.node < m.n_nodes ? bt.node : 0);
        f32 box[6];
        transform_box(finals[fin], bt.bbox, box);
        // (the sea's batches are culled one by one: it spans the world)
        if (sea && !box_visible(fr, box))
            continue;
        Cand c;
        c.priority = sky ? -1000 : mat.priority;
        c.distance = sky ? 0 : pass == Pass::TRANSLUCENT ? length(box_centre(box) - cam.eye)
                                  : sea ? box_distance(box, cam.eye) : v.distance;
        c.order = (u32)cands.size();
        c.final = fin;
        c.item = {(u16)b, 0, 0xFFFF, (u16)flags};
        c.tris = bt.n_indices / 3;
        cands.push_back(c);
    }
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

    // the candidates: cells' batches at their level, the models' (instances, sea; the sky first)
    cands.clear();
    for (const VisInst& v : vis_insts)
        if (v.inst == ~1u && pass == Pass::OPAQUE)
            add_model_items(v, pass, DRAW_SKY);
    const u32 n_sky = (u32)cands.size();
    for (const VisCell& vc : vis_cells) {
        const fmt::Cell& c = pkg.cells()[vc.cell];
        for (u32 i = 0; i < c.count[vc.lod]; i++) {
            const u16 b = pkg.cell_batches()[c.first[vc.lod] + i];
            const fmt::Batch& bt = pkg.batches()[b];
            const Material& mat = mats[bt.material];
            if (mat.translucent != (pass == Pass::TRANSLUCENT) || mat.pixel.cull_gx == 3)
                continue;
            Cand cd;
            cd.priority = mat.priority;
            cd.distance = pass == Pass::TRANSLUCENT ? length(box_centre(bt.bbox) - cam.eye) : vc.distance;
            cd.order = (u32)cands.size();
            cd.final = ~0u;
            cd.item = {b, 0, 0xFFFF, 0};
            cd.tris = bt.n_indices / 3;
            cands.push_back(cd);
        }
    }
    for (const VisInst& v : vis_insts)
        if (v.inst != ~1u)
            add_model_items(v, pass, 0);
    // the order: the sky as it comes, then by priority, then front to back (opaque) or back to
    // front (translucent), then as they came
    const bool back_to_front = pass == Pass::TRANSLUCENT;
    std::sort(cands.begin() + n_sky, cands.end(), [&](const Cand& a, const Cand& b) {
        if (a.priority != b.priority)
            return a.priority < b.priority;
        if (a.distance != b.distance)
            return back_to_front ? a.distance > b.distance : a.distance < b.distance;
        return a.order < b.order;
    });
    // the items, with their matrices and material states put in the packet as they are first used
    matrix_of.assign(finals.size(), -1);
    state_of.assign(mats.size(), -1);
    static const f32 k_identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    p->matrices.assign(k_identity, k_identity + 12);
    p->mat_states.clear();
    p->items.clear();
    u32 tris = 0;
    for (const Cand& c : cands) {
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
        const u16 mat = pkg.batches()[it.batch].material;
        if (dyn_of[mat] >= 0) {
            int& si = state_of[mat];
            if (si < 0) {
                si = (int)p->mat_states.size();
                p->mat_states.push_back(dyn[(u32)dyn_of[mat]].d);
            }
            it.mat_state = (u16)si;
        }
        p->items.push_back(it);
        tris += c.tris;
    }
    (pass == Pass::OPAQUE ? stats->items_opaque : stats->items_translucent) = (u32)p->items.size();
    stats->triangles += tris;
}

// ---------------------------------------------------------------- World
World::World(const Package& package) : impl_(new Impl(package)), package_(package) {
    impl_->stats = &stats_;
    impl_->init();
    set_camera(Camera());
}

World::~World() { delete impl_; }

f32 World::world_scale() const { return impl_->k; }
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

void World::set_lod_scale(f32 scale) {
    impl_->lod_scale = scale;
    impl_->dirty = true;
}

void World::update(f32 frame) {
    Impl& w = *impl_;
    w.frame = frame;
    w.dirty = true;  // (instance poses)
    if (w.dyn.empty())
        return;
    const Package& pkg = package_;
    std::vector<u8>& srt_changed = w.srt_changed;
    std::vector<u8>& tev_changed = w.tev_changed;
    srt_changed.assign(w.dyn.size(), 0), tev_changed.assign(w.dyn.size(), 0);
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
                    w.dyn[(u32)di].srt[user & 255][tr.target - fmt::TT_TEXMTX_SCALE_S] = v;
                    srt_changed[(u32)di] = 1;
                }
            } else if (tr.target >= fmt::TT_MAT_REG_R && tr.target <= fmt::TT_MAT_REG_A) {
                const int di = w.dyn_of[tr.index];
                if (di < 0)
                    continue;
                MatDynamic& d = w.dyn[(u32)di].d;
                const f32 v = eval_track(tr, pkg.keys().data, f);
                const u32 c = tr.target - fmt::TT_MAT_REG_R;
                switch (tr.reg) {
                case fmt::CR_MAT0: d.mat_color[c] = clamp_u8(v); break;
                case fmt::CR_AMB0: d.amb_color[c] = clamp_u8(v); break;
                case fmt::CR_TEV0:
                case fmt::CR_TEV1:
                case fmt::CR_TEV2: d.tev_reg[1 + tr.reg - fmt::CR_TEV0][c] = clamp_reg(v), tev_changed[(u32)di] = 1; break;
                case fmt::CR_KONST0:
                case fmt::CR_KONST1:
                case fmt::CR_KONST2:
                case fmt::CR_KONST3: d.tev_konst[tr.reg - fmt::CR_KONST0][c] = clamp_reg(v), tev_changed[(u32)di] = 1; break;
                default: break;  // (channel 1's colours: not used)
                }
            }
        }
    }
    for (u32 i = 0; i < w.dyn.size(); i++) {
        Impl::Dyn& d = w.dyn[i];
        const Material& m = w.mats[d.d.material];
        const fmt::Material& fm = pkg.materials()[d.d.material];
        if (srt_changed[i])
            for (u32 k = 0; k < 3 && k < fm.n_tex_mtx; k++) {
                const fmt::TexMtx& tm = pkg.tex_mtx()[fm.tex_mtx_first + k];
                tex_srt_matrix(tm.mode, d.srt[k][0], d.srt[k][1], d.srt[k][2], d.srt[k][3], d.srt[k][4], d.d.tex_mtx[k]);
            }
        if (tev_changed[i]) {
            // the registers are folded into the combiners' constants: translate again
            engine::gx::tev_to_combiners(tev_regs(fm, d.d.tev_reg, d.d.tev_konst), d.d.comb);
            if (m.lum_units)
                engine::gx::remap_luminance(d.d.comb.stage, m.lum_units);
        }
    }
}

void World::set_camera(const Camera& camera) {
    Impl& w = *impl_;
    w.cam = camera;
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
