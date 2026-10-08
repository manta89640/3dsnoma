// The Wii Remote, native. The game reads its remotes through KPAD (KPADReadEx, every frame) and
// asks WPAD whether one is connected. On the Wii a whole stack sits underneath: the Bluetooth stack
// (BTE, WUD), WPAD's reports, the remote's speaker stream and rumble, and KPAD working out the
// pointer from the IR camera's view of the sensor bar. Here remote 1 is the 3DS's controls (the
// platform layer's WiimoteInput), always connected and held level; remotes 2-4 aren't there.
// KPADInitEx doesn't start WPAD, so the Bluetooth stack never runs, and nothing goes to a speaker
// (the game's connect callback only sets one up).
#include "engine/input/remote.h"

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
// A tap is a press (the 3DS's touch screen as one, not only a pointer): when the touch ends, the
// game's cursor gets A where it was, as touching then pressing A did. Not in stick mode (a scene
// that walks something about: there A is the scene's, a jump on Wuhu Island).
bool s_was_touching, s_tap;
bool s_stick_dpad;  // input_set_stick_as_dpad
// the bottom screen's region (input_set_touch_region), and the pointer it last gave
bool s_region;
f32 s_region_box[4], s_region_x, s_region_y;
}

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
        s_in.buttons |= (s_in.stick_y > h ? WM_UP : 0) | (s_in.stick_y < -h ? WM_DOWN : 0) |
                        (s_in.stick_x < -h ? WM_LEFT : 0) | (s_in.stick_x > h ? WM_RIGHT : 0);
    }
    s_pressed |= s_in.buttons & ~s_prev_buttons;
    s_prev_buttons = s_in.buttons;
    if (s_region) {
        if (s_in.touching) {
            const f32 lx = s_region_box[0] + (f32)s_in.touch_x / 320.0f * s_region_box[2];
            const f32 ly = s_region_box[1] - (f32)s_in.touch_y / 240.0f * s_region_box[3];
            s_region_x = lx / 304.0f, s_region_y = -ly / 228.0f;
        }
        s_in.x = s_region_x, s_in.y = s_region_y;
    }
    if (s_was_touching && !s_in.touching && (!s_stick_mode || s_region))
        s_tap = true;
    s_was_touching = s_in.touching;
}

void input_set_stick_as_dpad(bool on) { s_stick_dpad = on; }

void input_set_touch_region(bool on, f32 left, f32 top, f32 width, f32 height) {
    s_region = on;
    s_region_box[0] = left, s_region_box[1] = top, s_region_box[2] = width, s_region_box[3] = height;
    s_region_x = (left + width / 2) / 304.0f, s_region_y = -(top - height / 2) / 228.0f;
    s_tap = false;
}

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
const WiimoteInput& wiimote_input() { return s_in; }
void input_set_stick_mode(bool on) { s_stick_mode = on; }
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
// A tap's A, held for one read: once the pointer has been on the screen for TAP_READY reads (the
// game's cursor (layout_Cursor_Update) takes no press for 10 frames after the pointer comes back,
// as a remote's pointer settles); a tap before then waits for it.
const u32 TAP_READY = 11;
u32 s_pointing_reads;  // reads in a row with the pointer on the screen
bool s_tap_waiting;    // a tap, its A not given yet
bool s_tap_given;      // the last read's A was a tap's

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
        s_pointing_reads = in.pointing ? s_pointing_reads + 1 : 0;
        if (guest::wiimote_take_tap())
            s_tap_waiting = in.pointing;
        if (!in.pointing || (s_hold & guest::WM_A))
            s_tap_waiting = false;  // (the pointer gone, or A held anyway: no press later on)
        // what is held now, with what was pressed and let go since the last read (not held then)
        u32 held = in.buttons | (guest::wiimote_take_pressed() & ~s_hold);
        s_tap_given = false;
        if (s_tap_waiting && s_pointing_reads >= TAP_READY) {
            held |= guest::WM_A;
            s_tap_waiting = false;
            s_tap_given = !(in.buttons & guest::WM_A);
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

namespace guest {
bool wiimote_last_read_tapped() { return g::s_tap_given; }
}  // namespace guest
