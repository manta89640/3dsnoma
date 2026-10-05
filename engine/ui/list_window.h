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
// being cut off. Holding a tab, or the D-pad's up or down, keeps it moving with the TV programme
// list's rhythm (a row in 13 frames, the next in 7, then one every 4). The D-pad's highlight shows
// as a row's focused look while the pointer is away from the screen (or since the D-pad was last
// used); A then pushes that row, as the pointer would (its push animation and sound). A row's push
// ends in a choice (take_chosen()).
//
// It opens with the window's FadeIn and closes with its FadeOut. All its guest objects are made
// with it (hidden until opened) and deleted with it; the names are the list's own (host) strings,
// set into text boxes made big enough at the start, so nothing guest-side is made while it runs.
#pragma once

#include <string>
#include <vector>

#include "runtime/runtime.h"

namespace engine::ui {

class ListWindow {
public:
    // cursor: the scene's layout::Cursor; group: the scene's exclusive button group (its buttons
    // join it: one push at a time); x, y: the window's centre, width: its width (layout units as
    // they show on the screen: 608 x 456, the origin at the centre, y up); five rows show at once
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

}  // namespace engine::ui
