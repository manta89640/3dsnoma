// Wuhu Island, walkable: a scene of the room's (docs/wuhu_design.md), reached from the room's bottom
// bar (出発, runtime/patches/menus.cpp) through a fade to black (runtime/patches/wipes.cpp), and left
// the same way. It is a scene of its own (engine/system/host_scene.h): its shell (scene.cpp: the
// loading screen, the arrival caption, the Home tab and the window that asks before going back)
// around the island's content (content.h).
#pragma once

#include "runtime/runtime.h"

namespace engine::wuhu {

// controller::living::SceneTransit::To for the island (the game's own go up to 8, the Sound
// Player's is 9)
const u32 TO_ISLAND = 10;

// at the start, right after the channel's own scene factories: the guest objects that last
void boot();

// the scene's guest lib::Scene::Factory
u32 scene_factory();

// The room's next fade in (its layout_parts_Wipe_In) is to come from black: set as the island
// fades out to the room, taken (true once) by runtime/patches/wipes.cpp, so that the way back is
// black both ways as the way there is. A flag left untaken (the room didn't fade in) lapses after
// a few seconds.
bool take_black_return();

}  // namespace engine::wuhu
