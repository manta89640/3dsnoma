// A short menu in the channel's own window: two to four choices, one above the other, in the look
// of the channel's dialogs (layout::parts::CommonDialogue, as the room's quit dialog shows it),
// built natively from the same layouts as engine/ui/confirm: Common/Common_Scene.lex (the scene),
// Common/Common_Window.lex hanging at its Loc_Window (the white veil over the screen and the
// window, its text box left out, the window made to fit the choices), and one
// Common/Common_LargeBtn.lex for each choice, made wider and stacked in the window. A choice the
// scene can't offer now is greyed (the button's own Disable look) and can't be chosen.
//
// Its ways are the dialogs': opening plays the window's FadeIn and the channel's window sound
// (WAV_SE_008_WINDOW_OPEN_B); a choice's push ends in the choice (take_choice()), the scene then
// closes the menu or puts another window in its place. With the pointer away from the screen, A
// pushes the highlighted choice and B the cancelling one (the one that just closes the menu, its
// push the dialogs' refusal sound, WAV_SE_031_CLICK_GENERAL_NO), their badges shown as the
// channel's dialogs show them (lyt::mark_a / mark_b); the D-pad's up and down move the highlight
// (its focused look, the focus sound). B is the cancelling choice with the pointer on the screen
// too, as in engine/ui/confirm.
//
// Another window can take its place (page_out: the window goes with its PageOut, the veil staying
// for the window over it or handed over to that window's own) and give it back (page_in).
// All its guest objects are made with it (hidden until opened) and deleted with it.
#pragma once

#include <string>
#include <vector>

#include "runtime/runtime.h"

namespace engine::ui {

class Menu {
public:
    // cursor: the scene's layout::Cursor; group: the scene's exclusive button group; choices: their
    // texts, top to bottom (2 to 4); cancel: the choice that only closes the menu (B's)
    Menu(u32 cursor, u32 group, const std::vector<std::u16string>& choices, int cancel);
    ~Menu();
    Menu(const Menu&) = delete;
    Menu& operator=(const Menu&) = delete;
    // the whole of it (window, buttons, veil) moved across by x layout units (a screen region not
    // centred on the layouts' middle: engine/ui/screen.h)
    void place(f32 x);

    // a choice offered or greyed (while the menu is hidden)
    void set_enabled(int choice, bool on);
    // the menu opens (FadeIn, the veil with it), the highlight on the cancelling choice
    void open();
    // it fades out, the veil with it
    void close();
    // Another window takes its place: the window goes (PageOut), the veil stays for the window
    // over it (keep_veil) or goes at once (that window brings its own, as a confirm opened with
    // Confirm::open(true) does); and comes back (PageIn, with its veil).
    void page_out(bool keep_veil);
    void page_in();
    // the cancelling choice pushed, as B would (START closing the menu)
    void press_cancel();
    // shown: opening, open, paged out or fading out
    bool shown() const { return state_ != State::HIDDEN; }
    // open: its choices can be pushed
    bool usable() const { return state_ == State::OPEN; }
    // once a frame (after the scene's own parts, before its cursor's update)
    void update();
    // its layouts (over the scene's other 2D parts, under the cursor)
    void draw();
    // A button of the scene's was pushed (its push animation ended): the menu's? (then that is
    // the choice)
    bool on_pushed(u32 button);
    // the choice made since the last call, or -1
    int take_choice();

private:
    enum class State { HIDDEN, OPEN, PAGED, CLOSING };
    struct Choice {
        u32 button = 0;
        u32 focus = 0;       // its Focus animation
        bool enabled = true;
        bool posed = false;  // posed in its focused look (the D-pad's highlight)
    };
    void show_choices(bool on);
    void mark();
    void unmark_all();
    void set_highlight(int i);
    void input();
    void pose_highlight();
    bool any_focused() const;

    u32 cursor_;
    u32 scene_ = 0, window_ = 0;  // Common_Scene (a layout of its own), Common_Window hanging from it
    u32 veil_ = 0, frame_ = 0;    // the window's Pic_Bg and Wind_01
    u32 fade_in_ = 0, fade_out_ = 0, page_in_ = 0, page_out_ = 0;
    std::vector<Choice> choices_;
    int cancel_ = 0;
    int highlight_ = 0;
    int marked_a_ = -1, marked_b_ = -1;  // the choices carrying the A and B marks (-1: none)
    bool pad_mode_ = false;  // the D-pad has been used since the pointer last moved
    f32 pointer_x_ = 0, pointer_y_ = 0;
    State state_ = State::HIDDEN;
    int choice_ = -1;
};

}  // namespace engine::ui
