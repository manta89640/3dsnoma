// The AX sound microcode, high-level emulated. Each audio frame (3 ms: 96 samples at 32 kHz)
// the CPU sends a command list (mail 0xBABE0000 | length, then its address); the list sets up
// the mixing buffers, runs every voice (a parameter block, "PB", in main memory), mixes the aux
// buses back in and writes the frame's output, which the AI DMA then plays.
//
// Voices: samples come from main memory through the DSP's accelerator (ADPCM, PCM8 or PCM16,
// with loop and end addresses), are resampled (linear interpolation: the polyphase filter's
// coefficients live in the DSP's ROM), scaled by the volume envelope, optionally low-pass or
// biquad filtered, then added to the main/aux buffers with per-channel volumes that may ramp.
// The PB's state (addresses, ADPCM history, volumes, depop values) is written back.
//
// Wii AX parameter block (u16 words, big-endian), as the 2006-2010 SDKs lay it out:
//   0x00 next PB   0x04 this PB   0x08 SRC type   0x0A polyphase coefficient set
//   0x0C mixer control (32 bits)  0x10 running    0x12 stream
//   0x14 mixer: 24 words, (volume, delta) for L R AuxA-L AuxA-R AuxB-L AuxB-R AuxC-L AuxC-R
//        S AuxA-S AuxB-S AuxC-S
//   0x44 initial time delay (7)   0x52 depop: L AuxA-L AuxB-L AuxC-L R AuxA-R ... S ... (12)
//   0x6A volume envelope (volume, delta)
//   0x6E address: loop flag, format, loop, end, current (32-bit each)
//   0x7E ADPCM: 16 coefficients, gain, pred/scale, yn1, yn2
//   0xA6 SRC: ratio (16.16), position fraction, last 4 samples
//   0xB4 ADPCM loop context: pred/scale, yn1, yn2
//   0xBA low-pass filter: on, yn1, a0, b0
//   0xC2 biquad: on, xn1, xn2, yn1, yn2, b0, b1, b2, a1, a2
//   0xD6 remote (Wii Remote speaker) mixing, not emulated: the 3DS has no Wii Remote
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "hw.h"

namespace guest {

namespace {

const u32 SPF = 96;  // samples per frame and channel

// mixing buffers: main L R S, then aux A, B, C (each L R S)
s32 s_buf[12][SPF];
enum { MAIN_L, MAIN_R, MAIN_S, AUXA_L, AUXA_R, AUXA_S, AUXB_L, AUXB_R, AUXB_S, AUXC_L, AUXC_R, AUXC_S };

u16 s_last_master = 0x8000;
u16 s_last_aux[3] = {0x8000, 0x8000, 0x8000};

inline s16 clamp16(s32 v) { return (s16)std::min<s32>(32767, std::max<s32>(-32768, v)); }
inline u32 w32(u32 addr) { return load32(addr); }
// an address the CPU put in a list or PB (physical, or a cached/uncached pointer): main memory
inline u32 mem(u32 a) { return a ? (0x80000000u | (a & 0x1FFFFFFF)) : 0; }

// ---------------------------------------------------------------- the PB, as words
struct Pb {
    u16 w[0xD6 / 2];  // through the biquad (the remote part from 0xD6 on is left alone)
    u32 addr;
    void read(u32 a) {
        addr = a;
        for (u32 i = 0; i < sizeof(w) / 2; i++)
            w[i] = load16(a + 2 * i);
    }
    void write() const {
        for (u32 i = 0; i < sizeof(w) / 2; i++)
            store16(addr + 2 * i, w[i]);
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

    static u8 byte_at(u32 phys) { return load8(0x80000000u | (phys & 0x1FFFFFFF)); }

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
            s = (s16)load16(0x80000000u | ((cur * 2) & 0x1FFFFFFF));
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

void process_voice(Pb& pb) {
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

// ---------------------------------------------------------------- commands
// SETUP: each buffer starts as a ramp (value, delta): the CPU's depop of stopped voices
void setup(u32 addr) {
    for (int b = 0; b < 12; b++) {
        u32 a = addr + 6 * b;
        s32 v = (s32)((u32)load16(a) << 16 | load16(a + 2));
        s16 d = (s16)load16(a + 4);
        for (u32 i = 0; i < SPF; i++)
            s_buf[b][i] = v + (s32)i * d;
    }
}

void add_to_lr(u32 addr, bool neg) {
    for (u32 i = 0; i < SPF; i++) {
        s32 v = (s32)w32(addr + 4 * i);
        s_buf[MAIN_L][i] += neg ? -v : v;
        s_buf[MAIN_R][i] += neg ? -v : v;
    }
}

void add_sub_to_lr(u32 addr) {
    for (u32 i = 0; i < SPF; i++)
        s_buf[MAIN_L][i] += (s32)w32(addr + 4 * i);
    for (u32 i = 0; i < SPF; i++)
        s_buf[MAIN_R][i] -= (s32)w32(addr + 4 * SPF + 4 * i);
}

int process_pbs(u32 addr) {
    static int s_logged;
    int running = 0;
    for (int guard = 0; addr && guard < 256; guard++) {
        Pb pb;
        pb.read(addr);
        if (s_logged < 6 && pb.at(PB_RUNNING) == 1) {
            s_logged++;
            log("ax: voice %08X: format %02X loop %u addr %08X-%08X @%08X ratio %08X src %u mix %08X ve %04X",
                addr, pb.at(PB_FORMAT), pb.at(PB_LOOPFLAG), pb.get32(PB_LOOP), pb.get32(PB_END),
                pb.get32(PB_CUR), pb.get32(PB_RATIO), pb.at(PB_SRC_TYPE), pb.get32(PB_MIXCTRL), pb.at(PB_VE));
        }
        running += pb.at(PB_RUNNING) == 1;
        static const bool s_debug = getenv("NOMA_DEBUG_AX") != nullptr;
        if (s_debug && pb.at(PB_RUNNING) == 1 && load16(addr + 0xD6)) {
            static int n;
            if (n++ < 5)  // (not mixed: the 3DS has no Wii Remote speaker)
                log("ax: voice %08X plays on a Wii Remote speaker (remote mixer %04X)", addr, load16(addr + 0xD8));
        }
        process_voice(pb);
        pb.write();
        addr = mem(pb.get32(PB_NEXT));
    }
    return running;
}

// aux bus: its frame goes to the CPU (for reverb and such), last frame's processed version comes
// back and is mixed into main L R S at the return volume
void mix_aux(int aux, u32 write_addr, u32 read_addr, u16 volume) {
    int base = AUXA_L + 3 * aux;
    for (int c = 0; c < 3; c++)
        for (u32 i = 0; i < SPF; i++)
            store32(write_addr + 4 * (c * SPF + i), (u32)s_buf[base + c][i]);
    u16 v0 = s_last_aux[aux];
    for (int c = 0; c < 3; c++)
        for (u32 i = 0; i < SPF; i++) {
            s32 vol = v0 + (s32)(volume - v0) * (s32)i / (s32)SPF;
            s32 s = (s32)w32(read_addr + 4 * (c * SPF + i));
            s_buf[MAIN_L + c][i] += (s32)(((s64)s * vol) >> 15);
        }
    s_last_aux[aux] = volume;
}

// the frame: S to the surround buffer, L and R clamped, as 16-bit R L pairs
void output(u32 surround_addr, u32 lr_addr, u16 volume) {
    for (u32 i = 0; i < SPF; i++)
        store32(surround_addr + 4 * i, (u32)s_buf[MAIN_S][i]);
    u16 v0 = s_last_master;
    for (u32 i = 0; i < SPF; i++) {
        s32 vol = v0 + (s32)(volume - v0) * (s32)i / (s32)SPF;
        s16 l = clamp16((s32)(((s64)s_buf[MAIN_L][i] * vol) >> 15));
        s16 r = clamp16((s32)(((s64)s_buf[MAIN_R][i] * vol) >> 15));
        store16(lr_addr + 4 * i, (u16)r);
        store16(lr_addr + 4 * i + 2, (u16)l);
    }
    s_last_master = volume;
}

}  // namespace

// NOMA_DEBUG_AX: the peak of main L/R after each command of a few busy frames
static s32 peak_lr() {
    s32 m = 0;
    for (u32 i = 0; i < SPF; i++)
        m = std::max<s32>(m, std::max<s32>(std::abs(s_buf[MAIN_L][i]), std::abs(s_buf[MAIN_R][i])));
    return m;
}

u32 g_ax_lists;

// run one command list (size in words) from main memory
void dsp_ax_command_list(u32 list_addr, u32 words) {
    if (audio_dsp_native())
        return audio_dsp_frame(list_addr, words);
    NOMA_PROF_SCOPE("(ax)");
    g_ax_lists++;
    static bool s_debug = getenv("NOMA_DEBUG_AX") != nullptr;
    static int s_frames_logged;
    bool dbg = false;
    u32 a = mem(list_addr);
    u32 end = a + 2 * std::min<u32>(words ? words : 64, 512);
    auto next16 = [&]() { u16 v = load16(a); a += 2; return v; };
    auto next32 = [&]() { u32 hi = next16(); return hi << 16 | next16(); };
    static int s_unknown;
    while (a < end) {
        u16 cmd = next16();
        if (dbg)
            log("ax:   before cmd %02X: peak L/R %d S %d", cmd, peak_lr(), std::abs(s_buf[MAIN_S][0]));
        switch (cmd) {
        case 0x00: setup(mem(next32())); break;
        case 0x01: add_to_lr(mem(next32()), false); break;
        case 0x02: add_to_lr(mem(next32()), true); break;
        case 0x03: add_sub_to_lr(mem(next32())); break;
        case 0x04: {
            u32 pbs = mem(next32());
            s32 before = peak_lr();
            int running = process_pbs(pbs);
            // how many voices the game plays at once (the 3DS's DSP has 24 channels)
            static int s_most, s_frames;
            s_most = std::max(s_most, running);
            if (++s_frames == 2000) {
                log("ax: at most %d voices in a frame (last 2000 frames)", s_most);
                s_most = s_frames = 0;
            }
            if (s_debug && running && s_frames_logged < 4) {
                dbg = true;
                s_frames_logged++;
                log("ax: frame with %d voices (list %08X): peak after setup %d, after voices %d", running,
                    list_addr, before, peak_lr());
            }
            break;
        }
        case 0x05: case 0x06: case 0x07: {  // MIX_AUXA/B/C: volume, write buffer, read buffer
            u16 vol = next16();
            u32 w = next32(), r = next32();
            mix_aux(cmd - 0x05, mem(w), mem(r), vol);
            break;
        }
        case 0x08: case 0x09: {  // UPL_AUXA/B_MIX_LRSC (Dolby Pro Logic II): not emulated
            next16();
            for (int i = 0; i < 6; i++)
                next32();
            break;
        }
        case 0x0A: {  // COMPRESSOR: threshold, release frames, table (a limiter; the clamp suffices)
            next16(), next16(), next32();
            break;
        }
        case 0x0B: case 0x0C: {  // OUTPUT / OUTPUT_DPL2: volume, surround buffer, L/R buffer
            u16 vol = next16();
            u32 s = next32(), lr = next32();
            output(mem(s), mem(lr), vol);
            break;
        }
        case 0x0D:  // WM_OUTPUT: four Wii Remote speaker buffers (no speaker here)
            for (int i = 0; i < 4; i++)
                next32();
            break;
        case 0x0E:  // END
            return;
        default:
            if (s_unknown++ < 8)
                log("ax: unknown command %04X in the list at %08X", cmd, list_addr);
            return;
        }
    }
}

}  // namespace guest
