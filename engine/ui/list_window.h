// A scrolling list of names in the channel's own list window, to choose one: the look and the ways
// of the Sound Player's list of tracks (engine/soundplayer/track_list.cpp, which this is modelled
// on, without its music): the white window of the friend and message lists
// (Friend_Message/Friend_Sel_Window.lex, resized, its page counter hidden), rows of the message
// board's list (Message_Inbox/Message_List_Text.lex, each a layout::Button: grey text over a line,
// the room's cyan when the pointer or the D-pad's highlight is on it), and the profile screen's
// scroll tabs on the window's top and bottom edges (Profile/Profile_Up_Btn.lex,
// Profile_Down_Btn.lex: greyed when there is nothing more that way).
//
// The list scrolls a row at a time, smoothly, its rows a few buttons that take the next name as
// they leave at one end and come back at the other; rows near the window's edges fade rather than
// being cut off. A tab scrolls a row as its push starts; held, it keeps the list moving with the TV
// programme list's rhythm (a row in 13 60ths of a second, the next in 7, then one every 4: its
// frames, counted in time, engine/ui/drag_scroll.h), as the D-pad's up or down held does. A touch
// on the rows drags them, and a flick sends them coasting (engine/ui/drag_scroll.h). The D-pad's
// highlight shows as a row's focused look while the pointer is away from the screen (the D-pad sends
// it away, as a remote pointed off the screen); A then pushes that row, as the pointer would (its
// push animation and sound). A row's push ends in a choice (take_chosen()).
//
// It opens with the window's FadeIn and closes with its FadeOut. All its guest objects are made
// with it (hidden until opened) and deleted with it; the names are the list's own (host) strings,
// set into text boxes made big enough at the start, so nothing guest-side is made while it runs.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "runtime/runtime.h"

namespace engine::ui {

class ListWindow {
public:
    // cursor: the scene's layout::Cursor; group: the scene's exclusive button group (its buttons
    // join it: one push at a time); x, y: the window's centre, width: its width (layout units as
    // they show on the screen: 608 x 456, the origin at the centre, y up); four rows show at once
    ListWindow(u32 cursor, u32 group, f32 x, f32 y, f32 width);
    ~ListWindow();
    ListWindow(const ListWindow&) = delete;
    ListWindow& operator=(const ListWindow&) = delete;

    // the names it lists (while it is hidden)
    void set_entries(const std::vector<std::u16string>& names);
    int size() const;
    // it opens (FadeIn), the highlight on entry `highlight` (-1: where it was, the first entry the
    // first time), scrolled to show it; it closes (FadeOut)
    void open(int highlight = -1);
    void close();
    // opening, open or closing
    bool shown() const;
    // open: its rows can be pushed
    bool usable() const;
    // once a frame (after the scene's own parts, before its cursor's update)
    void update();
    void draw();
    // a button of the scene's starts its push / was pushed (the push animation ended): the
    // list's? (a row's push is a choice)
    bool on_push_start(u32 button);
    bool on_pushed(u32 button);
    // the entry chosen since the last call, or -1
    int take_chosen();
    // the entry the highlight is on
    int highlighted() const;

private:
    struct Impl;
    Impl* impl_;
};

// ---- the rows of the bottom screen's lists: this window's and the Sound Player's tracks
// (engine/soundplayer/track_list.cpp), both the message board's rows (Message_List_Text)

// A row's line (B_Line) is 1 layout unit high at rest and 2 focused or pushed (its height is keyed
// in the rows' Focus and Push animations): the Wii's 1 and 2 pixels, but 0.6 and 1.2 of the bottom
// screen's (engine/ui/screen.h BOTTOM), where a line that covers no pixel's middle isn't drawn at
// all. The height keys of `anim` (with their slopes) are made the bottom screen's 1 and 2 pixels,
// the words changed kept in `saved` (beside the colours' the lists change), to be put back, the
// last first, before the rows go: the channel caches the animations for every user of the layout,
// and the message board draws them at its own scale. Keys already changed (the rows share their
// animations) are left alone.
void row_lines_to_pixels(u32 anim, std::vector<std::pair<u32, u32>>& saved);
// How far (layout units, y up) to move a row on the bottom screen whose line's middle is at layout
// y `line_y`, so that the middle is a quarter of a pixel off a boundary between the screen's rows of
// pixels: 1 pixel high, the line then covers one pixel's middle, 2 high two, each a quarter of a
// pixel inside (none on an edge, where rounding would decide), wherever the list has scrolled to;
// the rows' text keeps its place on the pixels as they move too.
f32 row_pixel_snap(f32 line_y);

}  // namespace engine::ui
