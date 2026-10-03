// The channel's heaps (lib::allocator, a tree of named heaps under lib_allocator_HeapBase_sRoot): every `new`
// goes through lib_allocator_HeapBase_AllocFromCurrent, which allocates from the current heap. The channel doesn't check
// for failure (the object is used at address 0), so a failed allocation is logged here, with the
// calls that led to it where the platform can show them (the host: its own stack, the guest's).
#include "noma_decomp.h"
#include "runtime.h"

namespace guest {
void platform_backtrace(const char* why);
}

namespace g {

u32 lib_allocator_HeapBase_AllocFromCurrent_orig(u32 r3, u32 r4);

// r3: the heap manager (+0x1C the current heap: +0x14 its name), r4: the size
u32 lib_allocator_HeapBase_AllocFromCurrent(u32 r3, u32 r4) {
    u32 p = lib_allocator_HeapBase_AllocFromCurrent_orig(r3, r4);
    if (!p) {
        u32 heap = M2C_FIELD(r3, gp<u32>, 0x1C);
        guest::log("heap: out of memory: %u bytes from heap %s (%08X)", r4,
                   guest::read_string(M2C_FIELD(heap, gp<u32>, 0x14), 32).c_str(), heap);
        guest::platform_backtrace("heap: called from");
    }
    return p;
}

}  // namespace g
