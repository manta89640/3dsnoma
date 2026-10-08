// nw4r::snd's reset (the channel's exit): AxManager::PrepareReset puts its own AI DMA callback
// (AiDmaCallbackFunc: the output faded out over a few DMAs, then the old callback called on) in the
// AI's place, keeping the old one at the manager's +0x4C -- with what AIRegisterDMACallback returns,
// after it returns. AIRegisterDMACallback's OSRestoreInterrupts delivers an AI DMA interrupt pending
// from before it, and the new callback calls the old one through +0x4C, not yet kept: a call through
// 0. On the Wii that window is a few instructions; here interrupts held back while they were disabled
// arrive at the restore, and a quit from the island (the HOME Menu's close, the power button) hit it
// once in a host run (frames dumped every few frames, which stretches the time interrupts wait). So
// the callback does nothing while there is no old one to call: that one DMA's buffer is the DSP's
// last again, during a fade to silence.
#include "noma_decomp.h"
#include "runtime.h"

namespace g {

u32 nw4r_snd_detail_AxManager_AiDmaCallbackFunc_orig(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7, u32 r8, u32 r9, u32 r10,
                                                     f64 f1, f64 f2, f64 f3, f64 f4, f64 f5, f64 f6, f64 f7, f64 f8);

u32 nw4r_snd_detail_AxManager_AiDmaCallbackFunc(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7, u32 r8, u32 r9, u32 r10,
                                                f64 f1, f64 f2, f64 f3, f64 f4, f64 f5, f64 f6, f64 f7, f64 f8) {
    const u32 ax = nw4r_snd_detail_AxManager_GetInstance(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (!M2C_FIELD(ax, gp<u32>, 0x4C))
        return 0;
    return nw4r_snd_detail_AxManager_AiDmaCallbackFunc_orig(r3, r4, r5, r6, r7, r8, r9, r10, f1, f2, f3, f4, f5, f6, f7,
                                                            f8);
}

}  // namespace g
