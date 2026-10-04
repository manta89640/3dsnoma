// Calls through guest code addresses (vtables, callbacks, thread entry points) and the
// fallbacks for functions that are stubbed or not decompiled yet.
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "functable.h"
#include "noma_decomp.h"
#include "runtime.h"

namespace guest {

static const NomaFn* find_fn(u32 a) {
    u32 lo = 0, hi = noma_functable_count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (noma_functable[mid].addr < a)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < noma_functable_count && noma_functable[lo].addr == a)
        return &noma_functable[lo];
    return nullptr;
}

const char* fn_name(u32 a) {
    static char buf[4][16];
    static int k;
    k = (k + 1) & 3;
    snprintf(buf[k], sizeof(buf[k]), "fn_%08X", a);
    return buf[k];
}

FnSlot g_fn_cache[4096];

// Native functions the guest calls through pointers (the virtuals of a scene written in native
// code, a callback): each gets a code address of its own past the program's, in a range that is
// neither the program's nor memory (MEM1 ends at 0x81800000, MEM2 starts at 0x90000000).
static const u32 HOST_FN_BASE = 0x8FF00000u, HOST_FN_MAX = 0x4000;
static std::vector<void*> s_host_fns;

u32 host_fn_address(void* fn) {
    for (size_t i = 0; i < s_host_fns.size(); i++)
        if (s_host_fns[i] == fn)
            return HOST_FN_BASE + 4 * (u32)i;
    if (s_host_fns.size() >= HOST_FN_MAX)
        fatal("more than %u native functions given guest addresses", HOST_FN_MAX);
    s_host_fns.push_back(fn);
    return HOST_FN_BASE + 4 * (u32)(s_host_fns.size() - 1);
}

void* lookup_fn_slow(u32 a) {
    const NomaFn* f = find_fn(a);
    void* fn = f ? f->fn : nullptr;
    if (!fn && a - HOST_FN_BASE < 4 * (u32)s_host_fns.size() && !(a & 3))
        fn = s_host_fns[(a - HOST_FN_BASE) >> 2];
    if (!fn)
        fatal("call through a pointer to 0x%08X, which is not a known function", a);
    FnSlot& s = g_fn_cache[(a >> 2) & 4095];
    s.fn = fn;
    s.tag = a | 1;
    return fn;
}

}  // namespace guest

void noma_unimplemented(const char* name) {
    guest::fatal("%s was called but is not decompiled yet", name);
}

void noma_stub(const char* name, const char* region) {
    static std::unordered_set<std::string> seen;
    if (seen.insert(name).second)
        guest::log("stub: %s (%s)", name, region);
}

// a trap instruction: on a retail console it ends in the OS's "unhandled exception" screen
void noma_trap(const char* where) { guest::fatal("trap instruction (debugger breakpoint) in %s", where); }

void noma_m2c_error(const char* where) {
    guest::fatal("decompiler error marker reached in %s", where);
}
