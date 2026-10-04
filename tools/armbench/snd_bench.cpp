// nw4r::snd's voice update as the 3DS build compiles it (engine/snd/voice.cpp, ax_voice.cpp),
// driven under Unicorn by armbench.py --snd: VoiceManager::UpdateAllVoices over voices like the
// room's, every one re-flagged each frame as the game's player does (it hands every voice its
// output parameters every frame: pitch, envelope, mix and filter all to be recalculated).
#include <cstring>

#include "runtime/runtime.h"
#include "engine/snd/voice.h"

namespace guest {
uintptr_t g_page[1u << (32 - PAGE_BITS)];
u32 g_sp;
u32 g_msr;
FnSlot g_fn_cache[4096];
u32 mmio_read(u32, int) { return 0; }
void mmio_write(u32, u32, int) {}
void* lookup_fn_slow(u32) { return nullptr; }
void fatal(const char*, ...) {
    for (;;) {
    }
}
void log(const char*, ...) {}
}  // namespace guest

// no environment, and static locals' guards for one thread (newlib's and libstdc++'s take locks
// that only the 3DS's libctru provides; the guards' every-call check is the same)
extern "C" char* getenv(const char*) { return nullptr; }
extern "C" int __cxa_guard_acquire(int* g) { return !*(char*)g; }
extern "C" void __cxa_guard_release(int* g) { *(char*)g = 1; }
extern "C" void __cxa_guard_abort(int*) {}

namespace {
u8 s_mem1[1u << guest::PAGE_BITS] __attribute__((aligned(32)));
const u32 MANAGER = 0x80010000, VOICES = 0x80020000, AXVOICES = 0x80040000, PBS = 0x80060000, AXMAN = 0x80080000;
const u32 VOICE_SIZE = 0x200, AXV_SIZE = 0x80, PB_SIZE = 0x180;
void st8(u32 a, u8 v) { guest::store8(a, v); }
void st16(u32 a, u16 v) { guest::store16(a, v); }
void st32(u32 a, u32 v) { guest::store32(a, v); }
void stf(u32 a, f32 f) {
    u32 w;
    memcpy(&w, &f, 4);
    st32(a, w);
}
}  // namespace

namespace g {
u32 nw4r_snd_detail_AxManager_GetInstance(u32, u32, u32, u32, u32, u32, u32, u32, f64, f64, f64, f64, f64, f64, f64,
                                          f64) {
    return AXMAN;
}
// (what a channel calls when it ends: the scenarios' channels don't)
void nw4r_snd_detail_Voice_Stop(u32) {}
u32 nw4r_snd_detail_Voice_Free(u32) { return 0; }
u32 nw4r_snd_detail_ChannelManager_GetInstance(void) { return 0; }
u32 nw4r_ut_detail_LinkListImpl_Erase(u32, u32) { return 0; }
u32 nw4r_snd_detail_PoolImpl_FreeImpl(u32, u32) { return 0; }
void nw4r_snd_detail_ChannelManager_UpdateAllChannel(u32 manager);
u64 nw4r_snd_detail_VoiceManager_UpdateAllVoices(u32, u32, u32, u32, u32, u32, u32, u32, f64, f64, f64, f64, f64,
                                                 f64, f64, f64);
}  // namespace g

// a random scenario for comparing two versions of engine/snd (armbench.py --snd-diff): voices with
// one or two channels and outputs, every output mode, pan curve and mode, remote speakers and
// filters, envelopes and mixes ramping or starting, voices started, paused, finishing; each frame
// some parameters change and some voices are flagged for some updates. Returns a hash of every
// voice, AX voice, PB and the AxManager after each frame (FNV-1a, chained).
namespace {
struct Rng {
    u32 seed;
    u32 operator()() {
        seed = seed * 1103515245u + 12345u;
        return seed >> 8;
    }
    f32 f(f32 lo, f32 hi) { return lo + (hi - lo) * (f32)((*this)() & 0xFFFF) / 65535.0f; }
};
}  // namespace

extern "C" u32 snd_random(u32 seed, u32 voices, u32 frames) {
    using namespace engine::snd;
    guest::g_page[0x80000000u >> guest::PAGE_BITS] = (uintptr_t)s_mem1 - 0x80000000u;
    memset(s_mem1, 0, 0x90000);
    Rng r{seed};
    auto modes = [&]() {
        st32(AXMAN + 0x0, r() % 4);  // stereo, surround, DPL2, mono
        stf(AXMAN + 0x1C, r.f(0.0f, 1.2f)), stf(AXMAN + 0x20, r.f(0.0f, 1.2f));
        st32(AXMAN + 0x24, r() % 3 ? 0 : r() % 50), st32(AXMAN + 0x28, r() % 60);
    };
    modes();
    st32(AXMAN + 0x4, 0x00900000);
    u32 head = MANAGER + 8, prev = head;
    auto params = [&](u32 v) {
        for (u32 o = 0; o < 2; o++) {  // VoiceOutParam: volume, pitch, pan, span, fx send, lpf
            u32 op = v + V_OUT + 0x18 * o;
            stf(op, r.f(0.0f, 1.5f)), stf(op + 4, r.f(0.5f, 2.0f)), stf(op + 8, r.f(-1.5f, 1.5f));
            stf(op + 0xC, r.f(-0.5f, 2.5f)), stf(op + 0x10, r.f(-0.5f, 1.2f)), stf(op + 0x14, r.f(-0.5f, 0.3f));
        }
        st32(v + V_PAN_MODE, r() % 2), st32(v + V_PAN_CURVE, r() % 11);
        stf(v + V_PAN, r.f(-1.5f, 1.5f)), stf(v + V_SPAN, r.f(-0.5f, 2.5f)), stf(v + V_LPF_FREQ, r.f(-0.2f, 1.2f));
        st32(v + V_OUTPUT_LINE, 1 | (r() % 4 ? 0 : (r() & 0x1E)));
        stf(v + V_MAIN_VOLUME, r.f(0.0f, 1.5f)), stf(v + V_MAIN_SEND, r.f(0.0f, 1.5f));
        for (u32 k = 0; k < 3; k++)
            stf(v + V_FX_SEND + 4 * k, r() % 2 ? 0.0f : r.f(-0.2f, 1.2f));
        for (u32 k = 0; k < 4; k++)
            stf(v + V_REMOTE_VOLUME + 4 * k, r.f(0.0f, 1.2f));
        stf(v + V_PITCH, r.f(0.25f, 4.0f)), stf(v + V_VOLUME, r.f(0.0f, 1.5f));
        stf(v + V_VE_INIT_VOLUME, r.f(0.0f, 1.2f)), stf(v + V_VE_VOLUME, r.f(0.0f, 1.2f));
        st8(v + V_REMOTE_FILTER, (u8)(r() % 4 ? 0 : r() % 140));
    };
    for (u32 i = 0; i < voices; i++) {
        u32 v = VOICES + VOICE_SIZE * i;
        u32 channels = 1 + r() % 2, outs = 1 + (r() % 4 == 0);
        st32(v + V_CHANNELS, channels), st32(v + V_OUTS, outs);
        for (u32 ch = 0; ch < channels; ch++)
            for (u32 o = 0; o < outs; o++) {
                u32 ax = AXVOICES + AXV_SIZE * (4 * i + 2 * ch + o), pb = PBS + PB_SIZE * (4 * i + 2 * ch + o);
                st32(v + V_AXVOICE + ch * 0x10 + o * 4, r() % 16 ? ax : 0);
                st32(ax + AXV_PB, r() % 16 ? pb : 0), st32(ax + AXV_DATA, r() % 4 ? 0x00100000 + 0x1000 * i : 0);
                st32(ax + AXV_FORMAT, 1 + r() % 3), st32(ax + AXV_RATE, r() % 2 ? 32000 : 22050 + r() % 30000);
                st16(ax + AXV_VE_VOL, (u16)r()), st16(ax + AXV_VE_TARGET, (u16)r()), st16(ax + AXV_VE_DELTA, (u16)(r() % 64 - 32));
                st8(ax + AXV_VE_FIRST, r() % 2), st8(ax + AXV_MIX_FIRST, r() % 2);
                for (u32 k = 0; k < 12; k++)
                    st16(ax + AXV_MIX + 2 * k, (u16)(r() % 3 ? r() % 0x9000 : 0));
                st16(pb + PB_STATE, r() % 4 ? 1 : 0);
                st16(pb + PB_ADDR_CUR, (u16)(r() % 2 ? 0x0020 : 0x0090)), st16(pb + PB_ADDR_CUR + 2, (u16)r());
                st16(pb + PB_VE + 2, (u16)(r() % 64 - 32));
            }
        st8(v + V_ALLOCATED, r() % 8 ? 1 : 0), st8(v + V_ACTIVE, r() % 8 ? 1 : 0), st8(v + V_STARTED, r() % 4 ? 1 : 0);
        st8(v + V_PAUSE, r() % 4 == 0), st8(v + V_PITCH_ONLY, r() % 4 == 0);
        params(v);
        st16(v + V_SYNC, (u16)(r() & 0x1BF & ~0x100));  // (no biquad: its coefficients come from guest code)
        st32(prev, v + V_LINK), prev = v + V_LINK;
    }
    st32(prev, head);
    // one field alone (each input of the mix, envelope, pitch and filter), now and then NaN, an
    // infinity or -0
    static const u32 kFloats[] = {V_MAIN_VOLUME, V_MAIN_SEND, V_FX_SEND, V_FX_SEND + 4, V_FX_SEND + 8,
                                  V_REMOTE_VOLUME, V_REMOTE_VOLUME + 4, V_REMOTE_VOLUME + 8, V_REMOTE_VOLUME + 12,
                                  V_PAN, V_SPAN, V_LPF_FREQ, V_PITCH, V_VOLUME, V_VE_INIT_VOLUME, V_VE_VOLUME,
                                  V_OUT, V_OUT + 4, V_OUT + 8, V_OUT + 0xC, V_OUT + 0x10, V_OUT + 0x14,
                                  V_OUT + 0x18, V_OUT + 0x1C, V_OUT + 0x20, V_OUT + 0x24, V_OUT + 0x28, V_OUT + 0x2C};
    auto one_field = [&](u32 v) {
        u32 k = r() % (sizeof(kFloats) / 4 + 4);
        if (k < sizeof(kFloats) / 4) {
            static const u32 odd[4] = {0x7FC00000u, 0x7F800000u, 0xFF800000u, 0x80000000u};  // NaN, +inf, -inf, -0
            if (r() % 16 == 0)
                st32(v + kFloats[k], odd[r() % 4]);
            else
                stf(v + kFloats[k], r.f(-1.5f, 2.5f));
        } else if (k == sizeof(kFloats) / 4) {
            st32(v + V_OUTPUT_LINE, 1 | (r() & 0x1E));
        } else if (k == sizeof(kFloats) / 4 + 1) {
            st32(v + V_PAN_MODE, r() % 2);
        } else if (k == sizeof(kFloats) / 4 + 2) {
            st32(v + V_PAN_CURVE, r() % 11);
        } else {
            st32(AXMAN + 0x0, r() % 4);
        }
    };
    u32 h = 2166136261u;
    for (u32 f = 0; f < frames; f++) {
        if (r() % 8 == 0)
            modes();
        for (u32 i = 0; i < voices; i++) {
            u32 v = VOICES + VOICE_SIZE * i;
            if (r() % 8 == 0)
                params(v);
            else if (r() % 2)
                one_field(v);
            u16 sync = guest::load16(v + V_SYNC) | (r() % 4 ? 0x3C : (r() & 0xBF));
            st16(v + V_SYNC, sync);
        }
        g::nw4r_snd_detail_VoiceManager_UpdateAllVoices(MANAGER, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        for (u32 i = 0x10000; i < 0x90000; i++)
            h = (h ^ s_mem1[i]) * 16777619u;
    }
    return h;
}

// `voices` voices like the room's (one channel, one output, ADPCM, a fixed volume, panned, the
// filter off), updated `frames` times, each frame flagged for pitch, envelope, mix and filter;
// with `hash`, an FNV-1a hash of the voices, AX voices and PBs after each frame
extern "C" u32 snd(u32 voices, u32 frames, u32 hash) {
    using namespace engine::snd;
    guest::g_page[0x80000000u >> guest::PAGE_BITS] = (uintptr_t)s_mem1 - 0x80000000u;
    memset(s_mem1, 0, 0x90000);
    // AxManager: stereo, the master volume at 1.0, the finished voices' zero buffer
    stf(AXMAN + 0x1C, 1.0f), stf(AXMAN + 0x20, 1.0f), st32(AXMAN + 0x4, 0x00900000);
    u32 head = MANAGER + 8, prev = head;
    for (u32 i = 0; i < voices; i++) {
        u32 v = VOICES + VOICE_SIZE * i, ax = AXVOICES + AXV_SIZE * i, pb = PBS + PB_SIZE * i;
        st32(v + V_AXVOICE, ax);
        stf(v + V_OUT + 0x0, 1.0f), stf(v + V_OUT + 0x4, 1.0f);  // VoiceOutParam: volume, pitch
        st32(v + V_CHANNELS, 1), st32(v + V_OUTS, 1);
        st8(v + V_ALLOCATED, 1), st8(v + V_ACTIVE, 1), st8(v + V_STARTED, 1);
        stf(v + V_PAN, -0.6f + 0.05f * (f32)i), stf(v + V_LPF_FREQ, 1.0f);
        st32(v + V_OUTPUT_LINE, 1);
        stf(v + V_MAIN_VOLUME, 1.0f), stf(v + V_MAIN_SEND, 1.0f);
        stf(v + V_PITCH, 1.0f + 0.01f * (f32)i), stf(v + V_VOLUME, 0.7f);
        stf(v + V_VE_INIT_VOLUME, 0.5f), stf(v + V_VE_VOLUME, 0.5f);
        st32(prev, v + V_LINK), prev = v + V_LINK;
        st32(ax + AXV_PB, pb), st32(ax + AXV_DATA, 0x00100000 + 0x1000 * i);
        st32(ax + AXV_FORMAT, 3), st32(ax + AXV_RATE, 32000);
        st16(ax + AXV_VE_VOL, 0x3000), st16(ax + AXV_VE_TARGET, 0x3000);
        st16(pb + PB_STATE, 1);
        st16(pb + PB_ADDR_CUR, 0x0020), st16(pb + PB_ADDR_CUR + 2, (u16)(0x1000 * i));
    }
    st32(prev, head);
    u32 h = 2166136261u;
    for (u32 f = 0; f < frames; f++) {
        for (u32 i = 0; i < voices; i++) {  // (Voice::SetVoiceOutParam, every frame)
            u32 v = VOICES + VOICE_SIZE * i;
            st16(v + V_SYNC, guest::load16(v + V_SYNC) | 0x3C);
        }
        g::nw4r_snd_detail_VoiceManager_UpdateAllVoices(MANAGER, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        for (u32 i = 0; hash && i < 0x30000; i++)  // (the voices, AX voices and PBs: what changed)
            h = (h ^ s_mem1[VOICES - 0x80000000u + i]) * 16777619u;
    }
    return h;
}

// `channels` channels like a sequence's notes (an envelope in its sustain, one in three with an LFO
// on the pitch, panned, a fixed volume), each driving a voice like snd()'s, `frames` frames of
// ChannelManager::UpdateAllChannel; with `hash`, an FNV-1a hash of the channels and voices
extern "C" u32 chan(u32 channels, u32 frames, u32 hash) {
    snd(channels, 0, 0);  // the voices
    const u32 CHANNELS = 0x80088000, CH_SIZE = 0x100, CM = 0x80087000;
    memset(s_mem1 + (CM - 0x80000000u), 0, 0x1000 + CH_SIZE * channels);
    u32 head = CM + 8, prev = head;
    for (u32 i = 0; i < channels; i++) {
        u32 c = CHANNELS + CH_SIZE * i;
        st32(c + 0x00, 3);                                        // envelope: sustain
        stf(c + 0x04, -60.0f - 4.0f * (f32)i);                    // its level (0.1 dB)
        stf(c + 0x08, 0.5f), stf(c + 0x0C, 1.0f), stf(c + 0x10, 0.9f);
        st8(c + 0x14, 100);
        if (i % 3 == 0)                                          // the LFO, on the pitch
            stf(c + 0x1C, 0.3f), stf(c + 0x1C + 4, 6.0f), st8(c + 0x1C + 0xC, 1), stf(c + 0x1C + 0x14, 0.1f * (f32)i);
        st8(c + 0x36, 1);                                         // active
        stf(c + 0x40, 1.0f), stf(c + 0x44, 1.0f), stf(c + 0x48, -0.5f + 0.1f * (f32)i);
        stf(c + 0x5C, 1.0f);                                      // main out volume
        for (u32 k = 0; k < 4; k++)
            stf(c + 0x70 + 4 * k, 1.0f);
        stf(c + 0x90, 0.8f), stf(c + 0x94, 0.1f), stf(c + 0x9C, 1.0f);
        st8(c + 0xA1, 255);                                       // no silence fade
        st32(c + 0xA8, 60 + i % 12), st32(c + 0xAC, 60);          // key, original key
        st32(c + 0xD0, VOICES + VOICE_SIZE * i);                  // its voice
        st32(prev, c + 0xD8), prev = c + 0xD8;
    }
    st32(prev, head);
    u32 h = 2166136261u;
    for (u32 f = 0; f < frames; f++) {
        g::nw4r_snd_detail_ChannelManager_UpdateAllChannel(CM);
        for (u32 i = 0; hash && i < 0x30000; i++)
            h = (h ^ s_mem1[VOICES - 0x80000000u + i]) * 16777619u;
        for (u32 i = 0; hash && i < CH_SIZE * channels; i++)
            h = (h ^ s_mem1[CHANNELS - 0x80000000u + i]) * 16777619u;
    }
    return h;
}

// a random scenario for comparing two versions of the channel update (armbench.py --snd-diff):
// every envelope state, LFOs with delays, depths, ranges and targets (pitch, volume, pan), sweeps,
// silence fades, keys and pitch offsets far out (octaves past a float's range), pauses, channels
// that end; then the voices' update. Returns a hash of the channels and voices after each frame.
extern "C" u32 chan_random(u32 seed, u32 channels, u32 frames) {
    snd_random(seed, channels, 0);  // random voices
    Rng r{seed * 7 + 1};
    const u32 CHANNELS = 0x80088000, CH_SIZE = 0x100, CM = 0x80087000;
    memset(s_mem1 + (CM - 0x80000000u), 0, 0x1000 + CH_SIZE * channels);
    auto params = [&](u32 c) {
        st32(c + 0x00, r() % 5);
        stf(c + 0x04, r() % 8 ? -r.f(0.0f, 900.0f) : r.f(-1000.0f, 50.0f));
        stf(c + 0x08, r.f(0.0f, 3.0f)), stf(c + 0x0C, r.f(0.0f, 5.0f)), stf(c + 0x10, r() % 6 ? r.f(0.5f, 1.0f) : 0.0f);
        st8(c + 0x14, (u8)(r() % 128)), st16(c + 0x16, (u16)(r() % 40)), st16(c + 0x18, (u16)(r() % 40));
        stf(c + 0x1C, r() % 2 ? 0.0f : r.f(0.0f, 1.0f)), stf(c + 0x20, r.f(0.0f, 20.0f));
        st32(c + 0x24, r() % 3 ? 0 : r() % 30), st8(c + 0x28, (u8)(r() % 16)), st32(c + 0x2C, r() % 40);
        stf(c + 0x30, r.f(-0.5f, 1.5f));
        st8(c + 0x34, (u8)(r() % 3)), st8(c + 0x35, r() % 8 == 0), st8(c + 0x36, r() % 16 ? 1 : 0);
        st8(c + 0x38, r() % 2), st8(c + 0x3C, (u8)(r() % 4 ? 0 : r() % 140));
        stf(c + 0x40, r.f(0.0f, 1.5f)), stf(c + 0x44, r.f(0.25f, 4.0f)), stf(c + 0x48, r.f(-1.5f, 1.5f));
        stf(c + 0x4C, r.f(-0.5f, 2.5f)), stf(c + 0x50, r.f(-1.2f, 0.2f));
        st32(c + 0x58, 1 | (r() % 4 ? 0 : r() & 0x1E));
        stf(c + 0x5C, r.f(0.0f, 1.5f)), stf(c + 0x60, r.f(-1.0f, 0.5f));
        for (u32 k = 0; k < 3; k++)
            stf(c + 0x64 + 4 * k, r() % 2 ? 0.0f : r.f(-0.2f, 1.2f));
        for (u32 k = 0; k < 4; k++)
            stf(c + 0x70 + 4 * k, r.f(0.0f, 1.2f));
        // the pitch offset (semitones): as a rule near 0; now and then far out, or at the octaves where
        // 2^octave turns subnormal (-127..-149 octaves) or overflows (128 on)
        static const f32 lo[4] = {-3e6f, -1800.0f, 1500.0f, -24.0f}, hi[4] = {3e6f, -1490.0f, 1600.0f, 24.0f};
        u32 k = r() % 8 ? 3 : r() % 3;
        stf(c + 0x80, r.f(lo[k], hi[k]));
        stf(c + 0x84, r() % 2 ? 0.0f : r.f(-12.0f, 12.0f));
        st32(c + 0x88, r() % 50), st32(c + 0x8C, r() % 60);
        stf(c + 0x90, r.f(0.0f, 1.5f)), stf(c + 0x94, r.f(-1.5f, 1.5f)), stf(c + 0x98, r.f(-0.5f, 2.5f));
        stf(c + 0x9C, r.f(0.25f, 4.0f));
        st8(c + 0xA0, (u8)r()), st8(c + 0xA1, (u8)r()), st16(c + 0xA2, (u16)(r() % 4 ? 0 : r() % 60));
        st16(c + 0xA4, (u16)(r() % 60));
        st32(c + 0xA8, r() % 128), st32(c + 0xAC, r() % 128);
        st32(c + 0xB4, r() % 2), st32(c + 0xB8, r() % 11);
    };
    u32 head = CM + 8, prev = head;
    for (u32 i = 0; i < channels; i++) {
        u32 c = CHANNELS + CH_SIZE * i;
        params(c);
        st32(c + 0xD0, r() % 16 ? VOICES + VOICE_SIZE * i : 0);
        st32(prev, c + 0xD8), prev = c + 0xD8;
    }
    st32(prev, head);
    u32 h = 2166136261u;
    for (u32 f = 0; f < frames; f++) {
        for (u32 i = 0; i < channels; i++)
            if (r() % 6 == 0)
                params(CHANNELS + CH_SIZE * i);
        g::nw4r_snd_detail_ChannelManager_UpdateAllChannel(CM);
        g::nw4r_snd_detail_VoiceManager_UpdateAllVoices(MANAGER, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        for (u32 i = 0x10000; i < 0x90000 + CH_SIZE * channels; i++)
            h = (h ^ s_mem1[i]) * 16777619u;
    }
    return h;
}
