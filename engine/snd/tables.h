// nw4r::snd's lookup tables (engine/snd/tables.cpp).
#pragma once

#include "guest.h"

namespace engine::snd {

extern const f32 kPanTable[3][257];          // by PanCurve, then (pan + 1) * 128
extern const f32 kVolumeTable[965];          // by dB * 10 + 904
extern const u16 kLpfFreqTable[24];          // Hz
extern const u16 kRemoteFilterTable[128][5];  // b0 b1 b2 a1 a2
extern const s8 kLfoTable[33];               // a quarter sine, 0..127
extern const s16 kSustainTable[128];         // 0.1 dB
extern const f32 kSemitoneTable[12];
extern const f32 kFineTuneTable[256];        // by 1/256 semitone

}  // namespace engine::snd
