// The AX DSP microcode, native on the 3DS: the voices play on the 3DS's own DSP (ndsp) instead
// of being mixed by the CPU (runtime/hw/dsp_ax.cpp does that on the host).
//
// Every AX frame (3 ms) the AX library sends a command list; its PROCESS command names the list
// of parameter blocks (PBs, one per voice: see dsp_ax.cpp for the layout). Here each running PB
// becomes an ndsp channel:
//   * a PB whose sample addresses or format change (a new sound, or the CPU moving it) starts
//     over: the samples from its current address to its end, then, if it loops, from its loop
//     address to its end again and again (two wave buffers). Guest memory is in the linear heap
//     (platform/3ds), so ADPCM and PCM8 samples are read where the game put them; the 3DS's
//     DSP-ADPCM is the Wii's (same frames, coefficients, context). Sounds the DSP can't play as
//     they are get decoded to PCM16 once (see Pcm below): ADPCM that loops (the Wii's loops
//     start anywhere in a frame), and PCM16 (big-endian on the Wii);
//   * every frame: rate (the PB's resampling ratio), volume (envelope times the main mix; the aux
//     buses' effects aren't there) and, back into the PB for the CPU, the current address and,
//     when a sound has ended, "not running" -- as the DSP writes them.
// Sample data the game changes while it plays (a stream's ring buffer) reaches the 3DS's DSP
// through its cache flushes: audio_dsp_flushed() writes the CPU's data cache back for them.
// Not yet: the aux buses (reverb), the biquad filter, volume ramps within a frame; and a
// playing ADPCM stream whose loop or end the CPU moves restarts at its position with the context
// the PB had when it started (a stream's end can click).
#ifdef __3DS__
#include <3ds.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "runtime.h"

namespace guest {

namespace {

enum : u32 {
    PB_NEXT = 0x00, PB_SRC_TYPE = 0x08, PB_MIXCTRL = 0x0C, PB_RUNNING = 0x10, PB_STREAM = 0x12,
    PB_MIX = 0x14, PB_VE = 0x6A, PB_LOOPFLAG = 0x6E, PB_FORMAT = 0x70, PB_LOOP = 0x72, PB_END = 0x76,
    PB_CUR = 0x7A, PB_COEFS = 0x7E, PB_PRED = 0xA0, PB_YN1 = 0xA2, PB_YN2 = 0xA4, PB_RATIO = 0xA6,
    PB_LOOP_PRED = 0xB4, PB_LOOP_YN1 = 0xB6, PB_LOOP_YN2 = 0xB8,
    PB_LPF = 0xBA,  // on, yn1, a0, b0
};
enum : u16 { FMT_ADPCM = 0x00, FMT_PCM16 = 0x0A, FMT_PCM8 = 0x19 };
const int CHANNELS = 24;

inline u16 pb16(u32 pb, u32 off) { return load16(pb + off); }
inline u32 pb32(u32 pb, u32 off) { return (u32)load16(pb + off) << 16 | load16(pb + off + 2); }
inline void set_pb16(u32 pb, u32 off, u16 v) { store16(pb + off, v); }
inline void set_pb32(u32 pb, u32 off, u32 v) { store16(pb + off, (u16)(v >> 16)), store16(pb + off + 2, (u16)v); }

// Accelerator addresses (physical, from 0): ADPCM counts nibbles (a frame: 2 header nibbles, 14
// samples), PCM16 16-bit samples, PCM8 bytes.
u32 byte_of(u16 fmt, u32 a) { return fmt == FMT_ADPCM ? a >> 1 : fmt == FMT_PCM16 ? a * 2 : a; }
// ADPCM: sample index of nibble address a (a frame's header counts as its first sample's)
u32 adpcm_sample(u32 a) { return (a >> 4) * 14 + ((a & 15) < 2 ? 0 : (a & 15) - 2); }

struct Voice {
    u32 pb = 0;       // the PB this channel plays (0: free)
    u16 fmt = 0;
    u32 start = 0;    // where the first wave buffer starts (accelerator address, ADPCM: a frame)
    u32 loop = 0, end = 0;
    bool looping = false;
    u32 cur_written = 0;  // the current address we last wrote into the PB
    // the ones before it: the AX library copies the game's PB into the DSP's when the game
    // changes its parameters, and the game's copy has the address we wrote a frame or two earlier
    u32 written[4] = {};
    u32 wi = 0;
    void wrote(u32 a) {
        written[wi++ & 3] = cur_written = a;
    }
    bool ours(u32 a) const {
        return a == cur_written || a == written[0] || a == written[1] || a == written[2] || a == written[3];
    }
    ndspWaveBuf wb[2];
    ndspAdpcmData ctx[2];
    bool stream = false;        // a stream: the CPU refills its samples while it plays
    struct Pcm* pcm = nullptr;  // the decoded samples it plays, if any
    u32 loop_start = 0;         // where the loop buffer starts (ADPCM played as it is: a frame)
    u32 seen = 0;               // the frame it was last in a command list
    u32 started = 0;         // the frame it started in
    bool lpf_set = false;    // the low-pass as last given to the DSP
    u16 lpf_on = 0, lpf_a0 = 0, lpf_b0 = 0;
    float level = 0;         // its volume (envelope times the louder side), for stealing
};

Voice s_voice[CHANNELS];
u32 s_frame;
bool s_on;

// The AX plays up to 96 voices, the 3DS's DSP 24 channels. A sequence keeps each released note on
// a voice of its own while it fades out; when every channel is taken, a new voice takes the
// channel of the quietest one (the oldest of equals; a new note's envelope starts from silence,
// so the newcomer always wins). A voice left without a channel stays silent until its PB stops or
// plays another sound (otherwise it would take a channel back the next frame).
struct Dropped {
    u32 pb, cur, end, loop;  // (nothing moves a silent PB's current address but the CPU)
};
std::vector<Dropped> s_dropped;

struct Stats {
    u32 starts, restarts, steals, dropped;
} s_stats;

// ---- sounds decoded to PCM16 in the linear heap (the only memory the DSP reads), once per sound
// and kept while there is room:
//   * DSP-ADPCM that loops: the 3DS's DSP starts an ADPCM buffer only at a frame (8 bytes, 14
//     samples), and the Wii's loops start anywhere (57 of the channel's 64 looped sounds start
//     inside a frame). Looping from the frame would repeat up to 13 samples with the wrong decoder
//     history on every pass: a buzz on short loops, a click on long ones;
//   * PCM16, big-endian in guest memory.
// A sound is known by its format and last sample; the guest's writes to its samples (mem
// generations: a new wave archive loaded over an old one) make it stale.
struct Pcm {
    u32 first = 0, end = 0;  // accelerator addresses: the first sample (ADPCM: its frame) and the last
    u16 fmt = 0;
    s16* data = nullptr;
    u32 samples = 0;
    u32 gen = 0;             // mem_generation of its bytes when decoded
    u32 users = 0;           // channels playing it
    u32 used = 0;            // the frame it last started in
};
const int PCM_SLOTS = 96;
const u32 PCM_BUDGET = 6u << 20;
Pcm s_pcm[PCM_SLOTS];
u32 s_pcm_bytes;

u32 bytes_of(const Pcm& p) { return byte_of(p.fmt, p.end) - byte_of(p.fmt, p.first) + 1; }

void pcm_free(Pcm& p) {
    linearFree(p.data);
    s_pcm_bytes -= p.samples * 2;
    p = Pcm();
}

void release(Voice& v) {
    if (v.pcm && v.pcm->users)
        v.pcm->users--;
    v.pcm = nullptr;
}

// DSP-ADPCM from frame `first` (a nibble address) to `end`, from the decoder state the PB holds
void decode_adpcm(u32 pb, const Pcm& p) {
    s32 coef[16];
    for (int i = 0; i < 16; i++)
        coef[i] = (s16)pb16(pb, PB_COEFS + 2 * i);
    s32 yn1 = (s16)pb16(pb, PB_YN1), yn2 = (s16)pb16(pb, PB_YN2);
    const u8* f = host_ptr(0x80000000u | (p.first >> 1));
    for (u32 k = 0; k < p.samples; f += 8) {
        s32 c1 = coef[(f[0] >> 4) * 2 & 15], c2 = coef[((f[0] >> 4) * 2 + 1) & 15];
        s32 scale = 1 << (f[0] & 15);
        for (u32 i = 0; i < 14 && k < p.samples; i++) {
            s32 nib = i & 1 ? f[1 + i / 2] & 15 : f[1 + i / 2] >> 4;
            nib = nib >= 8 ? nib - 16 : nib;
            s32 y = (((nib * scale) << 11) + c1 * yn1 + c2 * yn2 + 1024) >> 11;
            y = std::min<s32>(32767, std::max<s32>(-32768, y));
            p.data[k++] = (s16)y;
            yn2 = yn1, yn1 = y;
        }
    }
}

// the decoded sound a voice from `cur` to `end` plays: one already decoded (also when the voice
// restarts inside it), or a new one; null when there is no room
Pcm* pcm_for(u32 pb, u16 fmt, u32 cur, u32 end) {
    u32 first = fmt == FMT_ADPCM ? cur & ~15u : cur;
    Pcm* slot = nullptr;
    for (Pcm& p : s_pcm) {
        if (!p.data) {
            slot = slot ? slot : &p;
            continue;
        }
        if (p.fmt != fmt || p.end != end || p.first > first)
            continue;
        if (mem_generation(0x80000000u | byte_of(fmt, p.first), bytes_of(p)) == p.gen)
            return &p;
        if (!p.users)  // the guest put other samples there
            pcm_free(p), slot = slot ? slot : &p;
    }
    Pcm want;
    want.first = first, want.end = end, want.fmt = fmt;
    want.samples = fmt == FMT_ADPCM ? adpcm_sample(end) - adpcm_sample(first) + 1 : end - first + 1;
    u32 need = want.samples * 2;
    if ((s32)want.samples <= 0 || need > PCM_BUDGET || !host_ptr(0x80000000u | byte_of(fmt, end)))
        return nullptr;
    // room: the sounds no channel plays, least recently started first
    while (s_pcm_bytes + need > PCM_BUDGET || !slot) {
        Pcm* old = nullptr;
        for (Pcm& p : s_pcm)
            if (p.data && !p.users && (!old || p.used < old->used))
                old = &p;
        if (!old)
            return nullptr;
        pcm_free(*old);
        slot = slot ? slot : old;
    }
    if (!(want.data = (s16*)linearAlloc(need)))
        return nullptr;
    if (fmt == FMT_ADPCM) {
        decode_adpcm(pb, want);
    } else {
        for (u32 k = 0; k < want.samples; k++)
            want.data[k] = (s16)load16(0x80000000u | ((first + k) * 2));
    }
    DSP_FlushDataCache(want.data, need);
    want.gen = mem_generation(0x80000000u | byte_of(fmt, first), bytes_of(want));
    s_pcm_bytes += need;
    *slot = want;
    return slot;
}

void stop(int ch) {
    ndspChnWaveBufClear(ch);
    release(s_voice[ch]);
    s_voice[ch].pb = 0;
}

float level_of(u32 pb) {
    u32 mc = pb32(pb, PB_MIXCTRL);
    u16 l = mc & (1 | 4) ? pb16(pb, PB_MIX) : 0, r = mc & (2 | 4) ? pb16(pb, PB_MIX + 4) : 0;
    return pb16(pb, PB_VE) / 32768.0f * std::max(l, r) / 32768.0f;
}

// a voice left without a channel: a sound that plays once ends now (as when the DSP reaches its
// end, so the AX library frees the voice); a looping one waits
void drop(u32 pb) {
    s_stats.dropped++;
    if (!pb16(pb, PB_LOOPFLAG)) {
        set_pb16(pb, PB_RUNNING, 0);
        set_pb32(pb, PB_CUR, pb32(pb, PB_END));
        return;
    }
    if (s_dropped.size() >= 128)
        s_dropped.erase(s_dropped.begin());
    s_dropped.push_back({pb, pb32(pb, PB_CUR), pb32(pb, PB_END), pb32(pb, PB_LOOP)});
}

// a channel for a new voice: a free one, or the quietest voice's
int take_channel() {
    int best = -1;
    for (int i = 0; i < CHANNELS; i++) {
        const Voice& v = s_voice[i];
        if (!v.pb)
            return i;
        if (best < 0 || v.level < s_voice[best].level ||
            (v.level == s_voice[best].level && v.started < s_voice[best].started))
            best = i;
    }
    drop(s_voice[best].pb);
    stop(best);
    s_stats.steals++;
    return best;
}

// a PB left without a channel: true while it still plays the sound it had then
bool still_dropped(u32 pb, bool running) {
    for (size_t i = 0; i < s_dropped.size(); i++) {
        Dropped& d = s_dropped[i];
        if (d.pb != pb)
            continue;
        if (running && pb32(pb, PB_CUR) == d.cur && pb32(pb, PB_END) == d.end && pb32(pb, PB_LOOP) == d.loop)
            return true;
        d = s_dropped.back();
        s_dropped.pop_back();
        return false;
    }
    return false;
}

// the guest's sample bytes for the DSP, in place (written back from the CPU's cache)
void* sample_data(u32 first_byte, u32 bytes) {
    u8* p = host_ptr(0x80000000u | first_byte);
    if (p)
        DSP_FlushDataCache(p, bytes);
    return p;
}

// (re)start channel ch on PB pb from its current state
void start(int ch, u32 pb) {
    Voice& v = s_voice[ch];
    static int s_logged;
    static bool s_debug = getenv("NOMA_DEBUG_AX") != nullptr;
    if (s_debug && s_logged++ < 40)
        log("ax: voice %d: PB %08X format %u, %08X..%08X loop %08X (%s), ratio %08X", ch, pb, pb16(pb, PB_FORMAT),
            pb32(pb, PB_CUR), pb32(pb, PB_END), pb32(pb, PB_LOOP), pb16(pb, PB_LOOPFLAG) ? "looped" : "once",
            pb32(pb, PB_RATIO));
    ndspChnWaveBufClear(ch);
    release(v);
    v.pb = pb;
    v.started = s_frame;
    v.lpf_set = false;
    v.fmt = pb16(pb, PB_FORMAT);
    u32 cur = pb32(pb, PB_CUR);
    v.loop = pb32(pb, PB_LOOP);
    v.end = pb32(pb, PB_END);
    v.looping = pb16(pb, PB_LOOPFLAG) != 0;
    bool stream = v.stream = pb16(pb, PB_STREAM) != 0;
    memset(v.written, 0, sizeof(v.written));
    v.wrote(cur);
    memset(v.wb, 0, sizeof(v.wb));
    // the resampler: AX's 4-tap polyphase (0), linear (1), none (2)
    u16 src = pb16(pb, PB_SRC_TYPE);
    ndspChnSetInterp(ch, src == 0 ? NDSP_INTERP_POLYPHASE : src == 1 ? NDSP_INTERP_LINEAR : NDSP_INTERP_NONE);

    // decoded: looping ADPCM (a stream's ring buffer changes as it plays: as it is), and PCM16
    bool decode = v.fmt == FMT_PCM16 || (v.fmt == FMT_ADPCM && v.looping && !stream);
    if (decode && (v.pcm = pcm_for(pb, v.fmt, cur, v.end))) {
        Pcm& p = *v.pcm;
        p.users++, p.used = s_frame;
        auto index = [&](u32 a) {
            return v.fmt == FMT_ADPCM ? adpcm_sample(a) - adpcm_sample(p.first) : a - p.first;
        };
        ndspChnSetFormat(ch, NDSP_FORMAT_MONO_PCM16);
        v.start = cur, v.loop_start = v.loop;
        u32 from[2] = {index(cur), index(v.loop)};
        for (int i = 0; i < (v.looping ? 2 : 1); i++) {
            if (from[i] >= p.samples)
                continue;
            ndspWaveBuf& wb = v.wb[i];
            wb.data_pcm16 = p.data + from[i];
            wb.nsamples = p.samples - from[i];
            wb.looping = i == 1;
            ndspChnWaveBufAdd(ch, &wb);
        }
        return;
    }

    // as it is, from guest memory: ADPCM starts at a frame
    ndspChnSetFormat(ch, v.fmt == FMT_ADPCM ? NDSP_FORMAT_MONO_ADPCM : NDSP_FORMAT_MONO_PCM8);
    if (v.fmt == FMT_PCM16)
        return;  // no room to decode it: silent
    if (v.fmt == FMT_ADPCM) {
        u16 coefs[16];
        for (int i = 0; i < 16; i++)
            coefs[i] = pb16(pb, PB_COEFS + 2 * i);
        ndspChnSetAdpcmCoefs(ch, coefs);
    }
    // the first buffer: from the current address to the end; the second, when looping: from the
    // loop address to the end, repeated
    u32 from[2] = {cur, v.loop};
    for (int i = 0; i < (v.looping ? 2 : 1); i++) {
        u32 a = from[i];
        u32 first_byte, count;
        if (v.fmt == FMT_ADPCM) {
            a &= ~15u;
            first_byte = a >> 1;
            count = adpcm_sample(v.end) - adpcm_sample(a) + 1;
        } else {
            first_byte = a;
            count = v.end - a + 1;
        }
        if ((s32)count <= 0)
            continue;
        (i == 0 ? v.start : v.loop_start) = a;
        ndspWaveBuf& wb = v.wb[i];
        wb.data_vaddr = sample_data(first_byte, v.fmt == FMT_ADPCM ? (count + 13) / 14 * 8 : count);
        if (!wb.data_vaddr)
            continue;
        wb.nsamples = count;
        wb.looping = i == 1;
        if (v.fmt == FMT_ADPCM && (i == 0 || !stream)) {
            ndspAdpcmData& c = v.ctx[i];
            c.index = i == 0 ? pb16(pb, PB_PRED) : pb16(pb, PB_LOOP_PRED);
            c.history0 = (s16)(i == 0 ? pb16(pb, PB_YN1) : pb16(pb, PB_LOOP_YN1));
            c.history1 = (s16)(i == 0 ? pb16(pb, PB_YN2) : pb16(pb, PB_LOOP_YN2));
            wb.adpcm_data = &c;
        }
        ndspChnWaveBufAdd(ch, &wb);
    }
}

// the accelerator address channel ch plays now
u32 position(int ch) {
    Voice& v = s_voice[ch];
    u32 pos = ndspChnGetSamplePos(ch);
    u16 seq = ndspChnGetWaveBufSeq(ch);
    bool in_loop = v.looping && v.wb[1].nsamples && seq == v.wb[1].sequence_id;
    u32 base = in_loop ? v.loop_start : v.start;
    if (v.fmt != FMT_ADPCM)
        return std::min(base + pos, v.end);
    u32 s = adpcm_sample(base) + pos;
    u32 a = (s / 14) * 16 + 2 + s % 14;
    return std::min(a, v.end);
}

// volume of the voice: its envelope times the main L/R mix (1.15 fixed point)
void set_volume(int ch, u32 pb) {
    s_voice[ch].level = level_of(pb);
    float ve = pb16(pb, PB_VE) / 32768.0f;
    u32 mc = pb32(pb, PB_MIXCTRL);
    float mix[12] = {};
    if (mc & (1 | 4))
        mix[0] = ve * pb16(pb, PB_MIX) / 32768.0f;
    if (mc & (2 | 4))
        mix[1] = ve * pb16(pb, PB_MIX + 4) / 32768.0f;
    ndspChnSetMix(ch, mix);
}

int channel_of(u32 pb) {
    for (int i = 0; i < CHANNELS; i++)
        if (s_voice[i].pb == pb)
            return i;
    return -1;
}

void process_pbs(u32 pb) {
    for (int guard = 0; pb && guard < 128; guard++) {
        u32 next = pb32(pb, PB_NEXT);
        int ch = channel_of(pb);
        bool running = pb16(pb, PB_RUNNING) == 1;
        bool silent = ch < 0 && still_dropped(pb, running);  // (forgets it once it stops)
        if (!running || silent) {
            if (ch >= 0)
                stop(ch);
            pb = next ? 0x80000000u | (next & 0x1FFFFFFF) : 0;
            continue;
        }
        Voice* v = ch >= 0 ? &s_voice[ch] : nullptr;
        bool moved = v && (!v->ours(pb32(pb, PB_CUR)) || pb16(pb, PB_FORMAT) != v->fmt ||
                           pb32(pb, PB_END) != v->end || pb32(pb, PB_LOOP) != v->loop ||
                           (pb16(pb, PB_LOOPFLAG) != 0) != v->looping);
        if (!v || moved) {
            if (ch < 0)
                ch = take_channel();
            (moved ? s_stats.restarts : s_stats.starts)++;
            start(ch, pb);
            v = &s_voice[ch];
        }
        v->seen = s_frame;
        u32 ratio = pb32(pb, PB_RATIO);
        ndspChnSetRate(ch, pb16(pb, PB_SRC_TYPE) == 2 ? 32000.0f : 32000.0f * (float)ratio / 65536.0f);
        set_volume(ch, pb);
        // the one-pole low-pass nw4r sets per note: y = a0·x + b0·y[n-1] (1.15); the DSP's mono
        // IIR computes y = b0·x + a1·y[n-1] from what ndspChnIirMonoSetParamsCustomFilter takes
        u16 lpf_on = pb16(pb, PB_LPF), a0 = pb16(pb, PB_LPF + 4), b0 = pb16(pb, PB_LPF + 6);
        if (!v->lpf_set || lpf_on != v->lpf_on || (lpf_on && (a0 != v->lpf_a0 || b0 != v->lpf_b0))) {
            ndspChnIirMonoSetEnable(ch, lpf_on != 0);
            if (lpf_on)
                ndspChnIirMonoSetParamsCustomFilter(ch, 1.0f, b0 / 32768.0f, a0 / 32768.0f);
            v->lpf_set = true, v->lpf_on = lpf_on, v->lpf_a0 = a0, v->lpf_b0 = b0;
        }
        // what the DSP writes back: where the voice is, and whether it still runs
        if (!v->looping && v->wb[0].status == NDSP_WBUF_DONE && !ndspChnIsPlaying(ch)) {
            set_pb16(pb, PB_RUNNING, 0);
            set_pb32(pb, PB_CUR, v->end);
            v->wrote(v->end);
        } else {
            u32 a = position(ch);
            set_pb32(pb, PB_CUR, a);
            v->wrote(a);
        }
        pb = next ? 0x80000000u | (next & 0x1FFFFFFF) : 0;
    }
    // voices no list mentions any more
    for (int i = 0; i < CHANNELS; i++)
        if (s_voice[i].pb && s_voice[i].seen != s_frame)
            stop(i);
}

}  // namespace

bool audio_dsp_native() { return s_on; }

u32 audio_dsp_linear_reserve() { return s_on ? PCM_BUDGET - s_pcm_bytes : 0; }

void audio_dsp_init() {
    s_on = true;
    for (int i = 0; i < CHANNELS; i++)
        ndspChnReset(i);
    log("audio: voices on the DSP (ndsp, %d channels)", CHANNELS);
}

// one AX command list: the voices' PBs go to ndsp; nothing is mixed here
void audio_dsp_frame(u32 list_addr, u32 words) {
    NOMA_PROF_SCOPE("(ax)");
    s_frame++;
    static bool s_debug = getenv("NOMA_DEBUG_AX") != nullptr;
    static u64 s_since;
    if (s_debug && s_frame % 2000 == 0) {
        u64 now = host_time_us();
        int busy = 0;
        for (const Voice& v : s_voice)
            busy += v.pb != 0;
        if (s_since)
            log("ax: %u frames/s; starts %u, restarts %u, steals %u, dropped %u; %d channels busy",
                (u32)(2000ull * 1000000 / std::max<u64>(now - s_since, 1)), s_stats.starts,
                s_stats.restarts, s_stats.steals, s_stats.dropped, busy);
        s_since = now;
        s_stats = {};
    }
    u32 a = 0x80000000u | (list_addr & 0x1FFFFFFF);
    u32 end = a + 2 * std::min<u32>(words ? words : 64, 512);
    auto next16 = [&]() { u16 v = load16(a); a += 2; return v; };
    auto next32 = [&]() { u32 hi = next16(); return hi << 16 | next16(); };
    while (a < end) {
        switch (next16()) {
        case 0x00: case 0x01: case 0x02: case 0x03: next32(); break;
        case 0x04: {
            u32 pbs = next32();
            process_pbs(pbs ? 0x80000000u | (pbs & 0x1FFFFFFF) : 0);
            break;
        }
        case 0x05: case 0x06: case 0x07: next16(), next32(), next32(); break;
        case 0x08: case 0x09: next16(); for (int i = 0; i < 6; i++) next32(); break;
        case 0x0A: next16(), next16(), next32(); break;
        case 0x0B: case 0x0C: next16(), next32(), next32(); break;
        case 0x0D: for (int i = 0; i < 4; i++) next32(); break;
        default: return;  // 0x0E END, or something unknown
        }
    }
}

// the guest flushed [addr, addr + n) from its cache: sample data a voice plays goes to the DSP
void audio_dsp_flushed(u32 addr, u32 n) {
    if (!s_on || !n)
        return;
    u32 lo = addr & 0x1FFFFFFF, hi = lo + n;
    for (int i = 0; i < CHANNELS; i++) {
        Voice& v = s_voice[i];
        if (!v.pb)
            continue;
        if (v.pcm) {
            // decoded: a PCM16 stream's ring buffer is refreshed in place; other sounds don't
            // change while they play (a new sound over an old one shows in its mem generation)
            Pcm& p = *v.pcm;
            if (!v.stream || p.fmt != FMT_PCM16)
                continue;
            u32 first = p.first * 2, last = p.end * 2 + 2;
            if (lo >= last || hi <= first)
                continue;
            u32 from = std::max(lo, first) & ~1u, to = std::min(hi, last);
            for (u32 b = from; b + 1 < to; b += 2)
                p.data[b / 2 - p.first] = (s16)load16(0x80000000u | b);
            DSP_FlushDataCache(p.data + (from / 2 - p.first), to - from);
            continue;
        }
        // played where it is: the CPU's cache goes to memory for the DSP
        u32 lowest = v.looping ? std::min(v.start, v.loop_start) : v.start;
        u32 first = byte_of(v.fmt, lowest), last = byte_of(v.fmt, v.end) + 1;
        if (lo >= last || hi <= first)
            continue;
        u32 from = std::max(lo, first), to = std::min(hi, last);
        if (u8* p = host_ptr(0x80000000u | from))
            DSP_FlushDataCache(p, to - from);
    }
}

}  // namespace guest
#endif
