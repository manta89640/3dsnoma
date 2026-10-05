// The native g3d on the 3DS GPU (g3d_pica.cpp): engine/g3d/native_draw.h's Backend for the PICA200.
#pragma once

#include "engine/g3d/native_draw.h"

namespace guest {

// The backend, made at its first call: its shader loaded, its fog ramp in linear memory. Only
// with the GX renderer up (PicaRenderer::init: citro3d running), whose frame its draws go into;
// nullptr if it can't be made.
engine::g3d::Backend* g3d_pica_backend();

}  // namespace guest
