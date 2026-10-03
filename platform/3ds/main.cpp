// Nintendo 3DS build: runs the decompiled program on the 3DS. The game's data (main.dol,
// content/, nand/ as tools/stage_data.py makes them from the user's own WAD) lives on the SD card
// in sdmc:/3ds/noma.
//
// Controls (a Wii Remote held pointing at the screen): touch the bottom screen to point,
// A B 1(X) 2(Y) + (START) - (SELECT) and the D-pad as on the remote. The remote's HOME button
// has no counterpart: the Wii's HOME Button Menu isn't part of the port (engine/system).
#include <3ds.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <string>
#include <vector>

#include "gx_pica.h"
#include "runtime/hw/gx/raster.h"
#include "engine/input/remote.h"
#include "runtime/hw/ios/net.h"
#include "runtime/runtime.h"

void noma_start_guest();

// The linear heap holds guest memory (96 MB, see platform_alloc_guest) and the GPU's and DSP's
// buffers; the normal heap gets the rest (in the New 3DS's 178 MB mode, about 35 MB).
extern "C" u32 __ctru_linear_heap_size = 116u << 20;
extern "C" u32 __ctru_heap_size;

namespace guest {
bool load_dol(const std::vector<u8>& dol);
void setup_low_memory(u32 title_id_hi, u32 title_id_lo);
void ios_init(const std::string& nand_root, const std::string& content_dir);
void hw_init();

static FILE* s_log;
static u64 s_tick0;
static bool s_soc;  // the SOC service is up (online only)
static const char* DATA_DIR = "sdmc:/3ds/noma";

static LightLock s_log_lock;  // (the GPU's thread logs too)
__attribute__((constructor(102))) static void init_log_lock() { LightLock_Init(&s_log_lock); }

void log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    double t = (double)(svcGetSystemTick() - s_tick0) / SYSCLOCK_ARM11;
    LightLock_Lock(&s_log_lock);
    if (s_log) {
        fprintf(s_log, "[%8.3f] %s\n", t, buf);
        fflush(s_log);
    }
    svcOutputDebugString(buf, (s32)strlen(buf));  // emulators show it in their log
    printf("%.79s\n", buf);
    LightLock_Unlock(&s_log_lock);
}

// ---- sound: the AI DMA's blocks (32 kHz 16-bit stereo) queued to ndsp channel 0
static const int AUDIO_BUFS = 8;
static const u32 AUDIO_FRAMES = 96 * 8;  // 24 ms a buffer
static ndspWaveBuf s_wbuf[AUDIO_BUFS];
static int s_wcur;
static u32 s_wfill;
static bool s_audio;
static bool s_guest_linear = true;  // guest memory is in the linear heap (platform_alloc_guest)

static void audio_init() {
    if (R_FAILED(ndspInit())) {
        // ndsp needs the DSP firmware a homebrew setup dumps to sdmc:/3ds/dspfirm.cdc (DSP1)
        log("audio: ndspInit failed (no sdmc:/3ds/dspfirm.cdc?): no sound");
        return;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    // the AX voices are mixed natively (engine/audio/ax_render.cpp, as the Wii's DSP mixes them:
    // every voice, ramp, filter and aux effect), and the DSP plays the AI DMA's frames
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, 32000.0f);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = {1.0f, 1.0f};
    ndspChnSetMix(0, mix);
    s16* data = (s16*)linearAlloc(AUDIO_BUFS * AUDIO_FRAMES * 4);
    if (!data) {
        ndspExit();
        return;
    }
    memset(s_wbuf, 0, sizeof(s_wbuf));
    for (int i = 0; i < AUDIO_BUFS; i++) {
        s_wbuf[i].data_vaddr = data + i * AUDIO_FRAMES * 2;
        s_wbuf[i].nsamples = AUDIO_FRAMES;
        s_wbuf[i].status = NDSP_WBUF_DONE;
    }
    s_audio = true;
    log("audio: ndsp ready");
}

// buffers the DSP hasn't finished yet
static int audio_queued() {
    int n = 0;
    for (const ndspWaveBuf& wb : s_wbuf)
        n += wb.status == NDSP_WBUF_QUEUED || wb.status == NDSP_WBUF_PLAYING;
    return n;
}

// The AI DMA's frames, to the DSP in 24 ms buffers. Playing starts once AUDIO_PREBUFFER of them
// are queued, and when the DSP ran out (the CPU fell behind for longer than that) it pauses until
// the queue has filled again: one short pause instead of a gap at every buffer.
static const int AUDIO_PREBUFFER = 3;
void platform_audio_push(const s16* lr, u32 frames) {
    while (s_audio && frames) {
        ndspWaveBuf& wb = s_wbuf[s_wcur];
        if (wb.status != NDSP_WBUF_DONE && wb.status != NDSP_WBUF_FREE)
            return;  // every buffer still queued: the game runs ahead, drop this
        u32 n = std::min(frames, AUDIO_FRAMES - s_wfill);
        memcpy((s16*)wb.data_vaddr + 2 * s_wfill, lr, n * 4);
        s_wfill += n, lr += 2 * n, frames -= n;
        if (s_wfill == AUDIO_FRAMES) {
            int queued = audio_queued();
            if (queued == 0)
                ndspChnSetPaused(0, true);
            DSP_FlushDataCache(wb.data_vaddr, AUDIO_FRAMES * 4);
            ndspChnWaveBufAdd(0, &wb);
            if (queued + 1 >= AUDIO_PREBUFFER && ndspChnIsPaused(0))
                ndspChnSetPaused(0, false);
            s_wcur = (s_wcur + 1) % AUDIO_BUFS;
            s_wfill = 0;
        }
    }
}

// ---- Miis: the channel's face library reads the Wii Mii database; the Mii the player picks in
// the 3DS Mii Selector (on the first start, when SELECT is held while starting, or when the
// channel would send the player to the Mii Channel) goes into it, with the Miis already there
static std::string s_nand;

// the Mii Selector, and its Mii into the database: true if one was picked
static bool pick_mii(const std::string& path, bool exists, bool valid, std::vector<u8>& db) {
    MiiSelectorConf conf;
    miiSelectorInit(&conf);
    miiSelectorSetTitle(&conf, "Wii no Ma: choose your Mii");
    miiSelectorSetOptions(&conf, MIISELECTOR_CANCEL);
    MiiSelectorReturn ret;
    miiSelectorLaunch(&conf, &ret);
    if (ret.no_mii_selected) {
        log("mii: none chosen%s", exists ? " (keeping the database)" : "");
        return false;
    }
    u8 wii[0x4A];
    mii_3ds_to_wii((const u8*)&ret.mii, wii);
    db = rfl_add_mii(valid ? db : std::vector<u8>(), wii);
    mkdir((s_nand + "/shared2").c_str(), 0777);
    mkdir((s_nand + "/shared2/menu").c_str(), 0777);
    mkdir((s_nand + "/shared2/menu/FaceLib").c_str(), 0777);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    fwrite(db.data(), 1, db.size(), f);
    fclose(f);
    log("mii: the chosen Mii is in the database");
    return true;
}

// the database, if it's one this version writes
static bool read_mii_database(const std::string& path, bool& exists, std::vector<u8>& db) {
    db.assign(0x1F1E0, 0);
    exists = false;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    exists = true;
    bool valid = fread(db.data(), 1, db.size(), f) == db.size() && rfl_database_valid(db);
    fclose(f);
    return valid;
}

static void mii_setup(const std::string& nand) {
    s_nand = nand;
    std::string path = nand + "/shared2/menu/FaceLib/RFL_DB.dat";
    hidScanInput();
    bool exists;
    std::vector<u8> db;
    bool valid = read_mii_database(path, exists, db);
    if (exists && !valid)
        log("mii: %s is not one this version writes: choose the Mii again", path.c_str());
    if (valid && !(hidKeysHeld() & KEY_SELECT))
        return;
    pick_mii(path, exists, valid, db);
}

bool platform_choose_mii() {
    std::string path = s_nand + "/shared2/menu/FaceLib/RFL_DB.dat";
    bool exists;
    std::vector<u8> db;
    bool valid = read_mii_database(path, exists, db);
    if (!pick_mii(path, exists, valid, db))
        return false;
    aptSetChainloaderToSelf();  // (the face library reads the database when the app starts)
    return true;
}

// back to the HOME Menu, as the Wii channel would go back to the Wii Menu
void platform_quit(const char* why) {
    log("quit: %s", why);
    if (s_log)
        fflush(s_log);
    if (s_audio)
        ndspExit();
    if (s_soc)
        socExit();
    gfxExit();
    exit(0);
}

void platform_fatal(const char* msg) {
    printf("\x1b[31mfatal: %s\x1b[0m\n", msg);
    if (s_log)
        fflush(s_log);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;
        gspWaitForVBlank();
    }
    gfxExit();
    exit(1);
}

void platform_backtrace(const char* why) { log("%s", why); }

// Guest memory (MEM1, MEM2: 96 MB) goes in the linear heap while it fits, else in the normal heap.

void* platform_alloc_guest(u32 size) {
    void* p = s_guest_linear ? linearAlloc(size) : nullptr;
    if (!p) {
        if (s_guest_linear)
            log("memory: no %u MB in the linear heap (%u KB free): guest memory in the heap", size >> 20,
                (u32)(linearSpaceFree() >> 10));
        s_guest_linear = false;
        return calloc(1, size);
    }
    memset(p, 0, size);
    return p;
}

// system ticks (SYSCLOCK_ARM11, 268 MHz) to another clock: t * hz / SYSCLOCK_ARM11 as 32x32-bit
// multiplies by hz / SYSCLOCK_ARM11 in 0.32 fixed point (the ARM11 has no 64-bit division)
template <u64 HZ>
static inline u64 ticks_to(u64 t) {
    static const u32 MUL = (u32)((HZ << 32) / SYSCLOCK_ARM11);
    return (u64)(u32)(t >> 32) * MUL + (((u64)(u32)t * MUL) >> 32);
}

u64 host_time_us() { return ticks_to<1000000ull>(svcGetSystemTick() - s_tick0); }

u64 host_timebase() { return ticks_to<TB_HZ>(svcGetSystemTick() - s_tick0); }

void host_sleep_us(u64 us) { svcSleepThread((s64)us * 1000); }

// ---- the GPU's thread: on the New 3DS's third core (all the app's), else on the system core
// (the app may take up to 80% of it), just above the main thread's priority
struct HostSignal {
    LightEvent e;
};
HostSignal* platform_signal_new() {
    HostSignal* s = new HostSignal;
    LightEvent_Init(&s->e, RESET_ONESHOT);
    return s;
}
void platform_signal(HostSignal* s) { LightEvent_Signal(&s->e); }
void platform_wait(HostSignal* s, u64 timeout_us) { LightEvent_WaitTimeout(&s->e, (s64)timeout_us * 1000); }

static void (*s_gpu_fn)();
bool platform_gpu_thread_start(void (*fn)()) {
    if (getenv("NOMA_GX_INLINE"))
        return false;
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    int core = n3ds ? 2 : 1;
    if (!n3ds)
        APT_SetAppCpuTimeLimit(80);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    s_gpu_fn = fn;
    Thread t = threadCreate([](void*) { s_gpu_fn(); }, nullptr, 128 * 1024, prio - 1, core, true);
    if (!t) {
        log("gx: no thread on core %d: commands decoded on the main thread", core);
        return false;
    }
    log("gx: the GPU's thread runs on core %d", core);
    return true;
}

// ---- input: the 3DS controls as a Wii Remote
static WiimoteInput s_in;

static void poll_input() {
    hidScanInput();
    u32 held = hidKeysHeld();
    u16 b = 0;
    if (held & KEY_A) b |= WM_A;
    if (held & KEY_B) b |= WM_B;
    if (held & KEY_X) b |= WM_ONE;
    if (held & KEY_Y) b |= WM_TWO;
    if (held & KEY_START) b |= WM_PLUS;
    if (held & KEY_SELECT) b |= WM_MINUS;
    if (held & KEY_DUP) b |= WM_UP;
    if (held & KEY_DDOWN) b |= WM_DOWN;
    if (held & KEY_DLEFT) b |= WM_LEFT;
    if (held & KEY_DRIGHT) b |= WM_RIGHT;
    s_in.buttons = b;
    if (held & KEY_TOUCH) {
        touchPosition t;
        hidTouchRead(&t);
        s_in.x = (float)t.px / 160.0f - 1.0f;
        s_in.y = (float)t.py / 120.0f - 1.0f;
    }
    // the circle pad nudges the pointer too
    circlePosition c;
    hidCircleRead(&c);
    if (c.dx * c.dx + c.dy * c.dy > 20 * 20) {
        s_in.x += (float)c.dx / 156.0f * 0.03f;
        s_in.y -= (float)c.dy / 156.0f * 0.03f;
        s_in.x = s_in.x < -1 ? -1 : s_in.x > 1 ? 1 : s_in.x;
        s_in.y = s_in.y < -1 ? -1 : s_in.y > 1 ? 1 : s_in.y;
    }
    s_in.pointing = true;
    // NOMA_INPUT in env.txt: scripted presses on top (unattended runs in an emulator)
    WiimoteInput in = s_in;
    if (input_script_active())
        input_script_apply(in);
    wiimote_set_input(in);
}

// ---- video: the GPU renderer draws the top screen itself; with the software renderer
// (no GPU), the external frame buffer is scaled to the top screen here
static std::vector<u32> s_rgba;
static bool s_gpu;

void platform_present(u32 xfb) {
    NOMA_PROF_SCOPE("(present)");
    if (!aptMainLoop()) {
        gfxExit();
        exit(0);
    }
    poll_input();
    if (s_gpu)
        return;
    const XfbCopy* r = xfb ? gx_xfb_region(xfb) : nullptr;
    if (!r)
        return;
    const XfbCopy& c = *r;
    s_rgba.resize(c.width * c.height);
    gx_xfb_to_rgba(xfb, c.width, c.height, c.stride, s_rgba.data());
    u16 fw, fh;
    u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fw, &fh);  // 240 x 400, column-major
    for (u32 x = 0; x < 400; x++) {
        u32 sx = x * c.width / 400;
        u8* col = fb + x * 240 * 3;
        for (u32 y = 0; y < 240; y++) {
            u32 sy = y * c.height / 240;
            u32 p = s_rgba[sy * c.width + sx];
            u8* o = col + (239 - y) * 3;
            o[0] = (u8)(p >> 16), o[1] = (u8)(p >> 8), o[2] = (u8)p;
        }
    }
    gfxFlushBuffers();
    gfxSwapBuffers();
}

}  // namespace guest

static std::vector<u8> read_file(const std::string& path) {
    std::vector<u8> v;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return v;
    fseek(f, 0, SEEK_END);
    v.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    if (fread(v.data(), 1, v.size(), f) != v.size())
        v.clear();
    fclose(f);
    return v;
}

// debugging switches (NOMA_DEBUG_NET=1, ...) as KEY=VALUE lines in env.txt next to the data.
// Read before the other static constructors: many switches are file-scope statics.
__attribute__((constructor(101))) static void read_env_txt() {
    if (FILE* env = fopen((std::string(guest::DATA_DIR) + "/env.txt").c_str(), "r")) {
        char line[256];
        while (fgets(line, sizeof(line), env)) {
            line[strcspn(line, "\r\n")] = 0;
            char* eq = strchr(line, '=');
            if (line[0] != '#' && eq) {
                *eq = 0;
                setenv(line, eq + 1, 1);
            }
        }
        fclose(env);
    }
}

int main(int argc, char** argv) {
    osSetSpeedupEnable(true);
    gfxInitDefault();
    gfxSetDoubleBuffering(GFX_TOP, true);
    consoleInit(GFX_BOTTOM, nullptr);
    guest::s_tick0 = svcGetSystemTick();
    std::string data = guest::DATA_DIR;
    guest::s_log = fopen((data + "/noma.log").c_str(), "w");
    printf("Wii no Ma 3DS\n");
    std::vector<u8> dol = read_file(data + "/main.dol");
    if (dol.empty()) {
        printf("no %s/main.dol\n(see README: tools/stage_data.py)\n", data.c_str());
        guest::platform_fatal("missing game data");
    }
    // online (NOMA_OFFLINE=0), the guest's network calls (/dev/net/ip/top) go to host sockets:
    // start the SOC service, which takes a 1 MB buffer
    if (!guest::net_offline()) {
        u32* soc_buf = (u32*)memalign(0x1000, 0x100000);
        guest::s_soc = soc_buf && R_SUCCEEDED(socInit(soc_buf, 0x100000));
        if (!guest::s_soc)
            printf("socInit failed: no network\n");
    }
    guest::mii_setup(data + "/nand");
    guest::memory_init(64u << 20);
    guest::log("memory: heap %u KB, linear heap %u KB (%u KB free after guest memory)", __ctru_heap_size >> 10,
               __ctru_linear_heap_size >> 10, (u32)(linearSpaceFree() >> 10));
    guest::audio_init();
    if (!guest::load_dol(dol))
        guest::platform_fatal("bad main.dol");
    guest::setup_low_memory(0x00010001, 0x4843494A);  // HCIJ
    guest::ios_init(data + "/nand", data + "/content");
    guest::hw_init();
    static guest::PicaRenderer pica;
    static guest::SoftRenderer soft;
    if (pica.init()) {
        guest::s_gpu = true;
        guest::gx_set_renderer(&pica);
    } else {
        printf("GPU renderer unavailable: software rendering\n");
        guest::gx_set_renderer(&soft);
    }
    guest::threads_init();
    guest::profile_start();
    noma_start_guest();
    gfxExit();
    return 0;
}
