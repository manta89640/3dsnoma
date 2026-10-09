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
// The package: DATA/wuhu/island.ng3d, NG3D version 2 (a version 1 file, from before the speed work,
// is refused, the reason in the log: the island is staged again). The World gets the height of the ground under the camera's eye (the collision's highest
// surface below it facing up, not a fence: the stager's floors), which says whether the eye's cell's
// visibility row applies (world.h).
//
// NOMA_WUHU_AT=x,y,z[,yaw]: start on the ground at or below that place, or swimming there (host runs). NOMA_WUHU_START=
// <marker name>: start at another of the package's markers (a landmark, a warp place,
// or a camera preset view_*, whose target the Mii then stands at, facing away from its eye: the
// follow camera then sees about what the preset does). NOMA_WUHU_LOG_EVERY=N: the frame line every N
// frames (600). NOMA_WUHU_SPEED_OFF=pvs,detail,anim,sort: the World's speed-ups named turned off
// (world.h WorldFeature: the visibility rows, detail culling, the animation rate by size, the opaque
// items grouped by material), for comparisons on the console. NOMA_WUHU_BY_FRAMES=1: the Mii, the
// camera and the World move a sixtieth of a second a game frame, whatever the time (repeatable host
// runs). NOMA_WUHU_PLACEHOLDER=1: the stand-in content instead (content_placeholder.cpp: the scene's
// shell without the package).
//
// Shadows (docs/wuhu.md "Shadows"): the package's decals (WSR's silhouettes under the trees and props,
// the cars' under them) are drawn always; the shadow map about the Mii (world.h: the trees, props,
// windmills, vehicles and the Mii itself casting) unless NOMA_WUHU_SHADOWS=0, NOMA_WUHU_SHADOW_MAP=N
// texels a side (512; 256 and 1024 the others worth trying on the console). The Mii casts through its
// stand-in (the package's shadow_proxy, a capsule its height, never seen; on foot only: in a vehicle the
// vehicle casts), and has a blob under it (shadow_blob, WSR's spectators' silhouette) on the ground
// below its feet, which the map fades out where it covers: so the blob shows only without the map.
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
#include "engine/wuhu/sand_tracks.h"
#include "engine/wuhu/vehicles.h"
#include "noma_decomp.h"

namespace engine::wuhu {

IslandContent* make_placeholder_content();

namespace {

namespace fmt = engine::g3d::fmt;
using engine::g3d::Package;
using engine::math::Mtx34;
using engine::math::Vec3;

const char* const PACKAGE = "/wuhu/island.ng3d";
constexpr f32 TICK = 1.0f / 60.0f;
constexpr f32 DEG = 3.14159265f / 180.0f;
constexpr u32 NONE32 = 0xFFFFFFFFu;
// the Skydiving parachute (tools/wuhu/stage_wuhu.py stages it as the one instance of this class):
// shown over the Mii while it glides (Player::gliding)
constexpr u16 PARACHUTE_CLASS = 0xFFFEu;
const char* const PARACHUTE_MODEL = "WS2_omk_parachute_01";
constexpr f32 WSR_MII_HEIGHT = 15.24f;  // island units (research k.md)
constexpr f32 PARACHUTE_HANG = 0.8f;    // its risers' end over the Mii's feet, times its height
// the Mii's stand-ins for the shadows (stage_wuhu.py shadow_models): a caster a unit tall, a blob a
// unit across; the blob is BLOB_WIDTH Mii heights across, the stand-in as wide as the Mii's height x 0.16
constexpr u16 SHADOW_PROXY_CLASS = 0xFFFDu, SHADOW_BLOB_CLASS = 0xFFFCu;
const char* const SHADOW_PROXY_MODEL = "shadow_proxy";
const char* const SHADOW_BLOB_MODEL = "shadow_blob";
constexpr f32 BLOB_WIDTH = 0.6f;
constexpr u32 SHADOW_MAP_DEFAULT = 512;
// WSR's wake under the boat the player drives (the stager's instance of WS2_common_cruiser_ef of its
// own: shown and faded by the boat's speed, Vehicles::wake), its foam's alpha times its C0's
constexpr u16 WAKE_CLASS = 0xFFFBu;
const char* const WAKE_MODEL = "WS2_common_cruiser_ef#driven";
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
// whatever the frame rate (the per-frame pace of 1276ec5, 2, was that at 25 fps and 2.4 times it at
// 60). Its run is RUN_SPEED_MULTIPLIER times faster again than that, its size 0.8 of the 1.7 it had.
constexpr f32 MII_SCALE = 1.7f * 0.8f;
constexpr f32 PACE = 2.0f * 25.0f / 60.0f;
constexpr f32 RUN_SPEED_MULTIPLIER = 2.5f;

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
    bool enter_vehicle();
    bool leave_vehicle();
    void park_vehicle();
    void update_parachute();
    void update_shadows();
    void update_sea(const engine::g3d::Camera& cam);
    Vec3 controlled_position() const;
    bool in_car() const { return vehicles_ && vehicles_->driven() != Vehicles::NONE && !vehicles_->boat(); }
    f32 controlled_yaw() const;
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
    std::unique_ptr<Vehicles> vehicles_;
    std::unique_ptr<SandTracks> sand_;
    u32 parachute_instance_ = NONE32;
    bool parachute_visible_ = false;
    u32 parachute_since_ = 0;  // (frame_ the glide began)
    u32 proxy_instance_ = NONE32, blob_instance_ = NONE32;
    bool proxy_visible_ = true, blob_visible_ = true;
    u32 wake_instance_ = NONE32;
    u16 wake_material_ = fmt::NONE16;
    bool wake_visible_ = false;
    s16 wake_alpha_ = -1;
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
    Vec3 job_focus_{0, 0, 0};            // where the shadow map is about (the Mii, or its vehicle)
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
    sand_.reset();
    vehicles_.reset();
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
    for (u32 i = 0; i < world_->instance_count(); i++) {
        const u16 c = world_->instance_class(i);
        if (c == PARACHUTE_CLASS && parachute_instance_ == NONE32)
            parachute_instance_ = i;
        if (c == SHADOW_PROXY_CLASS && proxy_instance_ == NONE32)
            proxy_instance_ = i;
        if (c == SHADOW_BLOB_CLASS && blob_instance_ == NONE32)
            blob_instance_ = i;
    }
    // the shadow map (NOMA_WUHU_SHADOWS=0: none; NOMA_WUHU_SHADOW_MAP: its size)
    {
        const char* on = std::getenv("NOMA_WUHU_SHADOWS");
        const char* size = std::getenv("NOMA_WUHU_SHADOW_MAP");
        world_->set_shadow_map(on && !std::atoi(on) ? 0 : size ? (u32)std::atoi(size) : SHADOW_MAP_DEFAULT);
        if (package_->shadow_env())
            guest::log("wuhu: shadows: decals, the shadow map %s (%u texels a side, %.0f units), the Mii's stand-in %s",
                       world_->shadow_map() ? "on" : "off", (unsigned)world_->shadow_map(),
                       package_->shadow_env()->map_extent, proxy_instance_ != NONE32 ? "there" : "missing");
        else
            guest::log("wuhu: shadows: the package has none (staged before them)");
    }
    if (parachute_instance_ != NONE32)
        world_->set_group_visible(PARACHUTE_MODEL, false);
    for (u32 i = 0; i < world_->instance_count() && wake_instance_ == NONE32; i++)
        if (world_->instance_class(i) == WAKE_CLASS) {
            const fmt::Model& m = package_->models()[package_->instances()[i].model];
            if (m.n_batches) {
                wake_instance_ = i;
                wake_material_ = package_->batches()[m.first_batch].material;
                world_->set_group_visible(WAKE_MODEL, false);
            }
        }
    // (a package staged before the sea's waves, wakes and trails has none of them: the sea flat, no wake
    // or trail behind any boat; said here as for the shadows)
    {
        const fmt::Sea* sea = package_->sea();
        const bool trails = sea && sea->trail_batch != fmt::NONE16;
        guest::log("wuhu: the sea: %s, the boats' trails %s, the driven boat's wake %s%s", sea ? "waves" : "flat",
                   trails ? "on" : "none", wake_instance_ != NONE32 ? "there" : "missing",
                   trails && wake_instance_ != NONE32 ? "" : " (the package was staged before them)");
    }
    vehicles_ = std::make_unique<Vehicles>(*world_, *package_, *collision_);
    const u64 t1 = guest::host_time_us();
    // (the uploads themselves happen on the renderer's thread when it reaches the command)
    engine::g3d::native_draw_prepare(*package_);
    prepared_ = true;
    const u64 t2 = guest::host_time_us();

    PlayerParams pp = PlayerParams::make(k_, mii_->height_scale(), mii_->gait());
    // its pace (its speeds are the room's, per its own size: MII_SCALE of the way there already)
    pp.walk_speed *= PACE / MII_SCALE;
    pp.run_speed *= PACE / MII_SCALE * RUN_SPEED_MULTIPLIER;
    player_ = std::make_unique<Player>(*collision_, pp);
    player_->set_obstacles(vehicles_->obstacles());
    sand_ = std::make_unique<SandTracks>(*collision_);
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
    // NOMA_WUHU_AT=x,y,z[,yaw]: a place of one's own instead (host runs: a ledge, a shore), the ground
    // at or below it, or the water there
    f32 at[4] = {0, 0, 0, 0};
    const char* at_env = std::getenv("NOMA_WUHU_AT");
    if (at_env && std::sscanf(at_env, "%f,%f,%f,%f", &at[0], &at[1], &at[2], &at[3]) >= 3 &&
        (player_->place(Vec3{at[0], at[1], at[2]}, at[3]) ||
         player_->place_swimming(Vec3{at[0], at[1], at[2]}, at[3]))) {
        camera_->reset(player_->position(), player_->yaw());
        guest::log("wuhu: NOMA_WUHU_AT: at (%.1f, %.1f, %.1f) facing %.0f", player_->position().x,
                   player_->position().y, player_->position().z, player_->yaw());
    } else if (!start || !place_at(*start)) {
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
// it), else the sea's surface there (swimming); the camera behind it.
bool Island::place_at(const fmt::Marker& m) {
    Vec3 at{m.position[0], m.position[1], m.position[2]};
    f32 dx = m.dir[0], dz = m.dir[2];
    if (m.kind == fmt::MK_CAMERA) {
        dx = m.dir[0] - at.x, dz = m.dir[2] - at.z;
        at = Vec3{m.dir[0], m.dir[1], m.dir[2]};
    }
    const f32 yaw = dx * dx + dz * dz > 1e-12f ? std::atan2(dx, dz) / DEG : 0.0f;
    // (a landmark out at sea -- a whale watch, a diving spot -- has no ground: the Mii swims there)
    const f32 level = world_->sea().valid() ? world_->sea().params().level : 0.0f;
    if (!player_->place(at, yaw) && !player_->place_swimming(Vec3{at.x, level, at.z}, yaw)) {
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

Vec3 Island::controlled_position() const {
    return vehicles_->driven() == Vehicles::NONE ? player_->position() : vehicles_->position();
}

f32 Island::controlled_yaw() const {
    return vehicles_->driven() == Vehicles::NONE ? player_->yaw() : vehicles_->yaw();
}

// into the vehicle nearest the Mii, if one is within reach (not while in the air; a swimmer only
// into a boat)
bool Island::enter_vehicle() {
    if (player_->airborne())
        return false;
    const PlayerParams& pp = player_->params();
    const f32 reach = pp.radius + 0.4f * pp.height;
    const u32 v = vehicles_->nearest(player_->position(), reach, player_->swimming());
    if (v == Vehicles::NONE) {
        f32 d = 0;
        const u32 near = vehicles_->nearest(player_->position(), 1e9f, player_->swimming(), &d);
        if (near != Vehicles::NONE)
            guest::log("wuhu: no vehicle within reach (%.1f): the nearest, %u, is %.1f off", reach, (unsigned)near, d);
        return false;
    }
    vehicles_->enter(v);
    player_->set_obstacles(vehicles_->obstacles(v));
    sand_->end_trail();
    // the camera framed for the vehicle: as for a Mii as tall as 0.8 of half the vehicle's length (a
    // car a little further off than the Mii, a cruiser well back: further and a car's camera swings
    // through the hedges and house fronts the collision doesn't have)
    const Obstacle box = vehicles_->box(v);
    FollowCameraParams cp = FollowCameraParams::make(std::max(pp.height, 0.8f * box.half_length));
    cp.aspect = mii_->aspect();
    camera_->set_params(cp);
    guest::log("wuhu: into the %s (vehicle %u)", vehicles_->boat() ? "boat" : "car", (unsigned)v);
    return true;
}

// out of the vehicle: the Mii beside it (a side, else behind or before it) where it can stand or, by
// a boat, swim; false (still in it) if there is nowhere
bool Island::leave_vehicle() {
    const u32 v = vehicles_->driven();
    if (v == Vehicles::NONE)
        return true;
    const PlayerParams& pp = player_->params();
    const bool boat = vehicles_->boat();
    // (the vehicle is an obstacle again for where the Mii may be)
    player_->set_obstacles(vehicles_->obstacles());
    bool out = false;
    for (const Vec3& at : vehicles_->exits(pp.radius + 0.1f * pp.height)) {
        // (`at` is at the vehicle's bottom: a boat's is about the water's surface)
        Vec3 feet;
        if (boat && player_->place_swimming(at, vehicles_->yaw())) {
            out = true;
            break;
        }
        // ground no higher than a step over its bottom, no lower than a safe drop under it
        if (player_->can_stand(Vec3{at.x, at.y + pp.step_up, at.z}, at.y - pp.max_drop, &feet)) {
            player_->place(feet, vehicles_->yaw());
            out = true;
            break;
        }
    }
    if (!out) {
        player_->set_obstacles(vehicles_->obstacles(v));
        guest::log("wuhu: no room to get out of vehicle %u here", (unsigned)v);
        return false;
    }
    vehicles_->leave();
    FollowCameraParams cp = FollowCameraParams::make(pp.height);
    cp.aspect = mii_->aspect();
    camera_->set_params(cp);
    camera_->reset(player_->position(), player_->yaw());
    guest::log("wuhu: out of vehicle %u at (%.1f, %.1f, %.1f)", (unsigned)v, player_->position().x,
               player_->position().y, player_->position().z);
    return true;
}

// a warp from a vehicle: the vehicle stays where it is, the Mii goes
void Island::park_vehicle() {
    if (vehicles_->driven() == Vehicles::NONE)
        return;
    vehicles_->leave();
    player_->set_obstacles(vehicles_->obstacles());
    FollowCameraParams cp = FollowCameraParams::make(player_->params().height);
    cp.aspect = mii_->aspect();
    camera_->set_params(cp);
}

// the parachute over the Mii while it glides (its model hidden otherwise)
void Island::update_parachute() {
    if (parachute_instance_ == NONE32)
        return;
    const bool open = vehicles_->driven() == Vehicles::NONE && player_->gliding();
    if (open != parachute_visible_) {
        world_->set_group_visible(PARACHUTE_MODEL, open);
        parachute_visible_ = open;
        parachute_since_ = frame_;
    }
    if (!open)
        return;
    // its clip is the canopy opening (30 frames): played once from the glide's start, then held open
    world_->set_instance_anim_frame(parachute_instance_, (f32)(frame_ - parachute_since_));
    // hung from the Mii's shoulders, as big against it as against WSR's Mii (15.24 units tall)
    const f32 h = player_->params().height, sc = h / WSR_MII_HEIGHT;
    const f32 a = player_->yaw() * DEG, c = std::cos(a) * sc, s = std::sin(a) * sc;
    const Vec3 p = player_->position();
    const Mtx34 m{{{c, 0, s, p.x}, {0, sc, 0, p.y + PARACHUTE_HANG * h}, {-s, 0, c, p.z}}};
    world_->set_instance_transform(parachute_instance_, m);
}

// The Mii's stand-in caster and its blob, where the Mii stands (on foot only; in a vehicle the vehicle
// casts and its shadow is its own): the stand-in at its feet, turned with it, as tall as it; the blob on
// the ground under its feet (the collision's, facing up, within a few heights below), lying along the
// ground's slope, lifted a little; none while it swims (the ground is the sea's floor) or over nothing.
void Island::update_shadows() {
    const bool on_foot = vehicles_->driven() == Vehicles::NONE;
    const PlayerParams& pp = player_->params();
    const Vec3 p = player_->position();
    auto show = [&](u32 inst, const char* model, bool* visible, bool want) {
        if (inst != NONE32 && want != *visible) {
            world_->set_group_visible(model, want);
            *visible = want;
        }
    };
    show(proxy_instance_, SHADOW_PROXY_MODEL, &proxy_visible_, on_foot);
    if (on_foot && proxy_instance_ != NONE32) {
        const f32 a = player_->yaw() * DEG, h = pp.height, c = std::cos(a) * h, sn = std::sin(a) * h;
        world_->set_instance_transform(proxy_instance_, Mtx34{{{c, 0, sn, p.x}, {0, h, 0, p.y}, {-sn, 0, c, p.z}}});
    }
    if (blob_instance_ == NONE32)
        return;
    engine::collision::Hit hit;
    auto floor = [](u16 attr, Vec3 n) { return n.y > 0.5f && !engine::collision::kcl_is_fence(attr); };
    const bool ground = on_foot && !player_->swimming() &&
                        collision_->ground(p.x, p.z, p.y + 0.5f * pp.height, p.y - 4 * pp.height, floor, &hit);
    show(blob_instance_, SHADOW_BLOB_MODEL, &blob_visible_, ground);
    if (!ground)
        return;
    // the blob's axes: x across the ground's slope, z along it, y the ground's normal; a unit across,
    // BLOB_WIDTH Mii heights here, a twentieth of a unit up the normal
    const Vec3 n = engine::math::normalize(hit.normal);
    Vec3 x = engine::math::cross(Vec3{0, 0, 1}, n);
    x = engine::math::normalize(engine::math::dot(x, x) > 1e-6f ? x : Vec3{1, 0, 0});
    const Vec3 z = engine::math::cross(x, n);
    const f32 w = BLOB_WIDTH * pp.height;
    const Vec3 at = hit.point + n * 0.05f;
    world_->set_instance_transform(blob_instance_, Mtx34{{{x.x * w, n.x * w, z.x * w, at.x},
                                                          {x.y * w, n.y * w, z.y * w, at.y},
                                                          {x.z * w, n.z * w, z.z * w, at.z}}});
}

// The sea's part of a frame: the boats at anchor near the eye on the waves, the driven boat's wake
// under it (hidden when it barely moves; its alpha in steps of 1/32, each a combiner translation the
// World keeps a few of)
void Island::update_sea(const engine::g3d::Camera& cam) {
    vehicles_->float_moored(cam.eye, (f32)frame_ * TICK);
    if (wake_instance_ == NONE32)
        return;
    Mtx34 m;
    f32 strength = 0;
    const bool show = vehicles_->wake(&m, &strength) && strength > 0.02f;
    if (show != wake_visible_) {
        world_->set_group_visible(WAKE_MODEL, show);
        wake_visible_ = show;
    }
    if (!show)
        return;
    world_->set_instance_transform(wake_instance_, m);
    const s16 a = (s16)std::lround(std::min(1.0f, strength) * 32.0f) * 255 / 32;
    if (a != wake_alpha_) {
        const s16 c0[4] = {255, 255, 255, a};
        world_->set_material_register(wake_material_, fmt::CR_TEV0, c0);
        wake_alpha_ = a;
    }
}

void Island::warp(u32 marker) {
    if (!world_ || marker >= package_->markers().count)
        return;
    take_job();
    park_vehicle();
    sand_->end_trail();
    place_at(package_->markers()[marker]);
}

void Island::update(const Controls& controls, bool frozen) {
    if (!world_)
        return;
    // (the last frame's job, if its draws never came: the World is the game's thread's from here to
    // the next job's start; its stats are read for the log line here too)
    take_job();
    const u32 frames = clock_.frames();
    frame_ += frames;
    const f32 dt = TICK * (f32)frames;
    PlayerInput move;
    FollowCameraInput look;
    bool toggled = false;
    if (!frozen) {
        if (controls.vehicle)
            toggled = vehicles_->driven() == Vehicles::NONE ? enter_vehicle() : leave_vehicle();
        if (vehicles_->driven() == Vehicles::NONE && !toggled) {
            camera_->stick_to_world(controls.move_x, controls.move_y, &move.move_x, &move.move_z);
            move.jump = controls.jump;
            move.jump_held = controls.jump_held;
            move.fast_run = controls.fast_run;
        }
        // the camera stick and the D-pad: up looks up (the camera goes down round the Mii)
        look.orbit_x = controls.camera_x;
        look.orbit_y = -controls.camera_y;
        look.zoom = controls.zoom;
        look.behind = controls.behind;
    }
    const f32 walked = player_->distance(), yaw_before = player_->yaw();
    const Vec3 before = controlled_position();
    if (vehicles_->driven() == Vehicles::NONE) {
        player_->update(move, dt);
        const bool on_foot = !player_->airborne() && !player_->swimming() && !toggled;
        sand_->walk(before, player_->position(), player_->distance() - walked, player_->yaw(), player_->params().height,
                    on_foot, frame_);
    } else {
        if (!frozen && !toggled)
            vehicles_->drive(controls.move_y, controls.move_x, dt, (f32)frame_ * TICK);
        if (!vehicles_->boat()) {
            const f32 track = vehicles_->track_width(vehicles_->driven());
            sand_->drive(before, vehicles_->position(), vehicles_->yaw(), track, 0.12f * track, frame_);
        }
    }
    const Vec3 at = controlled_position();
    const f32 yaw = controlled_yaw();
    const f32 camera_speed = vehicles_->driven() == Vehicles::NONE ? player_->speed() / player_->params().run_speed
                                                                   : vehicles_->speed_fraction();
    camera_->set_obstacles(vehicles_->obstacles(vehicles_->driven()));
    camera_->update(look, at, yaw, camera_speed, dt);
    const engine::g3d::Camera& cam = camera_->camera();
    update_parachute();
    update_shadows();
    update_sea(cam);
    // (a swimmer drawn on the waves: its body stays on the collision's surface; asked of the World
    // here, before its job)
    Vec3 drawn = at;
    if (vehicles_->driven() == Vehicles::NONE && player_->swimming() && world_->sea().valid())
        drawn.y += world_->sea_height(at.x, at.z, (f32)frame_ * TICK) - world_->sea().params().level;
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
    job_camera_ = cam, job_ground_ = ground, job_frame_ = (f32)frame_, job_focus_ = at;
    job_ = on_helper_ = guest::platform_job_start(&Island::world_frame, this);
    if (!job_) {
        const u64 t0 = guest::host_time_us();
        world_->update((f32)frame_);
        world_->set_camera(cam, ground);
        world_->set_shadow_focus(at);
        world_us_ += guest::host_time_us() - t0;
    }
    // what the Mii is doing, for its clip: on foot the player's state; in a vehicle at its seat
    MiiMotion motion;
    motion.frames = frames;
    Vec3 feet = drawn;
    if (vehicles_->driven() != Vehicles::NONE) {
        motion.kind = vehicles_->boat() ? MiiMotion::BOAT : MiiMotion::CAR;
        motion.steer = frozen ? 0.0f : controls.move_x;
        feet = vehicles_->seat();
    } else {
        const Player& p = *player_;
        motion.kind = p.swimming() ? MiiMotion::SWIM
                      : p.gliding() ? MiiMotion::GLIDE
                      : p.sliding() ? MiiMotion::SLIDE
                      : p.airborne() ? MiiMotion::AIR
                                     : MiiMotion::GROUND;
        motion.walked = (p.distance() - walked) * k_;
        motion.turned = wrap_deg(p.yaw() - yaw_before);
        motion.sprint = move.fast_run;
        motion.jump = p.jumping();
        motion.leap = p.leaping();
        motion.rise = p.vertical_speed() / std::max(p.jump_speed(), 1e-3f);
        motion.air_time = (f32)p.air_ticks() * TICK;
        motion.depth = p.water_depth() * k_;
    }
    mii_->update(engine::g3d::scaled(cam, k_), feet * k_, yaw, motion);
}

// The job (on the helper): the World's frame and both passes' packets, as update() and the two
// emits would make them (nothing the World reads changes between them).
void Island::world_frame(void* self) {
    Island& i = *(Island*)self;
    const u64 t0 = guest::host_time_us();
    i.world_->update(i.job_frame_);
    i.world_->set_camera(i.job_camera_, i.job_ground_);
    i.world_->set_shadow_focus(i.job_focus_);
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
    const Vec3 p = controlled_position();
    static const char* const STATES[] = {"wait", "walk", "run", "fall", "jump", "swim"};
    static_assert(sizeof(STATES) / sizeof(STATES[0]) == (size_t)PlayerState::SWIM + 1, "a name a state");
    guest::log("wuhu: fps %.1f, %s, cells %u/%u (visibility row of cell %d hid %u), instances %u (%u posed, %u out "
               "of view or under a pixel), items %u+%u (%u details culled), tris %u, combiners %u+%u cached, shadow "
               "casters %u (%u instances, %u tris), receivers %u, decals faded %u, sea grid %u, trails %u, pos (%.1f, "
               "%.1f, %.1f) ground 0x%04X; yaw %.0f, %s, walked %.0f, camera yaw %.0f pitch %.0f at %.1f",
               fps, world, (unsigned)s.cells_visible, (unsigned)s.cells_total, s.pvs_cell, (unsigned)s.cells_pvs_hidden,
               (unsigned)s.instances_visible, (unsigned)s.poses, (unsigned)s.instances_small, (unsigned)s.items_opaque,
               (unsigned)s.items_translucent, (unsigned)s.details_culled, (unsigned)s.triangles,
               (unsigned)s.translations, (unsigned)s.translations_cached, (unsigned)s.shadow_casters,
               (unsigned)s.shadow_instances, (unsigned)s.shadow_triangles, (unsigned)s.shadow_receivers,
               (unsigned)s.shadow_faded, (unsigned)s.sea_grid, (unsigned)s.trails, p.x, p.y, p.z,
               (unsigned)player_->ground_attribute(), controlled_yaw(),
               vehicles_->driven() != Vehicles::NONE ? (vehicles_->boat() ? "boat" : "car") : STATES[(int)player_->state()],
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
    // (in a car the Mii isn't drawn: WSR's car is closed, its windows opaque, and the seated Mii
    // only a draw no one sees; at a boat's helm it shows)
    if (!in_car()) {
        mii_->draw_opaque();
        mii_->view();  // (the projection again for the sand's marks: the Mii's pass may have loaded another)
    }
    sand_->draw(camera_->camera(), k_, frame_);
}

void Island::draw_translucent() {
    if (!world_)
        return;
    // (the projection again: the Mii's opaque pass may have loaded another)
    mii_->view();
    emit(engine::g3d::Pass::TRANSLUCENT);
    if (!in_car())
        mii_->draw_translucent();
}

}  // namespace

IslandContent* make_content() {
    if (std::getenv("NOMA_WUHU_PLACEHOLDER"))
        return make_placeholder_content();
    return new Island();
}

}  // namespace engine::wuhu
