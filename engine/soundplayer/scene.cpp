// The Sound Player's scene: what it is made of, its frame, and the way in and out.
#include "engine/soundplayer/sound_player.h"

#include "engine/soundplayer/game.h"
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

    u32 cursor_ = 0;
    u32 bottom_ = 0;  // the Home tab, bottom left
    bool leaving_ = false;
};

SoundPlayerScene::SoundPlayerScene() {
    cursor_ = g::operator_new(game::CURSOR_SIZE);
    game::cursor_ctor(cursor_);
    bottom_ = g::operator_new(game::BOTTOM_SIZE);
    game::bottom_ctor(bottom_, cursor_);
    game::bottom_set_mode(bottom_, game::BOTTOM_HOME, false);
    engine::system::hook_button(bottom_ + game::BOTTOM_HOME_BTN_02);
    // the room left behind a white screen (its circle wipe): fade in from it
    game::wipe_in(game::WIPE_FADE_WHITE);
    guest::log("soundplayer: open (cursor %08X, bottom %08X, home vtable %08X)", cursor_, bottom_,
               guest::load32(bottom_ + game::BOTTOM_HOME_BTN_02));
}

SoundPlayerScene::~SoundPlayerScene() {
    game::bottom_dtor_delete(bottom_);
    game::cursor_dtor_delete(cursor_);
    guest::log("soundplayer: closed");
}

void SoundPlayerScene::update() {
    game::cursor_update(cursor_);
    game::bottom_update(bottom_);
    if (!leaving_ && (game::buttons_triggered() & game::PAD_B))
        game::bottom_press_back(bottom_, cursor_);
}

void SoundPlayerScene::draw() {
    game::bottom_draw(bottom_);
    game::cursor_draw(cursor_);
}

void SoundPlayerScene::on_pushed(u32 button) {
    if (button == bottom_ + game::BOTTOM_HOME_BTN_02)
        leave();
}

// back to the room: a white fade, then the room is made again (its music comes back by itself)
void SoundPlayerScene::leave() {
    if (leaving_)
        return;
    leaving_ = true;
    guest::log("soundplayer: back to the room (%s)", guest::read_string(game::NAME_LIVING_ROOM_SCENE, 32).c_str());
    game::change_scene_with_wipe(guest(), game::NAME_LIVING_ROOM_SCENE, game::WIPE_FADE_WHITE);
}

HostScene* make_scene() { return new SoundPlayerScene(); }

const char* const CLASS_NAME = "scene::SoundPlayerScene";

void make_lasting() {
    engine::system::host_scene_factory(CLASS_NAME, &make_scene);
    engine::system::hookable_button_class(game::VTABLE_LAYOUT_BUTTON, game::LAYOUT_BUTTON_SLOTS);
}

}  // namespace

void boot() { engine::system::host_scene_boot(&make_lasting); }

u32 scene_factory() { return engine::system::host_scene_factory(CLASS_NAME, &make_scene); }

}  // namespace engine::soundplayer
