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
    float bpm = 0;        // quarter notes a minute now (the sequence can change it as it plays)
    double beats = 0;     // quarter notes since the piece started, with the fraction
};

class Music {
public:
    // The room's theme goes on playing into the screen, as the piece playing; crossfades are
    // made short (half a second) while the screen is open.
    Music();
    // Leaving: the fade length goes back, and a paused piece plays again (the room, made again
    // next, asks for its theme, and that request does nothing if its theme is the piece current,
    // paused or not).
    ~Music();

    // play this sound (an ID of the archive): fades the last piece out and this one in; the piece
    // playing again is resumed if paused, restarted if it had ended
    void play(u32 sound_id);
    void pause();
    void resume();
    void stop();

    PlayState state() const;
    // the piece current (playing, paused, or the one the player switches to), or NO_SOUND
    u32 current() const;
    static const u32 NO_SOUND = ~0u;

    Beat beat() const;

private:
    s32 saved_fade_frames_;
};

}  // namespace engine::soundplayer
