// The sampling profiler, in builds with -DNOMA_PROFILE_CALLS (NOMA_PROFILE=seconds turns it on).
// Every decompiled function keeps its guest thread's function stack up to date (NOMA_TRACE ->
// NOMA_PROF_SCOPE, guest.h); a sampler thread looks at the running guest thread's stack every
// millisecond. Every that many seconds the main thread logs where the time went: per guest
// thread, the functions it was in ("self") and the ones it was under ("total"). The runtime's
// own work shows up as "(...)" scopes: (idle), (devices), (gx), (ax), (present).
#include "runtime.h"

#ifdef NOMA_PROFILE_CALLS
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <map>
#include <unordered_map>
#include <vector>
#ifdef __3DS__
#include <3ds.h>
#else
#include <chrono>
#include <thread>
#endif

namespace guest {

static ProfStack s_main_stack;
ProfStack* volatile g_prof = &s_main_stack;
thread_local ProfStack* t_prof;
static ProfStack s_gpu_stack;  // the GPU's thread (runtime/hw/gx/fifo.cpp)
static std::atomic<bool> s_gpu_on{false};

namespace {

struct Counts {
    u32 samples = 0;
    std::unordered_map<const char*, u32> self, total;
    std::unordered_map<u32, u32> threads;
    std::map<std::pair<u32, const char*>, u32> thread_self;  // per guest thread
};
Counts s_counts;
Counts s_gpu_counts;  // the GPU's thread: self only, "(gpu idle)" while it waits for commands
std::atomic_flag s_lock = ATOMIC_FLAG_INIT;
int s_every;  // seconds between reports

void sample_gpu() {
    u32 depth = s_gpu_stack.depth;
    const char* name = depth == 0 ? "(gpu idle)" : s_gpu_stack.names[std::min(depth, PROF_DEPTH) - 1];
    s_gpu_counts.samples++;
    s_gpu_counts.self[name]++;
}

void sample() {
    ProfStack* s = g_prof;
    u32 depth = s->depth;
    if (depth > PROF_DEPTH)
        depth = PROF_DEPTH;
    const char* names[PROF_DEPTH];
    for (u32 i = 0; i < depth; i++)
        names[i] = s->names[i];
    // the main thread only holds the lock to take the counts: never wait for it (on the same
    // core, this thread preempts it)
    if (s_lock.test_and_set(std::memory_order_acquire))
        return;
    if (s_gpu_on.load(std::memory_order_relaxed))
        sample_gpu();
    s_counts.samples++;
    s_counts.threads[s->thread]++;
    if (depth == 0) {
        s_counts.self["(no guest code)"]++;
    } else {
        s_counts.self[names[depth - 1]]++;
        s_counts.thread_self[{s->thread, names[depth - 1]}]++;
        for (u32 i = 0; i < depth; i++) {
            bool seen = false;  // a recursive function counts once
            for (u32 j = 0; j < i && !seen; j++)
                seen = names[j] == names[i];
            if (!seen)
                s_counts.total[names[i]]++;
        }
    }
    s_lock.clear(std::memory_order_release);
}

#ifdef __3DS__
void sampler(void*) {
    for (;;) {
        svcSleepThread(1000000LL);
        sample();
    }
}
#else
void sampler() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        sample();
    }
}
#endif

void log_top(const char* what, const std::unordered_map<const char*, u32>& m, u32 samples, int n) {
    std::vector<std::pair<u32, const char*>> v;
    for (auto& kv : m)
        v.push_back({kv.second, kv.first});
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (int i = 0; i < n && i < (int)v.size(); i++)
        log("profile: %s %5.1f%%  %s", what, 100.0 * v[i].first / samples, v[i].second);
}

}  // namespace

// the GPU's thread profiles itself on a stack of its own
void profile_gpu_thread() {
    t_prof = &s_gpu_stack;
    s_gpu_on.store(true);
}

ProfStack* profile_new_stack(u32 thread) {
    ProfStack* s = new ProfStack;
    s->thread = thread;
    return s;
}

void profile_start() {
    s_every = getenv("NOMA_PROFILE") ? atoi(getenv("NOMA_PROFILE")) : 0;
    if (s_every <= 0)
        return;
#ifdef __3DS__
    // the system core when the app may use some of it (the game runs on core 0), else core 0 at
    // a priority above the game's
    u32 limit = 0;
    if (R_FAILED(APT_GetAppCpuTimeLimit(&limit)) || limit < 30)
        APT_SetAppCpuTimeLimit(30);
    if (!threadCreate(sampler, nullptr, 16 * 1024, 0x18, 1, true))
        threadCreate(sampler, nullptr, 16 * 1024, 0x18, 0, true);
#else
    std::thread(sampler).detach();
#endif
    log("profile: sampling every millisecond, report every %d s", s_every);
}

// from loop_poll, on the main thread
void profile_report_due() {
    if (s_every <= 0)
        return;
    static u64 s_next;
    u64 now = host_time_us();
    if (!s_next)
        s_next = now + (u64)s_every * 1000000u;
    if (now < s_next)
        return;
    s_next = now + (u64)s_every * 1000000u;
    while (s_lock.test_and_set(std::memory_order_acquire)) {
    }
    Counts c = std::move(s_counts);
    s_counts = Counts();
    Counts gpu = std::move(s_gpu_counts);
    s_gpu_counts = Counts();
    s_lock.clear(std::memory_order_release);
    if (!c.samples)
        return;
    std::vector<std::pair<u32, u32>> th;
    for (auto& kv : c.threads)
        th.push_back({kv.second, kv.first});
    std::sort(th.begin(), th.end(), [](auto& a, auto& b) { return a.first > b.first; });
    char line[512];
    int n = snprintf(line, sizeof(line), "profile: %u samples; guest threads:", c.samples);
    for (auto& t : th)
        if (n < (int)sizeof(line) - 32)
            n += snprintf(line + n, sizeof(line) - n, " %08X %.1f%%", t.second, 100.0 * t.first / c.samples);
    log("%s", line);
    log_top("self ", c.self, c.samples, 25);
    log_top("total", c.total, c.samples, 25);
    // the busiest guest threads: where each spends its time
    for (size_t t = 0; t < th.size() && t < 3; t++) {
        std::unordered_map<const char*, u32> m;
        for (auto& kv : c.thread_self)
            if (kv.first.first == th[t].second)
                m[kv.first.second] = kv.second;
        char what[24];
        snprintf(what, sizeof(what), "%08X", th[t].second);
        log_top(what, m, c.samples, 10);
    }
    if (gpu.samples)
        log_top("gpu  ", gpu.self, gpu.samples, 12);
}

}  // namespace guest
#endif
