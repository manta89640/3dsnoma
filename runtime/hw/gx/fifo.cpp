// The write-gather pipe and the FIFOs between the CPU and the GPU.
//
// Writes to 0xCC008000 collect in a 32-byte buffer; each full burst goes to main memory at the
// PI FIFO write pointer (PI 0x14), which then advances -- or, after writing the block at the
// FIFO end (PI 0x10), wraps to the base (PI 0x0C) and sets the wrap flag (bit 29). That is all the CPU side does -- it is also how the SDK records display
// lists (it points the CPU FIFO at the list's buffer and reads the write pointer afterwards).
//
// The GPU reads its own FIFO (CP 0x20-0x3E: base, end, watermarks, read and write pointers).
// In linked mode (CP control bit 4, the usual setup) it is the same buffer and its write
// pointer follows the CPU's; with reads enabled (bit 0) the commands up to the write pointer
// are executed (cp.cpp), as soon as each burst lands: we have no reason to let the GPU lag.
//
// CP registers are 16 bits wide at 0xCC000000; the PE (pixel engine) ones at 0xCC001000.
//
// Where the platform has a core to spare, the GPU has a thread of its own, as it runs beside the
// CPU on the Wii: it decodes the command stream (cp.cpp) and draws, while the CPU's thread runs
// the guest. Everything the guest reads stays on the CPU's thread (the FIFO pointers and the CP
// registers here, the PE's token and interrupt status); the bytes to execute go to the GPU's
// thread through a ring, and the PE's events come back as flags that the device poll applies
// (gx_poll), interrupts being the CPU thread's business. Elsewhere the decoder runs inline.
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../../runtime.h"
#include "gx.h"

namespace guest {

GxState g_gx;

// ---- CPU side
static u8 s_gather[32];
static u32 s_gather_n;
static u32 s_pi_base, s_pi_end, s_pi_wptr;  // physical; wptr bit 29 = wrapped

// ---- GPU side
static u16 s_cp_cr;  // 0 read enable, 1 breakpoint enable, 2/3 overflow/underflow irq, 4 link
static u32 s_cp_base, s_cp_end, s_cp_hiwm, s_cp_lowm, s_cp_wptr, s_cp_rptr, s_cp_bp;
static bool s_running;

// ---- PE
static u16 s_pe_isr;  // 0 token irq enable, 1 finish irq enable, 2 token irq, 3 finish irq
static RegFile s_pe;
// from the decoder: finish/token events (applied by pe_apply on the CPU's thread), the last token
enum : u32 { PE_EV_FINISH = 1, PE_EV_TOKEN = 2 };
static std::atomic<u32> s_pe_events{0};
static std::atomic<u32> s_pe_token{0};

// ---- the GPU's thread: the ring of stream bytes from the CPU's thread (head: published, s_put:
// written; tail: taken by the GPU's thread)
static bool s_gpu_thread;
static const u32 RING_SIZE = 4u << 20;
static const u32 PUBLISH_EVERY = 16u << 10;
static u8* s_ring;
static std::atomic<u32> s_ring_head{0}, s_ring_tail{0};
static u32 s_put;
static std::atomic<bool> s_gpu_busy{false};  // decoding what it took
static std::atomic<bool> s_reset{false};     // drop the undecoded tail before the next bytes
static std::atomic<bool> s_cpu_waiting{false};
static HostSignal *s_gpu_wake, *s_cpu_wake;
static void ring_put(const u8* p, u32 n);
static void ring_put_guest(u32 addr, u32 n);
void gx_sync();

static bool linked() { return s_cp_cr & 0x10; }

void gx_fifo_init() {
    memset(&g_gx, 0, sizeof(g_gx));
    s_gather_n = 0;
}

static void store_burst() {
    u32 a = s_pi_wptr & 0x1FFFFFE0;
    // the GPU executes the FIFO as it fills (nothing older waiting): the burst goes straight to
    // its thread (nothing reads it back from memory)
    bool direct = s_gpu_thread && linked() && (s_cp_cr & 1) && s_cp_rptr == s_cp_wptr;
    if (direct)
        ring_put(s_gather, 32);
    else
        write_g(0x80000000 | a, s_gather, 32);
    u32 wrap = s_pi_wptr & 0x20000000;
    if (a >= s_pi_end) {
        a = s_pi_base;
        wrap = 0x20000000;
    } else {
        a += 32;
    }
    s_pi_wptr = a | wrap;
    s_gather_n = 0;
    if (linked()) {
        s_cp_wptr = a;
        if (direct)
            s_cp_rptr = a;
        else
            gx_run();
    }
}

// GXResetWriteGatherPipe (after GXSetCPUFifo, as in GXBeginDisplayList): what the pipe held for
// the old FIFO is not written to the new one. A display list that fills its buffer exactly would
// otherwise wrap, and GXEndDisplayList returns 0 (RFL's Mii face mask, 864 of 896 bytes)
void gx_gather_reset() { s_gather_n = 0; }

void gx_fifo_write(u32 off, u32 value, int size) {
    for (int i = size - 1; i >= 0; i--) {
        s_gather[s_gather_n++] = (u8)(value >> (i * 8));
        if (s_gather_n == 32)
            store_burst();
    }
}

void gx_pi_fifo_write(u32 off, u32 v, int size) {
    switch (off) {
    case 0x0C: s_pi_base = v & 0x1FFFFFE0; break;
    case 0x10: s_pi_end = v & 0x1FFFFFE0; break;
    case 0x14:  // (a partly filled gather buffer stays: its bytes go to the new pointer)
        s_pi_wptr = v & 0x1FFFFFE0;
        break;
    }
}

u32 gx_pi_fifo_read(u32 off) {
    switch (off) {
    case 0x0C: return s_pi_base;
    case 0x10: return s_pi_end;
    case 0x14: return s_pi_wptr;
    default: return 0;
    }
}

// ---- execution
static std::vector<u8> s_pending;  // undecoded tail of the stream (a command split by a burst)

static u32 cp_distance() {
    if (s_cp_wptr >= s_cp_rptr)
        return s_cp_wptr - s_cp_rptr;
    return (s_cp_end + 32 - s_cp_rptr) + (s_cp_wptr - s_cp_base);
}

// decode the complete commands s_pending holds (on the decoder's thread)
static void decode_pending() {
    u32 done = 0;
    while (done < s_pending.size()) {
        u32 used = gx_decode(s_pending.data() + done, (u32)(s_pending.size() - done), false);
        if (!used)
            break;
        done += used;
    }
    s_pending.erase(s_pending.begin(), s_pending.begin() + done);
}

void gx_run() {
    NOMA_PROF_SCOPE("(gx)");
    if (s_running || !(s_cp_cr & 1))
        return;
    s_running = true;
    // take the blocks between the read and write pointers; the block at the end is the last
    // before the read pointer wraps to the base
    for (int guard = 0; s_cp_rptr != s_cp_wptr && guard < 4; guard++) {
        u32 r = s_cp_rptr;
        bool wraps = s_cp_wptr < r;
        u32 stop = wraps ? s_cp_end + 32 : s_cp_wptr;
        if (stop <= r || stop - r > 0x1000000) {
            log("gx: inconsistent GP FIFO (base %08X end %08X read %08X write %08X)", s_cp_base,
                s_cp_end, s_cp_rptr, s_cp_wptr);
            s_cp_rptr = s_cp_wptr;
            break;
        }
        if (s_gpu_thread) {
            ring_put_guest(0x80000000 | r, stop - r);
        } else {
            size_t at = s_pending.size();
            s_pending.resize(at + (stop - r));
            read_g(s_pending.data() + at, 0x80000000 | r, stop - r);
        }
        s_cp_rptr = wraps ? s_cp_base : stop;
    }
    if (!s_gpu_thread)
        decode_pending();
    s_running = false;
}

// ---- the ring to the GPU's thread
static void ring_publish() {
    if (s_put == s_ring_head.load(std::memory_order_relaxed))
        return;
    s_ring_head.store(s_put, std::memory_order_release);
    platform_signal(s_gpu_wake);
}

// room for n more bytes: wait while the GPU's thread is a whole ring behind
static u32 ring_room(u32 n) {
    for (;;) {
        u32 room = RING_SIZE - (s_put - s_ring_tail.load(std::memory_order_acquire));
        if (room)
            return std::min(room, n);
        NOMA_PROF_SCOPE("(gx ring full)");
        ring_publish();
        s_cpu_waiting.store(true, std::memory_order_seq_cst);
        if (RING_SIZE - (s_put - s_ring_tail.load(std::memory_order_seq_cst)) == 0)
            platform_wait(s_cpu_wake, 2000);
        s_cpu_waiting.store(false, std::memory_order_relaxed);
    }
}

static void ring_put(const u8* p, u32 n) {
    while (n) {
        u32 at = s_put & (RING_SIZE - 1);
        u32 k = std::min(ring_room(n), RING_SIZE - at);
        memcpy(s_ring + at, p, k);
        s_put += k, p += k, n -= k;
        if (s_put - s_ring_head.load(std::memory_order_relaxed) >= PUBLISH_EVERY)
            ring_publish();
    }
}

static void ring_put_guest(u32 addr, u32 n) {
    while (n) {
        u32 at = s_put & (RING_SIZE - 1);
        u32 k = std::min(ring_room(n), RING_SIZE - at);
        read_g(s_ring + at, addr, k);
        s_put += k, addr += k, n -= k;
        if (s_put - s_ring_head.load(std::memory_order_relaxed) >= PUBLISH_EVERY)
            ring_publish();
    }
}

static void gpu_thread() {
    profile_gpu_thread();
    for (;;) {
        u32 tail = s_ring_tail.load(std::memory_order_relaxed);
        u32 head = s_ring_head.load(std::memory_order_acquire);
        if (head == tail) {
            s_gpu_busy.store(false, std::memory_order_release);
            if (s_cpu_waiting.load(std::memory_order_seq_cst))
                platform_signal(s_cpu_wake);
            platform_wait(s_gpu_wake, 4000);
            continue;
        }
        s_gpu_busy.store(true, std::memory_order_relaxed);
        if (s_reset.exchange(false, std::memory_order_acquire))
            s_pending.clear();
        size_t at = s_pending.size();
        u32 n = head - tail, from = tail & (RING_SIZE - 1);
        s_pending.resize(at + n);
        u32 first = std::min(n, RING_SIZE - from);
        memcpy(s_pending.data() + at, s_ring + from, first);
        memcpy(s_pending.data() + at + first, s_ring, n - first);
        s_ring_tail.store(head, std::memory_order_release);
        if (s_cpu_waiting.load(std::memory_order_seq_cst))
            platform_signal(s_cpu_wake);
        decode_pending();
    }
}

// let the GPU's thread finish what it was given (a reset of the FIFO, which on hardware happens
// once the GPU is idle)
void gx_sync() {
    if (!s_gpu_thread)
        return;
    ring_publish();
    while (s_ring_tail.load(std::memory_order_acquire) != s_put || s_gpu_busy.load(std::memory_order_acquire)) {
        NOMA_PROF_SCOPE("(gx sync)");
        host_sleep_us(100);
    }
}

// the PE's events from the decoder, as the CPU's thread sees them
static void pe_update_irq();
static void pe_apply() {
    u32 e = s_pe_events.exchange(0, std::memory_order_acq_rel);
    if (!e)
        return;
    if (e & PE_EV_FINISH)
        s_pe_isr |= 0x8;
    if (e & PE_EV_TOKEN)
        s_pe_isr |= 0x4;
    pe_update_irq();
}

// from the device polls (the CPU's thread): what the CPU wrote goes to the GPU, what the GPU
// finished comes back
void gx_poll() {
    if (!s_gpu_thread)
        return;
    ring_publish();
    pe_apply();
}

// ---- CP registers
u32 cp_read(u32 off, int size) {
    if (size == 4)
        return cp_read(off, 2) << 16 | cp_read(off + 2, 2);
    gx_run();
    gx_poll();
    switch (off) {
    case 0x00: {  // status: 0 overflow 1 underflow (below the low watermark) 2 read idle 3 command idle
        u32 dist = cp_distance();
        return 0x0C | (dist < s_cp_lowm ? 2 : 0) | (s_cp_hiwm && dist > s_cp_hiwm ? 1 : 0);
    }
    case 0x02: return s_cp_cr;
    case 0x20: return s_cp_base & 0xFFFF;
    case 0x22: return s_cp_base >> 16;
    case 0x24: return s_cp_end & 0xFFFF;
    case 0x26: return s_cp_end >> 16;
    case 0x28: return s_cp_hiwm & 0xFFFF;
    case 0x2A: return s_cp_hiwm >> 16;
    case 0x2C: return s_cp_lowm & 0xFFFF;
    case 0x2E: return s_cp_lowm >> 16;
    case 0x30: return cp_distance() & 0xFFFF;
    case 0x32: return cp_distance() >> 16;
    case 0x34: return s_cp_wptr & 0xFFFF;
    case 0x36: return s_cp_wptr >> 16;
    case 0x38: return s_cp_rptr & 0xFFFF;
    case 0x3A: return s_cp_rptr >> 16;
    case 0x3C: return s_cp_bp & 0xFFFF;
    case 0x3E: return s_cp_bp >> 16;
    default: return 0;
    }
}

static void set_lo(u32& r, u32 v) { r = (r & 0xFFFF0000) | (v & 0xFFFF); }
static void set_hi(u32& r, u32 v) { r = (r & 0xFFFF) | ((v & 0xFFFF) << 16); }

void cp_write(u32 off, u32 v, int size) {
    if (size == 4) {  // a 32-bit write sets two 16-bit registers
        cp_write(off, v >> 16, 2);
        cp_write(off + 2, v & 0xFFFF, 2);
        return;
    }
    switch (off) {
    case 0x00: break;
    case 0x02:
        s_cp_cr = (u16)v;
        gx_run();
        break;
    case 0x04: break;  // clear overflow/underflow
    case 0x20: set_lo(s_cp_base, v & 0xFFE0); break;
    case 0x22: set_hi(s_cp_base, v & 0x1FFF); break;
    case 0x24: set_lo(s_cp_end, v & 0xFFE0); break;
    case 0x26: set_hi(s_cp_end, v & 0x1FFF); break;
    case 0x28: set_lo(s_cp_hiwm, v); break;
    case 0x2A: set_hi(s_cp_hiwm, v); break;
    case 0x2C: set_lo(s_cp_lowm, v); break;
    case 0x2E: set_hi(s_cp_lowm, v); break;
    case 0x30: case 0x32: break;  // read/write distance: follows from the pointers
    case 0x34: set_lo(s_cp_wptr, v & 0xFFE0); break;
    case 0x36: set_hi(s_cp_wptr, v & 0x1FFF); break;
    case 0x38:
    case 0x3A:
        (off == 0x38 ? set_lo(s_cp_rptr, v & 0xFFE0) : set_hi(s_cp_rptr, v & 0x1FFF));
        // the FIFO starts over: what was split at its end is dropped
        if (s_gpu_thread) {
            gx_sync();
            s_reset.store(true, std::memory_order_release);
        } else {
            s_pending.clear();
        }
        break;
    case 0x3C: set_lo(s_cp_bp, v); break;
    case 0x3E: set_hi(s_cp_bp, v); break;
    default: break;
    }
}

// ---- PE registers and events
static void pe_update_irq() {
    if ((s_pe_isr & 0x4) && (s_pe_isr & 0x1))
        pi_raise(PI_INT_PE_TOKEN);
    else
        pi_clear(PI_INT_PE_TOKEN);
    if ((s_pe_isr & 0x8) && (s_pe_isr & 0x2))
        pi_raise(PI_INT_PE_FINISH);
    else
        pi_clear(PI_INT_PE_FINISH);
}

// (on the decoder's thread)
void pe_finish() {
    s_pe_events.fetch_or(PE_EV_FINISH, std::memory_order_release);
    if (!s_gpu_thread)
        pe_apply();
}

void pe_token(u16 value, bool interrupt) {
    s_pe_token.store(value, std::memory_order_release);
    if (interrupt)
        s_pe_events.fetch_or(PE_EV_TOKEN, std::memory_order_release);
    if (!s_gpu_thread)
        pe_apply();
}

u32 pe_read(u32 off, int size) {
    gx_run();
    gx_poll();
    switch (off) {
    case 0x0A: return s_pe_isr;
    case 0x0E: return s_pe_token.load(std::memory_order_acquire);
    default: return s_pe.read(off, size);
    }
}

void pe_write(u32 off, u32 v, int size) {
    if (off == 0x0A) {
        // enables in bits 0-1; writing 1 to the status bits 2-3 clears them
        s_pe_isr = (u16)((v & 0x3) | (s_pe_isr & 0xC & ~(v & 0xC)));
        pe_update_irq();
        return;
    }
    s_pe.write(off, v, size);
}

void gpu_init() {
    gx_fifo_init();
    // NOMA_DEBUG_GX_NAN reports the guest code writing a NaN: the decoder runs inline for it
    if (getenv("NOMA_DEBUG_GX_NAN") || s_gpu_thread)
        return;
    s_ring = (u8*)malloc(RING_SIZE);
    s_gpu_wake = platform_signal_new(), s_cpu_wake = platform_signal_new();
    s_gpu_thread = s_ring && platform_gpu_thread_start(gpu_thread);
    if (s_gpu_thread)
        log("gx: commands decoded and drawn on the GPU's own thread");
}

}  // namespace guest
