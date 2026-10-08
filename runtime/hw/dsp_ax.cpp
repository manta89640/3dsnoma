// The AX sound microcode, high-level emulated. Each audio frame (3 ms: 96 samples at 32 kHz)
// the CPU sends a command list (mail 0xBABE0000 | length, then its address); the list sets up
// the mixing buffers, runs every voice (a parameter block, "PB", in main memory), mixes the aux
// buses back in and writes the frame's output, which the AI DMA then plays.
//
// Voices are rendered natively (engine/audio/ax_render.cpp): samples from main memory through the
// DSP's accelerator (ADPCM, PCM8 or PCM16, with loop and end addresses), resampled (linear
// interpolation: the polyphase filter's coefficients live in the DSP's ROM), scaled by the volume
// envelope, optionally low-pass or biquad filtered, then added to the main/aux buffers with
// per-channel volumes that may ramp; the PB's state is written back.
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
#include "engine/audio/ax_render.h"
#include "engine/audio/stream.h"

namespace guest {

namespace {

const u32 SPF = 96;  // samples per frame and channel

// mixing buffers: main L R S, then aux A, B, C (each L R S)
engine::audio::AxBuses s_buf;
enum { MAIN_L, MAIN_R, MAIN_S, AUXA_L, AUXA_R, AUXA_S, AUXB_L, AUXB_R, AUXB_S, AUXC_L, AUXC_R, AUXC_S };

u16 s_last_master = 0x8000;
u16 s_last_aux[3] = {0x8000, 0x8000, 0x8000};

inline s16 clamp16(s32 v) { return (s16)std::min<s32>(32767, std::max<s32>(-32768, v)); }
inline u32 w32(u32 addr) { return load32(addr); }
// an address the CPU put in a list or PB (physical, or a cached/uncached pointer): main memory
inline u32 mem(u32 a) { return a ? (0x80000000u | (a & 0x1FFFFFFF)) : 0; }
// the n bytes at a as host memory, if they are RAM on one page (else nullptr: through the guest's
// loads and stores, as hardware registers would be)
inline u8* span(u32 a, u32 n) {
    u8* p = host_ptr(a);
    return p && (a & (PAGE_SIZE - 1)) + n <= PAGE_SIZE ? p : nullptr;
}
inline u32 rd32(const u8* p) {
    u32 v;
    memcpy(&v, p, 4);
    return v;
}
inline void wr32(u8* p, u32 v) { memcpy(p, &v, 4); }

// ---------------------------------------------------------------- voices
// PB words the command list reads itself (the voices are engine::audio::ax_render_voice's)
enum : u32 {
    PB_NEXT = 0x00, PB_SRC_TYPE = 0x08, PB_MIXCTRL = 0x0C, PB_RUNNING = 0x10, PB_VE = 0x6A,
    PB_LOOPFLAG = 0x6E, PB_FORMAT = 0x70, PB_LOOP = 0x72, PB_END = 0x76, PB_CUR = 0x7A, PB_RATIO = 0xA6,
};

// ---------------------------------------------------------------- commands
// SETUP: each buffer starts as a ramp (value, delta): the CPU's depop of stopped voices; as a
// rule all zero
void setup(u32 addr) {
    for (int b = 0; b < 12; b++) {
        u32 a = addr + 6 * b;
        u32 v = (u32)load16(a) << 16 | load16(a + 2);
        u32 d = (u32)(s32)(s16)load16(a + 4);
        s32* out = s_buf[b];
        if (d == 0 && v == 0) {
            memset(out, 0, sizeof(s_buf[b]));
        } else {
            for (u32 i = 0; i < SPF; i++, v += d)  // (v + i * d, wrapping)
                out[i] = (s32)v;
        }
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
    static const bool s_debug = getenv("NOMA_DEBUG_AX") != nullptr;
    int running = 0;
    for (int guard = 0; addr && guard < 256; guard++) {
        const u8* p = span(addr, 0x140);
        if (p && !s_debug && s_logged >= 6) {  // (the PB in place, as ax_render_voice reads it)
            running += engine::audio::ax_render_voice(addr, s_buf);
            addr = mem(__builtin_bswap32(rd32(p + PB_NEXT)));
            continue;
        }
        bool on = load16(addr + PB_RUNNING) == 1;
        if (s_logged < 6 && on) {
            s_logged++;
            log("ax: voice %08X: format %02X loop %u addr %08X-%08X @%08X ratio %08X src %u mix %08X ve %04X",
                addr, load16(addr + PB_FORMAT), load16(addr + PB_LOOPFLAG), load32(addr + PB_LOOP),
                load32(addr + PB_END), load32(addr + PB_CUR), load32(addr + PB_RATIO), load16(addr + PB_SRC_TYPE),
                load32(addr + PB_MIXCTRL), load16(addr + PB_VE));
        }
        running += on;
        if (s_debug && on && load16(addr + 0xD6)) {
            static int n;
            if (n++ < 5)  // (not mixed: the 3DS has no Wii Remote speaker)
                log("ax: voice %08X plays on a Wii Remote speaker (remote mixer %04X)", addr, load16(addr + 0xD8));
        }
        engine::audio::ax_render_voice(addr, s_buf);
        addr = mem(load32(addr + PB_NEXT));
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

// the frame: S to the surround buffer, L and R at the master volume (ramping from the last
// frame's to this one's) and clamped, as 16-bit R L pairs. As a rule the volume stays the same (no
// ramp, no division), at 1.0 (the sum only clamped), and both buffers are RAM.
void output(u32 surround_addr, u32 lr_addr, u16 volume) {
    engine::audio::stream_mix(s_buf[MAIN_L], s_buf[MAIN_R], SPF);  // a native music stream, if one plays
    u16 v0 = s_last_master;
    s_last_master = volume;
    u8* s_out = span(surround_addr, 4 * SPF);
    u8* lr_out = span(lr_addr, 4 * SPF);
    if (s_out) {
        for (u32 i = 0; i < SPF; i++)
            wr32(s_out + 4 * i, __builtin_bswap32((u32)s_buf[MAIN_S][i]));
    } else {
        for (u32 i = 0; i < SPF; i++)
            store32(surround_addr + 4 * i, (u32)s_buf[MAIN_S][i]);
    }
    auto pair = [&](u32 i, s16 l, s16 r) {  // (R then L, big-endian)
        if (lr_out) {
            wr32(lr_out + 4 * i, (u32)(u16)__builtin_bswap16((u16)r) | (u32)__builtin_bswap16((u16)l) << 16);
        } else {
            store16(lr_addr + 4 * i, (u16)r);
            store16(lr_addr + 4 * i + 2, (u16)l);
        }
    };
    if (volume == v0 && volume == 0x8000) {
        for (u32 i = 0; i < SPF; i++)
            pair(i, clamp16(s_buf[MAIN_L][i]), clamp16(s_buf[MAIN_R][i]));
    } else if (volume == v0) {
        for (u32 i = 0; i < SPF; i++)
            pair(i, clamp16((s32)(((s64)s_buf[MAIN_L][i] * volume) >> 15)),
                 clamp16((s32)(((s64)s_buf[MAIN_R][i] * volume) >> 15)));
    } else {
        for (u32 i = 0; i < SPF; i++) {
            s32 vol = v0 + (s32)(volume - v0) * (s32)i / (s32)SPF;
            pair(i, clamp16((s32)(((s64)s_buf[MAIN_L][i] * vol) >> 15)), clamp16((s32)(((s64)s_buf[MAIN_R][i] * vol) >> 15)));
        }
    }
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
            s32 before = s_debug ? peak_lr() : 0;
            int running = process_pbs(pbs);
            // how many voices the game plays at once (the 3DS's DSP has 24 channels)
            static int s_most, s_frames;
            s_most = std::max(s_most, running);
            if (++s_frames == 2000) {
                log("ax: at most %d voices in a frame (last 2000 frames)", s_most);
                s_most = s_frames = 0;
                if (s_debug) {
                    using namespace engine::audio;
                    const u32* n = ax_paths;
                    log("ax: voice frames so far: one pass %u (envelope 1.0 %u, fixed %u, ramping %u, above 1.0 %u); "
                        "muted %u; other buses, filters or SRC %u; levels or input %u",
                        n[AXP_ONE] + n[AXP_FIXED] + n[AXP_RAMP] + n[AXP_LOUD], n[AXP_ONE], n[AXP_FIXED], n[AXP_RAMP],
                        n[AXP_LOUD], n[AXP_MUTED], n[AXP_MIX], n[AXP_LEVELS]);
                }
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
