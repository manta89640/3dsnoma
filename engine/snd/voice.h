// nw4r::snd::detail::Voice (engine/snd/voice.cpp): its layout, and what Channel
// (engine/snd/channel.cpp) uses of it.
#pragma once

#include "ax_voice.h"

namespace engine::snd {

// Voice
enum : u32 {
    V_AXVOICE = 0x0C,      // AxVoice*[channel][out]: + channel * 0x10 + out * 4
    V_OUT = 0x2C,          // VoiceOutParam[out], 0x18 each: volume, pitch, pan, span, fxSend, lpf
    V_CHANNELS = 0x8C,     // s32
    V_OUTS = 0x90,         // s32
    V_CALLBACK = 0x94,     // void (*)(Voice*, int status, void* data)
    V_CALLBACK_DATA = 0x98,
    V_ALLOCATED = 0x9C,    // u8
    V_ACTIVE = 0x9D,       // u8
    V_STARTED = 0x9E,      // u8
    V_PAUSE = 0x9F,        // u8: requested
    V_PAUSED = 0xA0,       // u8
    V_PITCH_ONLY = 0xA1,   // u8: the outs' pitches are not applied
    V_SYNC = 0xA2,         // u16: what to update
    V_REMOTE_FILTER = 0xA4,
    V_BIQUAD_TYPE = 0xA5,
    V_PAN = 0xAC,
    V_SPAN = 0xB0,
    V_LPF_FREQ = 0xB4,
    V_BIQUAD_VALUE = 0xB8,
    V_OUTPUT_LINE = 0xBC,  // u32: 1 main, 2 << i remote i
    V_MAIN_VOLUME = 0xC0,
    V_MAIN_SEND = 0xC4,
    V_FX_SEND = 0xC8,      // f32[3]: aux A, B, C
    V_REMOTE_VOLUME = 0xD4,  // f32[4]
    V_PITCH = 0xE4,
    V_VOLUME = 0xE8,
    V_VE_INIT_VOLUME = 0xEC,
    V_VE_VOLUME = 0xF0,
    V_PAN_MODE = 0xF4,     // s32: 1 balance
    V_PAN_CURVE = 0xF8,    // u32
    V_LINK = 0xFC,         // VoiceManager's list node
};

enum : u16 {
    SYNC_START = 0x1,
    SYNC_PAUSE = 0x2,
    SYNC_PITCH = 0x4,
    SYNC_VE = 0x8,
    SYNC_MIX = 0x10,
    SYNC_LPF = 0x20,
    SYNC_REMOTE_FILTER = 0x80,
    SYNC_BIQUAD = 0x100,
};

f32 calc_volume_ratio(f32 db);  // Util::CalcVolumeRatio: dB to an amplitude ratio

// Voice::Set*: store the value; mark what changed for the next Calc
void voice_set_volume(GuestObj v, f64 volume);
void voice_set_ve_volume(GuestObj v, f64 target, f64 init);
void voice_set_pitch(GuestObj v, f64 pitch);
void voice_set_pan_mode(GuestObj v, u32 mode);
void voice_set_pan_curve(GuestObj v, u32 curve);
void voice_set_pan(GuestObj v, f64 pan);
void voice_set_surround_pan(GuestObj v, f64 span);
void voice_set_lpf_freq(GuestObj v, f64 freq);
void voice_set_biquad_filter(GuestObj v, u32 type, f64 value);
void voice_set_remote_filter(GuestObj v, s32 filter);
void voice_set_output_line(GuestObj v, u32 line);
void voice_set_main_out_volume(GuestObj v, f64 volume);
void voice_set_main_send(GuestObj v, f64 send);
void voice_set_fx_send(GuestObj v, u32 bus, f64 send);
void voice_set_remote_out_volume(GuestObj v, u32 remote, f64 volume);

}  // namespace engine::snd
