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

// The heap whose memory holds addr: from `heap` down its tree of child heaps (children in a list at
// 0xC, its first entry at 0x10, siblings at 0x4), into each child whose Contains (vtable slot 0x14)
// says yes, until none does. Every free asks it from the root. The heap kinds' Contains are done here
// (the call through the vtable cost more than the comparison); any other is called.
u32 find_contain_heap(u32 heap, u32 addr) {
    enum : u32 {
        HEAP_STD_CONTAINS = 0x80189E24,  // [0x1C, 0x20)
        HEAP_UNT_CONTAINS = 0x8018A108,  // [0x1C, 0x1C + 0x24 * 0x28): blocks of one size
        ROOT_CONTAINS = 0x803654A0,      // everything
    };
    u32 child = guest::load32(heap + 0x10);
    while (child != heap + 0xC) {
        if (--guest::g_loop_budget < 0)
            guest::loop_poll();
        const u32 fn = guest::load32(guest::load32(child + 0x8) + 0x14);
        bool in;
        if (fn == HEAP_STD_CONTAINS) {
            in = guest::load32(child + 0x1C) <= addr && addr < guest::load32(child + 0x20);
        } else if (fn == HEAP_UNT_CONTAINS) {
            const u32 start = guest::load32(child + 0x1C);
            in = start <= addr && addr < start + guest::load32(child + 0x24) * guest::load32(child + 0x28);
        } else if (fn == ROOT_CONTAINS) {
            in = true;
        } else {
            in = ::gcall<u32>(fn, child, addr) != 0;
        }
        if (in)
            heap = child, child = guest::load32(child + 0x10);
        else
            child = guest::load32(child + 0x4);
    }
    return heap;
}

}  // namespace engine::system

// ---- the guest ABI (decl.h)
namespace g {

u32 lib_allocator_Fill(u32 dst, u32 size) { return engine::system::heap_fill(dst, size); }

u32 lib_allocator_HeapBase_FindContainHeap(u32 heap, u32 addr) { return engine::system::find_contain_heap(heap, addr); }

}  // namespace g
