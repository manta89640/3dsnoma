// Video interface: registers, the vertical retrace interrupt at 60 Hz and the frame buffer
// address the game shows (the GPU emulation turns the EFB copy into a frame on the 3DS).
#include "hw.h"
#include "../load_stats.h"

namespace guest {

static RegFile s_vi;
static u64 s_next_retrace_us;
static u32 s_line;
static const u64 FRAME_US = 16683;  // 59.94 Hz
static u32 s_game_frames;  // frames the game finished: retraces that show a new frame buffer
static u32 s_shown_xfb;

void vi_init() {
    s_next_retrace_us = host_time_us() + FRAME_US;
}

u32 vi_game_frames() { return s_game_frames; }

u32 vi_current_xfb() {
    // TFBL (0x1C): bit 28 = "address shifted by 5", address in bits 0-23
    u32 t = s_vi.r32(0x1C);
    u32 addr = t & 0x00FFFFFF;
    if (t & 0x10000000)
        addr <<= 5;
    return addr ? 0x80000000 | addr : 0;
}

u32 vi_read(u32 off, int size) {
    switch (off) {
    case 0x2C:  // VCT: current line (2 bytes)
        return (s_line >> 1) + 1;
    case 0x2E:  // HCT
        return 1;
    default:
        return s_vi.read(off, size);
    }
}

void vi_write(u32 off, u32 v, int size) {
    // display interrupt status bits (bit 31 of DI0..DI3) are cleared by writing 0 to them
    if (off >= 0x30 && off < 0x40 && size == 4) {
        u32 old = s_vi.r32(off);
        u32 keep = old & v & 0x80000000;
        s_vi.w32(off, (v & 0x7FFFFFFF) | keep);
    } else if (off >= 0x30 && off < 0x40 && size == 2 && (off & 2) == 0) {
        u32 old = s_vi.r16(off);
        s_vi.w16(off, (u16)((v & 0x7FFF) | (old & v & 0x8000)));
    } else {
        s_vi.write(off, v, size);
    }
    bool any = false;
    for (u32 o = 0x30; o < 0x40; o += 4)
        if (s_vi.r32(o) & 0x80000000)
            any = true;
    if (!any)
        pi_clear(PI_INT_VI);
}

void vi_tick(u64 now_us) {
    if (now_us < s_next_retrace_us) {
        s_line = (u32)((FRAME_US - (s_next_retrace_us - now_us)) * 525 / FRAME_US);
        return;
    }
    while (s_next_retrace_us <= now_us)
        s_next_retrace_us += FRAME_US;
    // DI0 fires at the start of the vertical blank (the SDK sets it up as the retrace interrupt)
    static u32 s_frames;
    s_frames++;
    u32 xfb = vi_current_xfb();
    if (xfb && xfb != s_shown_xfb) {
        s_game_frames++, s_shown_xfb = xfb;
        lstats_frame();
    }
    platform_present(xfb);
    bool any = false;
    for (u32 o = 0x30; o < 0x40; o += 4) {
        u32 di = s_vi.r32(o);
        if (di & 0x10000000) {  // enabled
            s_vi.w32(o, di | 0x80000000);
            pi_raise(PI_INT_VI);
            any = true;
        }
    }
    if (s_frames % 120 == 1)
        log("vi: frame %u (game frame %u), display interrupts %s (DI0 %08X DI1 %08X), xfb %08X", s_frames,
            s_game_frames, any ? "raised" : "disabled", s_vi.r32(0x30), s_vi.r32(0x34), xfb);
}

}  // namespace guest
