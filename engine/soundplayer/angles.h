// Angles for the Sound Player's drawings (the stage, the amp, the bar button's icon); the vectors
// and matrices are engine/math/vecmath.h's.
#pragma once

#include "runtime/runtime.h"

namespace engine::soundplayer {

constexpr f32 PI = 3.14159265f;

// degrees to radians, as deg * PI / 180 (in that order: the drawings' numbers were tuned with it)
inline f32 deg_to_rad(f32 deg) { return deg * PI / 180; }

}  // namespace engine::soundplayer
