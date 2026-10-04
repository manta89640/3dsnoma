// The amp on the Sound Player's stage: drawn with GX from native code (the channel has no such
// model).
#pragma once

#include "runtime/runtime.h"

namespace engine::soundplayer {

class Amp {
public:
    Amp();
    ~Amp();
    // draw it in the stage's 3D pass: view = the camera's 3x4 view matrix (guest address of 12
    // f32), at = its place on the floor, yaw in degrees, pulse = 0..1 (the beat: the speaker moves)
    void draw(u32 view, const f32 at[3], f32 yaw, f32 pulse);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace engine::soundplayer
