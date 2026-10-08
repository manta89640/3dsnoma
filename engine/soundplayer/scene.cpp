// The Sound Player's scene: what it is made of, its frame, and the way in and out.
//
// Made like the channel's own TV scene (scene::TvScene): the cursor, the 3D stage with the Miis,
// the layouts, the bottom bar's Home tab; updated 3D first, then the layouts, then the cursor
// (which hit-tests the buttons). On the 3DS's two screens (engine/ui/screen.h): the top one has the
// stage, where the household's Miis dance, and the title of what plays over it; the bottom one the
// channel's list window of tracks, the video player's buttons and the Home tab, which the touch
// screen works directly (a tap presses what it is on; a drag isn't scrolling yet: the list's tabs
// are). The cursor isn't drawn: there is nothing to point at on the top screen. It is reached from
// the room's bar (runtime/patches/menus.cpp) through the room's circle wipe, and goes back with a
// white fade, after which the room is made again and its music comes back by itself.
//
// Buttons: the D-pad's up and down move the list's highlight, A plays the highlighted track (when
// nothing was touched for a while), left and right play the previous and next track, START
// pauses and resumes, SELECT stops, B goes back to the room.
#include "engine/soundplayer/sound_player.h"

#include "engine/soundplayer/catalog.h"
#include "engine/soundplayer/controls.h"
#include "engine/soundplayer/music.h"
#include "engine/soundplayer/stage.h"
#include "engine/soundplayer/track_list.h"
#include "engine/system/host_scene.h"
#include "engine/input/remote.h"
#include "engine/ui/game.h"
#include "engine/ui/lyt.h"
#include "engine/ui/screen.h"

namespace engine::soundplayer {

namespace game = engine::ui::game;
namespace lyt = engine::ui::lyt;

namespace {

using engine::system::HostScene;

// the bottom screen's part of the layouts' space: the bottom left of the 608x456 layouts (where the
// Home tab is), 533x400 units (4:3), at 0.6 pixels a unit; the list window and the buttons are
// placed in it (track_list.cpp, controls.cpp)
const engine::ui::screen::Region BOTTOM_REGION{-304, 172, 1600.0f / 3, 400};

class SoundPlayerScene : public HostScene {
public:
    SoundPlayerScene();
    ~SoundPlayerScene() override;
    void update() override;
    void draw() override;
    u32 cursor() override { return cursor_; }
    void on_push_start(u32 button) override { list_->on_push_start(button); }
    void on_pushed(u32 button) override;

private:
    void leave();
    void play_index(int i);
    void step(int direction);
    void toggle_play();
    int current_index() const;

    u32 cursor_ = 0;
    u32 group_ = 0;   // the exclusive button group: one push at a time
    u32 bottom_ = 0;  // the Home tab, bottom left
    Music* music_ = nullptr;
    Stage* stage_ = nullptr;
    TrackList* list_ = nullptr;
    Controls* controls_ = nullptr;
    bool leaving_ = false;
    PlayState logged_state_ = PlayState::STOPPED;
    u32 logged_sound_ = Music::NO_SOUND;
};

SoundPlayerScene::SoundPlayerScene() {
    const std::vector<Track>& tracks = catalog();
    music_ = new Music();
    cursor_ = g::operator_new(game::CURSOR_SIZE);
    game::cursor_ctor(cursor_);
    group_ = g::operator_new(game::GROUP_SIZE);
    game::group_ctor(group_);
    stage_ = new Stage();
    list_ = new TrackList(cursor_, group_);
    controls_ = new Controls(cursor_, group_);
    bottom_ = g::operator_new(game::BOTTOM_SIZE);
    game::bottom_ctor(bottom_, cursor_);
    game::bottom_set_mode(bottom_, game::BOTTOM_HOME, false);
    engine::system::hook_button(bottom_ + game::BOTTOM_HOME_BTN_02);
    engine::ui::screen::claim(BOTTOM_REGION);
    guest::input_set_stick_mode(true);  // (the circle pad doesn't move a pointer about)
    // the room left a white screen behind (its circle wipe): fade in from it
    game::wipe_in(game::WIPE_FADE_WHITE);
    guest::log("soundplayer: open, %u tracks, playing %d", (u32)tracks.size(), current_index());
}

SoundPlayerScene::~SoundPlayerScene() {
    guest::input_set_stick_mode(false);
    engine::ui::screen::release();
    game::bottom_dtor_delete(bottom_);
    delete controls_;
    delete list_;
    delete stage_;
    game::group_dtor_delete(group_);
    game::cursor_dtor_delete(cursor_);
    delete music_;
    guest::log("soundplayer: closed");
}

int SoundPlayerScene::current_index() const {
    const std::vector<Track>& tracks = catalog();
    u32 id = music_->current();
    for (size_t i = 0; i < tracks.size(); i++)
        if (tracks[i].sound_id == id)
            return (int)i;
    return -1;
}

void SoundPlayerScene::play_index(int i) {
    const std::vector<Track>& tracks = catalog();
    if (i < 0 || i >= (int)tracks.size())
        return;
    guest::log("soundplayer: play %d (sound %u)", i, tracks[i].sound_id);
    music_->play(tracks[i].sound_id);
}

void SoundPlayerScene::step(int direction) {
    int n = (int)catalog().size();
    if (!n)
        return;
    int i = current_index();
    if (i < 0)
        i = list_->highlighted() - (direction > 0 ? 1 : -1);
    play_index(((i + direction) % n + n) % n);
}

// the play/pause button and +: pause what plays, resume what's paused, play the track current
// (or the list's highlighted one) when stopped; nothing while the player switches pieces
void SoundPlayerScene::toggle_play() {
    switch (music_->state()) {
    case PlayState::PLAYING: music_->pause(); break;
    case PlayState::PAUSED: music_->resume(); break;
    case PlayState::STOPPED: play_index(current_index() >= 0 ? current_index() : list_->highlighted()); break;
    case PlayState::SWITCHING: break;
    }
}

void SoundPlayerScene::update() {
    music_->update();
    if (music_->state() != logged_state_ || music_->current() != logged_sound_) {
        logged_state_ = music_->state();
        logged_sound_ = music_->current();
        static const char* const NAMES[] = {"stopped", "playing", "paused", "switching"};
        guest::log("soundplayer: %s, sound %d", NAMES[(int)logged_state_], (int)logged_sound_);
    }
    namespace screen = engine::ui::screen;
    int i = current_index();
    // the top screen's: the stage, the title
    screen::squeeze(true);
    stage_->update(*music_);
    controls_->update_title(*music_, i >= 0 ? &catalog()[i] : nullptr);
    // the bottom screen's, and the cursor that hit-tests them
    screen::squeeze(false);
    list_->update(*music_);
    controls_->update(*music_);
    game::bottom_update(bottom_);
    game::cursor_update(cursor_);
    screen::squeeze(true);
    if (leaving_)
        return;

    int chosen = list_->take_chosen();
    if (chosen >= 0)
        play_index(chosen);
    switch (controls_->take_action()) {
    case Controls::PREVIOUS: step(-1); break;
    case Controls::NEXT: step(1); break;
    case Controls::STOP: music_->stop(); break;
    case Controls::PLAY_PAUSE: toggle_play(); break;
    case Controls::NONE: break;
    }

    u32 pressed = game::buttons_triggered();
    if (pressed & game::PAD_B)
        game::bottom_press_back(bottom_, cursor_);
    else if (pressed & game::PAD_LEFT)
        step(-1);
    else if (pressed & game::PAD_RIGHT)
        step(1);
    else if (pressed & game::PAD_PLUS)
        toggle_play();
    else if (pressed & game::PAD_MINUS)
        music_->stop();
    else if ((pressed & game::PAD_A) && !game::pointer_on_screen())
        play_index(list_->highlighted());
}

void SoundPlayerScene::draw() {
    namespace screen = engine::ui::screen;
    stage_->draw();
    controls_->draw_title();
    screen::to_bottom(BOTTOM_REGION);
    screen::squeeze(false);
    list_->draw();
    controls_->draw();
    game::bottom_draw(bottom_);
    screen::squeeze(true);
    screen::to_top();
}

void SoundPlayerScene::on_pushed(u32 button) {
    if (button == bottom_ + game::BOTTOM_HOME_BTN_02)
        leave();
    else if (!list_->on_pushed(button))
        controls_->on_pushed(button);
}

// back to the room: a white fade, then the room is made again (its music comes back by itself)
void SoundPlayerScene::leave() {
    if (leaving_)
        return;
    leaving_ = true;
    music_->leave();  // the player's piece stops as the screen goes
    guest::log("soundplayer: back to the room");
    game::change_scene_with_wipe(guest(), game::NAME_LIVING_ROOM_SCENE, game::WIPE_FADE_WHITE);
}

HostScene* make_scene() { return new SoundPlayerScene(); }

const char* const CLASS_NAME = "scene::SoundPlayerScene";

void make_lasting() {
    engine::system::host_scene_factory(CLASS_NAME, &make_scene);
    reserve_bar_button();
    engine::system::hookable_button_class(lyt::VTABLE_LAYOUT_BUTTON, lyt::LAYOUT_BUTTON_SLOTS);
    // layout::movie::PlayButton (the controls' play/pause disc): as many virtuals as layout::Button
    engine::system::hookable_button_class(lyt::VTABLE_PLAY_BUTTON, lyt::LAYOUT_BUTTON_SLOTS);
}

}  // namespace

void boot() { engine::system::host_scene_boot(&make_lasting); }

u32 scene_factory() { return engine::system::host_scene_factory(CLASS_NAME, &make_scene); }

}  // namespace engine::soundplayer
