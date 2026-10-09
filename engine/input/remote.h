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
    // The bottom screen's touch as it is (the pointer above is where it points on the top screen):
    // held, and where, in the bottom screen's pixels (0..319 across, 0..239 down). The game reads
    // it only as remote.cpp makes it a press and a pointer; native scenes (a list's drag) read it.
    bool touching = false;
    u16 touch_x = 0, touch_y = 0;
    // how far the circle pad moved the pointer with this input (out of stick mode; in the pointer's
    // units): inside the bottom screen's region (input_set_touch_region) it moves the region's
    // pointer by as much
    float nudge_x = 0.0f, nudge_y = 0.0f;
};
// remote 1 from now on (the game sees it at its next read); with NOMA_OLD3DS=1 less what an Old
// 3DS doesn't have (ZL, ZR, the C-stick)
void wiimote_set_input(const WiimoteInput& in);
bool input_old3ds();
// For KPADReadEx (remote.cpp), at the game's read: the buttons pressed since the last read (a press
// and its release between two reads still counts), and whether a touch that presses as a tap (see
// wiimote_a_from_touch) has ended, which the game's cursor gets as A for one read: a tap presses
// what it is on.
u16 wiimote_take_pressed();
bool wiimote_take_tap();

// While a native scene has the bottom screen (engine/ui/screen.h), the touch screen points into a
// region of the layouts' space (its left and top edges, width and height, in the channel's layout
// units, y up): the game's pointer goes where the touch is in it, as the game's cursor reads the
// pointer (x / 304, -y / 228 of the layout units), and stays inside it (the circle pad, out of
// stick mode, moves it about there as it moves the pointer otherwise); a touch is a press there in
// stick mode too. A touch going on when the region changes (or its tap still to come) presses
// nothing: it was aimed at what was there before.
void input_set_touch_region(bool on, f32 left, f32 top, f32 width, f32 height);
// whether the pointer was last moved by the circle pad (or a script), not by a touch: the pointer's
// hand shows only then (what is touched is pointed at directly)
bool input_pointer_by_stick();
// whether the pointer is the touch's now (touching, or a tap's A still to come): the game's cursor
// places itself there at once (runtime/patches/overlays.cpp layout_Cursor_Update), without the
// settling it gives a remote's pointer coming back
bool input_pointer_from_touch();
// The stick (in stick mode) as the D-pad as well, while a scene's menus are up: tilted more than half
// way, it holds the D-pad's direction for the game (its menus and lists move their highlight); `dirs`
// the directions it may give (WM_UP | WM_DOWN for a list whose left and right mean something else).
void input_set_stick_as_dpad(bool on, u16 dirs = WM_UP | WM_DOWN | WM_LEFT | WM_RIGHT);
// The touch is a press, as pointing and holding A was on the Wii: inside the claimed region, A goes
// down at a touch's second read (the first places the pointer) and stays down until it lifts. A
// touch that begins in a drag area (a list's rows: input_set_drag_area) presses only as a tap, A
// for one read after it ends, and not at all once it has become a drag (input_mark_drag); so do a
// touch outside a region (the top screen's pointer; none in stick mode) and one gone before its
// second read.
//
// Whether the A of the game's last read came from the touch, not the A button (on every read of a
// held touch): a scene that takes A for something else than pressing (a jump) leaves it out.
bool wiimote_a_from_touch();
// the A button's own state at the game's last read, the touch's A apart: held, and pressed since the
// read before (a scene where the touch presses its buttons and A does something else: the island's jump)
bool wiimote_a_button_held();
bool wiimote_a_button_pressed();
// a drag area for this frame, in the touch screen's pixels (x0, y0 inclusive, x1, y1 exclusive): the
// scene showing a draggable list renews it every frame; one not renewed for two reads is forgotten,
// and a new region (input_set_touch_region) forgets it at once. Whether a touch is in it is decided
// where the touch begins.
void input_set_drag_area(u16 x0, u16 y0, u16 x1, u16 y1);
// the touch going on is a drag (engine/ui/drag_scroll.h): no press (none at its release, and a held
// A, if it had one, let go), and the region's pointer goes back to where the touch began and stays
// there (nothing else takes the focus under the finger) until the next touch begins
void input_mark_drag();
// the tap just ended (or its A still waiting) is no press: a drag (a second guard: input_mark_drag)
void input_cancel_tap();
// The D-pad (or the stick as one) moved a scene's highlight: the pointer leaves the screen, as a Wii
// Remote pointed away, until the next touch or circle-pad nudge, so that the game's cursor drops the
// focus it had and A presses the highlight.
void input_pointer_away();
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
