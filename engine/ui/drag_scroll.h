// Dragging a list with the touch screen (the bottom screen's lists: the Sound Player's tracks, the
// island's places). A touch that starts on the list's rows and moves up or down by more than a few
// units takes the list along under it, a row's pitch of movement a row. Let go while moving, the
// list coasts on, slowing as if by friction, and comes to rest on a row; let go still, the list's
// own glide settles it on the nearest row. A touch on the rows while the list coasts catches it
// (a drag from there, never a press). A drag is not a tap: the input layer is told as it becomes
// one (input_mark_drag: no press at its release, and the pointer stays where the touch began, so
// that nothing under the finger takes the focus), its A is taken back at the release as well
// (input_cancel_tap, a second guard), and the rows can't be pushed while it lasts, while the list
// coasts, and for a moment after.
//
// The lists' timers count 60ths of a second of the platform's time (guest::timebase), not the
// game's frames, so that at the 3DS's 40-55 fps the lists keep the Wii's pace (as the way out does,
// runtime/patches/wii_parts.cpp app_Exit_Update); a list's Ticker gives it that time, once a frame.
#pragma once

#include <algorithm>
#include <cmath>

#include "engine/input/remote.h"
#include "engine/ui/screen.h"

namespace engine::ui {

// The time since its last tick, in 60ths of a second, at most MAX_STEP: a frame held up (loading)
// neither ends a glide at once nor fires a held repeat's steps all together. The first tick gives
// one.
class Ticker {
public:
    static constexpr f32 MAX_STEP = 4;
    f32 tick() {
        const u64 now = guest::timebase();
        const f32 dt = last_ ? (f32)((f64)(now - last_) * 60.0 / (f64)guest::TB_HZ) : 1.0f;
        last_ = now;
        return std::min(dt, MAX_STEP);
    }

private:
    u64 last_ = 0;
};

// The touch screen touched, while a scene has the bottom screen, and where: the middle of the pixel
// touched, in the claimed region's layout units (y up), as the input layer maps it for the game's
// pointer.
inline bool touch_point(f32& x, f32& y) {
    const guest::WiimoteInput& in = guest::wiimote_input();
    if (!in.touching || !screen::claimed())
        return false;
    const screen::Region& r = screen::claimed_region();
    x = r.left + ((f32)in.touch_x + 0.5f) / 320.0f * r.width;
    y = r.top - ((f32)in.touch_y + 0.5f) / 240.0f * r.height;
    return true;
}

class DragScroll {
public:
    // NONE: nothing; DRAG, COAST: a drag or the coast after one moved the list (pos), which glides
    // no more; LET_GO: the drag or the coast has ended, and the list settles itself on the nearest
    // row with its glide (a coast has come to rest on one); TAP: a touch that began on the rows
    // ended without becoming a drag (the game's cursor presses the row it is on; tap_y() is where,
    // for a row that is no button)
    enum class Step { NONE, DRAG, COAST, LET_GO, TAP };

    // Once a frame, before the list places its rows: `area`, where on the screen a drag can start
    // (the rows: left and top edges, width and height, layout units), given to the input layer
    // every frame; `pos`, the list's scroll position in rows, which a drag or a coast sets (within
    // 0..max_top); `pitch`, a row's; `dt`, the 60ths of a second since the last update (the list's
    // Ticker).
    Step update(const screen::Region& area, f32& pos, f32 pitch, int max_top, f32 dt);
    // the rows aren't to be pushed (a drag, a coast, or just after one)
    bool holding() const { return dragging_ || coasting_ || hold_ > 0; }
    // Another input (a tab, the D-pad) takes the list: a coast stops where it is. True if one did
    // (the list then settles itself on a row).
    bool halt() {
        const bool was = coasting_;
        coasting_ = false;
        return was;
    }
    // The list (re)opens or closes: a drag or a coast it was in is forgotten (its update isn't called
    // while the list is shut), and a touch going on now is none of its rows' drags.
    void reset() {
        dragging_ = coasting_ = armed_ = false;
        hold_ = 0, n_samples_ = 0;
        was_touching_ = guest::wiimote_input().touching;
    }
    // the rows unpushable for a moment: a tap the list took for itself (on a row that is no button)
    // has its A still to come, which isn't to push the row the list moves under it
    void hold() { hold_ = HOLD; }
    // where the last TAP was (layout units, y up)
    f32 tap_y() const { return tap_y_; }

private:
    static constexpr f32 SLOP = 10;  // how far a touch moves (layout units) before it is a drag
    static constexpr f32 HOLD = 3;   // 60ths of a second the rows stay unpushable after a drag
    // The coast. The finger's speed as it let go, over the last SAMPLE_SPAN 60ths (in rows a 60th,
    // at most MAX_SPEED; slower than FLING_MIN, about 190 pixels a second, no coast), would carry
    // the list speed / -ln FRICTION rows on, were it slowed by FRICTION a 60th: the row nearest
    // there (the next one at least, within the list) is where the list goes, setting off at the
    // finger's speed and slowing evenly, as by friction, to rest on that row (in 6 to 90 60ths).
    static constexpr int SAMPLES = 8;
    static constexpr f32 SAMPLE_SPAN = 6, FLING_MIN = 0.1f, MAX_SPEED = 2.5f, FRICTION = 0.95f;
    struct Sample {
        f32 y;
        u64 time;
    };

    void register_area(const screen::Region& area) const;
    void start_drag(f32 y, f32 pos, bool seed);
    f32 release_speed(f32 pitch) const;
    bool start_coast(f32 pos, f32 pitch, f32 top);

    bool was_touching_ = false, armed_ = false, dragging_ = false, coasting_ = false;
    f32 start_y_ = 0, start_pos_ = 0, last_y_ = 0, tap_y_ = 0;
    u64 last_time_ = 0;  // (when last_y_ was seen)
    f32 hold_ = 0;
    Sample samples_[SAMPLES] = {};
    int n_samples_ = 0;
    f32 coast_from_ = 0, coast_to_ = 0, coast_t_ = 0, coast_len_ = 0;
};

// The rows' rectangle in the touch screen's pixels, those whose middles map inside it (as the
// input layer maps a touch to layout units): a touch that begins there presses only as a tap.
inline void DragScroll::register_area(const screen::Region& area) const {
    if (!screen::claimed())
        return;
    const screen::Region& r = screen::claimed_region();
    const f32 x0 = (area.left - r.left) / r.width * 320.0f - 0.5f;
    const f32 x1 = (area.left + area.width - r.left) / r.width * 320.0f - 0.5f;
    const f32 y0 = (r.top - area.top) / r.height * 240.0f - 0.5f;
    const f32 y1 = (r.top - (area.top - area.height)) / r.height * 240.0f - 0.5f;
    auto pixel = [](f32 v, f32 most) { return (u16)std::min(std::max(v, 0.0f), most); };
    guest::input_set_drag_area(pixel(std::ceil(x0), 320), pixel(std::ceil(y0), 240), pixel(std::floor(x1) + 1, 320),
                               pixel(std::floor(y1) + 1, 240));
}

// the touch is a drag from here on: the list follows it from where it is now (so that it doesn't
// jump by the slop); `seed`: the touch was seen at the update before, its first speed sample (a
// quick flick crosses the slop and lets go within an update or two)
inline void DragScroll::start_drag(f32 y, f32 pos, bool seed) {
    dragging_ = true;
    coasting_ = false;
    start_y_ = y, start_pos_ = pos;
    n_samples_ = 0;
    if (seed)
        samples_[n_samples_++] = {last_y_, last_time_};
    guest::input_mark_drag();
}

// The finger's speed as it let go, in rows a 60th of a second (the list's way: up the screen is on
// down the list), from the oldest of the samples of the last SAMPLE_SPAN 60ths to the last (the
// two last at least).
inline f32 DragScroll::release_speed(f32 pitch) const {
    if (n_samples_ < 2)
        return 0;
    const Sample& last = samples_[(n_samples_ - 1) % SAMPLES];
    const u64 span = (u64)(SAMPLE_SPAN * (f32)guest::TB_HZ / 60.0f);
    const Sample* first = &samples_[(n_samples_ - 2) % SAMPLES];
    for (int k = n_samples_ - 3; k >= std::max(0, n_samples_ - SAMPLES); k--) {
        const Sample& s = samples_[k % SAMPLES];
        if (last.time - s.time > span)
            break;
        first = &s;
    }
    const f32 t = (f32)((f64)(last.time - first->time) * 60.0 / (f64)guest::TB_HZ);
    if (t <= 0 || t > 2 * SAMPLE_SPAN)
        return 0;  // (frames too far apart to tell)
    const f32 v = (last.y - first->y) / t / pitch;
    return std::min(std::max(v, -MAX_SPEED), MAX_SPEED);
}

// let go: a coast if the finger was moving
inline bool DragScroll::start_coast(f32 pos, f32 pitch, f32 top) {
    const f32 v = release_speed(pitch);
    if (std::fabs(v) < FLING_MIN)
        return false;
    f32 to = std::round(pos - v / std::log(FRICTION));
    to = v > 0 ? std::max(to, std::floor(pos) + 1) : std::min(to, std::ceil(pos) - 1);
    to = std::min(std::max(to, 0.0f), top);
    const f32 dist = to - pos;
    if (dist * v <= 0)
        return false;  // (at the list's end already)
    // pos = to - dist (1 - t / length)^2 sets off at 2 dist / length rows a 60th: the finger's speed
    coasting_ = true;
    coast_from_ = pos, coast_to_ = to, coast_t_ = 0;
    coast_len_ = std::min(std::max(2 * dist / v, 6.0f), 90.0f);
    return true;
}

inline DragScroll::Step DragScroll::update(const screen::Region& area, f32& pos, f32 pitch, int max_top, f32 dt) {
    hold_ = std::max(hold_ - dt, 0.0f);
    register_area(area);
    const f32 top = (f32)std::max(max_top, 0);
    f32 x, y;
    const bool touching = touch_point(x, y);
    Step step = Step::NONE;
    bool let_go = false;
    if (touching) {
        if (!was_touching_) {
            armed_ = x >= area.left && x <= area.left + area.width && y <= area.top && y >= area.top - area.height;
            start_y_ = y, start_pos_ = pos;
            if (armed_ && coasting_)
                start_drag(y, pos, false);  // the coast caught
        }
        if (armed_ && !dragging_ && std::fabs(y - start_y_) > SLOP)
            start_drag(y, pos, was_touching_);
        if (dragging_) {
            pos = std::min(std::max(start_pos_ + (y - start_y_) / pitch, 0.0f), top);
            samples_[n_samples_ % SAMPLES] = {y, guest::timebase()};
            n_samples_++;
            step = Step::DRAG;
        }
        last_y_ = y, last_time_ = guest::timebase();
    } else if (dragging_) {
        dragging_ = false;
        hold_ = HOLD;
        guest::input_cancel_tap();
        let_go = true;
        step = start_coast(pos, pitch, top) ? Step::COAST : Step::LET_GO;
    } else if (was_touching_ && armed_) {
        tap_y_ = last_y_;
        step = Step::TAP;
    }
    // (a coast goes on under a touch elsewhere than on the rows: a tab's or a control's)
    if (coasting_ && !let_go) {
        coast_t_ += dt;
        const f32 u = std::min(coast_t_ / coast_len_, 1.0f);
        pos = coast_to_ - (coast_to_ - coast_from_) * (1 - u) * (1 - u);
        step = Step::COAST;
        if (u >= 1) {
            coasting_ = false;
            hold_ = HOLD;
            step = Step::LET_GO;
        }
    }
    if (!touching)
        armed_ = false;
    was_touching_ = touching;
    return step;
}

}  // namespace engine::ui
