// The AX's aux buses, which feed the channel's effects (nw4r's reverb): their callbacks, logged as
// they're registered (the channel registers none at start, in the room or in the theater). The
// voices are mixed natively, aux buses included (runtime/hw/dsp_ax.cpp), so effects run as on
// the Wii.
#include "noma_decomp.h"
#include "runtime.h"

namespace g {

u64 AXRegisterAuxACallback_orig(u32 callback, u32 context);
void AXRegisterAuxBCallback_orig(u32 callback, u32 context);
void AXRegisterAuxCCallback_orig(u32 callback, u32 context);

static void log_aux(char bus, u32 callback, u32 context) {
    guest::log("ax: aux %c callback %08X (context %08X)", bus, callback, context);
}

u64 AXRegisterAuxACallback(u32 callback, u32 context) {
    log_aux('A', callback, context);
    return AXRegisterAuxACallback_orig(callback, context);
}

void AXRegisterAuxBCallback(u32 callback, u32 context) {
    log_aux('B', callback, context);
    AXRegisterAuxBCallback_orig(callback, context);
}

void AXRegisterAuxCCallback(u32 callback, u32 context) {
    log_aux('C', callback, context);
    AXRegisterAuxCCallback_orig(callback, context);
}

}  // namespace g
