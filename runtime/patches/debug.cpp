// Debugging aids on SDK functions; off unless their switch is set.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "noma_decomp.h"
#include "runtime.h"

namespace guest {
void platform_backtrace(const char* why);
}

namespace g {

u64 GXLoadPosMtxImm_orig(u32 r3, u32 r4);
u32 nw4r_g3d_ScnMdlSimple_GetScnMtxPos_orig(u32 r3, u32 r4, u32 r5, u32 r6);

static bool has_nan(u32 m, u32 words) {
    for (u32 i = 0; i < words; i++) {
        u32 v = guest::load32(m + 4 * i);
        if ((v & 0x7F800000) == 0x7F800000 && (v & 0x7FFFFF))
            return true;
    }
    return false;
}

// GXLoadPosMtxImm(mtx, id). NOMA_DEBUG_GX_NAN: the first matrices holding a NaN are reported
// with their guest address (for NOMA_WATCH) and the host stack.
u64 GXLoadPosMtxImm(u32 r3, u32 r4) {
    static bool s_on = ::getenv("NOMA_DEBUG_GX_NAN") != nullptr;
    static int s_left = 3;
    if (s_on && s_left > 0) {
        for (u32 i = 0; i < 12; i++) {
            u32 v = guest::load32(r3 + 4 * i);
            if ((v & 0x7F800000) == 0x7F800000 && (v & 0x7FFFFF)) {
                s_left--;
                char why[96];
                ::snprintf(why, sizeof(why), "gx: GXLoadPosMtxImm(%08X, %u): NaN %08X at %08X, from:", r3, r4, v,
                         r3 + 4 * i);
                guest::platform_backtrace(why);
                break;
            }
        }
    }
    return GXLoadPosMtxImm_orig(r3, r4);
}

// nw4r::g3d::ScnMdl's matrix of node r6 (r5 1: world, 2: view; view matrices are double
// buffered: +0xF4 the buffers, +0x101 the one in use, +0x102 matrices per buffer).
// NOMA_DEBUG_GX_NAN: report the first ones that hold a NaN.
u32 nw4r_g3d_ScnMdlSimple_GetScnMtxPos(u32 r3, u32 r4, u32 r5, u32 r6) {
    static bool s_on = ::getenv("NOMA_DEBUG_GX_NAN") != nullptr;
    static int s_left = 3;
    u32 r = nw4r_g3d_ScnMdlSimple_GetScnMtxPos_orig(r3, r4, r5, r6);
    if (s_on && s_left > 0 && r && r4 && has_nan(r4, 12)) {
        s_left--;
        guest::log("gx: ScnMdl %08X matrix %u of node %u holds a NaN: world %08X, view %08X (buffer %u, %u "
                   "matrices)", r3, r5, r6, guest::load32(r3 + 0xEC), guest::load32(r3 + 0xF4),
                   guest::load8(r3 + 0x101), guest::load16(r3 + 0x102));
    }
    return r;
}

}  // namespace g
