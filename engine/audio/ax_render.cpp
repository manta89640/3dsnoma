// AX voices, native (ax_render.h).
#include "ax_render.h"

namespace engine::audio {

namespace {

// Wii AX parameter block (u16 words, big-endian; runtime/hw/dsp_ax.cpp has the whole layout)
enum : u32 {
    PB_SRC_TYPE = 0x08, PB_MIXCTRL = 0x0C, PB_RUNNING = 0x10, PB_STREAM = 0x12, PB_VE = 0x6A,
    PB_LOOPFLAG = 0x6E, PB_FORMAT = 0x70, PB_LOOP = 0x72, PB_END = 0x76, PB_CUR = 0x7A,
    PB_COEFS = 0x7E, PB_PRED = 0xA0, PB_YN1 = 0xA2, PB_YN2 = 0xA4, PB_RATIO = 0xA6, PB_FRAC = 0xAA,
    PB_LAST = 0xAC, PB_LOOP_PRED = 0xB4, PB_LOOP_YN1 = 0xB6, PB_LOOP_YN2 = 0xB8, PB_LPF = 0xBA,
    PB_BIQUAD = 0xC2,
    PB_WORDS = 0xD6 / 2,  // through the biquad (the Wii Remote part after it is left alone)
};
enum : u16 { FMT_ADPCM = 0x00, FMT_PCM16 = 0x0A, FMT_PCM8 = 0x19 };

inline s16 clamp16(s32 v) { return v < -32768 ? (s16)-32768 : v > 32767 ? (s16)32767 : (s16)v; }

// the PB in host order: read whole and written back whole, as the DSP does
struct Pb {
    u16 w[PB_WORDS];
    u16& at(u32 off) { return w[off / 2]; }
    u32 get32(u32 off) const { return (u32)w[off / 2] << 16 | w[off / 2 + 1]; }
    void set32(u32 off, u32 v) { w[off / 2] = (u16)(v >> 16), w[off / 2 + 1] = (u16)v; }
};

// sample memory: the accelerator's addresses are physical (nibbles for ADPCM, 16-bit samples for
// PCM16, bytes for PCM8)
inline u8 mem8(u32 phys) { return guest::load8(0x80000000u | (phys & 0x1FFFFFFF)); }
inline s16 mem16(u32 phys) { return (s16)guest::load16(0x80000000u | (phys & 0x1FFFFFFF)); }

// the accelerator: the next sample at the current address, then the address moves on (the end
// address is inclusive: past it the voice loops or stops)
struct Accel {
    Pb& pb;
    u32 cur, end, loop;
    u16 fmt, pred;
    s32 yn1, yn2;
    s32 coef[16];
    bool looped, stream, stopped = false;

    explicit Accel(Pb& p) : pb(p) {
        cur = p.get32(PB_CUR), end = p.get32(PB_END), loop = p.get32(PB_LOOP);
        fmt = p.at(PB_FORMAT), pred = p.at(PB_PRED);
        yn1 = (s16)p.at(PB_YN1), yn2 = (s16)p.at(PB_YN2);
        for (int i = 0; i < 16; i++)
            coef[i] = (s16)p.at(PB_COEFS + 2 * i);
        looped = p.at(PB_LOOPFLAG) != 0, stream = p.at(PB_STREAM) != 0;
    }

    s16 sample() {
        if (stopped)
            return 0;
        s16 s;
        switch (fmt) {
        case FMT_ADPCM: {
            if ((cur & 15) == 0) {  // a frame header: predictor and scale for 14 samples
                pred = mem8(cur >> 1);
                cur += 2;
            }
            u32 b = mem8(cur >> 1);
            s32 nib = (cur & 1) ? (s32)(b & 15) : (s32)(b >> 4);
            nib -= (nib & 8) << 1;
            u32 idx = (pred >> 4) & 7;
            s32 v = nib * (1 << (pred & 15)) * 2048 + coef[idx * 2] * yn1 + coef[idx * 2 + 1] * yn2;
            s = clamp16((v + 0x400) >> 11);
            yn2 = yn1;
            yn1 = s;
            break;
        }
        case FMT_PCM16:
            s = mem16(cur * 2);
            break;
        case FMT_PCM8:
            s = (s16)((s8)mem8(cur) << 8);
            break;
        default:
            s = 0;
            break;
        }
        if (cur == end) {
            if (looped) {
                cur = loop;
                if (!stream) {  // a looped sound restarts with its saved context
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

// the sample rate converter: AX_SPF output samples, the input position running in 16.16 steps of
// the ratio, linearly between the second- and third-newest input samples (the 4-tap filter's centre)
void convert(Pb& pb, s16* out) {
    Accel acc(pb);
    u32 ratio = pb.get32(PB_RATIO);
    u32 pos = pb.at(PB_FRAC);
    s32 h0 = (s16)pb.at(PB_LAST), h1 = (s16)pb.at(PB_LAST + 2), h2 = (s16)pb.at(PB_LAST + 4),
        h3 = (s16)pb.at(PB_LAST + 6);
    if (pb.at(PB_SRC_TYPE) == 2) {  // no conversion: one input sample per output sample
        for (u32 i = 0; i < AX_SPF; i++) {
            h0 = h1, h1 = h2, h2 = h3, h3 = acc.sample();
            out[i] = (s16)h3;
        }
    } else {
        for (u32 i = 0; i < AX_SPF; i++) {
            pos += ratio;
            while (pos >= 0x10000) {
                h0 = h1, h1 = h2, h2 = h3, h3 = acc.sample();
                pos -= 0x10000;
            }
            s32 frac = (s32)(pos & 0xFFFF);
            out[i] = (s16)(((s64)h1 * 0x10000 + (s64)(h2 - h1) * frac) >> 16);
        }
    }
    pb.at(PB_FRAC) = (u16)pos;
    pb.at(PB_LAST) = (u16)h0, pb.at(PB_LAST + 2) = (u16)h1;
    pb.at(PB_LAST + 4) = (u16)h2, pb.at(PB_LAST + 6) = (u16)h3;
    acc.save();
}

// add `in` to a bus at the volume in mix word `vol_off`, ramping by its delta if `ramp`; the last
// sample added goes to the depop word `dpop_off`
void mix_add(Pb& pb, s32* out, const s16* in, u32 vol_off, u32 dpop_off, bool ramp) {
    u16 vol = pb.at(vol_off);
    u16 delta = ramp ? pb.at(vol_off + 2) : 0;
    s32 last = 0;
    for (u32 i = 0; i < AX_SPF; i++) {
        last = clamp16((in[i] * (s32)vol) >> 15);
        out[i] += last;
        vol = (u16)(vol + delta);
    }
    pb.at(vol_off) = vol;
    pb.at(dpop_off) = (u16)last;
}

void render(Pb& pb, AxBuses& bus) {
    s16 smp[AX_SPF];
    convert(pb, smp);

    // the volume envelope (unsigned on the Wii)
    u16 ve = pb.at(PB_VE);
    u16 ved = pb.at(PB_VE + 2);
    for (u32 i = 0; i < AX_SPF; i++) {
        smp[i] = clamp16((smp[i] * (s32)ve) >> 15);
        ve = (u16)(ve + ved);
    }
    pb.at(PB_VE) = ve;

    if (pb.at(PB_LPF)) {  // the one-pole low-pass
        s32 a0 = pb.at(PB_LPF + 4), b0 = (s16)pb.at(PB_LPF + 6);
        s32 y = (s16)pb.at(PB_LPF + 2);
        for (u32 i = 0; i < AX_SPF; i++)
            y = smp[i] = clamp16((a0 * smp[i] + b0 * y) >> 15);
        pb.at(PB_LPF + 2) = (u16)y;
    }
    if (pb.at(PB_BIQUAD)) {  // the biquad, coefficients in 2.14
        s64 b0 = (s16)pb.at(PB_BIQUAD + 10), b1 = (s16)pb.at(PB_BIQUAD + 12), b2 = (s16)pb.at(PB_BIQUAD + 14);
        s64 a1 = (s16)pb.at(PB_BIQUAD + 16), a2 = (s16)pb.at(PB_BIQUAD + 18);
        s32 xn1 = (s16)pb.at(PB_BIQUAD + 2), xn2 = (s16)pb.at(PB_BIQUAD + 4);
        s32 yn1 = (s16)pb.at(PB_BIQUAD + 6), yn2 = (s16)pb.at(PB_BIQUAD + 8);
        for (u32 i = 0; i < AX_SPF; i++) {
            s32 x = smp[i];
            s64 t = (b0 * x + b1 * xn1 + b2 * xn2 + a1 * yn1 + a2 * yn2) << 2;
            t += (t & 0x10000) ? 0x8000 : 0x7FFF;
            s32 y = clamp16((s32)(t >> 16));
            xn2 = xn1, xn1 = x, yn2 = yn1, yn1 = y;
            smp[i] = (s16)y;
        }
        pb.at(PB_BIQUAD + 2) = (u16)xn1, pb.at(PB_BIQUAD + 4) = (u16)xn2;
        pb.at(PB_BIQUAD + 6) = (u16)yn1, pb.at(PB_BIQUAD + 8) = (u16)yn2;
    }

    // the mixer control word: for each bus its L and R bits, a ramp bit for both, then S and S's
    // ramp bit
    u32 mc = pb.get32(PB_MIXCTRL);
    struct Out {
        u32 shift;  // the bus's L bit in the mixer control word
        int l, r, s;
        u32 mix_l, mix_r, mix_s;  // (volume, delta) in the PB
        u32 dpop_l, dpop_r, dpop_s;
    };
    static const Out outs[4] = {
        {0, AX_MAIN_L, AX_MAIN_R, AX_MAIN_S, 0x14, 0x18, 0x34, 0x52, 0x5A, 0x62},
        {16, AX_AUXA_L, AX_AUXA_R, AX_AUXA_S, 0x1C, 0x20, 0x38, 0x54, 0x5C, 0x64},
        {21, AX_AUXB_L, AX_AUXB_R, AX_AUXB_S, 0x24, 0x28, 0x3C, 0x56, 0x5E, 0x66},
        {26, AX_AUXC_L, AX_AUXC_R, AX_AUXC_S, 0x2C, 0x30, 0x40, 0x58, 0x60, 0x68},
    };
    for (const Out& o : outs) {
        u32 bits = mc >> o.shift;
        bool ramp = (bits & 4) != 0;
        if (bits & (1 | 4))
            mix_add(pb, bus[o.l], smp, o.mix_l, o.dpop_l, ramp);
        if (bits & (2 | 4))
            mix_add(pb, bus[o.r], smp, o.mix_r, o.dpop_r, ramp);
        if (bits & (8 | 16))
            mix_add(pb, bus[o.s], smp, o.mix_s, o.dpop_s, (bits & 16) != 0);
    }
}

}  // namespace

bool ax_render_voice(u32 pb_addr, AxBuses& bus) {
    u8* p = guest::host_ptr(pb_addr);
    if (!p)
        return false;
    Pb pb;
    for (u32 i = 0; i < PB_WORDS; i++)
        pb.w[i] = (u16)(p[2 * i] << 8 | p[2 * i + 1]);
    if (pb.at(PB_RUNNING) != 1)
        return false;
    render(pb, bus);
    for (u32 i = 0; i < PB_WORDS; i++)
        p[2 * i] = (u8)(pb.w[i] >> 8), p[2 * i + 1] = (u8)pb.w[i];
    return true;
}

}  // namespace engine::audio
