// The GX library's hot calls, native (gx_sdk.cpp), for other natives and runtime patches.
#pragma once

#include "noma_decomp.h"

namespace engine::gx {

void set_dirty_state();           // __GXSetDirtyState
void set_matrix_index(u32 which); // __GXSetMatrixIndex
// GXLoadPosMtxImm(mtx, id); returns the registers the original leaves (r3 mtx, r4 the pipe)
u64 load_pos_mtx_imm(u32 mtx, u32 id);

}  // namespace engine::gx
