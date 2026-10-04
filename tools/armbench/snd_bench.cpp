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
u64 nw4r_snd_detail_VoiceManager_UpdateAllVoices(u32, u32, u32, u32, u32, u32, u32, u32, f64, f64, f64, f64, f64,
                                                 f64, f64, f64);
}  // namespace g

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
