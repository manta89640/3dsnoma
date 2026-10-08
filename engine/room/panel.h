// The room's bottom screen: its buttons, the channel's own, laid out for the 3DS's touch screen.
//
// On the Wii the room (scene::LivingRoomScene) has its bar along the bottom of the TV picture
// (layout::living::MoveBtn, Move_Btn_Scene.lex: the way back, the theater, the port's サウンド and
// 出発, the TV programmes' end cap) and the その他 tab at the top right, whose column opens when
// the pointer comes to it (メッセージボード, メッセージを送る, 使い方, 設定). On the 3DS the top
// screen shows the room alone, and the bottom one these buttons, worked by touch: the bar at the
// bottom edge as the channel has it (the same layouts at the same places: the bottom screen's
// region of the layouts' space reaches down to the layouts' bottom edge and in to their left one),
// and the その他 column's buttons above it, two by two. The room's own bar and その他 are still
// made (the room's code drives them), but hidden and out of the pointer's reach
// (runtime/patches/menus.cpp), and the room's hand isn't drawn: the touch screen points.
//
// The buttons are the room's: they hang from a Move_Btn_Scene of the panel's own and are hit-tested
// by the room's cursor, and they go where the room's would, through SceneTransit::Request (the
// room's input locked, its circle wipe, then the scene). The way back asks the room's question
// (the channel's message 145, "Wiiの間をおわります。") in the channel's dialog on the bottom
// screen, and はい ends the app as the room's own dialog does.
#pragma once

#include "runtime/runtime.h"

namespace engine::room {

// the room scene made (scene_LivingRoomScene_ctor): the panel made for it
void open(u32 scene);
// the room going (its destructor, before the room's own parts go: the panel's buttons are its
// cursor's)
void close();
// once a frame, after the room's own update
void update();
// after the room's own draw (on the top screen): the panel on the bottom screen
void draw();

}  // namespace engine::room
