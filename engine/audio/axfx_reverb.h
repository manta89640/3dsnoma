// AXFX ReverbHiExp, native (axfx_reverb.cpp): for a 3DS mixer that routes AX's aux buses itself.
// The effect's state stays the guest's AXFX_REVERBHI_EXP (set up by the game through AXFX); the
// samples can be the mixer's own.
#pragma once

#include "noma_decomp.h"

namespace engine::audio {

// three channels (left, right, surround) of host s32 samples
struct HostBus {
    s32* ch[3];
    s32 get(int c, u32 i) const { return ch[c][i]; }
    void set(int c, u32 i, s32 v) const { ch[c][i] = v; }
};

// one block of n samples (AX's frame is 96; any n works): `main` in place, `in`/`out` optional
// (the effect's bus-in and send, which the guest callback takes from the effect's bus pointers)
template <class Bus>
void reverb_hi_process(u32 fx, const Bus& main, const Bus* in, const Bus* out, u32 n);

}  // namespace engine::audio
