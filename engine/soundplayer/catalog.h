// The Sound Player's tracks: the channel's music, found in its own sound archive.
#pragma once

#include <string>
#include <vector>

#include "runtime/runtime.h"

namespace engine::soundplayer {

struct Track {
    u32 sound_id;        // the nw4r::snd sound ID (its index in the archive's sound table)
    u32 label;           // guest address of its label in the archive ("SEQ_BGM_MAIN_01")
    std::u16string name; // the name the list shows
    bool jingle;         // a short piece (a logo, a Mii's feeling), not background music
    bool loops;          // plays until stopped (else it ends by itself)
};

// The archive (snd/douga.brsar, in memory from the start) holds 156 sounds: music, jingles and
// sound effects. Music is what plays on the archive's BGM player as a sequence with a SEQ_BGM_
// label (the channel's own naming), less the silent placeholder and the one sequence filed twice;
// jingles are the short pieces the channel plays for company logos and Miis' feelings. Their
// names are the channel's labels put into words (the archive has no other names). Made once,
// the first time it's asked for, in the list's order: by where the channel plays them.
const std::vector<Track>& catalog();

}  // namespace engine::soundplayer
