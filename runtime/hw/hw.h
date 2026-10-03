// Emulated Wii hardware below the decompiled SDK.
#pragma once
#include "../runtime.h"

namespace guest {

void mmio_log_unhandled(const char* dev, u32 off, u32 v, int size, bool write);
u32 lc_read(u32 addr, int size);
void lc_write(u32 addr, u32 value, int size);

#define DEVICE(name)                    \
    u32 name##_read(u32 off, int size); \
    void name##_write(u32 off, u32 value, int size);
DEVICE(cp)
DEVICE(pe)
DEVICE(vi)
DEVICE(pi)
DEVICE(mi)
DEVICE(dsp)
DEVICE(di)
DEVICE(si)
DEVICE(exi)
DEVICE(ai)
DEVICE(hollywood)
#undef DEVICE
void gx_fifo_write(u32 off, u32 value, int size);

// register file helper for devices that mostly just remember what was written
struct RegFile {
    u8 bytes[0x400] = {};
    u32 read(u32 off, int size) const {
        u32 v = 0;
        for (int i = 0; i < size; i++)
            v = (v << 8) | bytes[(off + i) & 0x3FF];
        return v;
    }
    void write(u32 off, u32 value, int size) {
        for (int i = size - 1; i >= 0; i--) {
            bytes[(off + i) & 0x3FF] = (u8)value;
            value >>= 8;
        }
    }
    u32 r32(u32 off) const { return read(off, 4); }
    u16 r16(u32 off) const { return (u16)read(off, 2); }
    void w32(u32 off, u32 v) { write(off, v, 4); }
    void w16(u32 off, u16 v) { write(off, v, 2); }
};

// devices with periodic work
void vi_init();
void vi_tick(u64 now_us);
u32 vi_current_xfb();
u32 vi_game_frames();  // frames the game has finished so far (new frame buffers shown)
void ipc_init();
void exi_init();
s64 rtc_seconds();  // the console's clock: local time in seconds since 2000-01-01
void dsp_tick(u64 now_us);
void gpu_init();
void gx_poll();  // from the device polls: the GPU's thread gets new commands, its events come back

}  // namespace guest
