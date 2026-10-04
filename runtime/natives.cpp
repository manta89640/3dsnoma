// Hand-written versions of functions that are assembly in the original and either touch
// machine state the decompiled code doesn't have (FPU context, caches, the decrementer) or are
// performance-critical library routines (memcpy/memset on guest memory).
#include "noma_decomp.h"
#include "runtime.h"

using namespace guest;

namespace g {

// ---- .init: memory routines (MSL's hand-optimized versions)
u32 memcpy(u32 dst, u32 src, u32 n) {
    memcpy_g(dst, src, n);
    return dst;
}

u32 memset(u32 dst, u32 c, u32 n) {
    memset_g(dst, (u8)c, n);
    return dst;
}

void __fill_mem(u32 dst, u32 c, u32 n) { memset_g(dst, (u8)c, n); }

// MSL's memcmp: -1 or 1 by the first bytes that differ (unsigned), 0 if none, in r3; r4 is left
// holding that byte of the first block (0 when equal), as the byte loop leaves it. The loop polled
// once a byte: charged in one go.
u64 memcmp(u32 a, u32 b, u32 n) {
    const u8 *ha = host_ptr(a), *hb = host_ptr(b);
    u32 i = 0;
    if (ha && hb && n && host_ptr(a + n - 1) == ha + (n - 1) && host_ptr(b + n - 1) == hb + (n - 1)) {
        while (i < n && ha[i] == hb[i])
            i++;
    } else {
        while (i < n && load8(a + i) == load8(b + i))
            i++;
    }
    if ((g_loop_budget -= (s32)(i + 1)) < 0)
        loop_poll();
    if (i == n)
        return NOMA_U64(0u, 0u);
    const u32 x = load8(a + i), y = load8(b + i);
    return NOMA_U64(x < y ? 0xFFFFFFFFu : 1u, x);
}

// ---- PPC special registers
void PPCMtdec(u32 v) { set_decrementer(v); }
u32 PPCMffpscr(void) { return 0; }
u64 PPCMtfpscr(u32) { return 0; }
void PPCSetFpNonIEEEMode(void) {}
void __OSFPRInit(void) {}

// ---- FPU context: each coroutine keeps its own host FPU state
void __OSLoadFPUContext(u32, u32) {}
void __OSSaveFPUContext(u32, u32, u32) {}
void OSSaveFPUContext(u32) {}
void OSSwitchFPUContext(u32, u32) {}

// ---- caches: guest memory is coherent. A flush or store is the guest telling the hardware it
// changed that memory: the write generations (memory.cpp) record it for the caches built from
// guest memory. The range functions work on whole 32-byte lines and leave r3 past the last one.
static u32 cache_lines(u32 a, u32 n) { return n ? (n + (a & 0x1F) + 0x1F) >> 5 : 0; }
static void flushed(u32 a, u32 n) {
    guest::mem_mark_written(a, n);
}
void DCFlushRange(u32 a, u32 n) { flushed(a, n); }
void DCStoreRange(u32 a, u32 n) { flushed(a, n); }
void DCInvalidateRange(u32, u32) {}
void DCFlushRangeNoSync(u32 a, u32 n) { flushed(a, n); }
void DCStoreRangeNoSync(u32 a, u32 n) { flushed(a, n); }
void DCZeroRange(u32 a, u32 n) {
    if (u32 lines = cache_lines(a, n))
        guest::memset_g(a & ~31u, 0, lines * 32);
}
u32 ICInvalidateRange(u32 a, u32 n) { return a + cache_lines(a, n) * 32; }
void __LCEnable(void) {}      // locked cache setup (HID2 LCE)

u32 OSGetStackPointer(void) { return g_sp; }

void PPCSync(void) {}  // PPCSync: "sc", whose handler runs "sync"
// __AI_SRC_INIT: measures how the sample rate converter starts (which only fits a window of a few
// microseconds on the console) to choose how long to wait for it; it ends by waiting that long
void __AI_SRC_INIT(void) { guest::host_sleep_us(50); }

// OSInitContext(ctx, pc, sp): a thread (re)starting at pc. A context that ran before (a thread
// created again on an OSThread that exited) gets a new coroutine at its next OSLoadContext.
void OSInitContext(u32 r3, u32 r4, u32 r5) {
    guest::threads_forget_context(r3);
    store32(r3 + 0x198, r4);       // srr0
    store32(r3 + 0x004, r5);       // r1
    store32(r3 + 0x19C, 0x9032);   // srr1: EE, ME, IR, DR, RI
    store32(r3 + 0x080, 0);        // cr
    store32(r3 + 0x08C, 0);        // xer
    store32(r3 + 0x008, 0x8088BDA0);  // r2, r13: the small data areas
    store32(r3 + 0x034, 0x808818A0);
    for (u32 r = 3; r < 32; r++)
        if (r != 13)
            store32(r3 + 4 * r, 0);
    for (u32 o = 0x1A4; o <= 0x1C0; o += 4)  // GQR1-7
        store32(r3 + o, 0);
    OSClearContext(r3);
}

// twui r0, 0 / blr: breakpoints for an attached debugger
void TRKAccessFile(void) { noma_trap("TRKAccessFile"); }
void TRKOpenFile(void) { noma_trap("TRKOpenFile"); }
void TRKCloseFile(void) { noma_trap("TRKCloseFile"); }
void TRKPositionFile(void) { noma_trap("TRKPositionFile"); }

}  // namespace g

namespace g {
// ---- memory protection / BAT setup (__OSInitMemoryProtection): no MMU to configure
void ConfigMEM1_24MB(void) {}
void ConfigMEM2_52MB(void) {}
void ConfigMEM2_56MB(void) {}
void ConfigMEM2_64MB(void) {}
void ConfigMEM2_112MB(void) {}
void ConfigMEM2_128MB(void) {}
void ConfigMEM_ES1_0(void) {}
void RealMode(u32) {}   // RealMode(fn): runs a BAT config routine with translation off
}  // namespace g
