// A whole AX frame as the 3DS build compiles it, driven under Unicorn by armbench.py --frame:
// runtime/hw/dsp_ax.cpp runs a command list like the game's (SETUP, PROCESS_PBS over `voices`
// voices like the room's, OUTPUT, END), so the frame's own work (the buses' set-up, the output)
// is counted with the voices' (engine/audio/ax_render.cpp).
#include <cstring>

#include "runtime/runtime.h"

namespace guest {
uintptr_t g_page[1u << (32 - PAGE_BITS)];
u32 g_sp;
FnSlot g_fn_cache[4096];
u32 mmio_read(u32, int) { return 0; }
void mmio_write(u32, u32, int) {}
void* lookup_fn_slow(u32) { return nullptr; }
void fatal(const char*, ...) {
    for (;;) {
    }
}
void log(const char*, ...) {}
void dsp_ax_command_list(u32 list_addr, u32 words);
}  // namespace guest

namespace {

// MEM1's first page: the command list, the set-up and output buffers, the PBs; the samples in MEM2
u8 s_mem1[1u << guest::PAGE_BITS] __attribute__((aligned(32)));
u8 s_mem2[0x40000] __attribute__((aligned(32)));
const u32 LIST = 0x80001000, SETUP = 0x80001200, SURROUND = 0x80001400, LR = 0x80001600, PBS = 0x80002000;
const u32 DATA = 0x90000000;

void st16(u32 a, u16 v) { guest::store16(a, v); }
void st32(u32 a, u32 v) { guest::store32(a, v); }

}  // namespace

// `frames` frames with `voices` voices: looped ADPCM resampled by 1.0-1.3 into main L and R at
// fixed volumes, envelopes fixed (one in five ramping), the master volume at 1.0
extern "C" u32 frame(u32 voices, u32 frames) {
    guest::g_page[0x80000000u >> guest::PAGE_BITS] = (uintptr_t)s_mem1 - 0x80000000u;
    guest::g_page[DATA >> guest::PAGE_BITS] = (uintptr_t)s_mem2 - DATA;
    u32 seed = 1;
    for (u32 i = 0; i < sizeof(s_mem2); i++)
        seed = seed * 1103515245u + 12345u, s_mem2[i] = (u8)(seed >> 16);
    for (u32 v = 0; v < voices; v++) {
        u32 pb = PBS + 0x140 * v;
        for (u32 k = 0; k < 0x140; k += 2)
            st16(pb + k, 0);
        st32(pb + 0x00, v + 1 < voices ? pb + 0x140 : 0);
        st16(pb + 0x10, 1);
        st32(pb + 0x0C, 3);
        st16(pb + 0x14, (u16)(0x2000 + 0x300 * v)), st16(pb + 0x18, (u16)(0x5000 - 0x200 * v));
        st16(pb + 0x6A, (u16)(0x3000 + 0x251 * v));
        if (v % 5 == 4)
            st16(pb + 0x6C, (u16)-3);
        st16(pb + 0x6E, 1);
        u32 start = ((DATA & 0x1FFFFFFF) + 0x1000 * v) * 2 + 2;  // nibbles
        st32(pb + 0x72, start + 0x40), st32(pb + 0x76, start + 0x1E00), st32(pb + 0x7A, start + 0x20 * v);
        for (u32 k = 0; k < 16; k++)
            st16(pb + 0x7E + 2 * k, (u16)(k & 1 ? -1200 + 50 * v : 2400 - 30 * v));
        st32(pb + 0xA6, 0x10000 + 0x1000 * (v % 5));
    }
    for (u32 b = 0; b < 12; b++)  // the buses start at 0 (no depop)
        st16(SETUP + 6 * b, 0), st16(SETUP + 6 * b + 2, 0), st16(SETUP + 6 * b + 4, 0);
    u32 a = LIST;
    auto cmd16 = [&](u16 v) { st16(a, v), a += 2; };
    auto cmd32 = [&](u32 v) { cmd16((u16)(v >> 16)), cmd16((u16)v); };
    cmd16(0x00), cmd32(SETUP);
    cmd16(0x04), cmd32(voices ? PBS : 0);
    cmd16(0x0B), cmd16(0x8000), cmd32(SURROUND), cmd32(LR);
    cmd16(0x0E);
    u32 h = 2166136261u;
    for (u32 f = 0; f < frames; f++) {
        guest::dsp_ax_command_list(LIST, (a - LIST) / 2);
        for (u32 i = 0; i < 96 * 4; i++)
            h = (h ^ s_mem1[(LR - 0x80000000u) + i]) * 16777619u;
    }
    return h;
}
