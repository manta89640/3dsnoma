// Boot: load main.dol into guest memory, set up what the IOS loader leaves in low memory,
// then run what __start does: OSInit, the C++ static constructors, main.
#include <cstdio>
#include <vector>

#include "noma_decomp.h"
#include <cstdlib>
#include <cstring>

#include "runtime.h"

namespace guest {

// The DOL (orig/HCIJ/sys/main.dol) is the same file the code was decompiled from; its text is
// loaded too, because some OS code copies or reads it (exception vectors, version strings).
bool load_dol(const std::vector<u8>& dol) {
    auto rd = [&](u32 off) {
        return (u32)dol[off] << 24 | (u32)dol[off + 1] << 16 | (u32)dol[off + 2] << 8 | dol[off + 3];
    };
    if (dol.size() < 0x100)
        return false;
    for (int i = 0; i < 18; i++) {
        u32 off = rd(i * 4), addr = rd(0x48 + i * 4), size = rd(0x90 + i * 4);
        if (!size)
            continue;
        if (off + size > dol.size())
            return false;
        write_g(addr, dol.data() + off, size);
    }
    u32 bss = rd(0xD8), bss_size = rd(0xDC);
    // .bss/.sbss lie between the data sections; clear only bytes no section loaded
    std::vector<bool> loaded(bss_size, false);
    for (int i = 0; i < 18; i++) {
        u32 addr = rd(0x48 + i * 4), size = rd(0x90 + i * 4);
        for (u32 a = addr; a < addr + size; a++)
            if (a >= bss && a < bss + bss_size)
                loaded[a - bss] = true;
    }
    for (u32 i = 0; i < bss_size; i++)
        if (!loaded[i])
            store8(bss + i, 0);
    log("dol: entry %08X, bss %08X-%08X", rd(0xE0), bss, bss + bss_size);
    return true;
}

// Low memory as IOS leaves it for a NAND title (values as Dolphin's HLE boot sets them).
void setup_low_memory(u32 title_id_hi, u32 title_id_lo) {
    auto w32 = [](u32 a, u32 v) { store32(a, v); };
    auto w16 = [](u32 a, u16 v) { store16(a, v); };
    u32 mem2_end = 0x90000000 + mem2_size();
    w32(0x80000020, 0x0D15EA5E);   // boot magic
    w32(0x80000024, 0x00000001);   // version
    w32(0x80000028, 0x01800000);   // physical MEM1 size
    w32(0x8000002C, 0x00000023);   // console type: retail RVL
    w32(0x80000030, 0x00000000);   // arena lo (0: the OS computes it)
    w32(0x80000034, 0x817FEC60);   // arena hi
    w32(0x80000038, 0x817FEC60);   // FST location
    w32(0x8000003C, 0x00000000);   // FST size
    w32(0x800000CC, 0x00000000);   // video mode: NTSC
    w32(0x800000EC, 0x81800000);   // debug monitor address
    w32(0x800000F0, 0x01800000);   // simulated MEM1 size
    w32(0x800000F4, 0x00000000);   // BI2 (none: channel)
    w32(0x800000F8, 0x0E7BE2C0);   // bus clock 243 MHz
    w32(0x800000FC, 0x2B73A840);   // CPU clock 729 MHz
    w16(0x800030E0, 0x0000);
    w32(0x80003100, 0x01800000);   // physical MEM1 size
    w32(0x80003104, 0x01800000);   // simulated MEM1 size
    w32(0x80003108, 0x817FEC60);   // end of MEM1 the PPC uses
    w32(0x8000310C, 0x00000000);   // MEM1 arena start (0: the OS computes it)
    w32(0x80003110, 0x00000000);   // MEM1 arena end (0: arena hi from 0x80000034)
    w32(0x80003114, 0x00000000);
    w32(0x80003118, mem2_size());  // physical MEM2 size
    w32(0x8000311C, mem2_size());  // simulated MEM2 size
    w32(0x80003120, mem2_end);     // end of MEM2 the PPC can address
    w32(0x80003124, 0x90000800);   // usable MEM2 start
    w32(0x80003128, mem2_end - 0x20000);  // usable MEM2 end (IPC buffers above)
    w32(0x80003130, mem2_end - 0x20000);  // IPC buffer start
    w32(0x80003134, mem2_end);     // IPC buffer end
    w32(0x80003138, 0x00000011);   // Hollywood version
    w32(0x80003140, 0x00380000 | 0x1525);  // IOS56
    w32(0x80003144, 0x00062507);   // IOS build date
    w32(0x80003148, mem2_end);     // IOS heap start (none in guest memory)
    w32(0x8000314C, mem2_end);     // IOS heap end
    w32(0x80003158, 0x0000FF16);   // GDDR vendor
    w32(0x8000315C, 0x00000000);   // boot flags
    w32(0x80003180, title_id_lo);  // game id (title id low for channels)
    store8(0x80003184, 0x81);      // application type: 0x81 NAND title (0x80 would be a disc)
    w32(0x80003188, 0x00380000);   // required IOS
    w32(0x8000318C, 0x00000000);
}

}  // namespace guest

using namespace guest;

// What __start does after its register/data setup (see .init: __start).
// NOMA_SELFTEST: exercise a few decompiled library functions before the game starts
static void selftest() {
    using namespace guest;
    u32 buf = 0x80003F00, fmt = 0x80003F80, str = 0x80003FC0;
    const char* f = "/dev/usb/%s/%x/%x|%d|%5.2f";
    write_g(fmt, f, (u32)strlen(f) + 1);
    write_g(str, "oh1", 4);
    memset_g(buf, 0, 0x40);
    u32 n = g::snprintf(buf, 0x40, fmt, str, 0x57e, 0x305, (u32)-42, 0, 1.5, 0, 0, 0, 0, 0, 0, 0);
    log("selftest: snprintf -> %u '%s'", n, read_string(buf, 64).c_str());
}

void noma_start_guest() {
    g_sp = 0x809A0C20 - 8;  // __init_registers: stack top, then __start's first frame
    store32(g_sp, 0xFFFFFFFF);
    store32(g_sp + 4, 0xFFFFFFFF);
    store32(0x80000044, 0);  // debugger flag
    if (getenv("NOMA_SELFTEST"))
        selftest();
    log("OSInit");
    g::OSInit();
    log("__init_user (static constructors)");
    g::__init_user(0);
    log("main");
    g::main();
    log("main returned");
    g::exit(0);
}
