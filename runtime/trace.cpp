// Function-entry tracing for debugging the decompilation (build with -DNOMA_TRACE_CALLS).
// NOMA_TRACE=all logs every call; NOMA_TRACE=OS,NAND logs calls whose name contains a word;
// NOMA_TRACE_LAST=N keeps the last N calls and prints them when the program stops;
// NOMA_TRACE_BT=fn_80012345,80023456 prints the host stack (the guest's call chain: decompiled
// functions are host functions) whenever one of those functions is entered.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "runtime.h"

#ifdef NOMA_TRACE_CALLS
namespace guest {
u32 g_watch_addr = getenv("NOMA_WATCH") ? (u32)strtoul(getenv("NOMA_WATCH"), nullptr, 16) & ~3u : 1u;
void platform_backtrace(const char* why);
}  // namespace guest
void noma_trace_dump();
namespace guest {
void noma_watch_hit(u32 a, u32 v, int size) {
    char why[96];
    snprintf(why, sizeof(why), "watch: store%d %08X = %08X", size * 8, a, v);
    platform_backtrace(why);
    static u32 only = getenv("NOMA_WATCH_VALUE") ? (u32)strtoul(getenv("NOMA_WATCH_VALUE"), nullptr, 16) : 0;
    if (!only || v == only)
        noma_trace_dump();
}
}  // namespace guest

namespace {
std::vector<std::string> s_words;
std::vector<u32> s_bt_addrs;
bool s_all;
bool s_init;
std::vector<const char*> s_ring;
size_t s_pos;
}  // namespace

// a copy of the ring taken at some moment (noma_trace_snapshot), printed later
static std::vector<const char*> s_snap;
void noma_trace_snapshot() {
    s_snap.clear();
    for (size_t i = 0; i < s_ring.size(); i++)
        s_snap.push_back(s_ring[(s_pos + i) % s_ring.size()]);
}
void noma_trace_snapshot_dump(const char* why) {
    guest::log("%s", why);
    for (const char* n : s_snap)
        if (n)
            guest::log("  snap: %s", n);
}

void noma_trace_dump();
namespace guest {
void noma_trace_dump_global() { ::noma_trace_dump(); }
}
void noma_trace_dump() {
    for (size_t i = 0; i < s_ring.size(); i++) {
        const char* n = s_ring[(s_pos + i) % s_ring.size()];
        if (n)
            guest::log("trace: %s", n);
    }
}

void noma_trace(const char* name, u32 addr) {
    if (!s_init) {
        s_init = true;
        if (const char* e = getenv("NOMA_TRACE")) {
            std::string w = e;
            s_all = w == "all";
            size_t p = 0;
            while (!s_all && p <= w.size()) {
                size_t q = w.find(',', p);
                if (q == std::string::npos)
                    q = w.size();
                if (q > p)
                    s_words.push_back(w.substr(p, q - p));
                p = q + 1;
            }
        }
        if (const char* e = getenv("NOMA_TRACE_LAST"))
            s_ring.assign(atoi(e), nullptr);
        for (const char* e = getenv("NOMA_TRACE_BT"); e && *e;) {
            if (!strncmp(e, "fn_", 3))
                e += 3;
            char* end;
            s_bt_addrs.push_back((u32)strtoul(e, &end, 16));
            e = *end ? end + 1 : end;
        }
    }
    for (u32 a : s_bt_addrs)
        if (a == addr) {
            char why[96];
            snprintf(why, sizeof(why), "trace: %s entered from:", name);
            guest::platform_backtrace(why);
        }
    if (!s_ring.empty()) {
        s_ring[s_pos] = name;
        s_pos = (s_pos + 1) % s_ring.size();
    }
    if (s_all) {
        guest::log("call %s", name);
        return;
    }
    for (auto& w : s_words)
        if (strstr(name, w.c_str())) {
            guest::log("call %s (sp %08X)", name, guest::g_sp);
            return;
        }
}
#else
void noma_trace_dump() {}
namespace guest {
void noma_trace_dump_global() {}
}
void noma_trace_snapshot() {}
void noma_trace_snapshot_dump(const char*) {}
#endif

// printf-style logging for temporary debugging code in generated sources
#include <cstdarg>
extern "C" void noma_dbg_log(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    guest::log("%s", buf);
}

namespace guest {
void platform_backtrace(const char* why);
}
void noma_dbg_backtrace() { guest::platform_backtrace("backtrace:"); }
