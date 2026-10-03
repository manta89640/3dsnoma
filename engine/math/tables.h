// nw4r::math's lookup tables (engine/math/tables.cpp).
#pragma once

#include "guest.h"

namespace engine::math {

// angles as nw4r's FIdx: 256 to a turn
struct SinCos {
    f32 sin_val, cos_val, sin_delta, cos_delta;
};
struct ArcTan {
    f32 atan_val, atan_delta;
};

extern const SinCos kSinCosTable[257];  // by FIdx (entry 256 = entry 0)
extern const ArcTan kArcTanTable[33];   // by 32 * tan, in FIdx

}  // namespace engine::math
