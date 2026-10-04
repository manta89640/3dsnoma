// The Sound Player's list of tracks: the left half of the screen.
#pragma once

#include "engine/soundplayer/catalog.h"
#include "engine/soundplayer/music.h"

namespace engine::soundplayer {

class TrackList {
public:
    // cursor: the scene's layout::Cursor; group: the scene's exclusive button group (its buttons
    // join it, so that one push at a time goes through)
    TrackList(u32 cursor, u32 group);
    ~TrackList();
    // once a frame, after the music's update: the rows' marks (playing / paused), the D-pad
    void update(const Music& music);
    // its layouts (after the 3D, before the controls and the cursor)
    void draw();
    // a button of the scene's starts its push / was pushed (the push animation ended): the
    // list's? (then it acts)
    bool on_push_start(u32 button);
    bool on_pushed(u32 button);
    // the track (an index into catalog()) chosen since the last call, or -1
    int take_chosen();
    // the track the list's highlight is on (A plays it when the pointer is away from the screen)
    int highlighted() const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace engine::soundplayer
