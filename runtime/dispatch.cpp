// Calls through guest code addresses (vtables, callbacks, thread entry points) and the
// fallbacks for functions that are stubbed or not decompiled yet.
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

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

void* lookup_fn_slow(u32 a) {
    const NomaFn* f = find_fn(a);
    if (!f)
        fatal("call through a pointer to 0x%08X, which is not a known function", a);
    FnSlot& s = g_fn_cache[(a >> 2) & 4095];
    s.fn = f->fn;
    s.tag = a | 1;
    return f->fn;
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
