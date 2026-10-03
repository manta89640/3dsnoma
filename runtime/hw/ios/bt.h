// The emulated Bluetooth controller (bt.cpp) and the Wii Remote behind it (wiimote.cpp).
#pragma once
#include <memory>
#include <vector>

#include "ios.h"

namespace guest {

// the remote's address, in HCI byte order (least significant byte first)
extern const u8 WIIMOTE_BDADDR[6];
// the link key the remote and console agreed on when they were paired
extern const u8 WIIMOTE_LINK_KEY[16];

std::unique_ptr<IosHandle> bt_open_device();
void bt_tick(u64 now_us);

// ---- wiimote.cpp: the remote's side of the HID protocol (reports without the 0xA1/0xA2 byte)
// An output report from the console; replies (acks, status, memory reads) are appended.
void wiimote_handle_output(const u8* report, u16 size, std::vector<std::vector<u8>>& replies);
// The next data report for the current reporting mode, if one is due.
bool wiimote_poll_report(std::vector<u8>& report);

// What the player is doing, set by the platform layer (3DS buttons/touch, or a test script).
struct WiimoteInput {
    u16 buttons = 0;           // WPAD button bits as the remote reports them (see wiimote.cpp)
    bool pointing = true;      // the sensor bar is in view
    float x = 0.0f, y = 0.0f;  // where it points: -1..1 across the screen, y down
    float accel[3] = {0.0f, 0.0f, 1.0f};  // in g, remote axes (x left, y forward, z up)
};
void wiimote_set_input(const WiimoteInput& in);
// NOMA_INPUT (input_script.cpp): is there a script; merge it into `in` (on the game's clock)
bool input_script_active();
void input_script_apply(WiimoteInput& in);

// button bits in the remote's core button report (byte 0 high, byte 1 low)
enum : u16 {
    WM_LEFT = 0x0100,
    WM_RIGHT = 0x0200,
    WM_DOWN = 0x0400,
    WM_UP = 0x0800,
    WM_PLUS = 0x1000,
    WM_TWO = 0x0001,
    WM_ONE = 0x0002,
    WM_B = 0x0004,
    WM_A = 0x0008,
    WM_MINUS = 0x0010,
    WM_HOME = 0x0080,
};

}  // namespace guest
