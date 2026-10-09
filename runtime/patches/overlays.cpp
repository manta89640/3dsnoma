// The app's overlays (app::Application's draw: over the scene, the wipe, the system prompt, the way
// out's fade) on the 3DS's two screens.
//
// The system prompt (app::SystemPrompt: the channel's dialogs over whatever scene shows, for
// errors, saving, the first start's questions and the like) goes on the bottom screen, where the
// touch screen answers it directly: the question and its buttons together, as the room's own
// buttons are (engine/room/panel.h). It is the channel's dialog as it was (layout::parts::
// CommonDialogue's layouts: Common_Scene with Common_Window and the buttons hanging from it),
// moved into the bottom screen's region (engine/ui/screen.h BOTTOM) as the native scenes'
// questions are (engine/ui/confirm.h: centred across, lowered by Confirm::DROP, which the
// dialog's 387 units up and down need), and updated and drawn without the 16:9 squeeze (the
// bottom screen's pixels are square). Its own cursor hit-tests it as before; while it is open the
// touch points into the region (a scene that has the bottom screen already points there), and its
// hand isn't drawn.
//
// The wipe between scenes and the way out's fade cover the bottom screen too while a scene has it
// (the room, the Sound Player, the island), the same layout or quad over the region, under the
// prompt: so the bottom screen's buttons go with the picture.
//
// The Wii Remote's pointer, the hand (layout::Cursor's layouts: +0x8 the hand, +0x130 its shadow),
// isn't drawn for the touch: what is touched is pointed at directly, on the bottom screen, and
// nothing on the top screen is pointed at; the cursor still follows the touch and hit-tests as
// before. Moved by the circle pad (buttons only), it shows, as the channel shows it.
//
// app::SystemPrompt (the app's +0x2000): +0x2 u8 open, +0x10 its layout::Cursor, +0x14 the dialog
// (+0x0 set while it has its layouts, +0x108 its scene layout, a LayoutWrapper).
// app::Application: +0x330 app::Exit (+0x0 set once asked, +0x8 its app::ScreenFade), +0x1368
// the HOME Button Menu, +0x1958 the lib::SceneManager, +0x1FF8 and +0x2028 the frame's drawing
// state (+0x5C and +0x24 set before the scene draws), +0x2000 the system prompt, +0x201C the data
// notice (the channel's "new data" window: online only), +0x2020 the wipe (layout::parts::Wipe,
// +0x20 set while it shows).

#include "engine/input/remote.h"
#include "engine/room/panel.h"
#include "engine/system/host_scene.h"
#include "engine/ui/confirm.h"
#include "engine/ui/lyt.h"
#include "engine/ui/screen.h"
#include "noma_decomp.h"

namespace g {

void app_Application_Draw_orig(u32 r3);
u32 layout_Cursor_Update_orig(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7, u32 r8, u32 r9, u32 r10, f64 f1, f64 f2, f64 f3,
                              f64 f4, f64 f5, f64 f6, f64 f7, f64 f8);
u32 layout_Button_OnFocus_orig(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7, u32 r8, u32 r9, u32 r10, f64 f1, f64 f2, f64 f3,
                               f64 f4, f64 f5, f64 f6, f64 f7, f64 f8);
u32 app_SystemPrompt_Update_orig(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7, u32 r8, u32 r9, u32 r10, f64 f1, f64 f2,
                                 f64 f3, f64 f4, f64 f5, f64 f6, f64 f7, f64 f8);

namespace {

namespace lyt = engine::ui::lyt;
namespace screen = engine::ui::screen;

bool is_open(u32 prompt) { return guest::load8(prompt + 0x2) != 0; }
u32 dialog(u32 prompt) { return guest::load32(prompt + 0x14); }

// the bottom screen's region the prompt shows in: the scene's, if one has it
const screen::Region& region() { return screen::claimed() ? screen::claimed_region() : screen::BOTTOM; }

// the dialog's place on the bottom screen (its scene layout's root pane: the dialog's animations
// move the window and buttons inside it): in the middle of the region (the window and buttons
// span y 191 to -196, their middle 2.5 under the dialog's origin)
void place(u32 prompt) {
    u32 d = dialog(prompt);
    if (!d || !guest::load32(d))
        return;
    const screen::Region& r = region();
    lyt::move(lyt::root_pane(d + 0x108), r.left + r.width / 2, r.top - r.height / 2 + 2.5f);
}

bool s_claimed;  // the prompt pointed the touch into the region (no scene had the bottom screen)

// the wipe's current layout, if it shows
void draw_wipe(u32 app) {
    u32 wipe = guest::load32(app + 0x2020);
    if (guest::load32(wipe + 0x20))
        layout_LayoutWrapper_Draw(layout_parts_Wipe_GetCurrentLayout(wipe));
}

// The bottom screen's wipe: the bottom region's size, centred on the layouts' middle, where the
// wipe's circle closes (its layout's matrices are worked out in its update, so it can't be moved
// for the one draw)
constexpr screen::Region WIPE_BOTTOM{-screen::BOTTOM.width / 2, screen::BOTTOM.height / 2, screen::BOTTOM.width,
                                     screen::BOTTOM.height};

}  // namespace

u32 app_SystemPrompt_Update(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7, u32 r8, u32 r9, u32 r10, f64 f1, f64 f2, f64 f3,
                            f64 f4, f64 f5, f64 f6, f64 f7, f64 f8) {
    if (is_open(r3))
        place(r3);
    screen::squeeze(false);
    u32 r = app_SystemPrompt_Update_orig(r3, r4, r5, r6, r7, r8, r9, r10, f1, f2, f3, f4, f5, f6, f7, f8);
    screen::squeeze(true);
    const bool open = is_open(r3);
    if (open && !s_claimed && !screen::claimed()) {
        screen::claim(screen::BOTTOM);
        s_claimed = true;
    } else if (!open && s_claimed) {
        screen::release();
        s_claimed = false;
    }
    return r;
}

// the dialog on the bottom screen, without the prompt's hand (its cursor's layouts)
void app_SystemPrompt_Draw(u32 r3) {
    if (!is_open(r3) || !dialog(r3) || !guest::load32(dialog(r3)))
        return;
    place(r3);
    screen::to_bottom(region());
    screen::squeeze(false);
    lyt::layout_draw(dialog(r3) + 0x108);
    screen::squeeze(true);
    screen::to_top();
}

void app_Application_Draw(u32 r3) {
    guest::store8(guest::load32(r3 + 0x1FF8) + 0x5C, 1);
    u32 state = guest::load32(r3 + 0x2028);
    guest::store8(state + 0x24, 1);
    lib_SceneManager_PreDraw(r3 + 0x1958, state, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    // a scene of the room's that is the channel's own: whole on the bottom screen
    const bool away = engine::room::away() && !engine::system::current_host_scene();
    if (away) {
        engine::room::draw_away_top();
        screen::to_bottom(screen::FULL);
    }
    lib_SceneManager_Draw(r3 + 0x1958, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (away)
        screen::to_top();
    draw_wipe(r3);
    const bool exiting = guest::load32(r3 + 0x330) != 0;
    if (screen::claimed()) {
        screen::to_bottom(WIPE_BOTTOM);
        draw_wipe(r3);
        if (exiting)
            app_ScreenFade_Draw(r3 + 0x330 + 8);
        screen::to_top();
    }
    app_DataNotice_Draw(guest::load32(r3 + 0x201C));
    app_SystemPrompt_Draw(guest::load32(r3 + 0x2000));
    homebutton_Manager_Draw(r3 + 0x1368);
    app_Exit_Draw(r3 + 0x330);
}

// layout::Cursor: +0x288 the updates it still waits, once the pointer is back on the screen, before
// it places itself, hit-tests and so takes a press where the pointer is (10, set while the pointer
// is away: a remote's pointer settles as it comes back onto the screen). A touch is exactly where
// it is, and its A comes at its second read (engine/input/remote.cpp): inside the wait the cursor
// hasn't hit-tested where the touch is, the press is lost, and a held A doesn't come down again
// while the touch lasts. So the touch's pointer is taken at once (a scripted pointer keeps the wait).
u32 layout_Cursor_Update(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7, u32 r8, u32 r9, u32 r10, f64 f1, f64 f2, f64 f3,
                         f64 f4, f64 f5, f64 f6, f64 f7, f64 f8) {
    if (guest::input_pointer_from_touch())
        guest::store32(r3 + 0x288, 0);
    u32 r = layout_Cursor_Update_orig(r3, r4, r5, r6, r7, r8, r9, r10, f1, f2, f3, f4, f5, f6, f7, f8);
    const bool shown = guest::input_pointer_by_stick();
    for (u32 layout : {0x8u, 0x130u})
        lyt::show(lyt::root_pane(r3 + layout), shown);
    return r;
}

// layout::Button::OnFocus (the pointer came onto it): its focus sound, and its layout's Focus
// animation (layout_ButtonLayout_Focus: the button rising to meet the pointer) played from its start.
// A remote's pointer rests on a button a while before A presses it; a touch presses at its second
// read, so the Push would start over a Focus barely begun, and the button would come up, drop back
// and go down. A button focused by a touch takes its focused look at once: the Focus posed at its
// last frame, its Push to come from there.
u32 layout_Button_OnFocus(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7, u32 r8, u32 r9, u32 r10, f64 f1, f64 f2, f64 f3,
                          f64 f4, f64 f5, f64 f6, f64 f7, f64 f8) {
    u32 r = layout_Button_OnFocus_orig(r3, r4, r5, r6, r7, r8, r9, r10, f1, f2, f3, f4, f5, f6, f7, f8);
    if (guest::input_pointer_from_touch() && guest::load8(r3 + 0x5B9)) {  // (enabled: its Focus played)
        const u32 w = lyt::button_layout(r3);
        if (u32 focus = lyt::find_anim(w, "Focus"))
            lyt::pose(w, focus, lyt::last_frame(focus));
    }
    return r;
}

}  // namespace g
