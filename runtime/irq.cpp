// Interrupts and the passage of time. The decompiled OS handles interrupts itself
// (__OSDispatchInterrupt, DecrementerExceptionCallback); this file decides when they happen:
//  * when the guest enables interrupts (mtmsr EE 0->1) and one is pending,
//  * while the scheduler idles (its idle loop runs on IdleContext with interrupts on),
//  * when device emulation polls (busy-wait loops on hardware registers).
// Delivery does what the exception vector does: mark the current context as saved by an
// exception, clear MSR[EE], call the handler. The handler ends in OSLoadContext(ctx), which
// longjmps back here (see threads.cpp).
#include <csetjmp>
#include <cstdint>
#include <cstdlib>

#include "hw/hw.h"
#include "noma_decomp.h"
#include "runtime.h"
#include "load_stats.h"

namespace guest {

// OS globals (OSThread.c / OSContext.c)
static const u32 OS_CURRENT_CONTEXT = 0x800000D4;
static const u32 IDLE_CONTEXT = 0x8088EF70;
static const u32 RUN_QUEUE_BITS = 0x80883228;
static const u32 CTX_STATE = 0x1A2;
static const u16 CTX_STATE_EXC = 0x2;

enum { EXC_DECREMENTER = 8, EXC_EXTERNAL = 4 };

u64 decrementer_deadline();
void decrementer_clear();
jmp_buf* threads_push_delivery(u32 ctx);
void threads_pop_delivery();

static int s_depth;

static void deliver(int kind) {
    u32 ctx = load32(OS_CURRENT_CONTEXT);
    // the exception entry saves the interrupted state into the current context
    os_save_context(ctx);
    store16(ctx + CTX_STATE, load16(ctx + CTX_STATE) | CTX_STATE_EXC);
    u32 msr = g_msr;
    u32 sp = g_sp;
    g_msr &= ~0x8000u;
    s_depth++;
    ProfMark prof;
    jmp_buf* jb = threads_push_delivery(ctx);
    if (setjmp(*jb) == 0) {
        if (kind == EXC_EXTERNAL)
            g::__OSDispatchInterrupt(EXC_EXTERNAL, ctx);
        else
            g::DecrementerExceptionCallback(EXC_DECREMENTER, ctx);
    }
    threads_pop_delivery();
    prof.unwind();  // the handler returned by longjmp, past its functions' scopes
    s_depth--;
    g_sp = sp;
    g_msr = msr;
}

void ai_tick();
void ios_async_poll();
bool ios_async_busy();
void ios_async_wait(u64 us);
void ipc_tick();
bool ipc_reply_waiting();

u64 g_polls;
u64 g_coarse_us;

void poll_devices() {
    NOMA_PROF_SCOPE("(devices)");
    g_polls++;
    u64 now = host_time_us();
    g_coarse_us = now;
    vi_tick(now);
    dsp_tick(now);
    ai_tick();
    ipc_tick();
    ios_async_poll();
    gx_poll();
}

static bool deliverable() {
    if (!(g_msr & 0x8000u))
        return false;
    if (pi_cause() & pi_mask())
        return true;
    u64 d = decrementer_deadline();
    return d && timebase() >= d;
}

static u32 s_irq_count[32];
static u64 s_irq_report_us;

void check_interrupts() {
    // called at every MSR[EE] 0->1 (often): look at the devices' clocks every 16th time; an
    // interrupt already pending is taken at once below
    static u32 s_calls;
    if ((++s_calls & 15) == 0)
        poll_devices();
    for (int guard = 0; guard < 64 && (g_msr & 0x8000u); guard++) {
        if (pi_cause() & pi_mask()) {
            u32 c = pi_cause() & pi_mask();
            for (int b = 0; b < 32; b++)
                if (c & (1u << b))
                    s_irq_count[b]++;
            if (getenv("NOMA_DEBUG_IRQ") && host_time_us() - s_irq_report_us > 2000000) {
                s_irq_report_us = host_time_us();
                log("irq: deliveries so far: VI %u DSP %u AI %u EXI %u SI %u IPC %u PE %u CP %u",
                    s_irq_count[8], s_irq_count[6], s_irq_count[5], s_irq_count[4], s_irq_count[3],
                    s_irq_count[14], s_irq_count[9] + s_irq_count[10], s_irq_count[11]);
            }
            deliver(EXC_EXTERNAL);
            continue;
        }
        u64 d = decrementer_deadline();
        if (d && timebase() >= d) {
            decrementer_clear();
            deliver(EXC_DECREMENTER);
            continue;
        }
        break;
    }
}

// The scheduler found no thread to run and enabled interrupts in its idle loop: wait for the
// next device event, deliver it, until some thread is ready.
void platform_backtrace(const char* why);

static u64 s_last_poll_us;  // see account_thread_time
static u32 s_idle_us;       // see idle_time_us

u32 idle_time_us() { return s_idle_us; }

void idle_wait() {
    NOMA_PROF_SCOPE("(idle)");
    u64 since = host_time_us();
    bool reported = false;
    account_thread_time();
    s_last_poll_us = 0;  // waiting is no thread's time
    struct IdleStats {  // the status line's idle time; NOMA_LOAD_STATS
        u64 since;
        ~IdleStats() {
            u64 us = host_time_us() - since;
            s_idle_us += (u32)us;
            if (g_lstats)
                lstats_idle(us);
        }
    } idle_stats{since};
    while (load32(RUN_QUEUE_BITS) == 0) {
        poll_devices();
        if (deliverable()) {
            check_interrupts();
            continue;
        }
        // NOMA_DEBUG_IDLE: when every thread has waited for 2 s, show what the thread that
        // went to sleep last (whose host stack the idle loop runs on) is waiting in
        if (!reported && host_time_us() - since > 2000000 && getenv("NOMA_DEBUG_IDLE")) {
            reported = true;
            platform_backtrace("all guest threads waiting for 2 s; the last one to sleep:");
        }
        if (ipc_reply_waiting())
            continue;  // posted at the next poll
        if (ios_async_busy())
            ios_async_wait(500);  // a read in flight: its end wakes us
        else
            host_sleep_us(500);
    }
}

// A decompiled loop ran LOOP_BUDGET iterations: let time pass for the devices and take
// pending interrupts, as the hardware would at any instruction while MSR[EE] is set.
static const s32 LOOP_BUDGET = 20000;
s32 g_loop_budget = LOOP_BUDGET;

extern u64 g_ee_off_since;
}  // namespace guest
void noma_trace_snapshot_dump(const char* why);
namespace guest {

// Time spent in guest threads above the default priority (16: the main thread's), against all
// time: dsp.cpp keeps their share bounded when the CPU is too slow for them. Charged at loop
// polls and thread switches to the thread that ran since the last charge; idle time is no
// thread's.
u64 g_hiprio_us;
static u32 s_acct_thread;
void account_thread_time() {
    u64 now = host_time_us();
    if (s_last_poll_us && s_acct_thread && (s32)load32(s_acct_thread + 0x2D0) < 16)
        g_hiprio_us += now - s_last_poll_us;
    if (g_lstats && s_last_poll_us && s_acct_thread)
        lstats_thread(s_acct_thread, now - s_last_poll_us);
    s_last_poll_us = now;
    s_acct_thread = load32(0x800000E4);  // OSGetCurrentThread (already the next one at a switch)
    static int s_debug = getenv("NOMA_DEBUG_PRIO") ? 1 : 0;
    static u64 s_report, s_report_hi;
    if (s_debug && now - s_report > 2000000) {
        if (s_report)
            log("prio: high-priority threads ran %llu of the last %llu ms", (unsigned long long)(g_hiprio_us - s_report_hi) / 1000,
                (unsigned long long)(now - s_report) / 1000);
        s_report = now, s_report_hi = g_hiprio_us;
    }
    static u32 s_seen[32];
    if (s_debug && s_acct_thread)
        for (u32& x : s_seen)
            if (x == s_acct_thread)
                break;
            else if (!x) {
                x = s_acct_thread;
                log("prio: thread %08X priority %d", x, (s32)load32(x + 0x2D0));
                break;
            }
}

void loop_poll() {
    g_loop_budget = LOOP_BUDGET;
    profile_report_due();
    account_thread_time();
    // NOMA_SLOWDOWN=us: spin that long at every poll -- guest code slower against the real-time
    // devices, as on the 3DS (reproduces its timing on a fast host)
    static int s_slow = getenv("NOMA_SLOWDOWN") ? atoi(getenv("NOMA_SLOWDOWN")) : 0;
    if (s_slow > 0) {
        u64 until = host_time_us() + (u64)s_slow;
        while (host_time_us() < until) {
        }
    }
    // a busy-wait with interrupts off can only end if the loop itself changes something
    static bool s_reported;
    if (!(g_msr & 0x8000u) && g_ee_off_since && !s_reported &&
        host_time_us() - g_ee_off_since > 2000000) {
        s_reported = true;
        log("guest busy-waits with interrupts off for 2 s (pending %08X)", pi_cause() & pi_mask());
        noma_trace_snapshot_dump("calls before interrupts were turned off:");
    }
    static bool s_debug = getenv("NOMA_DEBUG_POLL") != nullptr;
    static int s_n;
    if (s_debug && (s_n++ % 2000) == 0)
        log("loop_poll: msr %08X dec %llu tb %llu pi %08X/%08X", g_msr,
            (unsigned long long)decrementer_deadline(), (unsigned long long)timebase(), pi_cause(),
            pi_mask());
    // the devices at every loop poll: check_interrupts looks at them only every 16th time (it also
    // runs at each MSR[EE] 0->1), so a loop that never touches EE -- a thread busy-waiting for
    // another, a long computation -- took the retrace and the IOS replies only every 16 budgets
    // (about 40 ms on the 3DS), and the threads they wake, the main thread among them, that late
    poll_devices();
    check_interrupts();
}

// `for (;;);`: the SDK's way of stopping after a fatal error (it printed why with OSReport
// first). With interrupts off nothing can ever happen again; with them on, other threads and
// handlers still run, as on the console.
void spin_poll(const char* fn, u32 addr) {
    static u32 s_reported;
    if (!(g_msr & 0x8000u))
        fatal("guest stopped: endless loop with interrupts off in %s (%08X)", fn, addr);
    if (s_reported != addr) {
        s_reported = addr;
        log("guest spins forever in %s (%08X); interrupts still run", fn, addr);
    }
    loop_poll();
    host_sleep_us(1000);
}

void hw_init() {
    timebase_init();
    vi_init();
    ipc_init();
    exi_init();
    gpu_init();
}

bool in_idle_context() { return load32(OS_CURRENT_CONTEXT) == IDLE_CONTEXT; }

}  // namespace guest
