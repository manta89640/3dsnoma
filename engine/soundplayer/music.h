// The Sound Player's playback: the channel's own BGM player plays the tracks, and the music's
// sequencer tells the Miis where the beat is.
#pragma once

#include "runtime/runtime.h"

namespace engine::soundplayer {

enum class PlayState {
    STOPPED,    // nothing plays (or a piece that doesn't loop has ended)
    PLAYING,
    PAUSED,     // paused (or pausing: its fade-out)
    SWITCHING,  // the next piece waits for the last one's fade-out
};

// where the music is, read once a frame from the sequencer of the piece playing
struct Beat {
    bool valid = false;   // a sequence plays (a wave jingle has no sequencer: no beat)
    f32 bpm = 0;          // quarter notes a minute now (the sequence can change it as it plays)
    double beats = 0;     // quarter notes since the piece started, with the fraction
    u32 bar = 4;          // quarter notes a bar (engine::wuhu::music_bar_beats)
};

// Pieces start and stop cleanly, at once: no crossfades or fades in the player (a new piece cuts
// the last one off and starts from its first note; pause and resume take effect at once).
class Music {
public:
    // The room's theme goes on playing into the screen, as the piece playing.
    Music() {}
    // Leaving (also called by the scene as its wipe starts, so the music stops with the screen):
    // the piece the player started stops at once; the room's theme, if it still plays, plays on
    // into the room, and is resumed if it was paused (the room, made again next, asks for its
    // theme, and that request does nothing if its theme is the piece current, paused or not).
    ~Music() { leave(); }
    void leave();

    // play this sound (an ID of the archive), cutting off the last piece; the piece current is
    // resumed if paused, played again if it had ended, and left alone if it plays
    void play(u32 sound_id);
    void pause();
    void resume();
    void stop();

    PlayState state() const;
    // the piece current (playing, paused, or the one the player switches to), or NO_SOUND
    u32 current() const;
    static const u32 NO_SOUND = ~0u;

    Beat beat() const;
};

}  // namespace engine::soundplayer
