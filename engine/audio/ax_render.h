// AX voices, native: one voice's 3 ms frame (96 samples at 32 kHz) as the Wii's DSP renders it.
// The accelerator reads the samples (DSP-ADPCM, PCM16 or PCM8, with loop and end addresses), the
// sample rate converter resamples them, the volume envelope and the low-pass and biquad filters
// shape them, and they are added to the main and aux buses with each bus's volume ramp. The PB's
// state (addresses, ADPCM history, converter position, volumes, filter history) goes back to guest
// memory. runtime/hw/dsp_ax.cpp runs the AX command lists with it, on the host and the 3DS.
#pragma once

#include "noma_decomp.h"

namespace engine::audio {

const u32 AX_SPF = 96;  // samples per frame and channel

// the mixing buses: main L R S, then aux A, B and C (each L R S)
enum : int {
    AX_MAIN_L, AX_MAIN_R, AX_MAIN_S,
    AX_AUXA_L, AX_AUXA_R, AX_AUXA_S,
    AX_AUXB_L, AX_AUXB_R, AX_AUXB_S,
    AX_AUXC_L, AX_AUXC_R, AX_AUXC_S,
    AX_BUSES
};
typedef s32 AxBuses[AX_BUSES][AX_SPF];

// one frame of the voice whose PB is at `pb` (a guest address), added into `bus`. Only a running
// voice (its PB's running word is 1) plays; false otherwise.
bool ax_render_voice(u32 pb, AxBuses& bus);

// voice frames rendered so far by the way they went (NOMA_DEBUG_AX logs them): the one-pass path
// for main L and R by envelope, then the others by what kept them from it
enum AxPath {
    AXP_ONE, AXP_FIXED, AXP_RAMP, AXP_LOUD,  // one pass: the envelope at 1.0, fixed, ramping, above 1.0
    AXP_MUTED,                               // the envelope at 0
    AXP_MIX,                                 // other buses, a ramp or one side; filters; SRC type 2
    AXP_LEVELS,                              // volumes above 1.0, a ramp leaving 0 .. 1.0, too much input
    AXP_COUNT
};
extern u32 ax_paths[AXP_COUNT];

}  // namespace engine::audio
