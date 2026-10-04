// 「広い世界へ」 (Hiroi Sekai e, "Into the Wide World"): Wuhu Island's music, the port's own
// composition (its score is tools/music/hiroi_sekai.py), played by the channel's own sound system
// with the channel's own instruments. The room's theme pauses as the player sets out for the
// island and comes back, where it paused, on the way home.
#pragma once

#include "runtime/runtime.h"

namespace engine::wuhu {

// The sound ID the piece is asked for by (sound::BGMManager, the Sound Player's list): no sound
// of the channel's archive has it (the archive holds 156) and it isn't -1, the BGM manager's
// "none". The sound manager's start (music.cpp) plays our sequence for it.
constexpr u32 WIDE_WORLD_ID = 0x1000;

// At the start, where lasting guest memory is made (engine/system/host_scene.h): the sequence and
// what starts it are put into guest memory.
void music_boot();

// The island's flow (the island scene calls these, in this order):
// leaving the room, whatever plays fades out and the room's theme pauses (half a second);
void music_depart();
// on the island, once it is up, our piece plays from its start, at once;
void music_arrive();
// leaving the island, ours fades out as the room's theme fades back in where it paused (the BGM
// manager's own crossfade, 3 s), so the room finds its theme playing when it is made again.
void music_leave();

}  // namespace engine::wuhu
