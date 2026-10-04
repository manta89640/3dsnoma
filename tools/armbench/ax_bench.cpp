// engine/audio/ax_render.cpp as the 3DS build compiles it, driven under Unicorn by armbench.py:
// voices like the game's for instruction counts (count), and random voices against the
// sample-by-sample definition (exact), which reaches the 3DS-only paths (the ARM11's DSP
// instructions) that NOMA_SELFTEST=ax on the host can't.
#include <cstring>

#include "engine/audio/ax_reference.h"
#include "engine/audio/ax_render.h"

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
}  // namespace guest

namespace {

namespace ref = engine::audio::reference;

// guest memory: the PBs at 0x80000000 on, the random voices' samples from 0x90FF8000 across the
// page boundary at 0x91000000 (two buffers that aren't next to each other, as pages needn't be).
// The upper page is backed whole: a voice whose end address is skipped (an end on an ADPCM frame's
// header) reads on past the data, and must find the same bytes there for both renderers
const u32 DATA = 0x90FF8000, DATA_SIZE = 0x10000;
u8 s_pbs[0x40000] __attribute__((aligned(32)));
u8 s_lo[0x8000], s_hi[1u << guest::PAGE_BITS];
engine::audio::AxBuses s_bus;
s32 s_ref_bus[12][ref::SPF];

void map() {
    guest::g_page[0x80000000u >> guest::PAGE_BITS] = (uintptr_t)s_pbs - 0x80000000u;
    guest::g_page[DATA >> guest::PAGE_BITS] = (uintptr_t)s_lo - DATA;
    guest::g_page[(DATA + sizeof(s_lo)) >> guest::PAGE_BITS] = (uintptr_t)s_hi - (DATA + sizeof(s_lo));
}

void st16(u32 a, u16 v) { guest::store16(a, v); }
void st32(u32 a, u32 v) { guest::store32(a, v); }

}  // namespace

// one voice for `frames` frames (kind: 0 the room's, looped ADPCM resampled by 1.12 into main L
// and R at an envelope of 0x48EF; 1 the same at 1.0; 2 PCM16; 3 L, R and the envelope ramping;
// 4 muted; 5 main and aux A). With `hash`, an FNV-1a hash of every bus sample and the PB after
// each frame.
extern "C" u32 count(u32 kind, u32 frames, u32 hash) {
    map();
    u32 seed = 1;
    for (u32 i = 0x1000; i < sizeof(s_pbs); i++)
        seed = seed * 1103515245u + 12345u, s_pbs[i] = (u8)(seed >> 16);
    const u32 pb = 0x80000100;
    for (u32 k = 0; k < 0xD6; k += 2)
        st16(pb + k, 0);
    st16(pb + 0x10, 1);
    st32(pb + 0x0C, kind == 5 ? 0x30003 : kind == 3 ? 4 : 3);
    st16(pb + 0x14, 0x6000), st16(pb + 0x18, 0x5000), st16(pb + 0x1C, 0x2000), st16(pb + 0x20, 0x2000);
    if (kind == 3)
        st16(pb + 0x16, 3), st16(pb + 0x1A, (u16)-2), st16(pb + 0x6C, 1);
    st16(pb + 0x6A, kind == 1 ? 0x8000 : kind == 4 ? 0 : 0x48EF);
    st16(pb + 0x6E, 1);
    if (kind == 2) {
        st16(pb + 0x70, 0x0A);
        st32(pb + 0x72, 0x800), st32(pb + 0x76, 0x1F000), st32(pb + 0x7A, 0x800);
    } else {
        st32(pb + 0x72, 0x2002), st32(pb + 0x76, 0x7F000), st32(pb + 0x7A, 0x2002);
        for (u32 k = 0; k < 16; k++)
            st16(pb + 0x7E + 2 * k, (u16)(k & 1 ? -1200 : 2400));
    }
    st32(pb + 0xA6, 0x11EF0);
    u32 h = 2166136261u;
    for (u32 f = 0; f < frames; f++) {
        engine::audio::ax_render_voice(pb, s_bus);
        if (!hash)
            continue;
        for (u32 b = 0; b < 12; b++)
            for (u32 i = 0; i < 96; i++)
                h = (h ^ (u32)s_bus[b][i]) * 16777619u;
        for (u32 k = 0; k < 0xD6; k++)
            h = (h ^ guest::load8(pb + k)) * 16777619u;
    }
    return h;
}

u32 g_first_bad;  // the first frame that differed, as voice * 4 + frame, plus 1 (0: none)

// `voices` random voices, four frames each, rendered and defined: the frames that differ
extern "C" u32 exact(u32 voices) {
    g_first_bad = 0;
    map();
    const u32 pb_ref = 0x80000100, pb_nat4 = 0x80000400;
    ref::Rng rnd{12345};
    ref::test_data(rnd, DATA, DATA_SIZE);
    u32 bad = 0;
    for (u32 t = 0; t < voices; t++) {
        u16 w[0xD6 / 2];
        if (!ref::edge_voice(t, w, DATA))
            ref::random_voice(rnd, w, DATA, DATA_SIZE);
        // (every fourth voice's PB 2-byte aligned only: rendered from an aligned copy)
        const u32 pb_nat = t % 4 == 3 ? pb_nat4 + 0x202 : pb_nat4;
        for (u32 k = 0; k < 0xD6 / 2; k++)
            st16(pb_ref + 2 * k, w[k]), st16(pb_nat + 2 * k, w[k]);
        for (u32 f = 0; f < 4; f++) {
            memset(s_ref_bus, 0, sizeof(s_ref_bus));
            memset(s_bus, 0, sizeof(s_bus));
            ref::Pb pb;
            pb.read(pb_ref);
            ref::process_voice(pb, s_ref_bus);
            pb.write();
            engine::audio::ax_render_voice(pb_nat, s_bus);
            bool same = memcmp(s_ref_bus, s_bus, sizeof(s_bus)) == 0;
            for (u32 k = 0; k < 0xD6 && same; k += 2)
                same = guest::load16(pb_ref + k) == guest::load16(pb_nat + k);
            if (!same && !g_first_bad)
                g_first_bad = t * 4 + f + 1;
            bad += !same;
        }
    }
    return bad;
}
