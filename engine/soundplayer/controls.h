// The Sound Player's controls and title: under and over the stage, the right half of the screen.
#pragma once

#include "engine/soundplayer/catalog.h"
#include "engine/soundplayer/music.h"

namespace engine::soundplayer {

class Controls {
public:
    Controls(u32 cursor, u32 group);
    ~Controls();
    // once a frame: the play/pause button's look, the title (the track playing, or none)
    void update(const Music& music, const Track* playing);
    void draw();
    bool on_pushed(u32 button);
    enum Action { NONE, PREVIOUS, PLAY_PAUSE, NEXT, STOP };
    // the control pushed since the last call
    Action take_action();

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace engine::soundplayer
