// The island's scene: the shell around its content (see scene.h for its states).
//
// Made like the Sound Player's (engine/soundplayer/scene.cpp): the cursor, the content's 3D, the
// 2D parts (the caption, the Home tab, the window), the cursor last. Its guest objects are made in
// the constructor (the content's while it loads) and freed in the destructor in the reverse order:
// the channel's heaps are stacks. The log gives the heap's free bytes as the scene opens and
// closes, which come back the same every time.
//
// The way in is black: 出発 fades the room out to black (Fade_Black, runtime/patches/wipes.cpp) and
// the scene comes in under that black. The application draws the wipe's layout over the scene
// for as long as the wipe has an animation; the scene takes the black over (game::wipe_hand_over)
// and draws it itself while the content loads, with Progress_Black's rings over it once the load
// takes longer than half a second (the channel's own loading look on black: concentric ripples,
// layout::parts::Progress's second kind). When the content is ready, Fade_Black's In takes it away.
// The way out is black too, and the room fades in from black.
#include "engine/wuhu/scene.h"

#include <string>

#include "engine/ui/g3d.h"
#include "engine/ui/game.h"
#include "engine/ui/lyt.h"
#include "engine/wuhu/bar_button.h"
#include "engine/wuhu/music.h"
#include "engine/wuhu/wuhu.h"
#include "noma_decomp.h"
#include "runtime/hw/hw.h"

namespace engine::wuhu {

namespace game = engine::ui::game;
namespace lyt = engine::ui::lyt;

namespace {

using engine::system::HostScene;
using engine::ui::Caption;
using engine::ui::Confirm;

const char* const PROGRESS_LEX = "layout/Progress/Progress_Black.lex";
const char16_t* const QUESTION = u"部屋にもどります。\nよろしいですか？";
const char16_t* const TITLE = u"ウーフーアイランド";
const char16_t* const MUSIC_TITLE = u"♪ 広い世界へ";

// the rings show once the load has taken this long, and stay at least this long once shown (half
// a second: their first ripple grown, rather than a flash)
const u64 RINGS_AFTER_US = 500000;
const int RINGS_MIN_FRAMES = 30;
// the caption stays four seconds
const int CAPTION_FRAMES = 240;
// the fade in from black ends by itself in 16 frames; this opens the island anyway should the
// wipe never report its end (as lyt::screen_may_open)
const int WIPE_TIMEOUT_FRAMES = 90;
// the room's fade in from black, asked for as the island fades out, lapses after this many game
// frames if the room hasn't faded in by then
const u32 BLACK_RETURN_FRAMES = 300;

// take_black_return's flag, and when it was set (game frames)
bool s_black_return = false;
u32 s_black_return_at = 0;

const char* const STATE_NAMES[] = {"loading", "fading in", "playing", "menu", "leaving"};

// The circle pad walks the Mii here, rather than moving the pointer (which platform/3ds/main.cpp
// makes it do elsewhere): the stick mode of the remote's input, on while the scene lives.
// INTEGRATION (track G1, engine/input/remote.h): its stick mode is to be switched here; until then
// the circle pad still nudges the pointer on the island.
void stick_mode(bool on) { (void)on; }

// The player's controls this frame: the remote's buttons, as the game's input manager read them.
// INTEGRATION (track G1, engine/input/remote.h): the move and camera sticks are to be read here.
Controls read_controls() {
    Controls c;
    c.held = game::buttons_held();
    c.pressed = game::buttons_triggered();
    return c;
}

std::string heap_name;
u32 heap_free() { return engine::ui::g3d::heap_free(&heap_name); }

}  // namespace

IslandScene::IslandScene() {
    heap_at_start_ = heap_free();
    stick_mode(true);
    cursor_ = g::operator_new(game::CURSOR_SIZE);
    game::cursor_ctor(cursor_);
    group_ = g::operator_new(game::GROUP_SIZE);
    game::group_ctor(group_);

    // the black the room's fade ended on (Fade_Black at the end of its Out), drawn by the scene
    // from now on
    black_ = game::wipe_layout(game::WIPE_FADE_BLACK);
    u32 out = lyt::find_anim(black_, "Out");
    lyt::pose(black_, out, lyt::last_frame(out));
    lyt::layout_update(black_);
    game::wipe_hand_over();
    rings_ = lyt::layout_new(PROGRESS_LEX);

    caption_ = new Caption(TITLE, MUSIC_TITLE);
    confirm_ = new Confirm(cursor_, group_, QUESTION);
    bottom_ = g::operator_new(game::BOTTOM_SIZE);
    game::bottom_ctor(bottom_, cursor_);  // hidden until the island shows
    engine::system::hook_button(bottom_ + game::BOTTOM_HOME_BTN_02);
    u32 heap_shell = heap_free();

    content_ = make_content();
    load_start_us_ = guest::host_time_us();
    content_->start_loading();
    guest::log("wuhu: open; heap %s: %u bytes free, %u taken by the shell", heap_name.c_str(), (unsigned)heap_at_start_,
               (unsigned)(heap_at_start_ - heap_shell));
}

IslandScene::~IslandScene() {
    delete content_;
    game::bottom_dtor_delete(bottom_);
    delete confirm_;
    delete caption_;
    lyt::layout_delete(rings_);
    game::group_dtor_delete(group_);
    game::cursor_dtor_delete(cursor_);
    stick_mode(false);
    u32 heap_end = heap_free();
    guest::log("wuhu: closed; heap %s: %u bytes free (%u at the start)", heap_name.c_str(), (unsigned)heap_end,
               (unsigned)heap_at_start_);
}

void IslandScene::set_state(State s) {
    guest::log("wuhu: %s (after %d frames %s)", STATE_NAMES[(int)s], state_frames_, STATE_NAMES[(int)state_]);
    state_ = s;
    state_frames_ = 0;
}

void IslandScene::update() {
    state_frames_++;
    const Controls controls = read_controls();
    switch (state_) {
    case State::LOADING: {
        f32 progress = content_->progress();
        u64 t = guest::host_time_us() - load_start_us_;
        if (rings_frames_ < 0 && t > RINGS_AFTER_US && !content_->ready()) {
            rings_frames_ = 0;
            lyt::play_loop(rings_, lyt::find_anim(rings_, "Default"));
            guest::log("wuhu: loading rings (%u ms, %.0f%% loaded)", (unsigned)(t / 1000), progress * 100.0f);
        }
        if (rings_frames_ >= 0) {
            rings_frames_++;
            lyt::layout_update(rings_);
        }
        if (content_->ready() && (rings_frames_ < 0 || rings_frames_ >= RINGS_MIN_FRAMES)) {
            guest::log("wuhu: loaded in %u ms, %d frames; heap %s: %u bytes free", (unsigned)(t / 1000), state_frames_,
                       heap_name.c_str(), (unsigned)heap_free());
            set_state(State::FADING_IN);
            // the wipe draws the black from now on, fading it away (Wipe::In resets the wipe, its
            // layouts posed transparent, then poses its In); the scene draws its own black once more
            // under it, so that no frame can show the island before the fade's first
            game::wipe_in(game::WIPE_FADE_BLACK);
            black_frames_ = 1;
            game::bottom_set_mode(bottom_, game::BOTTOM_HOME, false);
            music_arrive();
        }
        break;
    }
    case State::FADING_IN:
        content_->update(controls, true);
        if (state_frames_ > 1 && (!lyt::wipe_playing() || state_frames_ > WIPE_TIMEOUT_FRAMES)) {
            set_state(State::PLAYING);
            caption_->show(CAPTION_FRAMES);
        }
        break;
    case State::PLAYING:
        content_->update(controls, false);
        // B presses the Home tab (its push, then on_pushed opens the window), START opens it at once
        if (controls.pressed & game::PAD_B) {
            if (!game::bottom_press_back(bottom_, cursor_))
                open_menu();
        } else if (controls.pressed & game::PAD_PLUS) {
            open_menu();
        }
        break;
    case State::MENU:
        content_->update(controls, true);
        switch (confirm_->take_choice()) {
        case Confirm::Choice::YES: leave(); break;
        case Confirm::Choice::NO:
            set_state(State::PLAYING);
            game::bottom_set_mode(bottom_, game::BOTTOM_HOME, false);
            break;
        case Confirm::Choice::NONE: break;
        }
        break;
    case State::LEAVING:
        content_->update(controls, true);
        break;
    }
    caption_->update();
    confirm_->update();
    game::bottom_update(bottom_);
    game::cursor_update(cursor_);
}

void IslandScene::draw() {
    if (state_ != State::LOADING) {
        content_->draw_opaque();
        content_->draw_translucent();
    }
    caption_->draw();
    game::bottom_draw(bottom_);
    confirm_->draw();
    if (state_ == State::LOADING || black_frames_ > 0) {
        lyt::layout_draw(black_);
        if (state_ == State::LOADING && rings_frames_ >= 0)
            lyt::layout_draw(rings_);
        if (state_ != State::LOADING)
            black_frames_--;
        return;  // no pointer on the black
    }
    game::cursor_draw(cursor_);
}

void IslandScene::on_pushed(u32 button) {
    if (button == bottom_ + game::BOTTOM_HOME_BTN_02) {
        if (state_ == State::PLAYING)
            open_menu();
        return;
    }
    confirm_->on_pushed(button);
}

void IslandScene::open_menu() {
    if (state_ != State::PLAYING)
        return;
    set_state(State::MENU);
    caption_->hide();
    // the Home tab goes while the window is open (pushed with the pointer, which stays over it,
    // the Bottom keeps it there, lowered, under the window's veil)
    game::bottom_set_mode(bottom_, game::BOTTOM_HIDDEN, false);
    confirm_->open();
}

// back to the room: the music's handoff, then a fade to black, after which the room is made again
// (and fades in from black: take_black_return)
void IslandScene::leave() {
    set_state(State::LEAVING);
    s_black_return = true;
    s_black_return_at = guest::vi_game_frames();
    music_leave();
    game::change_scene_with_wipe(guest(), game::NAME_LIVING_ROOM_SCENE, game::WIPE_FADE_BLACK);
}

namespace {

HostScene* make_scene() { return new IslandScene(); }

const char* const CLASS_NAME = "scene::IslandScene";

void make_lasting() {
    engine::system::host_scene_factory(CLASS_NAME, &make_scene);
    reserve_bar_button();
    // the Home tab and the window's buttons are layout::Buttons
    engine::system::hookable_button_class(lyt::VTABLE_LAYOUT_BUTTON, lyt::LAYOUT_BUTTON_SLOTS);
}

}  // namespace

void boot() { engine::system::host_scene_boot(&make_lasting); }

u32 scene_factory() { return engine::system::host_scene_factory(CLASS_NAME, &make_scene); }

bool take_black_return() {
    if (!s_black_return)
        return false;
    s_black_return = false;
    return guest::vi_game_frames() - s_black_return_at < BLACK_RETURN_FRAMES;
}

}  // namespace engine::wuhu
