// Wuhu Island's sound effects (docs/wuhu.md "Sounds"): Wii Sports Resort's own, staged from your disc
// into DATA/wuhu/sounds.nsfx (tools/wuhu/stage_sounds.py) and played natively over the island's music
// (engine/audio/sfx.h: the channel's own sound system plays only its own archive's sounds). island.cpp
// says what happens -- a foot down, a jump, a landing, a splash, a swimming stroke, the parachute
// opening, a vehicle got into or out of -- and, once an update, what goes on, for the loops: the wind
// under the parachute, the car's hum, the boat's engine and its hull in the water, each following
// the vehicle's speed.
//
// The footsteps are Swordplay Showdown's (a walk through the island), told apart by the ground as WSR
// tells them: the KCL attribute under the feet (grass, dirt, hard ground, sand, wood), the water's
// when the feet are in it. The levels are WSR's own (its archive's volumes for each note), all times
// one level against the island's music (sounds.cpp LEVEL_DB): NOMA_WUHU_SFX_DB=x moves it by x dB,
// NOMA_WUHU_SFX=0 turns the effects off, NOMA_WUHU_SFX_LOG=1 logs each one played.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "engine/audio/sfx.h"
#include "runtime/runtime.h"

namespace engine::wuhu {

class IslandSounds {
public:
    enum Ground : u8 { GRASS, DIRT, HARD, SAND, WOOD, WATER, N_GROUNDS };
    // the ground under the feet by its KCL attribute (engine/collision/kcl.h; a structure's, and any
    // other's, is hard ground), or the water's when the feet are in it
    static Ground ground(u16 attribute, bool wading);

    // the bank as the island's load read it (island_load.h); none (empty): the island is silent but
    // for its music, `why` in the log
    IslandSounds(std::vector<u8> bank, const std::string& why);
    ~IslandSounds();
    IslandSounds(const IslandSounds&) = delete;
    IslandSounds& operator=(const IslandSounds&) = delete;

    bool on() const { return bank_ != nullptr; }

    void step(Ground g, int foot, bool run);  // a foot down (0 the left, 1 the right)
    void jump(Ground g);                      // off the ground
    void land(Ground g, f32 strength);        // down on it again: 0 a hop, 1 the deepest drop
    // into the water, as hard as the splash island.cpp makes (its strength; 0: waded in)
    void splash(f32 strength);
    void stroke();                            // a swimming hand into the water
    void parachute();                         // the canopy opening
    void vehicle_in(bool boat);
    void vehicle_out(bool boat, Ground g);    // onto the ground (out of a boat into the water: splash)

    // what goes on, for the loops
    struct Going {
        bool glide = false;  // under the parachute
        u8 vehicle = 0;      // 0 on foot, CAR, BOAT
        f32 speed = 0;       // the vehicle's, of its top speed (0..1)
        f32 throttle = 0;    // the stick's ahead (-1..1)
    };
    enum : u8 { CAR = 1, BOAT = 2 };
    // once an update, `dt` seconds after the last; frozen (the menu over the island, a fade): the loops
    // fade out, and back in after
    void update(const Going& going, bool frozen, f32 dt);

private:
    struct Loop {
        int sound = -1;
        engine::audio::SfxHandle handle = 0;
        f32 gain = 0, pitch = 1;  // where it is (eased)
    };
    engine::audio::SfxHandle play(int sound, f32 gain = 1.0f, f32 pitch = 1.0f);
    void drive(Loop& l, f32 gain, f32 pitch, f32 dt);
    int find(const char* name);

    std::unique_ptr<engine::audio::SfxBank> bank_;
    f32 level_ = 1;  // the effects' level against the music (linear)
    bool log_ = false;
    std::vector<std::string> missing_;
    // the sounds by what they are for (-1: the bank has none)
    int steps_[N_GROUNDS][4] = {};  // walking left, right, running left, right
    int jumps_[N_GROUNDS] = {}, lands_[N_GROUNDS] = {};
    int parachute_ = -1, splash_[3] = {-1, -1, -1}, stroke_ = -1, car_in_ = -1, car_out_ = -1;
    Loop wind_, car_, boat_[3], hull_[2];  // the boat's engine idling, low, high; its hull low, high
    f32 rev_ = 0;                          // the driven vehicle's engine: its speed and throttle, eased
    f32 log_time_ = 0;                     // (NOMA_WUHU_SFX_LOG: seconds since the loops' last line)
};

}  // namespace engine::wuhu
