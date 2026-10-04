// The channel's own functions and objects the Sound Player uses, with their arguments spelt out
// (the decompiled functions take the register ABI's full argument list).
#pragma once

#include "noma_decomp.h"
#include "runtime/runtime.h"

namespace engine::soundplayer::game {

// ---- globals
inline u32 wipe() { return guest::load32(GA_lbl_80882C98); }           // layout::parts::Wipe
inline u32 scene_manager() { return guest::load32(GA_lbl_80882CA8); }  // lib::SceneManager
inline u32 controller() { return guest::load32(GA_lbl_80882C9C); }     // the input manager

// the controller's KPAD status: buttons newly pressed this frame (KPAD bits: B 0x400, A 0x800,
// D-pad LEFT 0x1, RIGHT 0x2, DOWN 0x4, UP 0x8)
inline u32 buttons_triggered() { return guest::load32(controller() + 0xB44); }
inline u32 buttons_held() { return guest::load32(controller() + 0xB40); }
enum : u32 { PAD_LEFT = 0x1, PAD_RIGHT = 0x2, PAD_DOWN = 0x4, PAD_UP = 0x8, PAD_B = 0x400, PAD_A = 0x800 };

// ---- lib::SceneManager
inline void change_scene(u32 factory) { g::fn_8018E0C8(scene_manager(), factory); }
// LayoutSceneBase::ChangeSceneWithWipe: lock the scene's cursor (its GetCursor), play the wipe's
// "Out", then change to the scene of that name ("LivingRoomScene")
inline void change_scene_with_wipe(u32 scene, u32 name, u32 wipe_type) { g::fn_8027F978(scene, name, wipe_type); }
const u32 NAME_LIVING_ROOM_SCENE = GA_lbl_80805B68;

// ---- layout::parts::Wipe: 3 = Fade_White
enum : u32 { WIPE_CIRCLE = 0, WIPE_FADE_WHITE = 3 };
inline void wipe_in(u32 type) {
    g::fn_80154D3C(wipe(), type);
    g::fn_80154DE0(wipe());
}

// ---- layout::Button (0x69C bytes)
const u32 VTABLE_LAYOUT_BUTTON = GA_lbl_80794960, LAYOUT_BUTTON_SLOTS = 21;

// ---- layout::Cursor (0x300 bytes)
const u32 CURSOR_SIZE = 0x300;
inline void cursor_ctor(u32 c) { g::fn_80138D2C(c, 0); }
inline void cursor_dtor_delete(u32 c) { g::fn_801397B4(c, 1, 0, 0, 0); }
inline void cursor_update(u32 c) { g::fn_8013A814(c, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); }
// the cursor's layouts, drawn last: its shadow, then the hand
inline void cursor_draw(u32 c) {
    g::fn_8013BECC(c + 0x130);
    g::fn_8013BECC(c + 0x8);
}
// press a listener as the A button would (its Push animation, sound and callbacks)
inline void cursor_press(u32 c, u32 listener) { g::fn_8013A2C0(c, listener, 0); }

// ---- layout::parts::Bottom (0x2460 bytes): the screens' bottom-left way back
const u32 BOTTOM_SIZE = 0x2460;
const u32 BOTTOM_HOME_BTN_02 = 0x1DC0;  // the Home tab (chabudai + B): the way back to the room
enum : u32 { BOTTOM_HIDDEN = 0, BOTTOM_HOME = 4 };
inline void bottom_ctor(u32 b, u32 cursor) { g::fn_8015A9D4(b, BOTTOM_HIDDEN, cursor, 0); }
inline void bottom_set_mode(u32 b, u32 mode, bool at_once) {
    g::fn_8015AFBC(b, mode, at_once, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}
// the B button: press the way back of the current mode
inline void bottom_press_back(u32 b, u32 cursor) { g::fn_8015B308(b, cursor); }
// its window virtuals: +0x8 dtor, +0xC Update, +0x10 Draw
inline void bottom_update(u32 b) { gcall<u32>(guest::load32(guest::load32(b) + 0xC), b); }
inline void bottom_draw(u32 b) { gcall<u32>(guest::load32(guest::load32(b) + 0x10), b); }
inline void bottom_dtor_delete(u32 b) { gcall<u32>(guest::load32(guest::load32(b) + 0x8), b, 1u); }

}  // namespace engine::soundplayer::game
