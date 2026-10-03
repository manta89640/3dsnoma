// A guest object (a struct in guest memory) as native code sees it: its address resolved to
// host memory once, its fields read and written big-endian at their offsets. MEM1 and MEM2 are
// each one block of host memory, so an object is contiguous from its first byte.
#pragma once

#include <cstring>

#include "guest.h"

namespace engine::snd {

struct GuestObj {
    u32 addr = 0;
    u8* p = nullptr;

    GuestObj() = default;
    explicit GuestObj(u32 a) : addr(a) {
        if (a) {
            p = guest::host_ptr(a);
            if (__builtin_expect(p == nullptr, 0))
                guest::fatal("engine/snd: object at unmapped guest address %08X", a);
        }
    }
    explicit operator bool() const { return addr != 0; }

    u8 get8(u32 o) const { return p[o]; }
    u16 get16(u32 o) const {
        u16 v;
        memcpy(&v, p + o, 2);
        return __builtin_bswap16(v);
    }
    s16 gets16(u32 o) const { return (s16)get16(o); }
    u32 get32(u32 o) const {
        u32 v;
        memcpy(&v, p + o, 4);
        return __builtin_bswap32(v);
    }
    s32 gets32(u32 o) const { return (s32)get32(o); }
    f32 getf(u32 o) const {
        u32 w = get32(o);
        f32 f;
        memcpy(&f, &w, 4);
        return f;
    }

    void set8(u32 o, u8 v) const { p[o] = v; }
    void set16(u32 o, u16 v) const {
        v = __builtin_bswap16(v);
        memcpy(p + o, &v, 2);
    }
    void set32(u32 o, u32 v) const {
        v = __builtin_bswap32(v);
        memcpy(p + o, &v, 4);
    }
    void setf(u32 o, f32 f) const {
        u32 w;
        memcpy(&w, &f, 4);
        set32(o, w);
    }
    void or32(u32 o, u32 bits) const { set32(o, get32(o) | bits); }
};

// the PowerPC's float-to-integer conversions as the decompiled code does them
// (NOMA_FCTIWZ, NOMA_FP2U in runtime/guest.h), on floats: fctiwz saturates, NaN gives INT_MIN
inline s32 fctiwz(f32 x) {
    if (!(x == x) || x <= -2147483648.0f)
        return (s32)0x80000000u;
    if (x >= 2147483648.0f)
        return 0x7FFFFFFF;
    return (s32)x;
}
inline u32 fp2u(f32 x) {
    if (x < 0.0f)
        return 0;
    if (!(x < 4294967296.0f))
        return 0xFFFFFFFFu;
    if (x < 2147483648.0f)
        return (u32)(s32)x;
    return (u32)(s32)(x - 2147483648.0f) + 0x80000000u;
}

// CodeWarrior's division of a 16-bit difference by 96 (a ramp over one 3 ms AX frame): truncated
inline s32 div96(s32 x) { return x / 96; }

}  // namespace engine::snd
