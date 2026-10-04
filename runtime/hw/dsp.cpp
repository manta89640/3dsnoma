// DSP interface (0xCC005000): mailboxes, the control/status register, ARAM and AI DMA.
// The DSP itself is high-level emulated: the 128-byte init program __OSInitAudioSystem runs,
// and (later) the AX microcode that mixes the voices.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

#include "hw.h"

namespace guest {

void platform_audio_push(const s16* lr, u32 frames);  // stereo, 32 kHz

static u32 s_cpu_mbox;  // CPU -> DSP (bit 31: not yet read by the DSP)
static u32 s_dsp_mbox;  // DSP -> CPU (bit 31: not yet read by the CPU)
static u16 s_dspcr = 0;
static u32 s_ar_mm, s_ar_ar, s_ar_cnt;
static u32 s_ai_start, s_ai_ctrl;
// the AI DMA in progress: it latched the start and length registers when it began
static bool s_dma_on;
static u32 s_dma_addr, s_dma_blocks;
static u64 s_dma_t0;
static std::vector<s16> s_dma_data;
u64 g_aid_raised;  // the block, read when the DMA took it (as L, R pairs)

static void latch_dma() {
    s_dma_addr = s_ai_start;
    s_dma_blocks = s_ai_ctrl & 0x7FFF;
    u32 frames = s_dma_blocks * 8;
    s_dma_data.resize(2 * frames);
    u32 a = 0x80000000u | (s_dma_addr & 0x1FFFFFFF);
    const u8* p = host_ptr(a);
    if (p && (a & (PAGE_SIZE - 1)) + 4 * frames <= PAGE_SIZE) {
        // the DSP writes big-endian R, L pairs: a pair's four bytes reversed are L, R
        for (u32 i = 0; i < frames; i++) {
            u32 v;
            memcpy(&v, p + 4 * i, 4);
            v = __builtin_bswap32(v);
            memcpy(&s_dma_data[2 * i], &v, 4);
        }
        return;
    }
    for (u32 i = 0; i < frames; i++) {  // the DSP writes R, L pairs
        s_dma_data[2 * i + 1] = (s16)load16(a + 4 * i);
        s_dma_data[2 * i] = (s16)load16(a + 4 * i + 2);
    }
}
static u16 s_regs[0x100];

// DSPCR bits
enum : u16 {
    CR_RES = 0x0001,
    CR_PIINT = 0x0002,
    CR_HALT = 0x0004,
    CR_AIDINT = 0x0008,
    CR_AIDINTMSK = 0x0010,
    CR_ARINT = 0x0020,
    CR_ARINTMSK = 0x0040,
    CR_DSPINT = 0x0080,
    CR_DSPINTMSK = 0x0100,
    CR_DSPDMA = 0x0200,
    CR_BUSY = 0x0400,
    CR_INIT = 0x0800,
};
static const u16 CR_ACK_BITS = CR_AIDINT | CR_ARINT | CR_DSPINT;

void dsp_hle_mail_from_cpu(u32 mail);  // dsp_hle.cpp: the microcode the game uploaded
void dsp_hle_reset(bool init_code);
void dsp_hle_run();

static void update_irq() {
    bool irq = ((s_dspcr & CR_AIDINT) && (s_dspcr & CR_AIDINTMSK)) ||
               ((s_dspcr & CR_ARINT) && (s_dspcr & CR_ARINTMSK)) ||
               ((s_dspcr & CR_DSPINT) && (s_dspcr & CR_DSPINTMSK));
    if (irq)
        pi_raise(PI_INT_DSP);
    else
        pi_clear(PI_INT_DSP);
}

// Mails the DSP sends after some work (AX's DSP_YIELD after a frame) arrive a little later, as
// on the console: the CPU sets up its state for the answer after sending the request.
struct LaterMail {
    u32 mail;
    u64 ready_us;
};
static std::deque<LaterMail> s_later;
void dsp_send_mail(u32 mail, bool interrupt);

void dsp_send_mail_later(u32 mail, u64 delay_us) { s_later.push_back({mail, host_time_us() + delay_us}); }

// true if a mail went to the CPU
static bool deliver_later_mail(u64 now) {
    if (s_later.empty() || (s_dsp_mbox & 0x80000000u) || now < s_later.front().ready_us)
        return false;
    u32 m = s_later.front().mail;
    s_later.pop_front();
    dsp_send_mail(m, true);
    return true;
}

// the DSP sends a mail to the CPU (and optionally interrupts it)
void dsp_send_mail(u32 mail, bool interrupt) {
    s_dsp_mbox = mail | 0x80000000u;
    static int s_n;
    if (s_n++ < 20)
        log("dsp: mail to cpu %08X%s (DSPCR %04X)", mail, interrupt ? " + interrupt" : "", s_dspcr);
    if (interrupt) {
        s_dspcr |= CR_DSPINT;
        update_irq();
    }
}

bool dsp_mail_pending_to_cpu() { return (s_dsp_mbox & 0x80000000u) != 0; }
bool dsp_halted() { return (s_dspcr & CR_HALT) != 0; }

u32 dsp_read(u32 off, int size) {
    switch (off) {
    case 0x00:
        return s_cpu_mbox >> 16;
    case 0x02:
        return s_cpu_mbox & 0xFFFF;
    case 0x04:
        if (!dsp_halted())
            dsp_hle_run();
        return s_dsp_mbox >> 16;
    case 0x06: {
        u32 lo = s_dsp_mbox & 0xFFFF;
        s_dsp_mbox &= ~0x80000000u;  // reading the low half takes the mail
        return lo;
    }
    case 0x0A:
        return s_dspcr;
    case 0x20: return s_ar_mm >> 16;
    case 0x22: return s_ar_mm & 0xFFFF;
    case 0x24: return s_ar_ar >> 16;
    case 0x26: return s_ar_ar & 0xFFFF;
    case 0x28: return s_ar_cnt >> 16;
    case 0x2A: return 0;  // DMA finished
    case 0x30: return s_ai_start >> 16;
    case 0x32: return s_ai_start & 0xFFFF;
    case 0x36: return s_ai_ctrl;
    case 0x3A: {  // AI DMA blocks left
        if (!s_dma_on || !s_dma_blocks)
            return 0;
        u64 done = (host_time_us() - s_dma_t0) * 32000 / 1000000 * 4 / 32;
        return done >= s_dma_blocks ? 0 : (u32)(s_dma_blocks - done);
    }
    default:
        if (size == 4)
            return (u32)s_regs[(off & 0x1FF) / 2] << 16 | s_regs[((off & 0x1FF) / 2 + 1) & 0xFF];
        return s_regs[(off & 0x1FF) / 2];
    }
}

static void write16(u32 off, u16 v) {
    switch (off) {
    case 0x00:
        s_cpu_mbox = (s_cpu_mbox & 0xFFFF) | ((u32)v << 16);
        break;
    case 0x02:
        // writing the low half sends the whole 32-bit mail (bit 31 is data: commands have
        // it set, values like an IRAM address of 0 don't); the DSP takes it at once
        s_cpu_mbox = (s_cpu_mbox & 0xFFFF0000u) | v;
        dsp_hle_mail_from_cpu(s_cpu_mbox);
        s_cpu_mbox &= ~0x80000000u;
        break;
    case 0x0A: {
        u16 old = s_dspcr;
        u16 acks = v & CR_ACK_BITS;
        if ((acks & CR_AIDINT) && (s_dspcr & CR_AIDINT) && getenv("NOMA_DEBUG_AX")) {
            extern u64 g_aid_raised;
            static u64 s_sum, s_n, s_max;
            u64 lat = host_time_us() - g_aid_raised;
            s_sum += lat, s_n++, s_max = std::max(s_max, lat);
            extern u64 g_polls;
            if (s_n % 500 == 0)
                log("ai: interrupt taken after %llu us on average (max %llu), %llu device polls so far",
                    (unsigned long long)(s_sum / s_n), (unsigned long long)s_max, (unsigned long long)g_polls);
        }
        s_dspcr = (u16)((v & ~CR_ACK_BITS) | (s_dspcr & CR_ACK_BITS & ~acks));
        // A reset restarts the DSP in its ROM (which then takes a task from the CPU); clearing
        // the init bit (0x800, 1 -> 0) instead loads the 128-byte init program the CPU put in
        // memory (__OSInitAudioSystem), as Dolphin documents.
        if (v & CR_RES) {
            s_dspcr &= ~(CR_RES | CR_BUSY);
            dsp_hle_reset(false);
        } else if ((old & CR_INIT) && !(s_dspcr & CR_INIT)) {
            dsp_hle_reset(true);
        }
        if ((old & CR_HALT) && !(s_dspcr & CR_HALT))
            dsp_hle_run();  // the DSP starts (or continues) running its program
        s_dspcr &= ~CR_BUSY;
        update_irq();
        break;
    }
    case 0x20: s_ar_mm = (s_ar_mm & 0xFFFF) | ((u32)v << 16); break;
    case 0x22: s_ar_mm = (s_ar_mm & 0xFFFF0000u) | v; break;
    case 0x24: s_ar_ar = (s_ar_ar & 0xFFFF) | ((u32)v << 16); break;
    case 0x26: s_ar_ar = (s_ar_ar & 0xFFFF0000u) | v; break;
    case 0x28: s_ar_cnt = (s_ar_cnt & 0xFFFF) | ((u32)v << 16); break;
    case 0x2A:
        // ARAM DMA (the Wii has no ARAM; the init code uses it to load the DSP's IRAM)
        s_ar_cnt = (s_ar_cnt & 0xFFFF0000u) | v;
        s_dspcr |= CR_ARINT;
        update_irq();
        break;
    case 0x30: s_ai_start = (s_ai_start & 0xFFFF) | ((u32)v << 16); break;
    case 0x32: s_ai_start = (s_ai_start & 0xFFFF0000u) | v; break;
    case 0x36: {
        bool was = (s_ai_ctrl & 0x8000) != 0;
        s_ai_ctrl = v;
        if ((v & 0x8000) && !was && (v & 0x7FFF)) {
            // the DMA starts: it takes the registers, and the interrupt lets the CPU queue the next
            s_dma_on = true;
            latch_dma();
            s_dma_t0 = host_time_us();
            s_dspcr |= CR_AIDINT;
            update_irq();
        } else if (!(v & 0x8000)) {
            s_dma_on = false;
        }
        break;
    }
    default:
        s_regs[(off & 0x1FF) / 2] = v;
        break;
    }
}

// AI DMA: the block is played at 32 kHz (16-bit stereo, 32 bytes a block); when it is done the
// DMA takes the registers again (the CPU has set up the next buffer by then) and interrupts.
// A block only ends once the CPU took the previous interrupt: this CPU notices interrupts only
// at certain points, and blocks ending together would merge their interrupts, so the AX would
// skip frames (and the same buffer would play again). The audio clock waits for the CPU instead.
//
// Each block (AX frame) wakes the sound thread, which runs above every other thread (priority 4).
// On a CPU too slow to finish a frame's sound work within the frame (the 3DS), it would never
// sleep and nothing else would run (the channel hung while formatting its cache file). So the
// threads above the main thread's priority get at most half the time since the last block, and
// the audio plays late instead (at least 5 blocks a second).
//
// The AX frames make the sound (engine/audio/ax_render.cpp mixes them) and move the sequences on
// (notes, envelopes) one frame at a time, so they have to come at the audio's own rate: frames the
// CPU took late are made up for, up to CATCH_UP_US behind, instead of being skipped (which slowed
// the music down and left gaps in the 3DS's audio stream). While it catches up the guard waits.
static const s64 CATCH_UP_US = 100000;
void dsp_ax_poll();
void dsp_ax_wait();

void dsp_tick(u64 now) {
    dsp_ax_poll();
    deliver_later_mail(now);
    if (s_dma_on && !(s_dspcr & CR_AIDINT)) {
        s64 dur = (s64)s_dma_blocks * 8 * 1000000 / 32000;  // 8 sample pairs a block
        if ((s64)(now - s_dma_t0) < dur)
            return;
        // The frame being mixed is in what plays next, and the CPU's next frame reads its PBs. AX
        // makes no frame for a block whose interrupt comes while the DSP is busy: the frame's yield
        // goes to the CPU first, and the block's interrupt follows at the next tick.
        dsp_ax_wait();
        if (deliver_later_mail(now) || !s_later.empty())
            return;
        extern u64 g_hiprio_us;
        static u64 s_prev_block, s_prev_hiprio;
        u64 since = now - s_prev_block, hi = g_hiprio_us - s_prev_hiprio;
        static bool s_guard = !getenv("NOMA_AUDIO_GUARD") || atoi(getenv("NOMA_AUDIO_GUARD")) != 0;
        bool behind = (s64)(now - s_dma_t0) > 2 * dur;
        if (s_guard && !behind && s_prev_block && hi * 2 > since && since < 200000)
            return;
        s_prev_block = now, s_prev_hiprio = g_hiprio_us;
        platform_audio_push(s_dma_data.data(), s_dma_blocks * 8);
        static u32 s_blocks;
        extern u32 g_ax_lists;
        if (++s_blocks % 1000 == 0 && getenv("NOMA_DEBUG_AX"))
            log("ai: %u DMA blocks played (%u bytes each, from %08X), %u AX command lists", s_blocks,
                s_dma_blocks * 32, s_dma_addr, g_ax_lists);
        s_dma_t0 += dur;
        // far behind (the CPU was late): continue from now
        if ((s64)(now - s_dma_t0) > CATCH_UP_US)
            s_dma_t0 = now;
        if (!(s_ai_ctrl & 0x8000) || !(s_ai_ctrl & 0x7FFF))
            s_dma_on = false;
        else
            latch_dma();
        s_dspcr |= CR_AIDINT;
        g_aid_raised = now;
        update_irq();
    }
}

void dsp_write(u32 off, u32 v, int size) {
    if (size == 4) {
        write16(off, (u16)(v >> 16));
        write16(off + 2, (u16)v);
    } else {
        write16(off, (u16)v);
    }
}

}  // namespace guest
