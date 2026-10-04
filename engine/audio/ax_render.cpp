// AX voices, native (ax_render.h).
//
// The comments define each stage sample by sample; the code works in blocks: the accelerator
// decodes runs of samples (to the end of an ADPCM frame, the end address, or what the frame
// needs) straight from guest memory, the converter interpolates from a buffer of them, and what
// can't be heard (a voice whose envelope is at zero, a bus at zero volume) is only accounted for.
// The PB is read and written in place, only the words a voice uses. The commonest voice (no
// filter, main L and R at fixed volumes up to 1.0) is interpolated, enveloped and mixed in one
// pass. On the 3DS the ARM11's DSP instructions do the arithmetic, chosen for its pipeline (ARM
// DDI 0360F, chapter 15): 16x16 and 32x16 multiplies issue in a cycle where MUL takes two and four
// to its result, so the ADPCM predictor and the converter's interpolation use SMLAxy/SMLAWy. The
// result is the same, bit for bit, as the sample-by-sample definition in ax_reference.h:
// NOMA_SELFTEST=ax checks that on random voices on the host, tools/armbench/armbench.py on the
// code the 3DS runs.
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
};
enum : u16 { FMT_ADPCM = 0x00, FMT_PCM16 = 0x0A, FMT_PCM8 = 0x19 };

// the ARM11's DSP instructions (one each on the 3DS), and what they compute
#ifdef AX_ARM_DSP
inline s32 sat16(s32 v) { return __ssat(v, 16); }
inline u32 rev16(u32 v) { return __rev16(v); }
inline s32 smlawb(s32 a, u32 b, s32 acc) { return __smlawb(a, (s32)b, acc); }
inline s32 smlawt(s32 a, u32 b, s32 acc) { return __smlawt(a, (s32)b, acc); }
inline s32 smlabb(u32 a, s32 b, s32 acc) { return __smlabb((s32)a, b, acc); }
inline s32 smlatb(u32 a, s32 b, s32 acc) { return __smlatb((s32)a, b, acc); }
inline s32 smulwb(s32 a, u32 b) {
    s32 r;
    asm("smulwb %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}
#else
// saturated to 16 bits
inline s32 sat16(s32 v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }
// the bytes of each 16-bit half swapped
inline u32 rev16(u32 v) { return (v & 0x00FF00FF) << 8 | (v >> 8 & 0x00FF00FF); }
// a times b's low (high) half, without the product's low 16 bits; plus acc (wrapping)
inline s32 smulwb(s32 a, u32 b) { return (s32)(((s64)a * (s16)b) >> 16); }
inline s32 smlawb(s32 a, u32 b, s32 acc) { return (s32)((u32)acc + (u32)smulwb(a, b)); }
inline s32 smlawt(s32 a, u32 b, s32 acc) { return (s32)((u32)acc + (u32)(s32)(((s64)a * (s16)(b >> 16)) >> 16)); }
// acc plus a's low (high) half times b's low half (wrapping)
inline s32 smlabb(u32 a, s32 b, s32 acc) { return (s32)((u32)acc + (u32)((s16)a * (s16)b)); }
inline s32 smlatb(u32 a, s32 b, s32 acc) { return (s32)((u32)acc + (u32)((s16)(a >> 16) * (s16)b)); }
#endif
// v, which the compiler must keep in a register (a mask it would otherwise put in an immediate,
// where it can't take the other operand shifted)
template <class T>
inline T opaque(T v) {
#ifdef AX_ARM_DSP
    asm("" : "+r"(v));
#endif
    return v;
}
inline u32 load32(const void* p) {
    u32 v;
    memcpy(&v, p, 4);
    return v;
}
inline void store32(void* p, u32 v) { memcpy(p, &v, 4); }
// two 16-bit samples as one word (the first in the low half)
inline u32 pair(const s16* p) { return load32(p); }

// the PB in guest memory, read and written in place: big-endian u16 words, 32-bit values as two
// of them (high first)
struct Pb {
    u8* p;
    u16 get(u32 off) const {
        u16 v;
        memcpy(&v, p + off, 2);
        return __builtin_bswap16(v);
    }
    void set(u32 off, u16 v) const {
        v = __builtin_bswap16(v);
        memcpy(p + off, &v, 2);
    }
    u32 get32(u32 off) const { return __builtin_bswap32(load32(p + off)); }
    void set32(u32 off, u32 v) const { store32(p + off, __builtin_bswap32(v)); }
    // the words at off and off + 2, the first in the low half
    u32 pair(u32 off) const { return rev16(load32(p + off)); }
    void set_pair(u32 off, u32 v) const { store32(p + off, rev16(v)); }
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

// One DSP-ADPCM sample: its nibble at the frame's scale plus the previous two samples (y1 the
// last) by the predictor's coefficient pair (5.11), rounded and saturated. t holds the nibble in
// bits 31..28 and zeros from 27 to 16, sc is 2^(15 + scale), so SMLAWT gives nibble * 2^(11 +
// scale) (t's high half is nibble * 4096); the coefficients are c1 in the low half, c2 in the high.
// The chain from one sample to the next is one 1-cycle multiply and the saturation: the c2 * y2
// product doesn't wait for the last sample.
inline s32 adpcm_sample(u32 t, s32 sc, u32 coefs, s32 y1, s32 y2) {
    s32 v = smlawt(sc, t, 0x400);
    v = smlatb(coefs, y2, v);
    v = smlabb(coefs, y1, v);
    return sat16(v >> 11);
}

// the accelerator. Per sample: at an ADPCM frame's header (a nibble address that is a multiple of
// 16) it takes the frame's predictor and scale and moves past the header; it reads the sample at
// the current address; then the address moves on, except at the end address (inclusive): there
// the voice loops (a looped sound that is not a stream restarts with the loop's ADPCM context) or
// stops (silence from then on, the PB no longer running).
struct Accel {
    Pb pb;
    u32 cur, end, loop;
    u16 fmt, pred;
    u32 hist;  // the ADPCM history: the last sample in the low half, the one before in the high
    bool looped, stream, stopped = false;

    explicit Accel(Pb p) : pb(p) {
        cur = p.get32(PB_CUR), end = p.get32(PB_END), loop = p.get32(PB_LOOP);
        fmt = p.get(PB_FORMAT), pred = p.get(PB_PRED);
        hist = p.pair(PB_YN1);
        looped = p.get(PB_LOOPFLAG) != 0, stream = p.get(PB_STREAM) != 0;
    }

    // after the end address: loop or stop
    void at_end() {
        if (looped) {
            cur = loop;
            if (!stream) {
                pred = pb.get(PB_LOOP_PRED);
                hist = pb.pair(PB_LOOP_YN1);
            }
        } else {
            cur = end;
            stopped = true;
            pb.set(PB_RUNNING, 0);
        }
    }

    // the next n samples, a run at a time: up to the end address, and within one ADPCM frame or
    // one page of memory
    void read(s16* out, u32 n) {
        if (fmt == FMT_ADPCM)
            return read_adpcm(out, n);
        u8 tmp[2 * FETCH];
        while (n && !stopped) {
            u32 cnt = n;
            const u8* src = nullptr;
            switch (fmt) {
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
            bool ends = left < cnt;
            if (ends)
                cnt = left + 1;
            switch (fmt) {
            case FMT_PCM16: {
                if (!src)
                    src = fetch(cur * 2, 2 * cnt, tmp);
                u32 i = 0;
                for (; i + 2 <= cnt; i += 2)  // (two samples a word)
                    store32(out + i, rev16(load32(src + 2 * i)));
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
            if (!ends)
                cur += cnt;
            else
                at_end();
        }
        memset(out, 0, n * sizeof(s16));  // (past the end)
    }

    // DSP-ADPCM, a frame (8 bytes: the header, then 14 nibbles) at a time; a frame never crosses a
    // page
    void read_adpcm(s16* out, u32 n) {
        alignas(8) u8 tmp[8];
        while (n && !stopped) {
            u32 fa = (cur >> 1) & ~7u;  // the frame's first byte
            const u8* f = ram(fa);
            u32 j = cur & 15;  // the nibble in the frame
            if (j == 0 && n >= 14 && f) {
                // whole frames, as many as the request, the page and the end address leave: an end
                // on a sample of frame m (its nibbles 2..15) ends the run before that frame; an end
                // on a header is never reached (the address moves past it)
                u32 dist = end - cur, frames = std::min(n / 14, page_room(fa) / 8);
                if ((dist & 15) >= 2)
                    frames = std::min(frames, dist >> 4);
                if (frames) {
                    frames_whole(f, frames, out);
                    cur += 16 * frames, out += 14 * frames, n -= 14 * frames;
                    continue;
                }
            }
            if (j == 0) {  // the header: predictor and scale for 14 samples
                pred = f ? f[0] : guest::load8(sample_addr(fa));
                cur += 2, j = 2;
            }
            u32 cnt = std::min(n, 16 - j);
            u32 left = end - cur;
            bool ends = left < cnt;
            if (ends)
                cnt = left + 1;
            if (!f) {  // (only the bytes the accelerator reads)
                memset(tmp, 0, 8);
                fetch(fa + (j >> 1), ((j + cnt - 1) >> 1) - (j >> 1) + 1, tmp + (j >> 1));
                f = tmp;
            }
            adpcm(f, j, cnt, out);
            out += cnt, n -= cnt;
            if (!ends)
                cur += cnt;
            else
                at_end();
        }
        memset(out, 0, n * sizeof(s16));  // (past the end)
    }

    // `frames` whole frames from f on: 14 samples each, every frame's header read
    void frames_whole(const u8* f, u32 frames, s16* out) {
        f = (const u8*)__builtin_assume_aligned(f, 8);
        s32 y1 = (s16)hist, y2 = (s16)(hist >> 16);
        const u32 M = opaque(0xF0000000u);
        u32 p = 0;
        do {
            u32 w0 = __builtin_bswap32(load32(f)), w1 = __builtin_bswap32(load32(f + 4));  // nibble k at bits 31 - 4k
            p = w0 >> 24;
            u32 coefs = pb.pair(PB_COEFS + 4 * ((p >> 4) & 7));
            s32 sc = 0x8000 << (p & 15);
            auto one = [&](u32 t) {
                s32 s = adpcm_sample(t & M, sc, coefs, y1, y2);
                y2 = y1, y1 = s;
                *out++ = (s16)s;
            };
            one(w0 << 8), one(w0 << 12), one(w0 << 16), one(w0 << 20), one(w0 << 24), one(w0 << 28);
            one(w1), one(w1 << 4), one(w1 << 8), one(w1 << 12), one(w1 << 16), one(w1 << 20), one(w1 << 24);
            one(w1 << 28);
            f += 8;
        } while (--frames);
        pred = (u16)p;
        hist = (u16)y1 | (u32)y2 << 16;
    }

    // samples j .. j + n - 1 of frame f (its nibbles j on, j >= 1, all within the frame)
    void adpcm(const u8* f, u32 j, u32 n, s16* out) {
        f = (const u8*)__builtin_assume_aligned(f, 8);
        u32 w0 = __builtin_bswap32(load32(f)), w1 = __builtin_bswap32(load32(f + 4));  // nibble k at bits 31 - 4k
        u32 coefs = pb.pair(PB_COEFS + 4 * ((pred >> 4) & 7));
        s32 sc = 0x8000 << (pred & 15);
        s32 y1 = (s16)hist, y2 = (s16)(hist >> 16);
        const u32 M = 0xF0000000u;
        auto one = [&](u32 t) {
            s32 s = adpcm_sample(t & M, sc, coefs, y1, y2);
            y2 = y1, y1 = s;
            *out++ = (s16)s;
        };
        if (j == 2 && n == 14) {  // a whole frame, as a rule
            one(w0 << 8), one(w0 << 12), one(w0 << 16), one(w0 << 20), one(w0 << 24), one(w0 << 28);
            one(w1), one(w1 << 4), one(w1 << 8), one(w1 << 12), one(w1 << 16), one(w1 << 20), one(w1 << 24);
            one(w1 << 28);
        } else {
            for (u32 e = j + n; j < e; j++)
                one(j < 8 ? w0 << 4 * j : w1 << 4 * (j - 8));
        }
        hist = (u16)y1 | (u32)y2 << 16;
    }

    void save() {
        pb.set32(PB_CUR, cur);
        pb.set(PB_PRED, pred);
        pb.set_pair(PB_YN1, hist);
    }
};

const u32 BLOCK = 512;  // input samples the converter holds

enum Env { ENV_ONE, ENV_FIXED, ENV_RAMP, ENV_LOUD };  // the envelope at 1.0, at another volume up to
                                                     // 1.0, ramping within 0 .. 1.0, above 1.0

// The converter's sample k: linear between buf[i + 1] and buf[i + 2] at the position's fraction f
// (x = i.f in 16.16), (h1 * (65536 - f) + h2 * f) >> 16. That is h1 + floor(d * f / 65536) with d =
// h2 - h1; SMLAWB takes f as signed, so for f >= 32768 it gives that minus d. buf1 and buf2 are
// buf + 1 and buf + 2, `even` is ~1 (the sample's byte offset is (x >> 15) & ~1: one instruction).
inline s32 lerp(const s16* buf1, const s16* buf2, u32 even, u32 x) {
    u32 off = (x >> 15) & even;
    s32 h1 = *(const s16*)((const u8*)buf1 + off), d = *(const s16*)((const u8*)buf2 + off) - h1;
    s32 s = smlawb(d, x, h1);
    if (x & 0x8000)
        s += d;
    return s;
}

// the envelope on a converter sample: (s * ve) >> 15, saturated (only a volume above 1.0 can
// leave 16 bits); ve2 is twice the volume
template <Env ENV>
inline s32 envelope(s32 s, s32 ve2) {
    if (ENV == ENV_ONE)
        return s;
    s32 e = smulwb(ve2, (u32)s);
    return ENV == ENV_LOUD ? sat16(e) : e;
}

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
bool convert(Pb pb, s16* out) {
    Accel acc(pb);
    u16 ve = pb.get(PB_VE), ved = pb.get(PB_VE + 2);
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
    store32(buf, pb.pair(PB_LAST)), store32(buf + 2, pb.pair(PB_LAST + 4));
    u32 pos = pb.get(PB_FRAC);
    if (pb.get(PB_SRC_TYPE) == 2) {  // no conversion: one input sample per output sample (the
        acc.read(buf + 4, AX_SPF);   // ratio 1.0 with the position at 0 gives exactly that)
        buf[4 + AX_SPF] = 0;         // (the last output's next sample, at weight 0)
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
    pb.set(PB_FRAC, (u16)pos);
    pb.set_pair(PB_LAST, pair(buf)), pb.set_pair(PB_LAST + 4, pair(buf + 2));
    pb.set(PB_VE, ve);
    acc.save();
    return heard;
}

// The commonest voice in one pass: converted, enveloped and added to main L and R at fixed
// volumes from 0 to 1.0 (vol_l2, vol_r2 twice those) without a buffer in between, each sample as
// convert, then mix_add_lr define it. Returns the last sample (for the depop words).
template <Env ENV>
s32 convert_mix_lr(const s16* buf, u32 x, u32 ratio, s32 ve2, s32 ved2, s32* out_l, s32* out_r, s32 vol_l2,
                   s32 vol_r2) {
    const s16 *buf1 = buf + 1, *buf2 = buf + 2;
    u32 even = opaque(~1u);
    s32 e = 0;
    for (u32 k = 0; k < AX_SPF; k++) {
        x += ratio;
        e = envelope<ENV>(lerp(buf1, buf2, even, x), ve2);
        if (ENV == ENV_RAMP)
            ve2 += ved2;
        out_l[k] = smlawb(vol_l2, (u32)e, out_l[k]);
        out_r[k] = smlawb(vol_r2, (u32)e, out_r[k]);
    }
    return e;
}

#ifdef AX_ARM_DSP
// convert_mix_lr for the 3DS, scheduled by hand for the ARM11's pipeline (GCC's code stalls on
// nearly every multiply and load). Software-pipelined: each step mixes sample k into L and R
// (out_r is out_l + 96) while it interpolates sample k + 1, so every load's and multiply's
// latency (3 cycles to the ALU, one more where the operand is shifted, a base or a multiplier)
// has independent work under it: 17 instructions, about 19 cycles a sample. The position's
// integer part indexes buf (MOVS also leaves the fraction's top bit in the carry: the + d of the
// interpolation); the envelope at 1.0 is ve2 = 0x10000 (SMULWB by it is exact). The ramp variant
// adds ved2 to the envelope each sample and keeps the loop's end on the stack.
extern "C" s32 ax_mix_lr_fixed(const s16* buf, u32 x, u32 ratio, s32 ve2, s32* out_l, s32 vol_l2, s32 vol_r2);
extern "C" s32 ax_mix_lr_ramp(const s16* buf, u32 x, u32 ratio, s32 ve2, s32* out_l, s32 vol_l2, s32 vol_r2,
                              s32 ved2);
#define AX_MIX_LR_FIRST /* sample 0's interpolation */ \
    "add    r1, r1, r2\n"                               \
    "movs   r11, r1, lsr #16\n"                         \
    "add    r11, r0, r11, lsl #1\n"                     \
    "ldrsh  r12, [r11, #2]\n"                           \
    "ldrsh  r11, [r11, #4]\n"                           \
    "sub    r11, r11, r12\n"                            \
    "smlawb r7, r11, r1, r12\n"                         \
    "addcs  r7, r7, r11\n"
#define AX_MIX_LR_LAST /* the last sample's mix */ \
    "smulwb r8, r3, r7\n"                          \
    "ldr    r9, [r4]\n"                            \
    "ldr    r10, [r4, #384]\n"                     \
    "smlawb r9, r5, r8, r9\n"                      \
    "smlawb r10, r6, r8, r10\n"                    \
    "str    r9, [r4]\n"                            \
    "str    r10, [r4, #384]\n"                     \
    "mov    r0, r8\n"
// registers: r0 buf, r1 x, r2 ratio, r3 ve2, r4 out_l (then the sample's L), r5 vol_l2, r6 vol_r2,
// r7 the interpolated sample, r8 the enveloped one, r9 L, r10 R, r11 the sample's address, then
// h2 and d, r12 h1, lr the loop's end (fixed) or ved2 (ramp)
asm(".section .text.ax_mix_lr_fixed,\"ax\",%progbits\n"
    ".global ax_mix_lr_fixed\n"
    ".type ax_mix_lr_fixed, %function\n"
    ".arm\n"
    ".align 3\n"
    "ax_mix_lr_fixed:\n"
    "push   {r4-r11, lr}\n"
    "ldr    r4, [sp, #36]\n"
    "ldr    r5, [sp, #40]\n"
    "ldr    r6, [sp, #44]\n"
    "add    lr, r4, #380\n"
    AX_MIX_LR_FIRST
    "1:\n"
    "smulwb r8, r3, r7\n"            // e = the envelope on sample k
    "add    r1, r1, r2\n"            // the position of sample k + 1
    "ldr    r9, [r4]\n"              // L[k]
    "movs   r11, r1, lsr #16\n"      // its integer part; the carry: fraction >= 0.5
    "ldr    r10, [r4, #384]\n"       // R[k]
    "add    r11, r0, r11, lsl #1\n"
    "smlawb r9, r5, r8, r9\n"        // L[k] + e * vol_l
    "ldrsh  r12, [r11, #2]\n"        // h1
    "ldrsh  r11, [r11, #4]\n"        // h2
    "smlawb r10, r6, r8, r10\n"      // R[k] + e * vol_r
    "str    r9, [r4], #4\n"
    "sub    r11, r11, r12\n"         // d = h2 - h1
    "smlawb r7, r11, r1, r12\n"      // h1 + d * fraction (taken as signed)
    "str    r10, [r4, #380]\n"
    "addcs  r7, r7, r11\n"           // + d where the fraction is >= 0.5
    "cmp    r4, lr\n"
    "bne    1b\n"
    AX_MIX_LR_LAST
    "pop    {r4-r11, pc}\n"
    ".size ax_mix_lr_fixed, .-ax_mix_lr_fixed\n"
    ".section .text.ax_mix_lr_ramp,\"ax\",%progbits\n"
    ".global ax_mix_lr_ramp\n"
    ".type ax_mix_lr_ramp, %function\n"
    ".arm\n"
    ".align 3\n"
    "ax_mix_lr_ramp:\n"
    "push   {r4-r11, lr}\n"
    "ldr    r4, [sp, #36]\n"
    "ldr    r5, [sp, #40]\n"
    "ldr    r6, [sp, #44]\n"
    "ldr    lr, [sp, #48]\n"
    "add    r8, r4, #380\n"
    "str    r8, [sp, #-8]!\n"        // (the loop's end; sp stays 8-byte aligned)
    AX_MIX_LR_FIRST
    "1:\n"
    "smulwb r8, r3, r7\n"
    "add    r1, r1, r2\n"
    "ldr    r9, [r4]\n"
    "movs   r11, r1, lsr #16\n"
    "ldr    r10, [r4, #384]\n"
    "add    r11, r0, r11, lsl #1\n"
    "smlawb r9, r5, r8, r9\n"
    "ldrsh  r12, [r11, #2]\n"
    "ldrsh  r11, [r11, #4]\n"
    "smlawb r10, r6, r8, r10\n"
    "str    r9, [r4], #4\n"
    "sub    r11, r11, r12\n"
    "ldr    r8, [sp]\n"
    "smlawb r7, r11, r1, r12\n"
    "str    r10, [r4, #380]\n"
    "add    r3, r3, lr\n"            // the envelope's next volume
    "addcs  r7, r7, r11\n"
    "cmp    r4, r8\n"
    "bne    1b\n"
    AX_MIX_LR_LAST
    "add    sp, sp, #8\n"
    "pop    {r4-r11, pc}\n"
    ".size ax_mix_lr_ramp, .-ax_mix_lr_ramp\n"
    ".text\n");
#endif

// render's fast path, for a voice that is converted (SRC type 0 or 1, every input in the buffer),
// heard, not filtered, mixed into main L and R only, at fixed volumes up to 1.0, with an envelope
// that stays within 0 .. 1.0 if it ramps: false (and nothing done) for any other voice
bool render_main_lr(Pb pb, AxBuses& bus) {
    if (pb.get32(PB_MIXCTRL) != 3 || pb.get(PB_LPF) || pb.get(PB_BIQUAD) || pb.get(PB_SRC_TYPE) == 2)
        return ax_paths[AXP_MIX]++, false;
    u32 vol_l = pb.get(0x14), vol_r = pb.get(0x18);
    u32 ratio = pb.get32(PB_RATIO), pos = pb.get(PB_FRAC);
    u64 all = pos + (u64)ratio * AX_SPF;
    if (vol_l > 0x8000 || vol_r > 0x8000 || ratio > 0xFFFF0000u || all >> 16 > BLOCK)
        return ax_paths[AXP_LEVELS]++, false;
    u32 ve = pb.get(PB_VE);
    s32 ved = (s16)pb.get(PB_VE + 2);
    Env env;
    if (ved == 0) {
        if (ve == 0)
            return ax_paths[AXP_MUTED]++, false;  // (muted: only the input is read, convert does that)
        env = ve == 0x8000 ? ENV_ONE : ve < 0x8000 ? ENV_FIXED : ENV_LOUD;
    } else {
        s32 last = (s32)ve + (s32)(AX_SPF - 1) * ved;  // (every step's volume within 0 .. 1.0)
        if (ve > 0x8000 || last < 0 || last > 0x8000)
            return ax_paths[AXP_LEVELS]++, false;
        env = ENV_RAMP;
    }
    ax_paths[env == ENV_ONE ? AXP_ONE : env == ENV_FIXED ? AXP_FIXED : env == ENV_RAMP ? AXP_RAMP : AXP_LOUD]++;
    Accel acc(pb);
    u32 n = (u32)(all >> 16);
    s16 buf[4 + BLOCK];  // the last four samples, then the samples read
    store32(buf, pb.pair(PB_LAST)), store32(buf + 2, pb.pair(PB_LAST + 4));
    acc.read(buf + 4, n);
    s32 *out_l = bus[AX_MAIN_L], *out_r = bus[AX_MAIN_R];
    s32 l2 = 2 * vol_l, r2 = 2 * vol_r, ve2 = 2 * ve, ved2 = 2 * ved, last;
#ifdef AX_ARM_DSP
    if (env == ENV_ONE || env == ENV_FIXED)  // (out_r is out_l + 96: AxBuses' layout)
        last = ax_mix_lr_fixed(buf, pos, ratio, ve2, out_l, l2, r2);
    else if (env == ENV_RAMP)
        last = ax_mix_lr_ramp(buf, pos, ratio, ve2, out_l, l2, r2, ved2);
    else
        last = convert_mix_lr<ENV_LOUD>(buf, pos, ratio, ve2, ved2, out_l, out_r, l2, r2);
#else
    switch (env) {
    case ENV_ONE: last = convert_mix_lr<ENV_ONE>(buf, pos, ratio, ve2, ved2, out_l, out_r, l2, r2); break;
    case ENV_FIXED: last = convert_mix_lr<ENV_FIXED>(buf, pos, ratio, ve2, ved2, out_l, out_r, l2, r2); break;
    case ENV_LOUD: last = convert_mix_lr<ENV_LOUD>(buf, pos, ratio, ve2, ved2, out_l, out_r, l2, r2); break;
    default: last = convert_mix_lr<ENV_RAMP>(buf, pos, ratio, ve2, ved2, out_l, out_r, l2, r2); break;
    }
#endif
    pb.set(PB_FRAC, (u16)all);
    pb.set_pair(PB_LAST, pair(buf + n)), pb.set_pair(PB_LAST + 4, pair(buf + n + 2));
    if (env == ENV_RAMP)
        pb.set(PB_VE, (u16)(ve + AX_SPF * ved));
    acc.save();
    pb.set(0x52, (u16)smlawb(l2, (u32)last, 0));  // the depop words: the last sample added
    pb.set(0x5A, (u16)smlawb(r2, (u32)last, 0));
    return true;
}

// add `in` to a bus at the volume in mix word `vol_off`, ramping by its delta if `ramp`; the last
// sample added goes to the depop word `dpop_off`. Per sample: in * volume >> 15, saturated, then
// the volume moves on by the delta. Nothing is added for silence (`heard` false, or volume 0
// without a ramp), and nothing saturates up to 1.0: one multiply-add, in * 2 volume >> 16.
void mix_add(Pb pb, s32* out, const s16* in, bool heard, u32 vol_off, u32 dpop_off, bool ramp) {
    u16 vol = pb.get(vol_off);
    u16 delta = ramp ? pb.get(vol_off + 2) : 0;
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
    pb.set(vol_off, vol);
    pb.set(dpop_off, (u16)last);
}

// a bus's L and R at fixed volumes from 1/32768 to 1.0 (main L R, as a rule), in one pass:
// mix_add's result for both
void mix_add_lr(Pb pb, s32* out_l, s32* out_r, const s16* in, u32 vol_l, u32 vol_r, u32 dpop_l, u32 dpop_r) {
    s32 l2 = 2 * pb.get(vol_l), r2 = 2 * pb.get(vol_r);
    for (u32 i = 0; i < AX_SPF; i += 2) {
        u32 two = pair(in + i);
        out_l[i] = smlawb(l2, two, out_l[i]);
        out_r[i] = smlawb(r2, two, out_r[i]);
        out_l[i + 1] = smlawt(l2, two, out_l[i + 1]);
        out_r[i + 1] = smlawt(r2, two, out_r[i + 1]);
    }
    u32 last = (u16)in[AX_SPF - 1];
    pb.set(dpop_l, (u16)smlawb(l2, last, 0));
    pb.set(dpop_r, (u16)smlawb(r2, last, 0));
}

void render(Pb pb, AxBuses& bus) {
    if (render_main_lr(pb, bus))
        return;
    alignas(4) s16 smp[AX_SPF];
    bool heard = convert(pb, smp);

    if (pb.get(PB_LPF)) {  // the one-pole low-pass
        if (!heard)
            memset(smp, 0, sizeof(smp)), heard = true;
        s32 a0 = pb.get(PB_LPF + 4), b0 = (s16)pb.get(PB_LPF + 6);
        s32 y = (s16)pb.get(PB_LPF + 2);
        for (u32 i = 0; i < AX_SPF; i++)
            y = smp[i] = (s16)sat16((a0 * smp[i] + b0 * y) >> 15);
        pb.set(PB_LPF + 2, (u16)y);
    }
    if (pb.get(PB_BIQUAD)) {  // the biquad, coefficients in 2.14
        if (!heard)
            memset(smp, 0, sizeof(smp)), heard = true;
        s64 b0 = (s16)pb.get(PB_BIQUAD + 10), b1 = (s16)pb.get(PB_BIQUAD + 12), b2 = (s16)pb.get(PB_BIQUAD + 14);
        s64 a1 = (s16)pb.get(PB_BIQUAD + 16), a2 = (s16)pb.get(PB_BIQUAD + 18);
        s32 xn1 = (s16)pb.get(PB_BIQUAD + 2), xn2 = (s16)pb.get(PB_BIQUAD + 4);
        s32 yn1 = (s16)pb.get(PB_BIQUAD + 6), yn2 = (s16)pb.get(PB_BIQUAD + 8);
        for (u32 i = 0; i < AX_SPF; i++) {
            s32 x = smp[i];
            s64 t = (b0 * x + b1 * xn1 + b2 * xn2 + a1 * yn1 + a2 * yn2) << 2;
            t += (t & 0x10000) ? 0x8000 : 0x7FFF;
            s32 y = sat16((s32)(t >> 16));
            xn2 = xn1, xn1 = x, yn2 = yn1, yn1 = y;
            smp[i] = (s16)y;
        }
        pb.set(PB_BIQUAD + 2, (u16)xn1), pb.set(PB_BIQUAD + 4, (u16)xn2);
        pb.set(PB_BIQUAD + 6, (u16)yn1), pb.set(PB_BIQUAD + 8, (u16)yn2);
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
        if (heard && l && r && !ramp && pb.get(o.mix_l) - 1u < 0x8000 && pb.get(o.mix_r) - 1u < 0x8000) {
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

u32 ax_paths[AXP_COUNT];

bool ax_render_voice(u32 pb_addr, AxBuses& bus) {
    u8* p = guest::host_ptr(pb_addr);
    if (!p || (p[PB_RUNNING] << 8 | p[PB_RUNNING + 1]) != 1)
        return false;
    render(Pb{p}, bus);
    return true;
}

}  // namespace engine::audio
