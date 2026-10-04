// Nintendo 3DS build: runs the decompiled program on the 3DS. The game's data (main.dol,
// content/, nand/ as tools/stage_data.py makes them from the user's own WAD) lives on the SD card
// in sdmc:/3ds/noma.
//
// Controls (a Wii Remote held pointing at the screen): touch the bottom screen to point,
// A B 1(X) 2(Y) + (START) - (SELECT) and the D-pad as on the remote. Left alone for 3 s, the
// pointer leaves the screen, as a remote put down does. The remote's HOME button has no
// counterpart: the Wii's HOME Button Menu isn't part of the port (engine/system).
#include <3ds.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>
#include <string>
#include <vector>

#include "gx_pica.h"
#include "runtime/hw/gx/raster.h"
#include "engine/input/remote.h"
#include "runtime/hw/ios/ios.h"
#include "runtime/hw/ios/net.h"
#include "runtime/runtime.h"

void noma_start_guest();

// The normal heap's addresses run from OS_HEAP_AREA_BEGIN to OS_HEAP_AREA_END (96 MB: the main
// thread's stack is just above them, at 0x10000000), so it can't take all the memory: it gets what's
// free but LINEAR_MIN, up to HEAP_MAX, and the linear heap the rest. (A real console's kernel refuses
// a heap that runs past them; Azahar let 120 MB through, and the first start on hardware stopped here.) The linear heap
// holds what the GPU and the DSP read (textures, vertices, sound buffers: GPU_RESERVE) and as much
// guest memory as fits beside it (see platform_alloc_guest), the normal heap the rest of it.
extern "C" u32 __ctru_linear_heap_size, __ctru_heap_size, __ctru_heap, __ctru_linear_heap;
extern "C" char *fake_heap_start, *fake_heap_end;
// (GPU_RESERVE: the PICA's vertex and index buffers, the textures it keeps, read-backs and sound
// take about 8 MB in the room; LINEAR_MIN also has room for the frame buffers. MEM1 is the
// program's own .bss, guest::g_mem1, already committed when this runs)
static const u32 GPU_RESERVE = 16u << 20, LINEAR_MIN = GPU_RESERVE + (8u << 20),
                 HEAP_MAX = OS_HEAP_AREA_END - OS_HEAP_AREA_BEGIN;

extern "C" void __system_allocateHeaps(void) {
    Handle reslimit = 0;
    if (R_FAILED(svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE)))
        svcBreak(USERBREAK_PANIC);
    s64 max_commit = 0, commit = 0;
    ResourceLimitType type = RESLIMIT_COMMIT;
    svcGetResourceLimitLimitValues(&max_commit, reslimit, &type, 1);
    svcGetResourceLimitCurrentValues(&commit, reslimit, &type, 1);
    svcCloseHandle(reslimit);
    u32 free = (u32)(max_commit - commit) & ~0xFFFu;
    __ctru_heap_size = free > LINEAR_MIN ? std::min(free - LINEAR_MIN, HEAP_MAX) : free / 2;
    __ctru_heap_size &= ~0xFFFu;
    __ctru_linear_heap_size = free - __ctru_heap_size;
    if (R_FAILED(svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0, __ctru_heap_size, MEMOP_ALLOC,
                                  (MemPerm)(MEMPERM_READ | MEMPERM_WRITE))) ||
        R_FAILED(svcControlMemory(&__ctru_linear_heap, 0, 0, __ctru_linear_heap_size, MEMOP_ALLOC_LINEAR,
                                  (MemPerm)(MEMPERM_READ | MEMPERM_WRITE))))
        svcBreak(USERBREAK_PANIC);
    mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
    fake_heap_start = (char*)__ctru_heap;
    fake_heap_end = fake_heap_start + __ctru_heap_size;
}

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

// noma.log is buffered (16 KB) and written at most every LOG_FLUSH_TICKS: a write per line was an SD
// card access on the CPU's thread for each of the ~400 lines of a start. The I/O thread, which
// wakes every 100 ms, writes what a stopped program left (platform_log_idle); a fatal error, a
// crash and quitting write it at once.
static const u64 LOG_FLUSH_TICKS = SYSCLOCK_ARM11 / 2;
static u64 s_log_flushed;  // (ticks)
static bool s_log_dirty;

static void log_flush_due(u64 now) {  // (with s_log_lock)
    if (s_log && s_log_dirty && now - s_log_flushed >= LOG_FLUSH_TICKS) {
        fflush(s_log);
        s_log_dirty = false;
        s_log_flushed = now;
    }
}

void platform_log_idle() {
    if (!s_log_dirty)
        return;
    LightLock_Lock(&s_log_lock);
    log_flush_due(svcGetSystemTick());
    LightLock_Unlock(&s_log_lock);
}

void log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    u64 now = svcGetSystemTick();
    double t = (double)(now - s_tick0) / SYSCLOCK_ARM11;
    LightLock_Lock(&s_log_lock);
    if (s_log) {
        fprintf(s_log, "[%8.3f] %s\n", t, buf);
        s_log_dirty = true;
        log_flush_due(now);
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
    ndspChnSetPaused(0, true);  // (until the first buffers are queued)
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

// The AI DMA's frames, to the DSP in 24 ms buffers. Playing starts once s_prebuffer of them are
// queued, and when the DSP ran out (the CPU fell behind for longer than that) it pauses until the
// queue has filled again: one short pause instead of a gap at every buffer. Each time it runs out
// the queue it waits for grows by a buffer (up to 6, 144 ms): a game that keeps falling behind by a
// little gets a little more latency instead of a crackle every few frames.
static int s_prebuffer = 3;
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
            if (queued == 0 && !ndspChnIsPaused(0)) {  // ran out while playing
                ndspChnSetPaused(0, true);
                if (s_prebuffer < AUDIO_BUFS - 2)
                    log("audio: the DSP ran out; %d buffers (%d ms) before playing again", ++s_prebuffer,
                        s_prebuffer * 24);
            }
            DSP_FlushDataCache(wb.data_vaddr, AUDIO_FRAMES * 4);
            ndspChnWaveBufAdd(0, &wb);
            if (queued + 1 >= s_prebuffer && ndspChnIsPaused(0))
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

// the system keyboard (a library applet: the game waits while it's up)
bool platform_text_input(const TextInput& in, std::string& out) {
    SwkbdState kb;
    SwkbdType type = in.kind == TextInput::NUMBERS ? SWKBD_TYPE_NUMPAD
                     : in.kind == TextInput::LATIN ? SWKBD_TYPE_QWERTY
                                                   : SWKBD_TYPE_NORMAL;
    int len = (int)std::min<u32>(std::max<u32>(in.max_length, 1), 0x3FF);
    swkbdInit(&kb, type, 2, len);
    swkbdSetInitialText(&kb, in.text.c_str());
    if (!in.title.empty())
        swkbdSetHintText(&kb, in.title.c_str());
    if (in.secret)
        swkbdSetPasswordMode(&kb, SWKBD_PASSWORD_HIDE_DELAY);
    if (in.multiline)
        swkbdSetFeatures(&kb, SWKBD_MULTILINE);
    std::vector<char> buf(len * 4 + 1);  // (UTF-8: up to 4 bytes a unit... and its NUL)
    SwkbdButton b = swkbdInputText(&kb, buf.data(), buf.size());
    if (b != SWKBD_BUTTON_CONFIRM)
        return false;
    out = buf.data();
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

// Guest memory (MEM2; MEM1 is a static array; nothing reads it by DMA): in the linear heap while
// GPU_RESERVE stays free there, else in the normal heap.
void* platform_alloc_guest(u32 size) {
    void* p = linearSpaceFree() >= size + GPU_RESERVE ? linearAlloc(size) : nullptr;
    log("memory: %u MB of guest memory in the %s heap", size >> 20, p ? "linear" : "normal");
    if (!p)
        p = memalign(64, size);
    if (p)
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

// ---- ARM exceptions (data/prefetch aborts, undefined instructions, VFP): logged with the
// registers and the guest's function stack (profiling builds), where the process would otherwise
// just end. Each thread that runs guest or GPU work sets the handler with its own stack.
static void on_arm_exception(ERRF_ExceptionInfo* e, CpuRegisters* r) {
    guest::fatal("ARM exception %u at pc %08lX (lr %08lX, sp %08lX), address %08lX, fsr %08lX; r0-r3 %08lX %08lX "
                 "%08lX %08lX r4-r7 %08lX %08lX %08lX %08lX",
                 (unsigned)e->type, r->pc, r->lr, r->sp, e->far, e->fsr, r->r[0], r->r[1], r->r[2], r->r[3],
                 r->r[4], r->r[5], r->r[6], r->r[7]);
}
// operator new failing aborts without a word (-fno-exceptions): say so first
static void on_out_of_memory() {
    guest::fatal("out of memory: the heap has %u KB in use of %u KB", (u32)(mallinfo().uordblks >> 10),
                 __ctru_heap_size >> 10);
}
static void catch_arm_exceptions() {
    const u32 STACK = 32 << 10;
    u8* stack = (u8*)memalign(8, STACK);
    if (stack)
        threadOnException(on_arm_exception, stack + STACK, WRITE_DATA_TO_HANDLER_STACK);
}

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
    Thread t = threadCreate(
        [](void*) {
            catch_arm_exceptions();
            s_gpu_fn();
        },
        nullptr, 128 * 1024, prio - 1, core, true);
    if (!t) {
        log("gx: no thread on core %d: commands decoded on the main thread", core);
        return false;
    }
    log("gx: the GPU's thread runs on core %d", core);
    return true;
}

// ---- the audio thread (NOMA_AX_THREAD=1, New 3DS only): on the system core (the GPU's thread has
// core 2; the app may take up to 80% of the system core), just above the main thread's priority.
// Not by default: in Azahar, which runs the cores one after another on one host thread, every
// frame's handoff became a wait (boot screens at 1-2.5 fps instead of 50-59, the room ~10%
// slower); whether it pays off on hardware is still to measure. The frames are mixed inline.
static void (*s_audio_fn)();
bool platform_audio_thread_start(void (*fn)()) {
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    const char* e = getenv("NOMA_AX_THREAD");
    const int core = e ? atoi(e) : 0;
    if (!n3ds || (core != 1 && core != 2))
        return false;
    if (core == 1)
        APT_SetAppCpuTimeLimit(80);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    s_audio_fn = fn;
    // (on core 2 above the GPU's thread: a frame is short and the next one waits for it)
    Thread t = threadCreate([](void*) { s_audio_fn(); }, nullptr, 64 * 1024, prio - 2, core, true);
    if (!t) {
        log("ax: no thread on core %d: the frames are mixed inline", core);
        return false;
    }
    log("ax: the frames are mixed on core %d", core);
    return true;
}

// ---- the I/O thread (runtime/hw/ios/ios.cpp): on the main thread's core, above it; it mostly
// waits for the SD card in the FS service, and the guest runs meanwhile
static void (*s_io_fn)();
bool platform_io_thread_start(void (*fn)()) {
    if (getenv("NOMA_IO_INLINE"))
        return false;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    s_io_fn = fn;
    Thread t = threadCreate(
        [](void*) {
            catch_arm_exceptions();
            s_io_fn();
        },
        nullptr, 32 * 1024, prio - 1, -2, true);
    if (!t) {
        log("ios: no I/O thread: reads are done at once");
        return false;
    }
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
    bool nudged = c.dx * c.dx + c.dy * c.dy > 20 * 20;
    if (nudged) {
        s_in.x += (float)c.dx / 156.0f * 0.03f;
        s_in.y -= (float)c.dy / 156.0f * 0.03f;
        s_in.x = s_in.x < -1 ? -1 : s_in.x > 1 ? 1 : s_in.x;
        s_in.y = s_in.y < -1 ? -1 : s_in.y > 1 ? 1 : s_in.y;
    }
    // The pointer stays where the stylus left it, so a menu that opens while the pointer is over
    // it (the room's その他 panel) never closed, and buttons stayed highlighted. A Wii Remote put
    // down or pointed away has no pointer, and the room hides its menus: here the pointer leaves
    // the screen 3 s after the last touch or circle pad move (time to tap, then press A), and the
    // next one brings it back. A doesn't: what was under the pointer, coming back, isn't ready
    // for a press yet (the room bar slides in first). Scripted runs keep it on the screen.
    static u64 s_used;
    u64 now = host_time_us();
    if ((held & KEY_TOUCH) || nudged)
        s_used = now;
    s_in.pointing = now - s_used < 3000000 || input_script_active();
    // NOMA_INPUT in env.txt: scripted presses on top (unattended runs in an emulator); a scripted
    // P(x;y) moves the pointer once, and touching or the circle pad moves it on from there
    if (input_script_active())
        input_script_apply(s_in);
    wiimote_set_input(s_in);
}

// ---- video: the GPU renderer draws the top screen itself; with the software renderer
// (no GPU), the external frame buffer is scaled to the top screen here
static std::vector<u32> s_rgba;
static bool s_gpu;

// Closing from the HOME Menu (its close, or the power button's power-off): the game gets the
// Wii's power button, and ends the way it does there -- its fade, a save in progress finished,
// then the shutdown (STM, platform_quit). The 3DS has handed the GPU back by then. If the game
// isn't waiting for the button, or hasn't ended within 10 s, the app ends here.
static void closing() {
    static u64 s_since;
    if (!s_since) {
        s_since = host_time_us() | 1;
        if (!stm_power_event())
            platform_quit("closed from the HOME Menu");
    } else if (host_time_us() - s_since > 10000000) {
        platform_quit("closed from the HOME Menu (the game hadn't ended 10 s later)");
    }
}

void platform_present(u32 xfb) {
    NOMA_PROF_SCOPE("(present)");
    if (!aptMainLoop())
        closing();
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

// debugging switches (NOMA_DEBUG_NET=1, ...) as KEY=VALUE lines in env.txt next to the data, as
// long as they come (a NOMA_INPUT script for the whole setup is most of a KB). Read before the
// other static constructors: many switches are file-scope statics.
__attribute__((constructor(101))) static void read_env_txt() {
    std::vector<u8> text = read_file(std::string(guest::DATA_DIR) + "/env.txt");
    std::string all(text.begin(), text.end());
    for (size_t p = 0; p < all.size();) {
        size_t end = std::min(all.find('\n', p), all.size());
        std::string line = all.substr(p, end - p);
        p = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        size_t eq = line.find('=');
        if (!line.empty() && line[0] != '#' && eq != std::string::npos)
            setenv(line.substr(0, eq).c_str(), line.c_str() + eq + 1, 1);
    }
}

int main(int argc, char** argv) {
    guest::catch_arm_exceptions();
    std::set_new_handler(guest::on_out_of_memory);
    osSetSpeedupEnable(true);
    gfxInitDefault();
    gfxSetDoubleBuffering(GFX_TOP, true);
    consoleInit(GFX_BOTTOM, nullptr);
    guest::s_tick0 = svcGetSystemTick();
    std::string data = guest::DATA_DIR;
    guest::s_log = fopen((data + "/noma.log").c_str(), "w");
    if (guest::s_log)
        setvbuf(guest::s_log, nullptr, _IOFBF, 16 << 10);
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
    guest::memory_init(guest::MEM2_SIZE);
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
