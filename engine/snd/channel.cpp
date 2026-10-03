// nw4r::snd::detail::Channel (snd_Channel.cpp) with its envelope (snd_EnvGenerator.cpp) and LFO
// (snd_Lfo.cpp), and Util::CalcPitchRatio, native: every 3 ms AX frame, each playing note's
// channel steps its envelope, LFO and sweep and hands the results to its voice
// (engine/snd/voice.cpp), which turns them into AX parameters on its next Calc.
#include "noma_decomp.h"
#include "voice.h"
#include "tables.h"

namespace engine::snd {

namespace {

// EnvGenerator (at the start of a Channel): an ADSR envelope in 0.1 dB
enum : u32 {
    ENV_STATE = 0x00,      // s32: 0 attack, 1 hold, 2 decay, 3 sustain, 4 release
    ENV_VALUE = 0x04,      // f32: 0.1 dB, 0 at full volume
    ENV_DECAY = 0x08,      // f32: per ms
    ENV_RELEASE = 0x0C,    // f32: per ms
    ENV_ATTACK = 0x10,     // f32: factor per ms
    ENV_SUSTAIN = 0x14,    // u8
    ENV_HOLD = 0x16,       // u16: ms
    ENV_HOLD_LEFT = 0x18,  // u16
};
enum { ENV_ATTACKING, ENV_HOLDING, ENV_DECAYING, ENV_SUSTAINING, ENV_RELEASING };

// Lfo (at Channel + 0x1C)
enum : u32 {
    LFO_DEPTH = 0x00,    // f32
    LFO_SPEED = 0x04,    // f32: cycles per second
    LFO_DELAY = 0x08,    // u32: ms
    LFO_RANGE = 0x0C,    // u8
    LFO_COUNTER = 0x10,  // u32: ms into the delay
    LFO_PHASE = 0x14,    // f32: 0..1
};

// Channel
enum : u32 {
    CH_ENV = 0x00,
    CH_LFO = 0x1C,
    CH_LFO_TARGET = 0x34,  // u8: 0 pitch, 1 volume, 2 pan
    CH_PAUSED = 0x35,      // u8
    CH_ACTIVE = 0x36,      // u8
    CH_POOLED = 0x37,      // u8: from ChannelManager's pool
    CH_AUTO_SWEEP = 0x38,  // u8
    CH_BIQUAD_TYPE = 0x3B,
    CH_REMOTE_FILTER = 0x3C,
    CH_USER_VOLUME = 0x40,
    CH_USER_PITCH = 0x44,
    CH_USER_PAN = 0x48,
    CH_USER_SPAN = 0x4C,
    CH_LPF_FREQ = 0x50,
    CH_BIQUAD_VALUE = 0x54,
    CH_OUTPUT_LINE = 0x58,
    CH_MAIN_OUT_VOLUME = 0x5C,
    CH_MAIN_SEND = 0x60,
    CH_FX_SEND = 0x64,         // f32[3]
    CH_REMOTE_VOLUME = 0x70,   // f32[4]
    CH_PITCH_OFFSET = 0x80,    // f32: semitones
    CH_SWEEP_PITCH = 0x84,     // f32: semitones at the start, to 0
    CH_SWEEP_COUNTER = 0x88,   // s32: ms
    CH_SWEEP_LENGTH = 0x8C,    // s32: ms
    CH_VOLUME = 0x90,
    CH_PAN = 0x94,
    CH_SPAN = 0x98,
    CH_PITCH = 0x9C,
    CH_SILENCE_FROM = 0xA0,    // u8: a fade, 0..255 over a number of frames
    CH_SILENCE_TO = 0xA1,      // u8
    CH_SILENCE_FRAMES = 0xA2,  // u16
    CH_SILENCE_FRAME = 0xA4,   // u16
    CH_KEY = 0xA8,             // s32
    CH_ORIGINAL_KEY = 0xAC,    // s32
    CH_PAN_MODE = 0xB4,
    CH_PAN_CURVE = 0xB8,
    CH_CALLBACK = 0xC0,        // void (*)(Channel*, int status, u32 data)
    CH_CALLBACK_DATA = 0xC4,
    CH_DISPOSER = 0xC8,        // an object with a virtual (slot 3) told when the channel ends
    CH_DISPOSER_DATA = 0xCC,
    CH_VOICE = 0xD0,
    CH_LINK = 0xD8,            // ChannelManager's list node
};

const u32 FRAME_MS = 3;  // an AX frame, as nw4r counts it

f32 env_value(GuestObj c) {  // EnvGenerator::GetValue: dB
    if (c.gets32(ENV_STATE) != ENV_ATTACKING || c.getf(ENV_ATTACK) != 0.0f)
        return c.getf(ENV_VALUE) / 10.0f;
    return 0.0f;
}

void env_update(GuestObj c, s32 ms) {  // EnvGenerator::Update
    switch (c.gets32(ENV_STATE)) {
    case ENV_ATTACKING:
        for (s32 i = ms; i > 0; i--) {
            f32 v = c.getf(ENV_VALUE) * c.getf(ENV_ATTACK);
            c.setf(ENV_VALUE, v);
            if (v > -0.03125f) {
                c.setf(ENV_VALUE, 0.0f);
                c.set32(ENV_STATE, ENV_HOLDING);
                c.set16(ENV_HOLD_LEFT, c.get16(ENV_HOLD));
                return;
            }
        }
        return;
    case ENV_HOLDING: {
        s32 left = c.get16(ENV_HOLD_LEFT);
        if (ms < left) {
            c.set16(ENV_HOLD_LEFT, (u16)(left - ms));
        } else {
            c.set16(ENV_HOLD_LEFT, 0);
            ms -= left;
            c.set32(ENV_STATE, ENV_DECAYING);
        }
        if (c.gets32(ENV_STATE) != ENV_DECAYING)
            return;
    }
        [[fallthrough]];
    case ENV_DECAYING: {
        f32 v = c.getf(ENV_VALUE) - c.getf(ENV_DECAY) * (f32)ms;
        // (sustain is 0..127; past that the original reads the data after its table)
        f32 sustain = (f32)kSustainTable[c.get8(ENV_SUSTAIN) & 0x7F];
        c.setf(ENV_VALUE, v);
        if (v < sustain) {
            c.setf(ENV_VALUE, sustain);
            c.set32(ENV_STATE, ENV_SUSTAINING);
        }
        return;
    }
    case ENV_RELEASING:
        c.setf(ENV_VALUE, c.getf(ENV_VALUE) - c.getf(ENV_RELEASE) * (f32)ms);
        return;
    }
}

f32 lfo_value(GuestObj l) {  // Lfo::GetValue: -1..1 times depth and range
    f32 depth = l.getf(LFO_DEPTH);
    if (depth == 0.0f)
        return 0.0f;
    if (l.get32(LFO_COUNTER) < l.get32(LFO_DELAY))
        return 0.0f;
    s32 i = fctiwz(4.0f * (32.0f * l.getf(LFO_PHASE)));
    i = i < 0 ? 0 : i > 127 ? 127 : i;  // (a phase outside 0..1: the original reads outside the table)
    s32 s;
    if (i < 0x20)
        s = kLfoTable[i];
    else if (i < 0x40)
        s = kLfoTable[0x40 - i];
    else if (i < 0x60)
        s = -kLfoTable[i - 0x40];
    else
        s = -kLfoTable[0x80 - i];
    return ((f32)s / 127.0f) * depth * (f32)l.get8(LFO_RANGE);
}

void lfo_update(GuestObj l, u32 ms) {  // Lfo::Update
    u32 delay = l.get32(LFO_DELAY), counter = l.get32(LFO_COUNTER);
    if (counter < delay) {
        if (counter + ms <= delay) {
            l.set32(LFO_COUNTER, counter + ms);
            return;
        }
        l.set32(LFO_COUNTER, delay);
        ms -= delay - counter;
    }
    f32 phase = l.getf(LFO_PHASE) + (l.getf(LFO_SPEED) * (f32)(s32)ms) / 1000.0f;
    l.setf(LFO_PHASE, phase - (f32)fctiwz(phase));
}

// the voice's envelope volume (amplitude) for the envelope and, if it modulates volume, the LFO
f32 ve_volume(GuestObj c, f32 lfo) {
    f32 v = 1.0f * calc_volume_ratio(env_value(c));
    if (c.get8(CH_LFO_TARGET) == 1)
        v = v * calc_volume_ratio(6.0f * lfo);
    return v;
}

// a channel's end, when its release has faded out: the voice freed, the owner and the disposer
// told, the channel back in the pool
void finish(GuestObj c) {
    u32 voice = c.get32(CH_VOICE);
    if (!voice)
        return;
    g::nw4r_snd_detail_Voice_Stop(voice);
    g::nw4r_snd_detail_Voice_Free(c.get32(CH_VOICE));
    u32 callback = c.get32(CH_CALLBACK);
    c.set32(CH_VOICE, 0);
    c.set8(CH_PAUSED, 0);
    c.set8(CH_ACTIVE, 0);
    if (callback)
        gcall<u32>(callback, c.addr, 0u, c.get32(CH_CALLBACK_DATA));
    if (u32 disposer = c.get32(CH_DISPOSER))
        gcall<u32>(guest::load32(guest::load32(disposer) + 0xC), disposer, c.get32(CH_DISPOSER_DATA));
    if (c.get8(CH_POOLED)) {
        c.set8(CH_POOLED, 0);
        u32 manager = g::nw4r_snd_detail_ChannelManager_GetInstance();
        g::nw4r_ut_detail_LinkListImpl_Erase(manager + 4, c.addr + CH_LINK);
        g::nw4r_snd_detail_PoolImpl_FreeImpl(manager, c.addr);
    }
}

}  // namespace

// Util::CalcPitchRatio: the frequency ratio of a pitch in 1/256 semitones
f32 calc_pitch_ratio(s32 pitch) {
    u32 p = (u32)pitch;
    s32 octave = 0;
    if ((s32)p < 0) {
        u32 n = (0xBFFu - p) / 3072u;
        octave -= (s32)n;
        p += n * 0xC00;
    }
    if ((s32)p >= 0xC00) {
        u32 n = p / 3072u;
        octave += (s32)n;
        p -= n * 0xC00;
    }
    // 2^octave (the original multiplies it out by 2 or 0.5: exact, and the same at the ends)
    f32 ratio = ldexpf(1.0f, octave);
    if (u32 semitone = p >> 8)
        ratio = ratio * kSemitoneTable[semitone];
    if (u32 fine = p & 0xFF)
        ratio = ratio * kFineTuneTable[fine];
    return ratio;
}

// Channel::Update: one AX frame of a playing channel. periodic: step the envelope, LFO and sweep
// (not while paused)
void channel_update(u32 addr, bool periodic) {
    GuestObj c(addr);
    if (!c.get8(CH_ACTIVE))
        return;
    if (c.get8(CH_PAUSED))
        periodic = false;
    GuestObj lfo(addr + CH_LFO);
    f32 lfo_before = lfo_value(lfo);

    u32 frames = c.get16(CH_SILENCE_FRAMES), frame = c.get16(CH_SILENCE_FRAME);
    if (frame < frames)
        c.set16(CH_SILENCE_FRAME, (u16)++frame);
    f32 volume = 1.0f * c.getf(CH_VOLUME) * c.getf(CH_USER_VOLUME);
    u32 silence = c.get8(CH_SILENCE_TO);
    if (frame < frames) {
        u32 from = c.get8(CH_SILENCE_FROM);
        silence = (u8)(from + NOMA_DIVW((s32)(frame * (silence - from)), (s32)frames));
    }
    volume = volume * ((f32)silence / 255.0f);
    f32 ve_before = ve_volume(c, lfo_before);

    if (c.gets32(ENV_STATE) == ENV_RELEASING) {  // faded out: the channel ends
        bool silent = c.get32(CH_CALLBACK) ? ve_before == 0.0f : volume * ve_before == 0.0f;
        if (silent) {
            finish(c);
            return;
        }
    }

    f32 sweep = 0.0f, sweep_pitch = c.getf(CH_SWEEP_PITCH);
    if (sweep_pitch != 0.0f) {
        s32 counter = c.gets32(CH_SWEEP_COUNTER), length = c.gets32(CH_SWEEP_LENGTH);
        if (counter < length)
            sweep = (sweep_pitch * (f32)(length - counter)) / (f32)length;
    }
    f32 semitones = (0.0f + (f32)(c.gets32(CH_KEY) - c.gets32(CH_ORIGINAL_KEY))) + sweep + c.getf(CH_PITCH_OFFSET);
    if (c.get8(CH_LFO_TARGET) == 0)
        semitones = semitones + lfo_before;
    const f32 pitch_scale = 1.0f * c.getf(CH_PITCH) * c.getf(CH_USER_PITCH);
    const f32 ratio = calc_pitch_ratio(fctiwz(256.0f * semitones));
    f32 pan = 0.0f + c.getf(CH_PAN) + c.getf(CH_USER_PAN);
    if (c.get8(CH_LFO_TARGET) == 2)
        pan = pan + lfo_before;
    const f32 span = 0.0f + c.getf(CH_SPAN) + c.getf(CH_USER_SPAN);
    const f32 lpf = 1.0f + c.getf(CH_LPF_FREQ);
    const f32 main_out = 1.0f * c.getf(CH_MAIN_OUT_VOLUME);
    const f32 main_send = 0.0f + c.getf(CH_MAIN_SEND);
    const u8 remote_filter = c.get8(CH_REMOTE_FILTER);
    f32 fx[3], remote[4];
    for (int i = 0; i < 3; i++)
        fx[i] = 0.0f + c.getf(CH_FX_SEND + 4 * i);
    for (int i = 0; i < 4; i++)
        remote[i] = 1.0f * c.getf(CH_REMOTE_VOLUME + 4 * i);

    if (periodic) {
        if (c.get8(CH_AUTO_SWEEP)) {
            s32 length = c.gets32(CH_SWEEP_LENGTH), counter = c.gets32(CH_SWEEP_COUNTER) + (s32)FRAME_MS;
            c.set32(CH_SWEEP_COUNTER, (u32)(counter > length ? length : counter));
        }
        lfo_update(lfo, FRAME_MS);
        env_update(c, FRAME_MS);
    }
    f32 ve = ve_volume(c, lfo_value(lfo));

    GuestObj v(c.get32(CH_VOICE));
    if (!v)
        return;
    voice_set_pan_mode(v, c.get32(CH_PAN_MODE));
    voice_set_pan_curve(v, c.get32(CH_PAN_CURVE));
    voice_set_volume(v, volume);
    voice_set_ve_volume(v, ve, ve_before);
    voice_set_pitch(v, ratio * pitch_scale);
    voice_set_pan(v, pan);
    voice_set_surround_pan(v, span);
    voice_set_lpf_freq(v, lpf);
    voice_set_biquad_filter(v, c.get8(CH_BIQUAD_TYPE), c.getf(CH_BIQUAD_VALUE));
    voice_set_remote_filter(v, remote_filter);
    voice_set_output_line(v, c.get32(CH_OUTPUT_LINE));
    voice_set_main_out_volume(v, main_out);
    voice_set_main_send(v, main_send);
    for (u32 i = 0; i < 3; i++)
        voice_set_fx_send(v, i, fx[i]);
    for (u32 i = 0; i < 4; i++)
        voice_set_remote_out_volume(v, i, remote[i]);
}

}  // namespace engine::snd

// ---- the guest ABI (decl.h)
namespace g {

using namespace engine::snd;

// every channel in the manager's list (the next one read first: a channel that ends leaves it)
void nw4r_snd_detail_ChannelManager_UpdateAllChannel(u32 manager) {
    NOMA_PROF_SCOPE("(snd channels)");
    const u32 head = manager + 8;
    for (u32 link = guest::load32(head); link != head;) {
        u32 next = guest::load32(link);
        channel_update(link - 0xD8, true);
        link = next;
    }
}

void nw4r_snd_detail_Channel_Update(u32 channel, u32 periodic) {
    channel_update(channel, periodic != 0);
}

f64 nw4r_snd_detail_Util_CalcPitchRatio(u32 pitch) {
    return calc_pitch_ratio((s32)pitch);
}

// (an envelope or LFO anywhere: a channel's are at offsets 0 and 0x1C)
f64 nw4r_snd_detail_EnvGenerator_GetValue(u32 env) {
    return env_value(GuestObj(env));
}

void nw4r_snd_detail_EnvGenerator_Update(u32 env, u32 ms) {
    env_update(GuestObj(env), (s32)ms);
}

f64 nw4r_snd_detail_Lfo_GetValue(u32 lfo) {
    return lfo_value(GuestObj(lfo));
}

void nw4r_snd_detail_Lfo_Update(u32 lfo, u32 ms) {
    lfo_update(GuestObj(lfo), ms);
}

}  // namespace g
