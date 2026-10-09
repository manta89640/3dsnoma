// Native sound effects (see sfx.h).
//
// The queue holds QUEUE commands; s_put and s_take count those put and taken (they wrap; their
// difference is what waits). Only the game's thread puts, and only the mixer takes, but for
// sfx_release, which takes in the mixer's place while it holds the lock the mixer tries at the start
// of a frame (so a frame the mixer has begun ends before the bank goes, and none begins until it
// has). A play is a command for each of its variant's layers, sharing a handle, so that sfx_set and
// sfx_stop reach them all; when every voice is busy, a play takes the oldest that doesn't loop.
#include "engine/audio/sfx.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "engine/audio/nstm.h"

namespace engine::audio {

namespace {

const u32 OUT_RATE = 32000;
const u32 VOICES = 24;
const u32 QUEUE = 256;  // (a power of two)
const f32 MAX_STEP = 8.0f;  // the most samples of a wave an output sample takes (pitch x rate)

enum Op : u8 { PLAY, SET, STOP };

struct Cmd {
    Op op;
    bool loops;
    SfxHandle handle;
    const NsfxWave* wave;
    const u8* data;
    f32 a, b;  // PLAY: the layer's gain and step; SET: the gain and pitch; STOP: the fade's seconds
};

struct Voice {
    SfxHandle handle = 0;  // 0: free
    const NsfxWave* w = nullptr;
    const u8* data = nullptr;
    bool loops = false;
    u32 next = 0;           // the next sample of the wave to read
    u32 frame = ~0u;        // the ADPCM frame decoded into buf
    s16 buf[NSTM_FRAME_SAMPLES] = {};
    s16 h1 = 0, h2 = 0;     // the decoder's history: at the start of the frame after buf's
    s32 s0 = 0, s1 = 0;     // the wave's samples either side of where the voice is
    u32 frac = 0;           // where it is past s0 (16.16)
    f32 step = 0;           // samples of the wave an output sample, at the play's own pitch
    f32 pitch = 1;          // sfx_set's
    f32 gain = 0;           // the layer's, at the play's own gain
    f32 level = 1;          // sfx_set's
    f32 now = 0;            // the gain the last frame ended on
    f32 fade = 0;           // stopping: the share of its gain taken off a second (0: not stopping)
    f32 share = 1;          // stopping: the share left
    u32 past = 0;           // reads past the end of a wave that doesn't loop
    u32 age = 0;            // frames played
};

Cmd s_queue[QUEUE];
std::atomic<u32> s_put{0}, s_take{0};
std::atomic<u32> s_active{0};
Voice s_voices[VOICES];

// the mixer's lock (never destroyed: a bank that outlives the program's statics still releases)
std::mutex& the_lock() {
    static std::mutex* m = new std::mutex;
    return *m;
}

// the game's thread's
u32 s_next_handle = 1;
u32 s_rng = 0x9E3779B9u;

u32 rng() {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}
// -1..1
f32 uniform() { return (f32)(rng() >> 8) / 8388608.0f - 1.0f; }

bool push(const Cmd& c) {
    const u32 put = s_put.load(std::memory_order_relaxed);
    if (put - s_take.load(std::memory_order_acquire) >= QUEUE)
        return false;
    s_queue[put & (QUEUE - 1)] = c;
    s_put.store(put + 1, std::memory_order_release);
    return true;
}

// the wave's next sample (0 past the end of one that doesn't loop)
s32 read(Voice& v) {
    const NsfxWave& w = *v.w;
    if (v.next >= w.samples) {
        if (!v.loops) {
            v.past++;
            return 0;
        }
        // back to the loop's frame with the history at its start (it is decoded again, from there)
        v.next = w.loop_start;
        v.frame = ~0u;
        v.h1 = w.loop_hist1, v.h2 = w.loop_hist2;
    }
    if (w.format == NSFX_PCM16) {
        s16 s;
        std::memcpy(&s, v.data + 2 * v.next++, 2);
        return s;
    }
    const u32 f = v.next / NSTM_FRAME_SAMPLES;
    if (f != v.frame) {
        nstm_decode_frame(v.data + f * NSTM_FRAME_BYTES, w.coefs, v.h1, v.h2, v.buf);
        v.frame = f;
    }
    return v.buf[v.next++ % NSTM_FRAME_SAMPLES];
}

void start(Voice& v, const Cmd& c) {
    v = Voice();
    v.handle = c.handle;
    v.w = c.wave;
    v.data = c.data;
    v.loops = c.loops && c.wave->loops;
    v.h1 = c.wave->hist1, v.h2 = c.wave->hist2;
    v.gain = c.a;
    v.step = c.b;
    // a loop comes in from nothing over its first frame; a one-shot starts at its level (its wave's
    // own attack is in its samples)
    v.now = v.loops ? 0.0f : v.gain;
    v.s0 = read(v);
    v.s1 = read(v);
}

void apply(const Cmd& c) {
    switch (c.op) {
    case PLAY: {
        Voice* free = nullptr;
        Voice* oldest = nullptr;
        for (Voice& v : s_voices) {
            if (!v.handle) {
                free = &v;
                break;
            }
            if (!v.loops && (!oldest || v.age > oldest->age))
                oldest = &v;
        }
        if (Voice* v = free ? free : oldest)
            start(*v, c);
        break;
    }
    case SET:
        for (Voice& v : s_voices)
            if (v.handle == c.handle)
                v.level = c.a, v.pitch = c.b;
        break;
    case STOP:
        for (Voice& v : s_voices)
            if (v.handle == c.handle && v.fade == 0)
                v.fade = c.a > 0 ? 1.0f / c.a : 1e9f;
        break;
    }
}

// the queue's commands (but plays from [lo, hi): a bank going)
void take_commands(const u8* lo = nullptr, const u8* hi = nullptr) {
    u32 take = s_take.load(std::memory_order_relaxed);
    const u32 put = s_put.load(std::memory_order_acquire);
    for (; take != put; take++) {
        const Cmd& c = s_queue[take & (QUEUE - 1)];
        if (c.op == PLAY && lo && c.data >= lo && c.data < hi)
            continue;
        apply(c);
    }
    s_take.store(take, std::memory_order_release);
}

// a voice's n samples into the bus; false once it has ended
bool mix_voice(Voice& v, s32* left, s32* right, u32 n) {
    f32 target = v.gain * v.level;
    if (v.fade > 0) {
        v.share = std::max(0.0f, v.share - v.fade * (f32)n / (f32)OUT_RATE);
        target *= v.share;
    }
    const f32 from = v.now, dg = (target - from) / (f32)n;
    const u32 step = (u32)(std::min(std::max(v.step * v.pitch, 0.0f), MAX_STEP) * 65536.0f);
    v.age++;
    for (u32 i = 0; i < n; i++) {
        // (the difference times 12 bits of the fraction stays inside 32 bits)
        const s32 s = v.s0 + (((v.s1 - v.s0) * (s32)(v.frac >> 4)) >> 12);
        const s32 o = (s32)((f32)s * (from + dg * (f32)i));
        left[i] += o;
        right[i] += o;
        v.frac += step;
        while (v.frac >= 0x10000) {
            v.frac -= 0x10000;
            v.s0 = v.s1;
            v.s1 = read(v);
        }
        if (v.past >= 2)
            return false;  // (both sides past the end: the rest is silence)
    }
    v.now = target;
    return !(v.fade > 0 && v.share <= 0);
}

}  // namespace

struct SfxAccess {
    static const NsfxSound& sound(const SfxBank& b, int i) { return b.sounds_[i]; }
    static const NsfxVariant& variant(const SfxBank& b, u32 i) { return b.variants_[i]; }
    static const NsfxLayer& layer(const SfxBank& b, u32 i) { return b.layers_[i]; }
    static const NsfxWave& wave(const SfxBank& b, u32 i) { return b.waves_[i]; }
    static const u8* data(const SfxBank& b) { return b.data_; }
    static const u8* begin(const SfxBank& b) { return b.bytes_.data(); }
    static const u8* end(const SfxBank& b) { return b.bytes_.data() + b.bytes_.size(); }
};

std::unique_ptr<SfxBank> SfxBank::load(std::vector<u8> bytes, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error)
            *error = why;
        return std::unique_ptr<SfxBank>();
    };
    if (bytes.size() < sizeof(NsfxHeader))
        return fail("too short for an NSFX file");
    std::unique_ptr<SfxBank> b(new SfxBank());
    b->bytes_ = std::move(bytes);
    const u8* d = b->bytes_.data();
    const u64 size = b->bytes_.size();
    const NsfxHeader& h = *(const NsfxHeader*)d;
    if (h.magic != NSFX_MAGIC)
        return fail("not an NSFX file");
    if (h.version != NSFX_VERSION)
        return fail("NSFX version " + std::to_string(h.version) + ", not " + std::to_string(NSFX_VERSION));
    const u64 tables = sizeof(NsfxHeader) + (u64)h.n_sounds * sizeof(NsfxSound) +
                       (u64)h.n_variants * sizeof(NsfxVariant) + (u64)h.n_layers * sizeof(NsfxLayer) +
                       (u64)h.n_waves * sizeof(NsfxWave);
    if (tables > h.strings || h.strings > h.data || (u64)h.data + h.data_size > size || h.data % 4)
        return fail("its sections don't fit in it");
    b->h_ = &h;
    b->sounds_ = (const NsfxSound*)(d + sizeof(NsfxHeader));
    b->variants_ = (const NsfxVariant*)(b->sounds_ + h.n_sounds);
    b->layers_ = (const NsfxLayer*)(b->variants_ + h.n_variants);
    b->waves_ = (const NsfxWave*)(b->layers_ + h.n_layers);
    b->data_ = d + h.data;
    const u32 names = h.data - h.strings;
    for (u32 i = 0; i < h.n_sounds; i++) {
        const NsfxSound& s = b->sounds_[i];
        if (s.name >= names || !std::memchr(d + h.strings + s.name, 0, names - s.name))
            return fail("sound " + std::to_string(i) + "'s name is out of place");
        if (!s.n_variants || (u32)s.first_variant + s.n_variants > h.n_variants || !(s.gain_spread >= 1) ||
            !(s.pitch_spread >= 0 && s.pitch_spread < 24))
            return fail(std::string("sound ") + b->name((int)i) + " is damaged");
    }
    for (u32 i = 0; i < h.n_variants; i++) {
        const NsfxVariant& v = b->variants_[i];
        if (!v.n_layers || (u32)v.first_layer + v.n_layers > h.n_layers)
            return fail("variant " + std::to_string(i) + " is damaged");
    }
    for (u32 i = 0; i < h.n_layers; i++) {
        const NsfxLayer& l = b->layers_[i];
        if (l.wave >= h.n_waves || !(l.gain >= 0 && l.gain < 16) || !(l.pitch > 0 && l.pitch < 16))
            return fail("layer " + std::to_string(i) + " is damaged");
    }
    for (u32 i = 0; i < h.n_waves; i++) {
        const NsfxWave& w = b->waves_[i];
        const u64 need = w.format == NSFX_ADPCM ? ((u64)w.samples + NSTM_FRAME_SAMPLES - 1) / NSTM_FRAME_SAMPLES *
                                                      NSTM_FRAME_BYTES
                                                : (u64)w.samples * 2;
        if ((w.format != NSFX_ADPCM && w.format != NSFX_PCM16) || w.rate < 1000 || w.rate > 96000 || !w.samples ||
            (w.loops && w.loop_start >= w.samples) || w.size < need || (u64)w.offset + w.size > h.data_size)
            return fail("wave " + std::to_string(i) + " is damaged");
    }
    return b;
}

SfxBank::~SfxBank() {
    if (h_)
        sfx_release(*this);
}

int SfxBank::find(const char* name) const {
    for (u32 i = 0; i < h_->n_sounds; i++)
        if (std::strcmp(this->name((int)i), name) == 0)
            return (int)i;
    return -1;
}

bool SfxBank::loops(int sound) const { return sound >= 0 && (u32)sound < h_->n_sounds && sounds_[sound].loops; }

const char* SfxBank::name(int sound) const {
    return (const char*)bytes_.data() + h_->strings + sounds_[sound].name;
}

SfxHandle sfx_play(const SfxBank& bank, int sound, f32 gain, f32 pitch) {
    if (sound < 0 || (u32)sound >= bank.sounds())
        return 0;
    const NsfxSound& s = SfxAccess::sound(bank, sound);
    const NsfxVariant& v = SfxAccess::variant(bank, s.first_variant + rng() % s.n_variants);
    if (s.gain_spread > 1)
        gain *= std::exp(uniform() * std::log(s.gain_spread));
    if (s.pitch_spread > 0)
        pitch *= std::exp2(uniform() * s.pitch_spread / 12.0f);
    // (room for all of its layers, or none of them)
    if (QUEUE - (s_put.load(std::memory_order_relaxed) - s_take.load(std::memory_order_acquire)) < v.n_layers)
        return 0;
    SfxHandle h = s_next_handle++;
    if (!h)
        h = s_next_handle++;
    for (u32 i = 0; i < v.n_layers; i++) {
        const NsfxLayer& l = SfxAccess::layer(bank, v.first_layer + i);
        const NsfxWave& w = SfxAccess::wave(bank, l.wave);
        Cmd c;
        c.op = PLAY;
        c.loops = s.loops != 0;
        c.handle = h;
        c.wave = &w;
        c.data = SfxAccess::data(bank) + w.offset;
        c.a = gain * l.gain;
        c.b = (f32)w.rate / (f32)OUT_RATE * l.pitch * pitch;
        push(c);
    }
    return h;
}

void sfx_set(SfxHandle h, f32 gain, f32 pitch) {
    if (!h)
        return;
    Cmd c = {};
    c.op = SET;
    c.handle = h;
    c.a = gain, c.b = pitch;
    push(c);
}

void sfx_stop(SfxHandle h, f32 fade) {
    if (!h)
        return;
    Cmd c = {};
    c.op = STOP;
    c.handle = h;
    c.a = fade;
    push(c);
}

void sfx_release(const SfxBank& bank) {
    std::lock_guard<std::mutex> lock(the_lock());
    const u8 *lo = SfxAccess::begin(bank), *hi = SfxAccess::end(bank);
    take_commands(lo, hi);
    u32 active = 0;
    for (Voice& v : s_voices) {
        if (v.handle && v.data >= lo && v.data < hi)
            v.handle = 0;
        active += v.handle != 0;
    }
    s_active.store(active, std::memory_order_relaxed);
}

void sfx_mix(s32* left, s32* right, u32 n) {
    // (nothing waiting and nothing playing: no lock)
    if (!n || (s_take.load(std::memory_order_relaxed) == s_put.load(std::memory_order_acquire) &&
               s_active.load(std::memory_order_relaxed) == 0))
        return;
    std::unique_lock<std::mutex> lock(the_lock(), std::try_to_lock);
    if (!lock.owns_lock())
        return;  // (a bank is going: what plays from it stops now anyway)
    take_commands();
    u32 active = 0;
    for (Voice& v : s_voices) {
        if (!v.handle)
            continue;
        if (mix_voice(v, left, right, n))
            active++;
        else
            v.handle = 0;
    }
    s_active.store(active, std::memory_order_relaxed);
}

u32 sfx_voices() { return s_active.load(std::memory_order_relaxed); }

}  // namespace engine::audio
