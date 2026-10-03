// The parts of MSL's math library (fdlibm) whose results are exactly specified, native: the
// square root and rounding are correctly rounded in both, so the hardware's give the same bits as
// fdlibm's software loops (sin, cos and the like are not: their last bits differ between
// implementations, so they stay decompiled).
#include <cmath>

#include "noma_decomp.h"

namespace g {

// fdlibm's __ieee754_sqrt: bit by bit, 26 iterations a half
f64 __ieee754_sqrt(f64 x) {
    return ::sqrt(x);
}

f64 sqrt(f64 x) {
    return ::sqrt(x);
}

f64 floor(f64 x) {
    return ::floor(x);
}

f64 ceil(f64 x) {
    return ::ceil(x);
}

}  // namespace g
