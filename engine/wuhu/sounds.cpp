// Wuhu Island's sound effects (see sounds.h).
//
// What plays when (the sounds' names are tools/wuhu/stage_sounds.py's):
//   - a foot down walking or running: Swordplay Showdown's step for that foot and gait on the ground
//     (step.GROUND.walk_l ...), one of its two recordings at random, its pitch and level varied as
//     Showdown's sequence varies them;
//   - a jump and a landing: Showdown's on the ground (jump.GROUND, land.GROUND), a landing as loud as
//     the drop was deep (half as loud for a hop);
//   - into the water: walking in, a splash of the water's steps; a fall, a jump, a glide or a slide,
//     by the splash's strength: Cycling's wheel landing in the shallows (small), a cyclist falling in
//     (medium, from a jump off a beach), a Mii knocked off Swordplay's stage into the sea (large, from
//     a drop of over a Mii's height and a half); over a boat's side, the small one;
//   - each swimming hand into the water: Canoeing's light paddle stroke;
//   - the parachute opening: Skydiving's canopy opening over the player; the glide under it, Island
//     Flyover's wind in flight (a loop);
//   - into a car and out of it: Skydiving's steps on the plane's floor and out of its hatch; into a
//     boat, a landing on wood (its deck), out of one onto land a landing on the ground there;
//   - driving a car: the island's electric cars' hum (a loop), louder and higher with the speed and
//     the throttle; a boat: Wakeboarding's tow boat, its engine idling, low and high crossfaded by the
//     speed (each higher with it), and its hull's water, low and high, rising with the speed.
// The loops follow what goes on eased (a quarter of a second up, a little more down), so that their
// level and pitch don't jump with the stick; frozen, they fade out, and the vehicle's back in after.
#include "engine/wuhu/sounds.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "runtime/hw/hw.h"

namespace engine::wuhu {

namespace {

using engine::audio::SfxBank;
using engine::audio::SfxHandle;

// The effects' level against the island's music. The staged levels are WSR's own (its archive's
// volumes, for the notes as its sequences play them). 「広い世界へ」 plays at a loud RMS of about -21.6 dBFS
// in the host's recording of the island, and WSR's streamed music at its archive's volumes at a median
// of -18.3 dBFS (42 pieces over 30 s, measured as tools/music/stage_music.py measures loudness): WSR's
// balance against our music is its levels 3.3 dB down. At that a jump into the sea off the plaza's front
// (the large splash with the strokes after it) still went 8 dB over the music and clipped the host's
// recording, the channel's mix having no room for WSR's loudest effects over its music: 6 dB down, the
// splash peaks about 6 dB under full scale, a walk's steps about 20.
constexpr f32 LEVEL_DB = -6.0f;
// The footsteps' level of their own: WSR's Showdown sets its steps well under its music (its archive's
// volume 50, a quarter of the splashes' level), and 6 dB further down they would be lost under ours on
// the 3DS's speakers; 3 dB up they peak about 17 dB under full scale, the music's peaks about 5.
constexpr f32 STEP_GAIN = 1.4f;
// a swimming stroke's level of its own: the crawl's hands go in four times a second at speed, each
// stroke a second long, so up to five overlap
constexpr f32 STROKE_GAIN = 0.4f;
const char* const GROUND_NAMES[IslandSounds::N_GROUNDS] = {"grass", "dirt", "hard", "sand", "wood", "water"};
const char* const STEP_KINDS[4] = {"walk_l", "walk_r", "run_l", "run_r"};
// the splash's strength (island.cpp: SPLASH_WEAKEST 0.15 .. SPLASH_STRONGEST 1.5) where the next
// sound takes over: a jump off a beach is about 0.65, a drop of a Mii height and a half 0.9
constexpr f32 SPLASH_MEDIUM = 0.45f, SPLASH_LARGE = 0.9f;
// the loops' easing (seconds): up, down
constexpr f32 EASE_UP = 0.25f, EASE_DOWN = 0.3f;
constexpr f32 SILENT = 0.002f;  // a loop eased under this stops

f32 clamp01(f32 v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

}  // namespace

IslandSounds::Ground IslandSounds::ground(u16 attribute, bool wading) {
    if (wading)
        return WATER;
    switch (attribute) {
    case 0x1101: return GRASS;
    case 0x1102: return DIRT;
    case 0x1104: return SAND;
    case 0x1105: return WOOD;
    case 0x1106: return WATER;
    default: return HARD;  // (0x1103 hard ground, 0x1108 the structures, and the rest)
    }
}

IslandSounds::IslandSounds(std::vector<u8> bank, const std::string& why) {
    const char* off = std::getenv("NOMA_WUHU_SFX");
    if (off && !std::atoi(off)) {
        guest::log("wuhu: sounds: off (NOMA_WUHU_SFX=0)");
        return;
    }
    if (bank.empty()) {
        guest::log("wuhu: sounds: none (%s)", why.c_str());
        return;
    }
    std::string error;
    const u32 bytes = (u32)bank.size();
    bank_ = SfxBank::load(std::move(bank), &error);
    if (!bank_) {
        guest::log("wuhu: sounds: the bank can't be read: %s (stage it again: tools/wuhu/stage_sounds.py DATA)",
                   error.c_str());
        return;
    }
    f32 db = LEVEL_DB;
    if (const char* e = std::getenv("NOMA_WUHU_SFX_DB"))
        db += (f32)std::atof(e);
    level_ = std::pow(10.0f, db / 20.0f);
    const char* log = std::getenv("NOMA_WUHU_SFX_LOG");
    log_ = log && std::atoi(log);
    char name[48];
    for (int g = 0; g < N_GROUNDS; g++) {
        for (int k = 0; k < 4; k++) {
            std::snprintf(name, sizeof(name), "step.%s.%s", GROUND_NAMES[g], STEP_KINDS[k]);
            steps_[g][k] = find(name);
        }
        std::snprintf(name, sizeof(name), "jump.%s", GROUND_NAMES[g]);
        jumps_[g] = find(name);
        std::snprintf(name, sizeof(name), "land.%s", GROUND_NAMES[g]);
        lands_[g] = find(name);
    }
    parachute_ = find("parachute.open");
    splash_[0] = find("splash.small"), splash_[1] = find("splash.medium"), splash_[2] = find("splash.large");
    stroke_ = find("swim.stroke");
    car_in_ = find("car.in"), car_out_ = find("car.out");
    wind_.sound = find("glide.wind");
    car_.sound = find("car.engine");
    boat_[0].sound = find("boat.engine.idle"), boat_[1].sound = find("boat.engine.low");
    boat_[2].sound = find("boat.engine.high");
    hull_[0].sound = find("boat.water.low"), hull_[1].sound = find("boat.water.high");
    std::string missing;
    for (const std::string& m : missing_)
        missing += (missing.empty() ? "" : ", ") + m;
    guest::log("wuhu: sounds: %u sounds, %u waves, %u bytes, level %+.1f dB%s%s", (unsigned)bank_->sounds(),
               (unsigned)bank_->waves(), (unsigned)bytes, db, missing.empty() ? "" : "; missing (an older bank): ",
               missing.c_str());
}

IslandSounds::~IslandSounds() {
    // (the bank's own end stops what plays from it, waiting for an AX frame that may be reading it)
    bank_.reset();
}

int IslandSounds::find(const char* name) {
    const int s = bank_->find(name);
    if (s < 0)
        missing_.push_back(name);
    return s;
}

SfxHandle IslandSounds::play(int sound, f32 gain, f32 pitch) {
    if (!bank_ || sound < 0)
        return 0;
    const SfxHandle h = engine::audio::sfx_play(*bank_, sound, gain * level_, pitch);
    if (log_)
        guest::log("wuhu: sound %s at %.2f, pitch %.2f (frame %u)", bank_->name(sound), gain, pitch,
                   (unsigned)guest::vi_game_frames());
    return h;
}

void IslandSounds::step(Ground g, int foot, bool run) {
    play(steps_[g][(run ? 2 : 0) + (foot & 1)], STEP_GAIN);
}

void IslandSounds::jump(Ground g) { play(jumps_[g]); }

void IslandSounds::land(Ground g, f32 strength) { play(lands_[g], 0.5f + 0.5f * clamp01(strength)); }

void IslandSounds::splash(f32 strength) {
    if (strength <= 0) {
        // walked in: the water's own landing, a little under its level
        play(lands_[WATER], 0.7f);
        return;
    }
    if (strength < SPLASH_MEDIUM)
        play(splash_[0], 0.6f + 0.4f * strength / SPLASH_MEDIUM);
    else if (strength < SPLASH_LARGE)
        play(splash_[1], 0.75f + 0.25f * (strength - SPLASH_MEDIUM) / (SPLASH_LARGE - SPLASH_MEDIUM));
    else
        play(splash_[2], std::min(1.0f, 0.8f + 0.2f * (strength - SPLASH_LARGE) / SPLASH_LARGE));
}

void IslandSounds::stroke() { play(stroke_, STROKE_GAIN); }

void IslandSounds::parachute() { play(parachute_); }

void IslandSounds::vehicle_in(bool boat) {
    if (boat)
        play(lands_[WOOD], 0.7f);
    else
        play(car_in_);
}

void IslandSounds::vehicle_out(bool boat, Ground g) {
    if (boat)
        play(lands_[g], 0.7f);
    else
        play(car_out_);
}

// a loop toward `gain` and `pitch` (eased), started when it rises from nothing, stopped when it has
// eased back to it
void IslandSounds::drive(Loop& l, f32 gain, f32 pitch, f32 dt) {
    if (l.sound < 0)
        return;
    const f32 tau = gain > l.gain ? EASE_UP : EASE_DOWN;
    const f32 k = dt > 0 ? 1.0f - std::exp(-dt / tau) : 0.0f;
    l.gain += (gain - l.gain) * k;
    l.pitch += (pitch - l.pitch) * k;
    if (!l.handle && gain > SILENT && l.gain > SILENT) {
        l.handle = play(l.sound, 1.0f, 1.0f);
        if (!l.handle)
            return;
    }
    if (!l.handle)
        return;
    if (gain <= SILENT && l.gain <= SILENT) {
        engine::audio::sfx_stop(l.handle, 0.05f);
        l.handle = 0;
        l.gain = 0;
        return;
    }
    engine::audio::sfx_set(l.handle, l.gain, l.pitch);
}

void IslandSounds::update(const Going& going, bool frozen, f32 dt) {
    if (!bank_)
        return;
    const f32 on = frozen ? 0.0f : 1.0f;
    drive(wind_, going.glide ? on : 0.0f, 1.0f, dt);
    // the engine: the speed, and a little more while the stick asks for more (eased: it revs up and
    // dies down rather than jumping)
    const f32 f = clamp01(going.speed);
    const f32 want = going.vehicle ? clamp01(f + 0.2f * std::max(0.0f, going.throttle)) : 0.0f;
    const f32 k = dt > 0 ? 1.0f - std::exp(-dt / EASE_UP) : 0.0f;
    rev_ += (want - rev_) * k;
    const bool car = going.vehicle == CAR, boat = going.vehicle == BOAT;
    // the car's hum: quiet and low standing, full and a fifth up at speed
    drive(car_, car ? on * (0.55f + 0.45f * f) : 0.0f, 0.8f + 0.7f * rev_, dt);
    // the boat's engine idling, low, high, crossfaded by the speed; its hull's water rising with it
    const f32 idle = clamp01(1.0f - f / 0.3f), high = clamp01((f - 0.45f) / 0.45f);
    const f32 low = clamp01(1.0f - idle - high);
    const f32 b = boat ? on : 0.0f;
    drive(boat_[0], b * (0.25f + 0.75f * idle), 0.9f + 0.4f * rev_, dt);
    drive(boat_[1], b * low, 0.85f + 0.45f * rev_, dt);
    drive(boat_[2], b * high, 0.85f + 0.35f * rev_, dt);
    drive(hull_[0], b * clamp01(f / 0.25f) * (1.0f - 0.6f * high), 0.9f + 0.3f * f, dt);
    drive(hull_[1], b * clamp01((f - 0.5f) / 0.4f), 0.9f + 0.2f * f, dt);
    // (NOMA_WUHU_SFX_LOG: the loops playing and the mixer's voices, a second at a time)
    if (log_ && (log_time_ += dt) >= 1.0f) {
        log_time_ = 0;
        const Loop* loops[] = {&wind_, &car_, &boat_[0], &boat_[1], &boat_[2], &hull_[0], &hull_[1]};
        std::string playing;
        for (const Loop* l : loops)
            if (l->handle) {
                char one[64];
                std::snprintf(one, sizeof(one), "%s%s %.2f at %.2f", playing.empty() ? "" : ", ", bank_->name(l->sound),
                              l->gain, l->pitch);
                playing += one;
            }
        if (!playing.empty() || engine::audio::sfx_voices())
            guest::log("wuhu: sounds: %u voices; loops: %s", (unsigned)engine::audio::sfx_voices(),
                       playing.empty() ? "none" : playing.c_str());
    }
}

}  // namespace engine::wuhu
