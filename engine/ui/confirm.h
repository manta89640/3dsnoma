// A yes-or-no question in the channel's own window: the look and the ways of the channel's dialogs
// (layout::parts::CommonDialogue, as the room's quit dialog shows it), built natively from the
// same layouts: Common/Common_Scene.lex (the scene: a locator for the window and two for the
// small buttons), Common/Common_Window.lex attached at its Loc_Window (a white veil over the
// screen, the window, its text), and two Common/Common_SmallBtn.lex buttons, いいえ at
// Loc_SmallBtn_01 on the left and はい at Loc_SmallBtn_02 on the right (the channel's messages 2 and
// 1, in its language). Opening plays the window's FadeIn and the channel's window sound
// (WAV_SE_008_WINDOW_OPEN_B); いいえ gets the dialogs' refusal sound (WAV_SE_031_CLICK_GENERAL_NO).
// With the pointer away from the screen, the remote's A pushes はい and B いいえ, their badges shown,
// as in the channel's dialogs (lyt::mark_a / mark_b); B is いいえ with the pointer on the screen too
// (the scenes open the window with B: B again closes it). A choice takes the buttons away and plays
// the window's FadeOut.
// It can also open over another window of the same look (engine/ui/menu's, paged out for it): it
// then comes in with its PageIn over the veil the other leaves it, and いいえ, going back to that
// window, leaves with its PageOut, handing the veil back at once (はい still fades it all out).
// All its guest objects are made with it (hidden until opened) and deleted with it.
#pragma once

#include <string>

#include "runtime/runtime.h"

namespace engine::ui {

class Confirm {
public:
    enum class Choice { NONE, YES, NO };

    // cursor: the scene's layout::Cursor; group: the scene's exclusive button group; text: the
    // question (two lines at most, "\n" between them)
    Confirm(u32 cursor, u32 group, const std::u16string& text);
    ~Confirm();
    Confirm(const Confirm&) = delete;
    Confirm& operator=(const Confirm&) = delete;

    // the window opens (nothing if it is shown already); over_window: in the place of another
    // window, whose veil it takes over (see above)
    void open(bool over_window = false);
    // shown: opening, open, or fading out after a choice
    bool shown() const { return state_ != State::HIDDEN; }
    // once a frame (after the scene's own parts, before its cursor's update)
    void update();
    // its layouts (over the scene's other 2D parts, under the cursor)
    void draw();
    // A button of the scene's was pushed (its push animation ended): the window's? (then it
    // fades out, and take_choice() gives the answer)
    bool on_pushed(u32 button);
    // the choice made since the last call (NONE: none)
    Choice take_choice();

private:
    enum class State { HIDDEN, OPEN, CLOSING };
    void close();

    u32 cursor_;
    u32 scene_ = 0, window_ = 0;  // Common_Scene (a layout of its own), Common_Window hanging from it
    u32 no_ = 0, yes_ = 0;        // the buttons
    u32 veil_ = 0;                // the window's Pic_Bg
    u32 window_in_ = 0, window_out_ = 0, page_in_ = 0, page_out_ = 0;
    bool over_window_ = false;
    State state_ = State::HIDDEN;
    Choice choice_ = Choice::NONE;
};

}  // namespace engine::ui
