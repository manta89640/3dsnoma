// Dragging a list with the touch screen (the bottom screen's lists: the Sound Player's tracks, the
// island's places). A touch that starts on the list's rows and moves up or down by more than a few
// units takes the list along under it, a row's pitch of movement a row, and letting go settles the
// list on the nearest row. A drag is not a tap: its A is taken back (input_cancel_tap), and the
// rows can't be pushed while it lasts and for the frames just after (the game's read of the
// letting go may come before or after the list sees it).
#pragma once

#include <algorithm>
#include <cmath>

#include "engine/input/remote.h"
#include "engine/ui/screen.h"

namespace engine::ui {

class DragScroll {
public:
    enum class Step { NONE, DRAG, LET_GO };

    // Once a frame, before the list places its rows: `area`, where on the screen a drag can start
    // (the rows: left and top edges, width and height, layout units); `pos`, the list's scroll
    // position in rows, which a drag sets (within 0..max_top); `pitch`, a row's. At LET_GO the
    // list settles itself on the nearest row.
    Step update(const screen::Region& area, f32& pos, f32 pitch, int max_top);
    // the rows aren't to be pushed (a drag, or just after one)
    bool holding() const { return dragging_ || hold_ > 0; }

private:
    static constexpr f32 SLOP = 10;  // how far a touch moves (layout units) before it is a drag
    static constexpr int HOLD = 3;   // frames the rows stay unpushable after a drag
    bool was_touching_ = false, armed_ = false, dragging_ = false;
    f32 start_y_ = 0, start_pos_ = 0;
    int hold_ = 0;
};

inline DragScroll::Step DragScroll::update(const screen::Region& area, f32& pos, f32 pitch, int max_top) {
    const guest::WiimoteInput& in = guest::wiimote_input();
    if (hold_ > 0)
        hold_--;
    const bool touching = in.touching && screen::claimed();
    Step step = Step::NONE;
    if (touching) {
        const screen::Region& r = screen::claimed_region();
        const f32 x = r.left + (f32)in.touch_x / 320.0f * r.width, y = r.top - (f32)in.touch_y / 240.0f * r.height;
        if (!was_touching_) {
            armed_ = x >= area.left && x <= area.left + area.width && y <= area.top && y >= area.top - area.height;
            start_y_ = y, start_pos_ = pos;
        }
        const f32 moved = y - start_y_;
        if (armed_ && !dragging_ && std::fabs(moved) > SLOP)
            dragging_ = true;
        if (dragging_) {
            // (from where it became a drag, so the list doesn't jump by the slop)
            const f32 rows = (moved - (moved > 0 ? SLOP : -SLOP)) / pitch;
            pos = std::min(std::max(start_pos_ + rows, 0.0f), (f32)std::max(max_top, 0));
            step = Step::DRAG;
        }
    } else if (dragging_) {
        dragging_ = false;
        hold_ = HOLD;
        guest::input_cancel_tap();
        step = Step::LET_GO;
    }
    if (!touching)
        armed_ = false;
    was_touching_ = touching;
    return step;
}

}  // namespace engine::ui
