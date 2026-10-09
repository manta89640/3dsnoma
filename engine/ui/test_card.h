// A calibration card for the 3DS's screens (the redesign's legibility rules: the smallest text and
// touch targets to allow are to be set from what reads and hits well on the console, not assumed):
// NOMA_TEST_CARD=1 shows it once, right after the room is first made. The top screen has the
// channel's text in its own font at 8 to 24 pixels high, the bottom one the same at 8 to 16 pixels
// and the channel's small buttons at 24 to 56 pixels square to tap (each push sounds and counts);
// B or the Home tab goes back to the room. Each row names its size in pixels of the screen it is
// on, so a look at the console says which sizes read, and a few taps which targets hit.
#pragma once

#include "runtime/runtime.h"

namespace engine::ui::test_card {

// at the start, with the channel's scene factories: the scene's guest objects that last
void boot();
// NOMA_TEST_CARD is set and the card hasn't been shown yet (true once)
bool take_request();
// the scene's guest lib::Scene::Factory
u32 scene_factory();

}  // namespace engine::ui::test_card
