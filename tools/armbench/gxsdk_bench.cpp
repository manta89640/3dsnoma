// engine/gx/gx_sdk.cpp against the decompiled GX library it replaces, under Unicorn (armbench.py
// --gxsdk): the decompiled functions (copied from build/decomp/src by the script, renamed dec_*) and
// the natives run on the same random __GXData and arguments; every byte they send down the
// write-gather pipe, the guest memory they leave and what they return must be the same.
//   run(fn, native, first, trials) -> a hash of all of it over trials [first, first + trials) (fn: an
//   index into the switch below); count(fn, native, calls): the calls alone on one state, for
//   instruction counts. After a run of one trial, gx_sent/gx_sent_n and gx_mem1 hold what it left.
#include <cstring>

#include "noma_decomp.h"

namespace guest {
uintptr_t g_page[1u << (32 - PAGE_BITS)];
u32 g_sp;
FnSlot g_fn_cache[4096];
s32 g_loop_budget = 1 << 30;
void loop_poll() { g_loop_budget = 1 << 30; }
void gx_fifo_write(u32, u32 value, int size);
u32 mmio_read(u32, int) { return 0; }
void mmio_write(u32 addr, u32 value, int size) {  // (as the runtime's: the pipe at any address)
    if ((addr & 0x0FFFF000) == 0x0C008000)
        gx_fifo_write(addr & 0xFFF, value, size);
}
void fatal(const char*, ...) {
    for (;;) {
    }
}
alignas(4) u8 g_gather[32];
u32 g_gather_n;
// what went down the pipe, burst by burst
extern "C" {
u8 gx_sent[1 << 16];
u32 gx_sent_n;
}
#define s_sent gx_sent
#define s_sent_n gx_sent_n
void gx_gather_burst() {
    if (s_sent_n + 32 <= sizeof(s_sent))
        memcpy(s_sent + s_sent_n, g_gather, 32), s_sent_n += 32;
    g_gather_n = 0;
}
void gx_fifo_write(u32, u32 value, int size) {
    for (int i = size - 1; i >= 0; i--) {
        g_gather[g_gather_n++] = (u8)(value >> (i * 8));
        if (g_gather_n == 32)
            gx_gather_burst();
    }
}
// the TLUT region callback a paletted texture object asks for (GXLoadTexObjPreLoaded)
u32 tlut_region(u32 name, u32 gxdata, u32, u32, u32, u32, u32, u32) { return 0x80810000 + (name & 7) * 16; }
void* lookup_fn_slow(u32) { return (void*)&tlut_region; }
}  // namespace guest

namespace g {
// the decompiled ones (dec.cpp, made by the script) and the natives (engine/gx/gx_sdk.cpp)
void dec___GXSetDirtyState();
void dec___GXSetSUTexRegs();
void dec___GXSetVCD();
void dec___GXSetVAT();
void dec___GXCalculateVLim();
void dec___GXSetMatrixIndex(u32);
void dec_GXSetTevColorS10(u32, u32);
void dec_GXSetTevKColor(u32, u32);
u32 dec_GXSetTevSwapModeTable(u32, u32, u32, u32, u32);
void dec_GXLoadTexMtxImm(u32, u32, u32);
u32 dec_GXLoadTexObjPreLoaded(u32, u32, u32);
u32 dec_GXBegin(u32, u32, u32);
void dec_GXSetArray(u32, u32, u32);
u32 dec_GXLoadTexObj(u32, u32);
void dec_GXSetCurrentMtx(u32);
u32 dec_GXCallDisplayList(u32, u32);
void dec_GXLoadLightObjImm(u32, u32, u32, u32);
void dec_GXLoadNrmMtxImm(u32, u32);
u64 dec_GXSetVtxDesc(u32, u32);
void dec_GXClearVtxDesc();
u64 dec_GXSetVtxAttrFmt(u32, u32, u32, u32, u32);
}  // namespace g

using namespace guest;

namespace {

// guest memory: MEM1 at 0x80000000 (the globals, __GXData's pointer among them)
const u32 MEM1 = 0x80000000, MEM1_BYTES = 0x900000;
}  // namespace
extern "C" u8 gx_mem1[0x900000] __attribute__((aligned(32)));
u8 gx_mem1[0x900000] __attribute__((aligned(32)));
#define s_mem1 gx_mem1
namespace {
const u32 GXDATA = 0x80800000;  // the GXData structure
const u32 OBJ = 0x80808000, REGION = 0x80808100, MTX = 0x80808200, COLOR = 0x80808300;
// what a test may change: GXData, the objects, the TLUT regions
const u32 TOUCHED = 0x80800000, TOUCHED_BYTES = 0x11000;

u32 s_rand;
u32 rnd() { return s_rand = s_rand * 1103515245u + 12345u, (s_rand >> 16) | (s_rand << 16); }

u32 hash(u32 h, const u8* p, u32 n) {
    for (u32 i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

// a random state of what the functions read and write (the rest of MEM1 is the DOL's sections, loaded by
// the script: the library's constant tables)
void randomize() {
    u8* gx = s_mem1 + (GXDATA - MEM1);
    for (u32 i = 0; i < 0x600; i++)
        gx[i] = (u8)rnd();
    for (u32 i = 0; i < 0x400; i++)
        s_mem1[(OBJ - MEM1) + i] = (u8)rnd();
    for (u32 i = 0; i < 0x1000; i++)
        s_mem1[0x810000 + i] = (u8)rnd();
    store32(GA___GXData, GXDATA);
}

void call(u32 fn, u32 native, u32 a, u32 b, u32 c, u32 d, u32* r) {
    switch (fn) {
    case 0: native ? g::__GXSetDirtyState() : g::dec___GXSetDirtyState(); break;
    case 1: native ? g::__GXSetSUTexRegs() : g::dec___GXSetSUTexRegs(); break;
    case 2: native ? g::__GXSetVCD() : g::dec___GXSetVCD(); break;
    case 3: native ? g::__GXSetVAT() : g::dec___GXSetVAT(); break;
    case 4: native ? g::__GXCalculateVLim() : g::dec___GXCalculateVLim(); break;
    case 5: native ? g::__GXSetMatrixIndex(a % 10) : g::dec___GXSetMatrixIndex(a % 10); break;
    case 6: native ? g::GXSetTevColorS10(a & 3, COLOR) : g::dec_GXSetTevColorS10(a & 3, COLOR); break;
    case 7: native ? g::GXSetTevKColor(a & 3, COLOR) : g::dec_GXSetTevKColor(a & 3, COLOR); break;
    case 8:
        *r = native ? g::GXSetTevSwapModeTable(a & 3, b & 3, c & 3, d & 3, (a >> 8) & 3)
                    : g::dec_GXSetTevSwapModeTable(a & 3, b & 3, c & 3, d & 3, (a >> 8) & 3);
        break;
    case 9: {
        u32 id = (b & 1) ? 0x40 + (a % 20) * 3 : (a % 20) * 3, type = c & 1;
        native ? g::GXLoadTexMtxImm(MTX, id, type) : g::dec_GXLoadTexMtxImm(MTX, id, type);
        break;
    }
    case 10:
        *r = native ? g::GXLoadTexObjPreLoaded(OBJ, REGION, a & 7) : g::dec_GXLoadTexObjPreLoaded(OBJ, REGION, a & 7);
        break;
    case 11:
        if (a & 1) {  // (the flush of a primitive after BP writes, a short one)
            store32(GXDATA, 0);
            store16(GXDATA + 4, (u16)(b & 15)), store16(GXDATA + 6, (u16)(c & 15));
        }
        *r = native ? g::GXBegin((a >> 4) & 0xF8, b & 7, c & 0xFFFF) : g::dec_GXBegin((a >> 4) & 0xF8, b & 7, c & 0xFFFF);
        break;
    case 12: native ? g::GXSetArray(9 + a % 17, b, c) : g::dec_GXSetArray(9 + a % 17, b, c); break;
    case 13: *r = native ? g::GXLoadTexObj(OBJ, a & 7) : g::dec_GXLoadTexObj(OBJ, a & 7); break;
    case 14: native ? g::GXSetCurrentMtx(a) : g::dec_GXSetCurrentMtx(a); break;
    case 15:
        if (a & 1)
            store32(GXDATA, 0);
        *r = native ? g::GXCallDisplayList(b, c) : g::dec_GXCallDisplayList(b, c);
        break;
    case 16: native ? g::GXLoadLightObjImm(OBJ, b, 0, 0) : g::dec_GXLoadLightObjImm(OBJ, b, 0, 0); break;
    case 17: native ? g::GXLoadNrmMtxImm(OBJ, a % 40) : g::dec_GXLoadNrmMtxImm(OBJ, a % 40); break;
    case 18: {
        u64 v = native ? g::GXSetVtxDesc(a % 28, b & 3) : g::dec_GXSetVtxDesc(a % 28, b & 3);
        *r = (u32)v ^ (u32)(v >> 32) * 31;
        break;
    }
    case 19: native ? g::GXClearVtxDesc() : g::dec_GXClearVtxDesc(); break;
    case 20: {
        u32 vf = (a & 0x100) ? a >> 9 : a & 7;  // (a format past the tables now and then)
        u64 v = native ? g::GXSetVtxAttrFmt(vf & 0xFF, b % 27, c & 3, d & 7, (d >> 4) & 31)
                       : g::dec_GXSetVtxAttrFmt(vf & 0xFF, b % 27, c & 3, d & 7, (d >> 4) & 31);
        *r = (u32)v ^ (u32)(v >> 32) * 31;
        break;
    }
    }
}

void map_memory() {
    for (u32 i = 0; i < MEM1_BYTES >> PAGE_BITS; i++)
        g_page[(MEM1 >> PAGE_BITS) + i] = (uintptr_t)s_mem1 - MEM1;
}

}  // namespace

extern "C" u32 run(u32 fn, u32 native, u32 first, u32 trials) {
    map_memory();
    u32 h = 2166136261u;
    for (u32 t = first; t < first + trials; t++) {
        s_rand = t * 104729u + fn * 15485863u + 1;
        randomize();
        s_sent_n = 0, g_gather_n = 0;
        const u32 a = rnd(), b = rnd(), c = rnd(), d = rnd();
        u32 r = 0;
        call(fn, native, a, b, c, d, &r);
        h = hash(h, s_sent, s_sent_n);
        h = hash(h, g_gather, g_gather_n);
        h = hash(h ^ r ^ g_gather_n * 31, s_mem1 + (TOUCHED - MEM1), TOUCHED_BYTES);
    }
    return h;
}

extern "C" u32 count(u32 fn, u32 native, u32 calls) {
    map_memory();
    s_rand = fn + 1;
    randomize();
    u32 r = 0;
    for (u32 i = 0; i < calls; i++) {
        s_sent_n = 0;
        call(fn, native, i * 2654435761u, i >> 1, i >> 2, i >> 3, &r);
    }
    return r;
}
