// NOMA_SELFTEST=walk: the island's collision (engine/collision/kcl), the player's walking
// (engine/wuhu/player) and the follow camera (engine/wuhu/follow_camera), on Wii Sports Resort's own
// collision files from your extraction (nothing of them is in this repository):
//
//   NOMA_KCL_DIR     the folder with WS2_common_island1..5.kcl (Stage/Static/StageArc.carc/Island);
//                    without it the test is skipped
//   NOMA_KCL_GROUND  a folder with the research rasters kcl_ground.npy and kcl_water.npy (the top
//                    non-water and water surfaces, float32, 25-unit cells from (-36000, -16000)):
//                    ground() is checked against them
//   NOMA_KCL_TRUNKS  tree trunks as cylinders, a CSV of x,z,y0,y1,radius lines
//   NOMA_WALK_TRAIL  waypoints up the volcano's dirt trail, a CSV of x,z,y lines (a walkable path
//                    over the KCL: from the river, y 109, to where the dirt ends on the volcano's
//                    shoulder, y 1010; above it the way up is steeper than 45 degrees or in tunnels)
//   NOMA_WALK_K      Wii no Ma units per island unit (default 10: Wii no Ma's Mii against WSR's,
//                    body and head, as the staged package's environment gives it; docs/wuhu_design.md
//                    §8)
//   NOMA_WALK_K2     a second k for a second long walk (default 9: a Mii a tenth bigger)
//   NOMA_WALK_OUT    a folder for paths.csv: every walk's path, ten points a second
//
// The walks, each for a Mii at both k: from the town's spawn north into the coin-toss fountain (its
// rim and basin stop the Mii), into a building's wall by the plaza, up the volcano's dirt trail
// (waypoints), into a tree's trunk, a rock shore out into the sea and back (it wades in, swims once
// out of its depth, and wades out again), a beach into the sea (off the sand's low edge it swims),
// a tapped and a held jump (their heights) and a running one (its momentum), to the edge of a grass ledge north of the plaza (a drop of 100: refused, it stops
// at the edge), off a lower ledge south of it (24: it falls and lands), to the top of a rock slope
// too steep to stand on (178 down to the sand: refused), and from halfway down it (put there, it
// slides down, keeping to it), across the suspension bridge (ハラハラつり橋), and random walking (10
// minutes from the spawn, 5 on the hills, 5 on a beach) with the camera turned now and then. Every
// tick of every walk checks that the Mii stands on the ground when not falling or swimming, is in no
// wall, isn't in deep water unless swimming, swims only at a water surface with no land over it (the
// sea's plane goes on under the island), didn't pass through a wall since the last tick, and that
// nothing stands between the camera and the Mii's head, nor water over the camera. Then the queries are timed.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "runtime.h"
#include "engine/collision/kcl.h"
#include "engine/input/remote.h"
#include "engine/wuhu/follow_camera.h"
#include "engine/wuhu/player.h"

namespace guest {

namespace {

using namespace engine::collision;
using engine::wuhu::FollowCamera;
using engine::wuhu::FollowCameraInput;
using engine::wuhu::FollowCameraParams;
using engine::wuhu::Player;
using engine::wuhu::PlayerInput;
using engine::wuhu::PlayerParams;
using engine::wuhu::PlayerState;

constexpr f32 PI = 3.14159265f;
constexpr f32 TICK = 1.0f / 60.0f;

std::vector<u8> slurp(const std::string& path) {
    std::vector<u8> d;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return d;
    fseek(f, 0, SEEK_END);
    d.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    if (fread(d.data(), 1, d.size(), f) != d.size())
        d.clear();
    fclose(f);
    return d;
}

// a 2-D little-endian float32 .npy (numpy's format 1.0)
struct Raster {
    u32 rows = 0, cols = 0;
    std::vector<f32> v;
    f32 at(u32 r, u32 c) const { return v[(size_t)r * cols + c]; }
};
bool read_npy(const std::string& path, Raster* out) {
    std::vector<u8> d = slurp(path);
    if (d.size() < 10 || memcmp(d.data(), "\x93NUMPY", 6) != 0)
        return false;
    u32 hl = d[8] | d[9] << 8;
    std::string h(d.begin() + 10, d.begin() + 10 + hl);
    if (h.find("'<f4'") == std::string::npos || h.find("'fortran_order': False") == std::string::npos)
        return false;
    size_t s = h.find("'shape': (");
    if (s == std::string::npos || sscanf(h.c_str() + s + 10, "%u, %u", &out->rows, &out->cols) != 2)
        return false;
    size_t n = (size_t)out->rows * out->cols;
    if (d.size() < 10 + hl + n * 4)
        return false;
    out->v.resize(n);
    memcpy(out->v.data(), d.data() + 10 + hl, n * 4);
    return true;
}

struct Rng {
    u64 s;
    u32 next() {
        s ^= s >> 12, s ^= s << 25, s ^= s >> 27;
        return (u32)((s * 0x2545F4914F6CDD1Dull) >> 32);
    }
    f32 uniform() { return (next() >> 8) * (1.0f / 16777216.0f); }  // [0, 1)
};

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool floor_of(u16 a, Vec3 n) { return n.y > 0.05f && !kcl_is_water(a) && !kcl_is_fence(a); }
bool water_of(u16 a, Vec3 n) { return n.y > 0.05f && kcl_is_water(a); }

// ---- ground() against the research rasters

void check_rasters(const World& w, const std::string& dir) {
    Raster g, wt;
    if (!read_npy(dir + "/kcl_ground.npy", &g) || !read_npy(dir + "/kcl_water.npy", &wt) || g.rows != wt.rows ||
        g.cols != wt.cols) {
        log("selftest walk: no rasters in %s (NOMA_KCL_GROUND): skipped", dir.c_str());
        return;
    }
    const f32 X0 = -36000, Z0 = -16000, RES = 25;
    Rng rng{0x5EED};
    // 10 000 cells with land under them, 2 000 anywhere
    for (int pass = 0; pass < 2; pass++) {
        const Raster& ref = pass == 0 ? g : wt;
        bool (*accept)(u16, Vec3) = pass == 0 ? +[](u16 a, Vec3) { return !kcl_is_water(a); }
                                              : +[](u16 a, Vec3) { return kcl_is_water(a); };
        u32 n = 0, agree = 0, both_none = 0, at_lips = 0, wrong = 0, logged = 0;
        f32 worst = 0;
        // A point a hair's breadth outside a triangle's edge still has it under it (kcl.cpp's EDGE_TOL),
        // so at the lip of a cliff ground() may give the lip where the raster has the face just under
        // it: the raster's height is then the next surface down, and the lip's surface ends within 0.02.
        auto at_lip = [&](f32 x, f32 z, f32 ours, f32 want) {
            Hit b;
            if (want >= ours || !w.ground(x, z, ours - 0.001f, -1e9f, accept, &b) ||
                std::fabs(b.point.y - want) >= 0.01f)
                return false;
            for (int a = 0; a < 4; a++) {
                Hit g;
                f32 dx = a == 0 ? 0.02f : a == 1 ? -0.02f : 0, dz = a == 2 ? 0.02f : a == 3 ? -0.02f : 0;
                if (!w.ground(x + dx, z + dz, 1e9f, -1e9f, accept, &g) || std::fabs(g.point.y - ours) > 0.05f)
                    return true;
            }
            return false;
        };
        for (u32 tries = 0; n < 12000 && tries < 10000000; tries++) {
            u32 r = rng.next() % ref.rows, c = rng.next() % ref.cols;
            f32 want = ref.at(r, c);
            bool want_some = std::isfinite(want);
            if (n < 10000 && !want_some)
                continue;  // (the first 10 000 on a surface)
            n++;
            f32 x = X0 + (c + 0.5f) * RES, z = Z0 + (r + 0.5f) * RES;
            Hit h;
            bool got = w.ground(x, z, 1e9f, -1e9f, accept, &h);
            if (!got && !want_some) {
                both_none++, agree++;
            } else if (got && want_some && std::fabs(h.point.y - want) < 0.01f) {
                agree++;
                worst = std::max(worst, std::fabs(h.point.y - want));
            } else if (got && want_some && at_lip(x, z, h.point.y, want)) {
                agree++, at_lips++;
            } else {
                wrong++;
                if (logged++ < 8)
                    log("selftest walk: %s at (%.1f, %.1f): %s, the raster %s%.3f", pass ? "water" : "ground", x, z,
                        got ? std::to_string(h.point.y).c_str() : "none", want_some ? "" : "none ",
                        want_some ? want : 0);
            }
        }
        log("selftest walk: ground() vs research raster kcl_%s.npy: %u of %u points agree (%u on a surface within "
            "%.4f, %u at a lip with the raster's surface just under it, %u with none), %u differ: %s",
            pass ? "water" : "ground", agree, n, agree - both_none - at_lips, worst, at_lips, both_none, wrong,
            wrong ? "MISMATCH" : "ok");
    }
}

// ---- walks

struct Tally {
    u32 ticks = 0, off_ground = 0, in_wall = 0, deep = 0, tunnel = 0, cam_blocked = 0, cam_wet = 0;
    u32 swim = 0, swim_dry = 0, under_land = 0;  // swimming ticks; ... with no water at the surface; under land
    u32 air = 0, max_air = 0;
    bool fell = false;
    f32 max_overlap = 0, max_depth = 0, min_y = 1e30f, max_y = -1e30f;
    u64 tests = 0, queries = 0;
    u32 max_tests = 0;
    Vec3 max_at{0, 0, 0};  // where the most were, and how the Mii was
    int max_state = 0;
    u32 cam_pulled = 0;    // ticks the terrain held the camera in
    f32 cam_min = 1e30f;
    f32 max_over_slope = 0;  // the feet's height over a slope too steep to stand on, falling or sliding
    bool broken() const {
        return off_ground || in_wall || deep || tunnel || cam_blocked || cam_wet || swim_dry || under_land;
    }
};

// what the scene would do each tick: the drive gives the stick (camera-relative, as the player holds
// it) or a world direction, the run button and the camera stick
struct Drive {
    f32 sx = 0, sy = 0;           // the stick (camera-relative)
    bool world = false;           // ...or a world direction in wx, wz
    f32 wx = 0, wz = 0;
    bool run = false;
    bool jump = false, jump_held = false;
    FollowCameraInput cam;
};

class Walker {
public:
    Walker(const World& w, const PlayerParams& pp, FILE* out, const char* tag)
        : w_(w), pp_(pp), cp_(FollowCameraParams::make(pp.height)), out_(out), tag_(tag) {}

    // runs `seconds` (or until drive returns false); returns the player at the end
    Tally run(const char* name, Vec3 start, f32 yaw, f32 seconds, Player& pl,
              const std::function<bool(const Player&, u32, Drive&)>& drive) {
        Tally t;
        FollowCamera cam(w_, cp_);
        if (!pl.place(start, yaw))
            log("selftest walk %s: no ground at the start (%.1f, %.1f, %.1f)", name, start.x, start.y, start.z);
        cam.reset(pl.position(), pl.yaw());
        const u32 ticks = (u32)(seconds * 60.0f);
        Vec3 prev = pl.position();
        auto solid = [&pl](u16 a, Vec3 n) { return pl.solid(a, n); };
        for (u32 i = 0; i < ticks; i++) {
            Drive d;
            if (!drive(pl, i, d))
                break;
            PlayerInput in;
            if (d.world)
                in.move_x = d.wx, in.move_z = d.wz;
            else
                cam.stick_to_world(d.sx, d.sy, &in.move_x, &in.move_z);
            in.run = d.run;
            in.jump = d.jump;
            in.jump_held = d.jump_held;
            w_.reset_stats();
            pl.update(in, TICK);
            cam.update(d.cam, pl.position(), pl.yaw(), pl.speed() / pp_.run_speed, TICK);
            const Stats& st = w_.stats();
            u32 tests = st.triangle_tests + st.cylinder_tests;
            t.tests += tests, t.queries += st.grounds + st.casts + st.push_outs;
            // the checks (not counted in the stats above)
            Vec3 p = pl.position();
            if (tests > t.max_tests)
                t.max_tests = tests, t.max_at = p, t.max_state = (int)pl.state();
            t.ticks++;
            t.min_y = std::min(t.min_y, p.y), t.max_y = std::max(t.max_y, p.y);
            if (pl.swimming()) {
                // at a water surface within its height over the feet, and no land over that (the
                // sea's plane goes on under the island)
                t.swim++;
                t.air = 0;
                Hit h;
                if (!w_.ground(p.x, p.z, p.y + pp_.height, p.y, water_of, &h))
                    t.swim_dry++;
                else if (w_.ground(p.x, p.z, h.point.y + 2 * pp_.height, h.point.y + 0.01f, floor_of, &h))
                    t.under_land++;
            } else if (pl.airborne()) {
                t.fell = true;
                t.max_air = std::max(t.max_air, ++t.air);
                Hit h;
                if (w_.ground(p.x, p.z, p.y + 0.01f, -1e9f, floor_of, &h) && h.normal.y >= pp_.wall_ny &&
                    h.normal.y < pp_.walkable_ny)
                    t.max_over_slope = std::max(t.max_over_slope, p.y - h.point.y);
            } else {
                t.air = 0;
                Hit h;
                if (!w_.ground(p.x, p.z, p.y + 0.01f, p.y - 0.01f, floor_of, &h))
                    t.off_ground++;
            }
            f32 o = pl.overlap(p);
            t.max_overlap = std::max(t.max_overlap, o);
            if (o > 0.05f * pp_.radius)
                t.in_wall++;
            t.max_depth = std::max(t.max_depth, pl.water_depth());
            if (!pl.swimming() && pl.water_depth() > pp_.wade_depth + 1e-3f)
                t.deep++;
            Hit h;
            if (w_.cast(prev + Vec3{0, pp_.knee, 0}, p + Vec3{0, pp_.knee, 0}, solid, &h))
                t.tunnel++;
            const engine::g3d::Camera& c = cam.camera();
            if (w_.cast(cam.pivot(), c.eye, FollowCamera::blocks_view, &h))  // (the head: what it orbits)
                t.cam_blocked++;
            t.cam_pulled += cam.distance() < 0.98f * cp_.distance;
            t.cam_min = std::min(t.cam_min, cam.distance());
            if (w_.ground(c.eye.x, c.eye.z, c.eye.y + 50.0f, c.eye.y, water_of, &h))
                t.cam_wet++;
            prev = p;
            if (out_ && i % 6 == 0)
                fprintf(out_, "%s%s,%.2f,%.2f,%.2f,%.2f,%d,%.2f,%.2f,%.2f\n", tag_, name, i / 60.0f, p.x, p.y, p.z,
                        (int)pl.state(), c.eye.x, c.eye.y, c.eye.z);
        }
        return t;
    }

    void report(const char* name, const Player& pl, const Tally& t, bool ok, const char* what) {
        Vec3 p = pl.position();
        log("selftest walk %s%s: %s (%s) at (%.1f, %.1f, %.1f), walked %.0f, ground %04X, depth %.2f, y %.1f..%.1f, "
            "%s%u ticks (%u swimming): off the ground %u, in a wall %u (worst %.3f), deep water %u, swimming out of "
            "water %u, under land %u, through a wall %u, camera "
            "blocked %u, under water %u, pulled in %.0f %% (to %.1f of %.1f); %.0f triangle tests a tick (%u at most, "
            "at (%.0f, %.0f, %.0f) state %d)",
            tag_, name, ok && !t.broken() ? "ok" : "MISMATCH", what, p.x, p.y, p.z, pl.distance(),
            pl.ground_attribute(),
            pl.water_depth(), t.min_y, t.max_y, t.fell ? "fell, " : "", t.ticks, t.swim, t.off_ground, t.in_wall,
            t.max_overlap, t.deep, t.swim_dry, t.under_land, t.tunnel, t.cam_blocked, t.cam_wet, t.ticks ? 100.0 * t.cam_pulled / t.ticks : 0.0,
            t.cam_min, cp_.distance, t.ticks ? (double)t.tests / t.ticks : 0.0,
            t.max_tests, t.max_at.x, t.max_at.y, t.max_at.z, t.max_state);
    }

    const World& w_;
    PlayerParams pp_;
    FollowCameraParams cp_;
    FILE* out_;
    const char* tag_;
};

// steer toward a point (a world direction), at a tilt
bool toward(const Player& pl, f32 x, f32 z, f32 tilt, Drive& d) {
    f32 dx = x - pl.position().x, dz = z - pl.position().z, l = std::sqrt(dx * dx + dz * dz);
    d.world = true;
    if (l > 1e-3f)
        d.wx = dx / l * tilt, d.wz = dz / l * tilt;
    return true;
}


// the scripted walks, for a Mii at k (tag: a prefix for the walks' names); trail: x, z, y waypoints
void scripted(const World& w, f32 k, const char* tag, const std::vector<Vec3>& trail, FILE* out) {
    const PlayerParams pp = PlayerParams::make(k);
    log("selftest walk: k %.3f: the Mii %.2f tall, walks %.3f and runs %.3f a frame (%.1f and %.1f a second), "
        "radius %.2f, step %.2f, wading %.2f, drops up to %.2f, slopes up to %.1f degrees",
        k, pp.height, pp.walk_speed, pp.run_speed, pp.walk_speed * 60, pp.run_speed * 60, pp.radius, pp.step_up,
        pp.wade_depth, pp.max_drop, std::acos(pp.walkable_ny) * 180 / PI);
    Walker wk(w, pp, out, tag);
    const Vec3 spawn{-2029, 69, 2330};

    // A pressed and let go at once: the rise under twice gravity, half jump_height; another press in
    // the air does nothing; held: under 0.72 of it, jump_height / 0.72 (player.cpp JUMP_CUT, JUMP_HELD)
    for (int held = 0; held < 2; held++) {
        Player pl(w, pp);
        f32 ground_y = 0;
        const char* name = held ? "jump_held" : "jump";
        Tally t = wk.run(name, spawn, 180, 2, pl, [&](const Player& p, u32 i, Drive& d) {
            if (i == 0)
                ground_y = p.position().y;
            d.jump = i == 0 || i == 20;
            d.jump_held = held && i < 90;
            return true;
        });
        const f32 want = held ? pp.jump_height / 0.72f : 0.5f * pp.jump_height, rise = t.max_y - ground_y;
        const bool landed = !pl.airborne() && std::fabs(pl.position().y - ground_y) < 0.1f;
        char what[128];
        snprintf(what, sizeof(what), "rose %.2f (%.2f wanted), one jump, landed", rise, want);
        wk.report(name, pl, t, std::fabs(rise - want) < 0.1f * want && landed, what);
    }

    {  // running momentum carries through an intentional jump
        Player pl(w, pp);
        Vec3 takeoff{};
        bool took_off = false, was_airborne = false;
        Tally t = wk.run("jump_momentum", spawn, 180, 3, pl, [&](const Player& p, u32 i, Drive& d) {
            if (i == 30)
                takeoff = p.position(), d.jump = true, took_off = true;
            else if (was_airborne && !p.airborne())
                return false;
            was_airborne |= p.airborne();
            d.world = true, d.wz = -1.0f, d.run = true;
            return true;
        });
        const Vec3 landed = pl.position();
        const f32 jump_distance = std::sqrt((landed.x - takeoff.x) * (landed.x - takeoff.x) +
                                            (landed.z - takeoff.z) * (landed.z - takeoff.z));
        wk.report("jump_momentum", pl, t,
                  took_off && was_airborne && !pl.airborne() && jump_distance > pp.run_speed * 30.0f,
                  "kept running momentum through the jump");
    }

    {  // north into the fountain: up its rim (y 79), and the basin's water has no floor
        Player pl(w, pp);
        Tally t = wk.run("fountain", spawn, 180, 20, pl, [](const Player&, u32, Drive& d) {
            d.world = true, d.wz = -0.5f;
            return true;
        });
        Vec3 p = pl.position();
        wk.report("fountain", pl, t, p.z > 2180 && p.z < 2240 && pl.water_depth() == 0 && !pl.airborne(),
                  "stopped at the basin");
    }
    {  // north into a building's wall west of the plaza (z 2417.5), over flat paving (y 72)
        Player pl(w, pp);
        Tally t = wk.run("wall", Vec3{-2400, 72, 2395}, 0, 10, pl, [](const Player&, u32, Drive& d) {
            d.world = true, d.wz = 1.0f;
            return true;
        });
        Vec3 knee = pl.position() + Vec3{0, pp.knee, 0};
        Hit h;
        bool hit = w.cast(knee, knee + Vec3{0, 0, 3 * pp.radius}, &h);
        f32 gap = hit ? h.t * 3 * pp.radius : -1;
        char what[96];
        snprintf(what, sizeof(what), "the wall %.3f from the knee, radius %.3f", gap, pp.radius);
        wk.report("wall", pl, t, hit && std::fabs(gap - pp.radius) < 0.1f * pp.radius, what);
    }
    if (trail.size() >= 2) {  // up the volcano's dirt trail, running
        Player pl(w, pp);
        const u32 n = (u32)trail.size();
        u32 next = 1, since = 0, worst = 0;
        bool top = false;
        Tally t = wk.run("trail", Vec3{trail[0].x, trail[0].y, trail[0].z}, 270, 1800, pl,
                         [&](const Player& p, u32, Drive& d) {
                             f32 dx = trail[next].x - p.position().x, dz = trail[next].z - p.position().z;
                             if (next + 1 < n && dx * dx + dz * dz < 30 * 30) {
                                 next++, since = 0;
                             } else if (next + 1 == n && dx * dx + dz * dz < 10 * 10) {
                                 top = true;
                                 return false;
                             }
                             worst = std::max(worst, ++since);
                             if (since > 60 * 60)
                                 return false;  // stuck
                             d.run = true;
                             return toward(p, trail[next].x, trail[next].z, 1.0f, d);
                         });
        char what[96];
        snprintf(what, sizeof(what), "waypoint %u of %u%s, %.1f s at most between two", next + 1, n,
                 top ? " (the end)" : "", worst / 60.0f);
        wk.report("trail", pl, t, top && std::fabs(pl.position().y - trail[n - 1].y) < 5, what);
    }
    {  // a rock shore on the volcano's north coast whose bed goes gently under the sea (1 deep 48
       // units out, 6 deep 96 out): wades in, then swims once out of its depth; turned back, it swims
       // to the shore and wades out
        Player pl(w, pp);
        const Vec3 from{3798.1f, 10, -6644.4f};
        bool swam = false, out = false;
        Tally t = wk.run("shore", from, 80, 40, pl, [&](const Player& p, u32 i, Drive& d) {
            swam |= p.swimming();
            if (i < 20 * 60) {
                d.world = true, d.wx = std::sin(80 * PI / 180), d.wz = std::cos(80 * PI / 180);
                return true;
            }
            if (!p.swimming() && p.water_depth() <= pp.wade_depth && swam)
                out = true;
            return toward(p, from.x, from.z, 1.0f, d);
        });
        wk.report("shore", pl, t, swam && out && !pl.swimming(), "swam out, came back and waded out");
    }
    {  // into a tree's trunk (a cylinder 8.2 wide, on the grass by the trail's foot)
        Player pl(w, pp);
        const f32 tx = 1134.7f, tz = 405.0f;
        Tally t = wk.run("tree", Vec3{tx + 40, 160, tz}, -90, 10, pl, [&](const Player& p, u32, Drive& d) {
            return toward(p, tx, tz, 0.5f, d);
        });
        f32 dx = pl.position().x - tx, dz = pl.position().z - tz, gap = std::sqrt(dx * dx + dz * dz);
        char what[96];
        snprintf(what, sizeof(what), "%.3f from the trunk's axis, radius %.3f + 8.2", gap, pp.radius);
        wk.report("tree", pl, t, w.cylinders() == 0 || std::fabs(gap - pp.radius - 8.2f) < 0.1f * pp.radius, what);
    }
    {  // Powder White Beach toward the sea: the water there has no floor, the sand's edge stops it
        Player pl(w, pp);
        Tally t = wk.run("beach", Vec3{6001, 100, 757}, 120, 100, pl, [](const Player&, u32, Drive& d) {
            d.world = true, d.wx = std::sin(120 * PI / 180), d.wz = std::cos(120 * PI / 180);
            d.run = true;
            return true;
        });
        wk.report("beach", pl, t,
                  pl.swimming() || (pl.water_depth() <= pp.wade_depth && pl.speed() < 0.1f * pp.walk_speed),
                  pl.swimming() ? "swimming in the sea" : "stopped at the sea");
    }
    {  // to the edge of the grass ledge north of the plaza (y 169), whose rock face drops 100 to the
       // plaza (69): deeper than max_drop, so refused as a wall would be
        Player pl(w, pp);
        Tally t = wk.run("cliff", Vec3{-2487.5f, 169, 1560}, 0, 12, pl, [](const Player&, u32, Drive& d) {
            d.world = true, d.wz = 0.5f;
            return true;
        });
        wk.report("cliff", pl, t,
                  !t.fell && std::fabs(pl.position().y - 169) < 1 && pl.speed() < 0.1f * pp.walk_speed,
                  "stopped at the edge of the 100 drop");
    }
    {  // east off the grass south of it (y 169) onto the lawn below (145.4): a drop of 24, within
       // max_drop, so it falls and lands
        Player pl(w, pp);
        const f32 lx = -2037.5f, lz = 1412.5f;  // the lawn
        bool there = false;
        Tally t = wk.run("ledge", Vec3{-2112.5f, 169, 1412.5f}, 90, 15, pl, [&](const Player& p, u32, Drive& d) {
            f32 dx = lx - p.position().x, dz = lz - p.position().z;
            if (dx * dx + dz * dz < 3 * 3 && !p.airborne()) {
                there = true;
                return false;
            }
            return toward(p, lx, lz, 0.5f, d);
        });
        wk.report("ledge", pl, t, there && t.fell && std::fabs(pl.position().y - 145.4f) < 1, "fell 24 and landed");
    }
    {  // east coast: from a grass top (y 238) toward its rock slope (64 degrees: too steep to stand on),
       // which ends on the sand 178 lower: deeper than max_drop, so it isn't stepped onto
        Player pl(w, pp);
        const f32 yaw = 12.2f;  // the slope's fall line
        Tally t = wk.run("slope", Vec3{9144.5f, 239, 5575.6f}, yaw, 10, pl, [&](const Player&, u32, Drive& d) {
            d.world = true, d.wx = 0.5f * std::sin(yaw * PI / 180), d.wz = 0.5f * std::cos(yaw * PI / 180);
            return true;
        });
        wk.report("slope", pl, t, !t.fell && pl.position().y > 230 && pl.speed() < 0.1f * pp.walk_speed,
                  "stopped at the top of the 178 slide");
        // put on the slope, a third of the way down (where nothing refuses it: a Mii warped onto steep
        // ground slides down whatever the depth), it slides down, keeping to the slope, onto the sand
        Vec3 on{0, 0, 0};
        Hit h;
        for (f32 dist = 0; dist < 100 && on.y == 0; dist += 1) {
            f32 x = 9144.5f + dist * std::sin(yaw * PI / 180), z = 5575.6f + dist * std::cos(yaw * PI / 180);
            if (w.ground(x, z, 1e9f, -1e9f, floor_of, &h) && h.normal.y < pp.walkable_ny && h.point.y < 180)
                on = h.point;
        }
        Player sl(w, pp);
        bool slid = false;
        Tally ts = wk.run("slide", on, yaw, 15, sl, [&](const Player& p, u32, Drive&) {
            slid |= p.airborne();
            return !(slid && !p.airborne());  // (until down)
        });
        char what[96];
        snprintf(what, sizeof(what), "slid down from y %.1f and landed, at most %.2f over the slope", on.y,
                 ts.max_over_slope);
        wk.report("slide", sl, ts,
                  on.y > 0 && ts.fell && !sl.airborne() && std::fabs(sl.position().y - 60) < 1 &&
                      ts.max_over_slope < 0.1f * pp.height,
                  what);
    }
    {  // across the suspension bridge (deck about y 699..766), east to west
        Player pl(w, pp);
        Tally t = wk.run("bridge", Vec3{-680, 760, -1454}, -90, 120, pl, [](const Player& p, u32, Drive& d) {
            if (p.position().x < -2000)
                return false;
            return toward(p, -2100, -1454, 1.0f, d);
        });
        wk.report("bridge", pl, t, pl.position().x < -1990 && t.min_y > 690 && !t.fell, "crossed");
    }
    // random walking, the camera turned now and then: ten minutes from the spawn, five from the
    // trail's foot (grass, the river, the forest), five on Powder White Beach and five from the rock
    // shore (into the sea and about in it)
    struct Start {
        const char* name;
        Vec3 at;
        f32 seconds;
    };
    for (const Start& st : {Start{"random", spawn, 600}, Start{"random_hills", Vec3{1356, 120, 456}, 300},
                            Start{"random_beach", Vec3{6001, 100, 757}, 300},
                            Start{"random_shore", Vec3{3798.1f, 10, -6644.4f}, 300}}) {
        Player pl(w, pp);
        Rng rng{std::hash<std::string>()(std::string(tag) + st.name) | 1};
        f32 sx = 0, sy = 0, cx = 0, cy = 0;
        bool run = false;
        u32 change = 0, cam_change = 0;
        Vec3 lo{1e30f, 0, 1e30f}, hi{-1e30f, 0, -1e30f};
        double t0 = now_s();
        Tally t = wk.run(st.name, st.at, 180, st.seconds, pl, [&](const Player& p, u32 i, Drive& d) {
            if (i >= change) {
                f32 a = rng.uniform() * 2 * PI, tilt = rng.uniform() < 0.15f ? 0 : 0.3f + 0.7f * rng.uniform();
                sx = std::sin(a) * tilt, sy = std::cos(a) * tilt;
                run = rng.uniform() < 0.2f;
                change = i + 60 + rng.next() % 180;
            }
            if (i >= cam_change) {
                bool turn = rng.uniform() < 0.3f;
                cx = turn ? rng.uniform() * 2 - 1 : 0, cy = turn ? rng.uniform() * 2 - 1 : 0;
                cam_change = i + 30 + rng.next() % 240;
            }
            d.sx = sx, d.sy = sy, d.run = run, d.cam.orbit_x = cx, d.cam.orbit_y = cy;
            Vec3 q = p.position();
            lo = {std::min(lo.x, q.x), 0, std::min(lo.z, q.z)}, hi = {std::max(hi.x, q.x), 0, std::max(hi.z, q.z)};
            return true;
        });
        double secs = now_s() - t0;
        char what[160];
        snprintf(what, sizeof(what), "over x %.0f..%.0f z %.0f..%.0f, %.1f queries a tick, %.1f us a tick", lo.x, hi.x,
                 lo.z, hi.z, (double)t.queries / t.ticks, secs * 1e6 / t.ticks);
        wk.report(st.name, pl, t, true, what);
    }
}

// ---- query timing

void timing(const World& w, f32 k) {
    const PlayerParams pp = PlayerParams::make(k);
    Vec3 lo, hi;
    w.bounds(&lo, &hi);
    // points on the land: the ground under random x/z in the main island's box
    Rng rng{42};
    std::vector<Vec3> pts;
    while (pts.size() < 20000) {
        f32 x = -12000 + 23000 * rng.uniform(), z = -15000 + 29000 * rng.uniform();
        Hit h;
        if (w.ground(x, z, 1e9f, 1.0f, floor_of, &h))
            pts.push_back(h.point);
    }
    auto bench = [&](const char* what, const std::function<void(const Vec3&)>& q) {
        w.reset_stats();
        double t0 = now_s();
        const int reps = 5;
        for (int r = 0; r < reps; r++)
            for (const Vec3& p : pts)
                q(p);
        double ns = (now_s() - t0) * 1e9 / (reps * pts.size());
        const Stats& s = w.stats();
        log("selftest walk: %s: %.0f ns a query, %.1f triangles looked at a query", what, ns,
            (double)s.triangle_tests / (reps * pts.size()));
    };
    volatile f32 sink = 0;
    bench("ground (a step's window)", [&](const Vec3& p) {
        Hit h;
        if (w.ground(p.x + 0.7f, p.z, p.y + pp.step_up, p.y - pp.snap_down, floor_of, &h))
            sink = sink + h.point.y;
    });
    bench("ground (from the sky)", [&](const Vec3& p) {
        Hit h;
        if (w.ground(p.x + 0.7f, p.z, 1e9f, -1e9f, &h))
            sink = sink + h.point.y;
    });
    Player probe(w, pp);
    bench("push_out (knee sphere)", [&](const Vec3& p) {
        Vec3 v = w.push_out(p + Vec3{0, pp.knee, 0}, pp.radius, [&](u16 a, Vec3 n) { return probe.solid(a, n); });
        sink = sink + v.x;
    });
    const FollowCameraParams cp = FollowCameraParams::make(pp.height);
    u32 i = 0;
    bench("cast (camera line)", [&](const Vec3& p) {
        f32 a = (f32)(i++ % 360) * PI / 180;
        Vec3 head = p + Vec3{0, cp.head, 0};
        Vec3 eye = head + Vec3{-std::sin(a) * 0.966f, 0.259f, -std::cos(a) * 0.966f} * cp.distance;
        Hit h;
        if (w.cast(head, eye, FollowCamera::blocks_view, &h))
            sink = sink + h.t;
    });
}

}  // namespace

// NOMA_SELFTEST=input: NOMA_INPUT's stick, camera stick, touch and 3DS-button tokens (and the old ones
// beside them), read at game time 0 (before the game starts, so every event here starts at 0)
void selftest_input() {
    setenv("NOMA_INPUT",
           "0:A+[L][ZR]:1,0:[X][SELECT],0:S(0.5;-1):0,0:S(0.25;0.75),0:C(1;-0.5):2,5:B,0:T(80;60;240;180):1,"
           "0:P(0.2;-0.1),0:2,3:T(10;10)",
           1);
    WiimoteInput in;
    in.buttons = in.pad = 0;
    in.stick_x = in.stick_y = in.cam_x = in.cam_y = 9;  // (left alone by the script: it must set them)
    input_script_apply(in);
    // A, + and 2 held, X (= 1) and SELECT (= -) (B starts at 5 s), L and ZR, the second S (the first,
    // at the same time, is over at once: 0 seconds), the camera stick held for 2 s, the drag's touch at
    // its start (the tap at 3 s not yet), the pointer moved by P (after the touch in the script)
    bool ok = in.buttons == (WM_A | WM_PLUS | WM_TWO | WM_ONE | WM_MINUS) && in.pad == (PAD_L | PAD_ZR) &&
              in.stick_x == 0.25f && in.stick_y == 0.75f && in.cam_x == 1.0f && in.cam_y == -0.5f &&
              in.touching && in.touch_x == 80 && in.touch_y == 60 && in.x == 0.2f && in.y == -0.1f;
    log("selftest input: buttons %04X pad %X stick (%g, %g) camera (%g, %g) touch %s(%u, %u) pointer (%g, %g): %s",
        in.buttons, in.pad, in.stick_x, in.stick_y, in.cam_x, in.cam_y, in.touching ? "" : "off ", in.touch_x,
        in.touch_y, in.x, in.y, ok ? "ok" : "MISMATCH");
}

// NOMA_SELFTEST=input, the game's reads (KPADReadEx) against the platform's input set every retrace:
// a press and its release between two reads seen held at one read and let go at the next; a tap
// (touch, let go) as A once the pointer has been on the screen for 11 reads, held for one read; no
// tap in stick mode
void selftest_kpad() {
    const u32 st = 0x81600000;  // (a KPADStatus in guest memory nothing else uses here)
    g::KPADInitEx(0, 0);
    auto read = [&]() {
        g::KPADReadEx(0, st, 1, 0);
        return std::pair<u32, u32>(guest::load32(st + 0x00), guest::load32(st + 0x04));  // hold, trigger
    };
    WiimoteInput in;
    in.buttons = 0, in.pointing = true;
    bool ok = true;
    // a B press and release between two reads
    wiimote_set_input(in);
    read();
    in.buttons = WM_B, wiimote_set_input(in);
    in.buttons = 0, wiimote_set_input(in);
    auto r1 = read(), r2 = read();
    ok &= r1.first == WM_B && r1.second == WM_B && r2.first == 0;
    // a tap: the A press waits for the pointer's 11th read on the screen, then lasts one read
    for (int i = 0; i < 20; i++)
        read();  // (the pointer has been on the screen for a while)
    in.touching = true, in.touch_x = 100, in.touch_y = 50, wiimote_set_input(in);
    read();
    in.touching = false, wiimote_set_input(in);
    auto t1 = read(), t2 = read();
    ok &= t1.first == WM_A && t1.second == WM_A && t2.first == 0;
    // a tap just after the pointer came back: A at its 11th read
    in.pointing = false, wiimote_set_input(in), read();
    in.pointing = true, in.touching = true, wiimote_set_input(in), read();
    in.touching = false, wiimote_set_input(in);
    int at = 0;
    for (int i = 2; i <= 14 && !at; i++)
        if (read().second == WM_A)
            at = i;
    ok &= at == 11;
    // stick mode: a tap is no press
    input_set_stick_mode(true);
    in.touching = true, wiimote_set_input(in), read();
    in.touching = false, wiimote_set_input(in);
    bool none = true;
    for (int i = 0; i < 14; i++)
        none &= read().first == 0;
    input_set_stick_mode(false);
    ok &= none;
    log("selftest input: KPAD reads: press between reads hold %X/%X, tap A %X then %X, a tap at read %d, stick mode %s: %s",
        r1.first, r2.first, t1.first, t2.first, at, none ? "no press" : "PRESSED", ok ? "ok" : "MISMATCH");
}

void selftest_walk() {
    const char* dir = getenv("NOMA_KCL_DIR");
    if (!dir) {
        log("selftest walk: NOMA_KCL_DIR not set (the folder with WS2_common_island1..5.kcl): skipped");
        return;
    }
    World w;
    u32 bytes = 0;
    double t0 = now_s();
    for (int i = 1; i <= 5; i++) {
        std::string name = "WS2_common_island" + std::to_string(i) + ".kcl";
        std::vector<u8> d = slurp(std::string(dir) + "/" + name);
        bytes += (u32)d.size();
        if (d.empty() || !w.add_kcl(d.data(), (u32)d.size(), name.c_str())) {
            log("selftest walk: %s: can't read it: MISMATCH", name.c_str());
            return;
        }
    }
    double t1 = now_s();
    u32 trunks = 0;
    if (const char* tf = getenv("NOMA_KCL_TRUNKS")) {
        if (FILE* f = fopen(tf, "r")) {
            Cylinder c;
            while (fscanf(f, "%f,%f,%f,%f,%f", &c.x, &c.z, &c.y0, &c.y1, &c.radius) == 5)
                w.add_cylinder(c), trunks++;
            fclose(f);
        }
    }
    w.finish();
    double t2 = now_s();
    Vec3 lo, hi;
    w.bounds(&lo, &hi);
    u32 columns, entries, most;
    w.grid_info(&columns, &entries, &most);
    log("selftest walk: %u KCL bytes: %u triangles, %u trunks, %u bytes in memory (grids: %u columns, %u entries, "
        "%u at most in one); rebuilt in %.1f ms, grids in %.1f ms; bounds (%.0f, %.0f, %.0f)..(%.0f, %.0f, %.0f)",
        bytes, w.triangles(), trunks, w.memory_bytes(), columns, entries, most, (t1 - t0) * 1e3, (t2 - t1) * 1e3,
        lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);

    if (const char* g = getenv("NOMA_KCL_GROUND"))
        check_rasters(w, g);
    const f32 k = getenv("NOMA_WALK_K") ? (f32)atof(getenv("NOMA_WALK_K")) : 10.0f;
    const f32 k2 = getenv("NOMA_WALK_K2") ? (f32)atof(getenv("NOMA_WALK_K2")) : 9.0f;
    FILE* out = nullptr;
    if (const char* o = getenv("NOMA_WALK_OUT")) {
        out = fopen((std::string(o) + "/paths.csv").c_str(), "w");
        if (out)
            fprintf(out, "walk,t,x,y,z,state,eye_x,eye_y,eye_z\n");
    }
    std::vector<Vec3> trail;
    if (const char* tf = getenv("NOMA_WALK_TRAIL")) {
        if (FILE* f = fopen(tf, "r")) {
            Vec3 v;
            while (fscanf(f, "%f,%f,%f", &v.x, &v.z, &v.y) == 3)
                trail.push_back(v);
            fclose(f);
        }
    }
    if (trail.size() < 2)
        log("selftest walk: no trail waypoints (NOMA_WALK_TRAIL): the trail walk is skipped");
    scripted(w, k, "", trail, out);
    scripted(w, k2, "k2_", trail, out);
    if (out)
        fclose(out);
    timing(w, k);
}

}  // namespace guest
