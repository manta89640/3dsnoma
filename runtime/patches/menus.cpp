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
// The room's bar also gets a sixth button, the Sound Player's (engine/soundplayer), in いろんな間's
// place, いろんな間 moving into the shop's; the Sound Player's scene factory is made with the
// application's (app_SceneFactories_ctor), and SceneTransit::Go (controller_living_SceneTransit_Go) opens the Sound Player for the
// button's own To. Offline, a seventh, 出発, takes the hidden TV programmes' place at the bar's right
// end: the way to Wuhu Island (engine/wuhu), made the same way.

#include "engine/math/vecmath.h"
#include "engine/room/panel.h"
#include "engine/ui/test_card.h"
#include "engine/ui/lyt.h"
#include "engine/soundplayer/sound_player.h"
#include "engine/system/host_scene.h"
#include "engine/system/save.h"
#include "engine/wuhu/music.h"
#include "engine/wuhu/wuhu.h"
#include "hw/ios/net.h"
#include "noma_decomp.h"

namespace g {

void layout_option_Option_ctor_orig(u32 r3, u32 r4, u32 r5);
u32 layout_option_Option_SetActive_orig(u32 r3, u32 r4);
u32 scene_OptionScene_StateTop_OnButton_orig(u32 r3, u32 r4);
u32 app_SystemPrompt_OpenPending_orig(u32 r3);
u32 controller_theater_LayoutController_ctor_orig(u32 r3, u32 r4, u32 r5, u32 r6);
void layout_living_MoveBtn_ctor_orig(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7);
u32 controller_living_SceneTransit_Go_orig(u32 r3, u32 r4);
void app_SceneFactories_ctor_orig(u32 r3);

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

// layout::option::Option, the options' top menu: a column of four buttons (Option_Scene.lex,
// under Loc_Btn_Top), pressed as Option::ButtonType 0..3 (scene_OptionScene_StateTop_OnButton;
// scene_OptionScene_StateTop_OnClosed then goes to the state)
enum : u32 {
    OPTION_FAMILY = 0x460,   // 0: the family's Miis and profiles (StateFamily)
    OPTION_VIDEO = 0xAFC,    // 1: streaming quality (StateVideo)
    OPTION_AREA = 0x1198,    // 2: region and address (StateSetting): the save-data button here
    OPTION_AGREE = 0x1834,   // 3: about Wii no Ma, the terms of use
};
const u32 TYPE_SAVE_DATA = 2;  // (OPTION_AREA's ButtonType)

// the options' save-data button was pushed: the system prompt asks next
bool s_ask_erase;
// the system prompt asking it (app_SystemPrompt_OpenPending's this), for the answers
u32 s_prompt;

// A guest std::tr1::function<void()> calling `fn` (a native function's guest address): the game's
// own function_imp for a plain void(*)() (its vtable, the function at +4), which the function
// holds by pointer (+0; its small buffer, +4, unused). Whoever takes one copies it (the imp cloned
// onto the game's heap), so both may live on the guest stack.
void void_function(u32 obj, u32 imp, u32 fn) {
    guest::store32(imp, GA_std_tr1_function_imp_VoidFn_vtable);
    guest::store32(imp + 4, fn), guest::store32(imp + 8, 0);
    guest::store32(obj, imp), guest::store32(obj + 4, 0), guest::store32(obj + 8, 0);
}

// the prompt's answers (called through those functions: the imp first). Yes: the save erased when
// the app ends, which it then does as the quit prompt's yes makes it (back to the Wii Menu: the
// game's last writes, its fade out), and the app starts again (app_Exit_Dispatch, wii_parts.cpp).
// No: the prompt closes as the game's own prompts close on a no (a prompt closes only when an
// answer closes it): app_SystemPrompt_Close shuts its window, then app_SystemPrompt_OnClosed gives
// back the cursor and the music.
u32 erase_yes(u32) {
    engine::save::erase_at_exit();
    app_Exit_Request(guest::load32(GA_app_Application_sInstance) + 0x330, 4);
    return 0;
}
u32 erase_no(u32) {
    app_SystemPrompt_Close(s_prompt);
    return 0;
}

}  // namespace

// The options' top menu: the video quality (high or normal quality for streaming; the first
// start's setup leaves it out too) is left out, and the region/address entry (which ended the
// channel to open the Wii's own region and address settings) becomes the save data's: erasing it,
// after the usual question. With the family and the terms of use, the three are centred between the
// four places (the locators aren't animated: the menu's animations move Loc_Btn_Top, above them).
void layout_option_Option_ctor(u32 r3, u32 r4, u32 r5) {
    layout_option_Option_ctor_orig(r3, r4, r5);
    static const u32 order[4] = {OPTION_FAMILY, OPTION_VIDEO, OPTION_AREA, OPTION_AGREE};
    f32 at[4][3];
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < 3; i++)
            at[k][i] = engine::math::load_f32(locator(r3 + order[k]) + 0x2C + 4 * i);
    auto place = [&](u32 button, int k) {
        for (int i = 0; i < 3; i++)
            engine::math::store_f32(locator(r3 + button) + 0x2C + 4 * i, (at[k][i] + at[k + 1][i]) / 2);
    };
    hide(r3 + OPTION_VIDEO);
    place(OPTION_FAMILY, 0), place(OPTION_AREA, 1), place(OPTION_AGREE, 2);
    // its label (the button's Text_Btn, the region text's box), and no map (Pic_Area): the box,
    // which kept to the map's right, across the button's picture (Pic_Btn, its parent) less the
    // ends, so the label fits on one line, centred
    if (u32 text = find_pane(r3 + OPTION_AREA, "Text_Btn")) {
        engine::ui::lyt::text_reserve(text, 16);
        engine::ui::lyt::set_text(text, u"セーブデータの消去");
        if (u32 btn = find_pane(r3 + OPTION_AREA, "Pic_Btn")) {
            engine::ui::lyt::move(text, 0, engine::math::load_f32(text + 0x30));
            engine::ui::lyt::resize(text, engine::math::load_f32(btn + 0x4C) - 40, engine::math::load_f32(text + 0x50));
        }
    }
    if (u32 map = find_pane(r3 + OPTION_AREA, "Pic_Area"))
        nw4r_lyt_Pane_SetVisible(map, 0);
}

// The menu's buttons are made active and inactive together (inactive while it opens and once a
// button is pressed, active when it has opened): the one left out stays inactive.
u32 layout_option_Option_SetActive(u32 r3, u32 r4) {
    u32 r = layout_option_Option_SetActive_orig(r3, r4);
    layout_Button_SetActive(r3 + OPTION_VIDEO, 0);
    return r;
}

// A button of the menu pushed: the save-data button leaves the menu as it is (no state to go to)
// and asks through the system prompt (below); the others go on as the game has them.
u32 scene_OptionScene_StateTop_OnButton(u32 r3, u32 r4) {
    if (r4 != TYPE_SAVE_DATA)
        return scene_OptionScene_StateTop_OnButton_orig(r3, r4);
    s_ask_erase = true;
    return 0;
}

// The system prompt (the application's: the question over any screen, the cursor and the music
// held until it's answered) opens what is pending once a frame; the save-data button's question
// goes first, with the game's own yes and no (messages 1 and 2).
u32 app_SystemPrompt_OpenPending(u32 r3) {
    if (!s_ask_erase)
        return app_SystemPrompt_OpenPending_orig(r3);
    s_ask_erase = false;
    s_prompt = r3;
    GFRAME(0x60);
    const u32 yes_fn = (u32)&GS(u8, 0x8), no_fn = yes_fn + 0xC, yes_imp = no_fn + 0xC, no_imp = yes_imp + 0xC;
    void_function(yes_fn, yes_imp, guest::host_fn(&erase_yes));
    void_function(no_fn, no_imp, guest::host_fn(&erase_no));
    GS(u8, 0x50) = 0, GS(u8, 0x54) = 0;
    const u32 yes = lib_Message_Get((u32)&GS(u8, 0x50), 1), no = lib_Message_Get((u32)&GS(u8, 0x54), 2);
    app_SystemPrompt_Ask(r3, engine::ui::lyt::Text(u"セーブデータを消して、はじめからにします。\nよろしいですか？"), yes,
                         yes_fn, no, no_fn);
    guest::log("options: the save data's question asked");
    return 1;
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
// layout_living_MoveBtn_CreateButtons makes them (+0xD04, a vector of layout::Button*, +0xD08 how
// many), the theater, the shop, いろんな間, the TV programmes and the way back, and the その他 tab
// (+0x68C, its layout Move_Btn_Other.lex at +0x124 of it, hanging from Loc_Other_Btn), whose
// column opens when the pointer comes to it. On the 3DS the room's buttons are on the bottom
// screen, made again there from the same layouts (engine/room/panel.h); the bar is still made
// (the room's code drives it) but none of it is shown, and none of it can be reached: the room's
// cursor hit-tests the bottom screen's buttons with the touch's place in the layouts' space, which
// is where the bar's buttons are too. So each of its buttons is hidden, inactive (the bar never
// makes them active again) and moved far out of the pointer's reach (its root pane: the bar's
// animations move the locators it hangs from, the buttons' own their insides), and the その他
// tab's layout too (its tab is no layout::Button, and opens its column by itself).
void layout_living_MoveBtn_ctor(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7) {
    layout_living_MoveBtn_ctor_orig(r3, r4, r5, r6, r7);
    const f32 AWAY = 10000;
    u32 count = guest::load32(r3 + 0xD08), buttons = guest::load32(r3 + 0xD04);
    for (u32 i = 0; i < count; i++) {
        u32 button = guest::load32(buttons + 4 * i);
        hide(button);
        engine::math::store_f32(root_pane(button) + 0x2C, AWAY);
    }
    u32 other = guest::load32(guest::load32(r3 + 0x68C + 0x124 + 0xC) + 0x10);
    nw4r_lyt_Pane_SetVisible(other, 0);
    engine::math::store_f32(other + 0x2C, AWAY);
}

// The application's scene factories, made at the start (one block of them): the Sound Player's
// and the island's are made with them (engine/soundplayer, engine/wuhu: their guest objects last
// as long as the program), and the island's music puts its sequence into guest memory
// (engine/wuhu/music.cpp).
void app_SceneFactories_ctor(u32 r3) {
    app_SceneFactories_ctor_orig(r3);
    engine::soundplayer::boot();
    engine::room::boot();
    engine::ui::test_card::boot();
    engine::wuhu::boot();
    engine::wuhu::music_boot();
}

// controller::living::SceneTransit::Go(To): the scene a room button leads to, once the room has
// been wiped out. The Sound Player's button and 出発 (engine/room/panel.cpp) ask for their own To.
u32 controller_living_SceneTransit_Go(u32 r3, u32 r4) {
    engine::room::leaving(r4);
    if (r4 == engine::soundplayer::TO_SOUND_PLAYER) {
        engine::system::change_scene(engine::soundplayer::scene_factory());
        return 0;
    }
    if (r4 == engine::wuhu::TO_ISLAND) {
        engine::system::change_scene(engine::wuhu::scene_factory());
        return 0;
    }
    return controller_living_SceneTransit_Go_orig(r3, r4);
}

}  // namespace g
