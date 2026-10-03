// Internal API of the Wii runtime under the decompiled code (not included by generated code).
#pragma once

#include <string>
#include <vector>

#include "guest.h"

namespace guest {

// ---- memory (memory.cpp)
void memory_init(u32 mem2_size);
void* platform_alloc_guest(u32 size);  // zeroed memory for MEM1/MEM2 (the 3DS: its linear heap)
u32 mem2_size();
void read_g(void* out, u32 src, u32 n);
void write_g(u32 dst, const void* in, u32 n);
std::string read_string(u32 addr, u32 max = 1024);
// write generations of guest memory (4 KB pages): the guest's cache flushes, IOS and GPU writes
// mark what changed; mem_generation is the newest mark in a range (0: never written that way)
u32 mem_mark_written(u32 addr, u32 size);
u32 mem_generation(u32 addr, u32 size);

// ---- logging / platform (platform/<host|3ds>)
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void platform_fatal(const char* msg);
// the game is done (it reset or powered off the console, e.g. "return to the Wii Menu"): leave
void platform_quit(const char* why);
// the player picks a Mii for the Wii Mii database (the 3DS's Mii Selector), which the program can
// only see when it starts again: true if one was picked and the next quit starts the app again;
// false where there's no Mii Selector (the host) or none was picked
bool platform_choose_mii();

// Miis (runtime/mii.cpp): a 3DS MiiData (0x5C bytes) as the Wii's RFLCharData (0x4A bytes), and a
// fresh /shared2/menu/FaceLib/RFL_DB.dat holding up to 100 of those
void mii_3ds_to_wii(const u8* mii_3ds, u8* char_data);
std::vector<u8> rfl_make_database(const u8* const* miis, int count);
std::vector<u8> rfl_add_mii(const std::vector<u8>& db, const u8* mii);
bool rfl_database_valid(const std::vector<u8>& db);
u64 host_time_us();  // monotonic microseconds
u64 host_timebase();  // the same clock in Wii timebase ticks (TB_HZ)
void host_sleep_us(u64 us);
void platform_present(u32 xfb);  // a vertical retrace: show the frame buffer at xfb (0: none)

// ---- host threads beside the one that runs every guest thread
// The GPU's own thread (runtime/hw/gx/fifo.cpp), on a core of its own where the platform has a
// spare one; false when it runs none (the command decoder then runs inline, on the CPU's thread).
bool platform_gpu_thread_start(void (*fn)());
// a wake-up between host threads: one waiting thread wakes per signal (or at the timeout)
struct HostSignal;
HostSignal* platform_signal_new();
void platform_signal(HostSignal* s);
void platform_wait(HostSignal* s, u64 timeout_us);

// ---- sound on the 3DS's DSP (engine/audio/dsp_ndsp.cpp): AX voices as ndsp channels, nothing
// mixed by the CPU. Off where the platform has no DSP for it (the host mixes, dsp_ax.cpp).
#ifdef __3DS__
bool audio_dsp_native();
void audio_dsp_init();
void audio_dsp_frame(u32 list_addr, u32 words);
void audio_dsp_flushed(u32 addr, u32 n);
// linear heap the voices' decoded sounds may still take (the renderer's textures leave it free)
u32 audio_dsp_linear_reserve();
#else
static inline bool audio_dsp_native() { return false; }
static inline void audio_dsp_frame(u32, u32) {}
static inline void audio_dsp_flushed(u32, u32) {}
#endif

// ---- the sampling profiler (profile.cpp; NOMA_PROFILE=seconds in -DNOMA_PROFILE_CALLS builds)
#ifdef NOMA_PROFILE_CALLS
ProfStack* profile_new_stack(u32 thread);  // for a new guest thread
void profile_gpu_thread();                 // the GPU's thread: its own stack
void profile_start();
void profile_report_due();  // the main thread logs a report when one is due
#else
static inline void profile_gpu_thread() {}
static inline void profile_start() {}
static inline void profile_report_due() {}
#endif

// ---- CPU state (cpu.cpp)
extern u32 g_msr;           // only EE (0x8000) matters
extern u32 g_gqr[8];        // quantization registers for psq_l/psq_st
extern u32 g_hid[5];
u64 timebase();             // Wii timebase: 60.75 MHz
static const u64 TB_HZ = 243000000ull / 4;
void timebase_init();       // set to the console's clock (rtc_seconds), as the system does at boot
void set_decrementer(u32 value);

// ---- interrupts and events (irq.cpp)
// Interrupt sources, as PI interrupt cause bits (0xCC003000)
enum : u32 {
    PI_INT_GPERR = 1u << 0,
    PI_INT_RSW = 1u << 1,
    PI_INT_DI = 1u << 2,
    PI_INT_SI = 1u << 3,
    PI_INT_EXI = 1u << 4,
    PI_INT_AI = 1u << 5,
    PI_INT_DSP = 1u << 6,
    PI_INT_MEM = 1u << 7,
    PI_INT_VI = 1u << 8,
    PI_INT_PE_TOKEN = 1u << 9,
    PI_INT_PE_FINISH = 1u << 10,
    PI_INT_CP = 1u << 11,
    PI_INT_DEBUG = 1u << 12,
    PI_INT_HSP = 1u << 13,
    PI_INT_ACR = 1u << 14,  // Hollywood (IPC)
    PI_INT_RSWST = 1u << 16,
};
void pi_raise(u32 bits);
void pi_clear(u32 bits);
u32 pi_cause();
u32 pi_mask();
void pi_set_mask(u32 m);
// deliver pending interrupts if the guest has them enabled; called at safe points
void check_interrupts();
// the guest has nothing to run: wait for the next device event and deliver it
void idle_wait();
// charge the time since the last call to the guest thread that ran (irq.cpp)
void account_thread_time();
// periodic device work (VI retrace, alarms, audio), driven by host time
void poll_devices();
extern u64 g_coarse_us;  // host time at the last device poll: "now" where milliseconds don't matter

// ---- guest threads (threads.cpp)
u32 os_save_context(u32 ctx);
void os_load_context(u32 ctx);
void threads_init();
void threads_forget_context(u32 ctx);  // the context is set up for a new thread

// ---- function dispatch (dispatch.cpp)
const char* fn_name(u32 guest_addr);

}  // namespace guest
