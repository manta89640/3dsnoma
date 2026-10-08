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
        u32 st = samples;
        for (u32 k = 0; k < KS_SIZE; k += 4)
            guest::store32(st + k, 0);
        guest::store32(st + KS_HOLD, in.buttons);
        guest::store32(st + KS_TRIG, in.buttons & ~s_hold);
        guest::store32(st + KS_RELEASE, s_hold & ~in.buttons);
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
        s_hold = in.buttons, s_x = x, s_y = y;
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
