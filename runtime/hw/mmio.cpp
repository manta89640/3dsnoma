// Hardware register accesses from the decompiled SDK: 0xCC000000 (GameCube-era devices),
// 0xCD000000 (Hollywood: IPC, GPIO, and the Wii addresses of SI/EXI/AI/DI) and the
// write-gather pipe at 0xCC008000 that feeds the GPU.
#include <cstdio>
#include <map>

#include "hw.h"

namespace guest {

struct Device {
    u32 base, size;
    const char* name;
    u32 (*read)(u32 off, int size);
    void (*write)(u32 off, u32 value, int size);
};

static std::map<u32, int> s_unhandled;

// an address that is neither memory nor a device: always a bug somewhere upstream. `from`: the
// host code that accessed it (addr2line on the executable names the decompiled function)
static void bad_address(u32 addr, bool write, void* from) {
    fatal("%s of unmapped guest address 0x%08X (guest sp %08X, host pc %p)", write ? "write" : "read", addr,
          g_sp, from);
}

static u32 unhandled_read(const char* dev, u32 addr, int size) {
    if (s_unhandled[addr]++ < 3)
        log("mmio: read%d %s 0x%08X (unhandled) -> 0", size * 8, dev, addr);
    return 0;
}

static void unhandled_write(const char* dev, u32 addr, u32 v, int size) {
    if (s_unhandled[addr]++ < 3)
        log("mmio: write%d %s 0x%08X = 0x%08X (unhandled)", size * 8, dev, addr, v);
}

static const Device s_devices[] = {
    {0x0C000000, 0x80, "CP", cp_read, cp_write},
    {0x0C001000, 0x100, "PE", pe_read, pe_write},
    {0x0C002000, 0x100, "VI", vi_read, vi_write},
    {0x0C003000, 0x100, "PI", pi_read, pi_write},
    {0x0C004000, 0x80, "MI", mi_read, mi_write},
    {0x0C005000, 0x200, "DSP", dsp_read, dsp_write},
    {0x0C006000, 0x40, "DI", di_read, di_write},
    {0x0C006400, 0x100, "SI", si_read, si_write},
    {0x0C006800, 0x80, "EXI", exi_read, exi_write},
    {0x0C006C00, 0x20, "AI", ai_read, ai_write},
    {0x0C008000, 0x1000, "WGPIPE", nullptr, gx_fifo_write},
    {0x0D000000, 0x400, "HW", hollywood_read, hollywood_write},
    {0x0D006000, 0x40, "DI", di_read, di_write},
    {0x0D006400, 0x100, "SI", si_read, si_write},
    {0x0D006800, 0x80, "EXI", exi_read, exi_write},
    {0x0D006C00, 0x20, "AI", ai_read, ai_write},
    {0x0D800000, 0x400, "HW", hollywood_read, hollywood_write},
    {0x0D806000, 0x40, "DI", di_read, di_write},
    {0x0D806400, 0x100, "SI", si_read, si_write},
    {0x0D806800, 0x80, "EXI", exi_read, exi_write},
    {0x0D806C00, 0x20, "AI", ai_read, ai_write},
};

static const Device* find_device(u32 addr) {
    u32 phys = addr & 0x0FFFFFFF;
    for (const Device& d : s_devices)
        if (phys >= d.base && phys < d.base + d.size)
            return &d;
    return nullptr;
}

u32 mmio_read(u32 addr, int size) {
    if ((addr >> 28) == 0xE)  // locked cache
        return lc_read(addr, size);
    const Device* d = find_device(addr);
    if (!d)
        bad_address(addr, false, __builtin_return_address(0));
    if (!d->read)
        return unhandled_read(d->name, addr, size);
    return d->read((addr & 0x0FFFFFFF) - d->base, size);
}

void mmio_write(u32 addr, u32 value, int size) {
    if ((addr & 0x0FFFF000) == 0x0C008000) {  // the write-gather pipe: every GX command and vertex
        gx_fifo_write(addr & 0xFFF, value, size);
        return;
    }
    if ((addr >> 28) == 0xE) {
        lc_write(addr, value, size);
        return;
    }
    const Device* d = find_device(addr);
    if (!d)
        bad_address(addr, true, __builtin_return_address(0));
    if (!d->write) {
        unhandled_write(d->name, addr, value, size);
        return;
    }
    d->write((addr & 0x0FFFFFFF) - d->base, value, size);
}

void mmio_log_unhandled(const char* dev, u32 off, u32 v, int size, bool write) {
    if (write)
        unhandled_write(dev, off, v, size);
    else
        unhandled_read(dev, off, size);
}

// ---- locked cache (memory.cpp maps it as a page: only addresses beyond its first 1 MB come
// here, and wrap around as its 16 KB do)
u32 lc_read(u32 addr, int size) {
    u32 o = addr & (LC_SIZE - 1);
    u32 v = 0;
    for (int i = 0; i < size; i++)
        v = (v << 8) | g_locked_cache[(o + i) & (LC_SIZE - 1)];
    return v;
}
void lc_write(u32 addr, u32 value, int size) {
    u32 o = addr & (LC_SIZE - 1);
    for (int i = size - 1; i >= 0; i--) {
        g_locked_cache[(o + i) & (LC_SIZE - 1)] = (u8)value;
        value >>= 8;
    }
}

}  // namespace guest
