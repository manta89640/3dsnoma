// The channel's heaps (lib::allocator) fill every block they hand out with a fill value, a NaN
// (lib_allocator_sFillValue, 0x7FFFFFFF, set at start), so that memory nobody wrote shows as NaN:
// a word at a time, then 0xFF in the last size & 3 bytes. Making the room fills 29 MB this way,
// starting the channel 28 MB; word by word in emulated code that was 14-16% of those frames
// (Azahar). Here it is a host fill of the same bytes, with the poll points the loop had (one per
// word, charged a chunk at a time).
#include "noma_decomp.h"
#include "runtime.h"

namespace engine::system {

namespace {

const u32 CHUNK_WORDS = 4096;  // 16 KB between poll points

void fill_words(u32* d, u32 v, u32 n) {
    for (; n >= 8; n -= 8, d += 8)
        d[0] = v, d[1] = v, d[2] = v, d[3] = v, d[4] = v, d[5] = v, d[6] = v, d[7] = v;
    for (; n; n--)
        *d++ = v;
}

}  // namespace

// size bytes at dst; returns dst + size (where the fill ended)
u32 heap_fill(u32 dst, u32 size) {
    const u32 value = guest::load32(GA_lib_allocator_sFillValue);  // (the float's bits)
    const u32 words = size >> 2, tail = size & 3;
    u8* h = guest::host_ptr(dst);
    if (h && size && guest::host_ptr(dst + size - 1) == h + (size - 1) && ((uintptr_t)h & 3) == 0) {
        const u32 v = __builtin_bswap32(value);  // (guest memory is big-endian)
        u32* d = (u32*)h;
        for (u32 left = words; left;) {
            u32 n = left < CHUNK_WORDS ? left : CHUNK_WORDS;
            fill_words(d, v, n);
            d += n, left -= n;
            if ((guest::g_loop_budget -= (s32)n) < 0)
                guest::loop_poll();
        }
        ::memset(h + words * 4, 0xFF, tail);
        return dst + size;
    }
    for (u32 i = 0; i < words; i++) {  // (not one host block, or not aligned: as the loop did)
        guest::store32(dst + i * 4, value);
        if (--guest::g_loop_budget < 0)
            guest::loop_poll();
    }
    for (u32 i = 0; i < tail; i++)
        guest::store8(dst + words * 4 + i, 0xFF);
    return dst + size;
}

}  // namespace engine::system

// ---- the guest ABI (decl.h)
namespace g {

u32 lib_allocator_Fill(u32 dst, u32 size) { return engine::system::heap_fill(dst, size); }

}  // namespace g
