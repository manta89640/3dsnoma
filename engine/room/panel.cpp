// The room's bottom screen (see panel.h).
#include "engine/room/panel.h"

#include <vector>

#include "engine/soundplayer/sound_player.h"
#include "engine/system/host_scene.h"
#include "engine/ui/confirm.h"
#include "engine/ui/game.h"
#include "engine/ui/lyt.h"
#include "engine/ui/screen.h"
#include "engine/wuhu/bar_button.h"
#include "engine/wuhu/wuhu.h"
#include "noma_decomp.h"

namespace engine::room {

namespace game = engine::ui::game;
namespace lyt = engine::ui::lyt;
namespace screen = engine::ui::screen;

namespace {

using engine::ui::Confirm;

// The bottom screen's part of the layouts' space (engine/ui/screen.h BOTTOM: the Sound Player's
// and the island's). The bar's places in Move_Btn_Scene (y -162, the way back at x -267 to the TV
// programmes' end cap at 246) are inside it as they are inside the TV picture, the end cap running
// off its right edge as it ran off the picture's.
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

enum class Action { NONE, BACK, GO };

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
void dress_back(u32 button);
void dress_end_cap(u32 button);
const BarButton BAR[] = {
    {"layout/Common/Wii_Btn.lex", "Loc_Back_Btn", Action::BACK, 0, &dress_back},
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

// the channel's question for the way back (the room's quit dialog: "Wiiの間をおわります。" in the
// port's words, runtime/patches/wii_parts.cpp)
const u32 MSG_QUIT = 145;

// the Wii button goes out of the app here: its back arrow stays, the "Wii" under it goes
void dress_back(u32 button) {
    for (const char* logo : {"Pic_Wii", "Pic_Shadow"})
        if (u32 pane = lyt::find_pane(lyt::button_layout(button), logo))
            lyt::show(pane, false);
}

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
    void on_pushed(u32 button) override;

private:
    struct Entry {
        u32 button;
        Action action;
        u32 to;
    };
    void set_buttons_active(bool on);

    u32 cursor_;      // the room's layout::Cursor
    u32 group_ = 0;   // the panel's exclusive group: one push at a time
    u32 scene_ = 0;   // Move_Btn_Scene, the buttons' locators
    std::vector<Entry> entries_;
    Confirm* quit_ = nullptr;
    bool going_ = false;  // a button has sent the room on its way (its wipe closing)
};

Panel::Panel(u32 scene) : cursor_(scene + 0x498) {
    group_ = g::operator_new(game::GROUP_SIZE);
    game::group_ctor(group_);
    scene_ = lyt::layout_new(SCENE_LEX);
    for (const BarButton& b : BAR) {
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
        if (e.action == Action::NONE) {
            lyt::set_active(e.button, false);
            continue;
        }
        engine::system::hook_button(e.button);
        game::group_add(e.button, group_);
    }
    quit_ = new Confirm(cursor_, group_, lyt::message_string(MSG_QUIT));
    quit_->place(MIDDLE_X, Confirm::DROP);
    engine::system::set_push_listener(this);
    screen::claim(BOTTOM_REGION);
    guest::log("room: bottom screen panel, %u buttons", (u32)entries_.size());
}

Panel::~Panel() {
    screen::release();
    engine::system::set_push_listener(nullptr);
    delete quit_;
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

void Panel::update() {
    screen::squeeze(false);
    quit_->update();
    lyt::layout_update(scene_);
    screen::squeeze(true);
    switch (quit_->take_choice()) {
    case Confirm::Choice::YES:
        // as the room's dialog's はい: the app ends (its fade out, then back to the start)
        guest::log("room: quit");
        g::app_Exit_Request(guest::load32(GA_app_Application_sInstance) + 0x330, 4);
        break;
    case Confirm::Choice::NO: set_buttons_active(true); break;
    case Confirm::Choice::NONE: break;
    }
}

void Panel::draw() {
    screen::to_bottom(BOTTOM_REGION);
    screen::squeeze(false);
    lyt::layout_draw(scene_);
    quit_->draw();
    screen::squeeze(true);
    screen::to_top();
}

void Panel::on_pushed(u32 button) {
    if (quit_->on_pushed(button) || going_)
        return;
    for (const Entry& e : entries_) {
        if (e.button != button)
            continue;
        if (e.action == Action::BACK) {
            set_buttons_active(false);
            quit_->open();
        } else if (e.action == Action::GO) {
            going_ = true;
            guest::log("room: to scene %u", e.to);
            g::controller_living_SceneTransit_Request(guest::load32(GA_controller_living_SceneTransit_sInstance), e.to);
        }
        return;
    }
}

Panel* s_panel;

}  // namespace

void open(u32 scene) {
    if (!s_panel)
        s_panel = new Panel(scene);
}

void close() {
    delete s_panel;
    s_panel = nullptr;
}

void update() {
    if (s_panel)
        s_panel->update();
}

void draw() {
    if (s_panel)
        s_panel->draw();
}

}  // namespace engine::room
