// The SDK's interrupt control (OSInterrupt.c), native. The guest's MSR[EE] lives in g_msr
// (runtime/cpu.cpp); turning interrupts back on through __mtmsr takes any that are pending (or
// idles, from the scheduler's idle context), as the hardware does after mtmsr.
#include "noma_decomp.h"
#include "runtime.h"

namespace g {

// r3: whether interrupts were on; r4: the new MSR, as the SDK's version leaves it
u64 OSDisableInterrupts(void) {
    u32 old = guest::g_msr;
    if (old & 0x8000u)
        __mtmsr(old & ~0x8000u);
    return NOMA_U64((old >> 15) & 1, guest::g_msr);
}

u32 OSEnableInterrupts(void) {
    u32 old = guest::g_msr;
    if (!(old & 0x8000u))
        __mtmsr(old | 0x8000u);
    return (old >> 15) & 1;
}

// r3: whether interrupts were on; r4: the MSR before (as the SDK's version leaves it)
u64 OSRestoreInterrupts(u32 level) {
    u32 old = guest::g_msr;
    if (level && !(old & 0x8000u))
        __mtmsr(old | 0x8000u);
    else if (!level && (old & 0x8000u))
        __mtmsr(old & ~0x8000u);
    return NOMA_U64((old >> 15) & 1, old);
}

}  // namespace g
