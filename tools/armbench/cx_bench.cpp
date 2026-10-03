// engine/cx/uncompress.cpp as the 3DS build compiles it, driven under Unicorn by armbench.py --cx:
// a compressed stream (LZ or Huffman, written into cx_in by the script) decoded by the native and
// by the streaming code it started from (engine/cx/cx_reference.h), for instruction counts and a
// hash of each output.
#include <cstring>

#include "engine/cx/cx_reference.h"

namespace guest {
uintptr_t g_page[1u << (32 - PAGE_BITS)];
u32 g_sp;
FnSlot g_fn_cache[4096];
s32 g_loop_budget = 20000;
void loop_poll() { g_loop_budget = 20000; }
u32 mmio_read(u32, int) { return 0; }
void mmio_write(u32, u32, int) {}
void* lookup_fn_slow(u32) { return nullptr; }
void fatal(const char*, ...) {
    for (;;) {
    }
}
}  // namespace guest

namespace g {
u32 CXiVerifyHuffmanTable(u32, u32) { return 1; }
}  // namespace g

// guest memory: the context at 0x80000000, the input at 0x90100000, the output at 0x90400000
const u32 CTX = 0x80000000, IN = 0x90100000, OUT = 0x90400000;
u8 s_ctx[0x1000] __attribute__((aligned(32)));
extern "C" {
u8 cx_in[0x80000];
}
u8 s_out[0x100000];

// the stream's len bytes in pieces of `piece`, by the native (or the reference); with `hash`, an
// FNV-1a hash of the output instead of the call's result
extern "C" u32 cx(u32 native, u32 len, u32 piece, u32 hash) {
    guest::g_page[CTX >> guest::PAGE_BITS] = (uintptr_t)s_ctx - CTX;
    guest::g_page[IN >> guest::PAGE_BITS] = (uintptr_t)cx_in - IN;
    guest::g_page[OUT >> guest::PAGE_BITS] = (uintptr_t)s_out - OUT;
    bool huffman = (cx_in[0] & 0xF0) == 0x20;
    memset(s_ctx, 0, 0x220);
    guest::store32(CTX, OUT);
    if (huffman) {  // CXInitUncompContextHuffman (0x802FFBC0)
        guest::store32(CTX + 0x0C, CTX + 0x20);
        guest::store16(CTX + 0x18, 0xFFFF);
        guest::store8(CTX + 0x1D, 8);
    } else {  // CXInitUncompContextLZ (0x802FFB80)
        guest::store8(CTX + 0x10, 3);
        guest::store8(CTX + 0x13, 8);
    }
    namespace ref = engine::cx::reference;
    u32 r = 0;
    for (u32 at = 0; at < len; at += piece) {
        u32 n = len - at < piece ? len - at : piece;
        if (huffman)
            native ? g::CXReadUncompHuffman(CTX, IN + at, n) : ref::read_uncomp_huffman(CTX, IN + at, n);
        else
            r = native ? g::CXReadUncompLZ(CTX, IN + at, n) : ref::read_uncomp_lz(CTX, IN + at, n);
    }
    if (!hash)
        return r;
    u32 h = 2166136261u;
    for (u32 i = 0, end = guest::load32(CTX) - OUT; i < end; i++)
        h = (h ^ s_out[i]) * 16777619u;
    return h ^ r;
}
