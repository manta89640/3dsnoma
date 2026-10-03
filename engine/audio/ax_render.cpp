// AX voices, native (ax_render.h).
//
// The comments define each stage sample by sample; the code works in blocks: the accelerator
// decodes runs of samples (to the end of an ADPCM frame, the end address, or what the frame
// needs) straight from guest memory, the converter interpolates from a buffer of them, and what
// can't be heard (a voice whose envelope is at zero, a bus at zero volume) is only accounted for.
// On the 3DS the ARM11's DSP instructions do the arithmetic. The result is the same, bit for bit,
// as the sample-by-sample definition in ax_reference.h: NOMA_SELFTEST=ax checks that on random
// voices on the host, tools/armbench/armbench.py on the code the 3DS runs.
#include "ax_render.h"

#include <algorithm>
#include <cstring>
#if defined(__ARM_FEATURE_DSP) && defined(__ARM_FEATURE_SIMD32)
#include <arm_acle.h>
#define AX_ARM_DSP 1
#endif

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

// the ARM11's DSP instructions (one each on the 3DS), and what they compute
#ifdef AX_ARM_DSP
inline s32 sat16(s32 v) { return __ssat(v, 16); }
inline u32 rev16(u32 v) { return __rev16(v); }
inline s32 smlad(u32 a, u32 b, s32 acc) { return __smlad((s32)a, (s32)b, acc); }
inline s32 smlawb(s32 a, u32 b, s32 acc) { return __smlawb(a, (s32)b, acc); }
inline s32 smlawt(s32 a, u32 b, s32 acc) { return __smlawt(a, (s32)b, acc); }
// a's low half, with b's low half above it (PKHBT)
inline u32 pack(u32 a, u32 b) {
    u32 r;
    asm("pkhbt %0, %1, %2, lsl #16" : "=r"(r) : "r"(a), "r"(b));
    return r;
}
#else
// saturated to 16 bits
inline s32 sat16(s32 v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }
// the bytes of each 16-bit half swapped
inline u32 rev16(u32 v) { return (v & 0x00FF00FF) << 8 | (v >> 8 & 0x00FF00FF); }
// acc plus the products of the 16-bit halves (wrapping)
inline s32 smlad(u32 a, u32 b, s32 acc) {
    return (s32)((u32)acc + (u32)((s16)a * (s16)b) + (u32)((s16)(a >> 16) * (s16)(b >> 16)));
}
// acc plus a times b's low (high) half, without the product's low 16 bits
inline s32 smlawb(s32 a, u32 b, s32 acc) { return (s32)((u32)acc + (u32)(s32)(((s64)a * (s16)b) >> 16)); }
inline s32 smlawt(s32 a, u32 b, s32 acc) { return (s32)((u32)acc + (u32)(s32)(((s64)a * (s16)(b >> 16)) >> 16)); }
// a's low half, with b's low half above it
inline u32 pack(u32 a, u32 b) { return b << 16 | (a & 0xFFFF); }
#endif
// two 16-bit samples as one word (the first in the low half)
inline u32 pair(const s16* p) {
    u32 v;
    memcpy(&v, p, 4);
    return v;
}

// the PB in host order: read whole and written back whole, as the DSP does
struct Pb {
    alignas(4) u16 w[PB_WORDS + 1];
    u16& at(u32 off) { return w[off / 2]; }
    u32 get32(u32 off) const { return (u32)w[off / 2] << 16 | w[off / 2 + 1]; }
    void set32(u32 off, u32 v) { w[off / 2] = (u16)(v >> 16), w[off / 2 + 1] = (u16)v; }
    // (two big-endian words at a time: one 32-bit load and a byte swap in each half)
    void load(const u8* p) {
        for (u32 i = 0; i < PB_WORDS / 2; i++) {
            u32 v;
            memcpy(&v, p + 4 * i, 4);
            v = rev16(v);
            memcpy(w + 2 * i, &v, 4);
        }
        w[PB_WORDS - 1] = (u16)(p[2 * PB_WORDS - 2] << 8 | p[2 * PB_WORDS - 1]);
    }
    void store(u8* p) const {
        for (u32 i = 0; i < PB_WORDS / 2; i++) {
            u32 v;
            memcpy(&v, w + 2 * i, 4);
            v = rev16(v);
            memcpy(p + 4 * i, &v, 4);
        }
        p[2 * PB_WORDS - 2] = (u8)(w[PB_WORDS - 1] >> 8), p[2 * PB_WORDS - 1] = (u8)w[PB_WORDS - 1];
    }
};

// sample memory: the accelerator's addresses are physical (nibbles for ADPCM, 16-bit samples for
// PCM16, bytes for PCM8); RAM is read in place, a page at a time
inline u32 sample_addr(u32 phys) { return 0x80000000u | (phys & 0x1FFFFFFF); }
inline u32 page_room(u32 phys) { return guest::PAGE_SIZE - (phys & (guest::PAGE_SIZE - 1)); }  // bytes to its page's end
inline const u8* ram(u32 phys) { return guest::host_ptr(sample_addr(phys)); }
// n bytes from phys on, not on a RAM page (hardware registers, say): through the guest's loads
const u8* fetch(u32 phys, u32 n, u8* tmp) {
    for (u32 i = 0; i < n; i++)
        tmp[i] = guest::load8(sample_addr(phys + i));
    return tmp;
}
const u32 FETCH = 64;  // samples a run off RAM reads at most

// the accelerator. Per sample: at an ADPCM frame's header (a nibble address that is a multiple of
// 16) it takes the frame's predictor and scale and moves past the header; it reads the sample at
// the current address; then the address moves on, except at the end address (inclusive): there
// the voice loops (a looped sound that is not a stream restarts with the loop's ADPCM context) or
// stops (silence from then on, the PB no longer running).
struct Accel {
    Pb& pb;
    u32 cur, end, loop;
    u16 fmt, pred;
    u32 hist;  // the ADPCM history: the last sample in the low half, the one before in the high
    bool looped, stream, stopped = false;

    explicit Accel(Pb& p) : pb(p) {
        cur = p.get32(PB_CUR), end = p.get32(PB_END), loop = p.get32(PB_LOOP);
        fmt = p.at(PB_FORMAT), pred = p.at(PB_PRED);
        hist = (u32)p.at(PB_YN2) << 16 | p.at(PB_YN1);
        looped = p.at(PB_LOOPFLAG) != 0, stream = p.at(PB_STREAM) != 0;
    }

    // the next n samples, a run at a time: up to the end address, and within one ADPCM frame or
    // one page of memory
    void read(s16* out, u32 n) {
        u8 tmp[2 * FETCH];
        while (n && !stopped) {
            u32 cnt = n;
            const u8* src = nullptr;
            switch (fmt) {
            case FMT_ADPCM:
                if ((cur & 15) == 0) {
                    pred = guest::load8(sample_addr(cur >> 1));
                    cur += 2;
                }
                cnt = std::min(cnt, (cur | 15) - cur + 1);
                break;
            case FMT_PCM16:
                src = ram(cur * 2);
                cnt = std::min(cnt, src ? page_room(cur * 2) / 2 : FETCH);
                break;
            case FMT_PCM8:
                src = ram(cur);
                cnt = std::min(cnt, src ? page_room(cur) : FETCH);
                break;
            }
            u32 left = end - cur;  // samples after this one through the end address
            bool at_end = left < cnt;
            if (at_end)
                cnt = left + 1;
            switch (fmt) {
            case FMT_ADPCM:
                adpcm(out, cnt, tmp);
                break;
            case FMT_PCM16: {
                if (!src)
                    src = fetch(cur * 2, 2 * cnt, tmp);
                u32 i = 0;
                for (; i + 2 <= cnt; i += 2) {  // (two samples a word)
                    u32 v;
                    memcpy(&v, src + 2 * i, 4);
                    v = rev16(v);
                    memcpy(out + i, &v, 4);
                }
                if (i < cnt)
                    out[i] = (s16)(src[2 * i] << 8 | src[2 * i + 1]);
                break;
            }
            case FMT_PCM8:
                if (!src)
                    src = fetch(cur, cnt, tmp);
                for (u32 i = 0; i < cnt; i++)
                    out[i] = (s16)((s8)src[i] * 256);
                break;
            default:
                memset(out, 0, cnt * sizeof(s16));
                break;
            }
            out += cnt, n -= cnt;
            if (!at_end) {
                cur += cnt;
            } else if (looped) {
                cur = loop;
                if (!stream) {
                    pred = pb.at(PB_LOOP_PRED);
                    hist = (u32)pb.at(PB_LOOP_YN2) << 16 | pb.at(PB_LOOP_YN1);
                }
            } else {
                cur = end;
                stopped = true;
                pb.at(PB_RUNNING) = 0;
            }
        }
        memset(out, 0, n * sizeof(s16));  // (past the end)
    }

    // DSP-ADPCM samples cur .. cur + n - 1 (in one frame): each nibble at the frame's scale plus
    // the previous two samples by the predictor's coefficient pair (5.11), rounded
    void adpcm(s16* out, u32 n, u8* tmp) {
        u32 first = cur >> 1;
        const u8* src = ram(first);
        if (!src)
            src = fetch(first, ((cur + n - 1) >> 1) - first + 1, tmp);
        u32 k = (pred >> 4) & 7;
        u32 coefs = (u32)pb.at(PB_COEFS + 4 * k + 2) << 16 | pb.at(PB_COEFS + 4 * k);  // (paired as hist)
        s32 scale = 2048 << (pred & 15);
        u32 h = hist;
        auto put = [&](s32 nib) {
            s32 s = sat16(smlad(coefs, h, nib * scale + 0x400) >> 11);
            h = pack((u32)s, h);
            *out++ = (s16)s;
        };
        // (a byte's nibbles: the high one is its sign-extended value >> 4)
        auto two = [&](s32 b) {
            put(b >> 4);
            put((s32)((u32)b << 28) >> 28);
        };
        const s8* in = (const s8*)src;
        if (n == 14 && !(cur & 1)) {  // a whole frame, as a rule
#pragma GCC unroll 7
            for (u32 i = 0; i < 7; i++)
                two(in[i]);
        } else {
            if (cur & 1)  // starting on a byte's low nibble
                put((s32)((u32)*in++ << 28) >> 28), n--;
            for (; n >= 2; n -= 2)
                two(*in++);
            if (n)
                put(*in >> 4);
        }
        hist = h;
    }

    void save() {
        pb.set32(PB_CUR, cur);
        pb.at(PB_PRED) = pred;
        pb.at(PB_YN1) = (u16)hist;
        pb.at(PB_YN2) = (u16)(hist >> 16);
    }
};

const u32 BLOCK = 512;  // input samples the converter holds

enum Env { ENV_ONE, ENV_FIXED, ENV_RAMP };  // the envelope at 1.0, at another volume, ramping

// out[i..j): interpolated from buf (buf[c .. c + 3] the last four samples), the position moving
// on by the ratio before each, then scaled by the envelope
template <Env ENV>
void interpolate(s16* out, u32 i, u32 j, const s16* buf, u32 c, u32 pos, u32 ratio, u16& ve_io, u16 ved) {
    u16 ve = ve_io;
    auto one = [&](u32 k) {
        pos += ratio, c += pos >> 16, pos &= 0xFFFF;
        const s16* h = buf + c;
        s32 s = (h[1] * (s32)(0x10000 - pos) + h[2] * (s32)pos) >> 16;
        if (ENV != ENV_ONE)
            s = sat16((s * (s32)ve) >> 15);
        if (ENV == ENV_RAMP)
            ve = (u16)(ve + ved);
        out[k] = (s16)s;
    };
    for (; i + 2 <= j; i += 2)  // (two at a time: the second's multiplies overlap the first's)
        one(i), one(i + 1);
    if (i < j)
        one(i);
    ve_io = ve;
}

// the sample rate converter and the volume envelope: AX_SPF output samples, the input position
// running in 16.16 steps of the ratio, linearly between the second- and third-newest input samples
// (the 4-tap filter's centre), then scaled by the envelope (1.15, unsigned, with a per-sample
// delta). False if the envelope mutes the voice (it is 0 and stays there): then only the input is
// read.
bool convert(Pb& pb, s16* out) {
    Accel acc(pb);
    u16 ve = pb.at(PB_VE), ved = pb.at(PB_VE + 2);
    bool heard = ve != 0 || ved != 0;
    Env env = ved != 0 ? ENV_RAMP : ve == 0x8000 ? ENV_ONE : ENV_FIXED;
    auto lerp = [&](u32 i, u32 j, const s16* buf, u32 c, u32 pos, u32 ratio) {
        if (env == ENV_ONE)
            interpolate<ENV_ONE>(out, i, j, buf, c, pos, ratio, ve, ved);
        else if (env == ENV_FIXED)
            interpolate<ENV_FIXED>(out, i, j, buf, c, pos, ratio, ve, ved);
        else
            interpolate<ENV_RAMP>(out, i, j, buf, c, pos, ratio, ve, ved);
    };
    s16 buf[4 + BLOCK];  // the last four samples, then the samples read
    for (u32 k = 0; k < 4; k++)
        buf[k] = (s16)pb.at(PB_LAST + 2 * k);
    u32 pos = pb.at(PB_FRAC);
    if (pb.at(PB_SRC_TYPE) == 2) {  // no conversion: one input sample per output sample (the
        acc.read(buf + 4, AX_SPF);  // ratio 1.0 with the position at 0 gives exactly that)
        buf[4 + AX_SPF] = 0;        // (the last output's next sample, at weight 0)
        if (heard)
            lerp(0, AX_SPF, buf, 2, 0, 0x10000);
        memmove(buf, buf + AX_SPF, 4 * sizeof(s16));
    } else {
        u32 ratio = pb.get32(PB_RATIO);
        for (u32 i = 0; i < AX_SPF;) {
            // the outputs from i on whose input fits in the buffer (as a rule all of them), the
            // samples they read, and the position after them
            u32 j = AX_SPF, n, to;
            u64 all = pos + (u64)ratio * (AX_SPF - i);
            if (ratio <= 0xFFFF0000u && all >> 16 <= BLOCK) {  // (no step wraps the position)
                n = (u32)(all >> 16), to = (u32)all & 0xFFFF;
            } else {
                n = 0, to = pos;
                for (j = i; j < AX_SPF; j++) {
                    u32 q = to + ratio;
                    if (j > i && n + (q >> 16) > BLOCK)
                        break;
                    n += q >> 16, to = q & 0xFFFF;
                }
            }
            u32 c = 0;  // buf[c .. c + 3]: the last four samples
            for (; n > BLOCK; n -= BLOCK, c -= BLOCK) {  // one step longer than the buffer
                acc.read(buf + 4, BLOCK);
                memmove(buf, buf + BLOCK, 4 * sizeof(s16));
            }
            acc.read(buf + 4, n);
            if (heard)
                lerp(i, j, buf, c, pos, ratio);
            i = j, pos = to;
            memmove(buf, buf + n, 4 * sizeof(s16));
        }
    }
    pb.at(PB_FRAC) = (u16)pos;
    for (u32 k = 0; k < 4; k++)
        pb.at(PB_LAST + 2 * k) = (u16)buf[k];
    pb.at(PB_VE) = ve;
    acc.save();
    return heard;
}

// add `in` to a bus at the volume in mix word `vol_off`, ramping by its delta if `ramp`; the last
// sample added goes to the depop word `dpop_off`. Per sample: in * volume >> 15, saturated, then
// the volume moves on by the delta. Nothing is added for silence (`heard` false, or volume 0
// without a ramp), and nothing saturates up to 1.0: one multiply-add, in * 2 volume >> 16.
void mix_add(Pb& pb, s32* out, const s16* in, bool heard, u32 vol_off, u32 dpop_off, bool ramp) {
    u16 vol = pb.at(vol_off);
    u16 delta = ramp ? pb.at(vol_off + 2) : 0;
    s32 last = 0;
    if (!heard || (vol == 0 && delta == 0)) {
        vol = (u16)(vol + AX_SPF * delta);
    } else if (delta == 0 && vol <= 0x8000) {
        s32 v2 = 2 * vol;
        for (u32 i = 0; i < AX_SPF; i += 2) {
            u32 two = pair(in + i);
            out[i] = smlawb(v2, two, out[i]);
            out[i + 1] = smlawt(v2, two, out[i + 1]);
        }
        last = smlawb(v2, (u16)in[AX_SPF - 1], 0);
    } else {
        for (u32 i = 0; i < AX_SPF; i++) {
            last = sat16((in[i] * (s32)vol) >> 15);
            out[i] += last;
            vol = (u16)(vol + delta);
        }
    }
    pb.at(vol_off) = vol;
    pb.at(dpop_off) = (u16)last;
}

// a bus's L and R at fixed volumes from 1/32768 to 1.0 (main L R, as a rule), in one pass:
// mix_add's result for both
void mix_add_lr(Pb& pb, s32* out_l, s32* out_r, const s16* in, u32 vol_l, u32 vol_r, u32 dpop_l, u32 dpop_r) {
    s32 l2 = 2 * pb.at(vol_l), r2 = 2 * pb.at(vol_r);
    for (u32 i = 0; i < AX_SPF; i += 2) {
        u32 two = pair(in + i);
        out_l[i] = smlawb(l2, two, out_l[i]);
        out_r[i] = smlawb(r2, two, out_r[i]);
        out_l[i + 1] = smlawt(l2, two, out_l[i + 1]);
        out_r[i + 1] = smlawt(r2, two, out_r[i + 1]);
    }
    u32 last = (u16)in[AX_SPF - 1];
    pb.at(dpop_l) = (u16)smlawb(l2, last, 0);
    pb.at(dpop_r) = (u16)smlawb(r2, last, 0);
}

void render(Pb& pb, AxBuses& bus) {
    alignas(4) s16 smp[AX_SPF];
    bool heard = convert(pb, smp);

    if (pb.at(PB_LPF)) {  // the one-pole low-pass
        if (!heard)
            memset(smp, 0, sizeof(smp)), heard = true;
        s32 a0 = pb.at(PB_LPF + 4), b0 = (s16)pb.at(PB_LPF + 6);
        s32 y = (s16)pb.at(PB_LPF + 2);
        for (u32 i = 0; i < AX_SPF; i++)
            y = smp[i] = (s16)sat16((a0 * smp[i] + b0 * y) >> 15);
        pb.at(PB_LPF + 2) = (u16)y;
    }
    if (pb.at(PB_BIQUAD)) {  // the biquad, coefficients in 2.14
        if (!heard)
            memset(smp, 0, sizeof(smp)), heard = true;
        s64 b0 = (s16)pb.at(PB_BIQUAD + 10), b1 = (s16)pb.at(PB_BIQUAD + 12), b2 = (s16)pb.at(PB_BIQUAD + 14);
        s64 a1 = (s16)pb.at(PB_BIQUAD + 16), a2 = (s16)pb.at(PB_BIQUAD + 18);
        s32 xn1 = (s16)pb.at(PB_BIQUAD + 2), xn2 = (s16)pb.at(PB_BIQUAD + 4);
        s32 yn1 = (s16)pb.at(PB_BIQUAD + 6), yn2 = (s16)pb.at(PB_BIQUAD + 8);
        for (u32 i = 0; i < AX_SPF; i++) {
            s32 x = smp[i];
            s64 t = (b0 * x + b1 * xn1 + b2 * xn2 + a1 * yn1 + a2 * yn2) << 2;
            t += (t & 0x10000) ? 0x8000 : 0x7FFF;
            s32 y = sat16((s32)(t >> 16));
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
        bool l = (bits & (1 | 4)) != 0, r = (bits & (2 | 4)) != 0;
        if (heard && l && r && !ramp && pb.at(o.mix_l) - 1u < 0x8000 && pb.at(o.mix_r) - 1u < 0x8000) {
            mix_add_lr(pb, bus[o.l], bus[o.r], smp, o.mix_l, o.mix_r, o.dpop_l, o.dpop_r);
        } else {
            if (l)
                mix_add(pb, bus[o.l], smp, heard, o.mix_l, o.dpop_l, ramp);
            if (r)
                mix_add(pb, bus[o.r], smp, heard, o.mix_r, o.dpop_r, ramp);
        }
        if (bits & (8 | 16))
            mix_add(pb, bus[o.s], smp, heard, o.mix_s, o.dpop_s, (bits & 16) != 0);
    }
}

}  // namespace

bool ax_render_voice(u32 pb_addr, AxBuses& bus) {
    u8* p = guest::host_ptr(pb_addr);
    if (!p || (p[PB_RUNNING] << 8 | p[PB_RUNNING + 1]) != 1)
        return false;
    Pb pb;
    pb.load(p);
    render(pb, bus);
    pb.store(p);
    return true;
}

}  // namespace engine::audio
