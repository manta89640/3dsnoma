// The island's scene (scene::IslandScene): the shell around the island's content (content.h).
//
//   LOADING    the room's fade has left the screen black: the scene keeps it black (the loading
//              rings over it once the load takes more than half a second) while the content loads
//   FADING_IN  the content is ready: the black fades away (Fade_Black In, 16 frames), the Home tab
//              comes up at the bottom left
//   PLAYING    the island runs; the arrival caption shows for a few seconds; the Home tab, B or
//              START (+) open the window
//   MENU       the window asks 「部屋にもどります。よろしいですか？」, the island frozen; いいえ goes
//              back to PLAYING, はい to LEAVING
//   LEAVING    the screen fades to black (Fade_Black Out) and the room is made again, fading in
//              from black (runtime/patches/wipes.cpp)
#pragma once

#include "engine/system/host_scene.h"
#include "engine/ui/caption.h"
#include "engine/ui/confirm.h"
#include "engine/wuhu/content.h"

namespace engine::wuhu {

class IslandScene : public engine::system::HostScene {
public:
    IslandScene();
    ~IslandScene() override;
    void update() override;
    void draw() override;
    u32 cursor() override { return cursor_; }
    void on_pushed(u32 button) override;

    enum class State { LOADING, FADING_IN, PLAYING, MENU, LEAVING };

private:
    void set_state(State s);
    void open_menu();
    void leave();

    State state_ = State::LOADING;
    int state_frames_ = 0;
    u32 cursor_ = 0;
    u32 group_ = 0;       // the exclusive button group: one push at a time
    u32 bottom_ = 0;      // the Home tab, bottom left
    u32 rings_ = 0;       // Progress_Black: the loading rings
    u32 black_ = 0;       // the wipe's Fade_Black layout, drawn by the scene while it loads
    engine::ui::Caption* caption_ = nullptr;
    engine::ui::Confirm* confirm_ = nullptr;
    IslandContent* content_ = nullptr;
    u64 load_start_us_ = 0;
    int rings_frames_ = -1;  // frames the rings have shown (-1: not shown)
    int black_frames_ = 0;   // frames the scene still draws its black after the fade in started
    u32 heap_at_start_ = 0;
};

}  // namespace engine::wuhu
