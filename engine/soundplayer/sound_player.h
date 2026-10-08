// The Sound Player: a screen of the room's for listening to the channel's music, with the
// household's Miis dancing at the TV studio's table. Reached from the room's bottom bar
// (runtime/patches/menus.cpp), it is a scene of its own (engine/system/host_scene.h).
#pragma once

#include "runtime/runtime.h"

namespace engine::soundplayer {

// at the start, right after the channel's own scene factories: the guest objects that last
void boot();

// the scene's guest lib::Scene::Factory
u32 scene_factory();

// controller::living::SceneTransit::To for the Sound Player (the game's own go up to 8)
const u32 TO_SOUND_PLAYER = 9;

// the room bar's button for the Sound Player (a layout::Button made from いろんな間's layout):
// give it its own icon and label (their textures' block is reserved at the start)
void reserve_bar_button();
void dress_bar_button(u32 button);

}  // namespace engine::soundplayer
