// The island's way in and out in black (engine/wuhu). The room's bar buttons go to their scenes
// through the room's circle wipe, which closes on white (SceneTransit::Request, fn_8006FCA8: the
// room's input locked, layout::parts::Wipe's Out of kind 0, the circle, fn_80154CE8, then
// SceneTransit::Go when it ends); the channel's own way into something
// that fills the screen, its movies and Flash, is a fade to black instead (Wipe kind 2,
// Fade_Black). 出発 takes the island's To (engine::wuhu::TO_ISLAND) through the same Request, and
// its Out is made the fade to black. The island fades out to the room in black as well
// (ChangeSceneWithWipe with Fade_Black); the room, made again, then fades in as it does coming
// back from any of its screens, from white (Wipe::In of kind 4, Fade_White_Long: logged coming
// back from the island and from the Sound Player), which after black would be a cut to white and
// a fade: that one fade in is made Fade_Black's.
// NOMA_DEBUG_WIPE=1 logs every wipe In and Out with its kind.

#include <cstdlib>

#include "engine/ui/game.h"
#include "engine/wuhu/music.h"
#include "engine/wuhu/wuhu.h"
#include "noma_decomp.h"

namespace g {

u32 fn_8006FCA8_orig(u32 r3, u32 r4);
void fn_80154CE8_orig(u32 r3, u32 r4);
void layout_parts_Wipe_In_orig(u32 r3, u32 r4);

namespace {

namespace game = engine::ui::game;

// set while SceneTransit::Request runs for the island
bool s_to_island;

bool debug() {
    static const bool on = std::getenv("NOMA_DEBUG_WIPE") != nullptr;
    return on;
}

}  // namespace

// controller::living::SceneTransit::Request(To): for the island, the wipe out of the room is the
// black fade, and the island's music takes over from the room's
u32 fn_8006FCA8(u32 r3, u32 r4) {
    if (r4 != engine::wuhu::TO_ISLAND)
        return fn_8006FCA8_orig(r3, r4);
    engine::wuhu::music_depart();
    s_to_island = true;
    u32 r = fn_8006FCA8_orig(r3, r4);
    s_to_island = false;
    return r;
}

// layout::parts::Wipe::Out(kind): the circle is Fade_Black on the way to the island
void fn_80154CE8(u32 r3, u32 r4) {
    if (s_to_island && r4 == game::WIPE_CIRCLE)
        r4 = game::WIPE_FADE_BLACK;
    if (debug())
        guest::log("wipe: out, kind %u", (unsigned)r4);
    fn_80154CE8_orig(r3, r4);
}

// layout::parts::Wipe::In(kind): the room's fade in after the island is from black (a white one:
// Fade_White or Fade_White_Long; any other is left as it is)
void layout_parts_Wipe_In(u32 r3, u32 r4) {
    if (engine::wuhu::take_black_return() && (r4 == game::WIPE_FADE_WHITE || r4 == game::WIPE_FADE_WHITE_LONG)) {
        guest::log("wipe: in, kind %u after the island: Fade_Black", (unsigned)r4);
        r4 = game::WIPE_FADE_BLACK;
    } else if (debug()) {
        guest::log("wipe: in, kind %u", (unsigned)r4);
    }
    layout_parts_Wipe_In_orig(r3, r4);
}

}  // namespace g
