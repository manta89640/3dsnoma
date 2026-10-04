// The Sound Player's scene: what it is made of, its frame, and the way in and out.
//
// Made like the channel's own TV scene (scene::TvScene): the cursor, the 3D stage with the Miis,
// the layouts, the bottom bar's Home tab; updated 3D first, then the layouts, then the cursor
// (which hit-tests the buttons); drawn 3D first, then the layouts, the cursor last. It is reached
// from the room's bar (runtime/patches/menus.cpp) through the room's circle wipe, and goes back
// with a white fade, after which the room is made again and its music comes back by itself.
//
// Controls besides the pointer: the D-pad's up and down move the list's highlight, A plays the
// highlighted track (when the pointer is away from the screen), left and right play the previous
// and next track, + pauses and resumes, − stops, B goes back to the room.
#include "engine/soundplayer/sound_player.h"

#include <algorithm>

#include "engine/soundplayer/catalog.h"
#include "engine/soundplayer/controls.h"
#include "engine/soundplayer/game.h"
#include "engine/soundplayer/music.h"
#include "engine/soundplayer/stage.h"
#include "engine/soundplayer/track_list.h"
#include "engine/system/host_scene.h"

namespace engine::soundplayer {

namespace {

using engine::system::HostScene;

class SoundPlayerScene : public HostScene {
public:
    SoundPlayerScene();
    ~SoundPlayerScene() override;
    void update() override;
    void draw() override;
    u32 cursor() override { return cursor_; }
    void on_pushed(u32 button) override;

private:
    void leave();
    void play_index(int i);
    void step(int direction);
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
    // the room left a white screen behind (its circle wipe): fade in from it
    game::wipe_in(game::WIPE_FADE_WHITE);
    guest::log("soundplayer: open, %u tracks, playing %d", (u32)tracks.size(), current_index());
}

SoundPlayerScene::~SoundPlayerScene() {
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

void SoundPlayerScene::update() {
    if (music_->state() != logged_state_ || music_->current() != logged_sound_) {
        logged_state_ = music_->state();
        logged_sound_ = music_->current();
        static const char* const NAMES[] = {"stopped", "playing", "paused", "switching"};
        guest::log("soundplayer: %s, sound %d", NAMES[(int)logged_state_], (int)logged_sound_);
    }
    stage_->update(*music_);
    list_->update(*music_);
    int i = current_index();
    controls_->update(*music_, i >= 0 ? &catalog()[i] : nullptr);
    game::bottom_update(bottom_);
    game::cursor_update(cursor_);
    if (leaving_)
        return;

    int chosen = list_->take_chosen();
    if (chosen >= 0)
        play_index(chosen);
    switch (controls_->take_action()) {
    case Controls::PREVIOUS: step(-1); break;
    case Controls::NEXT: step(1); break;
    case Controls::STOP: music_->stop(); break;
    case Controls::PLAY_PAUSE:
        if (music_->state() == PlayState::PLAYING)
            music_->pause();
        else if (music_->state() == PlayState::PAUSED)
            music_->resume();
        else
            play_index(current_index() >= 0 ? current_index() : list_->highlighted());
        break;
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
        music_->state() == PlayState::PLAYING ? music_->pause() : music_->resume();
    else if (pressed & game::PAD_MINUS)
        music_->stop();
    else if ((pressed & game::PAD_A) && !game::pointer_on_screen())
        play_index(list_->highlighted());
}

void SoundPlayerScene::draw() {
    stage_->draw();
    list_->draw();
    controls_->draw();
    game::bottom_draw(bottom_);
    game::cursor_draw(cursor_);
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
    guest::log("soundplayer: back to the room");
    game::change_scene_with_wipe(guest(), game::NAME_LIVING_ROOM_SCENE, game::WIPE_FADE_WHITE);
}

HostScene* make_scene() { return new SoundPlayerScene(); }

const char* const CLASS_NAME = "scene::SoundPlayerScene";

void make_lasting() {
    engine::system::host_scene_factory(CLASS_NAME, &make_scene);
    engine::system::hookable_button_class(game::VTABLE_LAYOUT_BUTTON, game::LAYOUT_BUTTON_SLOTS);
    // layout::movie::PlayButton (the controls' play/pause disc): 21 virtuals, as layout::Button's
    engine::system::hookable_button_class(GA_lbl_80796B4C, 21);
}

}  // namespace

void boot() { engine::system::host_scene_boot(&make_lasting); }

u32 scene_factory() { return engine::system::host_scene_factory(CLASS_NAME, &make_scene); }

}  // namespace engine::soundplayer
