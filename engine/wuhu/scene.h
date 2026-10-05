// The island's scene (scene::IslandScene): the shell around the island's content (content.h).
//
//   LOADING    the room's fade has left the screen black: the scene keeps it black (the loading
//              rings over it once the load takes more than half a second) while the content loads
//   FADING_IN  the content is ready: the black fades away (Fade_Black In, 16 frames), the Home tab
//              comes up at the bottom left
//   PLAYING    the island runs; the arrival caption shows for a few seconds; the Home tab, B or
//              START (+) open the exploration menu
//   MENU       the menu (engine/ui/menu), the island frozen under its veil: 探検をつづける (B,
//              START) goes back to PLAYING, ワープ to PLACES, 部屋にもどる to CONFIRM
//   PLACES     the list of places (engine/ui/list_window) in the menu's place, the back tab at the
//              bottom left: a place goes to WARP_OUT, the back tab or B back to MENU
//   CONFIRM    the window asks 「部屋にもどります。よろしいですか？」 in the menu's place; いいえ
//              goes back to MENU, はい to LEAVING
//   WARP_OUT   the screen fades to black (Fade_Black Out); the content then puts the Mii at the
//              place, and after a moment in black WARP_IN
//   WARP_IN    the black fades away; then PLAYING, the place's name as the caption
//   LEAVING    the screen fades to black (Fade_Black Out) and the room is made again, fading in
//              from black (runtime/patches/wipes.cpp)
#pragma once

#include <vector>

#include "engine/system/host_scene.h"
#include "engine/ui/caption.h"
#include "engine/ui/confirm.h"
#include "engine/ui/list_window.h"
#include "engine/ui/menu.h"
#include "engine/wuhu/content.h"
#include "engine/wuhu/places.h"

namespace engine::wuhu {

class IslandScene : public engine::system::HostScene {
public:
    IslandScene();
    ~IslandScene() override;
    void update() override;
    void draw() override;
    u32 cursor() override { return cursor_; }
    void on_push_start(u32 button) override;
    void on_pushed(u32 button) override;

    enum class State { LOADING, FADING_IN, PLAYING, MENU, PLACES, CONFIRM, WARP_OUT, WARP_IN, LEAVING };

private:
    void set_state(State s);
    void open_menu();
    void close_menu();
    void open_places();
    void back_to_menu();
    void start_warp(int place);
    void leave();

    State state_ = State::LOADING;
    int state_frames_ = 0;
    u32 cursor_ = 0;
    u32 group_ = 0;       // the exclusive button group: one push at a time
    u32 bottom_ = 0;      // the Home tab or the back tab, bottom left
    u32 rings_ = 0;       // Progress_Black: the loading rings
    u32 black_ = 0;       // the wipe's Fade_Black layout, drawn by the scene while it loads
    engine::ui::Caption* caption_ = nullptr;  // the arrival's, then a warp's place
    engine::ui::Caption* prompt_ = nullptr;   // over the list of places
    engine::ui::Menu* menu_ = nullptr;
    engine::ui::ListWindow* places_list_ = nullptr;
    engine::ui::Confirm* confirm_ = nullptr;
    IslandContent* content_ = nullptr;
    std::vector<Place> places_;  // the package's, once the content is ready
    int place_ = -1;             // the place being warped to
    bool warped_ = false;        // WARP_OUT: the content has been put at the place
    int black_hold_ = 0;         // WARP_OUT: frames in black since
    u64 load_start_us_ = 0;
    int rings_frames_ = -1;  // frames the rings have shown (-1: not shown)
    int black_frames_ = 0;   // frames the scene still draws its black after a fade in started
    u32 heap_at_start_ = 0;
};

}  // namespace engine::wuhu
