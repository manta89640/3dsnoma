// CodeWarrior's 64-bit integer helpers (runtime.c), native: a 64-bit value travels as r3 (high
// word) and r4 (low word); the result too (NOMA_U64). Shift counts follow the PowerPC's slw/srw
// (a count's low 6 bits; 32..63 shift a word out completely), as the original's do.
#include "noma_decomp.h"

namespace g {

static inline u64 pair(u32 hi, u32 lo) { return (u64)hi << 32 | lo; }
static inline u64 ret(u64 v) { return NOMA_U64((u32)(v >> 32), (u32)v); }

// division by zero has no defined result (the original loops on with a zero divisor): 0 here
u64 __div2u(u32 ahi, u32 alo, u32 bhi, u32 blo) {
    u64 b = pair(bhi, blo);
    return ret(b ? pair(ahi, alo) / b : 0);
}

u64 __mod2u(u32 ahi, u32 alo, u32 bhi, u32 blo) {
    u64 a = pair(ahi, alo), b = pair(bhi, blo);
    return ret(b ? a % b : a);
}

u64 __div2i(u32 ahi, u32 alo, u32 bhi, u32 blo) {
    s64 a = (s64)pair(ahi, alo), b = (s64)pair(bhi, blo);
    if (!b)
        return ret(0);
    if (b == -1)  // INT64_MIN / -1 wraps, as two's complement negation does
        return ret(0 - (u64)a);
    return ret((u64)(a / b));
}

u64 __mod2i(u32 ahi, u32 alo, u32 bhi, u32 blo) {
    s64 a = (s64)pair(ahi, alo), b = (s64)pair(bhi, blo);
    if (!b)
        return ret((u64)a);
    if (b == -1)
        return ret(0);
    return ret((u64)(a % b));
}

u64 __shl2i(u32 hi, u32 lo, u32 n) {
    return ret(pair(NOMA_SLW(hi, n) | NOMA_SRW(lo, 32 - n) | NOMA_SLW(lo, n - 32), NOMA_SLW(lo, n)));
}

}  // namespace g
