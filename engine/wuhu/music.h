// The island's music, 「広い世界へ」 (docs/wuhu_design.md §6: engine/wuhu/music.cpp, the music's
// own track): when the scene's way in and out call it.
#pragma once

namespace engine::wuhu {

// 出発 pushed: the room fades out to black (the room's theme can go with it)
void music_depart();
// the island is ready and fades in from black
void music_arrive();
// はい in the island's window: the island fades out to black and the room is made again (the
// room's theme is to come back with it)
void music_leave();

}  // namespace engine::wuhu
