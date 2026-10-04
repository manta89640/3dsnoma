// The AX voice pipeline sample by sample, as runtime/hw/dsp_ax.cpp first emulated the microcode:
// the definition that engine/audio/ax_render.cpp's block renderer has to match, bit for bit. Only
// tests use it: NOMA_SELFTEST=ax (platform/host/selftest.cpp) on the host, and tools/armbench on
// the code the 3DS runs (its DSP instructions), both on random_voice()'s voices.
#pragma once

#include <algorithm>

#include "noma_decomp.h"

namespace engine::audio::reference {

const u32 SPF = 96;
enum { MAIN_L, MAIN_R, MAIN_S, AUXA_L, AUXA_R, AUXA_S, AUXB_L, AUXB_R, AUXB_S, AUXC_L, AUXC_R, AUXC_S };
inline s16 clamp16(s32 v) { return (s16)std::min<s32>(32767, std::max<s32>(-32768, v)); }
struct Pb {
    u16 w[0xD6 / 2];  // through the biquad (the remote part from 0xD6 on is left alone)
    u32 addr;
    void read(u32 a) {
        addr = a;
        for (u32 i = 0; i < sizeof(w) / 2; i++)
            w[i] = guest::load16(a + 2 * i);
    }
    void write() const {
        for (u32 i = 0; i < sizeof(w) / 2; i++)
            guest::store16(addr + 2 * i, w[i]);
    }
    u16& at(u32 byte_off) { return w[byte_off / 2]; }
    u32 get32(u32 off) const { return (u32)w[off / 2] << 16 | w[off / 2 + 1]; }
    void set32(u32 off, u32 v) {
        w[off / 2] = (u16)(v >> 16);
        w[off / 2 + 1] = (u16)v;
    }
};

enum : u32 {
    PB_NEXT = 0x00, PB_SRC_TYPE = 0x08, PB_MIXCTRL = 0x0C, PB_RUNNING = 0x10, PB_STREAM = 0x12,
    PB_MIX = 0x14, PB_DPOP = 0x52, PB_VE = 0x6A, PB_LOOPFLAG = 0x6E, PB_FORMAT = 0x70,
    PB_LOOP = 0x72, PB_END = 0x76, PB_CUR = 0x7A, PB_COEFS = 0x7E, PB_PRED = 0xA0, PB_YN1 = 0xA2,
    PB_YN2 = 0xA4, PB_RATIO = 0xA6, PB_FRAC = 0xAA, PB_LAST = 0xAC, PB_LOOP_PRED = 0xB4,
    PB_LOOP_YN1 = 0xB6, PB_LOOP_YN2 = 0xB8, PB_LPF = 0xBA, PB_BIQUAD = 0xC2,
};
enum : u16 { FMT_ADPCM = 0x00, FMT_PCM16 = 0x0A, FMT_PCM8 = 0x19 };

// ---------------------------------------------------------------- the accelerator
// Addresses count nibbles for ADPCM, 16-bit samples for PCM16 and bytes for PCM8, from physical 0.
struct Accel {
    Pb& pb;
    u32 cur, end, loop;
    u16 fmt, pred;
    s16 yn1, yn2;
    bool stopped = false;

    explicit Accel(Pb& p) : pb(p) {
        cur = p.get32(PB_CUR);
        end = p.get32(PB_END);
        loop = p.get32(PB_LOOP);
        fmt = p.at(PB_FORMAT);
        pred = p.at(PB_PRED);
        yn1 = (s16)p.at(PB_YN1);
        yn2 = (s16)p.at(PB_YN2);
    }

    static u8 byte_at(u32 phys) { return guest::load8(0x80000000u | (phys & 0x1FFFFFFF)); }

    s16 sample() {
        if (stopped)
            return 0;
        s16 s;
        switch (fmt) {
        case FMT_ADPCM: {
            if ((cur & 15) == 0) {  // a frame header: predictor and scale for 14 samples
                pred = byte_at(cur >> 1);
                cur += 2;
            }
            u8 b = byte_at(cur >> 1);
            s32 nib = (cur & 1) ? (b & 15) : (b >> 4);
            if (nib >= 8)
                nib -= 16;
            u32 idx = (pred >> 4) & 7;
            s32 c1 = (s16)pb.at(PB_COEFS + idx * 4), c2 = (s16)pb.at(PB_COEFS + idx * 4 + 2);
            s32 v = nib * (1 << (pred & 15)) * 2048 + c1 * yn1 + c2 * yn2;
            s = clamp16((v + 0x400) >> 11);
            yn2 = yn1;
            yn1 = s;
            break;
        }
        case FMT_PCM16:
            s = (s16)guest::load16(0x80000000u | ((cur * 2) & 0x1FFFFFFF));
            break;
        case FMT_PCM8:
            s = (s16)((s8)byte_at(cur) << 8);
            break;
        default:
            s = 0;
            break;
        }
        if (cur == end) {  // the end address is inclusive
            if (pb.at(PB_LOOPFLAG)) {
                cur = loop;
                if (!pb.at(PB_STREAM)) {  // a looped sound restarts with its saved context
                    pred = pb.at(PB_LOOP_PRED);
                    yn1 = (s16)pb.at(PB_LOOP_YN1);
                    yn2 = (s16)pb.at(PB_LOOP_YN2);
                }
            } else {
                stopped = true;
                pb.at(PB_RUNNING) = 0;
            }
        } else {
            cur++;
        }
        return s;
    }

    void save() {
        pb.set32(PB_CUR, cur);
        pb.at(PB_PRED) = pred;
        pb.at(PB_YN1) = (u16)yn1;
        pb.at(PB_YN2) = (u16)yn2;
    }
};

// resample to SPF output samples: the position runs in 16.16 steps of `ratio`
void input_samples(Pb& pb, s16* out) {
    Accel acc(pb);
    u32 ratio = pb.get32(PB_RATIO);
    u32 pos = pb.at(PB_FRAC);
    s16 hist[4];
    for (int i = 0; i < 4; i++)
        hist[i] = (s16)pb.at(PB_LAST + 2 * i);
    u32 type = pb.at(PB_SRC_TYPE);
    for (u32 i = 0; i < SPF; i++) {
        if (type == 2) {  // no conversion: one input sample per output sample
            hist[0] = hist[1], hist[1] = hist[2], hist[2] = hist[3], hist[3] = acc.sample();
            out[i] = hist[3];
            continue;
        }
        pos += ratio;
        while (pos >= 0x10000) {
            hist[0] = hist[1], hist[1] = hist[2], hist[2] = hist[3], hist[3] = acc.sample();
            pos -= 0x10000;
        }
        u32 frac = pos & 0xFFFF;
        // between the second- and third-newest samples, as the 4-tap filter is centred
        out[i] = (s16)(((s64)hist[1] * (0x10000 - frac) + (s64)hist[2] * frac) >> 16);
    }
    pb.at(PB_FRAC) = (u16)(pos & 0xFFFF);
    for (int i = 0; i < 4; i++)
        pb.at(PB_LAST + 2 * i) = (u16)hist[i];
    acc.save();
}

// add `in` to a buffer at the volume at mix word `vol_off` (ramping by its delta if `ramp`);
// the last value goes to the depop word `dpop_off`
void mix_add(Pb& pb, s32* out, const s16* in, u32 vol_off, u32 dpop_off, bool ramp) {
    u16 vol = pb.at(vol_off);
    u16 delta = ramp ? pb.at(vol_off + 2) : 0;
    s16 last = 0;
    for (u32 i = 0; i < SPF; i++) {
        last = clamp16((s32)(((s64)in[i] * vol) >> 15));
        out[i] += last;
        vol += delta;
    }
    pb.at(vol_off) = vol;
    pb.at(dpop_off) = (u16)last;
}

void process_voice(Pb& pb, s32 (*s_buf)[SPF]) {
    if (pb.at(PB_RUNNING) != 1)
        return;
    s16 smp[SPF];
    input_samples(pb, smp);
    // volume envelope (unsigned on the Wii)
    u16 ve = pb.at(PB_VE);
    s16 ved = (s16)pb.at(PB_VE + 2);
    for (u32 i = 0; i < SPF; i++) {
        smp[i] = clamp16(((s32)smp[i] * (s32)ve) >> 15);
        ve = (u16)(ve + ved);
    }
    pb.at(PB_VE) = ve;

    if (pb.at(PB_LPF)) {  // one-pole low-pass
        s32 a0 = pb.at(PB_LPF + 4), b0 = (s16)pb.at(PB_LPF + 6);
        s16 y = (s16)pb.at(PB_LPF + 2);
        for (u32 i = 0; i < SPF; i++)
            y = smp[i] = clamp16((a0 * smp[i] + b0 * y) >> 15);
        pb.at(PB_LPF + 2) = (u16)y;
    }
    if (pb.at(PB_BIQUAD)) {  // coefficients in 2.14
        auto c = [&](u32 k) { return (s64)(s16)pb.at(PB_BIQUAD + 2 * k); };
        s16 xn1 = (s16)pb.at(PB_BIQUAD + 2), xn2 = (s16)pb.at(PB_BIQUAD + 4);
        s16 yn1 = (s16)pb.at(PB_BIQUAD + 6), yn2 = (s16)pb.at(PB_BIQUAD + 8);
        for (u32 i = 0; i < SPF; i++) {
            s16 x = smp[i];
            s64 t = (c(5) * x + c(6) * xn1 + c(7) * xn2 + c(8) * yn1 + c(9) * yn2) << 2;
            t += (t & 0x10000) ? 0x8000 : 0x7FFF;
            s16 y = clamp16((s32)(t >> 16));
            xn2 = xn1, xn1 = x, yn2 = yn1, yn1 = y;
            smp[i] = y;
        }
        pb.at(PB_BIQUAD + 2) = (u16)xn1, pb.at(PB_BIQUAD + 4) = (u16)xn2;
        pb.at(PB_BIQUAD + 6) = (u16)yn1, pb.at(PB_BIQUAD + 8) = (u16)yn2;
    }

    // mixer control: for each bus, L and R bits, a ramp bit for both, S and its ramp bit
    u32 mc = pb.get32(PB_MIXCTRL);
    struct Bus {
        u32 shift;  // bit of L in the mixer control word
        int l, r, s;
        u32 mix_l, mix_r, mix_s;  // offsets of (volume, delta) in the PB
        u32 dpop_l, dpop_r, dpop_s;
    };
    static const Bus buses[4] = {
        {0, MAIN_L, MAIN_R, MAIN_S, 0x14, 0x18, 0x34, 0x52, 0x5A, 0x62},
        {16, AUXA_L, AUXA_R, AUXA_S, 0x1C, 0x20, 0x38, 0x54, 0x5C, 0x64},
        {21, AUXB_L, AUXB_R, AUXB_S, 0x24, 0x28, 0x3C, 0x56, 0x5E, 0x66},
        {26, AUXC_L, AUXC_R, AUXC_S, 0x2C, 0x30, 0x40, 0x58, 0x60, 0x68},
    };
    for (const Bus& b : buses) {
        u32 bits = mc >> b.shift;
        bool ramp = (bits & 4) != 0;
        if (bits & (1 | 4))
            mix_add(pb, s_buf[b.l], smp, b.mix_l, b.dpop_l, ramp);
        if (bits & (2 | 4))
            mix_add(pb, s_buf[b.r], smp, b.mix_r, b.dpop_r, ramp);
        if (bits & (8 | 16))
            mix_add(pb, s_buf[b.s], smp, b.mix_s, b.dpop_s, (bits & 16) != 0);
    }
}

struct Rng {
    u32 seed;
    u32 operator()() {
        seed = seed * 1103515245u + 12345u;
        return seed >> 8;
    }
};

// the tests' sample data: random bytes, but for the first 512, runs of the extremes (PCM16
// -32768 and 32767, PCM8 -128 and 127, 32 samples each)
inline void test_data(Rng& rnd, u32 data, u32 data_size) {
    for (u32 i = 0; i < data_size; i++)
        guest::store8(data + i, (u8)rnd());
    for (u32 i = 0; i < 512; i++)
        guest::store8(data + i, (u8)(i < 256 ? ((i >> 6) & 1 ? (i & 1 ? 0x00 : 0x80) : (i & 1 ? 0xFF : 0x7F))
                                             : ((i >> 5) & 1 ? 0x80 : 0x7F)));
}

// the edges of the renderer's paths, as voices (false past the last): PCM16 looping on a run of
// -32768 or 32767 (test_data's), main L and R, every output at that value, with the envelope
// ramping onto 0, 1.0 and just past 1.0 (where it saturates), at 1.0, above it, volumes at 1.0 and
// just above, zero, and the converter reading exactly 511, 512 and 513 input samples
inline bool edge_voice(u32 i, u16 (&w)[0xD6 / 2], u32 data) {
    struct Edge {
        bool low;      // -32768 (else 32767)
        u16 ve;
        s16 ved;
        u16 vol_l, vol_r;
        u32 ratio;
    };
    static const Edge edges[] = {
        {true, 0x8001 - 95, 1, 0x8000, 0x8000, 0x10000},  // ramp onto 1.0 + 1/32768: saturates
        {true, 0x8000 - 95, 1, 0x8000, 0x8000, 0x10000},  // onto 1.0
        {true, 0x8000, -1, 0x8000, 0x8000, 0x10000},      // from 1.0 down
        {true, 95, -1, 0x8000, 0x8000, 0x10000},          // onto 0
        {true, 94, -1, 0x8000, 0x8000, 0x10000},          // past 0 (the volume wraps)
        {false, 0x8001 - 95, 1, 0x8000, 0x8000, 0x10000},
        {true, 0x8000, 0, 0x8000, 0x8000, 0x10000},       // at 1.0
        {false, 0xFFFF, 0, 0x8000, 0x8000, 0x10000},      // near 2.0: saturates up
        {true, 0xFFFF, 0, 0x8000, 0x8000, 0x10000},       // and down
        {true, 0x8000, 0, 0x8000, 0x8001, 0x10000},       // a volume just above 1.0
        {false, 0x4000, 0, 0, 0, 0x10000},                // volumes 0
        {true, 0x7FFF, 0, 0x8000, 0x7FFF, 349525},        // 511 input samples
        {true, 0x7FFF, 0, 0x8000, 0x7FFF, 349526},        // 512: the most the buffer holds
        {false, 0x7FFF, 0, 0x7FFF, 0x8000, 350208},       // 513
    };
    if (i >= sizeof(edges) / sizeof(edges[0]))
        return false;
    const Edge& e = edges[i];
    for (u16& x : w)
        x = 0;
    auto set32 = [&](u32 off, u32 v) { w[off / 2] = (u16)(v >> 16), w[off / 2 + 1] = (u16)v; };
    u32 run = (data & 0x1FFFFFFF) / 2 + (e.low ? 32 : 0);  // 32 samples of the value
    w[0x10 / 2] = 1, w[0x70 / 2] = 0x0A, w[0x6E / 2] = 1;
    set32(0x72, run), set32(0x76, run + 31), set32(0x7A, run);
    set32(0x0C, 3);
    w[0x14 / 2] = e.vol_l, w[0x18 / 2] = e.vol_r;
    w[0x6A / 2] = e.ve, w[0x6C / 2] = (u16)e.ved;
    set32(0xA6, e.ratio);
    for (u32 k = 0; k < 4; k++)
        w[0xAC / 2 + k] = (u16)(e.low ? -32768 : 32767);
    return true;
}

// a random voice for the tests, as PB words: every format with addresses in [data, data + size)
// (the caller fills that with random bytes), loops and streams, every SRC type, ratios (now and
// then more input per output than the renderer's buffer), the mixer control with every bus and
// ramp, the envelope muted, at 1.0 or anything (ramping now and then), the filters. Half of them
// are shaped like the game's voices instead, which the renderer takes a path of its own for: main
// L and R (now and then a ramp, S, aux A or one side only), volumes up to 1.0 and just above, the
// envelope at 1.0, below, above, or ramping within 0 .. 1.0 and across its ends, the filters off
// as a rule, ratios around the most the buffer holds
inline void random_voice(Rng& rnd, u16 (&w)[0xD6 / 2], u32 data, u32 data_size) {
    for (u16& x : w)
        x = 0;
    auto set32 = [&](u32 off, u32 v) { w[off / 2] = (u16)(v >> 16), w[off / 2 + 1] = (u16)v; };
    static const u16 fmts[3] = {0x00, 0x0A, 0x19};
    u16 fmt = fmts[rnd() % 3];
    // addresses in the format's units (ADPCM nibbles, PCM16 samples, PCM8 bytes), in the data
    u32 phys = data & 0x1FFFFFFF;
    u32 base = fmt == 0x00 ? phys * 2 : fmt == 0x0A ? phys / 2 : phys;
    u32 span = fmt == 0x00 ? data_size * 2 : fmt == 0x0A ? data_size / 2 : data_size;
    u32 most = rnd() % 4 ? 600 : 6000;
    u32 len = 32 + rnd() % most;
    u32 start = base + rnd() % (span - len - 64);
    if (fmt == 0x00)
        start = (start & ~15u) + 2;
    u32 end = start + len, loop = start + rnd() % len;
    if (fmt == 0x00 && (loop & 15) < 2)
        loop = (loop & ~15u) + 2;
    w[0x10 / 2] = 1;               // running
    w[0x12 / 2] = rnd() % 5 == 0;  // stream
    w[0x08 / 2] = rnd() % 3;       // SRC type
    set32(0x0C, rnd() & 0x7FFFFFF);  // mixer control: every bus, every ramp
    for (u32 k = 0x14; k < 0x44; k += 4)
        w[k / 2] = (u16)(rnd() % 4 ? rnd() : 0), w[k / 2 + 1] = (u16)(rnd() % 5 ? 0 : rnd() & 0x3F);
    // the envelope: muted, at 1.0, or anything (ramping a third of the time)
    u32 ve = rnd() % 8;
    w[0x6A / 2] = (u16)(ve == 0 ? 0 : ve == 1 ? 0x8000 : rnd());
    w[0x6C / 2] = (u16)(ve < 2 || rnd() % 3 ? 0 : (s16)(rnd() % 129 - 64));
    w[0x6E / 2] = rnd() % 2;  // loop flag
    w[0x70 / 2] = fmt;
    set32(0x72, loop), set32(0x76, end), set32(0x7A, start + (rnd() % 4 ? 0 : rnd() % len));
    for (u32 k = 0; k < 16; k++)
        w[0x7E / 2 + k] = (u16)((s16)(rnd() % 8192) - 4096);
    w[0xA0 / 2] = (u16)(rnd() & 0x7F), w[0xA2 / 2] = (u16)rnd(), w[0xA4 / 2] = (u16)rnd();
    static const u32 ratios[6] = {0x10000, 0x8000, 0x18000, 0x20000, 0xC000, 0x2A000};
    set32(0xA6, rnd() % 40 == 0 ? 0x1000000 + rnd() % 0x2000000  // more input per output than a block
                : rnd() % 2 ? ratios[rnd() % 6] : 0x2000 + rnd() % 0x40000);
    w[0xAA / 2] = (u16)rnd();
    for (u32 k = 0; k < 4; k++)
        w[0xAC / 2 + k] = (u16)rnd();
    w[0xB4 / 2] = (u16)(rnd() & 0x7F), w[0xB6 / 2] = (u16)rnd(), w[0xB8 / 2] = (u16)rnd();
    w[0xBA / 2] = rnd() % 2, w[0xBC / 2] = (u16)rnd(), w[0xBE / 2] = (u16)(rnd() % 0x8000);
    w[0xC0 / 2] = (u16)(rnd() % 0x8000);
    w[0xC2 / 2] = rnd() % 3 == 0;
    for (u32 k = 1; k < 10; k++)
        w[0xC2 / 2 + k] = (u16)((s16)(rnd() % 0x6000) - 0x3000);
    if (rnd() % 2)
        return;
    // like the game's
    w[0x08 / 2] = rnd() % 8 ? rnd() % 2 : 2;
    static const u32 mcs[8] = {3, 3, 3, 3, 3 | 4, 1, 3 | 8, 0x30003};
    set32(0x0C, rnd() % 4 ? 3 : mcs[rnd() % 8]);
    auto vol = [&]() -> u16 {
        u32 k = rnd() % 16;
        return (u16)(k == 0 ? 0 : k == 1 ? 0x8000 : k == 2 ? 0x8001 + rnd() % 0x7FFF : k == 3 ? 1 : 1 + rnd() % 0x8000);
    };
    w[0x14 / 2] = vol(), w[0x18 / 2] = vol();
    w[0xBA / 2] = rnd() % 10 == 0, w[0xC2 / 2] = rnd() % 10 == 0;
    u32 kind = rnd() % 8;
    s32 env = kind == 0 ? 0x8000 : kind == 1 ? 1 + rnd() % 0x7FFF : kind == 2 ? 0x8001 + rnd() % 0x7FFF
            : kind == 3 ? 0 : (s32)(rnd() % 0x8001);
    s32 ved = 0;
    if (kind >= 4) {  // ramping: the last sample's volume (95 steps on) within 0 .. 1.0, exactly on
                      // or just past an end of that, or anywhere
        static const s32 ends[4] = {0, 0x8000, 0x8001, -1};
        s32 to = kind == 4 ? (s32)(rnd() % 0x8001) : kind == 5 ? ends[rnd() % 4]
               : kind == 6 ? (s32)(rnd() % 0x10000) - 0x4000 : env + (s32)(rnd() % 191) - 95;
        ved = (s32)(rnd() % 0x200) - 0x100;
        if (ved == 0)
            ved = 1;
        env = to - 95 * ved;
        if (env < 0 || env > 0xFFFF)
            ved = -ved, env = to - 95 * ved;
        if (env < 0 || env > 0xFFFF)
            env = to < 0 ? 0 : to > 0xFFFF ? 0xFFFF : to;
    }
    w[0x6A / 2] = (u16)env, w[0x6C / 2] = (u16)(s16)ved;
    if (rnd() % 4 == 0) {  // in the data's first 512 bytes: the extremes (test_data)
        u32 n = fmt == 0x00 ? 1024 : fmt == 0x0A ? 256 : 512;
        u32 at = base + rnd() % (n / 2), len2 = 16 + rnd() % (n / 2 - 16);
        if (fmt == 0x00)
            at = (at & ~15u) + 2;
        set32(0x72, at + rnd() % len2), set32(0x76, at + len2), set32(0x7A, at);
    }
    if (rnd() % 4 == 0) {  // just inside or outside the most input the buffer holds (512 samples)
        u32 pos = w[0xAA / 2];
        set32(0xA6, (((512u << 16) - pos) / 96) + rnd() % 5 - 2);
    } else if (rnd() % 2) {
        set32(0xA6, 0x8000 + rnd() % 0x28000);
    }
}

}  // namespace engine::audio::reference
