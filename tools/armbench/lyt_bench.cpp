// engine/lyt/quad.cpp against the decompiled nw4r::lyt quads it replaces, under Unicorn (armbench.py
// --lyt): the decompiled functions (copied from build/decomp/src by the script, renamed dec_*) and
// the natives draw the same random quads; the bytes they send down the write-gather pipe and what
// they return must be the same.
//   run(native, first, trials) -> a hash of it all; count(native, alpha, n_tex, calls): calls alone
#include <cstring>

#include "noma_decomp.h"

namespace guest {
uintptr_t g_page[1u << (32 - PAGE_BITS)];
u32 g_sp;
FnSlot g_fn_cache[4096];
s32 g_loop_budget = 1 << 30;
void loop_poll() { g_loop_budget = 1 << 30; }
void gx_fifo_write(u32, u32 value, int size);
u32 mmio_read(u32, int) { return 0; }
void mmio_write(u32 addr, u32 value, int size) {
    if ((addr & 0x0FFFF000) == 0x0C008000)
        gx_fifo_write(addr & 0xFFF, value, size);
}
void fatal(const char*, ...) {
    for (;;) {
    }
}
void* lookup_fn_slow(u32) { return nullptr; }
alignas(4) u8 g_gather[32];
u32 g_gather_n;
u8 s_sent[1 << 16];
u32 s_sent_n;
void gx_gather_burst() {
    if (s_sent_n + 32 <= sizeof(s_sent))
        memcpy(s_sent + s_sent_n, g_gather, 32), s_sent_n += 32;
    g_gather_n = 0;
}
void gx_fifo_write(u32, u32 value, int size) {
    for (int i = size - 1; i >= 0; i--) {
        g_gather[g_gather_n++] = (u8)(value >> (i * 8));
        if (g_gather_n == 32)
            gx_gather_burst();
    }
}
}  // namespace guest

namespace g {
u32 dec_fn_80464570(u32, u32, u32, u32, u32);
u32 dec_fn_80464AD0(u32, u32, u32, u32, u32, u32);
u32 nw4r_lyt_detail_DrawQuad(u32, u32, u32, u32, u32);
u32 nw4r_lyt_detail_DrawQuad_Alpha(u32, u32, u32, u32, u32, u32);
}  // namespace g

using namespace guest;

extern "C" u8 gx_mem1[0x900000] __attribute__((aligned(32)));
u8 gx_mem1[0x900000] __attribute__((aligned(32)));

namespace {
const u32 MEM1 = 0x80000000, BASE = 0x80810000, SIZE = 0x80810010, TEX = 0x80810100, COLORS = 0x80810400;
u32 s_rand;
u32 rnd() { return s_rand = s_rand * 1103515245u + 12345u, (s_rand >> 16) | (s_rand << 16); }
u32 hash(u32 h, const u8* p, u32 n) {
    for (u32 i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}
// floats like a layout's, now and then any bits at all
u32 rnd_float() {
    if (rnd() % 8 == 0)
        return rnd();
    f32 f = (f32)((s32)(rnd() % 20000) - 10000) / 16.0f;
    u32 b;
    memcpy(&b, &f, 4);
    return b;
}
void setup() {
    for (u32 i = 0; i < 0x900000 >> PAGE_BITS; i++)
        g_page[(MEM1 >> PAGE_BITS) + i] = (uintptr_t)gx_mem1 - MEM1;
    g_sp = 0x80880000;
    // GXBegin's __GXData (all clean: no dirty state to send)
    store32(GA___GXData, 0x80800000);
}
u32 call(u32 native, u32 alpha_variant, u32 n_tex, u32 colors, u32 alpha) {
    if (alpha_variant)
        return native ? g::nw4r_lyt_detail_DrawQuad_Alpha(BASE, SIZE, n_tex, TEX, colors, alpha)
                      : g::dec_fn_80464AD0(BASE, SIZE, n_tex, TEX, colors, alpha);
    return native ? g::nw4r_lyt_detail_DrawQuad(BASE, SIZE, n_tex, TEX, colors)
                  : g::dec_fn_80464570(BASE, SIZE, n_tex, TEX, colors);
}
}  // namespace

extern "C" u32 run(u32 native, u32 first, u32 trials) {
    setup();
    u32 h = 2166136261u;
    for (u32 t = first; t < first + trials; t++) {
        s_rand = t * 104729u + 1;
        for (u32 i = 0; i < 4; i++)
            store32(BASE + 4 * i, rnd_float());
        for (u32 i = 0; i < 0x100 / 4; i++)
            store32(TEX + 4 * i, rnd_float());
        for (u32 i = 0; i < 4; i++)
            store32(COLORS + 4 * i, rnd());
        memset(gx_mem1 + (0x80800000 - MEM1), 0, 0x600);
        s_sent_n = 0, g_gather_n = 0;
        const u32 alpha_variant = rnd() & 1, n_tex = rnd() % 9, colors = rnd() % 4 ? COLORS : 0;
        const u32 alpha = rnd() % 3 == 0 ? 0xFF : rnd() & 0xFF;
        const u32 r = call(native, alpha_variant, n_tex, colors, alpha);
        h = hash(h ^ r, s_sent, s_sent_n);
        h = hash(h, g_gather, g_gather_n);
    }
    return h;
}

extern "C" u32 count(u32 native, u32 alpha_variant, u32 n_tex, u32 calls) {
    setup();
    for (u32 i = 0; i < 0x200; i++)
        gx_mem1[(BASE - MEM1) + i] = (u8)i;
    u32 r = 0;
    for (u32 i = 0; i < calls; i++) {
        s_sent_n = 0;
        r += call(native, alpha_variant, n_tex, COLORS, 0x80);
    }
    return r;
}
