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
// stride from the distance walked. They all go by the time the frame took (FrameClock: the display's
// frames since the last update, at most three), not a sixtieth of a second a game frame: the Mii's
// pace a second, the camera's smoothing and the World's animations are the same at 20, 30 or 60
// frames a second. The World's part of the frame (its animations, the camera, both
// passes' culling and draw lists: about a millisecond of the New 3DS's CPU) is a job (platform_job_start:
// the New 3DS's helper core) from the end of the update on, while the game's thread goes on to the
// Mii and the rest of its frame; the draws take its packets (waiting for it if it isn't done). Until
// then the game's thread doesn't touch the World. Where there's no helper the World does it all on
// the game's thread, as it did: its update and camera in the update, each pass's list at its draw.
// The draw: the clear colour (the sky's haze, for the next frame;
// the channel's black again when the island goes), the camera's projection and the viewport into GX
// (they place the native draws as they place the Mii's), the World's opaque pass (sky, island, props)
// then the Mii's, the World's translucent pass then the Mii's.
//
// The package: DATA/wuhu/island_v2.ng3d if there is one, else DATA/wuhu/island.ng3d (for as long as
// the restaged version 2 island sits beside the old one; a version 1 file is refused, the reason in
// the log). The World gets the height of the ground under the camera's eye (the collision's highest
// surface below it facing up, not a fence: the stager's floors), which says whether the eye's cell's
// visibility row applies (world.h).
//
// NOMA_WUHU_START=<marker name>: start at another of the package's markers (a landmark, a warp place,
// or a camera preset view_*, whose target the Mii then stands at, facing away from its eye: the
// follow camera then sees about what the preset does). NOMA_WUHU_LOG_EVERY=N: the frame line every N
// frames (600). NOMA_WUHU_SPEED_OFF=pvs,detail,anim,sort: the World's speed-ups named turned off
// (world.h WorldFeature: the visibility rows, detail culling, the animation rate by size, the opaque
// items grouped by material), for comparisons on the console. NOMA_WUHU_BY_FRAMES=1: the Mii, the
// camera and the World move a sixtieth of a second a game frame, whatever the time (repeatable host
// runs). NOMA_WUHU_PLACEHOLDER=1: the stand-in content instead (content_placeholder.cpp: the scene's
// shell without the package).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "engine/g3d/native_draw.h"
#include "engine/g3d/world.h"
#include "engine/ui/g3d.h"
#include "engine/wuhu/content.h"
#include "engine/wuhu/follow_camera.h"
#include "engine/wuhu/island_load.h"
#include "engine/wuhu/island_mii.h"
#include "engine/wuhu/player.h"
#include "noma_decomp.h"

namespace engine::wuhu {

IslandContent* make_placeholder_content();

namespace {

namespace fmt = engine::g3d::fmt;
using engine::g3d::Package;
using engine::math::Vec3;

// the restaged island (NG3D version 2) while it sits beside the old one, else the island
const char* const PACKAGE_V2 = "/wuhu/island_v2.ng3d";
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
// The Mii's size and pace on the island (the user, after the first runs): at WSR's own scale (the
// package's k: a Mii as tall as WSR's own, research k.md) it was dwarfed by the town, the trees and the
// cars, so it is drawn MII_SCALE times as big against the island (its body, steps, reach and camera
// all follow k); and it crosses the island at PACE times the pace it has at WSR's scale (its own walk
// and run, from the room, are per its size: MII_SCALE of that comes with the size, PACE / MII_SCALE
// is applied). The pace is a second's: the user asked for twice the pace they saw at about 25 frames
// a second, when the Mii moved a sixtieth of a second's worth a frame (41 % of its pace a second),
// and the Mii now moves by the time a frame takes (FrameClock): 2 x 25 / 60 of its pace a second
// whatever the frame rate (the per-frame pace of 1276ec5, 2, was that at 25 fps and 2.4 times it at 60).
constexpr f32 MII_SCALE = 1.7f;
constexpr f32 PACE = 2.0f * 25.0f / 60.0f;

// The time an update moves the Mii, the camera and the World's animations by: the display's frames
// (retraces, 1/59.94 s) since the last update, rounded, so that at 30 frames a second each update is
// two of them (the guest's frame waits for a retrace: its frames last 1, 2 or 3 of them); at least
// one, at most MAX_STEP_FRAMES (a longer hitch -- a load, a stall -- doesn't carry the Mii on: the
// rest is dropped). NOMA_WUHU_BY_FRAMES=1: one a game frame whatever the time, as before this clock
// (host runs that must repeat: the host's time isn't the game's).
class FrameClock {
public:
    u32 frames() {
        static const bool by_frames = std::getenv("NOMA_WUHU_BY_FRAMES") && std::atoi(std::getenv("NOMA_WUHU_BY_FRAMES"));
        const u64 now = guest::host_time_us();
        if (by_frames || !last_us_) {
            last_us_ = now;
            return 1;
        }
        const u64 n = now > last_us_ ? (now - last_us_ + RETRACE_US / 2) / RETRACE_US : 0;
        if (n > MAX_STEP_FRAMES) {
            last_us_ = now;
            return MAX_STEP_FRAMES;
        }
        // (the remainder carried: the frames counted keep to the time; one at the least)
        last_us_ += (n ? n : 1) * RETRACE_US;
        return n ? (u32)n : 1;
    }
    static constexpr u64 RETRACE_US = 16683;
    static constexpr u32 MAX_STEP_FRAMES = 3;

private:
    u64 last_us_ = 0;
};

// NOMA_WUHU_SPEED_OFF: the World's speed-ups to turn off (WorldFeature bits, from a comma list)
u32 features_from_env() {
    u32 f = engine::g3d::WF_ALL;
    const char* e = std::getenv("NOMA_WUHU_SPEED_OFF");
    if (!e)
        return f;
    static const struct {
        const char* name;
        u32 bit;
    } k_names[] = {{"pvs", engine::g3d::WF_VISIBILITY}, {"detail", engine::g3d::WF_DETAIL},
                   {"anim", engine::g3d::WF_ANIM_LOD}, {"sort", engine::g3d::WF_MATERIAL_SORT},
                   {"all", engine::g3d::WF_ALL}};
    for (const auto& n : k_names)
        if (std::strstr(e, n.name))
            f &= ~n.bit;
    return f;
}

void set_clear(u32 rgba) {
    GFRAME(0x10);
    GS(u32, 0x8) = rgba;
    g::GXSetCopyClear((u32)&GS(u32, 0x8), 0xFFFFFF);
}
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
    void log_frame();
    static void world_frame(void* self);
    void take_job();
    void emit(engine::g3d::Pass pass);

    std::unique_ptr<PackageLoad> load_;
    std::unique_ptr<IslandMii> mii_;
    std::unique_ptr<Package> package_;
    std::unique_ptr<engine::collision::World> collision_;
    std::unique_ptr<engine::g3d::World> world_;
    std::unique_ptr<Player> player_;
    std::unique_ptr<FollowCamera> camera_;
    bool ready_ = false, prepared_ = false;
    u32 clear_ = CHANNEL_CLEAR;  // the EFB's clear colour while the island shows (set_clear)
    f32 k_ = 1;
    u64 start_us_ = 0;
    u32 frame_ = 0;  // the World's clock (sixtieths of a second since the island came up: FrameClock's)
    FrameClock clock_;
    u32 log_every_ = 600, log_frames_ = 0;
    u64 log_us_ = 0;
    // the World's frame as a job (world_frame): what it is given, what it makes, and what it took
    bool job_ = false;                   // a job is out (the World is the helper's until take_job)
    engine::g3d::Camera job_camera_;
    f32 job_ground_ = 0;                 // the ground under its eye (found on the game's thread)
    f32 job_frame_ = 0;
    engine::g3d::Packet built_[2];       // the passes' packets, made by the job
    bool built_ready_[2] = {false, false};
    u64 world_us_ = 0, waited_us_ = 0;   // the World's time (wherever it ran), the game's waits for it
    bool on_helper_ = false;             // (the last frame's World ran on the helper)
};

Island::~Island() {
    take_job();  // (the World is the helper's while a job is out)
    // the Mii's guest objects (the content's only ones); then the package's GPU resources, before
    // the package (native_draw.h), and what reads the package and the collision before them
    mii_.reset();
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
    std::string path = guest::data_dir() + PACKAGE_V2;
    if (FILE* f = std::fopen(path.c_str(), "rb"))
        std::fclose(f);
    else
        path = guest::data_dir() + PACKAGE;
    load_ = std::make_unique<PackageLoad>(path);
    load_->start();
    mii_ = std::make_unique<IslandMii>();
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
    // the Mii drawn MII_SCALE times as big against the island as the package's k makes it
    k_ = package_->env().mii_units_per_unit / MII_SCALE;
    clear_ = clear_of(*package_);

    const u64 t0 = guest::host_time_us();
    world_ = std::make_unique<engine::g3d::World>(*package_);
    world_->set_world_scale(k_);
    world_->set_features(features_from_env());
    const u64 t1 = guest::host_time_us();
    // (the uploads themselves happen on the renderer's thread when it reaches the command)
    engine::g3d::native_draw_prepare(*package_);
    prepared_ = true;
    const u64 t2 = guest::host_time_us();

    PlayerParams pp = PlayerParams::make(k_, mii_->height_scale(), mii_->gait());
    // its pace (its speeds are the room's, per its own size: MII_SCALE of the way there already)
    pp.walk_speed *= PACE / MII_SCALE;
    pp.run_speed *= PACE / MII_SCALE;
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

    const u64 t3 = guest::host_time_us();
    guest::log("wuhu: load: %u bytes in %.0f ms (read %.0f, check %.0f, collision %.0f, world %.1f, prepare %.1f "
               "ms); k %.3f, the Mii %.2f units tall (walks %.3f, runs %.3f a frame, step %.2f, slope limit %.0f "
               "degrees, drops up to %.1f)",
               (unsigned)package_->size(), ms_between(start_us_, t3), l.read_ms, l.check_ms, l.collision_ms,
               ms_between(t0, t1), ms_between(t1, t2), k_, pp.height, pp.walk_speed, pp.run_speed, pp.step_up,
               std::acos(pp.walkable_ny) / DEG, pp.max_drop);
    const fmt::Env& env = package_->env();
    guest::log("wuhu: speed-ups: visibility rows %s (%u cells, up to %.0f units over the ground), detail batches %s "
               "(beyond %.0f), animation rate by size %s, opaque items by material %s",
               env.pvs_row_bytes ? (world_->features() & engine::g3d::WF_VISIBILITY ? "on" : "off") : "none",
               (unsigned)package_->cells().count, env.pvs_max_height,
               env.detail_distance > 0 ? (world_->features() & engine::g3d::WF_DETAIL ? "on" : "off") : "none",
               env.detail_distance, world_->features() & engine::g3d::WF_ANIM_LOD ? "on" : "off",
               world_->features() & engine::g3d::WF_MATERIAL_SORT ? "on" : "off");
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

void Island::warp(u32 marker) {
    if (!world_ || marker >= package_->markers().count)
        return;
    place_at(package_->markers()[marker]);
}

void Island::update(const Controls& controls, bool frozen) {
    if (!world_)
        return;
    const u32 frames = clock_.frames();
    frame_ += frames;
    const f32 dt = TICK * (f32)frames;
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
    player_->update(move, dt);
    camera_->update(look, player_->position(), player_->yaw(), player_->speed() / player_->params().run_speed, dt);
    const engine::g3d::Camera& cam = camera_->camera();
    // (the last frame's job, if its draws never came; its stats for the log line: read only here,
    // between the job's take and the next one's start)
    take_job();
    if (++log_frames_ >= log_every_)
        log_frame();
    // the ground under the eye, for the visibility rows (none within their height: below it), by the
    // rule the stager placed the rows' eyes by (tools/wuhu/stage_wuhu.py): the highest surface facing
    // up that isn't an invisible fence or plain wall (water counts); here on the game's thread (the
    // collision is its), never while a job is out
    f32 ground = NAN;
    const fmt::Env& env = package_->env();
    if (env.pvs_row_bytes) {
        const f32 reach = env.pvs_max_height + 1.0f;
        engine::collision::Hit hit;
        auto floor = [](u16 a, Vec3 n) { return n.y > 0.05f && !engine::collision::kcl_is_fence(a); };
        ground = collision_->ground(cam.eye.x, cam.eye.z, cam.eye.y, cam.eye.y - reach, floor, &hit)
                     ? hit.point.y
                     : cam.eye.y - reach;
    }
    built_ready_[0] = built_ready_[1] = false;
    job_camera_ = cam, job_ground_ = ground, job_frame_ = (f32)frame_;
    job_ = on_helper_ = guest::platform_job_start(&Island::world_frame, this);
    if (!job_) {
        const u64 t0 = guest::host_time_us();
        world_->update((f32)frame_);
        world_->set_camera(cam, ground);
        world_us_ += guest::host_time_us() - t0;
    }
    mii_->update(engine::g3d::scaled(cam, k_), player_->position() * k_, player_->yaw(), player_->state(),
                 (player_->distance() - walked) * k_);
}

// The job (on the helper): the World's frame and both passes' packets, as update() and the two
// emits would make them (nothing the World reads changes between them).
void Island::world_frame(void* self) {
    Island& i = *(Island*)self;
    const u64 t0 = guest::host_time_us();
    i.world_->update(i.job_frame_);
    i.world_->set_camera(i.job_camera_, i.job_ground_);
    i.world_->build_packet(engine::g3d::Pass::OPAQUE, &i.built_[0]);
    i.world_->build_packet(engine::g3d::Pass::TRANSLUCENT, &i.built_[1]);
    i.world_us_ += guest::host_time_us() - t0;
}

// the job's packets, once it is done (the game's thread waits for it if it isn't)
void Island::take_job() {
    if (!job_)
        return;
    const u64 t0 = guest::host_time_us();
    guest::platform_job_wait();
    waited_us_ += guest::host_time_us() - t0;
    job_ = false;
    built_ready_[0] = built_ready_[1] = true;
}

// a pass's draws into the GX stream: the job's packet (into a packet of the ring, which keeps its
// id), else made now
void Island::emit(engine::g3d::Pass pass) {
    take_job();
    const int i = pass == engine::g3d::Pass::TRANSLUCENT;
    if (built_ready_[i]) {
        built_ready_[i] = false;
        engine::g3d::Packet* p = engine::g3d::native_draw_begin();
        const u32 id = p->id;
        std::swap(*p, built_[i]);
        p->id = id;
        engine::g3d::native_draw_submit(p);
        return;
    }
    // (World::emit's steps, its own time counted without the ring's waits)
    engine::g3d::Packet* p = engine::g3d::native_draw_begin();
    const u64 t0 = guest::host_time_us();
    world_->build_packet(pass, p);
    world_us_ += guest::host_time_us() - t0;
    engine::g3d::native_draw_submit(p);
}

void Island::log_frame() {
    const u64 now = guest::host_time_us();
    const f32 fps = now > log_us_ ? (f32)log_frames_ * 1e6f / (f32)(now - log_us_) : 0.0f;
    const f32 world_ms = (f32)world_us_ / 1000.0f / (f32)log_frames_, waited_ms = (f32)waited_us_ / 1000.0f / (f32)log_frames_;
    char world[96];
    if (on_helper_)
        std::snprintf(world, sizeof(world), "world %.2f ms a frame on the helper (waited %.2f)", world_ms, waited_ms);
    else
        std::snprintf(world, sizeof(world), "world %.2f ms a frame", world_ms);
    world_us_ = waited_us_ = 0;
    log_us_ = now, log_frames_ = 0;
    const engine::g3d::WorldStats& s = world_->stats();
    const Vec3 p = player_->position();
    static const char* const STATES[] = {"wait", "walk", "run", "fall"};
    guest::log("wuhu: fps %.1f, %s, cells %u/%u (visibility row of cell %d hid %u), instances %u (%u posed, %u out "
               "of view or under a pixel), items %u+%u (%u details culled), tris %u, combiners %u+%u cached, pos (%.1f, "
               "%.1f, %.1f) ground 0x%04X; yaw %.0f, %s, walked %.0f, camera yaw %.0f pitch %.0f at %.1f",
               fps, world, (unsigned)s.cells_visible, (unsigned)s.cells_total, s.pvs_cell, (unsigned)s.cells_pvs_hidden,
               (unsigned)s.instances_visible, (unsigned)s.poses, (unsigned)s.instances_small, (unsigned)s.items_opaque,
               (unsigned)s.items_translucent, (unsigned)s.details_culled, (unsigned)s.triangles,
               (unsigned)s.translations, (unsigned)s.translations_cached, p.x, p.y, p.z,
               (unsigned)player_->ground_attribute(), player_->yaw(), STATES[(int)player_->state()],
               player_->distance(), camera_->yaw(), camera_->pitch(), camera_->distance());
}

void Island::draw_opaque() {
    if (!world_)
        return;
    // (every frame: the channel's texture copies may set a clear colour of their own)
    if (clear_ != CHANNEL_CLEAR)
        set_clear(clear_);
    mii_->view();  // (the camera's viewport, scissor and projection: what the native draws take)
    emit(engine::g3d::Pass::OPAQUE);
    mii_->draw_opaque();
}

void Island::draw_translucent() {
    if (!world_)
        return;
    // (the projection again: the Mii's opaque pass may have loaded another)
    mii_->view();
    emit(engine::g3d::Pass::TRANSLUCENT);
    mii_->draw_translucent();
}

}  // namespace

IslandContent* make_content() {
    if (std::getenv("NOMA_WUHU_PLACEHOLDER"))
        return make_placeholder_content();
    return new Island();
}

}  // namespace engine::wuhu
