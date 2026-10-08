// The island's scene: the shell around its content (see scene.h for its states).
//
// Made like the Sound Player's (engine/soundplayer/scene.cpp): the cursor, the content's 3D, the
// 2D parts (the captions, the Home tab, the menu and its windows), the cursor last. Its guest
// objects are made in the constructor (the content's while it loads) and freed in the destructor
// in the reverse order: the channel's heaps are stacks. The log gives the heap's free bytes as the
// scene opens and closes, which come back the same every time.
//
// The way in is black: 出発 fades the room out to black (Fade_Black, runtime/patches/wipes.cpp) and
// the scene comes in under that black. The application draws the wipe's layout over the scene
// for as long as the wipe has an animation; the scene takes the black over (game::wipe_hand_over)
// and draws it itself while the content loads, with Progress_Black's rings over it once the load
// takes longer than half a second (the channel's own loading look on black: concentric ripples,
// layout::parts::Progress's second kind). When the content is ready, Fade_Black's In takes it away.
// The way out is black too, and the room fades in from black.
//
// The exploration menu (docs/wuhu_design.md §8) is the channel's dialog look throughout: the menu
// in its window over the white veil, and the windows that take its place over the same veil (the
// list of places, the question before going back), each giving the menu back with B or its own
// way back. The bottom left's tab follows: the Home tab while the island runs, nothing under the
// menu and the question (their windows answer B), the back tab (the return arrow) under the list.
// A warp is a fade to black and back within the scene (the same Fade_Black), the content moving
// the Mii while the screen is black, and the place's name as the caption.
#include "engine/wuhu/scene.h"

#include <string>

#include "engine/input/remote.h"
#include "engine/ui/g3d.h"
#include "engine/ui/game.h"
#include "engine/ui/screen.h"
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
using engine::ui::ListWindow;
using engine::ui::Menu;

const char* const PROGRESS_LEX = "layout/Progress/Progress_Black.lex";
const char16_t* const QUESTION = u"部屋にもどります。\nよろしいですか？";
const char16_t* const TITLE = u"ウーフーアイランド";
const char16_t* const MUSIC_TITLE = u"♪ 広い世界へ";
const char16_t* const PROMPT = u"行き先をえらんでください";

// the menu's choices, top to bottom (部屋にもどる on top, 探検をつづける at the bottom: the user's
// order); the last only closes it (B's, START's), and the highlight starts on it
enum Choice { ROOM, WARP, CONTINUE };
const std::vector<std::u16string> CHOICES = {u"部屋にもどる", u"ワープ", u"探検をつづける"};

// the captions' room: the longest place name the list sets small still fits Top_Message's line
const u32 CAPTION_CHARS = 32;

// the list of places: in the middle, its top tab clear of the prompt's rule (Top_Message's, at
// y 160) and its bottom tab of the back tab at the bottom left (layout units as they show)
// The bottom screen's part of the layouts' space (engine/ui/screen.h), as the Sound Player's: the
// bottom left of the 608x456 layouts (the Home tab's corner), 533x400 units at 0.6 pixels a unit;
// the menu, the places and the question are centred in it (BOTTOM_MIDDLE_X)
const engine::ui::screen::Region BOTTOM_REGION{-304, 172, 1600.0f / 3, 400};
const f32 BOTTOM_MIDDLE_X = -304 + 800.0f / 3;
// the places' window (4 rows) low enough for its prompt over it and its tabs, clear of the back tab
const f32 PLACES_X = BOTTOM_MIDDLE_X, PLACES_Y = -42, PLACES_W = 400;
// the prompt over it, from the screens' heading (y 169) down into the region (whose top is 172)
const f32 PROMPT_DROP = -26;

// the rings show once the load has taken this long, and stay at least this long once shown (half
// a second: their first ripple grown, rather than a flash)
const u64 RINGS_AFTER_US = 500000;
const int RINGS_MIN_FRAMES = 30;
// the captions stay four seconds (the prompt as long as the list shows)
const int CAPTION_FRAMES = 240;
// a warp holds the black this long after the content has moved (a beat between the places)
const int WARP_BLACK_FRAMES = 12;
// the room's fade in from black, asked for as the island fades out, lapses after this many game
// frames if the room hasn't faded in by then
const u32 BLACK_RETURN_FRAMES = 300;

// take_black_return's flag, and when it was set (game frames)
bool s_black_return = false;
u32 s_black_return_at = 0;

const char* const STATE_NAMES[] = {"loading", "fading in", "playing", "menu", "places",
                                   "confirm", "warp out",  "warp in", "leaving"};

// The circle pad walks the Mii here, rather than moving the pointer (which platform/3ds/main.cpp
// makes it do elsewhere): the stick mode of the remote's input, on while the scene lives (touching
// still points, for the Home tab and the window).
void stick_mode(bool on) { guest::input_set_stick_mode(on); }

f32 clamp1(f32 v) { return v < -1 ? -1 : v > 1 ? 1 : v; }

// The player's controls this frame: the remote's buttons, as the game's input manager read them;
// the move stick (the circle pad); the camera: the New 3DS's C-stick, or L and R to turn it round
// the Mii (R to the right); B held sprints (ZR too on a New 3DS: an Old 3DS has only B), the D-pad's
// up and down look up and down and its left and right bring it in and out, Y (the remote's 2)
// brings it round behind the Mii, A jumps (the A button's: a tap on the bottom screen's Home tab is
// a press there, no jump). (NOMA_INPUT on the host: S(x;y), C(x;y), [L], [R], [ZR], B, the D-pad's
// U, D, L, R, 2 and A.)
Controls read_controls() {
    Controls c;
    c.held = game::buttons_held();
    c.pressed = game::buttons_triggered();
    const guest::WiimoteInput& in = guest::wiimote_input();
    auto on = [&](u32 bit) { return (c.held & bit) ? 1.0f : 0.0f; };
    c.move_x = in.stick_x, c.move_y = in.stick_y;
    const f32 l = (in.pad & guest::PAD_L) ? 1.0f : 0.0f, r = (in.pad & guest::PAD_R) ? 1.0f : 0.0f;
    c.camera_x = clamp1(in.cam_x + r - l);
    c.camera_y = clamp1(in.cam_y + on(game::PAD_UP) - on(game::PAD_DOWN));
    c.zoom = on(game::PAD_RIGHT) - on(game::PAD_LEFT);
    c.fast_run = (in.pad & guest::PAD_ZR) != 0 || (c.held & game::PAD_B) != 0;
    c.behind = (c.pressed & game::PAD_TWO) != 0;
    const bool tapped = guest::wiimote_last_read_tapped();
    c.jump = (c.pressed & game::PAD_A) != 0 && !tapped;
    c.jump_held = (c.held & game::PAD_A) != 0 && !tapped;
    c.vehicle = (c.pressed & game::PAD_ONE) != 0;
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

    caption_ = new Caption(TITLE, MUSIC_TITLE, CAPTION_CHARS);
    prompt_ = new Caption(PROMPT, u"");
    menu_ = new Menu(cursor_, group_, CHOICES, CONTINUE);
    places_list_ = new ListWindow(cursor_, group_, PLACES_X, PLACES_Y, PLACES_W);
    confirm_ = new Confirm(cursor_, group_, QUESTION);
    // on the bottom screen, as the Sound Player's (engine/ui/screen.h): its region's middle is 37
    // units left of the layouts', so the windows move with it
    menu_->place(BOTTOM_MIDDLE_X);
    confirm_->place(BOTTOM_MIDDLE_X);
    prompt_->place(BOTTOM_MIDDLE_X, PROMPT_DROP);
    bottom_ = g::operator_new(game::BOTTOM_SIZE);
    game::bottom_ctor(bottom_, cursor_);  // hidden until the island shows
    engine::system::hook_button(bottom_ + game::BOTTOM_HOME_BTN_02);
    engine::system::hook_button(bottom_ + game::BOTTOM_RETURN_BTN_03);
    u32 heap_shell = heap_free();

    engine::ui::screen::claim(BOTTOM_REGION);

    content_ = make_content();
    load_start_us_ = guest::host_time_us();
    content_->start_loading();
    guest::log("wuhu: open; heap %s: %u bytes free, %u taken by the shell", heap_name.c_str(), (unsigned)heap_at_start_,
               (unsigned)(heap_at_start_ - heap_shell));
}

IslandScene::~IslandScene() {
    guest::input_set_stick_as_dpad(false);
    engine::ui::screen::release();
    delete content_;
    game::bottom_dtor_delete(bottom_);
    delete confirm_;
    delete places_list_;
    delete menu_;
    delete prompt_;
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
    // the circle pad moves the menus' highlights while one is up (it walks the Mii otherwise)
    guest::input_set_stick_as_dpad(s == State::MENU || s == State::PLACES || s == State::CONFIRM);
    state_ = s;
    state_frames_ = 0;
}

void IslandScene::update() {
    state_frames_++;
    const Controls controls = read_controls();
    switch (state_) {
    case State::LOADING: {
        // the load's next step, until it is done (the rings may stay a few frames longer)
        const f32 progress = content_->ready() ? 1.0f : content_->progress();
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
            // the menu's places, from the content's package (none without one: ワープ is greyed)
            if (const engine::g3d::Package* package = content_->package())
                places_ = warp_places(*package);
            std::vector<std::u16string> names;
            for (const Place& p : places_)
                names.push_back(p.name);
            places_list_->set_entries(names);
            guest::log("wuhu: %u places to warp to", (unsigned)places_.size());
            // the wipe draws the black from now on, fading it away (Wipe::In resets the wipe, its
            // layouts posed transparent, then poses its In); the scene draws its own black once more
            // under it, so that no frame can show the island before the fade's first
            game::wipe_in(game::WIPE_FADE_BLACK);
            black_frames_ = 1;
            game::bottom_set_mode(bottom_, game::BOTTOM_HOME, false);
            music_arrive();
            // the content's first update, before its first draw (this frame's: from ready() on it
            // is drawn every frame, and updated before it is drawn)
            content_->update(controls, true);
        }
        break;
    }
    case State::FADING_IN:
        content_->update(controls, true);
        // the fade in from black (16 frames) has ended, or never reports its end (lyt.h's timeout)
        if (lyt::screen_may_open(state_frames_)) {
            set_state(State::PLAYING);
            caption_->show(CAPTION_FRAMES);
        }
        break;
    case State::PLAYING:
        content_->update(controls, false);
        // START opens the menu (as the Home tab does, touched: on_pushed); B is the sprint now
        if (controls.pressed & game::PAD_PLUS)
            open_menu();
        break;
    case State::MENU:
        content_->update(controls, true);
        // START again closes it, as 探検をつづける (its push, then the choice)
        if (controls.pressed & game::PAD_PLUS)
            menu_->press_cancel();
        switch (menu_->take_choice()) {
        case CONTINUE: close_menu(); break;
        case WARP: open_places(); break;
        case ROOM:
            set_state(State::CONFIRM);
            menu_->page_out(false);
            confirm_->open(true);
            break;
        default: break;
        }
        break;
    case State::PLACES: {
        content_->update(controls, true);
        int chosen = places_list_->take_chosen();
        if (chosen >= 0 && chosen < (int)places_.size()) {
            start_warp(chosen);
        } else if ((controls.pressed & game::PAD_B) && places_list_->usable()) {
            // B presses the back tab (then on_pushed goes back), or goes back at once if the tab
            // can't take it (still coming in)
            if (!game::bottom_press_back(bottom_, cursor_))
                back_to_menu();
        }
        break;
    }
    case State::CONFIRM:
        content_->update(controls, true);
        switch (confirm_->take_choice()) {
        case Confirm::Choice::YES: leave(); break;
        case Confirm::Choice::NO:
            set_state(State::MENU);
            menu_->page_in();
            break;
        case Confirm::Choice::NONE: break;
        }
        break;
    case State::WARP_OUT:
        // Once the fade to black has ended (or never reports its end), the content moves the Mii
        // to the place under the black, and after a beat the black fades away.
        if (!warped_ && state_frames_ > 1 && (!lyt::wipe_playing() || state_frames_ > lyt::WIPE_TIMEOUT_FRAMES)) {
            const Place& p = places_[place_];
            guest::log("wuhu: warp to place %d (marker %u) after %d frames of fade", place_, (unsigned)p.marker,
                       state_frames_);
            content_->warp(p.marker);
            warped_ = true;
            black_hold_ = 0;
        }
        content_->update(controls, true);
        if (warped_ && ++black_hold_ >= WARP_BLACK_FRAMES) {
            set_state(State::WARP_IN);
            game::wipe_in(game::WIPE_FADE_BLACK);
            black_frames_ = 1;
            game::bottom_set_mode(bottom_, game::BOTTOM_HOME, false);
        }
        break;
    case State::WARP_IN:
        content_->update(controls, true);
        if (lyt::screen_may_open(state_frames_)) {
            set_state(State::PLAYING);
            caption_->set_text(places_[place_].name, u"");
            caption_->show(CAPTION_FRAMES);
        }
        break;
    case State::LEAVING:
        content_->update(controls, true);
        break;
    }
    caption_->update();
    // the bottom screen's layouts, and the cursor that hit-tests them (engine/ui/screen.h)
    engine::ui::screen::squeeze(false);
    prompt_->update();
    menu_->update();
    places_list_->update();
    confirm_->update();
    game::bottom_update(bottom_);
    game::cursor_update(cursor_);
    engine::ui::screen::squeeze(true);
}

void IslandScene::draw() {
    namespace screen = engine::ui::screen;
    // the top screen: the island and its caption, black while loading or fading in (the rings)
    if (state_ != State::LOADING) {
        content_->draw_opaque();
        content_->draw_translucent();
    }
    caption_->draw();
    const bool black = state_ == State::LOADING || black_frames_ > 0;
    if (black) {
        lyt::layout_draw(black_);
        if (state_ == State::LOADING && rings_frames_ >= 0)
            lyt::layout_draw(rings_);
    }
    // the bottom screen: the Home tab, the menu, the places, the question (black with the top)
    screen::to_bottom(BOTTOM_REGION);
    screen::squeeze(false);
    // the tab under the menu's veil, but the back tab, the list's way back, over it
    const bool tab_over_veil = state_ == State::PLACES;
    if (!tab_over_veil)
        game::bottom_draw(bottom_);
    menu_->draw();
    confirm_->draw();
    places_list_->draw();
    prompt_->draw();
    if (tab_over_veil)
        game::bottom_draw(bottom_);
    if (black)
        lyt::layout_draw(black_);
    screen::squeeze(true);
    screen::to_top();
    if (black && state_ != State::LOADING)
        black_frames_--;
}

void IslandScene::on_push_start(u32 button) { places_list_->on_push_start(button); }

void IslandScene::on_pushed(u32 button) {
    if (button == bottom_ + game::BOTTOM_HOME_BTN_02) {
        if (state_ == State::PLAYING)
            open_menu();
        return;
    }
    if (button == bottom_ + game::BOTTOM_RETURN_BTN_03) {
        if (state_ == State::PLACES)
            back_to_menu();
        return;
    }
    if (menu_->on_pushed(button) || places_list_->on_pushed(button))
        return;
    confirm_->on_pushed(button);
}

void IslandScene::open_menu() {
    // not while a window still fades out (after 探検をつづける, or a warp's list): it would not
    // open again, and the scene would wait in MENU for a choice that can't come
    if (state_ != State::PLAYING || menu_->shown() || places_list_->shown() || confirm_->shown())
        return;
    set_state(State::MENU);
    caption_->hide();
    // the Home tab goes while the menu is open (pushed with the pointer, which stays over it,
    // the Bottom keeps it there, lowered, under the window's veil)
    game::bottom_set_mode(bottom_, game::BOTTOM_HIDDEN, false);
    menu_->set_enabled(WARP, !places_.empty());
    menu_->open();
}

void IslandScene::close_menu() {
    set_state(State::PLAYING);
    menu_->close();
    game::bottom_set_mode(bottom_, game::BOTTOM_HOME, false);
}

// the list of places in the menu's place, over its veil, the prompt over it and the back tab
void IslandScene::open_places() {
    set_state(State::PLACES);
    menu_->page_out(true);
    places_list_->open();  // where it was: the last place warped to, or where the D-pad left it
    prompt_->show(Caption::UNTIL_HIDDEN);
    game::bottom_set_mode(bottom_, game::BOTTOM_RETURN, false);
}

void IslandScene::back_to_menu() {
    set_state(State::MENU);
    places_list_->close();
    prompt_->hide();
    game::bottom_set_mode(bottom_, game::BOTTOM_HIDDEN, false);
    menu_->page_in();
}

// a place chosen: everything goes as the screen fades to black (Fade_Black's Out, as on the way
// out of the room), the rest in WARP_OUT
void IslandScene::start_warp(int place) {
    set_state(State::WARP_OUT);
    place_ = place;
    warped_ = false;
    places_list_->close();
    prompt_->hide();
    menu_->close();
    game::bottom_set_mode(bottom_, game::BOTTOM_HIDDEN, false);
    game::wipe_out(game::WIPE_FADE_BLACK);
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
    // the Home and back tabs, the menu's, list's and window's buttons are layout::Buttons
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
