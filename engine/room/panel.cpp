// The room's bottom screen (see panel.h).
#include "engine/room/panel.h"

#include <vector>

#include "engine/input/remote.h"
#include "engine/soundplayer/sound_player.h"
#include "engine/system/host_scene.h"
#include "engine/ui/drag_scroll.h"
#include "engine/ui/game.h"
#include "engine/ui/lyt.h"
#include "engine/ui/screen.h"
#include "engine/ui/test_card.h"
#include "engine/wuhu/bar_button.h"
#include "engine/wuhu/wuhu.h"
#include "noma_decomp.h"

namespace engine::room {

namespace game = engine::ui::game;
namespace lyt = engine::ui::lyt;
namespace screen = engine::ui::screen;

namespace {

// The bottom screen's part of the layouts' space (engine/ui/screen.h BOTTOM: the Sound Player's
// and the island's). The bar's places in Move_Btn_Scene (y -162, the way back at x -267 to the TV
// programmes' end cap at 246) are inside it as they are inside the TV picture. The way back (the
// Wii button, to the Wii Menu) is left out: the 3DS's HOME button closes the app. The bar's other
// places move left by its width (BAR_SHIFT), so that ホームシアター starts at the screen's left edge
// as the Wii button did, and the end cap ends at its right edge.
const screen::Region& BOTTOM_REGION = screen::BOTTOM;
const f32 MIDDLE_X = screen::BOTTOM_MIDDLE_X;

const char* const SCENE_LEX = "layout/Move_Btn/Move_Btn_Scene.lex";

// controller::living::SceneTransit::To: the scenes the room's buttons go to
enum : u32 {
    TO_THEATER = 0,
    TO_MESSAGE_BOARD = 5,  // scene::MessageInBox
    TO_MESSAGE_SEND = 6,
    TO_OPTIONS = 7,
    TO_HELP = 8,  // scene::TutorialSelect, 使い方
};

enum class Action { NONE, GO };

// The bar, at its own locators. サウンド and 出発 take いろんな間's layout, as the room's bar made
// them (engine/ui/bar_button.h dresses them); the TV programmes' end cap is only there for the
// bar's right end (inactive, without its icon and label).
struct BarButton {
    const char* lex;
    const char* locator;
    Action action;
    u32 to;
    void (*dress)(u32 button);
};
void dress_end_cap(u32 button);
const BarButton BAR[] = {
    {"layout/Move_Btn/Move_Btn_Theater_Btn.lex", "Loc_Theater_Btn", Action::GO, TO_THEATER, nullptr},
    {"layout/Move_Btn/Move_Btn_Ironna_Btn.lex", "Loc_shopping_Btn", Action::GO, engine::soundplayer::TO_SOUND_PLAYER,
     &engine::soundplayer::dress_bar_button},
    {"layout/Move_Btn/Move_Btn_Ironna_Btn.lex", "Loc_ironna_Btn", Action::GO, engine::wuhu::TO_ISLAND,
     &engine::wuhu::dress_bar_button},
    {"layout/Move_Btn/Move_Btn_Tv_Btn.lex", "Loc_Tv_Btn", Action::NONE, 0, &dress_end_cap},
};

// The その他 column's buttons (their layouts' faces are 166 wide, 86 or 64 high, 37 right of the
// layout's origin), two by two above the bar in the order the column has them, a fifth bigger than
// the column's (their pictures of text read at the bar's size then): in the middle of the region
// across, and between its top and the bar's tops (y -118, less a margin for their rise when
// touched) up and down.
struct ItemButton {
    const char* lex;
    u32 to;
};
const ItemButton ITEMS[] = {
    {"layout/Move_Btn/Move_Btn_Message_Btn.lex", TO_MESSAGE_BOARD},
    {"layout/Move_Btn/Move_Btn_Message_Send_Btn.lex", TO_MESSAGE_SEND},
    {"layout/Move_Btn/Move_Btn_Help_Btn.lex", TO_HELP},
    {"layout/Move_Btn/Move_Btn_Option_Btn.lex", TO_OPTIONS},
};
const f32 ITEM_SCALE = 1.2f;
const f32 ITEM_FACE_X = 37 * ITEM_SCALE;              // the face's middle, right of the button's origin
const f32 ITEM_W = 166 * ITEM_SCALE, ITEM_GAP = 16;   // across
const f32 ITEM_H[2] = {86 * ITEM_SCALE, 64 * ITEM_SCALE}, ROW_GAP = 14;  // the rows' heights
const f32 ITEMS_TOP = 172, ITEMS_BOTTOM = -106;       // the space for them
// (the buttons hang from the scene's Loc_Btn, at the layouts' origin)
const char* const ITEM_LOCATOR = "Loc_Btn";
// the Wii button's width with the gap after it (its face from x -301.5, ホームシアター's from -214)
const f32 BAR_SHIFT = -88;

// When the panel comes in. The room shows its bar a while after it is made (its intro: the bar's
// Move_Btn_Scene FadeIn, which slides each button up and fades it in, the way back first, over
// 50 frames); until then the bottom screen is black. When the room's own bar (hidden, far away:
// runtime/patches/menus.cpp) starts its FadeIn, its way back's locator going from transparent to
// seen, the panel plays its own and the black fades away over FADE_FROM_BLACK; its buttons take
// touches once the FadeIn has played. Should the room's bar never be seen to start (made in a way
// not met here), the panel comes in after FADE_FALLBACK. (60ths of a second, the platform's time.)
const f32 FADE_FROM_BLACK = 20, FADE_IN_FRAMES = 50, FADE_FALLBACK = 240;
const char* const FADE_IN = "FadeIn";
u32 s_bar_locator;  // the room's own bar's Loc_Back_Btn (set_bar_locator), or 0
// app::ScreenFade (app_ScreenFade_Draw: a quad over the screen in a colour): +2 u16 its length in
// frames (0: no fading, the colour's full), +4 u16 the frame, +6 the colour (RGBA): black here, its
// alpha frame / length
u32 s_black;

void dress_end_cap(u32 button) {
    for (const char* part : {"Pic_Icon", "Pic_Text"})
        if (u32 pane = lyt::find_pane(lyt::button_layout(button), part))
            lyt::show(pane, false);
}

class Panel : public engine::system::PushListener {
public:
    explicit Panel(u32 scene);
    ~Panel() override;
    void update();
    void draw();
    void on_push_start(u32 button) override;
    void on_pushed(u32 button) override;

private:
    struct Entry {
        u32 button;
        Action action;
        u32 to;
    };
    void set_buttons_active(bool on);
    void fade_in(bool with_bar);

    enum class State { WAITING, FADING, READY };
    u32 cursor_;      // the room's layout::Cursor
    u32 group_ = 0;   // the panel's exclusive group: one push at a time
    u32 scene_ = 0;   // Move_Btn_Scene, the buttons' locators
    std::vector<Entry> entries_;
    bool going_ = false;  // a button has sent the room on its way (its wipe closing)
    u32 pushed_ = 0;      // ... that button (its look held)
    State state_ = State::WAITING;
    bool bar_hidden_ = false;  // the room's bar's way back seen transparent (its FadeIn bound)
    f32 waited_ = 0, faded_ = 0;
    engine::ui::Ticker ticker_;
};

Panel::Panel(u32 scene) : cursor_(scene + 0x498) {
    group_ = g::operator_new(game::GROUP_SIZE);
    game::group_ctor(group_);
    scene_ = lyt::layout_new(SCENE_LEX);
    for (const BarButton& b : BAR) {
        if (u32 locator = lyt::find_pane(scene_, b.locator))
            lyt::store_f32(locator + 0x2C, lyt::load_f32(locator + 0x2C) + BAR_SHIFT);
        u32 button = lyt::button_new(b.lex, scene_, b.locator, cursor_);
        if (b.dress)
            b.dress(button);
        entries_.push_back({button, b.action, b.to});
    }
    const f32 rows_h = ITEM_H[0] + ROW_GAP + ITEM_H[1];
    const f32 row_y[2] = {(ITEMS_TOP + ITEMS_BOTTOM + rows_h) / 2 - ITEM_H[0] / 2,
                          (ITEMS_TOP + ITEMS_BOTTOM - rows_h) / 2 + ITEM_H[1] / 2};
    const f32 column_x[2] = {MIDDLE_X - (ITEM_W + ITEM_GAP) / 2, MIDDLE_X + (ITEM_W + ITEM_GAP) / 2};
    for (u32 i = 0; i < 4; i++) {
        u32 button = lyt::button_new(ITEMS[i].lex, scene_, ITEM_LOCATOR, cursor_);
        u32 root = lyt::button_root(button);
        lyt::move(root, column_x[i % 2] - ITEM_FACE_X, row_y[i / 2]);
        lyt::scale(root, ITEM_SCALE, ITEM_SCALE);
        entries_.push_back({button, Action::GO, ITEMS[i].to});
    }
    for (const Entry& e : entries_) {
        lyt::set_active(e.button, false);  // (until the panel has come in)
        if (e.action == Action::NONE)
            continue;
        engine::system::hook_button(e.button);
        game::group_add(e.button, group_);
    }
    engine::system::set_push_listener(this);
    screen::claim(BOTTOM_REGION);
    guest::log("room: bottom screen panel, %u buttons", (u32)entries_.size());
}

Panel::~Panel() {
    screen::release();
    engine::system::set_push_listener(nullptr);
    if (pushed_)
        engine::system::hold_button_look(pushed_, false);
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
        lyt::button_delete(it->button);
    lyt::layout_delete(scene_);
    game::group_dtor_delete(group_);
}

void Panel::set_buttons_active(bool on) {
    for (const Entry& e : entries_)
        if (e.action != Action::NONE)
            lyt::set_active(e.button, on);
}

// the panel comes in: the bar's FadeIn as the Wii played it, the black fading away over it
void Panel::fade_in(bool with_bar) {
    state_ = State::FADING;
    faded_ = 0;
    lyt::play(scene_, lyt::find_anim(scene_, FADE_IN));
    guest::log("room: the bottom screen panel comes in %s, %.0f 60ths after it was made",
               with_bar ? "with the room's bar" : "by itself (the room's bar not seen)", waited_);
}

void Panel::update() {
    const f32 dt = ticker_.tick();
    if (state_ == State::WAITING) {
        waited_ += dt;
        const u8 alpha = s_bar_locator ? guest::load8(s_bar_locator + 0xB8) : 0;
        if (alpha == 0)
            bar_hidden_ = true;
        if (bar_hidden_ && alpha > 0)
            fade_in(true);
        else if (waited_ >= FADE_FALLBACK)
            fade_in(false);
    } else if (state_ == State::FADING) {
        faded_ += dt;
        if (faded_ >= FADE_IN_FRAMES) {
            state_ = State::READY;
            set_buttons_active(true);
        }
    }
    screen::squeeze(false);
    lyt::layout_update(scene_);
    screen::squeeze(true);
}

void Panel::draw() {
    screen::to_bottom(BOTTOM_REGION);
    screen::squeeze(false);
    lyt::layout_draw(scene_);
    // black until the panel comes in, fading away as it does
    const f32 black = state_ == State::WAITING ? 1.0f : state_ == State::FADING ? 1.0f - faded_ / FADE_FROM_BLACK : 0.0f;
    if (black > 0 && s_black) {
        guest::store16(s_black + 4, (u16)(black * 255.0f));
        g::app_ScreenFade_Draw(s_black);
    }
    screen::squeeze(true);
    screen::to_top();
}

// A push sends the room on its way as it starts: the wipe closes over the button's Push (the
// room's own bar waited for the Push's end, 11 frames, before it asked). The button keeps its
// pushed look from then on (engine/system/host_scene.h hold_button_look): no pointer stays on it
// after the finger lifts (engine/input/remote.cpp), and the cursor's unfocus would fade it out
// under the wipe.
void Panel::on_push_start(u32 button) {
    if (going_)
        return;
    for (const Entry& e : entries_) {
        if (e.button != button)
            continue;
        if (e.action == Action::GO) {
            going_ = true;
            pushed_ = button;
            engine::system::hold_button_look(button, true);
            guest::log("room: to scene %u", e.to);
            g::controller_living_SceneTransit_Request(guest::load32(GA_controller_living_SceneTransit_sInstance), e.to);
        }
        return;
    }
}

// The Push has ended: the button's own end of a push has just put its layout back unfocused (its
// Focus played backwards from the start: grey); posed focused again before the frame is drawn
// (Push ends on the focused look)
void Panel::on_pushed(u32 button) {
    if (button != pushed_)
        return;
    const u32 w = lyt::button_layout(button);
    if (u32 focus = lyt::find_anim(w, "Focus"))
        lyt::pose(w, focus, lyt::last_frame(focus));
}

Panel* s_panel;
bool s_away;  // (away())
u32 s_white;  // (an app::ScreenFade as s_black, held at full white)

}  // namespace

void open(u32 scene) {
    if (!s_panel)
        s_panel = new Panel(scene);
}

// the panel goes; for a scene of the room's that is the channel's own, the bottom screen is the
// scene's whole (its touch over all of it)
void close() {
    delete s_panel;
    s_panel = nullptr;
    s_bar_locator = 0;
    if (s_away)
        screen::claim(screen::FULL);
}

void leaving(u32 to) {
    switch (to) {
    case TO_THEATER:
    case TO_MESSAGE_BOARD:
    case TO_MESSAGE_SEND:
    case TO_OPTIONS:
    case TO_HELP:
        s_away = true;
        screen::layouts_43(true);
        guest::log("room: away to scene %u, its layouts for 4:3 on the bottom screen", to);
        break;
    default: break;
    }
}

void away_for_setup() {
    if (s_away)
        return;
    s_away = true;
    screen::layouts_43(true);
    screen::claim(screen::FULL);
    guest::log("room: the first start's setup, its layouts for 4:3 on the bottom screen");
}

void back() {
    if (!s_away)
        return;
    s_away = false;
    screen::release();
    screen::layouts_43(false);
}

bool away() { return s_away; }

void boot() {
    engine::system::host_scene_boot([] {
        s_white = engine::system::lasting_alloc(12);
        for (u32 i = 0; i < 12; i++)
            guest::store8(s_white + i, i >= 6 ? 0xFF : 0);
        s_black = engine::system::lasting_alloc(12);
        for (u32 i = 0; i < 12; i++)
            guest::store8(s_black + i, i == 9 ? 0xFF : 0);  // (black, opaque; its alpha is frame / length)
        guest::store16(s_black + 2, 255);
    });
}

void set_bar_locator(u32 pane) { s_bar_locator = pane; }

void draw_away_top() {
    if (s_white)
        g::app_ScreenFade_Draw(s_white);
}

void update() {
    if (s_panel)
        s_panel->update();
    // NOMA_TEST_CARD: the screens' calibration card, once, over the room just made
    if (engine::ui::test_card::take_request())
        engine::system::change_scene(engine::ui::test_card::scene_factory());
}

void draw() {
    if (s_panel)
        s_panel->draw();
}

}  // namespace engine::room
