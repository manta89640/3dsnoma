// Guest memory: MEM1 (24 MB at 0x80000000) and MEM2 (at 0x90000000), both also reachable
// through their uncached mirrors (0xC0000000, 0xD0000000). Hardware registers (0xCC.., 0xCD..)
// and everything unmapped go to mmio_read/mmio_write (hw/mmio.cpp).
#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace guest {

u8* g_page[256];
u32 g_sp;

static u8* s_mem1;
static u8* s_mem2;
static u32 s_mem2_size;
// write generations, per 4 KB page (see mem_mark_written)
static std::atomic<u32> s_gen{1};  // (the GPU's thread marks its EFB copies too)
static u32 s_page_gen1[(32u << 20) >> 12];
static u32* s_page_gen2;

void memory_init(u32 mem2_size) {
    // MEM1 is 24 MB; the 16 MB pages cover 32 MB so that page 0x81 is fully backed.
    s_mem1 = (u8*)platform_alloc_guest(32u << 20);
    s_mem2_size = mem2_size;
    s_mem2 = (u8*)platform_alloc_guest(mem2_size);
    s_page_gen2 = (u32*)calloc(mem2_size >> 12, sizeof(u32));
    if (!s_mem1 || !s_mem2)
        fatal("guest memory: could not allocate %u MB", (32u << 20) / (1u << 20) + mem2_size / (1u << 20));
    for (int i = 0; i < 256; i++)
        g_page[i] = nullptr;
    for (u32 p = 0; p < 2; p++) {
        g_page[0x80 + p] = s_mem1 + (p << 24);
        g_page[0xC0 + p] = s_mem1 + (p << 24);
    }
    for (u32 p = 0; p < (mem2_size >> 24); p++) {
        g_page[0x90 + p] = s_mem2 + (p << 24);
        g_page[0xD0 + p] = s_mem2 + (p << 24);
    }
}

u32 mem2_size() { return s_mem2_size; }

// ---- write generations: per 4 KB page of MEM1/MEM2, the last time the guest made new data in
// it visible to the hardware (a cache flush, an IOS or GPU write). Caches of things built from
// guest memory (decoded textures, sample data) only look again at what changed.

static u32* page_gens(u32 phys, u32* first, u32* limit) {
    if (phys < (32u << 20)) {
        *first = phys >> 12, *limit = (32u << 20) >> 12;
        return s_page_gen1;
    }
    if (phys >= 0x10000000u && phys - 0x10000000u < s_mem2_size) {
        *first = (phys - 0x10000000u) >> 12, *limit = s_mem2_size >> 12;
        return s_page_gen2;
    }
    return nullptr;
}

u32 mem_mark_written(u32 addr, u32 size) {
    u32 g = s_gen.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!size)
        return g;
    u32 first, limit;
    u32* gens = page_gens(addr & 0x1FFFFFFF, &first, &limit);
    if (!gens)
        return g;
    u32 last = std::min(limit - 1, first + (((addr & 0xFFF) + size - 1) >> 12));
    for (u32 p = first; p <= last; p++)
        gens[p] = g;
    return g;
}

u32 mem_generation(u32 addr, u32 size) {
    u32 first, limit, g = 0;
    u32* gens = page_gens(addr & 0x1FFFFFFF, &first, &limit);
    if (!gens || !size)
        return 0;
    u32 last = std::min(limit - 1, first + (((addr & 0xFFF) + size - 1) >> 12));
    for (u32 p = first; p <= last; p++)
        g = std::max(g, gens[p]);
    return g;
}

// how much of MEM1/MEM2 holds anything but zeros (4 KB pages), for sizing the 3DS build
void memory_report() {
    auto count = [](const u8* m, u32 size, u32* top) {
        u32 used = 0;
        *top = 0;
        for (u32 o = 0; o < size; o += 4096) {
            const u64* p = (const u64*)(m + o);
            for (int i = 0; i < 512; i++)
                if (p[i]) {
                    used++;
                    *top = o + 4096;
                    break;
                }
        }
        return used * 4;
    };
    u32 top1, top2;
    u32 u1 = count(s_mem1, 24u << 20, &top1), u2 = count(s_mem2, s_mem2_size, &top2);
    log("memory: MEM1 %u KB in use (highest %08X), MEM2 %u KB in use (highest %08X)", u1, 0x80000000 + top1, u2,
        0x90000000 + top2);
}

void memcpy_g(u32 dst, u32 src, u32 n) {
    u8* d = host_ptr(dst);
    u8* s = host_ptr(src);
    if (d && s && host_ptr(dst + n - 1) == d + n - 1 && host_ptr(src + n - 1) == s + n - 1) {
        memmove(d, s, n);
        return;
    }
    if (dst <= src) {
        for (u32 i = 0; i < n; i++)
            store8(dst + i, load8(src + i));
    } else {
        for (u32 i = n; i-- > 0;)
            store8(dst + i, load8(src + i));
    }
}

void memset_g(u32 dst, u8 v, u32 n) {
    u8* d = host_ptr(dst);
    if (d && host_ptr(dst + n - 1) == d + n - 1) {
        memset(d, v, n);
        return;
    }
    for (u32 i = 0; i < n; i++)
        store8(dst + i, v);
}

void read_g(void* out, u32 src, u32 n) {
    u8* s = host_ptr(src);
    if (s && host_ptr(src + n - 1) == s + n - 1) {
        memcpy(out, s, n);
        return;
    }
    for (u32 i = 0; i < n; i++)
        ((u8*)out)[i] = load8(src + i);
}

void write_g(u32 dst, const void* in, u32 n) {
    mem_mark_written(dst, n);  // IOS and device writes: what the hardware wrote, others see
    u8* d = host_ptr(dst);
    if (d && host_ptr(dst + n - 1) == d + n - 1) {
        memcpy(d, in, n);
        return;
    }
    for (u32 i = 0; i < n; i++)
        store8(dst + i, ((const u8*)in)[i]);
}

std::string read_string(u32 addr, u32 max) {
    std::string s;
    for (u32 i = 0; i < max; i++) {
        u8 c = load8(addr + i);
        if (!c)
            break;
        s.push_back((char)c);
    }
    return s;
}

void fatal(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log("FATAL: %s", buf);
#ifdef NOMA_PROFILE_CALLS
    // where the guest was: its function stack (profiling builds keep one, also on the 3DS)
    {
        ProfStack* st = g_prof;
        u32 depth = std::min((u32)st->depth, PROF_DEPTH);
        log("FATAL: in guest thread %08X:", st->thread);
        for (u32 i = depth; i-- > 0 && depth - i <= 24;)
            log("    %s", st->names[i]);
    }
#endif
    platform_fatal(buf);
    abort();
}

// NOMA_DUMP_MEM (host, on a signal): MEM1 and MEM2 to mem1.bin / mem2.bin for offline searching
void memory_dump() {
    FILE* f = fopen("mem1.bin", "wb");
    if (f) {
        fwrite(s_mem1, 1, 24u << 20, f);
        fclose(f);
    }
    f = fopen("mem2.bin", "wb");
    if (f) {
        fwrite(s_mem2, 1, s_mem2_size, f);
        fclose(f);
    }
}

}  // namespace guest
