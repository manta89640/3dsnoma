// A short menu in the channel's own window (see menu.h).
#include "engine/ui/menu.h"

#include <algorithm>
#include <cmath>

#include "engine/system/host_scene.h"
#include "engine/ui/game.h"
#include "engine/ui/lyt.h"

namespace engine::ui {

using namespace lyt;

namespace {

const char* const SCENE_LEX = "layout/Common/Common_Scene.lex";
const char* const WINDOW_LEX = "layout/Common/Common_Window.lex";
const char* const BUTTON_LEX = "layout/Common/Common_LargeBtn.lex";

// Geometry, in the dialog's own layout units (608 x 456, the origin at the centre, y up; drawn
// under Common_Scene's location-adjusted Loc_Top, as the confirm's window and buttons are). The
// buttons are Common_LargeBtn's capsule (230 x 46) made wider, so that the longest choice and the
// A badge beside it (at Loc_Push_Mark, 22 in from the right end) don't meet; the window fits the
// column with a margin about the buttons' focused size (they grow 1.2 times).
const f32 MENU_Y = 16;          // the column's centre: a little above the screen's, as the dialog's window
const f32 PITCH = 70;           // from one button to the next
const f32 BUTTON_W = 280, BUTTON_H = 46;
const f32 SHADOW_W = BUTTON_W + 10, SHADOW_H = 56;  // Wind_Shadow, as the layout has it about Wind_Btn
const f32 BOUND_W = BUTTON_W + 20, BOUND_H = 56;    // the hit area (under the pitch: no two touch)
const f32 MARK_INSET = 22;
const f32 WINDOW_W = 400, MARGIN = 36;
const u32 TEXT_CHARS = 16;

// the scene's Loc_Window: the window hangs there, the buttons after it (drawn over it)
const char* const LOCATOR = "Loc_Window";

}  // namespace

Menu::Menu(u32 cursor, u32 group, const std::vector<std::u16string>& choices, int cancel)
    : cursor_(cursor), cancel_(cancel) {
    scene_ = layout_new(SCENE_LEX);
    window_ = layout_new_in(WINDOW_LEX, scene_, LOCATOR);
    show(find_pane(window_, "Text_Text_01"), false);
    veil_ = find_pane(window_, "Pic_Bg");
    frame_ = find_pane(window_, "Wind_01");
    const int n = (int)choices.size();
    resize(frame_, WINDOW_W, (f32)(n - 1) * PITCH + BUTTON_H + 2 * MARGIN);
    move(frame_, 0, MENU_Y);

    choices_.resize(choices.size());
    for (int i = 0; i < n; i++) {
        Choice& c = choices_[i];
        c.button = button_new(BUTTON_LEX, scene_, LOCATOR, cursor);
        u32 layout = button_layout(c.button);
        move(button_root(c.button), 0, MENU_Y + ((f32)(n - 1) / 2 - (f32)i) * PITCH);
        resize(find_pane(layout, "Wind_Btn"), BUTTON_W, BUTTON_H);
        resize(find_pane(layout, "Wind_Shadow"), SHADOW_W, SHADOW_H);
        resize(find_pane(layout, "Bound_Btn"), BOUND_W, BOUND_H);
        move(find_pane(layout, "Loc_Push_Mark"), BUTTON_W / 2 - MARK_INSET, 0);
        u32 text = find_pane(layout, "Text_Btn");
        text_reserve(text, TEXT_CHARS);
        set_text(text, choices[i]);
        set_sounds(c.button, SE_OVER_M, i == cancel ? SE_CLICK_GENERAL_NO : SE_CLICK_GENERAL);
        c.focus = find_anim(layout, "Focus");
        engine::system::hook_button(c.button);
        game::group_add(c.button, group);
        set_active(c.button, false);
    }

    fade_in_ = find_anim(window_, "FadeIn");
    fade_out_ = find_anim(window_, "FadeOut");
    page_in_ = find_anim(window_, "PageIn");
    page_out_ = find_anim(window_, "PageOut");
    show(root_pane(scene_), false);
}

Menu::~Menu() {
    unmark_all();
    for (auto it = choices_.rbegin(); it != choices_.rend(); ++it)
        button_delete(it->button);
    layout_delete(window_);
    layout_delete(scene_);
}

void Menu::set_enabled(int choice, bool on) {
    if (state_ == State::HIDDEN && choice >= 0 && choice < (int)choices_.size())
        choices_[choice].enabled = on;
}

// The buttons shown, with their FadeIn (a greyed one with its Disable_FadeIn) played on their own
// layouts, or taken away at once: the channel's dialogs take their buttons away as the window
// starts going (CommonDialogue detaches them).
void Menu::show_choices(bool on) {
    for (Choice& c : choices_) {
        show(button_root(c.button), on);
        if (on) {
            lyt::set_enabled(c.button, c.enabled);
            u32 layout = button_layout(c.button);
            play(layout, find_anim(layout, c.enabled ? "FadeIn" : "Disable_FadeIn"));
            c.posed = false;
        }
        set_active(c.button, on && c.enabled);
    }
}

void Menu::open() {
    if (state_ != State::HIDDEN)
        return;
    state_ = State::OPEN;
    choice_ = -1;
    pad_mode_ = false;
    highlight_ = cancel_;
    show(root_pane(scene_), true);
    show(veil_, true);
    show(frame_, true);
    play(window_, fade_in_);
    show_choices(true);
    mark();
    play_sound(choices_[0].button, SE_LABEL_WINDOW_OPEN_B);
}

void Menu::close() {
    if (state_ == State::HIDDEN || state_ == State::CLOSING)
        return;
    // paged out, the window has gone already: its FadeOut mustn't bring it back (the veil, if it
    // stayed, fades with it)
    if (state_ == State::PAGED)
        show(frame_, false);
    state_ = State::CLOSING;
    unmark_all();
    show_choices(false);
    play(window_, fade_out_);
}

void Menu::page_out(bool keep_veil) {
    if (state_ != State::OPEN)
        return;
    state_ = State::PAGED;
    unmark_all();
    show_choices(false);
    play(window_, page_out_);
    if (!keep_veil)
        show(veil_, false);
}

void Menu::page_in() {
    if (state_ != State::PAGED)
        return;
    state_ = State::OPEN;
    choice_ = -1;
    show(veil_, true);  // PageIn holds it as it is when open
    show(frame_, true);
    play(window_, page_in_);
    show_choices(true);
    mark();
}

void Menu::press_cancel() {
    u32 b = choices_[cancel_].button;
    if (state_ == State::OPEN && !group_busy(b))
        game::cursor_press(cursor_, b);
}

// The A mark on the highlighted choice, the B mark on the cancelling one when that is another:
// the cursor pushes them with A and B while the pointer is away, and shows their badges then. A
// button's marks go together (fn_80138888), so only the buttons whose marks change are touched.
void Menu::mark() {
    int want_a = state_ == State::OPEN ? highlight_ : -1;
    int want_b = state_ == State::OPEN && cancel_ != highlight_ ? cancel_ : -1;
    if (want_a == marked_a_ && want_b == marked_b_)
        return;
    for (int i = 0; i < (int)choices_.size(); i++) {
        bool changes = (i == marked_a_) != (i == want_a) || (i == marked_b_) != (i == want_b);
        if (!changes)
            continue;
        u32 b = choices_[i].button;
        if (i == marked_a_ || i == marked_b_)
            unmark(b, cursor_);
        if (i == want_a)
            mark_a(b, cursor_);
        if (i == want_b)
            mark_b(b, cursor_);
    }
    marked_a_ = want_a;
    marked_b_ = want_b;
}

void Menu::unmark_all() {
    for (int i : {marked_a_, marked_b_})
        if (i >= 0)
            unmark(choices_[i].button, cursor_);
    marked_a_ = marked_b_ = -1;
}

void Menu::set_highlight(int i) {
    if (i == highlight_)
        return;
    highlight_ = i;
    mark();
}

bool Menu::any_focused() const {
    for (const Choice& c : choices_)
        if (focused(c.button))
            return true;
    return false;
}

// the D-pad, and A and B with the pointer on the screen (with it away, the cursor pushes the
// marked buttons itself; but B's mark is on no button while the highlight, carrying A's, is on the
// cancelling choice: B is pushed here then)
void Menu::input() {
    f32 px, py;
    game::pointer_at(px, py);
    if (game::pointer_on_screen() && (std::fabs(px - pointer_x_) > 0.002f || std::fabs(py - pointer_y_) > 0.002f))
        pad_mode_ = false;
    pointer_x_ = px;
    pointer_y_ = py;
    // the pointer on a choice: the highlight is there
    for (int i = 0; i < (int)choices_.size(); i++)
        if (focused(choices_[i].button))
            set_highlight(i);
    if (group_busy(choices_[0].button))
        return;

    const u32 pressed = game::buttons_triggered();
    if (pressed & (game::PAD_UP | game::PAD_DOWN)) {
        const int dir = (pressed & game::PAD_UP) ? -1 : 1;
        int i = highlight_ + dir;
        while (i >= 0 && i < (int)choices_.size() && !choices_[i].enabled)
            i += dir;
        if (i >= 0 && i < (int)choices_.size()) {
            set_highlight(i);
            play_sound(choices_[i].button, sound_label(SE_OVER_M));
        }
        pad_mode_ = true;
    }
    const bool pointer = game::pointer_on_screen();
    if (pointer && (pressed & game::PAD_A) && pad_mode_ && !any_focused())
        game::cursor_press(cursor_, choices_[highlight_].button);
    else if ((pressed & game::PAD_B) && (pointer || marked_b_ != cancel_))
        game::cursor_press(cursor_, choices_[cancel_].button);
}

// The D-pad's highlight shows (as the button's focused look) while the pointer is away from the
// screen, or since the D-pad was last used; a button the pointer is on shows its own focus. A
// button's own animation (its FadeIn, its focus going) is left to end first.
void Menu::pose_highlight() {
    bool on = state_ == State::OPEN && (!game::pointer_on_screen() || pad_mode_);
    for (int i = 0; i < (int)choices_.size(); i++) {
        Choice& c = choices_[i];
        if (!c.enabled)
            continue;
        if (focused(c.button)) {
            c.posed = false;
            continue;
        }
        bool want = on && i == highlight_;
        u32 layout = button_layout(c.button);
        if (want != c.posed && !animating(layout)) {
            pose(layout, c.focus, want ? last_frame(c.focus) : 0);
            c.posed = want;
        }
    }
}

void Menu::update() {
    if (state_ == State::HIDDEN)
        return;
    if (state_ == State::CLOSING && !animating(window_)) {
        state_ = State::HIDDEN;
        show(root_pane(scene_), false);
        return;
    }
    if (state_ == State::OPEN)
        input();
    pose_highlight();
    layout_update(scene_);
}

void Menu::draw() {
    if (state_ != State::HIDDEN)
        layout_draw(scene_);
}

bool Menu::on_pushed(u32 button) {
    for (int i = 0; i < (int)choices_.size(); i++) {
        if (choices_[i].button != button)
            continue;
        if (state_ == State::OPEN && choice_ < 0) {
            choice_ = i;
            // one choice: the others can't be pushed while the scene acts on it
            for (Choice& c : choices_)
                set_active(c.button, false);
        }
        return true;
    }
    return false;
}

int Menu::take_choice() {
    int c = choice_;
    choice_ = -1;
    return c;
}

}  // namespace engine::ui
