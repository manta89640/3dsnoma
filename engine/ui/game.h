// The channel's own functions and objects the native scenes use (the Sound Player, Wuhu Island),
// with their arguments spelt out (the decompiled functions take the register ABI's full argument
// list). engine/ui holds what those scenes share: these, the layouts (lyt.h), the run-time pictures
// (art.h) and the 3D pieces (g3d.h), first written for the Sound Player.
#pragma once

#include "engine/math/vecmath.h"
#include "noma_decomp.h"
#include "runtime/runtime.h"

namespace engine::ui::game {

// ---- globals
inline u32 wipe() { return guest::load32(GA_layout_parts_Wipe_sInstance); }        // layout::parts::Wipe
inline u32 controller() { return guest::load32(GA_input_Controller_sInstance); }  // the input manager

// the controller's KPAD status: +0xB40 the buttons held, +0xB44 those newly pressed this frame
// (KPAD bits: B 0x400, A 0x800, 1 0x200, 2 0x100, D-pad LEFT 0x1, RIGHT 0x2, DOWN 0x4, UP 0x8; the
// 3DS's X and Y are 1 and 2), +0xB60 the pointer's
// position (two f32), +0xB9E whether the pointer is on the screen
inline u32 buttons_triggered() { return guest::load32(controller() + 0xB44); }
inline u32 buttons_held() { return guest::load32(controller() + 0xB40); }
enum : u32 {
    PAD_LEFT = 0x1, PAD_RIGHT = 0x2, PAD_DOWN = 0x4, PAD_UP = 0x8, PAD_PLUS = 0x10,
    PAD_TWO = 0x100, PAD_ONE = 0x200, PAD_B = 0x400, PAD_A = 0x800, PAD_MINUS = 0x1000,
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

// ---- layout::parts::Wipe: its kinds 0 = the circle (the room bar's way into a screen, ending
// white), 2 = Fade_Black (a black fade, 16 frames each way), 3 = Fade_White (11 frames), 4 =
// Fade_White_Long (21 frames: the room's fade in, coming back from its screens). Its
// layouts, one a kind, are at +0x0 + 4 * kind; +0x10C is the kind playing, +0x20 the animation
// played ("In" or "Out", set by In and Out, cleared by its reset): while it is set, the
// application draws the kind's layout over the frame, after the scene (posed where the animation
// is, or where it ended).
enum : u32 { WIPE_CIRCLE = 0, WIPE_FADE_BLACK = 2, WIPE_FADE_WHITE = 3, WIPE_FADE_WHITE_LONG = 4 };
inline void wipe_in(u32 type) {
    g::layout_parts_Wipe_In(wipe(), type);
    g::layout_parts_Wipe_Play(wipe());
}
// Wipe::Out(kind) (layout_parts_Wipe_Out; runtime/patches/wipes.cpp only changes it on the room's way to the
// island) and Play: the kind's Out from now, which the application draws over the frame, then its
// last pose (Fade_Black: black) until the next In. A scene's own fade to black and back, within
// the scene (wipe_out, then wipe_in once the Out has ended).
inline void wipe_out(u32 type) {
    g::layout_parts_Wipe_Out(wipe(), type);
    g::layout_parts_Wipe_Play(wipe());
}
inline u32 wipe_layout(u32 kind) { return guest::load32(wipe() + 4 * kind); }
// The application stops drawing the wipe over the frame (+0x20 cleared; its layouts keep their
// poses): a scene that comes in under a wipe that has ended (a black screen) draws that layout
// itself, under parts of its own (the loading rings), until its wipe_in starts the wipe again.
inline void wipe_hand_over() { guest::store32(wipe() + 0x20, 0); }

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
// a button the cursor knows pushed as if the pointer had pushed it (layout_Cursor_Press: its push
// animation and sound, then its callbacks); false if the cursor didn't take it (the button isn't
// one of its own, or the cursor is locked)
inline bool cursor_press(u32 c, u32 button) { return g::layout_Cursor_Press(c, button, 0) != 0; }
// the cursor's layouts, drawn last: its shadow, then the hand
inline void cursor_draw(u32 c) {
    g::layout_LayoutWrapper_Draw(c + 0x130);
    g::layout_LayoutWrapper_Draw(c + 0x8);
}

// ---- layout::parts::Bottom (0x2460 bytes): the screens' bottom-left way back
const u32 BOTTOM_SIZE = 0x2460;
const u32 BOTTOM_HOME_BTN_02 = 0x1DC0;  // the Home tab (chabudai + B): the way back to the room
// the back tab (Common/Return_Btn_03.lex: the return arrow + B, the Home tab's shape): the way back
// to the screen before
const u32 BOTTOM_RETURN_BTN_03 = 0x1088;
// its modes (layout_parts_Bottom_SetMode): 0 nothing, 2 the back tab, 4 the Home tab (both at the
// bottom left; PressBack presses either)
enum : u32 { BOTTOM_HIDDEN = 0, BOTTOM_RETURN = 2, BOTTOM_HOME = 4 };
inline void bottom_ctor(u32 b, u32 cursor) { g::layout_parts_Bottom_ctor(b, BOTTOM_HIDDEN, cursor, 0); }
inline void bottom_set_mode(u32 b, u32 mode, bool at_once) {
    g::layout_parts_Bottom_SetMode(b, mode, at_once, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}
// the B button: press the way back of the current mode (the cursor presses it, layout_Cursor_Press: its
// push animation and sound, then its pushed hook); false if the cursor didn't take the press
inline bool bottom_press_back(u32 b, u32 cursor) { return g::layout_parts_Bottom_PressBack(b, cursor) != 0; }
// its window virtuals: +0x8 dtor, +0xC Update, +0x10 Draw
inline void bottom_update(u32 b) { gcall<u32>(guest::load32(guest::load32(b) + 0xC), b); }
inline void bottom_draw(u32 b) { gcall<u32>(guest::load32(guest::load32(b) + 0x10), b); }
inline void bottom_dtor_delete(u32 b) { gcall<u32>(guest::load32(guest::load32(b) + 0x8), b, 1u); }

}  // namespace engine::ui::game
