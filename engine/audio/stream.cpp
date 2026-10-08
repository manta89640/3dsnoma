// A native music stream (see stream.h).
//
// The ring holds RING stereo samples; s_write and s_read count samples written and taken (they
// wrap; their difference is what's buffered). A piece is a generation: stream_play and stream_stop
// note where the new one starts in the ring (s_gen_start) and bump s_gen; the mixer, seeing a new
// generation, fades out what it was playing over that frame and goes on from s_gen_start
// (acknowledging it in s_mix_gen). Until it has, the pump counts the ring's free room from
// s_gen_start, so the new piece fills it at once. The pump reads READ_FRAMES ADPCM frames at a
// time and decodes them into the ring until it is full.
#include "engine/audio/stream.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "runtime/runtime.h"

namespace engine::audio {

namespace {

const u32 RING = 1u << 16;  // stereo samples: 2 s at 32 kHz
const u32 MASK = RING - 1;
const u32 READ_FRAMES = 64;  // ADPCM frames a read (896 samples)

s16 s_left[RING], s_right[RING];

// shared by the two threads
std::atomic<u32> s_write{0};      // the pump's: samples written
std::atomic<u32> s_read{0};       // the mixer's: samples taken
std::atomic<u32> s_gen{0};        // the piece's generation (a new piece or a stop)
std::atomic<u32> s_gen_start{0};  // where it starts in the ring
std::atomic<u32> s_mix_gen{0};    // the generation the mixer plays
std::atomic<bool> s_on{false};    // a piece plays or is paused
std::atomic<bool> s_hold{false};  // paused
std::atomic<bool> s_eof{false};   // the whole of a piece that ends is in the ring

// the mixer's
u32 m_gen = 0;
bool m_audible = false;  // its last frame was heard (the next starts at full level)

// the pump's (the game's thread)
FILE* p_file = nullptr;
NstmHeader p_h = {};
NstmChannel p_ch[2] = {};
u32 p_frame = 0, p_frames = 0;  // the next ADPCM frame, and how many there are
s16 p_hist1[2] = {}, p_hist2[2] = {};
u32 p_skip = 0;       // samples of the next frame to leave out (a loop starting inside it)
u64 p_written = 0;    // samples of this piece written
u8 p_buf[READ_FRAMES * 2 * NSTM_FRAME_BYTES];

void close_file() {
    if (p_file)
        std::fclose(p_file);
    p_file = nullptr;
}

// the mixer's position in this generation (or its start, until the mixer has gone on to it)
u32 read_base() {
    return s_mix_gen.load(std::memory_order_acquire) == s_gen.load(std::memory_order_relaxed)
               ? s_read.load(std::memory_order_acquire)
               : s_gen_start.load(std::memory_order_relaxed);
}

void new_generation() {
    s_gen_start.store(s_write.load(std::memory_order_relaxed), std::memory_order_relaxed);
    s_gen.fetch_add(1, std::memory_order_release);
}

// n samples from ring position r, fading from full to nothing, without taking them
void fade_out(u32 r, u32 w, s32* left, s32* right, u32 n) {
    u32 m = std::min(n, w - r);
    for (u32 i = 0; i < m; i++) {
        s32 g = (s32)(n - i);
        left[i] += s_left[(r + i) & MASK] * g / (s32)n;
        right[i] += s_right[(r + i) & MASK] * g / (s32)n;
    }
}

}  // namespace

bool stream_play(const std::string& path) {
    stream_stop();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        guest::log("stream: no %s", path.c_str());
        return false;
    }
    NstmHeader h;
    bool ok = std::fread(&h, sizeof(h), 1, f) == 1 && h.magic == NSTM_MAGIC && h.version == NSTM_VERSION &&
              (h.channels == 1 || h.channels == 2) && h.rate == NSTM_RATE && h.samples > 0 &&
              (!h.loops || h.loop_start < h.samples) &&
              std::fread(p_ch, sizeof(NstmChannel), h.channels, f) == h.channels &&
              std::fseek(f, (long)h.data_offset, SEEK_SET) == 0;
    if (!ok) {
        std::fclose(f);
        guest::log("stream: %s is not an NSTM stream of this version", path.c_str());
        return false;
    }
    p_file = f;
    p_h = h;
    p_frames = (h.samples + NSTM_FRAME_SAMPLES - 1) / NSTM_FRAME_SAMPLES;
    p_frame = 0;
    p_skip = 0;
    p_written = 0;
    std::memset(p_hist1, 0, sizeof(p_hist1));
    std::memset(p_hist2, 0, sizeof(p_hist2));
    s_eof.store(false, std::memory_order_relaxed);
    s_hold.store(false, std::memory_order_relaxed);
    new_generation();
    s_on.store(true, std::memory_order_release);
    stream_pump();
    return true;
}

void stream_stop() {
    if (!s_on.load(std::memory_order_relaxed) && !p_file)
        return;
    s_on.store(false, std::memory_order_release);
    new_generation();
    close_file();
}

void stream_pause(bool pause) { s_hold.store(pause, std::memory_order_release); }

bool stream_active() {
    if (!s_on.load(std::memory_order_relaxed))
        return false;
    if (!s_eof.load(std::memory_order_relaxed))
        return true;
    return s_write.load(std::memory_order_relaxed) != read_base();
}

bool stream_paused() { return stream_active() && s_hold.load(std::memory_order_relaxed); }

const NstmHeader& stream_header() { return p_h; }

double stream_seconds() {
    if (!s_on.load(std::memory_order_relaxed))
        return 0;
    u32 buffered = s_write.load(std::memory_order_relaxed) - read_base();
    return (double)(p_written - std::min<u64>(buffered, p_written)) / NSTM_RATE;
}

void stream_pump() {
    if (!p_file || s_eof.load(std::memory_order_relaxed))
        return;
    const u32 ch = p_h.channels;
    u32 w = s_write.load(std::memory_order_relaxed);
    u32 free = RING - 1 - (w - read_base());
    while (free >= NSTM_FRAME_SAMPLES) {
        if (p_frame == p_frames) {
            if (!p_h.loops) {
                s_eof.store(true, std::memory_order_release);
                close_file();
                break;
            }
            // back to the loop's frame, with the history the encoder had there
            p_frame = p_h.loop_start / NSTM_FRAME_SAMPLES;
            p_skip = p_h.loop_start % NSTM_FRAME_SAMPLES;
            for (u32 c = 0; c < ch; c++) {
                p_hist1[c] = p_ch[c].loop_hist1;
                p_hist2[c] = p_ch[c].loop_hist2;
            }
            std::fseek(p_file, (long)(p_h.data_offset + (u64)p_frame * ch * NSTM_FRAME_BYTES), SEEK_SET);
        }
        u32 n = std::min(std::min(READ_FRAMES, p_frames - p_frame), free / NSTM_FRAME_SAMPLES);
        u32 got = (u32)std::fread(p_buf, ch * NSTM_FRAME_BYTES, n, p_file);
        if (got < n) {  // a short file: it ends here
            guest::log("stream: the file ends early (frame %u of %u)", p_frame + got, p_frames);
            p_frames = p_frame + got;
            p_h.loops = 0;
        }
        for (u32 f = 0; f < got; f++, p_frame++) {
            s16 pcm[2][NSTM_FRAME_SAMPLES];
            for (u32 c = 0; c < ch; c++)
                nstm_decode_frame(p_buf + (f * ch + c) * NSTM_FRAME_BYTES, p_ch[c].coefs, p_hist1[c], p_hist2[c],
                                  pcm[c]);
            if (p_h.gain && p_h.gain != 4096) {
                for (u32 c = 0; c < ch; c++) {
                    for (u32 k = 0; k < NSTM_FRAME_SAMPLES; k++) {
                        s32 v = (pcm[c][k] * (s32)p_h.gain) >> 12;
                        pcm[c][k] = (s16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                    }
                }
            }
            u32 count = std::min(NSTM_FRAME_SAMPLES, p_h.samples - p_frame * NSTM_FRAME_SAMPLES);
            for (u32 k = p_skip; k < count; k++, w++) {
                s_left[w & MASK] = pcm[0][k];
                s_right[w & MASK] = pcm[ch - 1][k];
            }
            p_written += count - std::min(p_skip, count);
            free -= count - std::min(p_skip, count);
            p_skip = 0;
        }
        s_write.store(w, std::memory_order_release);
        if (!got)
            break;
    }
}

void stream_mix(s32* left, s32* right, u32 n) {
    u32 gen = s_gen.load(std::memory_order_acquire);
    u32 r = s_read.load(std::memory_order_relaxed);
    u32 w = s_write.load(std::memory_order_acquire);
    if (gen != m_gen) {
        // the last piece fades out over this frame; the next starts with the next
        if (m_audible)
            fade_out(r, w, left, right, n);
        m_audible = false;
        m_gen = gen;
        s_read.store(s_gen_start.load(std::memory_order_relaxed), std::memory_order_release);
        s_mix_gen.store(gen, std::memory_order_release);
        return;
    }
    if (!s_on.load(std::memory_order_acquire) || s_hold.load(std::memory_order_acquire)) {
        if (m_audible)
            fade_out(r, w, left, right, n);  // (paused: the same samples start the resume)
        m_audible = false;
        return;
    }
    u32 m = std::min(n, w - r);
    if (m_audible) {
        for (u32 i = 0; i < m; i++) {
            left[i] += s_left[(r + i) & MASK];
            right[i] += s_right[(r + i) & MASK];
        }
    } else {
        for (u32 i = 0; i < m; i++) {
            left[i] += s_left[(r + i) & MASK] * (s32)i / (s32)n;
            right[i] += s_right[(r + i) & MASK] * (s32)i / (s32)n;
        }
    }
    m_audible = m == n;  // (short of samples: the pump fell behind, the next frame fades in again)
    s_read.store(r + m, std::memory_order_release);
}

}  // namespace engine::audio
