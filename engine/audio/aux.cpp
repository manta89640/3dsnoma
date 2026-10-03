// The AX's aux buses, which feed the channel's effects (nw4r's reverb). With the voices on the 3DS's
// DSP (dsp_ndsp.cpp) their aux sends aren't mixed, so an effect would only process silence -- the
// reverb was 5% of all the CPU's time. Its callbacks aren't registered then; where the CPU mixes
// the voices (the host, dsp_ax.cpp) they are, as on the Wii.
#include "noma_decomp.h"
#include "runtime.h"

namespace g {

u64 AXRegisterAuxACallback_orig(u32 callback, u32 context);
void AXRegisterAuxBCallback_orig(u32 callback, u32 context);
void AXRegisterAuxCCallback_orig(u32 callback, u32 context);

u64 AXRegisterAuxACallback(u32 callback, u32 context) {
    return AXRegisterAuxACallback_orig(guest::audio_dsp_native() ? 0 : callback, context);
}

void AXRegisterAuxBCallback(u32 callback, u32 context) {
    AXRegisterAuxBCallback_orig(guest::audio_dsp_native() ? 0 : callback, context);
}

void AXRegisterAuxCCallback(u32 callback, u32 context) {
    AXRegisterAuxCCallback_orig(guest::audio_dsp_native() ? 0 : callback, context);
}

}  // namespace g
