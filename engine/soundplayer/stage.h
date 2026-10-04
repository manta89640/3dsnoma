// The Sound Player's stage: the household's Miis at a table with an amp, rocking out to the music
// (3D, on the right of the screen).
#pragma once

#include "engine/soundplayer/music.h"

namespace engine::soundplayer {

class Stage {
public:
    Stage();
    ~Stage();
    // once a frame, first in the scene's update (cameras, the Miis' behaviours and animation)
    void update(const Music& music);
    // first in the scene's draw: the 3D (the 2D layouts go over it)
    void draw();

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace engine::soundplayer
