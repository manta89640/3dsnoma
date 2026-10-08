// A native music stream: an NSTM file (nstm.h) read from the SD card and mixed into the AX output's
// main bus, beside the channel's own sound (the Sound Player plays Wii Sports' and Wii Sports
// Resort's music with it). One piece at a time.
//
// Two threads: the game's thread opens, stops and pauses the piece and calls stream_pump() once a
// frame, which reads and decodes ahead into a ring of about two seconds; the AX frame (on the audio
// thread where there is one: runtime/hw/dsp_ax.cpp) takes 96 samples a frame from the ring with
// stream_mix(). The ring is single-producer, single-consumer; a new piece is a new generation the
// mixer jumps to, so neither thread waits for the other. A piece fades in and out over one AX frame
// (3 ms) as it starts, pauses, resumes and stops (no clicks), otherwise it cuts at once.
#pragma once

#include <string>

#include "engine/audio/nstm.h"
#include "noma_decomp.h"

namespace engine::audio {

// play the NSTM file at `path` from its start (the piece before stops); false if it can't be read
bool stream_play(const std::string& path);
void stream_stop();
void stream_pause(bool pause);

// a piece is current (playing or paused; not stopped, not ended)
bool stream_active();
bool stream_paused();
// the current piece's header (valid while stream_active())
const NstmHeader& stream_header();
// seconds of the piece heard so far, loops counted on (for its beat)
double stream_seconds();

// the game's thread, once a frame: read and decode ahead
void stream_pump();
// the AX frame: add n samples of the piece into main L and R (in 16-bit units)
void stream_mix(s32* left, s32* right, u32 n);

}  // namespace engine::audio
