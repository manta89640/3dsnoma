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

// the 3DS's own buttons, which a Wii Remote doesn't have (WiimoteInput::pad)
enum : u16 {
    PAD_L = 0x0001,
    PAD_R = 0x0002,
    PAD_ZL = 0x0004,  // (New 3DS)
    PAD_ZR = 0x0008,  // (New 3DS)
};

struct WiimoteInput {
    u16 buttons = 0;           // the WM_ bits held
    bool pointing = true;      // aimed at the screen
    float x = 0.0f, y = 0.0f;  // where: -1..1 across the screen, y down
    // For native scenes only (the game's KPAD reads none of these). The stick walks the player: the
    // circle pad while a scene has stick mode on (input_set_stick_mode); the camera stick is the
    // New 3DS's C-stick. Both -1..1, x right and y UP (unlike the pointer), 0 when let go.
    float stick_x = 0.0f, stick_y = 0.0f;
    float cam_x = 0.0f, cam_y = 0.0f;
    u16 pad = 0;               // the PAD_ bits held
};
// remote 1 from now on (the game sees it at its next read)
void wiimote_set_input(const WiimoteInput& in);
// the input as the platform last set it: native scenes read the stick, the camera stick and the
// pad here (and the buttons and the pointer, if they like)
const WiimoteInput& wiimote_input();
// Stick mode, for a native scene that walks something about (the island): the platform's stick
// fills stick_x/y instead of moving the pointer, and no longer keeps the pointer on the screen (on
// the 3DS the circle pad nudges the pointer otherwise; touching still points). The scene turns it on
// in its constructor and off in its destructor.
void input_set_stick_mode(bool on);
bool input_stick_mode();
// NOMA_INPUT (runtime/hw/ios/input_script.cpp): is there a script; merge it into `in` (on the
// game's clock)
bool input_script_active();
void input_script_apply(WiimoteInput& in);
// the game asked for message `id` (its text at `text`): scripts can count from the first time,
// and NOMA_DEBUG_MSG=1 logs each message the first time
void input_script_message(u32 id, u32 text);

}  // namespace guest
