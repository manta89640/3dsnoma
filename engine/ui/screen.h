// The 3DS's two screens for the native scenes: what a scene draws goes onto the top screen, unless
// it puts it on the bottom one.
//
// The channel draws one picture, its 640x456 frame, and gx_pica.cpp shows it on the top screen. A
// scene that puts some of its layouts on the bottom screen draws them, in the same frame, after
// to_bottom(): from there the GPU draws onto the bottom screen, and a region of the layouts' space
// fills it (its layouts placed there for it, at the size the region gives them: a region of
// 533x400 layout units shows them at 0.6 pixels a unit). to_top() goes back. The command goes into
// the GX stream as the channel's draws do (runtime/hw/gx/gx.h SCREEN_OPCODE), so it lands between
// them in order on the GPU's thread.
//
// While the scene has the bottom screen (claim() .. release()): the log console stops drawing there;
// the touch screen points into the region (the game's cursor gets the place touched, in layout
// units, and a tap presses what it is on, engine/input); and the bottom screen's layouts are made and
// drawn without the channel's 16:9 squeeze (squeeze(false) around their update and draw: the
// bottom screen's pixels are square at the region's 4:3), the top screen's with it.
#pragma once

#include "runtime/runtime.h"

namespace engine::ui::screen {

// A region of the layouts' space (units of the channel's 608x456 layouts, the origin at the
// middle, y up): its left and top edges, its width and height (4:3 for square pixels).
struct Region {
    f32 left, top, width, height;
};

// The native scenes' bottom screen: the bottom left of the 608x456 layouts (where the channel has
// its Home tab and the room its bar), 533x400 units (4:3), at 0.6 pixels a unit, so that the
// channel's parts read larger there than on the top screen; the middle of it across, BOTTOM_MIDDLE_X
// (37 units left of the layouts'), and down, BOTTOM_MIDDLE_Y.
constexpr Region BOTTOM{-304, 172, 1600.0f / 3, 400};
constexpr f32 BOTTOM_MIDDLE_X = BOTTOM.left + BOTTOM.width / 2, BOTTOM_MIDDLE_Y = BOTTOM.top - BOTTOM.height / 2;

void to_bottom(const Region& r);
void to_top();

void claim(const Region& r);
void release();
// a scene has the bottom screen (claim() .. release())
bool claimed();

// the channel's 16:9 squeeze of panes laid out for 4:3 (layout_sDrawInfo's location adjust): on for
// the top screen's layouts, off for the bottom screen's
void squeeze(bool on);

}  // namespace engine::ui::screen
