// The channel's thread class (wii::Thread: an OSThread with its stack at 0x318/0x31C, filled with
// 0xDEADBABE when the thread is created): how much of the stack was never touched, counted from
// its far end one word at a time. The loader thread asks on every pass of its loop; here it is a
// host loop over the stack's words (NOMA_SELFTEST=stack checks it against the guest's loop).
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
        // the stack is one host block: compare its words in place, four at a time (the fill's
        // big-endian bytes DE AD BA BE read as a little-endian word)
        static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "host words are little-endian");
        const u32 F = 0xBEBAADDEu;
        const u32 lim = (end - base + 3) / 4 - 1;  // the words that can count
        u32 i = 0;
        for (; i + 4 <= lim; i += 4) {
            u32 w[4];
            ::memcpy(w, h + i * 4, 16);
            if ((w[0] ^ F) | (w[1] ^ F) | (w[2] ^ F) | (w[3] ^ F))
                break;
        }
        for (u32 w; i < lim && (::memcpy(&w, h + i * 4, 4), w == F);)
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
