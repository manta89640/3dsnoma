// Menu buttons that lead only to the Wii's own settings and services, left out on the 3DS: in the
// options' top menu the video quality and the region/address settings, in the theater the Wii
// Points button; and the room's shop button, whose shop was Flash (left out: players.cpp), and
// offline its TV programmes button. Each is still built as the channel builds it (its layout
// loaded, its callbacks wired), then hidden and kept inactive, the way the channel hides a
// layout::Button of its own: the root pane of the button's layout invisible
// (nw4r_lyt_Pane_SetVisible), and the button inactive (layout_Button_SetActive, +0x5B8), which its
// hit test and its focus check ask first.
// Buttons are reached with the pointer only (layout::Cursor asks every listener's hit test; there
// is no D-pad focus and no default focus), so nothing else has to skip them.
#include <cstring>

#include "hw/ios/net.h"
#include "noma_decomp.h"

namespace g {

void layout_option_Option_ctor_orig(u32 r3, u32 r4, u32 r5);
u32 layout_option_Option_SetActive_orig(u32 r3, u32 r4);
u32 controller_theater_LayoutController_ctor_orig(u32 r3, u32 r4, u32 r5, u32 r6);
void layout_living_MoveBtn_ctor_orig(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7);

namespace {

// layout::Button (layout_Button_ctor): +0x14 its layout::Layout, whose +0x8 is the locator pane
// the button hangs from in the parent layout and +0xC the nw4r::lyt::Layout (+0x10 its root pane,
// a child of the locator). A pane's translation: +0x2C, three floats.
u32 locator(u32 button) { return guest::load32(button + 0x14 + 0x8); }
u32 root_pane(u32 button) { return guest::load32(guest::load32(button + 0x14 + 0xC) + 0x10); }

// a pane of the button's layout by its name, or 0 (nw4r::lyt::Pane::FindPaneByName on the root
// pane, recursive: the pane's virtual at +0x3C; the name goes on the guest stack)
u32 find_pane(u32 button, const char* name) {
    GFRAME(0x20);
    u32 s = (u32)&GS(u8, 0x8);
    for (u32 i = 0; i < 0x17; i++) {
        guest::store8(s + i, (u8)name[i]);
        if (!name[i])
            break;
    }
    u32 root = root_pane(button);
    return gcall<u32>(guest::load32(guest::load32(root) + 0x3C), root, s, 1u);
}

void hide(u32 button) {
    nw4r_lyt_Pane_SetVisible(root_pane(button), 0);
    layout_Button_SetActive(button, 0);
}

// the button goes where the other one's locator is
void move_to(u32 button, u32 other) {
    for (u32 i = 0; i < 3; i++)
        guest::store32(locator(button) + 0x2C + 4 * i, guest::load32(locator(other) + 0x2C + 4 * i));
}

// a locator's x
float locator_x(u32 button) {
    u32 bits = guest::load32(locator(button) + 0x2C);
    float x;
    std::memcpy(&x, &bits, 4);
    return x;
}
void set_locator_x(u32 button, float x) {
    u32 bits;
    std::memcpy(&bits, &x, 4);
    guest::store32(locator(button) + 0x2C, bits);
}

// layout::option::Option, the options' top menu: a column of four buttons (Option_Scene.lex,
// under Loc_Btn_Top), pressed as Option::ButtonType 0..3 (scene_OptionScene_StateTop_OnButton;
// scene_OptionScene_StateTop_OnClosed then goes to the state)
enum : u32 {
    OPTION_FAMILY = 0x460,   // 0: the family's Miis and profiles (StateFamily)
    OPTION_VIDEO = 0xAFC,    // 1: streaming quality (StateVideo)
    OPTION_AREA = 0x1198,    // 2: region and address (StateSetting)
    OPTION_AGREE = 0x1834,   // 3: about Wii no Ma, the terms of use
};

}  // namespace

// The options' top menu: the video quality (high or normal quality for streaming; the first
// start's setup leaves it out too) and the region/address entry (which ends the channel to open
// the Wii's own region and address settings) are left out. The family and the terms of use take
// the middle two places, so the column stays centred where the four were (the locators aren't
// animated: the menu's animations move Loc_Btn_Top, above them).
void layout_option_Option_ctor(u32 r3, u32 r4, u32 r5) {
    layout_option_Option_ctor_orig(r3, r4, r5);
    hide(r3 + OPTION_VIDEO);
    hide(r3 + OPTION_AREA);
    move_to(r3 + OPTION_FAMILY, r3 + OPTION_VIDEO);
    move_to(r3 + OPTION_AGREE, r3 + OPTION_AREA);
}

// The menu's buttons are made active and inactive together (inactive while it opens and once a
// button is pressed, active when it has opened): the two left out stay inactive.
u32 layout_option_Option_SetActive(u32 r3, u32 r4) {
    u32 r = layout_option_Option_SetActive_orig(r3, r4);
    layout_Button_SetActive(r3 + OPTION_VIDEO, 0);
    layout_Button_SetActive(r3 + OPTION_AREA, 0);
    return r;
}

// The theater's buttons (controller::theater::LayoutController, Theater_Room_Scene.lex: back to
// the room, Wii Points, the bought videos, search): the Wii Points button (+0xE64, Loc_WiiPointBtn)
// showed the Wii Points balance with a way to the Wii Shop Channel for more (message 250, then
// app_Exit_Request 16). Left out; the other buttons keep their places.
u32 controller_theater_LayoutController_ctor(u32 r3, u32 r4, u32 r5, u32 r6) {
    u32 r = controller_theater_LayoutController_ctor_orig(r3, r4, r5, r6);
    hide(r3 + 0xE64);
    return r;
}

// The room's bottom bar (layout::living, Move_Btn_Scene.lex): its buttons, in the order
// layout_living_MoveBtn_CreateButtons makes them (+0xD04, a vector of layout::Button*), are the
// theater, the shop, いろんな間, the TV programmes and the way back. The shop is left out: its
// catalogues and menus were Flash files from the server, and Flash is left out of the port (it
// only showed "can't connect"). Offline the TV programmes are too: the programme guide and the
// programmes are the server's, and the TV scene (only reached from here) has nothing to show
// (its top page says "今のおすすめはこちらです" over an empty screen, over and over). いろんな間 and
// the TV move left into the shop's place, keeping their spacing (the bar's animations move its
// buttons only up and down). The way back (Common/Wii_Btn.lex) went to the Wii Menu, and ends the
// app here: its back arrow stays, the "Wii" logo under it goes (Pic_Wii and its shadow
// Pic_Shadow; the button's animations change their colours, not whether they're shown).
void layout_living_MoveBtn_ctor(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7) {
    layout_living_MoveBtn_ctor_orig(r3, r4, r5, r6, r7);
    enum : u32 { SHOP = 1, IRONNA = 2, TV = 3, BACK = 4 };
    u32 count = guest::load32(r3 + 0xD08);
    if (count <= TV)
        return;
    u32 buttons = guest::load32(r3 + 0xD04);
    auto button = [buttons](u32 i) { return guest::load32(buttons + 4 * i); };
    hide(button(SHOP));
    if (guest::net_offline())
        hide(button(TV));
    float dx = locator_x(button(SHOP)) - locator_x(button(IRONNA));
    for (u32 i : {IRONNA, TV})
        set_locator_x(button(i), locator_x(button(i)) + dx);
    if (count <= BACK)
        return;
    for (const char* logo : {"Pic_Wii", "Pic_Shadow"})
        if (u32 pane = find_pane(button(BACK), logo))
            nw4r_lyt_Pane_SetVisible(pane, 0);
}

}  // namespace g
