// Guest threads. The decompiled OS scheduler (SelectThread & co.) decides which thread runs;
// only its two context primitives are native:
//   OSSaveContext(ctx)  binds ctx to the running coroutine and returns 0
//   OSLoadContext(ctx)  switches to the coroutine bound to ctx; a context that was never saved
//                       is a new thread: a coroutine that calls ctx->srr0(ctx->gpr[3]) and then
//                       ctx->lr (OSExitThread) with the result.
// When a coroutine is resumed it returns from the OSLoadContext call it switched away in; the
// callers of SelectThread ignore its result, so this is the same as "OSSaveContext returned 1".
#ifndef __3DS__
#include <execinfo.h>
#endif

#include <algorithm>
#include <csetjmp>
#include <cstdlib>
#include <unordered_map>
#include <vector>

#include "../third-party/libco/libco.h"
#include "noma_decomp.h"
#include "runtime.h"

namespace guest {
void platform_backtrace(const char* why);

#if defined(__3DS__)
static const unsigned STACK_SIZE = 256 * 1024;
#else
static const unsigned STACK_SIZE = 1024 * 1024;
#endif

// OSContext layout (RVL SDK)
enum : u32 { CTX_GPR = 0x000, CTX_LR = 0x084, CTX_SRR0 = 0x198, CTX_SRR1 = 0x19C };

struct Coro {
    cothread_t co = nullptr;
    u32 ctx = 0;
    u32 sp = 0;
    u32 entry = 0, arg = 0, lr = 0;
    void* stack = nullptr;
    bool dead = false;
    bool exiting = false;  // in OSExitThread: may only idle here until it switches away
    struct Delivery {
        u32 ctx;  // the interrupted context, which the handler loads to return
        jmp_buf* jb;
    };
    std::vector<Delivery> deliveries;  // interrupt deliveries in progress on this thread
    std::vector<void*> parked;         // NOMA_DEBUG_THREADS: host stack where it switched away
#ifdef NOMA_PROFILE_CALLS
    ProfStack* prof = nullptr;  // its function stack, for the profiler
#endif
};

static std::unordered_map<u32, Coro*> s_by_ctx;
static Coro s_main;
static Coro* s_cur;

// A guest thread's host stack: libco keeps the saved registers at the bottom of the block and the
// stack grows down toward them, so an overflow would hand the thread garbage callee-saved
// registers at its next switch. A pattern above them is checked whenever a thread switches away
// (the main thread runs on the process's own stack). libco's save area is 104 bytes on ARM, 192
// on arm64 hosts.
static const u32 CANARY_AT = 512, CANARY_WORDS = 32, CANARY = 0xC0DEF00Du;
static void canary_set(Coro* c) {
    u32* w = (u32*)((u8*)c->stack + CANARY_AT);
    for (u32 i = 0; i < CANARY_WORDS; i++)
        w[i] = CANARY ^ i;
}
static void canary_check(Coro* c) {
    if (!c->stack)
        return;
    const u32* w = (const u32*)((const u8*)c->stack + CANARY_AT);
    for (u32 i = 0; i < CANARY_WORDS; i++)
        if (w[i] != (CANARY ^ i))
            fatal("guest thread %08X (entry %08X) overflowed its %u KB host stack", c->ctx, c->entry,
                  STACK_SIZE >> 10);
}

void threads_init() {
    s_main.co = co_active();
    s_cur = &s_main;
#ifdef NOMA_PROFILE_CALLS
    s_main.prof = g_prof;
#endif
}

// On hardware, OSLoadContext ends in rfi: an interrupt that is pending when the loaded context
// has MSR[EE] set is taken before that context runs a single instruction. (Interrupts raised
// while the previous thread had them off would otherwise wait for the next mtmsr, and land in
// the middle of whatever the resumed thread does first, e.g. right after it takes a mutex.)
static void take_pending_on_resume() {
    if ((g_msr & 0x8000u) && s_cur->deliveries.empty())
        check_interrupts();
}

static void coro_entry() {
    Coro* c = s_cur;
    g_sp = c->sp;
    take_pending_on_resume();
    u32 ret = gcall<u32>(c->entry, c->arg);
    c->exiting = true;
    gcall<u32>(c->lr, ret);  // OSExitThread(result): switches away for good
    fatal("guest thread at %08X came back from OSExitThread", c->entry);
}

// A coroutine can hold several contexts: its thread's, and IdleContext while the scheduler idles
// on it (interrupts taken in the idle loop are saved there).
u32 os_save_context(u32 ctx) {
    s_cur->ctx = ctx;
    s_by_ctx[ctx] = s_cur;
    store32(ctx + CTX_SRR1, g_msr);  // the MSR the context resumes with (interrupts on/off)
    return 0;
}

// A context was set up again (OSCreateThread on a finished thread's OSThread): the coroutine
// that used it is never resumed again.
void threads_forget_context(u32 ctx) {
    auto it = s_by_ctx.find(ctx);
    if (it == s_by_ctx.end() || it->second == s_cur)
        return;
    it->second->dead = true;  // its host stack is leaked; threads are rarely recreated
    s_by_ctx.erase(it);
}

jmp_buf* threads_push_delivery(u32 ctx) {
    jmp_buf* jb = new jmp_buf[1];
    s_cur->deliveries.push_back({ctx, jb});
    return jb;
}

void threads_pop_delivery() {
    delete[] s_cur->deliveries.back().jb;
    s_cur->deliveries.pop_back();
}

static const u32 CTX_STATE = 0x1A2;

void os_load_context(u32 ctx) {
    Coro* target;
    // loading a context resumes it: it no longer holds an exception's register state
    store16(ctx + CTX_STATE, load16(ctx + CTX_STATE) & ~2u);
    auto it = s_by_ctx.find(ctx);
    if (it != s_by_ctx.end()) {
        target = it->second;
        if (target->exiting && target != s_cur) {
            log("thread: loading context %08X resumes the exited thread %08X (entry %08X) from ctx %08X", ctx,
                target->ctx, target->entry, s_cur->ctx);
            platform_backtrace("thread: switching back to an exited thread");
        }
        if (target == s_cur) {
            // returning from an interrupt into the code it interrupted
            if (!s_cur->deliveries.empty() && s_cur->deliveries.back().ctx == ctx)
                longjmp(*s_cur->deliveries.back().jb, 1);
            // the scheduler picked the thread that was idling on this coroutine: carry on, with
            // the MSR it saved (on hardware OSLoadContext returns through rfi with SRR1)
            g_msr = load32(ctx + CTX_SRR1);
            take_pending_on_resume();
            return;
        }
    } else {
        target = new Coro();
        target->ctx = ctx;
        target->entry = load32(ctx + CTX_SRR0);
        target->sp = load32(ctx + CTX_GPR + 1 * 4);
        target->arg = load32(ctx + CTX_GPR + 3 * 4);
        target->lr = load32(ctx + CTX_LR);
        target->stack = malloc(STACK_SIZE);
        if (!target->stack)
            fatal("no memory for a guest thread stack");
        target->co = co_derive(target->stack, STACK_SIZE, coro_entry);
        canary_set(target);
#ifdef NOMA_PROFILE_CALLS
        target->prof = profile_new_stack(ctx);
#endif
        s_by_ctx[ctx] = target;
        log("thread: new guest thread %08X, entry %s (%08X), stack %08X", ctx, fn_name(target->entry),
            target->entry, target->sp);
    }
    s_cur->sp = g_sp;
    canary_check(s_cur);
#ifndef __3DS__
    static int s_debug = getenv("NOMA_DEBUG_THREADS") ? 1 : 0;
    if (s_debug) {
        void* frames[48];
        int n = backtrace(frames, 48);
        s_cur->parked.assign(frames, frames + n);
    }
#endif
    account_thread_time();
    s_cur = target;
    g_sp = target->sp;
    // a new thread starts with OSInitContext's MSR (interrupts on); a suspended one with the
    // MSR it saved
    g_msr = load32(ctx + CTX_SRR1);
#ifdef NOMA_PROFILE_CALLS
    g_prof = target->prof;
#endif
    co_switch(target->co);
    // resumed by another thread's OSLoadContext, which set s_cur, g_sp and g_msr for us
    take_pending_on_resume();
}

// NOMA_DEBUG_THREADS: where each guest thread last switched away (on ^C / timeout)
void threads_dump() {
#ifndef __3DS__
    std::vector<Coro*> all{&s_main};
    for (auto& kv : s_by_ctx)
        if (std::find(all.begin(), all.end(), kv.second) == all.end())
            all.push_back(kv.second);
    for (Coro* c : all) {
        log("thread ctx %08X%s, entry %08X:", c->ctx, c == s_cur ? " (running)" : "", c->entry);
        if (c->parked.empty())
            continue;
        char** syms = backtrace_symbols(c->parked.data(), (int)c->parked.size());
        for (size_t i = 0; i < c->parked.size(); i++)
            log("    %s", syms[i]);
        free(syms);
    }
#endif
}

}  // namespace guest

namespace g {
u32 OSSaveContext(u32 r3) { return guest::os_save_context(r3); }
void OSLoadContext(u32 r3) { guest::os_load_context(r3); }
}  // namespace g
