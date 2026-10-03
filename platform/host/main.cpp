// Host (macOS/Linux) build: runs the decompiled program without graphics, for debugging the
// decompilation and the hardware emulation quickly.
//
//   noma-host --data DIR    (DIR: main.dol, content/, nand/ as made by tools/stage_data.py)
#include <execinfo.h>
#include <csignal>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>

#include "runtime/hw/gx/raster.h"
#include "runtime/hw/ios/bt.h"
#include "runtime/runtime.h"

bool write_png(const std::string& path, const u32* rgba, u32 w, u32 h);

void noma_start_guest();
void noma_trace_dump();

namespace guest {
bool load_dol(const std::vector<u8>& dol);
void setup_low_memory(u32 title_id_hi, u32 title_id_lo);
void ios_init(const std::string& nand_root, const std::string& content_dir);
void hw_init();

static FILE* s_log;
static auto s_t0 = std::chrono::steady_clock::now();

void log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_t0).count();
    static std::mutex s_lock;  // (the GPU's thread logs too)
    std::lock_guard<std::mutex> lock(s_lock);
    fprintf(stderr, "[%8.3f] %s\n", t, buf);
    if (s_log) {
        fprintf(s_log, "[%8.3f] %s\n", t, buf);
        fflush(s_log);
    }
}

void platform_quit(const char* why) {
    log("quit: %s", why);
    if (s_log)
        fflush(s_log);
    exit(0);
}

void platform_fatal(const char* msg) {
    ::noma_trace_dump();
    fprintf(stderr, "fatal: %s\n", msg);
    void* frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, 2);
    if (s_log) {
        fflush(s_log);
        backtrace_symbols_fd(frames, n, fileno(s_log));
    }
    fflush(stderr);
}

void platform_backtrace(const char* why) {
    log("%s", why);
    void* frames[64];
    int n = backtrace(frames, 64);
    char** syms = backtrace_symbols(frames, n);
    for (int i = 0; i < n; i++)
        log("  %s", syms[i]);
    free(syms);
}

u64 host_time_us() {
    return (u64)std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - s_t0)
        .count();
}

void* platform_alloc_guest(u32 size) { return calloc(1, size); }

u64 host_timebase() {
    u64 ns = (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - s_t0)
                 .count();
    return ns / 1000 * TB_HZ / 1000000 + ns % 1000 * TB_HZ / 1000000000;
}

void host_sleep_us(u64 us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }

// ---- the GPU's thread: only with NOMA_GX_THREAD=1 (inline, a run is the same every time)
struct HostSignal {
    std::mutex m;
    std::condition_variable cv;
    bool set = false;
};
HostSignal* platform_signal_new() { return new HostSignal; }
void platform_signal(HostSignal* s) {
    {
        std::lock_guard<std::mutex> lock(s->m);
        s->set = true;
    }
    s->cv.notify_one();
}
void platform_wait(HostSignal* s, u64 timeout_us) {
    std::unique_lock<std::mutex> lock(s->m);
    s->cv.wait_for(lock, std::chrono::microseconds(timeout_us), [&] { return s->set; });
    s->set = false;
}
bool platform_gpu_thread_start(void (*fn)()) {
    if (!getenv("NOMA_GX_THREAD"))
        return false;
    std::thread(fn).detach();
    return true;
}

// NOMA_INPUT (runtime/hw/ios/input_script.cpp): scripted Wii Remote input
static void scripted_input() {
    if (!input_script_active())
        return;
    static WiimoteInput in;
    in.buttons = 0;
    input_script_apply(in);
    in.pointing = true;
    wiimote_set_input(in);
}

// NOMA_DUMP_AUDIO=file.wav: what the AI DMA plays (32 kHz stereo), for listening to offline
void platform_audio_push(const s16* lr, u32 frames) {
    static const char* path = getenv("NOMA_DUMP_AUDIO");
    static FILE* f;
    static u32 bytes;
    if (!path)
        return;
    auto header = [](FILE* out, u32 data) {
        auto put32 = [&](u32 v) { fwrite(&v, 4, 1, out); };
        auto put16 = [&](u16 v) { fwrite(&v, 2, 1, out); };
        fwrite("RIFF", 1, 4, out), put32(36 + data), fwrite("WAVEfmt ", 1, 8, out);
        put32(16), put16(1), put16(2), put32(32000), put32(32000 * 4), put16(4), put16(16);
        fwrite("data", 1, 4, out), put32(data);
    };
    if (!f) {
        f = fopen(path, "wb");
        if (!f)
            return;
        header(f, 0);
    }
    fwrite(lr, 4, frames, f);  // host order: little-endian, as WAV wants
    bytes += frames * 4;
    long at = ftell(f);
    fseek(f, 0, SEEK_SET);
    header(f, bytes);
    fseek(f, at, SEEK_SET);
}

// The software renderer. With NOMA_DUMP_FRAMES (and not NOMA_RENDER_ALL), it only rasterizes the
// few frames before each one that is saved (the GPU state is kept up to date all along): a run
// that only needs the saved frames goes at the game's own speed. Until the game copies the EFB
// to a texture (Mii faces, effects): what it draws for that is needed whenever it happens, so
// from then on every frame is drawn.
struct HostRenderer : SoftRenderer {
    bool drawing = true;
    bool texture_copies = false;  // seen one: draw every frame
    void draw(u32 prim, u32 vat, u32 count, const u8* data) override {
        if (drawing)
            SoftRenderer::draw(prim, vat, count, data);
    }
    void efb_copy(u32 value) override {
        if (!(value & 0x4000) && !texture_copies) {  // (bit 14: a copy to the frame buffer)
            texture_copies = true;
            if (!drawing)
                log("host: the EFB was copied to a texture in a frame that wasn't drawn; drawing every frame "
                    "from now on");
        }
        SoftRenderer::efb_copy(value);
    }
};
static HostRenderer s_renderer;

// NOMA_DUMP_FRAMES=N: every Nth displayed frame goes to frames/NNNNN.png
void platform_present(u32 xfb) {
    NOMA_PROF_SCOPE("(present)");
    scripted_input();
    static int every = getenv("NOMA_DUMP_FRAMES") ? atoi(getenv("NOMA_DUMP_FRAMES")) : 0;
    // (the GPU's own thread draws a frame later than this thread counts it: then draw them all)
    static bool all = every <= 0 || getenv("NOMA_RENDER_ALL") || getenv("NOMA_GX_THREAD");
    static u32 n;
    n++;
    s_renderer.drawing = all || s_renderer.texture_copies || (n + 4) % (u32)every < 5;
    if (every <= 0 || n % (u32)every || !xfb)
        return;
    const XfbCopy* r = gx_xfb_region(xfb);
    if (!r)
        return;
    const XfbCopy& c = *r;
    std::vector<u32> rgba(c.width * c.height);
    gx_xfb_to_rgba(xfb, c.width, c.height, c.stride, rgba.data());
    mkdir("frames", 0755);
    char path[64];
    snprintf(path, sizeof(path), "frames/%05u.png", n);
    if (write_png(path, rgba.data(), c.width, c.height))
        log("frame %u -> %s (xfb %08X, %ux%u)", n, path, xfb, c.width, c.height);
}

}  // namespace guest

static std::vector<u8> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// timeout(1) / ^C: show what the guest was doing
namespace guest {
void threads_dump();
void platform_backtrace(const char* why);
}

namespace guest {
void memory_report();
void memory_dump();
bool run_selftest(const std::string& name);
}

static void on_signal(int sig) {
    ::noma_trace_dump();
    if (getenv("NOMA_DEBUG_MEM"))
        guest::memory_report();
    if (getenv("NOMA_DUMP_MEM"))
        guest::memory_dump();
    if (getenv("NOMA_DEBUG_THREADS")) {
        guest::threads_dump();
        guest::platform_backtrace("host stack at the signal:");
    }
    guest::log("stopped by signal %d", sig);
    _exit(128 + sig);
}

int main(int argc, char** argv) {
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    std::string data = "data";
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--data" && i + 1 < argc)
            data = argv[++i];
    }
    guest::s_log = fopen((data + "/noma.log").c_str(), "w");
    std::vector<u8> dol = read_file(data + "/main.dol");
    if (dol.empty()) {
        fprintf(stderr, "no %s/main.dol (run tools/stage_data.py)\n", data.c_str());
        return 1;
    }
    guest::memory_init(64u << 20);
    if (!guest::load_dol(dol)) {
        fprintf(stderr, "bad main.dol\n");
        return 1;
    }
    guest::setup_low_memory(0x00010001, 0x4843494A);  // HCIJ
    if (const char* t = getenv("NOMA_SELFTEST"))
        return guest::run_selftest(t) ? 0 : 2;
    guest::ios_init(data + "/nand", data + "/content");
    guest::hw_init();
    if (!getenv("NOMA_NO_RENDER"))
        guest::gx_set_renderer(&guest::s_renderer);
    guest::threads_init();
    guest::profile_start();
    noma_start_guest();
    return 0;
}
