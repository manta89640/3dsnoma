// The Wii Remote the game reads (remote.cpp): what the player is doing, set by the platform layer
// (the 3DS's buttons, and its touch screen or circle pad as the pointer; NOMA_INPUT on the host).
#pragma once

#include "noma_decomp.h"

namespace guest {

// button bits, as WPAD and KPAD give them to the game
enum : u16 {
    WM_LEFT = 0x0001,
    WM_RIGHT = 0x0002,
    WM_DOWN = 0x0004,
    WM_UP = 0x0008,
    WM_PLUS = 0x0010,
    WM_TWO = 0x0100,
    WM_ONE = 0x0200,
    WM_B = 0x0400,
    WM_A = 0x0800,
    WM_MINUS = 0x1000,
    WM_HOME = 0x8000,
};

struct WiimoteInput {
    u16 buttons = 0;           // the WM_ bits held
    bool pointing = true;      // aimed at the screen
    float x = 0.0f, y = 0.0f;  // where: -1..1 across the screen, y down
};
// remote 1 from now on (the game sees it at its next read)
void wiimote_set_input(const WiimoteInput& in);
// NOMA_INPUT (runtime/hw/ios/input_script.cpp): is there a script; merge it into `in` (on the
// game's clock)
bool input_script_active();
void input_script_apply(WiimoteInput& in);
// the game asked for message `id` (its text at `text`): scripts can count from the first time,
// and NOMA_DEBUG_MSG=1 logs each message the first time
void input_script_message(u32 id, u32 text);

}  // namespace guest
