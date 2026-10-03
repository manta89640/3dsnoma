// Simple devices: PI (interrupt controller), MI, DI, SI, EXI, AI, CP/PE status, Hollywood misc.
#include "gx/gx.h"
#include "hw.h"

namespace guest {

// ---------------------------------------------------------------------------- PI
static u32 s_pi_cause;
static u32 s_pi_mask;
static RegFile s_pi;

void pi_raise(u32 bits) { s_pi_cause |= bits; }
void pi_clear(u32 bits) { s_pi_cause &= ~bits; }
u32 pi_cause() { return s_pi_cause; }
u32 pi_mask() { return s_pi_mask; }
void pi_set_mask(u32 m) { s_pi_mask = m; }

u32 pi_read(u32 off, int size) {
    switch (off) {
    case 0x00:  // INTSR
        return s_pi_cause;
    case 0x04:  // INTMR
        return s_pi_mask;
    case 0x0C: case 0x10: case 0x14:  // CPU FIFO base, end, write pointer
        return gx_pi_fifo_read(off);
    case 0x2C:  // Flipper/Hollywood revision
        return 0x246500B1;
    default:
        return s_pi.read(off, size);
    }
}

void pi_write(u32 off, u32 v, int size) {
    switch (off) {
    case 0x00:  // INTSR: write 1 to acknowledge the reset switch interrupt
        s_pi_cause &= ~(v & (PI_INT_RSW));
        break;
    case 0x04:
        s_pi_mask = v;
        check_interrupts();
        break;
    case 0x0C: case 0x10: case 0x14:
        gx_pi_fifo_write(off, v, size);
        break;
    default:
        s_pi.write(off, v, size);
        break;
    }
}

// ---------------------------------------------------------------------------- MI
static RegFile s_mi;
u32 mi_read(u32 off, int size) { return s_mi.read(off, size); }
void mi_write(u32 off, u32 v, int size) { s_mi.write(off, v, size); }

// ---------------------------------------------------------------------------- DI (no disc)
static RegFile s_di;
u32 di_read(u32 off, int size) {
    if (off == 0x24)  // DICFG
        return 0;
    return s_di.read(off, size);
}
void di_write(u32 off, u32 v, int size) {
    mmio_log_unhandled("DI", off, v, size, true);
    s_di.write(off, v, size);
}

// ---------------------------------------------------------------------------- SI (no controllers)
// GameCube controller ports, none connected: every transfer ends at once with "no response".
//   0x34 SICOMCSR  31 TCINT (write 1 clears) 30 TCINTMSK 29 COMERR 28 RDSTINT (write 1 clears)
//                  27 RDSTINTMSK 1-2 channel 0 TSTART
//   0x38 SISR      per channel (ch0 in bits 24-31): 3 NOREP ... (error bits: write 1 clears)
static RegFile s_si;
static u32 s_si_comcsr, s_si_sr;
enum : u32 {
    SI_TCINT = 1u << 31,
    SI_TCINTMSK = 1u << 30,
    SI_COMERR = 1u << 29,
    SI_RDSTINT = 1u << 28,
    SI_RDSTINTMSK = 1u << 27,
};

static void si_update_irq() {
    bool on = ((s_si_comcsr & SI_TCINT) && (s_si_comcsr & SI_TCINTMSK)) ||
              ((s_si_comcsr & SI_RDSTINT) && (s_si_comcsr & SI_RDSTINTMSK));
    if (on)
        pi_raise(PI_INT_SI);
    else
        pi_clear(PI_INT_SI);
}

u32 si_read(u32 off, int size) {
    if (off == 0x34 && size == 4)
        return s_si_comcsr;
    if (off == 0x38 && size == 4)
        return s_si_sr;
    return s_si.read(off, size);
}

void si_write(u32 off, u32 v, int size) {
    if (off == 0x34 && size == 4) {
        u32 keep = s_si_comcsr & (SI_TCINT | SI_RDSTINT | SI_COMERR);
        if (v & SI_TCINT)
            keep &= ~SI_TCINT;
        if (v & SI_RDSTINT)
            keep &= ~SI_RDSTINT;
        s_si_comcsr = (v & ~(SI_TCINT | SI_RDSTINT | SI_COMERR | 1u)) | keep;
        if (v & 1) {  // TSTART: the addressed channel doesn't answer
            u32 ch = (v >> 1) & 3;
            s_si_sr |= 0x08u << (8 * (3 - ch));  // NOREP
            s_si_comcsr |= SI_COMERR | SI_TCINT;
        }
        si_update_irq();
        return;
    }
    if (off == 0x38 && size == 4) {
        s_si_sr &= ~(v & 0x0F0F0F0Fu);  // error bits: write 1 to clear
        return;
    }
    s_si.write(off, v, size);
}

// ---------------------------------------------------------------------------- AI
// The audio interface's streaming side: a sample counter that runs at 32 or 48 kHz while
// playing, and an interrupt when it reaches AIIT. (The DSP's output goes through the DSP/AX
// emulation; the SDK only uses these registers for timing.)
//   0x00 AICR  0 PSTAT (play) 1 AISFR (1: 48 kHz) 2 AIINTMSK 3 AIINT (write 1 clears)
//              4 AIINTVLD 5 SCRESET (write 1: counter to 0) 6 DSP sample rate
//   0x04 AIVR volume   0x08 AISCNT sample counter   0x0C AIIT interrupt at this count
static u32 s_ai_cr, s_ai_vr, s_ai_it;
static u32 s_ai_count0;    // counter value at s_ai_t0
static u64 s_ai_t0;        // host time (us) when the counter last started or was reset
static bool s_ai_int_due;  // AIIT not reached yet since it was set

static u32 ai_rate() { return (s_ai_cr & 2) ? 48000 : 32000; }

static u32 ai_count() {
    if (!(s_ai_cr & 1))
        return s_ai_count0;
    u64 dt = host_time_us() - s_ai_t0;
    return s_ai_count0 + (u32)(dt * ai_rate() / 1000000);
}

static void ai_update_irq() {
    if ((s_ai_cr & 0x8) && (s_ai_cr & 0x4))
        pi_raise(PI_INT_AI);
    else
        pi_clear(PI_INT_AI);
}

void ai_tick() {
    if (s_ai_int_due && (s_ai_cr & 1) && ai_count() >= s_ai_it) {
        s_ai_int_due = false;
        s_ai_cr |= 0x8;
        ai_update_irq();
    }
}

u32 ai_read(u32 off, int size) {
    switch (off & ~3u) {
    case 0x00: return s_ai_cr;
    case 0x04: return s_ai_vr;
    case 0x08: return ai_count();
    case 0x0C: return s_ai_it;
    default: return 0;
    }
}

void ai_write(u32 off, u32 v, int size) {
    switch (off & ~3u) {
    case 0x00: {
        u32 now_count = ai_count();
        bool was_playing = s_ai_cr & 1;
        u32 keep_int = s_ai_cr & 0x8 & ~(v & 0x8);  // AIINT: write 1 to clear
        s_ai_cr = (v & ~0x28u) | keep_int;
        if (v & 0x20)  // SCRESET
            now_count = 0;
        if ((s_ai_cr & 1) != was_playing || (v & 0x20) || true) {
            s_ai_count0 = now_count;
            s_ai_t0 = host_time_us();
        }
        ai_update_irq();
        break;
    }
    case 0x04: s_ai_vr = v; break;
    case 0x08: break;  // read only
    case 0x0C:
        s_ai_it = v;
        s_ai_int_due = true;
        break;
    }
}

// ---------------------------------------------------------------------------- Hollywood misc
static RegFile s_hw;
u32 ipc_reg_read(u32 off);
void ipc_reg_write(u32 off, u32 v);
u32 hollywood_read(u32 off, int size) {
    if (off < 0x10 || off == 0x30 || off == 0x34)
        return ipc_reg_read(off);
    switch (off) {
    case 0x214:  // HW_VERSION
        return 0x00000011;
    default:
        return s_hw.read(off, size);
    }
}
void hollywood_write(u32 off, u32 v, int size) {
    if (off < 0x10 || off == 0x30 || off == 0x34) {
        ipc_reg_write(off, v);
        return;
    }
    s_hw.write(off, v, size);
}

}  // namespace guest
