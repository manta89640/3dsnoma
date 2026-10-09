// NSFX: the port's own bank of short sounds, effects ready to play natively from memory and mix into
// the AX output beside the channel's own sound (engine/audio/sfx.h). tools/wuhu/stage_sounds.py makes
// Wuhu Island's (DATA/wuhu/sounds.nsfx) from Wii Sports Resort's sound archive, keeping the disc's
// own DSP-ADPCM waves as they are (no second encoding).
//
// Layout (little-endian): the header, then the tables one after another -- NsfxSound[n_sounds],
// NsfxVariant[n_variants], NsfxLayer[n_layers], NsfxWave[n_waves] --, the sounds' names (NUL-ended,
// at `strings`), and the waves' data (at `data`, each wave's 32-byte aligned).
//
// A sound is played as one of its variants, chosen at random (the recordings WSR's sequence picks
// between: two of each footstep), and a variant as its layers together (the notes its sequence
// starts at once), each a wave at its own gain and pitch. A play's gain and pitch are varied at
// random within the sound's spreads, as the sequence's random volume and pitch bend would. A wave
// that loops plays from 0 to `samples` and then from loop_start on, on and on: the decoder goes back
// to the frame holding loop_start (loop_start / 14) with the history at that frame's start
// (loop_hist1/2: what decoding the wave from its start leaves there) and skips the samples before
// loop_start in it (as engine/audio/nstm.h's streams do).
#pragma once

#include <cstdint>

namespace engine::audio {

const uint32_t NSFX_MAGIC = 0x5846534E;  // "NSFX"
const uint16_t NSFX_VERSION = 1;

struct NsfxHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t n_sounds, n_variants, n_layers, n_waves;
    uint16_t reserved;
    uint32_t strings;    // the names, from the file's start
    uint32_t data;       // the waves' data, from the file's start
    uint32_t data_size;
    uint32_t reserved2;
};
static_assert(sizeof(NsfxHeader) == 32, "NSFX header");

struct NsfxSound {
    uint32_t name;                      // in the strings
    uint16_t first_variant, n_variants;
    float gain_spread;                  // a play's gain times between 1/spread and spread (1: none)
    float pitch_spread;                 // and its pitch up to this many semitones either way (0: none)
    uint8_t loops;                      // its waves loop: it plays until it is stopped
    uint8_t reserved[3];
};
static_assert(sizeof(NsfxSound) == 20, "NSFX sound");

struct NsfxVariant {
    uint16_t first_layer, n_layers;
};
static_assert(sizeof(NsfxVariant) == 4, "NSFX variant");

struct NsfxLayer {
    uint16_t wave;
    uint16_t reserved;
    float gain;   // linear: nw4r::snd's level for the note (1: the wave at full scale)
    float pitch;  // the note's pitch against the wave's own rate (1: as recorded)
};
static_assert(sizeof(NsfxLayer) == 12, "NSFX layer");

enum : uint8_t { NSFX_ADPCM = 0, NSFX_PCM16 = 1 };

struct NsfxWave {
    uint8_t format;   // NSFX_ADPCM (8-byte frames of 14 samples) or NSFX_PCM16 (host order)
    uint8_t loops;
    uint16_t reserved;
    uint32_t rate;    // its samples a second
    uint32_t samples;
    uint32_t loop_start;
    int16_t coefs[16];               // ADPCM: the 8 predictor pairs
    int16_t hist1, hist2;            // ADPCM: the history before sample 0
    int16_t loop_hist1, loop_hist2;  // ADPCM: the history at the start of loop_start's frame
    uint32_t offset;                 // its data, from the data section's start
    uint32_t size;
};
static_assert(sizeof(NsfxWave) == 64, "NSFX wave");

}  // namespace engine::audio
