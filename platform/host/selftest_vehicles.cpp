// NOMA_SELFTEST=vehicles: the island's cars and boats (engine/wuhu/vehicles.h) on the staged package
// (NOMA_G3D_PACKAGE, default wuhu/island.ng3d in the working directory: run it from runs/data) and its
// collision. Every parked car and anchored boat is taken in turn and driven: ahead at full throttle,
// ahead steering right, then back; every tick checks that a car stands on drivable ground under its
// front, back and sides (within its step) and a boat floats on deep water, that it overlaps no other
// vehicle it didn't already, and where it ended. Then a way out beside it: the player stands (or, by
// a boat, swims) at one of its exits.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "runtime.h"
#include "engine/collision/kcl.h"
#include "engine/g3d/package.h"
#include "engine/g3d/world.h"
#include "engine/wuhu/player.h"
#include "engine/wuhu/vehicles.h"

namespace guest {

namespace {

namespace fmt = engine::g3d::fmt;
using engine::collision::Hit;
using engine::math::Vec3;
using engine::wuhu::Obstacle;
using engine::wuhu::Player;
using engine::wuhu::PlayerParams;
using engine::wuhu::Vehicles;

bool floor_of(u16 a, Vec3 n) { return n.y > 0.05f && !engine::collision::kcl_is_water(a) && !engine::collision::kcl_is_fence(a); }
bool water_of(u16 a, Vec3 n) { return n.y > 0.05f && engine::collision::kcl_is_water(a); }

}  // namespace

void selftest_vehicles() {
    const char* path = getenv("NOMA_G3D_PACKAGE") ? getenv("NOMA_G3D_PACKAGE") : "wuhu/island.ng3d";
    FILE* f = fopen(path, "rb");
    if (!f) {
        log("selftest vehicles: no %s (NOMA_G3D_PACKAGE): skipped", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void* block = malloc((size_t)n);
    const bool read_ok = fread(block, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    std::string error;
    std::unique_ptr<engine::g3d::Package> pkg(read_ok ? engine::g3d::Package::load(block, (u32)n, &error) : nullptr);
    if (!pkg) {
        log("selftest vehicles: %s: %s: MISMATCH", path, error.c_str());
        return;
    }
    engine::collision::World col;
    for (const fmt::Collision& c : pkg->collisions())
        col.add_kcl(pkg->at(c.data), c.size, pkg->str(c.name));
    for (const fmt::Cylinder& c : pkg->cylinders())
        col.add_cylinder({c.x, c.z, c.y0, c.y1, c.radius});
    col.finish();
    engine::g3d::World world(*pkg);
    Vehicles veh(world, *pkg, col);
    // the player as the island makes it (island.cpp: MII_SCALE 1.36, a man's gait)
    const f32 k = pkg->env().mii_units_per_unit / 1.36f;
    PlayerParams pp = PlayerParams::make(k);
    Player pl(col, pp);
    log("selftest vehicles: %s: %u vehicles", path, veh.count());

    u32 cars = 0, boats = 0, moved = 0, stuck = 0, bad = 0, out_ok = 0;
    for (u32 v = 0; v < veh.count(); v++) {
        veh.enter(v);
        const bool boat = veh.boat();
        (boat ? boats : cars)++;
        const Vec3 start = veh.position();
        u32 off = 0, hits = 0;
        const f32 dt = 1.0f / 60.0f;
        for (u32 i = 0; i < 6 * 60; i++) {
            const f32 throttle = i < 4 * 60 ? 1.0f : -1.0f, steer = i >= 2 * 60 && i < 4 * 60 ? 0.7f : 0.0f;
            std::vector<Obstacle> before = veh.obstacles(v);
            const Obstacle was = veh.box(v);
            veh.drive(throttle, steer, dt, i * dt);
            const Obstacle now = veh.box(v);
            for (const Obstacle& o : before)
                if (engine::wuhu::boxes_overlap(now, o) && !engine::wuhu::boxes_overlap(was, o))
                    hits++;
            const Vec3 c = now.centre;
            Hit h;
            if (boat) {
                // the surface under its middle, about its draft over its bottom; no land over it
                if (!col.ground(c.x, c.z, now.top, now.bottom - 10.0f, water_of, &h) ||
                    std::fabs(h.point.y - now.bottom) > 0.3f * (now.top - now.bottom) ||
                    col.ground(c.x, c.z, h.point.y + 50.0f, h.point.y - 1.0f, floor_of, &h))
                    off++;
            } else if (!col.ground(c.x, c.z, now.bottom + 0.3f * (now.top - now.bottom), now.bottom - 0.3f * (now.top - now.bottom),
                                   floor_of, &h)) {
                off++;
            }
        }
        const Vec3 end = veh.position();
        const f32 dist = std::sqrt((end.x - start.x) * (end.x - start.x) + (end.z - start.z) * (end.z - start.z));
        (dist > 20.0f ? moved : stuck)++;
        // out beside it
        bool out = false;
        pl.set_obstacles(veh.obstacles());
        for (const Vec3& at : veh.exits(pp.radius + 0.1f * pp.height)) {
            Vec3 feet;
            if ((boat && pl.place_swimming(at, veh.yaw())) || pl.can_stand(Vec3{at.x, at.y + pp.step_up, at.z}, at.y - pp.max_drop, &feet)) {
                out = true;
                break;
            }
        }
        if (!out && getenv("NOMA_VEH_DEBUG")) {
            const Obstacle o = veh.box(v);
            for (const Vec3& at : veh.exits(pp.radius + 0.1f * pp.height)) {
                Hit w, g;
                const bool water = col.ground(at.x, at.z, at.y + pp.height, at.y - 2 * pp.height, water_of, &w);
                const bool ground = water && col.ground(at.x, at.z, std::max(at.y + pp.height, w.point.y), w.point.y - pp.wade_depth, floor_of, &g);
                log("  exit (%.1f, %.1f, %.1f): box %.1f..%.1f, water %d at %.1f, ground over %d at %.1f, overlap %.3f", at.x, at.y, at.z,
                    o.bottom, o.top, water, water ? w.point.y : 0, ground, ground ? g.point.y : 0,
                    pl.overlap(Vec3{at.x, water ? w.point.y - 0.62f * pp.height : at.y, at.z}));
            }
        }
        out_ok += out;
        if (off || hits)
            bad++;
        if (off || hits || !out || dist <= 20.0f)
            log("selftest vehicles: %s %u (instance at (%.0f, %.0f, %.0f)): moved %.1f, %u ticks off its %s, %u new "
                "overlaps, %s", boat ? "boat" : "car", v, start.x, start.y, start.z, dist, off, boat ? "water" : "ground",
                hits, out ? "a way out" : "NO WAY OUT");
        veh.leave();
    }
    log("selftest vehicles: %u cars, %u boats: %u moved, %u held where they stood (walls, other vehicles, shores), %u "
        "broke a rule, %u with a way out: %s",
        cars, boats, moved, stuck, bad, out_ok, bad ? "MISMATCH" : "ok");
}

}  // namespace guest
