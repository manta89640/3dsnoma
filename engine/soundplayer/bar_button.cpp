// The Sound Player's button on the room's bottom bar. runtime/patches/menus.cpp makes it as the bar
// makes its others, from いろんな間's layout (Move_Btn_Ironna_Btn.lex): the same face, frame,
// animations and sounds. Here it gets its own icon and label, in the bar's two-tone style
// (engine/ui/bar_button.h): a pair of beamed eighth notes drawn here, and サウンド set from the
// channel's font.
#include "engine/soundplayer/sound_player.h"

#include <cmath>

#include "engine/soundplayer/angles.h"
#include "engine/ui/art.h"
#include "engine/ui/bar_button.h"

namespace engine::soundplayer {

namespace art = engine::ui::art;

namespace {

using art::Coverage;

// ---- the icon

// The notes are as heavy as the bar's icons (engine/ui/bar_button.h): strokes of 5 to 6 px, the
// ink within 3 px of the edges.
const u32 ICON_SIZE = engine::ui::BAR_ICON_SIZE;

// A pair of eighth notes joined by a beam (♫): two note heads (ellipses tilted up to the right),
// their stems up from the heads' right sides, and the beam across the stems' tops, rising to the
// right.
Coverage draw_icon() {
    const f32 tilt = deg_to_rad(-22.0f);
    const f32 head_rx = 9.6f, head_ry = 7.2f, stem = 5.5f, beam = 9.5f;
    const art::Ellipse heads[2] = {{12.0f, 39.0f, head_rx, head_ry, std::cos(tilt), std::sin(tilt)},
                                   {37.5f, 35.0f, head_rx, head_ry, std::cos(tilt), std::sin(tilt)}};
    // the stems' right edges, just inside the heads' right ends
    const f32 stem_right[2] = {heads[0].cx + head_rx * 0.93f, heads[1].cx + head_rx * 0.93f};
    // the beam's top edge, from the left stem's left edge to the right stem's right edge
    const f32 beam_x0 = stem_right[0] - stem, beam_x1 = stem_right[1], beam_y0 = 6.5f, beam_y1 = 2.5f;
    auto beam_top = [&](f32 x) { return beam_y0 + (beam_y1 - beam_y0) * (x - beam_x0) / (beam_x1 - beam_x0); };
    auto inside = [&](f32 x, f32 y) {
        if (heads[0].inside(x, y) || heads[1].inside(x, y))
            return true;
        if (x < beam_x0 || x > beam_x1 || y < beam_top(x))
            return false;
        if (y <= beam_top(x) + beam)
            return true;
        for (u32 i = 0; i < 2; i++)
            if (x >= stem_right[i] - stem && x <= stem_right[i] && y <= heads[i].cy)
                return true;
        return false;
    };
    art::Samples icon(ICON_SIZE, ICON_SIZE);
    icon.mark(0.0f, 0.0f, (f32)ICON_SIZE, (f32)ICON_SIZE, inside);
    return icon.coverage();
}

engine::ui::BarButtonArt s_art("soundplayer", &draw_icon, u"サウンド");

}  // namespace

void reserve_bar_button() { s_art.reserve(); }

void dress_bar_button(u32 button) { s_art.dress(button); }

}  // namespace engine::soundplayer
