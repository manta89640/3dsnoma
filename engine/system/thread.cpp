// The channel's thread class (wii::Thread: an OSThread with its stack at 0x318/0x31C, filled with
// 0xDEADBABE when the thread is created): how much of the stack was never touched, counted from
// its far end one word at a time. Asked every frame for each thread, it walked whole stacks in
// emulated code; here it is a host loop over the stack's bytes.
#include "noma_decomp.h"

namespace engine::system {

namespace {

enum : u32 { T_STACK_BASE = 0x318, T_STACK_SIZE = 0x31C };
const u32 STACK_FILL = 0xDEADBABEu;

// the bytes from the stack's base that still hold the fill (the last word read is never counted)
u32 unused_stack(u32 thread) {
    u32 base = guest::load32(thread + T_STACK_BASE);
    u32 end = base + guest::load32(thread + T_STACK_SIZE);
    u32 n = 0;
    const u8* h = guest::host_ptr(base);
    if (h && end > base && guest::host_ptr(end - 1) == h + (end - 1 - base)) {
        // the stack is one host block: compare the big-endian words in place
        static const u8 fill[4] = {0xDE, 0xAD, 0xBA, 0xBE};
        u32 words = (end - base) / 4;
        u32 i = 0;
        while (i < words && ::memcmp(h + i * 4, fill, 4) == 0 && base + (i + 1) * 4 < end)
            i++;
        n = i;
    } else {
        for (u32 p = base;;) {
            u32 w = guest::load32(p);
            p += 4;
            if (w != STACK_FILL || p >= end)
                break;
            n++;
        }
    }
    return n * 4;
}

}  // namespace

}  // namespace engine::system

// ---- the guest ABI (decl.h)
namespace g {

u32 wii_Thread_GetUnusedStackSize(u32 thread) {
    return engine::system::unused_stack(thread);
}

}  // namespace g
