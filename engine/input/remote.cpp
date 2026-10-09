// The Wii Remote, native. The game reads its remotes through KPAD (KPADReadEx, every frame) and
// asks WPAD whether one is connected. On the Wii a whole stack sits underneath: the Bluetooth stack
// (BTE, WUD), WPAD's reports, the remote's speaker stream and rumble, and KPAD working out the
// pointer from the IR camera's view of the sensor bar. Here remote 1 is the 3DS's controls (the
// platform layer's WiimoteInput), always connected and held level; remotes 2-4 aren't there.
// KPADInitEx doesn't start WPAD, so the Bluetooth stack never runs, and nothing goes to a speaker
// (the game's connect callback only sets one up).
#include "engine/input/remote.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace guest {

namespace {
WiimoteInput s_in;  // set by the platform layer
bool s_stick_mode;  // set by a native scene
// Between two of the game's reads (KPADReadEx, once a game frame) the platform sets the input every
// retrace: a button pressed and let go in between would never be seen held. s_pressed: the buttons
// pressed since the last read, shown held at the next one (and let go at the one after).
u16 s_prev_buttons, s_pressed;
// The touch screen is a press, not only a pointer. Inside a claimed region a touch is A held, as
// pointing and holding A was on the Wii: from the touch's second read (the first places the
// pointer) until it lifts, so the game's buttons push as the finger comes down and its arrows
// (layout::RepeatButton) and the native lists' tabs repeat while it stays. A touch that begins in
// a drag area (a list's rows, which the touch may drag instead) is a tap: when it ends, the game's
// cursor gets A where it was, for one read; so is a touch outside a region (the top screen's
// pointer), and one that ended before its second read. A drag (input_mark_drag) and a touch the
// region changed under press nothing. Outside a region in stick mode a touch is no press (a scene
// that walks something about: there A is the scene's, a jump on Wuhu Island).
bool s_was_touching, s_tap;
bool s_tap_waiting;  // a tap, its A not given yet (KPADReadEx)
// the touch going on, or the last one: the touch-downs so far (KPADReadEx tells a new touch from the
// one it was counting by them), and what it is
u32 s_touches;
bool s_touch_deferred;  // it began in a drag area: a tap at its release, no A while it lasts
bool s_touch_drag;      // a drag (input_mark_drag): no press, the region's pointer where it began
bool s_touch_void;      // the region changed under it: it was aimed at what was there before
bool s_touch_pressed;   // its A has been given: no tap at its release
bool s_a_from_touch;    // (wiimote_a_from_touch)
bool s_a_button_held, s_a_button_pressed;  // the A button's own, at the game's last read
// The drag area (input_set_drag_area: touch pixels x0, y0, x1, y1) and the game's reads since it was
// last set: its scene renews it every frame, between two reads, so one not renewed by the second
// read after is no longer on the screen.
const u32 DRAG_AREA_READS = 2;
u16 s_drag_area[4];
u32 s_drag_area_reads = DRAG_AREA_READS;
bool s_stick_dpad;  // input_set_stick_as_dpad, and the directions it gives
u16 s_stick_dirs;
// the bottom screen's region (input_set_touch_region), the pointer it last gave, and where the
// touch going on began (input_mark_drag)
bool s_region;
f32 s_region_box[4], s_region_x, s_region_y, s_down_x, s_down_y;
bool s_pointer_by_stick;  // (input_pointer_by_stick)
bool s_away;              // input_pointer_away, until the next touch or nudge

bool in_drag_area(u16 x, u16 y) {
    return s_drag_area_reads < DRAG_AREA_READS && x >= s_drag_area[0] && y >= s_drag_area[1] && x < s_drag_area[2] &&
           y < s_drag_area[3];
}

// the touch going on is A held (KPADReadEx): in a region, and neither a tap, a drag nor void
// the touch's pointer in the region: where the touched pixel's middle is in its box
void touch_to_region() {
    const f32 lx = s_region_box[0] + ((f32)s_in.touch_x + 0.5f) / 320.0f * s_region_box[2];
    const f32 ly = s_region_box[1] - ((f32)s_in.touch_y + 0.5f) / 240.0f * s_region_box[3];
    s_region_x = lx / 304.0f, s_region_y = -ly / 228.0f;
}

bool touch_holds() { return s_region && !s_touch_deferred && !s_touch_drag && !s_touch_void; }

// the touch that has just ended is a tap (its A at the next read)
bool touch_taps() {
    if (s_touch_drag || s_touch_void)
        return false;
    if (!s_region)
        return !s_stick_mode;
    return s_touch_deferred || !s_touch_pressed;
}
}  // namespace

// NOMA_OLD3DS=1: an Old 3DS's controls on any machine (a New 3DS, an emulator, the host's scripts):
// no ZL, ZR or C-stick, which an Old 3DS doesn't have (libctru starts the New 3DS's irrst only on
// one), so what needs them shows without them
bool input_old3ds() {
    static const bool s_old = [] {
        const char* e = getenv("NOMA_OLD3DS");
        return e && *e && strcmp(e, "0");
    }();
    return s_old;
}

void wiimote_set_input(const WiimoteInput& in) {
    s_in = in;
    if (input_old3ds()) {
        s_in.pad &= ~(PAD_ZL | PAD_ZR);
        s_in.cam_x = s_in.cam_y = 0;
    }
    if (s_stick_dpad && s_stick_mode) {
        const f32 h = 0.5f;
        s_in.buttons |= s_stick_dirs & ((s_in.stick_y > h ? WM_UP : 0) | (s_in.stick_y < -h ? WM_DOWN : 0) |
                                        (s_in.stick_x < -h ? WM_LEFT : 0) | (s_in.stick_x > h ? WM_RIGHT : 0));
    }
    s_pressed |= s_in.buttons & ~s_prev_buttons;
    s_prev_buttons = s_in.buttons;
    const f32 dx = s_in.nudge_x, dy = s_in.nudge_y;
    const bool nudged = dx != 0 || dy != 0;
    if (s_in.touching)
        s_pointer_by_stick = false;
    else if (nudged)
        s_pointer_by_stick = true;
    if (s_in.touching || nudged)
        s_away = false;
    const bool down = s_in.touching && !s_was_touching;
    if (down) {
        s_touches++;
        s_touch_deferred = s_region && in_drag_area(s_in.touch_x, s_in.touch_y);
        s_touch_drag = s_touch_void = s_touch_pressed = false;
    }
    if (s_region) {
        if (s_in.touching && !s_touch_drag) {
            touch_to_region();
            if (down)
                s_down_x = s_region_x, s_down_y = s_region_y;
        } else if (!s_in.touching && nudged) {
            const f32 x0 = s_region_box[0] / 304.0f, x1 = (s_region_box[0] + s_region_box[2]) / 304.0f;
            const f32 y0 = -s_region_box[1] / 228.0f, y1 = -(s_region_box[1] - s_region_box[3]) / 228.0f;
            s_region_x = std::min(std::max(s_region_x + dx, x0), x1);
            s_region_y = std::min(std::max(s_region_y + dy, y0), y1);
        }
        s_in.x = s_region_x, s_in.y = s_region_y;
    }
    if (s_was_touching && !s_in.touching && touch_taps())
        s_tap = true;
    // a drag let go takes the pointer with it, as a remote pointed away: held where the drag began,
    // it would give the focus to whatever row scrolls under it once the rows take presses again
    if (s_was_touching && !s_in.touching && s_touch_drag)
        s_away = true;
    s_was_touching = s_in.touching;
    if (s_away)
        s_in.pointing = false;
}

void input_set_stick_as_dpad(bool on, u16 dirs) {
    s_stick_dpad = on;
    s_stick_dirs = dirs;
}

void input_set_touch_region(bool on, f32 left, f32 top, f32 width, f32 height) {
    s_region = on;
    s_region_box[0] = left, s_region_box[1] = top, s_region_box[2] = width, s_region_box[3] = height;
    // (the pointer at the box's middle; where the finger is, if a touch goes on: a drag it may yet
    // become holds the pointer where it began)
    s_region_x = (left + width / 2) / 304.0f, s_region_y = -(top - height / 2) / 228.0f;
    if (s_was_touching)
        touch_to_region();
    s_down_x = s_region_x, s_down_y = s_region_y;
    // what was on the bottom screen has gone: a touch on it (going on, or its tap still to come)
    // presses nothing in what comes now, and its drag area goes with it
    input_cancel_tap();
    s_touch_void = s_was_touching;
    s_touch_drag = false;
    s_drag_area_reads = DRAG_AREA_READS;
}

void input_set_drag_area(u16 x0, u16 y0, u16 x1, u16 y1) {
    s_drag_area[0] = x0, s_drag_area[1] = y0, s_drag_area[2] = x1, s_drag_area[3] = y1;
    s_drag_area_reads = 0;
}

void input_mark_drag() {
    s_touch_drag = true;
    if (s_region) {
        s_region_x = s_down_x, s_region_y = s_down_y;
        s_in.x = s_region_x, s_in.y = s_region_y;
    }
    if (!s_in.touching)
        input_cancel_tap();  // (it has just ended: the tap its release made)
}

void input_cancel_tap() {
    s_tap = false;
    s_tap_waiting = false;
}

void input_pointer_away() {
    s_away = true;
    s_in.pointing = false;
}

bool input_pointer_from_touch() { return s_in.touching || s_tap || s_tap_waiting; }

// the game's read takes the latched presses and the tap (KPADReadEx)
u16 wiimote_take_pressed() {
    u16 p = s_pressed;
    s_pressed = 0;
    return p;
}
bool wiimote_take_tap() {
    bool t = s_tap;
    s_tap = false;
    return t;
}
bool wiimote_a_from_touch() { return s_a_from_touch; }
bool wiimote_a_button_held() { return s_a_button_held; }
bool wiimote_a_button_pressed() { return s_a_button_pressed; }
const WiimoteInput& wiimote_input() { return s_in; }
void input_set_stick_mode(bool on) { s_stick_mode = on; }
bool input_pointer_by_stick() { return s_pointer_by_stick; }
bool input_stick_mode() { return s_stick_mode; }

}  // namespace guest

namespace g {

namespace {

// KPADStatus: 0xF0 bytes, big-endian; a core remote fills these (the rest, extension controllers
// and MotionPlus, stays 0)
enum : u32 {
    KS_HOLD = 0x00, KS_TRIG = 0x04, KS_RELEASE = 0x08,
    KS_ACC = 0x0C, KS_ACC_VALUE = 0x18,       // acceleration in g (3 floats), its magnitude
    KS_POS = 0x20, KS_VEC = 0x28, KS_SPEED = 0x30,  // the pointer, its move since the last read, how far
    KS_HORIZON = 0x34,                        // roll: (cos, sin)
    KS_DIST = 0x48,                           // from the sensor bar, in metres
    KS_ACC_VERTICAL = 0x54,
    KS_DEV_TYPE = 0x5C, KS_WPAD_ERR = 0x5D, KS_DPD_VALID = 0x5E, KS_DATA_FORMAT = 0x5F,
    KS_SIZE = 0xF0,
};
enum : s32 {
    KPAD_READ_ERR_NONE = 0, KPAD_READ_ERR_NO_CONTROLLER = -2, KPAD_READ_ERR_INIT = -5,
    WPAD_ERR_NO_CONTROLLER = -1, WPAD_ERR_BUSY = -2,
};
enum : u8 { WPAD_DEV_CORE = 0, WPAD_FMT_CORE_ACC_DPD = 2, DPD_TWO_POINTS = 2 };

bool s_ready;
u32 s_hold;          // the buttons at the last read
float s_x, s_y;      // the pointer at the last read
// A touch's A (held, or a tap's) comes at the TAP_READY-th read that shows its pointer: the reads
// while it is down and, for a tap, while its A waits (a touch that came and went between two reads
// is shown first by the read after). The first places the pointer, so that what the touch is on has
// the game's cursor's focus a read before A comes. The game's own buttons wouldn't need that read:
// the cursor (layout_Cursor_Update) places itself and hit-tests before it takes A, in the same
// update. The native scenes do: they update before the cursor and look at the focus it gave at its
// last update (the Sound Player's rows and controls, the island's menus). The 10 updates the
// cursor waits after the pointer comes back, as a remote's pointer settles, are skipped for the
// touch's pointer (runtime/patches/overlays.cpp): inside them its A would be lost.
const u32 TAP_READY = 2;
u32 s_touch_seen;    // the touch the reads are counting (guest::s_touches)
u32 s_touch_reads;   // the reads that have shown its pointer
bool s_touch_held;   // the last read's A was the touch's, held

void store_f32(u32 a, float v) {
    u32 bits;
    std::memcpy(&bits, &v, 4);
    guest::store32(a, bits);
}

}  // namespace

void KPADInitEx(u32, u32) { s_ready = true; }
void KPADSetPosParam(u32, f64, f64) {}  // (the pointer's dead zone and smoothing: it goes where it's put)
void KPADSetConnectCallback(u32, u32) {}
void WPADRegisterAllocator(u32, u32) {}

// KPADReadEx(chan, samples, length, &result): how many samples were written, newest first. One a
// read: the controls as they are now, with the buttons pressed and released since the last read.
u32 KPADReadEx(u32 chan, u32 samples, u32 length, u32 result) {
    s32 err = KPAD_READ_ERR_NONE;
    u32 n = 0;
    if (!s_ready) {
        err = KPAD_READ_ERR_INIT;
    } else if (chan != 0) {
        err = KPAD_READ_ERR_NO_CONTROLLER;
    } else if (samples && length) {
        const guest::WiimoteInput& in = guest::s_in;
        float x = s_x, y = s_y;
        if (in.pointing)
            x = in.x, y = in.y;
        if (guest::s_drag_area_reads < guest::DRAG_AREA_READS)
            guest::s_drag_area_reads++;
        if (guest::wiimote_take_tap())
            guest::s_tap_waiting = true;
        if (!in.pointing || (s_hold & guest::WM_A))
            guest::s_tap_waiting = false;  // (the pointer gone, or A held anyway: no press later on)
        if (in.touching || guest::s_tap_waiting) {
            if (s_touch_seen != guest::s_touches)
                s_touch_seen = guest::s_touches, s_touch_reads = 0, s_touch_held = false;
            s_touch_reads++;
        }
        const bool placed = s_touch_reads >= TAP_READY;
        // what is held now, with what was pressed and let go since the last read (not held then)
        const u16 latched = guest::wiimote_take_pressed();
        u32 held = in.buttons | (latched & ~s_hold);
        const bool a_button = (held & guest::WM_A) != 0;
        // (the A button's own press: latched even while a touch holds the game's A)
        guest::s_a_button_pressed = (latched & guest::WM_A) != 0;
        guest::s_a_button_held = ((in.buttons | latched) & guest::WM_A) != 0;
        guest::s_a_from_touch = false;
        // the touch's A, held; it comes down only after a read with A up, so that the A button let
        // go during a touch and the touch's press are two presses, not one
        s_touch_held = in.touching && placed && guest::touch_holds() && (s_touch_held || !(s_hold & guest::WM_A));
        if (s_touch_held) {
            held |= guest::WM_A;
            guest::s_touch_pressed = true;
            guest::s_a_from_touch = !a_button;
        }
        // a tap's, for this read
        if (guest::s_tap_waiting && placed) {
            held |= guest::WM_A;
            guest::s_tap_waiting = false;
            guest::s_a_from_touch = !a_button;
        }
        u32 st = samples;
        for (u32 k = 0; k < KS_SIZE; k += 4)
            guest::store32(st + k, 0);
        guest::store32(st + KS_HOLD, held);
        guest::store32(st + KS_TRIG, held & ~s_hold);
        guest::store32(st + KS_RELEASE, s_hold & ~held);
        store_f32(st + KS_ACC + 4, -1.0f);  // held level, pointing at the screen
        store_f32(st + KS_ACC_VALUE, 1.0f);
        store_f32(st + KS_POS, x), store_f32(st + KS_POS + 4, y);
        store_f32(st + KS_VEC, x - s_x), store_f32(st + KS_VEC + 4, y - s_y);
        store_f32(st + KS_SPEED, std::sqrt((x - s_x) * (x - s_x) + (y - s_y) * (y - s_y)));
        store_f32(st + KS_HORIZON, 1.0f);
        store_f32(st + KS_DIST, 1.3f);
        store_f32(st + KS_ACC_VERTICAL, 1.0f);
        guest::store8(st + KS_DEV_TYPE, WPAD_DEV_CORE);
        guest::store8(st + KS_DPD_VALID, in.pointing ? DPD_TWO_POINTS : 0);
        guest::store8(st + KS_DATA_FORMAT, WPAD_FMT_CORE_ACC_DPD);
        s_hold = held, s_x = x, s_y = y;
        n = 1;
    }
    if (result)
        guest::store32(result, (u32)err);
    return n;
}

// WPADProbe(chan, &type): remote 1 is there (a core remote, no extension), the others aren't
u32 WPADProbe(u32 chan, u32 type) {
    if (chan != 0)
        return (u32)WPAD_ERR_NO_CONTROLLER;
    if (type)
        guest::store32(type, WPAD_DEV_CORE);
    return 0;
}
void WPADDisconnect(u32) {}
u64 WPADControlMotor(u32, u32) { return 0; }  // (no rumble on the 3DS)
// no speaker: the sound library sends it nothing
u32 WPADIsSpeakerEnabled(u32) { return 0; }
u32 WPADSendStreamData(u32, u32, u32) { return (u32)WPAD_ERR_BUSY; }
// ... and nw4r::snd's remote speakers don't run: their alarm (every 6.67 ms, for good) isn't set
void nw4r_snd_detail_RemoteSpeakerManager_Setup(u32 mgr, u32) { guest::store8(mgr, 1); }

}  // namespace g
