// NSTM: the port's own music stream file, a piece of music ready to stream from the SD card and
// mix into the AX output (engine/audio/stream.h). tools/music/stage_music.py makes them (from Wii
// Sports' and Wii Sports Resort's streams, waves and sequences), tools/nstm_enc.cpp encodes them
// (a piece the disc already has in DSP-ADPCM, as it is, keeps the disc's own frames).
//
// Layout (little-endian): the header, then for each channel its NstmChannel, then the data from
// data_offset: DSP-ADPCM frames (8 bytes: a predictor/scale byte and 14 nibbles, 14 samples)
// interleaved by frame (frame 0 of channel 0, frame 0 of channel 1, frame 1 of channel 0, ...).
// Always 32 kHz, the AX output's rate (the stager resamples), so the mixer only adds.
//
// A piece that loops plays from 0 to `samples` and then again from loop_start, on and on: the
// decoder goes back to the frame holding loop_start (loop_start / 14) with each channel's history
// at that frame's start (loop_hist1/2, the encoder's own decoded history there) and skips the
// samples before loop_start in it.
#pragma once

#include <cstdint>

namespace engine::audio {

const uint32_t NSTM_MAGIC = 0x4D54534E;  // "NSTM"
const uint16_t NSTM_VERSION = 1;
const uint32_t NSTM_RATE = 32000;
const uint32_t NSTM_FRAME_SAMPLES = 14, NSTM_FRAME_BYTES = 8;

struct NstmHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t channels;     // 1 or 2 (one channel plays on both sides)
    uint32_t rate;         // NSTM_RATE
    uint32_t samples;      // the piece's length, a channel's samples
    uint32_t loop_start;   // where a loop goes back to (a sample)
    uint8_t loops;         // 1: plays on from loop_start at the end; 0: ends
    uint8_t beats_per_bar; // the metre for the dances (4, or 3 for a waltz)
    uint16_t gain;         // applied as it plays, in 1/4096 (0: none): pieces whose ADPCM is the
                           // disc's own keep it (no second encoding) and get their level here
    float bpm;             // its tempo (quarter notes a minute), 0 if none was found
    float first_beat;      // seconds from the start to a beat (the beat grid's phase)
    uint32_t data_offset;  // from the file's start
};
static_assert(sizeof(NstmHeader) == 36, "NSTM header");

struct NstmChannel {
    int16_t coefs[16];  // the 8 predictor pairs
    int16_t loop_hist1, loop_hist2;  // the history at the start of loop_start's frame
};
static_assert(sizeof(NstmChannel) == 36, "NSTM channel");

// one DSP-ADPCM frame of 14 samples, as the Wii's DSP decodes it; hist1/hist2 carry over
inline void nstm_decode_frame(const uint8_t* frame, const int16_t* coefs, int16_t& hist1, int16_t& hist2,
                              int16_t* out) {
    int predictor = (frame[0] >> 4) & 7, scale = 1 << (frame[0] & 0xF);
    int c1 = coefs[2 * predictor], c2 = coefs[2 * predictor + 1];
    int h1 = hist1, h2 = hist2;
    for (int k = 0; k < 14; k++) {
        int nibble = k & 1 ? frame[1 + (k >> 1)] & 0xF : frame[1 + (k >> 1)] >> 4;
        if (nibble >= 8)
            nibble -= 16;
        int s = nibble * scale * 2048 + 1024 + c1 * h1 + c2 * h2;
        s >>= 11;
        s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
        out[k] = (int16_t)s;
        h2 = h1;
        h1 = s;
    }
    hist1 = (int16_t)h1;
    hist2 = (int16_t)h2;
}

}  // namespace engine::audio
