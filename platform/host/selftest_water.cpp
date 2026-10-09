// NOMA_SELFTEST=water: the player's ways into the water and out of it (engine/wuhu/player.h
// water_entry, water_exit: what the island's splashes and the Mii's plunge and climb out are made
// from), on Wii Sports Resort's own collision files from your extraction, as NOMA_SELFTEST=walk takes
// them (NOMA_KCL_DIR, the folder with WS2_common_island1..5.kcl; NOMA_KCL_TRUNKS, the trunks; without
// NOMA_KCL_DIR the test is skipped). For a Mii at k 10 and at the island's k (7.353: the package's
// over the size the Mii is drawn at), with the island's pace (engine/wuhu/island.cpp).
//
// Every update of every walk checks the latches against the player: an entry exactly when the update
// began out of the water and ended in it (never from a placing), at the swimmer's feet under the
// water's surface; an exit exactly when it began in the water and ended on the ground, its climb the
// feet's rise. The walks:
//   shore      the rock shore of NOMA_SELFTEST=walk out to sea and back: in walking (WADE, no
//              vertical speed), out wading (the ground under the water's surface)
//   beach      パウダー・ホワイト・ビーチ toward the sea at a run: in off the sand's edge, falling or
//              walking
//   sea_front  the plaza's west sea front (72 over the sea), a running jump off it at its edge: in as
//              a jump, falling faster than a fall from the deepest drop the Mii may take
//   leap       the same front at a sprint: off it as a leap (LEAP, or GLIDE once a Mii height down)
//   pier       from the water south of the town's east pier (5.56 over the sea), swum to: out onto
//              it, its climb the pier's height over the swimmer's feet
//   random     five minutes of random walking, running, jumping and swimming from the rock shore
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "runtime.h"
#include "engine/collision/kcl.h"
#include "engine/wuhu/player.h"

namespace guest {

namespace {

using namespace engine::collision;
using engine::wuhu::Player;
using engine::wuhu::PlayerInput;
using engine::wuhu::PlayerParams;
using engine::wuhu::WaterCause;
using engine::wuhu::WaterEntry;
using engine::wuhu::WaterExit;

constexpr f32 PI = 3.14159265f;
constexpr f32 TICK = 1.0f / 60.0f;
// the island's Mii (engine/wuhu/island.cpp): drawn 1.36 times WSR's size, at its pace
constexpr f32 ISLAND_K = 10.0f / 1.36f, PACE = 2.0f * 25.0f / 60.0f / 1.36f, RUN_MULTIPLIER = 2.5f;

const char* const CAUSES[] = {"fall", "jump", "leap", "glide", "slide", "wade"};

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

struct Rng {
    u64 s;
    u32 next() {
        s ^= s >> 12, s ^= s << 25, s ^= s >> 27;
        return (u32)((s * 0x2545F4914F6CDD1Dull) >> 32);
    }
    f32 uniform() { return (next() >> 8) * (1.0f / 16777216.0f); }  // [0, 1)
};

// what a walk saw: its entries and exits, and the updates the latches disagreed with the player
struct Seen {
    std::vector<WaterEntry> entries;
    std::vector<WaterExit> exits;
    u32 updates = 0, wrong = 0;
    std::string first_wrong;
};

// `ticks` updates of a sixtieth (fewer if drive returns false), the latches checked each one
template <class Drive>
Seen walk(Player& pl, u32 ticks, Drive drive) {
    Seen seen;
    const PlayerParams& pp = pl.params();
    for (u32 i = 0; i < ticks; i++) {
        PlayerInput in;
        if (!drive(pl, i, in))
            break;
        const bool was_swimming = pl.swimming();
        const Vec3 before = pl.position();
        pl.update(in, TICK);
        seen.updates++;
        const WaterEntry* e = pl.water_entry();
        const WaterExit* x = pl.water_exit();
        const bool went_in = !was_swimming && pl.swimming();
        const bool came_out = was_swimming && !pl.swimming() && !pl.airborne();
        char why[160] = "";
        if (!!e != went_in)
            snprintf(why, sizeof(why), "tick %u: %s entry, the swim %s", i, e ? "an" : "no",
                     went_in ? "began" : "didn't begin");
        else if (!!x != came_out)
            snprintf(why, sizeof(why), "tick %u: %s exit, %s", i, x ? "an" : "no",
                     came_out ? "out onto the ground" : "not out");
        else if (e && (std::fabs(e->at.x - pl.position().x) > pp.radius ||
                       std::fabs(e->at.z - pl.position().z) > pp.radius || !(e->surface > e->at.y)))
            snprintf(why, sizeof(why), "tick %u: the entry at (%.1f, %.1f, %.1f) surface %.1f, the swimmer at (%.1f, "
                     "%.1f, %.1f)", i, e->at.x, e->at.y, e->at.z, e->surface, pl.position().x, pl.position().y,
                     pl.position().z);
        else if (x && std::fabs(x->climbed - (pl.position().y - before.y)) > 0.01f * pp.height)
            snprintf(why, sizeof(why), "tick %u: climbed %.2f, the feet rose %.2f", i, x->climbed,
                     pl.position().y - before.y);
        if (why[0] && !seen.wrong++)
            seen.first_wrong = why;
        if (e)
            seen.entries.push_back(*e);
        if (x)
            seen.exits.push_back(*x);
    }
    return seen;
}

void report(const char* tag, const char* name, const Seen& s, bool ok, const char* what, int* failed) {
    std::string in, out;
    for (const WaterEntry& e : s.entries) {
        char b[96];
        snprintf(b, sizeof(b), "%s%s vy %.1f speed %.2f", in.empty() ? "" : "; ", CAUSES[(int)e.cause], e.vy, e.speed);
        in += b;
    }
    for (const WaterExit& x : s.exits) {
        char b[64];
        snprintf(b, sizeof(b), "%sclimbed %.2f, %.2f over the surface", out.empty() ? "" : "; ", x.climbed,
                 x.over_surface);
        out += b;
    }
    ok = ok && !s.wrong;
    *failed += !ok;
    log("selftest water %s%s: %s (%s), %u updates: in %s; out %s%s%s", tag, name, ok ? "ok" : "MISMATCH", what,
        s.updates, in.empty() ? "none" : in.c_str(), out.empty() ? "none" : out.c_str(),
        s.wrong ? "; the latches wrong at " : "", s.wrong ? s.first_wrong.c_str() : "");
}

// a world direction (yaw, degrees) at a tilt
void toward(f32 yaw, f32 tilt, PlayerInput& in) {
    in.move_x = std::sin(yaw * PI / 180) * tilt, in.move_z = std::cos(yaw * PI / 180) * tilt;
}

bool floor_of(u16 a, Vec3 n) { return n.y > 0.05f && !kcl_is_water(a) && !kcl_is_fence(a); }

// the walks for a Mii (`who` for the log; `tag` before the walks' names)
int walks(const World& w, const PlayerParams& pp, const char* who, const char* tag) {
    int failed = 0;
    log("selftest water: %s: the Mii %.2f tall, walks %.3f and runs %.3f a frame, drops up to %.1f", who, pp.height,
        pp.walk_speed, pp.run_speed, pp.max_drop);
    const f32 v_deepest = std::sqrt(2.0f * pp.gravity * pp.max_drop);

    {  // the rock shore (selftest_walk.cpp's): out 20 s, then back to where it began
        Player pl(w, pp);
        const Vec3 from{3798.1f, 10, -6644.4f};
        pl.place(from, 80);
        Seen s = walk(pl, 40 * 60, [&](const Player& p, u32 i, PlayerInput& in) {
            if (i < 20 * 60) {
                toward(80, 1.0f, in);
                return true;
            }
            const f32 dx = from.x - p.position().x, dz = from.z - p.position().z;
            toward(std::atan2(dx, dz) * 180 / PI, 1.0f, in);
            return dx * dx + dz * dz > pp.radius * pp.radius;
        });
        const bool ok = s.entries.size() == 1 && s.entries[0].cause == WaterCause::WADE && s.entries[0].vy == 0 &&
                        s.exits.size() == 1 && s.exits[0].over_surface <= 0 && s.exits[0].climbed > 0;
        report(tag, "shore", s, ok, "walked in, waded out", &failed);
    }
    {  // Powder White Beach toward the sea, running
        Player pl(w, pp);
        pl.place(Vec3{6001, 100, 757}, 120);
        Seen s = walk(pl, 100 * 60, [&](const Player& p, u32, PlayerInput& in) {
            toward(120, 1.0f, in);
            in.run = true;
            return !p.swimming();
        });
        const bool ok = s.entries.size() == 1 &&
                        ((s.entries[0].cause == WaterCause::FALL && s.entries[0].vy < 0) ||
                         (s.entries[0].cause == WaterCause::WADE && s.entries[0].vy == 0));
        report(tag, "beach", s, ok, "into the sea off the sand", &failed);
    }
    // the plaza's west sea front: from the plaza (69) toward the sea, over a step (72) to its edge
    const Vec3 plaza{-2992.4f, 69, 3192.4f};
    const f32 front = -45;
    for (int sprint = 0; sprint < 2; sprint++) {
        Player pl(w, pp);
        pl.place(plaza, front);
        bool jumped = false;
        Seen s = walk(pl, 30 * 60, [&](const Player& p, u32, PlayerInput& in) {
            toward(front, 1.0f, in);
            in.run = true;
            in.fast_run = sprint;
            // (a running jump where the ground ends two radii ahead: off the edge)
            const Vec3 q = p.position();
            const f32 a = front * PI / 180, reach = 2 * pp.radius;
            Hit h;
            if (!sprint && !p.airborne() && !jumped &&
                !w.ground(q.x + std::sin(a) * reach, q.z + std::cos(a) * reach, q.y + pp.step_up, q.y - pp.max_drop,
                          floor_of, &h))
                in.jump = in.jump_held = jumped = true;
            in.jump_held = jumped;
            return !p.swimming();
        });
        bool ok = s.entries.size() == 1 && s.entries[0].vy < 0;
        if (sprint) {
            ok = ok && (s.entries[0].cause == WaterCause::LEAP || s.entries[0].cause == WaterCause::GLIDE);
        } else {
            ok = ok && jumped && s.entries[0].cause == WaterCause::JUMP && -s.entries[0].vy > v_deepest;
        }
        char what[96];
        snprintf(what, sizeof(what), "%s off the 72 sea front (a fall from the deepest drop: %.1f down)",
                 sprint ? "a sprint" : "a running jump", v_deepest);
        report(tag, sprint ? "leap" : "sea_front", s, ok, what, &failed);
    }
    {  // south of the town's east pier (its deck 5.56 over the sea, z 2760..2790), swum north to it
        Player pl(w, pp);
        const bool placed = pl.place_swimming(Vec3{-880, 0, 2830}, 180);
        Seen s = walk(pl, 30 * 60, [&](const Player& p, u32, PlayerInput& in) {
            toward(180, 1.0f, in);
            return p.swimming();
        });
        const f32 deck = 5.56f, sink = 0.62f * pp.height;
        const bool ok = placed && s.entries.empty() && s.exits.size() == 1 &&
                        std::fabs(s.exits[0].over_surface - deck) < 0.05f &&
                        std::fabs(s.exits[0].climbed - (deck + sink)) < 0.05f;
        char what[96];
        snprintf(what, sizeof(what), "climbed out onto the pier: %.2f over the surface, %.2f over the swimmer's feet",
                 deck, deck + sink);
        report(tag, "pier", s, ok, what, &failed);
    }
    {  // random walking, running, jumping and swimming from the rock shore
        Player pl(w, pp);
        pl.place(Vec3{3798.1f, 10, -6644.4f}, 80);
        Rng rng{std::hash<std::string>()(std::string(tag) + "water") | 1};
        f32 yaw = 80, tilt = 1;
        bool run = false;
        u32 change = 0;
        Seen s = walk(pl, 300 * 60, [&](const Player&, u32 i, PlayerInput& in) {
            if (i >= change) {
                yaw = rng.uniform() * 360, tilt = rng.uniform() < 0.15f ? 0 : 0.3f + 0.7f * rng.uniform();
                run = rng.uniform() < 0.3f;
                change = i + 60 + rng.next() % 180;
            }
            toward(yaw, tilt, in);
            in.run = run;
            in.jump = rng.uniform() < 0.01f;
            in.jump_held = in.jump;
            return true;
        });
        char what[64];
        snprintf(what, sizeof(what), "%zu in, %zu out", s.entries.size(), s.exits.size());
        report(tag, "random", s, !s.entries.empty() && !s.exits.empty(), what, &failed);
    }
    return failed;
}

}  // namespace

void selftest_water() {
    const char* dir = getenv("NOMA_KCL_DIR");
    if (!dir) {
        log("selftest water: NOMA_KCL_DIR not set (the folder with WS2_common_island1..5.kcl): skipped");
        return;
    }
    World w;
    for (int i = 1; i <= 5; i++) {
        std::string name = "WS2_common_island" + std::to_string(i) + ".kcl";
        std::vector<u8> d = slurp(std::string(dir) + "/" + name);
        if (d.empty() || !w.add_kcl(d.data(), (u32)d.size(), name.c_str())) {
            log("selftest water: %s: can't read it: MISMATCH", name.c_str());
            return;
        }
    }
    if (const char* tf = getenv("NOMA_KCL_TRUNKS")) {
        if (FILE* f = fopen(tf, "r")) {
            Cylinder c;
            while (fscanf(f, "%f,%f,%f,%f,%f", &c.x, &c.z, &c.y0, &c.y1, &c.radius) == 5)
                w.add_cylinder(c);
            fclose(f);
        }
    }
    w.finish();
    int failed = walks(w, PlayerParams::make(10.0f), "k 10", "");
    PlayerParams island = PlayerParams::make(ISLAND_K);
    island.walk_speed *= PACE;
    island.run_speed *= PACE * RUN_MULTIPLIER;
    failed += walks(w, island, "the island's k and pace", "island_");
    log("selftest water: %s", failed ? "MISMATCH" : "ok");
}

}  // namespace guest
