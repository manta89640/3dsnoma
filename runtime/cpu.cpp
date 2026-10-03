// PowerPC state the decompiled code reaches through intrinsics: special-purpose registers
// (GQRs for quantized loads, HIDs, decrementer, time base) and the MSR's interrupt enable.
#include <cmath>
#include <cstdlib>

#include "runtime.h"
#include "hw/hw.h"

namespace guest {

u32 g_msr = 0;  // external interrupts (EE) are off until the OS turns them on
u32 g_gqr[8];
u32 g_hid[5];
static u32 s_spr[1024];
static u64 s_tb_base;
static u64 s_dec_deadline;  // timebase value at which the decrementer fires (0: off)

u64 timebase() { return host_timebase() + s_tb_base; }

// Before a title runs, the system sets the time base from the RTC: it counts from 2000-01-01 in
// local time, and OSGetTime is the date.
void timebase_init() { s_tb_base = (u64)rtc_seconds() * TB_HZ - host_timebase(); }

void set_decrementer(u32 value) {
    // the decrementer counts down at the timebase rate and interrupts on the 0 -> -1 edge
    s_dec_deadline = timebase() + (u64)(s32)value + 1;
    if ((s32)value < 0)
        s_dec_deadline = timebase();
}

u64 decrementer_deadline() { return s_dec_deadline; }
void decrementer_clear() { s_dec_deadline = 0; }

u32 lc_read(u32 addr, int size);
void lc_write(u32 addr, u32 value, int size);

// The locked cache's DMA (DMA_U 922: memory address, the length's high bits; DMA_L 923: the
// cache address, LD (bit 4: memory to cache, else back), the length's low bits, T (bit 1: go)),
// in 32-byte blocks, 0 meaning 128. NW4R computes model matrices in the locked cache and stores
// them with it (LCStoreData). The transfer is done at once: HID2's queue length stays 0.
static void lc_dma(u32 u, u32 l) {
    u32 blocks = ((u & 0x1F) << 2) | ((l >> 2) & 3);
    if (!blocks)
        blocks = 128;
    u32 mem = 0x80000000u | (u & 0x1FFFFFE0), lc = l & 0xFFFFFFE0;
    bool load = (l & 0x10) != 0;
    for (u32 i = 0; i < blocks * 32; i += 4) {
        if (load)
            lc_write(lc + i, load32(mem + i), 4);
        else
            store32(mem + i, lc_read(lc + i, 4));
    }
}

}  // namespace guest

using namespace guest;

void __mtspr(u32 spr, u32 v) {
    switch (spr) {
    case 912: case 913: case 914: case 915: case 916: case 917: case 918: case 919:
        g_gqr[spr - 912] = v;
        break;
    case 22:  // DEC
        set_decrementer(v);
        break;
    case 284:  // TBL (written by OSSetTime)
    case 285:
        break;
    case 1008: g_hid[0] = v; break;
    case 1009: g_hid[1] = v; break;
    case 920: g_hid[2] = v; break;
    case 1011: g_hid[4] = v; break;
    case 923:  // DMA_L
        if (v & 2)
            lc_dma(s_spr[922], v);
        break;
    default:
        break;
    }
    if (spr < 1024)
        s_spr[spr] = v;
}

u32 __mfspr(u32 spr) {
    switch (spr) {
    case 912: case 913: case 914: case 915: case 916: case 917: case 918: case 919:
        return g_gqr[spr - 912];
    case 22: {  // DEC
        u64 d = decrementer_deadline();
        u64 now = timebase();
        return d > now ? (u32)(d - now) : 0xFFFFFFFFu;
    }
    case 268: case 284:
        return (u32)timebase();
    case 269: case 285:
        return (u32)(timebase() >> 32);
    case 287:  // PVR: Broadway
        return 0x00087102;
    case 1008: return g_hid[0];
    case 1009: return g_hid[1];
    case 920: return g_hid[2];
    case 1011: return g_hid[4];
    default:
        return spr < 1024 ? s_spr[spr] : 0;
    }
}

u32 __mftb(void) { return (u32)timebase(); }
u32 __mftbu(void) { return (u32)(timebase() >> 32); }
u32 __mfmsr(void) { return g_msr; }

namespace guest {
bool in_idle_context();
}

void noma_trace_snapshot();
namespace guest {
u64 g_ee_off_since;  // (coarse) host time when the guest last turned interrupts off (0: they're on)
}

void __mtmsr(u32 v) {
    u32 old = g_msr;
    g_msr = v;
    if ((old & 0x8000) && !(v & 0x8000)) {
        g_ee_off_since = g_coarse_us | 1;
        static bool s_snap = getenv("NOMA_DEBUG_EE") != nullptr;
        if (s_snap)
            noma_trace_snapshot();
    } else if (v & 0x8000) {
        g_ee_off_since = 0;
    }
    if (!(old & 0x8000) && (v & 0x8000)) {
        if (in_idle_context())
            idle_wait();  // the scheduler's idle loop: block until a thread is ready
        else
            check_interrupts();
    }
}

// ---- quantized paired-single loads/stores (GQR type: 0 f32, 4 u8, 5 u16, 6 s8, 7 s16)
static f32 dequant(u32 ea, u32 type, u32 scale) {
    f32 v;
    switch (type) {
    case 4: v = (f32)load8(ea); break;
    case 5: v = (f32)load16(ea); break;
    case 6: v = (f32)(s8)load8(ea); break;
    case 7: v = (f32)(s16)load16(ea); break;
    default: {
        u32 b = load32(ea);
        ::memcpy(&v, &b, 4);
        return v;
    }
    }
    s32 s = (s32)(scale << 26) >> 26;  // 6-bit signed
    return ldexpf(v, -s);
}

static u32 qsize(u32 type) {
    switch (type) {
    case 4: case 6: return 1;
    case 5: case 7: return 2;
    default: return 4;
    }
}

f32 __psq_ld0(u32 ea, u32 qr) {
    u32 gqr = g_gqr[qr & 7];
    return dequant(ea, (gqr >> 16) & 7, (gqr >> 24) & 0x3F);
}

f32 __psq_ld1(u32 ea, u32 w, u32 qr) {
    if (w)
        return 1.0f;
    u32 gqr = g_gqr[qr & 7];
    u32 type = (gqr >> 16) & 7;
    return dequant(ea + qsize(type), type, (gqr >> 24) & 0x3F);
}

static void quant_store(u32 ea, f32 v, u32 type, u32 scale) {
    if (type == 0 || type > 7 || (type >= 1 && type <= 3)) {
        u32 b;
        ::memcpy(&b, &v, 4);
        store32(ea, b);
        return;
    }
    s32 s = (s32)(scale << 26) >> 26;
    f32 x = ldexpf(v, s);
    switch (type) {
    case 4: store8(ea, (u8)(x < 0 ? 0 : x > 255 ? 255 : (s32)x)); break;
    case 5: store16(ea, (u16)(x < 0 ? 0 : x > 65535 ? 65535 : (s32)x)); break;
    case 6: store8(ea, (u8)(s8)(x < -128 ? -128 : x > 127 ? 127 : (s32)x)); break;
    case 7: store16(ea, (u16)(s16)(x < -32768 ? -32768 : x > 32767 ? 32767 : (s32)x)); break;
    }
}

void __psq_st(u32 ea, f64 ps0, f64 ps1, u32 w, u32 qr) {
    u32 gqr = g_gqr[qr & 7];
    u32 type = gqr & 7;
    u32 scale = (gqr >> 8) & 0x3F;
    quant_store(ea, (f32)ps0, type, scale);
    if (!w)
        quant_store(ea + qsize(type), (f32)ps1, type, scale);
}
