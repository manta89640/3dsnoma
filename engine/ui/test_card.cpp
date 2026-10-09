// The screens' calibration card (see test_card.h).
#include "engine/ui/test_card.h"

#include <cstdlib>
#include <string>
#include <vector>

#include "engine/system/host_scene.h"
#include "engine/ui/game.h"
#include "engine/ui/lyt.h"
#include "engine/ui/screen.h"

namespace engine::ui::test_card {

namespace {

using engine::system::HostScene;
using namespace lyt;

const char* const ROW_LEX = "layout/Common/Top_Message.lex";      // a line of the channel's text
const char* const WHITE_LEX = "layout/Common/Common_White_Bg.lex";  // the menus' white ground
const char* const SCENE_LEX = "layout/Common/Common_Scene.lex";   // the targets' locators
const char* const BUTTON_LEX = "layout/Common/Common_SmallBtn.lex";

// pixels a layout unit, up and down: the top screen shows the layouts' 456 units on 240 lines, the
// bottom screen's region its 400 on 240
const f32 TOP_PX = 240.0f / 456.0f, BOTTOM_PX = 240.0f / 400.0f;
// Top_Message's own font, its width to its height
const f32 FONT_ASPECT = 22.4f / 26.6f;
const u32 ROW_CHARS = 40;

const int TOP_SIZES[] = {8, 10, 12, 14, 16, 18, 20, 24};  // text, pixels high (the font's cell)
const int BOTTOM_SIZES[] = {8, 10, 12, 14, 16};
const int TARGET_SIZES[] = {24, 32, 40, 48, 56};        // buttons, pixels square
const f32 ROW_GAP_PX = 6, TARGET_GAP = 18;

std::u16string number(int n) {
    std::u16string s;
    do {
        s.insert(s.begin(), (char16_t)(u'0' + n % 10));
        n /= 10;
    } while (n);
    return s;
}

std::u16string sample(int px) { return number(px) + u" px  Wiiの間 メッセージ ABC 123"; }

class TestCard : public HostScene {
public:
    TestCard();
    ~TestCard() override;
    void update() override;
    void draw() override;
    u32 cursor() override { return cursor_; }
    void on_pushed(u32 button) override;

private:
    struct Target {
        u32 button, text;
        int px, hits;
    };
    // a row of text at `px` pixels high, its middle `y` units from the layouts' middle
    u32 make_row(int px, f32 px_per_unit, f32 left, f32 y, bool top);
    void label(Target& t);
    void leave();

    u32 cursor_ = 0, group_ = 0, bottom_ = 0, scene_ = 0, white_ = 0;
    std::vector<u32> top_rows_, bottom_rows_;
    std::vector<Target> targets_;
    bool leaving_ = false;
};

u32 TestCard::make_row(int px, f32 px_per_unit, f32 left, f32 y, bool top) {
    u32 row = layout_new(ROW_LEX);
    u32 loc = find_pane(row, "Loc_Top");
    location_adjust(loc, top);  // (the top screen's 16:9 squeeze keeps the text's shape there)
    move(loc, 0, y);
    show(find_pane(row, "Pic_Line"), false);
    u32 text = find_pane(row, "Text_Title");
    const f32 h = (f32)px / px_per_unit;
    move(text, left + 360, 0);
    resize(text, 720, h * 1.5f);
    text_reserve(text, ROW_CHARS);
    font_size(text, h * FONT_ASPECT, h);
    text_position(text, TEXT_LEFT);
    set_text(text, sample(px));
    for (const char* anim : {"FadeIn_All", "FadeIn"})
        if (u32 a = find_anim(row, anim))
            pose(row, a, last_frame(a));
    return row;
}

TestCard::TestCard() {
    namespace screen = engine::ui::screen;
    cursor_ = g::operator_new(game::CURSOR_SIZE);
    game::cursor_ctor(cursor_);
    group_ = g::operator_new(game::GROUP_SIZE);
    game::group_ctor(group_);

    // the top screen: the white ground, rows from the top down
    white_ = layout_new(WHITE_LEX);
    f32 y_px = 8;
    for (int px : TOP_SIZES) {
        const f32 mid = y_px + px * 0.75f;
        top_rows_.push_back(make_row(px, TOP_PX, -290, 228 - mid / TOP_PX, true));
        y_px += px * 1.5f + ROW_GAP_PX * 0.5f;
    }
    // the bottom screen: rows from its region's top, then the targets in a row under them
    const screen::Region& r = screen::BOTTOM;
    y_px = 8;
    for (int px : BOTTOM_SIZES) {
        const f32 mid = y_px + px * 0.75f;
        bottom_rows_.push_back(make_row(px, BOTTOM_PX, r.left + 16, r.top - mid / BOTTOM_PX, false));
        y_px += px * 1.5f + ROW_GAP_PX * 0.5f;
    }
    scene_ = layout_new(SCENE_LEX);
    f32 total = 0;
    for (int px : TARGET_SIZES)
        total += px / BOTTOM_PX + TARGET_GAP;
    f32 x = screen::BOTTOM_MIDDLE_X - (total - TARGET_GAP) / 2;
    const f32 y = r.top - (y_px + 8 + TARGET_SIZES[4] / 2.0f) / BOTTOM_PX;
    for (int px : TARGET_SIZES) {
        const f32 s = px / BOTTOM_PX;
        Target t{button_new(BUTTON_LEX, scene_, "Loc_Top", cursor_), 0, px, 0};
        u32 layout = button_layout(t.button);
        move(button_root(t.button), x + s / 2, y);
        for (const char* part : {"Wind_Btn", "Wind_Shadow", "Bound_Btn"})
            if (u32 p = find_pane(layout, part))
                resize(p, s, s);
        show(find_pane(layout, "Loc_Push_Mark"), false);
        t.text = find_pane(layout, "Text_Btn");
        text_reserve(t.text, 12);
        resize(t.text, s, s);
        font_size(t.text, 9 / BOTTOM_PX * FONT_ASPECT, 9 / BOTTOM_PX);
        label(t);
        set_sounds(t.button, SE_OVER_M, SE_CLICK_GENERAL);
        engine::system::hook_button(t.button);
        game::group_add(t.button, group_);
        targets_.push_back(t);
        x += s + TARGET_GAP;
    }
    bottom_ = g::operator_new(game::BOTTOM_SIZE);
    game::bottom_ctor(bottom_, cursor_);
    game::bottom_set_mode(bottom_, game::BOTTOM_HOME, false);
    engine::system::hook_button(bottom_ + game::BOTTOM_HOME_BTN_02);
    screen::claim(r);
    game::wipe_in(game::WIPE_FADE_WHITE);
    guest::log("test card: open");
}

TestCard::~TestCard() {
    engine::ui::screen::release();
    game::bottom_dtor_delete(bottom_);
    for (auto it = targets_.rbegin(); it != targets_.rend(); ++it)
        button_delete(it->button);
    layout_delete(scene_);
    for (auto it = bottom_rows_.rbegin(); it != bottom_rows_.rend(); ++it)
        layout_delete(*it);
    for (auto it = top_rows_.rbegin(); it != top_rows_.rend(); ++it)
        layout_delete(*it);
    layout_delete(white_);
    game::group_dtor_delete(group_);
    game::cursor_dtor_delete(cursor_);
    guest::log("test card: closed");
}

// "32 px", and the taps it took
void TestCard::label(Target& t) {
    std::u16string s = number(t.px) + u"px";
    if (t.hits)
        s += u"\nx" + number(t.hits);  // (no × in the font)
    set_text(t.text, s);
}

void TestCard::update() {
    namespace screen = engine::ui::screen;
    screen::squeeze(true);
    layout_update(white_);
    for (u32 row : top_rows_)
        layout_update(row);
    screen::squeeze(false);
    for (u32 row : bottom_rows_)
        layout_update(row);
    layout_update(scene_);
    game::bottom_update(bottom_);
    game::cursor_update(cursor_);
    screen::squeeze(true);
    if (!leaving_ && (game::buttons_triggered() & game::PAD_B))
        game::bottom_press_back(bottom_, cursor_);
}

void TestCard::draw() {
    namespace screen = engine::ui::screen;
    layout_draw(white_);
    for (u32 row : top_rows_)
        layout_draw(row);
    screen::to_bottom(screen::BOTTOM);
    screen::squeeze(false);
    for (u32 row : bottom_rows_)
        layout_draw(row);
    layout_draw(scene_);
    game::bottom_draw(bottom_);
    screen::squeeze(true);
    screen::to_top();
}

void TestCard::on_pushed(u32 button) {
    if (button == bottom_ + game::BOTTOM_HOME_BTN_02) {
        leave();
        return;
    }
    for (Target& t : targets_) {
        if (t.button == button) {
            t.hits++;
            label(t);
            guest::log("test card: %d px target pushed (%d times)", t.px, t.hits);
        }
    }
}

void TestCard::leave() {
    if (leaving_)
        return;
    leaving_ = true;
    game::change_scene_with_wipe(guest(), game::NAME_LIVING_ROOM_SCENE, game::WIPE_FADE_WHITE);
}

HostScene* make_scene() { return new TestCard(); }

const char* const CLASS_NAME = "scene::TestCardScene";

}  // namespace

void boot() {
    engine::system::host_scene_boot([] {
        engine::system::host_scene_factory(CLASS_NAME, &make_scene);
        engine::system::hookable_button_class(VTABLE_LAYOUT_BUTTON, LAYOUT_BUTTON_SLOTS);
    });
}

bool take_request() {
    static bool s_done;
    if (s_done)
        return false;
    s_done = true;
    const char* e = std::getenv("NOMA_TEST_CARD");
    return e && *e && *e != '0';
}

u32 scene_factory() { return engine::system::host_scene_factory(CLASS_NAME, &make_scene); }

}  // namespace engine::ui::test_card
