// Guest memory: MEM1 (24 MB at 0x80000000) and MEM2 (at 0x90000000, as large as the platform
// gives it), both also reachable through their uncached mirrors (0xC0000000, 0xD0000000).
// Hardware registers (0xCC.., 0xCD..) and everything unmapped go to mmio_read/mmio_write
// (hw/mmio.cpp).
#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace guest {

uintptr_t g_page[1u << (32 - PAGE_BITS)];
u32 g_sp;

u8 g_locked_cache[LC_SIZE] __attribute__((aligned(32)));
u8 g_mem1[MEM1_SIZE] __attribute__((aligned(64)));
static u8* const s_mem1 = g_mem1;
static u8* s_mem2;
static u32 s_mem2_size;
// write generations, per 4 KB page (see mem_mark_written)
static std::atomic<u32> s_gen{1};  // (the GPU's thread marks its EFB copies too)
static u32 s_page_gen1[MEM1_SIZE >> 12];
static u32* s_page_gen2;

// a bank's cached and uncached views (its physical address | 0x80000000, | 0xC0000000)
static void map_bank(u32 phys, u8* host, u32 size) {
    for (u32 o = 0; o < size; o += PAGE_SIZE)
        for (u32 view : {0x80000000u, 0xC0000000u}) {
            u32 a = view | (phys + o);
            g_page[a >> PAGE_BITS] = (uintptr_t)(host + o) - a;
            if (!g_page[a >> PAGE_BITS])  // (0 means unmapped)
                fatal("guest memory: page %08X maps to its own address", a);
        }
}

void memory_init(u32 mem2_size) {
    if (mem2_size % PAGE_SIZE)
        fatal("guest memory: MEM2 size %X is not a whole number of pages", mem2_size);
    s_mem2_size = mem2_size;
    s_mem2 = (u8*)platform_alloc_guest(mem2_size);
    s_page_gen2 = (u32*)calloc(mem2_size >> 12, sizeof(u32));
    if (!s_mem2)
        fatal("guest memory: could not allocate %u MB", mem2_size >> 20);
    for (uintptr_t& b : g_page)
        b = 0;
    map_bank(0x00000000, s_mem1, MEM1_SIZE);
    map_bank(0x10000000, s_mem2, mem2_size);
    // (its 1 MB page holds only the 16 KB the guest uses: NW4R stays within them)
    g_page[LC_BASE >> PAGE_BITS] = (uintptr_t)g_locked_cache - LC_BASE;
}

u32 mem2_size() { return s_mem2_size; }

// ---- write generations: per 4 KB page of MEM1/MEM2, the last time the guest made new data in
// it visible to the hardware (a cache flush, an IOS or GPU write). Caches of things built from
// guest memory (decoded textures, sample data) only look again at what changed.

static u32* page_gens(u32 phys, u32* first, u32* limit) {
    if (phys < MEM1_SIZE) {
        *first = phys >> 12, *limit = MEM1_SIZE >> 12;
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
    u32 u1 = count(s_mem1, MEM1_SIZE, &top1), u2 = count(s_mem2, s_mem2_size, &top2);
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
        fwrite(s_mem1, 1, MEM1_SIZE, f);
        fclose(f);
    }
    f = fopen("mem2.bin", "wb");
    if (f) {
        fwrite(s_mem2, 1, s_mem2_size, f);
        fclose(f);
    }
}

}  // namespace guest
