// Wuhu Island's content (content.h): the island drawn by the native g3d (engine/g3d/world.h), the
// household's Mii walking it (player.h over the island's collision, follow_camera.h behind it,
// island_mii.h for the Mii the channel draws), loaded while the scene keeps the screen black.
//
// Loading. The package (DATA/wuhu/island.ng3d, staged from your own Wii Sports Resort and baked:
// docs/wuhu.md) is read, checked and its collision built on a worker (island_load.h), while the game's
// thread makes the Mii's guest objects a step a frame (island_mii.h). When both are done, on the
// game's thread: the World over the package, the backend's prepare (its uploads, a command in the GX
// stream: under the black screen, before the first draw), the player at the package's spawn marker
// (or NOMA_WUHU_START's), its measures from k (the package's Mii units per island unit) and the Mii's
// height and sex, and the camera behind it. A package that can't be had (missing, not baked, damaged)
// leaves the island empty, the reason in the log, and the scene's way back working.
//
// A frame. The player walks by the move stick, turned by the camera's heading (frozen: no input, so a
// walking Mii comes to a stop and a falling one lands); the camera follows it (the camera stick, L and
// R, the D-pad and Y: scene.cpp's controls); the World's animations go on and it is seen from that
// camera; the Mii stands where the player does, times k, its clip from the player's state and its
// stride from the distance walked. The draw: the clear colour (the sky's haze, for the next frame;
// the channel's black again when the island goes), the camera's projection and the viewport into GX
// (they place the native draws as they place the Mii's), the World's opaque pass (sky, island, props)
// then the Mii's, the World's translucent pass then the Mii's.
//
// NOMA_WUHU_START=<marker name>: start at another of the package's markers (a landmark, a warp place,
// or a camera preset view_*, whose target the Mii then stands at, facing away from its eye: the
// follow camera then sees about what the preset does). NOMA_WUHU_LOG_EVERY=N: the frame line every N
// frames (600). NOMA_WUHU_PLACEHOLDER=1: the stand-in content instead (content_placeholder.cpp: the
// scene's shell without the package).
//
// ミドハ (engine/wuhu/midoha.h), while his design is approved: NOMA_WUHU_MIDOHA=A|B|C shows him (that
// candidate face) standing 25 units before the player's start, facing it (NOMA_WUHU_MIDOHA_AT=<marker>:
// at that marker instead), his head pieces drawn after the Mii's opaque pass. NOMA_MIDOHA_SHOTS=1: the
// design renders: the camera goes round him through a list of shots (turnarounds on a plain grey
// ground, his face close, him on the plaza), the player's Mii out of sight, each shot logged
// ("midoha: shot NAME begin" once it has settled, "... end" on its last frame: the frame dumps between
// are of it).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "engine/g3d/native_draw.h"
#include "engine/g3d/world.h"
#include "engine/ui/g3d.h"
#include "engine/wuhu/content.h"
#include "engine/wuhu/follow_camera.h"
#include "engine/wuhu/island_load.h"
#include "engine/wuhu/island_mii.h"
#include "engine/wuhu/midoha.h"
#include "engine/wuhu/player.h"
#include "noma_decomp.h"

namespace engine::wuhu {

IslandContent* make_placeholder_content();

namespace {

namespace fmt = engine::g3d::fmt;
using engine::g3d::Package;
using engine::math::Vec3;

const char* const PACKAGE = "/wuhu/island.ng3d";
constexpr f32 TICK = 1.0f / 60.0f;
constexpr f32 DEG = 3.14159265f / 180.0f;
// how the load's progress is shared: the file's reading, the rest of the worker's work, the Mii
constexpr f32 SHARE_READ = 0.7f, SHARE_BUILD = 0.1f, SHARE_MII = 0.2f;

f32 ms_between(u64 a, u64 b) { return (f32)(b - a) / 1000.0f; }

// The colour the frame's copy clears the EFB to for the next frame (GXSetCopyClear, RGBA; alpha 0
// and the depth cleared to the far end, as the channel does). The channel's own is black (every
// GXSetCopyClear of its frame and video paths); the island's is its first fog in use (docs/wuhu.md:
// the package has no clear colour of its own, the scene sets it), fog 1's #83D5FF, which the gap
// under the sky dome's lower edge then meets the fogged sea in, as tools/g3d/preview.py draws it.
const u32 CHANNEL_CLEAR = 0;
void set_clear(u32 rgba) {
    GFRAME(0x10);
    GS(u32, 0x8) = rgba;
    g::GXSetCopyClear((u32)&GS(u32, 0x8), 0xFFFFFF);
}
// The design renders' shots (NOMA_MIDOHA_SHOTS): the camera about the NPC, `yaw` degrees round from
// his front (90: from his left), `pitch` degrees above, `distance` and `look` (the point it looks at,
// above his feet) in his heights, the field of view; the island drawn or a grey ground. Each is
// held SHOT_HOLD frames, logged from SHOT_SETTLE on.
struct Shot {
    const char* name;
    bool island;
    f32 yaw, pitch, distance, look, fovy;
};
const Shot SHOTS[] = {
    {"turn_front", false, 0, 4, 2.4f, 0.56f, 30},    {"turn_34", false, 45, 4, 2.4f, 0.56f, 30},
    {"turn_side", false, 90, 4, 2.4f, 0.56f, 30},    {"turn_back34", false, 135, 4, 2.4f, 0.56f, 30},
    {"turn_back", false, 180, 4, 2.4f, 0.56f, 30},   {"turn_side_r", false, 270, 4, 2.4f, 0.56f, 30},
    {"face_front", false, 0, 2, 0.95f, 0.86f, 30},    {"face_34", false, 35, 6, 0.95f, 0.86f, 30},
    {"face_side", false, 90, 4, 0.95f, 0.86f, 30},    {"face_back34", false, 145, 10, 0.95f, 0.86f, 30},
    {"face_top", false, 20, 55, 1.05f, 0.82f, 30},    {"plaza", true, 25, 9, 3.6f, 0.55f, 45},
    {"plaza_wide", true, -30, 14, 7.5f, 0.5f, 45},
};
const u32 N_SHOTS = sizeof(SHOTS) / sizeof(SHOTS[0]);
// (from 6 s after the island shows: its caption has gone)
const u32 SHOT_START = 360, SHOT_HOLD = 50, SHOT_SETTLE = 28;
const u32 SHOT_GREY = 0xA8ACB0FF;

// (0 if no fog is in use: the channel's black stays)
u32 clear_of(const Package& p) {
    for (const fmt::Fog& f : p.fogs())
        if (f.type != fmt::FOG_NONE)
            return (u32)f.rgba[0] << 24 | (u32)f.rgba[1] << 16 | (u32)f.rgba[2] << 8;
    return CHANNEL_CLEAR;
}

class Island : public IslandContent {
public:
    ~Island() override;
    void start_loading() override;
    f32 progress() override;
    bool ready() const override { return ready_; }
    void update(const Controls& controls, bool frozen) override;
    void draw_opaque() override;
    void draw_translucent() override;
    const Package* package() const override { return world_ ? package_.get() : nullptr; }
    void warp(u32 marker) override;

private:
    void finish_loading();
    void fail(const std::string& why);
    bool place_at(const fmt::Marker& m);
    void place_npc();
    bool shot_camera(engine::g3d::Camera* cam, bool* island);
    void log_frame();

    std::unique_ptr<PackageLoad> load_;
    std::unique_ptr<IslandMii> mii_;
    std::unique_ptr<Package> package_;
    std::unique_ptr<engine::collision::World> collision_;
    std::unique_ptr<engine::g3d::World> world_;
    std::unique_ptr<Player> player_;
    std::unique_ptr<FollowCamera> camera_;
    // ミドハ: his candidate, his place (a Player of his own: the ground under him), his head
    const midoha::Candidate* npc_ = nullptr;
    std::unique_ptr<Player> npc_body_;
    std::unique_ptr<MidohaLook> look_;
    bool shots_ = false, island_shown_ = true;
    bool ready_ = false, prepared_ = false;
    u32 clear_ = CHANNEL_CLEAR;  // the EFB's clear colour while the island shows (set_clear)
    f32 k_ = 1;
    u64 start_us_ = 0;
    u32 frame_ = 0;  // the World's clock (frames since the island came up)
    u32 log_every_ = 600, log_frames_ = 0;
    u64 log_us_ = 0;
};

Island::~Island() {
    // the Mii's guest objects (the content's only ones); then the package's GPU resources, before
    // the package (native_draw.h), and what reads the package and the collision before them
    mii_.reset();
    look_.reset();
    if (clear_ != CHANNEL_CLEAR)
        set_clear(CHANNEL_CLEAR);
    if (prepared_)
        engine::g3d::native_draw_release(*package_);
    world_.reset();
    camera_.reset();
    player_.reset();
    collision_.reset();
    package_.reset();
    load_.reset();  // (waits for the worker, if it never finished)
}

void Island::start_loading() {
    start_us_ = guest::host_time_us();
    if (const char* e = std::getenv("NOMA_WUHU_LOG_EVERY"))
        log_every_ = std::max(1, std::atoi(e));
    load_ = std::make_unique<PackageLoad>(guest::data_dir() + PACKAGE);
    load_->start();
    npc_ = midoha_candidate();
    mii_ = std::make_unique<IslandMii>(npc_ ? npc_->record : nullptr);
    if (npc_) {
        const char* e = std::getenv("NOMA_MIDOHA_SHOTS");
        shots_ = e && *e && std::strcmp(e, "0");
        guest::log("midoha: candidate %c (%s)%s", npc_->letter, npc_->summary, shots_ ? ", design shots" : "");
    }
}

// One step a frame of the Mii's (each a few guest calls; the actor manager's reads its files, which
// the guest waits for while its other threads run); the package's own pace on the worker.
f32 Island::progress() {
    if (ready_)
        return 1.0f;
    const bool mii_more = mii_->make_step();
    if (!mii_more && load_->done())
        finish_loading();
    if (ready_)
        return 1.0f;
    const u32 total = load_->bytes_total();
    const f32 read = total ? (f32)load_->bytes_read() / (f32)total : 0.0f;
    return SHARE_READ * read + (load_->done() ? SHARE_BUILD : 0.0f) + SHARE_MII * mii_->progress();
}

void Island::fail(const std::string& why) {
    guest::log("wuhu: the island can't be shown: %s", why.c_str());
    ready_ = true;
}

void Island::finish_loading() {
    PackageLoad& l = *load_;
    if (!l.package) {
        fail(l.error);
        return;
    }
    if (!mii_->made()) {
        fail("the Mii's guest objects couldn't be made");
        return;
    }
    package_ = std::move(l.package);
    collision_ = std::move(l.collision);
    k_ = package_->env().mii_units_per_unit;
    clear_ = clear_of(*package_);

    const u64 t0 = guest::host_time_us();
    world_ = std::make_unique<engine::g3d::World>(*package_);
    const u64 t1 = guest::host_time_us();
    // (the uploads themselves happen on the renderer's thread when it reaches the command)
    engine::g3d::native_draw_prepare(*package_);
    prepared_ = true;
    const u64 t2 = guest::host_time_us();

    const PlayerParams pp = PlayerParams::make(k_, mii_->height_scale(), mii_->gait());
    player_ = std::make_unique<Player>(*collision_, pp);
    FollowCameraParams cp = FollowCameraParams::make(pp.height);
    cp.aspect = mii_->aspect();
    camera_ = std::make_unique<FollowCamera>(*collision_, cp);

    // where to start: NOMA_WUHU_START's marker (a warp place before a landmark of the same name: the
    // landmark marks the thing, the warp place the ground to stand on before it), else the spawn
    // marker, else the first spawn point
    const fmt::Marker* start = nullptr;
    if (const char* e = std::getenv("NOMA_WUHU_START")) {
        for (const fmt::Marker& m : package_->markers()) {
            if (std::strcmp(package_->str(m.name), e) != 0)
                continue;
            if (!start || (m.kind == fmt::MK_WARP && start->kind != fmt::MK_WARP))
                start = &m;
        }
        if (!start)
            guest::log("wuhu: NOMA_WUHU_START: no marker %s: the spawn point instead", e);
    }
    if (!start)
        start = package_->find_marker("spawn");
    for (const fmt::Marker& m : package_->markers())
        if (!start && m.kind == fmt::MK_SPAWN)
            start = &m;
    if (!start || !place_at(*start)) {
        player_->place(Vec3{0, 1e4f, 0}, 0);
        camera_->reset(player_->position(), player_->yaw());
        guest::log("wuhu: no spawn point with ground under it: the Mii stands at the island's middle");
    }

    if (mii_->npc_present()) {
        place_npc();
        // his head pieces (their uploads a command in the GX stream after the island's)
        look_ = std::make_unique<MidohaLook>();
        if (!look_->load())
            look_.reset();
    }

    const u64 t3 = guest::host_time_us();
    guest::log("wuhu: load: %u bytes in %.0f ms (read %.0f, check %.0f, collision %.0f, world %.1f, prepare %.1f "
               "ms); k %.3f, the Mii %.2f units tall (walks %.3f, runs %.3f a frame, step %.2f, slope limit %.0f "
               "degrees, drops up to %.1f)",
               (unsigned)package_->size(), ms_between(start_us_, t3), l.read_ms, l.check_ms, l.collision_ms,
               ms_between(t0, t1), ms_between(t1, t2), k_, pp.height, pp.walk_speed, pp.run_speed, pp.step_up,
               std::acos(pp.walkable_ny) / DEG, pp.max_drop);
    u32 heap_free = 0, linear_free = 0;
    const bool counted = guest::platform_native_memory(&heap_free, &linear_free);
    std::string heap;
    const u32 guest_free = engine::ui::g3d::heap_free(&heap);
    char native[96] = "";
    if (counted)
        std::snprintf(native, sizeof(native), ", native heap %u KB free, linear %u KB free",
                      (unsigned)(heap_free >> 10), (unsigned)(linear_free >> 10));
    guest::log("wuhu: memory: package %u bytes, collision %u bytes (%u triangles, %u trunks)%s; heap %s: %u bytes "
               "free",
               (unsigned)package_->size(), (unsigned)collision_->memory_bytes(), (unsigned)collision_->triangles(),
               (unsigned)collision_->cylinders(), native, heap.c_str(), (unsigned)guest_free);
    load_.reset();
    log_us_ = guest::host_time_us();
    ready_ = true;
}

// The player at a marker: a spawn point, a warp place or a landmark facing its direction; a camera
// preset at the point it looks at, facing away from its eye. The ground under it (or a step above
// it), the camera behind it.
bool Island::place_at(const fmt::Marker& m) {
    Vec3 at{m.position[0], m.position[1], m.position[2]};
    f32 dx = m.dir[0], dz = m.dir[2];
    if (m.kind == fmt::MK_CAMERA) {
        dx = m.dir[0] - at.x, dz = m.dir[2] - at.z;
        at = Vec3{m.dir[0], m.dir[1], m.dir[2]};
    }
    const f32 yaw = dx * dx + dz * dz > 1e-12f ? std::atan2(dx, dz) / DEG : 0.0f;
    if (!player_->place(at, yaw)) {
        guest::log("wuhu: no ground under the marker %s (%.0f, %.0f, %.0f)", package_->str(m.name), at.x, at.y,
                   at.z);
        return false;
    }
    camera_->reset(player_->position(), player_->yaw());
    const Vec3 p = player_->position();
    guest::log("wuhu: at %s (%.1f, %.1f, %.1f) facing %.0f, ground %04X", package_->str(m.name), p.x, p.y, p.z,
               player_->yaw(), (unsigned)player_->ground_attribute());
    return true;
}

// ミドハ where NOMA_WUHU_MIDOHA_AT says, or 25 units before the player's start, facing it; his feet on
// the ground there (a Player's placing, with his own height)
void Island::place_npc() {
    npc_body_ = std::make_unique<Player>(*collision_, PlayerParams::make(k_, mii_->npc_height_scale()));
    const f32 a = player_->yaw() * DEG;
    Vec3 at = player_->position() + Vec3{std::sin(a), 0, std::cos(a)} * 25.0f;
    f32 yaw = player_->yaw() + 180.0f;
    if (const char* e = std::getenv("NOMA_WUHU_MIDOHA_AT")) {
        if (const fmt::Marker* m = package_->find_marker(e)) {
            at = Vec3{m->position[0], m->position[1], m->position[2]};
            yaw = std::atan2(m->dir[0], m->dir[2]) / DEG;
        } else {
            guest::log("midoha: NOMA_WUHU_MIDOHA_AT: no marker %s", e);
        }
    }
    if (!npc_body_->place(at + Vec3{0, 5, 0}, yaw))
        npc_body_->place(at, yaw);
    const Vec3 p = npc_body_->position();
    guest::log("midoha: stands at (%.1f, %.1f, %.1f) facing %.0f, %.2f units tall", p.x, p.y, p.z, npc_body_->yaw(),
               npc_body_->params().height);
}

// The design renders' camera this frame (false: the shots are over, or not started)
bool Island::shot_camera(engine::g3d::Camera* cam, bool* island) {
    if (!shots_ || !npc_body_ || frame_ < SHOT_START)
        return false;
    const u32 f = frame_ - SHOT_START, i = f / SHOT_HOLD, in = f % SHOT_HOLD;
    if (i >= N_SHOTS) {
        if (in == 0 && i == N_SHOTS)
            guest::log("midoha: all shots done");
        return false;
    }
    const Shot& s = SHOTS[i];
    if (in == SHOT_SETTLE)
        guest::log("midoha: shot %s begin", s.name);
    if (in == SHOT_HOLD - 1)
        guest::log("midoha: shot %s end", s.name);
    const f32 h = npc_body_->params().height;
    const Vec3 feet = npc_body_->position();
    const f32 yaw = (npc_body_->yaw() + s.yaw) * DEG, pitch = s.pitch * DEG, d = s.distance * h;
    cam->target = feet + Vec3{0, s.look * h, 0};
    const Vec3 back = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    cam->eye = cam->target + back * d;
    cam->up = Vec3{0, 1, 0};
    cam->fovy_deg = s.fovy;
    cam->aspect = mii_->aspect();
    cam->near_ = std::max(0.2f, d * 0.2f);
    cam->far_ = 80000.0f;
    *island = s.island;
    return true;
}

void Island::warp(u32 marker) {
    if (!world_ || marker >= package_->markers().count)
        return;
    place_at(package_->markers()[marker]);
}

void Island::update(const Controls& controls, bool frozen) {
    if (!world_)
        return;
    frame_++;
    PlayerInput move;
    FollowCameraInput look;
    if (!frozen) {
        camera_->stick_to_world(controls.move_x, controls.move_y, &move.move_x, &move.move_z);
        // the camera stick and the D-pad: up looks up (the camera goes down round the Mii)
        look.orbit_x = controls.camera_x;
        look.orbit_y = -controls.camera_y;
        look.zoom = controls.zoom;
        look.behind = controls.behind;
    }
    const f32 walked = player_->distance();
    player_->update(move, TICK);
    camera_->update(look, player_->position(), player_->yaw(), player_->speed() / player_->params().run_speed, TICK);
    engine::g3d::Camera cam = camera_->camera();
    island_shown_ = true;
    const bool shot = shot_camera(&cam, &island_shown_);
    mii_->set_hide_player(shot);
    world_->update((f32)frame_);
    world_->set_camera(cam);
    if (npc_body_)
        mii_->place_npc(npc_body_->position() * k_, npc_body_->yaw());
    mii_->update(engine::g3d::scaled(cam, k_), player_->position() * k_, player_->yaw(), player_->state(),
                 (player_->distance() - walked) * k_);
    engine::math::Mtx34 head;
    if (look_ && mii_->npc_head(&head))
        look_->update(head, engine::g3d::scaled(cam, k_), (f32)frame_);
    if (++log_frames_ >= log_every_)
        log_frame();
}

void Island::log_frame() {
    const u64 now = guest::host_time_us();
    const f32 fps = now > log_us_ ? (f32)log_frames_ * 1e6f / (f32)(now - log_us_) : 0.0f;
    log_us_ = now, log_frames_ = 0;
    const engine::g3d::WorldStats& s = world_->stats();
    const Vec3 p = player_->position();
    static const char* const STATES[] = {"wait", "walk", "run", "fall"};
    guest::log("wuhu: fps %.1f, cells %u/%u, instances %u, items %u+%u, tris %u, pos (%.1f, %.1f, %.1f) ground 0x%04X; "
               "yaw %.0f, %s, walked %.0f, camera yaw %.0f pitch %.0f at %.1f",
               fps, (unsigned)s.cells_visible, (unsigned)s.cells_total, (unsigned)s.instances_visible,
               (unsigned)s.items_opaque, (unsigned)s.items_translucent, (unsigned)s.triangles, p.x, p.y, p.z,
               (unsigned)player_->ground_attribute(), player_->yaw(), STATES[(int)player_->state()],
               player_->distance(), camera_->yaw(), camera_->pitch(), camera_->distance());
}

void Island::draw_opaque() {
    if (!world_)
        return;
    // (every frame: the channel's texture copies may set a clear colour of their own)
    if (!island_shown_)
        set_clear(SHOT_GREY);
    else if (clear_ != CHANNEL_CLEAR)
        set_clear(clear_);
    mii_->view();  // (the camera's viewport, scissor and projection: what the native draws take)
    if (island_shown_)
        world_->emit(engine::g3d::Pass::OPAQUE);
    mii_->draw_opaque();
    if (look_) {
        mii_->view();
        look_->emit(engine::g3d::Pass::OPAQUE);
    }
}

void Island::draw_translucent() {
    if (!world_)
        return;
    // (the projection again: the Mii's opaque pass may have loaded another)
    mii_->view();
    if (island_shown_)
        world_->emit(engine::g3d::Pass::TRANSLUCENT);
    if (look_)
        look_->emit(engine::g3d::Pass::TRANSLUCENT);
    mii_->draw_translucent();
}

}  // namespace

IslandContent* make_content() {
    if (std::getenv("NOMA_WUHU_PLACEHOLDER"))
        return make_placeholder_content();
    return new Island();
}

}  // namespace engine::wuhu
