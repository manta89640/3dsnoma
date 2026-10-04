// nw4r::snd::detail::AxVoice and AxVoiceParamBlock (snd_AxVoice.cpp), and the SDK's
// AXSetVoiceState and AXGetLpfCoefs, native. See ax_voice.h.
//
// The originals bracket every change with OSDisableInterrupts/OSRestoreInterrupts so the AX
// interrupt never sees a half-written block. Here interrupts are only ever taken at the
// decompiled code's poll points (runtime/irq.cpp), none of which native code passes, so a native
// function is atomic as it stands.
#include "noma_decomp.h"
#include "ax_voice.h"
#include "tables.h"

namespace engine::snd {

// (a function-local static: its address never changes once GetInstance has constructed it)
u32 ax_manager() {
    static u32 s_instance;
    if (!s_instance)
        s_instance = g::nw4r_snd_detail_AxManager_GetInstance(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    return s_instance;
}
u8* ax_manager_ptr() {
    static u8* s_p;
    if (!s_p)
        s_p = GuestObj(ax_manager()).p;
    return s_p;
}

// AXPBMIX order of a MixParam's volumes (MixParam: L R S per bus; AXPBMIX: the L R pairs of all
// buses first, then the S of each)
static const u8 kMixToAx[12] = {0, 1, 8, 2, 3, 9, 4, 5, 10, 6, 7, 11};

// mixerCtrl bits for a nonzero volume (even index) or step (odd index) in AXPBMIX order
static const u32 kMixerCtrlBits[24] = {
    0x1,        0x5,        0x2,        0x6,         // L, R
    0x10000,    0x50000,    0x20000,    0x60000,     // aux A L, R
    0x200000,   0xA00000,   0x400000,   0xC00000,    // aux B L, R
    0x4000000,  0x14000000, 0x8000000,  0x18000000,  // aux C L, R
    0x8,        0x18,       0x80000,    0x180000,    // S, aux A S
    0x1000000,  0x3000000,  0x20000000, 0x60000000,  // aux B S, aux C S
};

// AxVoiceParamBlock::SetVoiceMix: the 12 volumes and their steps (AXPBMIX order); immediately
// marks the AXVPB itself, otherwise the flags wait for Sync
static void pb_set_voice_mix(GuestObj ax, const u16 mix[24], bool immediate) {
    GuestObj pb(ax.get32(AXV_PB));
    if (!pb)
        return;
    u32 ctrl = 0;
    for (int i = 0; i < 24; i++) {
        pb.set16(PB_MIX + 2 * i, mix[i]);
        if (mix[i])
            ctrl |= kMixerCtrlBits[i];
    }
    pb.set32(PB_MIXER_CTRL, ctrl);
    if (immediate)
        pb.or32(PB_SYNC, 0x12);
    else
        ax.or32(AXV_SYNC, 0x12);
}

// AxVoiceParamBlock::SetVoiceRmtMix: 16 volumes and steps, 4 speakers x (main, aux)
static void pb_set_rmt_mix(GuestObj ax, const u16 mix[16]) {
    GuestObj pb(ax.get32(AXV_PB));
    if (!pb)
        return;
    u32 ctrl = 0;
    for (int i = 0; i < 16; i++) {
        pb.set16(PB_RMT_MIX + 2 * i, mix[i]);
        if (mix[i])
            ctrl |= 1u << i;
    }
    pb.set16(PB_RMT_MIXER_CTRL, (u16)ctrl);
    ax.or32(AXV_SYNC, 0x3000000);
}

// AxVoiceParamBlock::SetVoiceRmtIIRCoefs: type 1 (lowpass: 2 coefficients) or 2 (biquad: 5)
static void pb_set_rmt_iir_coefs(GuestObj ax, u32 type, const u16* c) {
    GuestObj pb(ax.get32(AXV_PB));
    if (!pb)
        return;
    if (type == 1) {
        pb.set16(0x140, c[0]);
        pb.set16(0x142, c[1]);
        ax.or32(AXV_SYNC, 0x20000000);
    } else if (type == 2) {
        for (int i = 0; i < 5; i++)
            pb.set16(0x146 + 2 * i, c[i]);
        ax.or32(AXV_SYNC, 0x40000000);
    }
}

static void pb_copy(GuestObj pb, u32 off, const u16* v, int n) {
    for (int i = 0; i < n; i++)
        pb.set16(off + 2 * i, v[i]);
}

// the pitch: the playback ratio from the sample rate (16.16 fixed point); initial: the whole
// sample-rate converter state, at its start
void ax_set_src(GuestObj ax, bool initial, f32 pitch) {
    GuestObj pb(ax.get32(AXV_PB));
    if (!pb)
        return;
    f32 ratio = (pitch * (f32)ax.gets32(AXV_RATE)) / 32000.0f;
    if (initial) {
        if (!(ratio > 65535.0f))
            ratio = ratio < 0.0f ? 0.0f : ratio;
        else
            ratio = 65535.0f;
        u32 r = fp2u(65536.0f * ratio);
        const u16 src[7] = {(u16)(r >> 16), (u16)r, 0, 0, 0, 0, 0};
        pb_copy(pb, PB_SRC, src, 7);
        ax.set32(AXV_SYNC, (ax.get32(AXV_SYNC) & ~0x20000u) | 0x10000);
    } else {
        u32 r = fp2u(65536.0f * ratio);
        pb.set16(PB_SRC, (u16)(r >> 16));
        pb.set16(PB_SRC + 2, (u16)r);
        u32 sync = ax.get32(AXV_SYNC);
        if (!(sync & 0x10000))
            ax.set32(AXV_SYNC, sync | 0x20000);
    }
}

static u16 ve_volume(f32 v) {
    s32 x = fctiwz(32767.0f * v);
    return (u16)(x > 0xFFFF ? 0xFFFF : x < 0 ? 0 : x);
}

// the volume envelope's target (and its start, for the first one after a voice starts)
void ax_set_ve(GuestObj ax, f32 volume, f32 init_volume) {
    if (!ax.get32(AXV_PB))
        return;
    u16 v = ve_volume(volume), init = ve_volume(init_volume);
    if (ax.get8(AXV_VE_FIRST)) {
        ax.set16(AXV_VE_VOL, init);
        ax.set8(AXV_VE_FIRST, 0);
    }
    ax.set16(AXV_VE_TARGET, v);
}

// big-endian 16-bit words in pairs: a word's bytes swapped in each half (REV16)
static inline u32 rev16(u32 v) { return (v & 0x00FF00FFu) << 8 | (v >> 8 & 0x00FF00FFu); }
// (one word access where p is 4-byte aligned, two halves where it isn't: about what an unaligned
// word access costs the ARM11 anyway, without depending on it)
static inline u32 load_pair(const u8* p) {
    u32 v;
    if (((uintptr_t)p & 3) == 0) {
        memcpy(&v, p, 4);
    } else {
        u16 a, b;
        memcpy(&a, p, 2), memcpy(&b, p + 2, 2);
        v = (u32)a | (u32)b << 16;
    }
    return rev16(v);  // (the first word in the low half)
}
static inline void store_pair(u8* p, u32 lo, u32 hi) {
    u32 v = rev16((lo & 0xFFFF) | hi << 16);
    if (((uintptr_t)p & 3) == 0) {
        memcpy(p, &v, 4);
    } else {
        u16 a = (u16)v, b = (u16)(v >> 16);
        memcpy(p, &a, 2), memcpy(p + 2, &b, 2);
    }
}

// the mix volumes: each ramps toward its target over AX frames, by whole steps per sample. (Its
// volumes and steps, AxVoiceParamBlock::SetVoiceMix's, go to the AXVPB a pair of words at a time.)
bool ax_set_mix(GuestObj ax, const MixParam& mix) {
    GuestObj pb(ax.get32(AXV_PB));
    if (!pb)
        return false;
    u8* axm = ax.p + AXV_MIX;
    if (ax.get8(AXV_MIX_FIRST) || pb.get16(PB_STATE) != 1) {  // no ramp: start at the targets
        for (int k = 0; k < 12; k += 2)
            store_pair(axm + 2 * k, mix.v[k], mix.v[k + 1]);
        ax.set8(AXV_MIX_FIRST, 0);
    }
    u16 cur[12];
    s32 step[12];
    bool changed = false;
    for (int k = 0; k < 12; k += 2) {
        u32 two = load_pair(axm + 2 * k);
        cur[k] = (u16)two, cur[k + 1] = (u16)(two >> 16);
        changed |= two != ((u32)mix.v[k] | (u32)mix.v[k + 1] << 16);
    }
    u32 ctrl = 0;
    u8* pbm = pb.p + PB_MIX;
    if (!changed) {  // (as a rule: at the targets, no steps, and the volumes reached stay)
        u32 words[12];   // the (volume, 0) pairs in AXPBMIX order, as guest memory holds them
#pragma GCC unroll 12
        for (int k = 0; k < 12; k++) {
            u32 i = kMixToAx[k];
            words[i] = __builtin_bswap16(cur[k]);
            if (cur[k])
                ctrl |= kMixerCtrlBits[2 * i];
        }
        if (((uintptr_t)pbm & 3) == 0) {
            memcpy(pbm, words, sizeof(words));
        } else {
            for (int i = 0; i < 12; i++)
                memcpy(pbm + 4 * i, words + i, 2), memset(pbm + 4 * i + 2, 0, 2);
        }
        pb.set32(PB_MIXER_CTRL, ctrl);
        ax.or32(AXV_SYNC, 0x12);
        return false;
    }
    for (int k = 0; k < 12; k++) {
        u32 c = cur[k], m = mix.v[k];
        s32 d = c == m ? 0 : div96((s32)m - (s32)c);
        step[k] = d;
        u32 i = kMixToAx[k];
        store_pair(pbm + 4 * i, c, (u32)(u16)(s16)d);
        if (c)
            ctrl |= kMixerCtrlBits[2 * i];
        if ((u16)d)
            ctrl |= kMixerCtrlBits[2 * i + 1];
    }
    pb.set32(PB_MIXER_CTRL, ctrl);
    ax.or32(AXV_SYNC, 0x12);
    for (int k = 0; k < 12; k += 2) {
        u32 a = mix.v[k] == 0 || step[k] == 0 ? mix.v[k] : (u16)(cur[k] + step[k] * 0x60);
        u32 b = mix.v[k + 1] == 0 || step[k + 1] == 0 ? mix.v[k + 1] : (u16)(cur[k + 1] + step[k + 1] * 0x60);
        store_pair(axm + 2 * k, a, b);
    }
    return changed;
}

void ax_set_rmt_mix(GuestObj ax, const RemoteMixParam& mix) {
    u16 v[16];
    for (int i = 0; i < 8; i++) {
        v[2 * i] = mix.v[i];
        v[2 * i + 1] = 0;
    }
    pb_set_rmt_mix(ax, v);
}

void ax_enable_remote(GuestObj ax, u16 on) {
    GuestObj pb(ax.get32(AXV_PB));
    if (!pb)
        return;
    pb.set16(PB_REMOTE, on);
    ax.or32(AXV_SYNC, 0x800000);
}

// the SDK's lowpass coefficients for a cutoff (Hz): a0 = 0x7FFF - b0. Kept in double precision,
// as the decompiled SDK computes them: the coefficients are truncated, and this runs only when a
// voice's cutoff changes.
static void lpf_coefs(u16 freq, u16& a0, u16& b0) {
    f32 c = (f32)::cos((f64)((6.2831855f * (f32)freq) / 32000.0f));
    f32 t = 2.0f - c;
    f32 s = (f32)::sqrt((f64)(t * t - 1.0f));
    s32 b = fctiwz(32768.0f * -(s - t));
    b0 = (u16)b;
    a0 = (u16)(0x7FFF - (u16)b);
}

// the lowpass filter: off at 16 kHz and above
void ax_set_lpf(GuestObj ax, u16 freq) {
    GuestObj pb(ax.get32(AXV_PB));
    if (!pb)
        return;
    if (freq >= 16000) {
        // (the original copies two stack words it never wrote as a0, b0: unused while off)
        const u16 off[4] = {0, 0, 0, 0};
        pb_copy(pb, PB_LPF, off, 4);
        ax.or32(AXV_SYNC, 0x80000);
        return;
    }
    u16 a0, b0;
    lpf_coefs(freq, a0, b0);
    if (pb.get16(PB_LPF) == 1) {  // on already: new coefficients only
        pb.set16(PB_LPF + 4, a0);
        pb.set16(PB_LPF + 6, b0);
        ax.or32(AXV_SYNC, 0x100000);
    } else {
        const u16 lpf[4] = {1, 0, a0, b0};
        pb_copy(pb, PB_LPF, lpf, 4);
        ax.or32(AXV_SYNC, 0x80000);
    }
}

// the biquad filter of a registered type (AxManager's table of BiquadFilterCallback objects,
// whose GetCoef virtual makes the coefficients)
void ax_set_biquad(GuestObj ax, u8 type, f32 value) {
    GuestObj pb(ax.get32(AXV_PB));
    if (!pb)
        return;
    ax_manager();
    u32 filter = type ? guest::load32(GA_lbl_809633A0 + 4 * type) : 0;
    if (!filter) {
        // (the original leaves the coefficients as stack words it never wrote: unused while off)
        const u16 off[10] = {};
        pb_copy(pb, PB_BIQUAD, off, 10);
        ax.or32(AXV_SYNC, 0x200000);
        return;
    }
    u16 c[5];
    {
        GuestFrame frame(0x20);  // the coefficients, written by guest code
        u32 coefs = frame.sp + 8;
        u32 get_coef = guest::load32(guest::load32(filter) + 0xC);
        gcall<u32>(get_coef, filter, (u32)type, coefs, (f64)value);
        for (int i = 0; i < 5; i++)
            c[i] = guest::load16(coefs + 2 * i);
    }
    if (pb.get16(PB_BIQUAD) == 2) {  // on already: new coefficients only
        pb_copy(pb, PB_BIQUAD + 10, c, 5);
        ax.or32(AXV_SYNC, 0x400000);
    } else {
        const u16 bq[10] = {2, 0, 0, 0, 0, c[0], c[1], c[2], c[3], c[4]};
        pb_copy(pb, PB_BIQUAD, bq, 10);
        ax.or32(AXV_SYNC, 0x200000);
    }
}

// the remote speaker's filter (0: off), a biquad from nw4r's table (by the value clamped to 0..127)
void ax_set_remote_filter(GuestObj ax, s32 value) {
    GuestObj pb(ax.get32(AXV_PB));
    if (!pb)
        return;
    if (value == 0) {
        // (the original writes only `on`; the rest are stack words it never wrote)
        const u16 off[10] = {};
        pb_copy(pb, PB_RMT_IIR, off, 10);
        ax.or32(AXV_SYNC, 0x10000000);
        return;
    }
    const u16* c = kRemoteFilterTable[value > 0x7F ? 0x7F : value < 0 ? 0 : value];
    if (pb.get16(PB_RMT_IIR) == 2) {
        pb_set_rmt_iir_coefs(ax, 2, c);
    } else {
        const u16 iir[10] = {2, 0, 0, 0, 0, c[0], c[1], c[2], c[3], c[4]};
        pb_copy(pb, PB_RMT_IIR, iir, 10);
        ax.or32(AXV_SYNC, 0x10000000);
    }
}

// hands the collected changes to the AXVPB: the volume envelope's ramp toward its target for the
// next frame (a step per sample, the one of 96 steps that gets closest), and the sync flags
void ax_sync(GuestObj ax) {
    GuestObj pb(ax.get32(AXV_PB));
    if (!pb)
        return;
    s32 cur = ax.get16(AXV_VE_VOL), target = ax.get16(AXV_VE_TARGET);
    s16 d = (s16)div96(target - cur);
    s32 unit = d == 0 ? -1 : 1;  // (as the original computes it)
    s32 err_unit = target - (cur + unit * 0x60), err_d = target - (cur + d * 0x60);
    if (err_unit < 0)
        err_unit = -err_unit;
    if (err_d < 0)
        err_d = -err_d;
    s16 delta = (s16)(err_d < err_unit ? d : unit);
    s32 end = cur + delta * 0x60;
    if (end < 0)
        delta = (s16)div96(-cur);
    else if (end > 0xFFFF)
        delta = (s16)div96(0xFFFF - cur);
    u16 ve = delta == 0 && ax.gets16(AXV_VE_DELTA) == 0 ? (u16)target : (u16)cur;
    store_pair(pb.p + PB_VE, ve, (u16)delta);
    u32 sync = (ax.get32(AXV_SYNC) & ~0x200u) | 0x100;
    store_pair(ax.p + AXV_VE_VOL, ve, (u16)delta);
    pb.or32(PB_SYNC, sync);
    ax.set32(AXV_SYNC, 0);
}

// a one-shot voice has finished when it plays the zero buffer AxManager points finished voices at
bool ax_is_play_finished(GuestObj ax) {
    if (!ax.get32(AXV_DATA))
        return false;
    u32 pb = ax.get32(AXV_PB);
    u32 cur = pb ? (u32)guest::load16(pb + PB_ADDR_CUR) << 16 | guest::load16(pb + PB_ADDR_CUR + 2) : 0;
    u32 zero = GuestObj(ax_manager()).get32(0x4);
    if (zero)
        zero += 0x80000000u;  // physical
    u32 start, len;
    switch (ax.gets32(AXV_FORMAT)) {
    case 3: start = zero * 2 + 2, len = 0x200; break;
    case 2: start = zero, len = 0x100; break;
    case 1: start = zero >> 1, len = 0x80; break;
    default: return false;
    }
    return start <= cur && cur < start + len;
}

// ends the ramps where they are: the envelope's current step applied, the mix at its last values
void ax_reset_delta(GuestObj ax) {
    GuestObj pb(ax.get32(AXV_PB));
    if (!pb)
        return;
    u16 vol = (u16)(ax.get16(AXV_VE_VOL) + (u16)(pb.gets16(PB_VE + 2) * 0x60));
    ax.set16(AXV_VE_VOL, vol);
    pb.set16(PB_VE, vol);
    pb.set16(PB_VE + 2, 0);
    pb.or32(PB_SYNC, 0x100);
    u16 axmix[24];
    for (int k = 0; k < 12; k++) {
        axmix[2 * kMixToAx[k]] = ax.get16(AXV_MIX + 2 * k);
        axmix[2 * kMixToAx[k] + 1] = 0;
    }
    pb_set_voice_mix(ax, axmix, true);
}

void ax_set_voice_state(GuestObj pb, u16 state) {
    if (pb.get16(PB_STATE) == state)
        return;
    pb.set16(PB_STATE, state);
    pb.or32(PB_SYNC, 4);
    if (state == 0)
        pb.set32(PB_DEPOP, 1);
}

}  // namespace engine::snd

// ---- the guest ABI (decl.h)
namespace g {

using namespace engine::snd;

static MixParam load_mix(u32 a) {
    GuestObj m(a);
    MixParam p;
    for (int k = 0; k < 12; k++)
        p.v[k] = m.get16(2 * k);
    return p;
}

void nw4r_snd_detail_AxVoice_SetSrc(u32 ax, u32 initial, f64 pitch) {
    ax_set_src(GuestObj(ax), initial != 0, (f32)pitch);
}

void nw4r_snd_detail_AxVoice_SetVe(u32 ax, f64 volume, f64 init_volume) {
    ax_set_ve(GuestObj(ax), (f32)volume, (f32)init_volume);
}

u32 nw4r_snd_detail_AxVoice_SetMix(u32 ax, u32 mix) {
    return ax_set_mix(GuestObj(ax), load_mix(mix));
}

// whether the mix volumes reached differ from these
u32 nw4r_snd_detail_AxVoice_IsMixChanged(u32 ax, u32 mix) {
    GuestObj a(ax), m(mix);
    for (int k = 0; k < 12; k++)
        if (a.get16(AXV_MIX + 2 * k) != m.get16(2 * k))
            return 1;
    return 0;
}

void nw4r_snd_detail_AxVoiceParamBlock_SetVoiceMix(u32 ax, u32 mix, u32 immediate) {
    GuestObj m(mix);
    u16 v[24];
    for (int i = 0; i < 24; i++)
        v[i] = m.get16(2 * i);
    pb_set_voice_mix(GuestObj(ax), v, immediate != 0);
}

void nw4r_snd_detail_AxVoice_SetRmtMix(u32 ax, u32 mix) {
    GuestObj m(mix);
    RemoteMixParam p;
    for (int i = 0; i < 8; i++)
        p.v[i] = m.get16(2 * i);
    ax_set_rmt_mix(GuestObj(ax), p);
}

void nw4r_snd_detail_AxVoiceParamBlock_SetVoiceRmtMix(u32 ax, u32 mix) {
    GuestObj m(mix);
    u16 v[16];
    for (int i = 0; i < 16; i++)
        v[i] = m.get16(2 * i);
    pb_set_rmt_mix(GuestObj(ax), v);
}

// (ax, type, coefficients...): a variadic function; its coefficients come in r5..r9
void nw4r_snd_detail_AxVoiceParamBlock_SetVoiceRmtIIRCoefs(u32 ax, u32 type, u32 c0, u32 c1, u32 c2, u32 c3, u32 c4,
                                                           u32, f64, f64, f64, f64, f64, f64, f64, f64) {
    const u16 c[5] = {(u16)c0, (u16)c1, (u16)c2, (u16)c3, (u16)c4};
    pb_set_rmt_iir_coefs(GuestObj(ax), type, c);
}

void nw4r_snd_detail_AxVoice_EnableRemote(u32 ax, u32 on) {
    ax_enable_remote(GuestObj(ax), (u16)on);
}

void nw4r_snd_detail_AxVoice_SetLpf(u32 ax, u32 freq) {
    // (freq is a u16 parameter: the comparison with 16000 sees the whole register, as here)
    if (freq >= 16000)
        ax_set_lpf(GuestObj(ax), 16000);
    else
        ax_set_lpf(GuestObj(ax), (u16)freq);
}

void nw4r_snd_detail_AxVoice_SetBiquad(u32 ax, u32 type, f64 value) {
    ax_set_biquad(GuestObj(ax), (u8)type, (f32)value);
}

void nw4r_snd_detail_AxVoice_SetRemoteFilter(u32 ax, u32 value) {
    ax_set_remote_filter(GuestObj(ax), (s32)value);
}

void nw4r_snd_detail_AxVoice_Sync(u32 ax) {
    ax_sync(GuestObj(ax));
}

u32 nw4r_snd_detail_AxVoice_IsPlayFinished(u32 ax) {
    return ax_is_play_finished(GuestObj(ax));
}

void nw4r_snd_detail_AxVoice_ResetDelta(u32 ax) {
    ax_reset_delta(GuestObj(ax));
}

u32 AXSetVoiceState(u32 pb, u32 state) {
    ax_set_voice_state(GuestObj(pb), (u16)state);
    return 0;
}

void AXGetLpfCoefs(u32 freq, u32 a0_out, u32 b0_out) {
    u16 a0, b0;
    engine::snd::lpf_coefs((u16)freq, a0, b0);
    guest::store16(b0_out, b0);
    guest::store16(a0_out, a0);
}

}  // namespace g
