// nw4r::snd::detail::AxVoice, native (engine/snd/ax_voice.cpp): the layer between nw4r's voices
// and the SDK's AX voices. It writes the AX parameter blocks (AXVPB) exactly as nw4r does -- the
// fields, and the sync flags that tell the DSP (runtime/hw/dsp_ax.cpp)
// what changed -- so the backends see the same blocks as from the decompiled code.
//
// An AxVoice (guest memory) starts with its AxVoiceParamBlock: the AXVPB and the sync flags
// collected until Voice::Update hands them to the AXVPB (Sync).
#pragma once

#include "guest_obj.h"

namespace engine::snd {

// AxVoice
enum : u32 {
    AXV_PB = 0x00,          // AXVPB*
    AXV_SYNC = 0x04,        // u32: sync flags not yet given to the AXVPB
    AXV_VE_VOL = 0x08,      // u16: volume envelope: current volume
    AXV_VE_DELTA = 0x0A,    // s16: its step per sample
    AXV_VE_FIRST = 0x0C,    // u8: the next SetVe sets the current volume too
    AXV_VE_TARGET = 0x0E,   // u16: the volume to ramp to
    AXV_DATA = 0x10,        // the wave data (0: none)
    AXV_FORMAT = 0x14,      // s32: sample format (1, 2: PCM, 3: ADPCM)
    AXV_RATE = 0x18,        // s32: sample rate
    AXV_MIX_FIRST = 0x1C,   // u8: the next SetMix starts at its values, without a ramp
    AXV_MIX = 0x1E,         // u16[12]: the mix volumes reached (MixParam order)
};

// AXVPB
enum : u32 {
    PB_SYNC = 0x1C,         // u32: what changed (AX_SYNC_*)
    PB_DEPOP = 0x20,        // u32: ramp the voice's output down (set when it stops)
    PB_MIXER_CTRL = 0x34,
    PB_STATE = 0x38,        // u16: 1 running
    PB_MIX = 0x3C,          // AXPBMIX: 12 volumes with their steps
    PB_VE = 0x92,           // AXPBVE: currentVolume, currentDelta
    PB_ADDR_CUR = 0xA2,     // current address, high and low half
    PB_SRC = 0xCE,          // AXPBSRC: ratio (16.16), position, last samples (14 bytes)
    PB_LPF = 0xE2,          // AXPBLPF: on, yn1, a0, b0
    PB_BIQUAD = 0xEA,       // AXPBBIQUAD: on, xn1, xn2, yn1, yn2, b0, b1, b2, a1, a2
    PB_REMOTE = 0xFE,
    PB_RMT_MIXER_CTRL = 0x100,
    PB_RMT_MIX = 0x102,     // AXPBRMTMIX: 4 speakers x (main, aux) with steps
    PB_RMT_IIR = 0x13C,     // AXPBRMTIIR: the remote speaker's filter
};

// a MixParam: main, aux A, aux B, aux C buses, each L R S
struct MixParam {
    u16 v[12];
};
// a RemoteMixParam: 4 speakers, each main and fx (fx unused: 0)
struct RemoteMixParam {
    u16 v[8];
};

u32 ax_manager();  // nw4r::snd::detail::AxManager::GetInstance()
u8* ax_manager_ptr();  // ... in host memory

void ax_set_src(GuestObj ax, bool initial, f32 pitch);
void ax_set_ve(GuestObj ax, f32 volume, f32 init_volume);
bool ax_set_mix(GuestObj ax, const MixParam& mix);  // true: the mix is still ramping
void ax_set_rmt_mix(GuestObj ax, const RemoteMixParam& mix);
void ax_enable_remote(GuestObj ax, u16 on);
void ax_set_lpf(GuestObj ax, u16 freq);
void ax_set_biquad(GuestObj ax, u8 type, f32 value);
void ax_set_remote_filter(GuestObj ax, s32 value);
void ax_sync(GuestObj ax);
bool ax_is_play_finished(GuestObj ax);
void ax_reset_delta(GuestObj ax);
void ax_set_voice_state(GuestObj pb, u16 state);  // the SDK's AXSetVoiceState

}  // namespace engine::snd
