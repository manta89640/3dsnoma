// The SDK's clock (OSTime.c), native: the time base is the host's monotonic clock at the Wii's
// rate (runtime/cpu.cpp timebase()), read once instead of mftbu/mftb/mftbu.
#include "noma_decomp.h"
#include "runtime.h"

namespace g {

u64 OSGetTime(void) {
    u64 t = guest::timebase();
    return NOMA_U64((u32)(t >> 32), (u32)t);
}

u32 OSGetTick(u32, u32) { return (u32)guest::timebase(); }

}  // namespace g
