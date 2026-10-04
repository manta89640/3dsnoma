// A yes-or-no question in the channel's own window (see confirm.h).
#include "engine/ui/confirm.h"

#include "engine/system/host_scene.h"
#include "engine/ui/game.h"
#include "engine/ui/lyt.h"

namespace engine::ui {

namespace {

const char* const SCENE_LEX = "layout/Common/Common_Scene.lex";
const char* const WINDOW_LEX = "layout/Common/Common_Window.lex";
const char* const BUTTON_LEX = "layout/Common/Common_SmallBtn.lex";

// the channel's messages for the buttons
const u32 MSG_YES = 1, MSG_NO = 2;

// the question's text box (Text_Text_01: 435 x 226 in the window's upper part, made for eight lines
// of 20 characters) holds this many characters
const u32 TEXT_CHARS = 64;

// A button shown, its FadeIn played on its own layout (the button plays its Focus and Push there
// too), or hidden at once: the channel's dialogs take their buttons away as the window starts
// fading out (CommonDialogue detaches them; their FadeOut doesn't fade them).
void show_button(u32 button, bool on) {
    lyt::show(lyt::button_root(button), on);
    if (on) {
        u32 layout = lyt::button_layout(button);
        lyt::play(layout, lyt::find_anim(layout, "FadeIn"));
    }
}

}  // namespace

Confirm::Confirm(u32 cursor, u32 group, const std::u16string& text) : cursor_(cursor) {
    scene_ = lyt::layout_new(SCENE_LEX);
    window_ = lyt::layout_new_in(WINDOW_LEX, scene_, "Loc_Window");
    no_ = lyt::button_new(BUTTON_LEX, scene_, "Loc_SmallBtn_01", cursor);
    yes_ = lyt::button_new(BUTTON_LEX, scene_, "Loc_SmallBtn_02", cursor);

    // The question, its lines centred as a block in the middle of the text box (as the channel's
    // dialogs set it: CommonDialogue's fn_801554B4 makes the box's vertical place the middle).
    u32 box = lyt::find_pane(window_, "Text_Text_01");
    lyt::text_reserve(box, TEXT_CHARS);
    lyt::set_text(box, text);
    lyt::text_position(box, lyt::TEXT_CENTRE);

    lyt::set_text_at(lyt::find_pane(lyt::button_layout(no_), "Text_Btn"), lyt::message(MSG_NO));
    lyt::set_text_at(lyt::find_pane(lyt::button_layout(yes_), "Text_Btn"), lyt::message(MSG_YES));
    lyt::set_sounds(no_, lyt::SE_OVER_M, lyt::SE_CLICK_GENERAL_NO);
    for (u32 b : {no_, yes_}) {
        engine::system::hook_button(b);
        game::group_add(b, group);
        lyt::set_active(b, false);
    }

    window_in_ = lyt::find_anim(window_, "FadeIn");
    window_out_ = lyt::find_anim(window_, "FadeOut");
    lyt::show(lyt::root_pane(scene_), false);
}

Confirm::~Confirm() {
    lyt::unmark(yes_, cursor_);
    lyt::unmark(no_, cursor_);
    lyt::button_delete(yes_);
    lyt::button_delete(no_);
    lyt::layout_delete(window_);
    lyt::layout_delete(scene_);
}

void Confirm::open() {
    if (state_ != State::HIDDEN)
        return;
    state_ = State::OPEN;
    choice_ = Choice::NONE;
    lyt::show(lyt::root_pane(scene_), true);
    lyt::play(window_, window_in_);
    for (u32 b : {no_, yes_}) {
        show_button(b, true);
        lyt::set_active(b, true);
    }
    lyt::mark_a(yes_, cursor_);
    lyt::mark_b(no_, cursor_);
    lyt::play_sound(yes_, lyt::SE_LABEL_WINDOW_OPEN_B);
}

void Confirm::close() {
    state_ = State::CLOSING;
    lyt::play(window_, window_out_);
    for (u32 b : {no_, yes_}) {
        lyt::unmark(b, cursor_);
        lyt::set_active(b, false);
        show_button(b, false);
    }
}

void Confirm::update() {
    // B is いいえ with the pointer on the screen too (the cursor pushes the B-marked button only
    // while the pointer is away): pushed as the cursor would, its animation and sound
    if (state_ == State::OPEN && (game::buttons_triggered() & game::PAD_B) && game::pointer_on_screen() &&
        !lyt::group_busy(no_))
        game::cursor_press(cursor_, no_);
    if (state_ == State::CLOSING && !lyt::animating(window_)) {
        state_ = State::HIDDEN;
        lyt::show(lyt::root_pane(scene_), false);
    }
    lyt::layout_update(scene_);
}

void Confirm::draw() {
    if (state_ != State::HIDDEN)
        lyt::layout_draw(scene_);
}

bool Confirm::on_pushed(u32 button) {
    if (button != yes_ && button != no_)
        return false;
    if (state_ == State::OPEN) {
        choice_ = button == yes_ ? Choice::YES : Choice::NO;
        close();
    }
    return true;
}

Confirm::Choice Confirm::take_choice() {
    Choice c = choice_;
    choice_ = Choice::NONE;
    return c;
}

}  // namespace engine::ui
