// The channel's own functions and objects the Sound Player uses, with their arguments spelt out
// (the decompiled functions take the register ABI's full argument list).
#pragma once

#include "engine/math/vecmath.h"
#include "noma_decomp.h"
#include "runtime/runtime.h"

namespace engine::soundplayer::game {

// ---- globals
inline u32 wipe() { return guest::load32(GA_layout_parts_Wipe_sInstance); }        // layout::parts::Wipe
inline u32 controller() { return guest::load32(GA_input_Controller_sInstance); }  // the input manager

// the controller's KPAD status: +0xB40 the buttons held, +0xB44 those newly pressed this frame
// (KPAD bits: B 0x400, A 0x800, D-pad LEFT 0x1, RIGHT 0x2, DOWN 0x4, UP 0x8), +0xB60 the pointer's
// position (two f32), +0xB9E whether the pointer is on the screen
inline u32 buttons_triggered() { return guest::load32(controller() + 0xB44); }
inline u32 buttons_held() { return guest::load32(controller() + 0xB40); }
enum : u32 {
    PAD_LEFT = 0x1, PAD_RIGHT = 0x2, PAD_DOWN = 0x4, PAD_UP = 0x8, PAD_PLUS = 0x10,
    PAD_B = 0x400, PAD_A = 0x800, PAD_MINUS = 0x1000,
};
// the pointer is on the screen (KPADStatus dpd_valid_fg, s8): on the 3DS, while the touch screen or
// the circle pad was used in the last three seconds
inline bool pointer_on_screen() { return (s8)guest::load8(controller() + 0xB9E) > 0; }
// where the pointer is (KPADStatus pos: -1..1 across the screen)
inline void pointer_at(f32& x, f32& y) {
    u32 c = controller();
    x = engine::math::load_f32(c + 0xB60);
    y = engine::math::load_f32(c + 0xB64);
}

// ---- leaving the scene
// LayoutSceneBase::ChangeSceneWithWipe: lock the scene's cursor (its GetCursor), play the wipe's
// "Out", then change to the scene of that name ("LivingRoomScene")
inline void change_scene_with_wipe(u32 scene, u32 name, u32 wipe_type) { g::scene_LayoutSceneBase_ChangeSceneWithWipe(scene, name, wipe_type); }
const u32 NAME_LIVING_ROOM_SCENE = GA_scene_sNameLivingRoomScene;

// ---- layout::parts::Wipe: its kinds 0 = the circle, 3 = Fade_White
enum : u32 { WIPE_FADE_WHITE = 3 };
inline void wipe_in(u32 type) {
    g::layout_parts_Wipe_In(wipe(), type);
    g::layout_parts_Wipe_Play(wipe());
}

// ---- the exclusive button group (0x10 bytes): a button's push locks the group's other buttons
const u32 GROUP_SIZE = 0x10;
inline void group_ctor(u32 g) { g::layout_ExclusiveGroup_ctor(g); }
inline void group_dtor_delete(u32 g) { g::layout_ExclusiveGroup_dtor(g, 1); }
inline void group_add(u32 button, u32 g) { g::layout_ExclusiveGroup_Add(button, g); }

// ---- layout::Cursor (0x300 bytes)
const u32 CURSOR_SIZE = 0x300;
inline void cursor_ctor(u32 c) { g::layout_Cursor_ctor(c, 0); }
inline void cursor_dtor_delete(u32 c) { g::layout_Cursor_dtor(c, 1, 0, 0, 0); }
inline void cursor_update(u32 c) { g::layout_Cursor_Update(c, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); }
// the cursor's layouts, drawn last: its shadow, then the hand
inline void cursor_draw(u32 c) {
    g::layout_LayoutWrapper_Draw(c + 0x130);
    g::layout_LayoutWrapper_Draw(c + 0x8);
}

// ---- layout::parts::Bottom (0x2460 bytes): the screens' bottom-left way back
const u32 BOTTOM_SIZE = 0x2460;
const u32 BOTTOM_HOME_BTN_02 = 0x1DC0;  // the Home tab (chabudai + B): the way back to the room
enum : u32 { BOTTOM_HIDDEN = 0, BOTTOM_HOME = 4 };
inline void bottom_ctor(u32 b, u32 cursor) { g::layout_parts_Bottom_ctor(b, BOTTOM_HIDDEN, cursor, 0); }
inline void bottom_set_mode(u32 b, u32 mode, bool at_once) {
    g::layout_parts_Bottom_SetMode(b, mode, at_once, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}
// the B button: press the way back of the current mode
inline void bottom_press_back(u32 b, u32 cursor) { g::layout_parts_Bottom_PressBack(b, cursor); }
// its window virtuals: +0x8 dtor, +0xC Update, +0x10 Draw
inline void bottom_update(u32 b) { gcall<u32>(guest::load32(guest::load32(b) + 0xC), b); }
inline void bottom_draw(u32 b) { gcall<u32>(guest::load32(guest::load32(b) + 0x10), b); }
inline void bottom_dtor_delete(u32 b) { gcall<u32>(guest::load32(guest::load32(b) + 0x8), b, 1u); }

}  // namespace engine::soundplayer::game
