// The channel's own 3D models drawn natively: nw4r::g3d's DrawResMdlDirectly (every ScnMdl and
// ScnMdlSimple draw of every ScnRoot pass: the room, the sky, the theater, the Miis' bodies) in
// C++, drawn by the native g3d's backends in place of the emulated GX path (see live_draw.cpp;
// until the backends take live models, the game's own function still draws).
// NOMA_CHECK_G3D_LIVE=1 (host): the game's own function draws, and the native derivation is
// compared draw by draw with what its GX commands leave in the GPU's registers.
#pragma once

#include "runtime/guest.h"

namespace engine::g3d::live {

// The check's hooks in the host's renderer (no-ops unless NOMA_CHECK_G3D_LIVE): a 0x50 command's
// id (true: one of the check's own markers, not a native draw), and every draw the GX decoder
// executes (with g_gx as it stands for that draw).
bool check_marker(u32 id);
void check_gx_draw(u32 prim, u32 vat, u32 count, const u8* data);

}  // namespace engine::g3d::live
