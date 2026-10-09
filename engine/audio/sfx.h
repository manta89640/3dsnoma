// Native sound effects: short sounds from an NSFX bank (nsfx.h) played from memory and mixed into
// the AX output's main bus, beside the channel's own sound and the music stream (stream.h). Wuhu
// Island plays Wii Sports Resort's effects with it (engine/wuhu/sounds.h): the channel's own sound
// system can play only the sounds of its own archive, and WSR's are not in it.
//
// Two threads, as the stream's: the game's thread plays, changes and stops sounds through a queue of
// commands (single-producer, single-consumer: it never waits for the mixer); the AX frame (on the
// audio thread where there is one: runtime/hw/dsp_ax.cpp, through stream_mix) takes the commands at
// its start and mixes the voices, 96 samples a frame. A voice plays a wave at its pitch (linear
// interpolation from the wave's own rate to the output's 32 kHz), decoding its DSP-ADPCM a frame of 14
// samples at a time as it goes; its gain moves to a new value over one AX frame (3 ms: no clicks), a
// loop starts from nothing over its first frame, and a stop fades over the time asked for. The bank's
// memory is the caller's: sfx_release, before it goes, stops whatever plays from it and waits for an
// AX frame the mixer may be in the middle of (the one lock, which the mixer only tries: it never
// waits for the game's thread).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "engine/audio/nsfx.h"
#include "noma_decomp.h"

namespace engine::audio {

class SfxBank {
public:
    // the bank in `bytes` (an NSFX file, checked whole); null, and why in *error, if it can't be read
    static std::unique_ptr<SfxBank> load(std::vector<u8> bytes, std::string* error);
    // (stops what plays from it first: sfx_release)
    ~SfxBank();
    SfxBank(const SfxBank&) = delete;
    SfxBank& operator=(const SfxBank&) = delete;

    // a sound's index by its name, -1 if the bank has none of that name
    int find(const char* name) const;
    u32 sounds() const { return h_->n_sounds; }
    u32 waves() const { return h_->n_waves; }
    u32 bytes() const { return (u32)bytes_.size(); }
    bool loops(int sound) const;
    const char* name(int sound) const;

private:
    friend struct SfxAccess;
    SfxBank() = default;
    std::vector<u8> bytes_;
    const NsfxHeader* h_ = nullptr;
    const NsfxSound* sounds_ = nullptr;
    const NsfxVariant* variants_ = nullptr;
    const NsfxLayer* layers_ = nullptr;
    const NsfxWave* waves_ = nullptr;
    const u8* data_ = nullptr;
};

// a sound playing (all of its layers), 0: none
using SfxHandle = u32;

// Sound `sound` of `bank` (one of its variants at random, its gain and pitch varied within its
// spreads) at `gain` and `pitch` times its own; its handle, 0 if it can't start (no such sound, the
// queue full). A sound that doesn't loop stops by itself at its end; a loop plays until sfx_stop.
SfxHandle sfx_play(const SfxBank& bank, int sound, f32 gain = 1.0f, f32 pitch = 1.0f);
// a sound's gain and pitch from now on, times its own as it started (a loop's, as an engine follows
// its speed); reached over the next AX frame
void sfx_set(SfxHandle h, f32 gain, f32 pitch);
// it fades out over `fade` seconds and stops (0: over the next AX frame)
void sfx_stop(SfxHandle h, f32 fade = 0.05f);
// everything playing from `bank` stops at once, and no AX frame is still reading it when this
// returns (before the bank's memory goes)
void sfx_release(const SfxBank& bank);

// the AX frame: add n samples of the voices into main L and R (in 16-bit units)
void sfx_mix(s32* left, s32* right, u32 n);

// for tests: the voices playing as the last AX frame ended
u32 sfx_voices();

}  // namespace engine::audio
