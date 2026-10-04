// Internal API of the Wii runtime under the decompiled code (not included by generated code).
#pragma once

#include <string>
#include <vector>

#include "guest.h"

namespace guest {

// ---- memory (memory.cpp)
void memory_init(u32 mem2_size);
// MEM2 as large as the game needs, not the Wii's 64 MB: with the Flash player and MobiClip left out
// (runtime/patches/players.cpp) the room and its menus run in 44 MB (host, NOMA_MEM2_MB); 42 is too
// little to make the room. The IPC buffers stay at its top (boot.cpp).
static const u32 MEM2_SIZE = 48u << 20;
void* platform_alloc_guest(u32 size);  // zeroed memory for MEM1/MEM2, one block each
u32 mem2_size();
// the locked cache: 16 KB of the CPU's L1 data cache at 0xE0000000, used as scratch memory (NW4R
// computes its model matrices there); a page of guest memory like RAM, so loads and stores reach
// it directly
static const u32 LC_BASE = 0xE0000000u, LC_SIZE = 16u << 10;
extern u8 g_locked_cache[LC_SIZE];
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
// text from the player (engine/system/keyboard.cpp; the 3DS: its system keyboard), in UTF-8: true
// with the text entered, false if cancelled
struct TextInput {
    enum Kind { ANY, LATIN, NUMBERS } kind = ANY;
    bool secret = false;     // shown as dots (PINs, passwords)
    bool multiline = false;
    u32 max_length = 0;      // in UTF-16 units
    std::string text;        // to start from
    std::string title;       // what it is for (may be empty)
};
bool platform_text_input(const TextInput& in, std::string& out);

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
void platform_log_idle();  // (the I/O thread, every 100 ms at least) write out the log if it is due

// ---- host threads beside the one that runs every guest thread
// The GPU's own thread (runtime/hw/gx/fifo.cpp), on a core of its own where the platform has a
// spare one; false when it runs none (the command decoder then runs inline, on the CPU's thread).
bool platform_gpu_thread_start(void (*fn)());
// The audio thread, which mixes the AX frames (runtime/hw/dsp_hle.cpp) like the Wii's DSP beside
// its CPU; false where there is no spare core for it (the frames are then mixed inline).
bool platform_audio_thread_start(void (*fn)());
// The I/O thread, which reads the channel's contents for IOS (runtime/hw/ios/ios.cpp) while the
// guest runs on; false where it can't start (reads are then done at once).
bool platform_io_thread_start(void (*fn)());
// A worker for a long piece of native work the guest mustn't wait for (a scene's load: reading a big
// file, building what it needs from it), running fn(arg) once and ending: below the guest's thread
// in priority, so that the guest (its sound thread with it) runs on and the work goes on while it
// waits; on the 3DS on the GPU thread's core. False where none can start (the caller then does the
// work itself).
bool platform_worker_start(void (*fn)(void*), void* arg);
// where the program's data is: the host's --data folder, the 3DS's sdmc:/3ds/noma (main.dol,
// content/, nand/, and the port's own data such as wuhu/island.ng3d)
const std::string& data_dir();
// the native heap's and the GPU-visible (linear) memory's free bytes, for logs; false where the
// platform doesn't count them (the host)
bool platform_native_memory(u32* heap_free, u32* linear_free);
// a wake-up between host threads: one waiting thread wakes per signal (or at the timeout)
struct HostSignal;
HostSignal* platform_signal_new();
void platform_signal(HostSignal* s);
void platform_wait(HostSignal* s, u64 timeout_us);

// ---- the sampling profiler (profile.cpp; NOMA_PROFILE=seconds in -DNOMA_PROFILE_CALLS builds)
#ifdef NOMA_PROFILE_CALLS
ProfStack* profile_new_stack(u32 thread);  // for a new guest thread
void profile_gpu_thread();                 // the GPU's thread: its own stack
void profile_audio_thread();               // the audio thread: its own stack
void profile_start();
void profile_report_due();  // the main thread logs a report when one is due
void profile_frame_end(bool report);  // (NOMA_LOAD_STATS) a game frame: report its samples if slow
#else
static inline void profile_gpu_thread() {}
static inline void profile_audio_thread() {}
static inline void profile_start() {}
static inline void profile_report_due() {}
static inline void profile_frame_end(bool) {}
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
// a guest code address for a native function (the same one every time), so that guest code can
// call it through a pointer: a vtable slot, a callback. The function takes the register ABI's
// arguments (u32 per integer register, in order, at most eight; f64 per float register).
u32 host_fn_address(void* fn);
template <class F>
static inline u32 host_fn(F* fn) { return host_fn_address((void*)fn); }

}  // namespace guest
