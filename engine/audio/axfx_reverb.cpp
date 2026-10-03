// AXFX's high-quality reverb (AXFXReverbHi, the 3-channel "Exp" kind nw4r's FxReverbHi uses),
// native. Per channel: early reflections (three taps of a delay line), a pre-delay, three
// parallel combs, two allpasses, a one-pole lowpass and a third allpass; then the channels cross-
// feed and go back to the bus. All of it in single precision, as the Wii does it, so the output
// is bit-identical; but the state stays in registers for a whole call (the original reloads ~20
// fields per sample), and the sample buffers are a template parameter: the AX callback's big-
// endian guest buffers now, the 3DS mixer's own buffers once the aux buses are routed there.
#include "axfx_reverb.h"

#include "noma_decomp.h"
#include "../snd/guest_obj.h"

namespace engine::audio {

using engine::snd::GuestObj;
using engine::snd::fctiwz;

namespace {

// AXFX_REVERBHI_EXP (the fields this uses)
enum : u32 {
    R_EARLY_LINE = 0x0,       // f32*[3]
    R_EARLY_POS = 0xC,        // u32[3]: three taps of each channel's early line
    R_EARLY_LEN = 0x18,
    R_EARLY_COEF = 0x20,      // f32[3]: the taps' gains
    R_PREDELAY_LINE = 0x2C,   // f32*[3]
    R_PREDELAY_POS = 0x38,
    R_PREDELAY_LEN = 0x3C,    // 0: no pre-delay
    R_COMB_LINE = 0x44,       // f32*[3][3]
    R_COMB_POS = 0x68,        // u32[3]
    R_COMB_LEN = 0x74,        // u32[3]
    R_COMB_COEF = 0x8C,       // f32[3]
    R_AP_LINE = 0x98,         // f32*[3][2]
    R_AP_POS = 0xB0,          // u32[2]
    R_AP_LEN = 0xB8,          // u32[2]
    R_LAST_AP_LINE = 0xC8,    // f32*[3]
    R_LAST_AP_POS = 0xD4,     // u32[3]
    R_LAST_AP_LEN = 0xE0,     // u32[3]
    R_AP_COEF = 0xF8,
    R_LPF_STATE = 0xFC,       // f32[3]
    R_LPF_COEF = 0x108,
    R_TEMP_DISABLE = 0x10C,
    R_CROSSTALK = 0x12C,
    R_FUSED_GAIN = 0x134,     // the reverb's share of the output
    R_BUS_IN = 0x138,         // AXFX_BUS*: {s32* left, right, surround}
    R_BUS_OUT = 0x13C,
    R_OUT_GAIN = 0x140,
    R_SEND_GAIN = 0x144,
};
const u32 FRAME = 96;  // samples in an AX frame

// a delay line: f32s in guest memory, read through a host pointer when the line is one block
// (positions past the end, which a corrupt state could hold, go through the guest as well)
struct Line {
    u32 base, len;
    u8* h;
    Line() : base(0), len(0), h(nullptr) {}
    Line(u32 base, u32 len) : base(base), len(len), h(guest::host_ptr(base)) {
        if (h && (len == 0 || len > 0x1000000 || guest::host_ptr(base + len * 4 - 1) != h + len * 4 - 1))
            h = nullptr;
    }
    f32 get(u32 i) const {
        u32 b;
        if (h && i < len) {
            const u8* p = h + i * 4;
            b = (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
        } else {
            b = guest::load32(base + i * 4);
        }
        f32 f;
        ::memcpy(&f, &b, 4);
        return f;
    }
    void set(u32 i, f32 f) const {
        u32 b;
        ::memcpy(&b, &f, 4);
        if (h && i < len) {
            u8* p = h + i * 4;
            p[0] = (u8)(b >> 24), p[1] = (u8)(b >> 16), p[2] = (u8)(b >> 8), p[3] = (u8)b;
        } else {
            guest::store32(base + i * 4, b);
        }
    }
};

// position i of a ring of length len, advanced as the original does (wraps to 0 at len)
inline u32 advance(u32 i, u32 len) { return ++i >= len ? 0 : i; }

}  // namespace

// three channels of s32 samples in guest memory (big-endian), as AX hands them to callbacks
struct GuestBus {
    u32 ch[3];
    s32 get(int c, u32 i) const { return (s32)guest::load32(ch[c] + i * 4); }
    void set(int c, u32 i, s32 v) const { guest::store32(ch[c] + i * 4, (u32)v); }
};

// one frame of n samples: `main` is reverberated in place; `in` (optional) is added to the input,
// `out` (optional) gets the wet signal at the send gain
template <class Bus>
void reverb_hi_process(u32 fx, const Bus& main, const Bus* in, const Bus* out, u32 n) {
    GuestObj r(fx);
    // the state, for the whole frame
    u32 early_pos[3], early_len = r.get32(R_EARLY_LEN);
    f32 early_coef[3];
    for (int k = 0; k < 3; k++)
        early_pos[k] = r.get32(R_EARLY_POS + 4 * k), early_coef[k] = r.getf(R_EARLY_COEF + 4 * k);
    u32 pre_pos = r.get32(R_PREDELAY_POS), pre_len = r.get32(R_PREDELAY_LEN);
    u32 comb_pos[3], comb_len[3];
    f32 comb_coef[3];
    for (int k = 0; k < 3; k++) {
        comb_pos[k] = r.get32(R_COMB_POS + 4 * k), comb_len[k] = r.get32(R_COMB_LEN + 4 * k);
        comb_coef[k] = r.getf(R_COMB_COEF + 4 * k);
    }
    u32 ap_pos[2], ap_len[2];
    for (int k = 0; k < 2; k++)
        ap_pos[k] = r.get32(R_AP_POS + 4 * k), ap_len[k] = r.get32(R_AP_LEN + 4 * k);
    u32 last_pos[3], last_len[3];
    f32 lpf[3];
    Line early[3], pre[3], comb[3][3], ap[3][2], last[3];
    for (int c = 0; c < 3; c++) {
        last_pos[c] = r.get32(R_LAST_AP_POS + 4 * c), last_len[c] = r.get32(R_LAST_AP_LEN + 4 * c);
        lpf[c] = r.getf(R_LPF_STATE + 4 * c);
        early[c] = Line(r.get32(R_EARLY_LINE + 4 * c), early_len);
        pre[c] = Line(r.get32(R_PREDELAY_LINE + 4 * c), pre_len);
        for (int k = 0; k < 3; k++)
            comb[c][k] = Line(r.get32(R_COMB_LINE + 0xC * c + 4 * k), comb_len[k]);
        for (int k = 0; k < 2; k++)
            ap[c][k] = Line(r.get32(R_AP_LINE + 8 * c + 4 * k), ap_len[k]);
        last[c] = Line(r.get32(R_LAST_AP_LINE + 4 * c), last_len[c]);
    }
    const f32 ap_coef = r.getf(R_AP_COEF);
    const f32 lpf_coef = r.getf(R_LPF_COEF);
    const f32 lpf_in = 1.0f - lpf_coef;
    const f32 fused = 0.6f * r.getf(R_FUSED_GAIN);
    const f32 crosstalk = 0.5f * r.getf(R_CROSSTALK);
    const f32 out_gain = r.getf(R_OUT_GAIN);
    const f32 send_gain = out ? r.getf(R_SEND_GAIN) : 0.0f;

    for (u32 i = 0; i < n; i++) {
        f32 wet[3];
        for (int c = 0; c < 3; c++) {
            s32 s = main.get(c, i);
            if (in)
                s = (s32)((u32)s + (u32)in->get(c, i));
            f32 x = (f32)s;
            // early reflections (the third tap is where this sample goes in)
            f32 e0 = early_coef[0] * early[c].get(early_pos[0]);
            f32 e2 = early[c].get(early_pos[2]);
            f32 e1 = early_coef[1] * early[c].get(early_pos[1]);
            early[c].set(early_pos[2], x);
            // pre-delay
            f32 d = x;
            if (pre_len) {
                d = pre[c].get(pre_pos);
                pre[c].set(pre_pos, x);
            }
            // three parallel combs
            f32 acc = 0.0f;
            for (int k = 0; k < 3; k++) {
                f32 y = comb[c][k].get(comb_pos[k]);
                acc = acc + y;
                comb[c][k].set(comb_pos[k], d + y * comb_coef[k]);
            }
            // two allpasses
            f32 a = ap[c][0].get(ap_pos[0]);
            f32 t = acc + a * ap_coef;
            ap[c][0].set(ap_pos[0], t);
            f32 b = ap[c][1].get(ap_pos[1]);
            f32 t2 = (a - t * ap_coef) + b * ap_coef;
            ap[c][1].set(ap_pos[1], t2);
            // lowpass
            f32 lp = lpf_in * (b - t2 * ap_coef) + lpf_coef * lpf[c];
            lpf[c] = lp;
            // the channel's own allpass
            f32 q = last[c].get(last_pos[c]);
            f32 t3 = lp + q * ap_coef;
            last[c].set(last_pos[c], t3);
            last_pos[c] = advance(last_pos[c], last_len[c]);
            f32 y = q - t3 * ap_coef;
            wet[c] = y * fused + (early_coef[2] * e2 + (e0 + e1));
        }
        // cross-feed: each channel gets half (times crosstalk) of the other two
        f32 left = wet[0] + (wet[1] + wet[2]) * crosstalk;
        f32 right = wet[1] + (wet[0] + wet[2]) * crosstalk;
        f32 surround = wet[2] + (wet[0] + wet[1]) * crosstalk;
        main.set(0, i, fctiwz(left * out_gain));
        main.set(1, i, fctiwz(right * out_gain));
        main.set(2, i, fctiwz(surround * out_gain));
        if (out) {
            out->set(0, i, fctiwz(left * send_gain));
            out->set(1, i, fctiwz(right * send_gain));
            out->set(2, i, fctiwz(surround * send_gain));
        }
        for (int k = 0; k < 3; k++)
            early_pos[k] = advance(early_pos[k], early_len);
        if (pre_len)
            pre_pos = advance(pre_pos, pre_len);
        for (int k = 0; k < 3; k++)
            comb_pos[k] = advance(comb_pos[k], comb_len[k]);
        for (int k = 0; k < 2; k++)
            ap_pos[k] = advance(ap_pos[k], ap_len[k]);
    }

    for (int k = 0; k < 3; k++) {
        r.set32(R_EARLY_POS + 4 * k, early_pos[k]);
        r.set32(R_COMB_POS + 4 * k, comb_pos[k]);
        r.set32(R_LAST_AP_POS + 4 * k, last_pos[k]);
        r.setf(R_LPF_STATE + 4 * k, lpf[k]);
    }
    r.set32(R_PREDELAY_POS, pre_pos);
    for (int k = 0; k < 2; k++)
        r.set32(R_AP_POS + 4 * k, ap_pos[k]);
}

template void reverb_hi_process<GuestBus>(u32, const GuestBus&, const GuestBus*, const GuestBus*, u32);
template void reverb_hi_process<HostBus>(u32, const HostBus&, const HostBus*, const HostBus*, u32);

}  // namespace engine::audio

// ---- the guest ABI (decl.h)
namespace g {

// AXFXReverbHiExpCallback's processing (the AX aux callback: buffers {left, right, surround}, the
// effect). Returns what the original leaves in r3, which no caller reads.
u32 AXFXReverbHiExp_Process(u32 bufs, u32 fx) {
    using namespace engine::audio;
    GuestObj r(fx);
    u32 disable = r.get32(R_TEMP_DISABLE);
    if (disable) {
        r.set32(R_TEMP_DISABLE, disable & ~2u);
        return bufs;
    }
    GuestBus main{{guest::load32(bufs), guest::load32(bufs + 4), guest::load32(bufs + 8)}};
    u32 ret = bufs;
    GuestBus in_bus, out_bus;
    u32 in_addr = r.get32(R_BUS_IN), out_addr = r.get32(R_BUS_OUT);
    if (in_addr) {
        in_bus = GuestBus{{guest::load32(in_addr), guest::load32(in_addr + 4), guest::load32(in_addr + 8)}};
        ret = in_bus.ch[1];
    }
    if (out_addr) {
        out_bus = GuestBus{{guest::load32(out_addr), guest::load32(out_addr + 4), guest::load32(out_addr + 8)}};
        ret = out_bus.ch[0] + FRAME * 4;
    }
    reverb_hi_process(fx, main, in_addr ? &in_bus : nullptr, out_addr ? &out_bus : nullptr, FRAME);
    // the original advances the bus-in pointers it read through (in its frame, not the bus)
    return ret;
}

}  // namespace g
