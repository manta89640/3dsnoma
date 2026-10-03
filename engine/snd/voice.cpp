// nw4r::snd::detail::Voice and VoiceManager::UpdateAllVoices (snd_Voice.cpp, snd_VoiceManager.cpp)
// and the curves of snd_Util.cpp, native: the sound thread's work for every playing voice, every
// 3 ms AX frame -- a sequenced song plays each note as a voice, so this is most of the thread.
// The voices' AX voices are driven through engine/snd/ax_voice.cpp.
#include "noma_decomp.h"
#include "runtime.h"
#include "voice.h"
#include "tables.h"

namespace engine::snd {

namespace {

// AxManager
enum : u32 {
    AM_OUTPUT_MODE = 0x0,  // 0 stereo, 1 surround, 2 Dolby Pro Logic II, 3 mono
    AM_VOLUME_FROM = 0x1C,  // the master volume, fading
    AM_VOLUME_TO = 0x20,
    AM_VOLUME_FRAMES = 0x24,
    AM_VOLUME_FRAME = 0x28,
};

inline f32 clamp(f32 x, f32 lo, f32 hi) { return x > hi ? hi : x < lo ? lo : x; }

struct Voice {
    GuestObj v;
    s32 channels, outs;

    explicit Voice(u32 a) : v(a), channels(v.gets32(V_CHANNELS)), outs(v.gets32(V_OUTS)) {}
    GuestObj ax(s32 ch, s32 out) const { return GuestObj(v.get32(V_AXVOICE + ch * 0x10 + out * 4)); }
    f32 out_param(s32 out, u32 field) const { return v.getf(V_OUT + out * 0x18 + field); }
    void clear_sync(u16 bit) const { v.set16(V_SYNC, v.get16(V_SYNC) & ~bit); }
};

// a pan curve's choice of table and its options (PanInfo)
struct PanInfo {
    s32 curve;
    bool center_zero;  // 0 dB at the center
    bool zero_clamp;   // at most 1
};

PanInfo pan_info(u32 pan_curve) {
    static const PanInfo kCurves[9] = {
        {0, false, false}, {0, true, false}, {0, true, true},  // square root
        {1, false, false}, {1, true, false}, {1, true, true},  // sine/cosine
        {2, false, false}, {2, true, false}, {2, true, true},  // linear
    };
    return pan_curve < 9 ? kCurves[pan_curve] : kCurves[0];
}

inline s32 pan_index(f32 x) {  // (fctiwz of a value in 0.5..256.5)
    s32 i = fctiwz(0.5f + 256.0f * x);
    return i < 0 ? 0 : i > 256 ? 256 : i;  // (a NaN pan: the original reads outside the table)
}

// Util::CalcPanRatio: one side's level for a pan in -1..1 (1: all to this side)
f32 calc_pan_ratio(const PanInfo& info, f32 pan) {
    f32 x = clamp(pan, -1.0f, 1.0f);
    const f32* t = kPanTable[info.curve];
    f32 r = t[pan_index((1.0f + x) * 0.5f)];
    if (info.center_zero)
        r = r / t[128];
    return clamp(r, 0.0f, info.zero_clamp ? 1.0f : 2.0f);
}

// Util::CalcSurroundPanRatio: the front (or, for 2 - span, rear) level for a span in 0..2
f32 calc_surround_pan_ratio(const PanInfo& info, f32 span) {
    f32 x = clamp(span, 0.0f, 2.0f);
    return clamp(kPanTable[info.curve][pan_index(x * 0.5f)], 0.0f, 2.0f);
}

// Util::CalcLpfFreq: 0..1 to a cutoff in Hz
u16 calc_lpf_freq(f32 x) {
    x = clamp(x, 0.0f, 1.0f);
    if (x < 0.13561438f)
        return 80;
    if (x >= 0.9f)
        return 16000;
    return kLpfFreqTable[fctiwz((x - 0.13561438f) / 0.033333335f)];
}

// AxManager's master volume, fading from one value to another over a number of frames
f32 output_volume(GuestObj am) {
    s32 frames = am.gets32(AM_VOLUME_FRAMES), frame = am.gets32(AM_VOLUME_FRAME);
    if (frame >= frames)
        return am.getf(AM_VOLUME_TO);
    f32 from = am.getf(AM_VOLUME_FROM);
    return from + ((f32)frame * (am.getf(AM_VOLUME_TO) - from)) / (f32)frames;
}

// Voice::CalcDpl2Pan: pan and span as Dolby Pro Logic II encodes them. (nw4r's 0.15 and 0.35 are
// 1 - 0.85 and 0.35 + 1 ulp as floats: the DOL's values are used, not the nearest floats.)
void calc_dpl2_pan(f32 pan, f32 span, f32& out_pan, f32& out_span) {
    f32 apan = fabsf(pan);
    f32 s = span - 1.0f;
    if (apan <= fabsf(s)) {
        if (s <= 0.0f) {
            out_pan = pan;
            out_span = -0.12f + 0.88f * s;
        } else {
            out_pan = 0.5f * pan;
            out_span = -0.12f + 1.12f * s;
        }
    } else if (pan >= 0.0f) {
        if (s <= 0.0f) {
            out_pan = (0.85f + 0.149999976f * (-s / pan)) * apan;
            out_span = -0.12f + (2.0f * s + 0.88f * pan);
        } else {
            out_span = -0.12f + 1.12f * pan;
            out_pan = (0.85f + 0.350000024f * (-s / pan)) * apan;
        }
    } else if (s <= 0.0f) {
        out_pan = (0.149999976f * (-s / pan) - 0.85f) * apan;
        out_span = -0.12f + (2.0f * s - 1.12f * pan);
    } else {
        out_span = -0.12f + 1.12f * -pan;
        out_pan = (0.350000024f * (-s / pan) - 0.85f) * apan;
    }
    out_span = out_span + 1.0f;
}

// a level (0..) as an AX volume: 1.0 = 0x8000, at most 0xFFFF
inline u16 ax_volume(f32 x) {
    if (!(x > 0.0f) && x == x)
        return 0;
    u32 v = fp2u(32768.0f * x);
    return (u16)(v < 0xFFFF ? v : 0xFFFF);
}

// Voice::CalcMixParam: one AX voice's volumes on each bus and remote speaker, from the voice's
// volumes, sends, pan and span and the output mode
void calc_mix_param(const Voice& vo, s32 ch, s32 out, MixParam& mix, RemoteMixParam& rmt) {
    GuestObj v = vo.v;
    f32 main = 0, send = 0, fx_a = 0, fx_b = 0, fx_c = 0;
    u32 line = v.get32(V_OUTPUT_LINE);
    if (line & 1) {
        main = v.getf(V_MAIN_VOLUME);
        send = v.getf(V_MAIN_SEND);
        fx_a = v.getf(V_FX_SEND) + vo.out_param(out, 0x10);
        if (fx_a < 0.0f)
            fx_a = 0.0f;
        fx_b = v.getf(V_FX_SEND + 4);
        fx_c = v.getf(V_FX_SEND + 8);
    }
    const f32 bus_main = main * clamp(send, 0.0f, 1.0f);
    const f32 bus_a = main * clamp(fx_a, 0.0f, 1.0f);
    const f32 bus_b = main * clamp(fx_b, 0.0f, 1.0f);
    const f32 bus_c = main * clamp(fx_c, 0.0f, 1.0f);
    f32 remote[4];
    for (int i = 0; i < 4; i++)
        remote[i] = line & (2u << i) ? v.getf(V_REMOTE_VOLUME + 4 * i) : 0.0f;

    PanInfo info = pan_info(v.get32(V_PAN_CURVE));
    GuestObj am(ax_manager());
    u32 mode = am.get32(AM_OUTPUT_MODE);
    f32 left = 0, right = 0, front, rear;
    if (vo.channels > 1 && v.gets32(V_PAN_MODE) == 1) {  // balance: each channel keeps its side
        f32 pan = v.getf(V_PAN) + vo.out_param(out, 0x8);
        f32 span = v.getf(V_SPAN) + vo.out_param(out, 0xC);
        if (ch == 0)
            left = calc_pan_ratio(info, pan);
        else if (ch == 1)
            right = calc_pan_ratio(info, -pan);
        front = calc_surround_pan_ratio(info, span);
        rear = calc_surround_pan_ratio(info, 2.0f - span);
    } else {
        f32 ch_pan = 0.0f;
        if (vo.channels == 2)
            ch_pan = ch == 0 ? -1.0f : ch == 1 ? 1.0f : 0.0f;
        f32 pan = v.getf(V_PAN) + ch_pan + vo.out_param(out, 0x8);
        f32 span = v.getf(V_SPAN) + vo.out_param(out, 0xC);
        if (mode == 2)
            calc_dpl2_pan(pan, span, pan, span);
        left = calc_pan_ratio(info, pan);
        right = calc_pan_ratio(info, -pan);
        front = calc_surround_pan_ratio(info, span);
        rear = calc_surround_pan_ratio(info, 2.0f - span);
    }
    static const f32 kMinus3dB = calc_volume_ratio(-3.0f);
    const f32 mono = 0.5f * (left + right);

    // per bus: L R S (main, aux A, aux B, aux C)
    f32 m[12] = {};
    switch (mode) {
    case 0:  // stereo
        m[0] = bus_main * left, m[1] = bus_main * right;
        m[3] = bus_a * left, m[4] = bus_a * right;
        m[6] = bus_b * left, m[7] = bus_b * right;
        m[9] = bus_c * left, m[10] = bus_c * right;
        break;
    case 3:  // mono
        m[0] = m[1] = bus_main * mono;
        m[3] = m[4] = bus_a * mono;
        m[6] = m[7] = bus_b * mono;
        m[9] = m[10] = bus_c * mono;
        break;
    case 1: {  // surround
        f32 fl = left * front, fr = right * front, s = kMinus3dB * rear;
        m[0] = bus_main * fl, m[1] = bus_main * fr, m[2] = bus_main * s;
        m[3] = bus_a * fl, m[4] = bus_a * fr, m[5] = bus_a * s;
        m[6] = bus_b * fl, m[7] = bus_b * fr, m[8] = bus_b * s;
        m[9] = bus_c * fl, m[10] = bus_c * fr, m[11] = bus_c * s;
        break;
    }
    case 2: {  // Dolby Pro Logic II: the rear pair on S and aux C L for the main bus, the aux
               // buses' rears on their S and the next bus's slots
        f32 rl = left * rear, rr = right * rear, fl = left * front, fr = right * front;
        m[0] = bus_main * fl, m[1] = bus_main * fr, m[2] = bus_main * rl, m[9] = bus_main * rr;
        m[3] = bus_a * fl, m[4] = bus_a * fr, m[5] = bus_a * rl, m[10] = bus_a * rr;
        m[6] = bus_b * fl, m[7] = bus_b * fr, m[8] = bus_b * rl, m[11] = bus_b * rr;
        break;
    }
    }
    for (int k = 0; k < 12; k++)
        mix.v[k] = ax_volume(m[k]);
    for (int i = 0; i < 4; i++) {
        rmt.v[2 * i] = ax_volume(mono * remote[i]);
        rmt.v[2 * i + 1] = 0;
    }
}

void calc_ve(const Voice& vo) {
    GuestObj v = vo.v;
    f32 volume = 1.0f * v.getf(V_VOLUME);
    volume = volume * output_volume(GuestObj(ax_manager()));
    for (s32 out = 0; out < vo.outs; out++) {
        f32 o = volume * vo.out_param(out, 0x0);
        f32 ve = o * v.getf(V_VE_VOLUME), init = o * v.getf(V_VE_INIT_VOLUME);
        for (s32 ch = 0; ch < vo.channels; ch++)
            if (GuestObj ax = vo.ax(ch, out))
                ax_set_ve(ax, ve, init);
    }
}

// true: some mix volume is still ramping
bool calc_mix(const Voice& vo) {
    bool ramping = false;
    bool remote = vo.v.get32(V_OUTPUT_LINE) > 1;
    for (s32 ch = 0; ch < vo.channels; ch++) {
        for (s32 out = 0; out < vo.outs; out++) {
            GuestObj ax = vo.ax(ch, out);
            if (!ax)
                continue;
            MixParam mix;
            RemoteMixParam rmt;
            calc_mix_param(vo, ch, out, mix, rmt);
            ramping |= ax_set_mix(ax, mix);
            ax_enable_remote(ax, remote);
            if (remote)
                ax_set_rmt_mix(ax, rmt);
        }
    }
    return ramping;
}

f32 out_pitch(const Voice& vo, s32 out) {
    f32 pitch = vo.v.getf(V_PITCH);
    if (!vo.v.get8(V_PITCH_ONLY))
        pitch = pitch * vo.out_param(out, 0x4);
    return pitch;
}

// Voice::Calc: brings the AX voices up to date with what changed
void calc(u32 addr) {
    Voice vo(addr);
    GuestObj v = vo.v;
    if (!v.get8(V_ACTIVE))
        return;
    if (v.get16(V_SYNC) & SYNC_PITCH) {
        for (s32 out = 0; out < vo.outs; out++) {
            f32 pitch = out_pitch(vo, out);
            for (s32 ch = 0; ch < vo.channels; ch++)
                if (GuestObj ax = vo.ax(ch, out))
                    ax_set_src(ax, false, pitch);
        }
        vo.clear_sync(SYNC_PITCH);
    }
    if (v.get16(V_SYNC) & SYNC_VE) {
        calc_ve(vo);
        vo.clear_sync(SYNC_VE);
    }
    if ((v.get16(V_SYNC) & SYNC_MIX) && !calc_mix(vo))
        vo.clear_sync(SYNC_MIX);
    if (v.get16(V_SYNC) & SYNC_LPF) {
        for (s32 out = 0; out < vo.outs; out++) {
            u16 freq = calc_lpf_freq(v.getf(V_LPF_FREQ) + vo.out_param(out, 0x14));
            for (s32 ch = 0; ch < vo.channels; ch++)
                if (GuestObj ax = vo.ax(ch, out))
                    ax_set_lpf(ax, freq);
        }
        vo.clear_sync(SYNC_LPF);
    }
    if (v.get16(V_SYNC) & SYNC_BIQUAD) {
        for (s32 out = 0; out < vo.outs; out++)
            for (s32 ch = 0; ch < vo.channels; ch++)
                if (GuestObj ax = vo.ax(ch, out))
                    ax_set_biquad(ax, v.get8(V_BIQUAD_TYPE), v.getf(V_BIQUAD_VALUE));
        vo.clear_sync(SYNC_BIQUAD);
    }
    if (v.get16(V_SYNC) & SYNC_REMOTE_FILTER) {
        for (s32 out = 0; out < vo.outs; out++)
            for (s32 ch = 0; ch < vo.channels; ch++)
                if (GuestObj ax = vo.ax(ch, out))
                    ax_set_remote_filter(ax, v.get8(V_REMOTE_FILTER));
        vo.clear_sync(SYNC_REMOTE_FILTER);
    }
}

// Voice::Update: starts, pauses and resumes the AX voices, and hands each its changes (Sync)
void update(u32 addr) {
    Voice vo(addr);
    GuestObj v = vo.v;
    if (!v.get8(V_ALLOCATED))
        return;
    enum { NONE, RUN, STOP } state = NONE;
    if ((v.get16(V_SYNC) & SYNC_START) && v.get8(V_ACTIVE) && !v.get8(V_STARTED)) {
        for (s32 out = 0; out < vo.outs; out++) {
            f32 pitch = out_pitch(vo, out);
            for (s32 ch = 0; ch < vo.channels; ch++)
                if (GuestObj ax = vo.ax(ch, out))
                    ax_set_src(ax, true, pitch);
        }
        state = RUN;
        v.set8(V_STARTED, 1);
        v.set16(V_SYNC, v.get16(V_SYNC) & ~(SYNC_START | SYNC_PITCH));
    }
    if (v.get8(V_STARTED)) {
        if ((v.get16(V_SYNC) & SYNC_PAUSE) && v.get8(V_ACTIVE)) {
            if (v.get8(V_PAUSE)) {
                v.set8(V_PAUSED, 1);
                state = STOP;
            } else {
                v.set8(V_PAUSED, 0);
                state = RUN;
            }
            vo.clear_sync(SYNC_PAUSE);
        }
        for (s32 ch = 0; ch < vo.channels; ch++)
            for (s32 out = 0; out < vo.outs; out++)
                if (GuestObj ax = vo.ax(ch, out))
                    ax_sync(ax);
    }
    if (state == RUN) {
        for (s32 ch = 0; ch < vo.channels; ch++)
            for (s32 out = 0; out < vo.outs; out++)
                if (GuestObj ax = vo.ax(ch, out))
                    if (u32 pb = ax.get32(AXV_PB))
                        ax_set_voice_state(GuestObj(pb), 1);
    } else if (state == STOP) {
        for (s32 ch = 0; ch < vo.channels; ch++)
            for (s32 out = 0; out < vo.outs; out++)
                if (GuestObj ax = vo.ax(ch, out))
                    if (u32 pb = ax.get32(AXV_PB); pb && guest::load16(pb + PB_STATE) == 1)
                        ax_set_voice_state(GuestObj(pb), 0);
    }
}

// Voice::StopFinished: a voice whose one-shot sample ended tells its owner (status 0) and stops
void stop_finished(u32 addr) {
    GuestObj v(addr);
    if (!v.get8(V_ALLOCATED) || !v.get8(V_STARTED))
        return;
    GuestObj ax(v.get32(V_AXVOICE));
    if (!ax || !ax_is_play_finished(ax))
        return;
    if (u32 callback = v.get32(V_CALLBACK))
        gcall<u32>(callback, addr, 0u, v.get32(V_CALLBACK_DATA));
    v.set8(V_STARTED, 0);
    v.set8(V_ACTIVE, 0);
}

}  // namespace

// Util::CalcVolumeRatio: dB to an amplitude ratio
f32 calc_volume_ratio(f32 db) {
    return kVolumeTable[fctiwz(10.0f * clamp(db, -90.4f, 6.0f)) + 904];
}

// ---- Voice's setters (nw4r::snd::detail::Voice::Set*): a value and the update it needs
namespace {
// a float field set if it changed (compared in double: callers may pass double values), marking
// the update it needs
inline void set_field(GuestObj v, u32 off, f64 x, u16 sync) {
    if (x != (f64)v.getf(off)) {
        v.setf(off, (f32)x);
        v.set16(V_SYNC, v.get16(V_SYNC) | sync);
    }
}
inline void set_word(GuestObj v, u32 off, u32 x, u16 sync) {
    if (x != v.get32(off)) {
        v.set32(off, x);
        v.set16(V_SYNC, v.get16(V_SYNC) | sync);
    }
}
inline f64 at_least_0(f64 x) { return x < 0.0 ? 0.0 : x; }
}  // namespace

void voice_set_volume(GuestObj v, f64 volume) { set_field(v, V_VOLUME, at_least_0(volume), SYNC_VE); }

void voice_set_ve_volume(GuestObj v, f64 target, f64 init) {
    target = at_least_0(target);
    init = at_least_0(init);
    if (init != (f64)v.getf(V_VE_INIT_VOLUME) || target != (f64)v.getf(V_VE_VOLUME)) {
        v.setf(V_VE_INIT_VOLUME, (f32)init);
        v.setf(V_VE_VOLUME, (f32)target);
        v.set16(V_SYNC, v.get16(V_SYNC) | SYNC_VE);
    }
}

void voice_set_pitch(GuestObj v, f64 pitch) { set_field(v, V_PITCH, pitch, SYNC_PITCH); }
void voice_set_pan_mode(GuestObj v, u32 mode) { set_word(v, V_PAN_MODE, mode, SYNC_MIX); }
void voice_set_pan_curve(GuestObj v, u32 curve) { set_word(v, V_PAN_CURVE, curve, SYNC_MIX); }
void voice_set_pan(GuestObj v, f64 pan) { set_field(v, V_PAN, pan, SYNC_MIX); }
void voice_set_surround_pan(GuestObj v, f64 span) { set_field(v, V_SPAN, span, SYNC_MIX); }
void voice_set_lpf_freq(GuestObj v, f64 freq) { set_field(v, V_LPF_FREQ, freq, SYNC_LPF); }

void voice_set_biquad_filter(GuestObj v, u32 type, f64 value) {
    value = value > 1.0 ? 1.0 : value < 0.0 ? 0.0 : value;
    bool changed = false;
    if ((s32)type != v.get8(V_BIQUAD_TYPE)) {
        v.set8(V_BIQUAD_TYPE, (u8)type);
        changed = true;
    }
    if (value != (f64)v.getf(V_BIQUAD_VALUE)) {
        v.setf(V_BIQUAD_VALUE, (f32)value);
        changed = true;
    }
    if (changed)
        v.set16(V_SYNC, v.get16(V_SYNC) | SYNC_BIQUAD);
}

void voice_set_remote_filter(GuestObj v, s32 filter) {
    filter = filter > 0x7F ? 0x7F : filter < 0 ? 0 : filter;
    if (filter != v.get8(V_REMOTE_FILTER)) {
        v.set8(V_REMOTE_FILTER, (u8)filter);
        v.set16(V_SYNC, v.get16(V_SYNC) | SYNC_REMOTE_FILTER);
    }
}

void voice_set_output_line(GuestObj v, u32 line) { set_word(v, V_OUTPUT_LINE, line, SYNC_MIX); }
void voice_set_main_out_volume(GuestObj v, f64 volume) { set_field(v, V_MAIN_VOLUME, at_least_0(volume), SYNC_MIX); }
// (kept as 1 + send: 0 is the default send)
void voice_set_main_send(GuestObj v, f64 send) {
    set_field(v, V_MAIN_SEND, at_least_0((f64)((f32)send + 1.0f)), SYNC_MIX);
}
void voice_set_fx_send(GuestObj v, u32 bus, f64 send) { set_field(v, V_FX_SEND + 4 * bus, at_least_0(send), SYNC_MIX); }
void voice_set_remote_out_volume(GuestObj v, u32 remote, f64 volume) {
    set_field(v, V_REMOTE_VOLUME + 4 * remote, at_least_0(volume), SYNC_MIX);
}

}  // namespace engine::snd

// ---- the guest ABI (decl.h)
namespace g {

using namespace engine::snd;

// every voice in the manager's list: StopFinished, then Calc, then Update. (The voice callbacks
// StopFinished calls are guest code, which may unlink the voice: the next one is read first.)
u64 nw4r_snd_detail_VoiceManager_UpdateAllVoices(u32 manager, u32, u32, u32, u32, u32, u32, u32, f64, f64, f64,
                                                 f64, f64, f64, f64, f64) {
    NOMA_PROF_SCOPE("(snd voices)");
    const u32 head = manager + 8;
    for (void (*pass)(u32) : {stop_finished, calc, update}) {
        for (u32 link = guest::load32(head); link != head;) {
            u32 next = guest::load32(link);
            pass(link - V_LINK);
            link = next;
        }
    }
    // (the original returns what OSRestoreInterrupts left in r3 and r4: interrupts were off)
    return NOMA_U64(0, guest::g_msr & ~0x8000u);
}

void nw4r_snd_detail_Voice_StopFinished(u32 voice) {
    stop_finished(voice);
}

void nw4r_snd_detail_Voice_Calc(u32 voice, u32, u32, u32, u32, u32, u32, u32, f64, f64, f64, f64, f64, f64, f64,
                                f64) {
    calc(voice);
}

void nw4r_snd_detail_Voice_CalcVe(u32 voice, u32, u32, u32, u32, u32, u32, u32, f64, f64, f64, f64, f64, f64, f64,
                                  f64) {
    calc_ve(Voice(voice));
}

u32 nw4r_snd_detail_Voice_CalcMix(u32 voice, u32, u32, u32, u32, u32, u32, u32, f64, f64, f64, f64, f64, f64, f64,
                                  f64) {
    return calc_mix(Voice(voice));
}

// (voice, channel, out, MixParam*, RemoteMixParam*)
void nw4r_snd_detail_Voice_CalcMixParam(u32 voice, u32 ch, u32 out, u32 mix_out, u32 rmt_out, u32, u32, u32, f64,
                                        f64, f64, f64, f64, f64, f64, f64) {
    MixParam mix;
    RemoteMixParam rmt;
    calc_mix_param(Voice(voice), (s32)ch, (s32)out, mix, rmt);
    GuestObj m(mix_out), r(rmt_out);
    for (int k = 0; k < 12; k++)
        m.set16(2 * k, mix.v[k]);
    for (int i = 0; i < 8; i++)
        r.set16(2 * i, rmt.v[i]);
}

// (voice, &pan, &span, pan, span)
void nw4r_snd_detail_Voice_CalcDpl2Pan(u32, u32 pan_out, u32 span_out, f64 pan, f64 span) {
    f32 p, s;
    calc_dpl2_pan((f32)pan, (f32)span, p, s);
    GuestObj(pan_out).setf(0, p);
    GuestObj(span_out).setf(0, s);
}

void nw4r_snd_detail_Voice_ResetDelta(u32 voice) {
    Voice vo(voice);
    for (s32 out = 0; out < vo.outs; out++)
        for (s32 ch = 0; ch < vo.channels; ch++)
            if (GuestObj ax = vo.ax(ch, out))
                ax_reset_delta(ax);
}

void nw4r_snd_detail_Voice_Update(u32 voice) {
    update(voice);
}

void nw4r_snd_detail_Voice_SetVolume(u32 voice, f64 volume) { voice_set_volume(GuestObj(voice), volume); }
void nw4r_snd_detail_Voice_SetVeVolume(u32 voice, f64 target, f64 init) {
    voice_set_ve_volume(GuestObj(voice), target, init);
}
void nw4r_snd_detail_Voice_SetPitch(u32 voice, f64 pitch) { voice_set_pitch(GuestObj(voice), pitch); }
void nw4r_snd_detail_Voice_SetPanMode(u32 voice, u32 mode) { voice_set_pan_mode(GuestObj(voice), mode); }
void nw4r_snd_detail_Voice_SetPanCurve(u32 voice, u32 curve) { voice_set_pan_curve(GuestObj(voice), curve); }
void nw4r_snd_detail_Voice_SetPan(u32 voice, f64 pan) { voice_set_pan(GuestObj(voice), pan); }
void nw4r_snd_detail_Voice_SetSurroundPan(u32 voice, f64 span) { voice_set_surround_pan(GuestObj(voice), span); }
void nw4r_snd_detail_Voice_SetLpfFreq(u32 voice, f64 freq) { voice_set_lpf_freq(GuestObj(voice), freq); }
void nw4r_snd_detail_Voice_SetBiquadFilter(u32 voice, u32 type, f64 value) {
    voice_set_biquad_filter(GuestObj(voice), type, value);
}
void nw4r_snd_detail_Voice_SetRemoteFilter(u32 voice, u32 filter) {
    voice_set_remote_filter(GuestObj(voice), (s32)filter);
}
void nw4r_snd_detail_Voice_SetOutputLine(u32 voice, u32 line) { voice_set_output_line(GuestObj(voice), line); }
void nw4r_snd_detail_Voice_SetMainOutVolume(u32 voice, f64 volume) {
    voice_set_main_out_volume(GuestObj(voice), volume);
}
void nw4r_snd_detail_Voice_SetMainSend(u32 voice, f64 send) { voice_set_main_send(GuestObj(voice), send); }
void nw4r_snd_detail_Voice_SetFxSend(u32 voice, u32 bus, f64 send) { voice_set_fx_send(GuestObj(voice), bus, send); }
void nw4r_snd_detail_Voice_SetRemoteOutVolume(u32 voice, u32 remote, f64 volume) {
    voice_set_remote_out_volume(GuestObj(voice), remote, volume);
}

// (PanInfo*, pan)
f64 nw4r_snd_detail_Util_CalcPanRatio(u32 info, f64 pan) {
    GuestObj i(info);
    PanInfo p = {i.gets32(0), i.get8(4) != 0, i.get8(5) != 0};
    if (p.curve < 0 || p.curve > 2)
        p.curve = 0;
    return calc_pan_ratio(p, (f32)pan);
}

f64 nw4r_snd_detail_Util_CalcSurroundPanRatio(u32 info, f64 span) {
    GuestObj i(info);
    PanInfo p = {i.gets32(0), i.get8(4) != 0, i.get8(5) != 0};
    if (p.curve < 0 || p.curve > 2)
        p.curve = 0;
    return calc_surround_pan_ratio(p, (f32)span);
}

f64 nw4r_snd_detail_Util_CalcVolumeRatio(f64 db) {
    return calc_volume_ratio((f32)db);
}

u32 nw4r_snd_detail_Util_CalcLpfFreq(f64 x) {
    return calc_lpf_freq((f32)x);
}

void nw4r_snd_detail_Util_GetRemoteFilterCoefs(u32 value, u32 b0, u32 b1, u32 b2, u32 a1, u32 a2) {
    s32 v = (s32)value;
    const u16* c = kRemoteFilterTable[v > 0x7F ? 0x7F : v < 0 ? 0 : v];
    const u32 out[5] = {b0, b1, b2, a1, a2};
    for (int i = 0; i < 5; i++)
        guest::store16(out[i], c[i]);
}

f64 nw4r_snd_detail_AxManager_GetOutputVolume(u32 manager) {
    return output_volume(GuestObj(manager));
}

}  // namespace g
