// The Wii Remote's side of its HID protocol (what WPAD talks to; reports as on wiibrew.org):
//   output 0x10 rumble, 0x11 LEDs, 0x12 reporting mode, 0x13/0x1A IR camera, 0x14/0x19 speaker,
//          0x15 status request, 0x16 write memory/registers, 0x17 read, 0x18 speaker data
//   input  0x20 status, 0x21 read data, 0x22 acknowledge, 0x30-0x3F data reports
// The first byte of every output report carries rumble (bit 0); bit 1 asks for an
// acknowledgement where one is optional. No extension (Nunchuk, Motion Plus) is attached.
// The IR camera sees the two sensor bar clusters where the player points.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bt.h"

namespace guest {

const u8 WIIMOTE_BDADDR[6] = {0x11, 0x02, 0x19, 0x79, 0x00, 0x00};
const u8 WIIMOTE_LINK_KEY[16] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18,
                                 0x29, 0x3A, 0x4B, 0x5C, 0x6D, 0x7E, 0x8F, 0x90};

namespace {

// accelerometer calibration: zero at 0x200 (10 bits), one g = 0x68 more
const int ACC_ZERO = 0x80 << 2, ACC_ONE_G = (0x9A - 0x80) << 2;

struct Wiimote {
    u8 leds = 0;
    bool rumble = false;
    u8 mode = 0x30;
    bool continuous = false;
    bool ir_clock = false, ir_enabled = false;
    bool speaker = false, speaker_muted = false;
    u8 eeprom[0x1700];
    u8 reg_ir[0x100] = {};
    u8 reg_speaker[0x100] = {};
    WiimoteInput in;
    std::vector<u8> last_report;
    bool status_due = false;

    Wiimote() {
        memset(eeprom, 0, sizeof(eeprom));
        // 0x00: IR calibration (two copies); 0x16: accelerometer calibration (two copies)
        static const u8 ir[11] = {0xA1, 0xAA, 0x8B, 0x99, 0xAE, 0x9E, 0x78, 0x30, 0xA7, 0x74, 0xD3};
        memcpy(eeprom + 0x00, ir, 11);
        memcpy(eeprom + 0x0B, ir, 11);
        u8 acc[10] = {0x80, 0x80, 0x80, 0, 0x9A, 0x9A, 0x9A, 0, 0x40, 0};
        u32 sum = 0;
        for (int i = 0; i < 9; i++)
            sum += acc[i];
        acc[9] = (u8)(sum + 0x55);
        memcpy(eeprom + 0x16, acc, 10);
        memcpy(eeprom + 0x20, acc, 10);
    }
};

Wiimote s_wm;

void put_buttons(std::vector<u8>& r, int acc10[3] = nullptr) {
    u16 b = s_wm.in.buttons & 0x1F9F;
    u8 hi = (u8)(b >> 8), lo = (u8)b;
    if (acc10) {
        hi |= (u8)((acc10[0] & 3) << 5);
        lo |= (u8)(((acc10[1] >> 1) & 1) << 5) | (u8)(((acc10[2] >> 1) & 1) << 6);
    }
    r.push_back(hi);
    r.push_back(lo);
}

void accel10(int out[3]) {
    for (int i = 0; i < 3; i++) {
        int v = ACC_ZERO + (int)lrintf(s_wm.in.accel[i] * ACC_ONE_G);
        out[i] = v < 0 ? 0 : v > 0x3FF ? 0x3FF : v;
    }
}

// The two sensor bar clusters as the camera (1024x768) sees them when the remote points at
// (x, y): pointing right moves them left in the picture, pointing down moves them down (the
// camera's y runs the other way). NOMA_IR="scale_x,scale_y,offset_x,offset_y" overrides the
// mapping, which is calibrated so that (x, y) lands on the same spot of the game's 640x456 screen
// (KPAD's pointer is not centred on the camera's centre).
void ir_dots(int dx[2], int dy[2], bool& visible) {
    visible = s_wm.in.pointing && s_wm.ir_enabled;
    const float half_spread = 100.0f;  // ~ the bar's width seen from a couple of metres
    static float sx = 236.0f, sy = 234.5f, ox = 30.5f, oy = 122.5f;
    static bool init;
    if (!init) {
        init = true;
        if (const char* e = getenv("NOMA_IR"))
            sscanf(e, "%f,%f,%f,%f", &sx, &sy, &ox, &oy);
    }
    float cx = 512.0f + ox - s_wm.in.x * sx;
    float cy = 384.0f + oy + s_wm.in.y * sy;
    dx[0] = (int)lrintf(cx - half_spread);
    dx[1] = (int)lrintf(cx + half_spread);
    dy[0] = dy[1] = (int)lrintf(cy);
    for (int i = 0; i < 2; i++)
        if (dx[i] < 0 || dx[i] > 1023 || dy[i] < 0 || dy[i] > 767)
            visible = false;
}

void put_ir_extended(std::vector<u8>& r) {  // 4 x 3 bytes
    int dx[2], dy[2];
    bool vis;
    ir_dots(dx, dy, vis);
    for (int i = 0; i < 4; i++) {
        if (!vis || i >= 2) {
            r.insert(r.end(), {0xFF, 0xFF, 0xFF});
            continue;
        }
        r.push_back((u8)dx[i]);
        r.push_back((u8)dy[i]);
        r.push_back((u8)(((dy[i] >> 8) & 3) << 6 | ((dx[i] >> 8) & 3) << 4 | 3));  // size 3
    }
}

void put_ir_basic(std::vector<u8>& r) {  // 2 x 5 bytes
    int dx[2], dy[2];
    bool vis;
    ir_dots(dx, dy, vis);
    if (!vis) {
        r.insert(r.end(), 10, 0xFF);
        return;
    }
    r.push_back((u8)dx[0]);
    r.push_back((u8)dy[0]);
    r.push_back((u8)(((dy[0] >> 8) & 3) << 6 | ((dx[0] >> 8) & 3) << 4 | ((dy[1] >> 8) & 3) << 2 |
                     ((dx[1] >> 8) & 3)));
    r.push_back((u8)dx[1]);
    r.push_back((u8)dy[1]);
    r.insert(r.end(), 5, 0xFF);
}

std::vector<u8> status_report() {
    std::vector<u8> r{0x20};
    put_buttons(r);
    u8 flags = (u8)(s_wm.leds << 4);
    if (s_wm.speaker)
        flags |= 0x04;
    if (s_wm.ir_enabled)
        flags |= 0x08;
    r.push_back(flags);
    r.push_back(0);
    r.push_back(0);
    r.push_back(0xC8);  // battery: full
    return r;
}

std::vector<u8> ack(u8 report, u8 error) {
    std::vector<u8> r{0x22};
    put_buttons(r);
    r.push_back(report);
    r.push_back(error);
    return r;
}

enum : u8 { ERR_OK = 0, ERR_NACK = 7, ERR_ADDR = 8 };

// register spaces (address >> 16): 0xA2 speaker, 0xA4 extension, 0xA6 Motion Plus, 0xB0 IR
u8* reg_space(u32 addr) {
    switch ((addr >> 16) & 0xFE) {
    case 0xA2: return s_wm.reg_speaker;
    case 0xB0: return s_wm.reg_ir;
    default: return nullptr;  // nothing plugged in
    }
}

void write_data(const u8* p, u16 n, std::vector<std::vector<u8>>& out) {
    if (n < 6)
        return;
    u8 space = (p[0] >> 2) & 3;
    u32 addr = (u32)(p[1] << 16 | p[2] << 8 | p[3]);
    u8 size = p[4] > 16 ? 16 : p[4];
    const u8* data = p + 5;
    u8 err = ERR_OK;
    if (space == 0) {
        if (addr + size > sizeof(s_wm.eeprom))
            err = ERR_ADDR;
        else
            memcpy(s_wm.eeprom + addr, data, size);
    } else {
        u8* regs = reg_space(addr);
        if (!regs) {
            err = ERR_NACK;
        } else {
            for (u32 i = 0; i < size; i++)
                regs[(addr + i) & 0xFF] = data[i];
            if ((addr >> 16) == 0xB0 && (addr & 0xFF) <= 0x30 && (addr & 0xFF) + size > 0x30)
                s_wm.ir_enabled = s_wm.ir_clock && (s_wm.reg_ir[0x30] & 0x08);
        }
    }
    out.push_back(ack(0x16, err));
}

void read_data(const u8* p, u16 n, std::vector<std::vector<u8>>& out) {
    if (n < 6)
        return;
    u8 space = (p[0] >> 2) & 3;
    u32 addr = (u32)(p[1] << 16 | p[2] << 8 | p[3]);
    u32 size = (u32)(p[4] << 8 | p[5]);
    auto reply = [&](u32 at, const u8* data, u32 len, u8 err) {
        std::vector<u8> r{0x21};
        put_buttons(r);
        r.push_back((u8)(((len ? len - 1 : 0xF) << 4) | err));
        r.push_back((u8)(at >> 8));
        r.push_back((u8)at);
        for (u32 i = 0; i < 16; i++)
            r.push_back(i < len ? data[i] : 0);
        out.push_back(r);
    };
    if (space == 0) {
        if (addr + size > sizeof(s_wm.eeprom)) {
            reply(addr, nullptr, 0, ERR_ADDR);
            return;
        }
        for (u32 o = 0; o < size; o += 16) {
            u32 len = size - o < 16 ? size - o : 16;
            reply(addr + o, s_wm.eeprom + addr + o, len, ERR_OK);
        }
        return;
    }
    u8* regs = reg_space(addr);
    if (!regs) {
        reply(addr, nullptr, 0, ERR_NACK);
        return;
    }
    for (u32 o = 0; o < size; o += 16) {
        u8 chunk[16];
        u32 len = size - o < 16 ? size - o : 16;
        for (u32 i = 0; i < len; i++)
            chunk[i] = regs[(addr + o + i) & 0xFF];
        reply(addr + o, chunk, len, ERR_OK);
    }
}

}  // namespace

void wiimote_handle_output(const u8* rep, u16 n, std::vector<std::vector<u8>>& out) {
    if (n < 2)
        return;
    u8 id = rep[0];
    const u8* p = rep + 1;
    bool want_ack = (p[0] & 2) != 0;
    s_wm.rumble = p[0] & 1;
    static int s_logged;
    if (s_logged++ < 60)
        log("wiimote: output report %02X (%u bytes) %02X %02X %02X", id, n - 1, p[0],
            n > 2 ? p[1] : 0, n > 3 ? p[2] : 0);
    switch (id) {
    case 0x10:  // rumble only
        break;
    case 0x11:
        s_wm.leds = p[0] >> 4;
        if (want_ack)
            out.push_back(ack(id, ERR_OK));
        break;
    case 0x12:
        s_wm.continuous = (p[0] & 4) != 0;
        if (n >= 3)
            s_wm.mode = p[1];
        s_wm.last_report.clear();
        if (want_ack)
            out.push_back(ack(id, ERR_OK));
        break;
    case 0x13:
    case 0x1A:
        if (id == 0x13)
            s_wm.ir_clock = (p[0] & 4) != 0;
        else
            s_wm.ir_enabled = (p[0] & 4) != 0;
        if (want_ack)
            out.push_back(ack(id, ERR_OK));
        break;
    case 0x14:
        if (s_wm.speaker != ((p[0] & 4) != 0))
            log("wiimote: speaker %s", (p[0] & 4) ? "on" : "off");
        s_wm.speaker = (p[0] & 4) != 0;
        if (want_ack)
            out.push_back(ack(id, ERR_OK));
        break;
    case 0x19:
        s_wm.speaker_muted = (p[0] & 4) != 0;
        if (want_ack)
            out.push_back(ack(id, ERR_OK));
        break;
    case 0x15:
        out.push_back(status_report());
        break;
    case 0x16:
        write_data(p, n - 1, out);
        break;
    case 0x17:
        read_data(p, n - 1, out);
        break;
    case 0x18: {  // speaker samples: nowhere to play them yet
        static u32 s_reports;
        if (s_reports++ % 1000 == 0)
            log("wiimote: speaker data (%u reports so far)", s_reports);
        break;
    }
    default:
        log("wiimote: unknown output report %02X", id);
        out.push_back(ack(id, ERR_NACK));
        break;
    }
}

bool wiimote_poll_report(std::vector<u8>& r) {
    r.clear();
    r.push_back(s_wm.mode);
    int acc[3];
    accel10(acc);
    switch (s_wm.mode) {
    case 0x30:
        put_buttons(r);
        break;
    case 0x31:
        put_buttons(r, acc);
        r.insert(r.end(), {(u8)(acc[0] >> 2), (u8)(acc[1] >> 2), (u8)(acc[2] >> 2)});
        break;
    case 0x32:
        put_buttons(r);
        r.insert(r.end(), 8, 0xFF);
        break;
    case 0x33:
        put_buttons(r, acc);
        r.insert(r.end(), {(u8)(acc[0] >> 2), (u8)(acc[1] >> 2), (u8)(acc[2] >> 2)});
        put_ir_extended(r);
        break;
    case 0x34:
        put_buttons(r);
        r.insert(r.end(), 19, 0xFF);
        break;
    case 0x35:
        put_buttons(r, acc);
        r.insert(r.end(), {(u8)(acc[0] >> 2), (u8)(acc[1] >> 2), (u8)(acc[2] >> 2)});
        r.insert(r.end(), 16, 0xFF);
        break;
    case 0x36:
        put_buttons(r);
        put_ir_basic(r);
        r.insert(r.end(), 9, 0xFF);
        break;
    case 0x37:
        put_buttons(r, acc);
        r.insert(r.end(), {(u8)(acc[0] >> 2), (u8)(acc[1] >> 2), (u8)(acc[2] >> 2)});
        put_ir_basic(r);
        r.insert(r.end(), 6, 0xFF);
        break;
    case 0x3D:
        r.insert(r.end(), 21, 0xFF);
        break;
    default:
        return false;
    }
    if (!s_wm.continuous && r == s_wm.last_report)
        return false;
    s_wm.last_report = r;
    return true;
}

void wiimote_set_input(const WiimoteInput& in) { s_wm.in = in; }

}  // namespace guest
