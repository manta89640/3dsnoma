// The Sound Player's tracks: the channel's music, found in its own sound archive.
#pragma once

#include <string>
#include <vector>

#include "runtime/runtime.h"

namespace engine::soundplayer {

struct Track {
    u32 sound_id;        // the nw4r::snd sound ID (its index in the archive's sound table), or a
                         // stream's (STREAM_ID + its place among the streams)
    std::u16string name; // the name the list shows
    bool jingle;         // a short piece (a logo, a Mii's feeling), not background music
    std::u16string heading;  // the list's heading over the tracks that have it ("": none)
    std::string stream;  // a native music stream (engine/audio/stream.h): its NSTM file
};

// Wii Sports' and Wii Sports Resort's music, staged from your own disc (tools/music/stage_music.py
// DATA: DATA/music/sports.tsv and its NSTM files), plays as native streams: listed after the
// channel's own, under a heading a game and one for its jingles. Their sound IDs start here.
const u32 STREAM_ID = 0x40000000;

// The archive (snd/douga.brsar, in memory from the start) holds 156 sounds: music, jingles and
// sound effects. Music is what plays on the archive's BGM player as a sequence with a SEQ_BGM_
// label (the channel's own naming), less the silent placeholder and the one sequence filed twice;
// jingles (under the heading ジングル) are the short pieces the channel plays for company logos
// and Miis' feelings. Their names are the channel's labels put into words (the archive has no
// other names). The island's music, the port's own (engine/wuhu/music.h), is listed with the
// places of the room's bar. The two games' music follows, if staged. Made once, the first time
// it's asked for, in the list's order: by where the channel (and each game) plays them.
const std::vector<Track>& catalog();
// the track with this sound ID, or none
const Track* track_of(u32 sound_id);

}  // namespace engine::soundplayer
