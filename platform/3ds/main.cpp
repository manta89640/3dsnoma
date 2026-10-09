// Nintendo 3DS build: runs the decompiled program on the 3DS. The game's data (main.dol,
// content/, nand/ as tools/stage_data.py makes them from the user's own WAD) lives on the SD card
// in sdmc:/3ds/noma.
//
// Controls (the game reads a Wii Remote, engine/input/remote.cpp): touch the bottom screen to press
// what is there, A B 1(X) 2(Y) + (START) - (SELECT) and the D-pad as on the remote, the circle pad
// as the D-pad (the island's and the Sound Player's stick). There is no pointer but the touch's.
// The remote's HOME button has no counterpart: the Wii's HOME Button Menu isn't part of the port
// (engine/system).
#include <3ds.h>
#include <citro3d.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>
#include <string>
#include <vector>

#include "g3d_pica.h"
#include "gx_pica.h"
#include "runtime/hw/gx/raster.h"
#include "engine/input/remote.h"
#include "runtime/hw/ios/ios.h"
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
static volatile bool s_quitting;  // platform_quit: the I/O thread, if it didn't stop, leaves the screen and the log alone

// The bottom screen: a status area of STATUS_ROWS rows at the top, drawn once a second (frames a
// second and where a frame's time goes, as the Jak 1 port shows them), and the log below it.
static PrintConsole s_status_con, s_log_con;
static const int STATUS_ROWS = 2;
static bool s_status_on;
// a native UI has the bottom screen (platform_bottom_claim): the console draws nothing there
static std::atomic<bool> s_bottom_claimed;
// NOMA_DEBUG_CONSOLE=1: the log and the status rows on the bottom screen while no scene has it.
// Otherwise the bottom screen is white then (the channel's ground; noma.log has every line), and
// only a fatal error is written there.
static bool s_console;
static void bottom_white();
static void status_update(u64 now);

static void log_flush_due(u64 now) {  // (with s_log_lock)
    if (s_log && s_log_dirty && now - s_log_flushed >= LOG_FLUSH_TICKS) {
        fflush(s_log);
        s_log_dirty = false;
        s_log_flushed = now;
    }
}

void platform_log_idle() {
    if (s_quitting)
        return;
    pica_watchdog();
    status_update(svcGetSystemTick());
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
    if (s_console && !s_bottom_claimed.load(std::memory_order_relaxed))
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

// ---- Miis: the channel's face library reads the Wii Mii database. It holds the 3DS's own Miis:
// at every start the system's Mii Maker database is read (as games' Mii library reads it) and its
// Miis go in, so the family's Miis are chosen from all of them, as the Wii's are from the Mii
// Channel's. Where it can't be read, the Mii the player picks in the 3DS Mii Selector goes in (on
// the first start, when SELECT is held while starting, or when the channel would send the player
// to the Mii Channel), with the Miis already there.
static std::string s_nand;

static bool write_mii_database(const std::string& path, const std::vector<u8>& db) {
    mkdir((s_nand + "/shared2").c_str(), 0777);
    mkdir((s_nand + "/shared2/menu").c_str(), 0777);
    mkdir((s_nand + "/shared2/menu/FaceLib").c_str(), 0777);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    bool ok = fwrite(db.data(), 1, db.size(), f) == db.size();
    fclose(f);
    return ok;
}

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
    if (!write_mii_database(path, db))
        return false;
    log("mii: the chosen Mii is in the database");
    return true;
}

// The 3DS's Miis as RFLCharData (0x4A bytes each): the Mii Maker's database, CFL_DB.dat in the
// shared extdata 0xF000000B: "CFOG", then 100 entries of 0x5C bytes (libctru's MiiData, as the
// Mii Selector returns one; Mii ID 0: an empty entry). None if it can't be read (an emulator has
// no database until its Mii Maker has run).
static std::vector<u8> read_3ds_miis() {
    std::vector<u8> out;
    static const u32 ID[3] = {MEDIATYPE_NAND, 0xF000000B, 0x00048000};
    const FS_Path where = {PATH_BINARY, sizeof(ID), ID};
    FS_Archive arch;
    Result rc = FSUSER_OpenArchive(&arch, ARCHIVE_SHARED_EXTDATA, where);
    if (R_FAILED(rc)) {
        log("mii: the 3DS's Mii database can't be opened (%08lX)", (unsigned long)rc);
        return out;
    }
    Handle f;
    rc = FSUSER_OpenFile(&f, arch, fsMakePath(PATH_ASCII, "/CFL_DB.dat"), FS_OPEN_READ, 0);
    if (R_SUCCEEDED(rc)) {
        std::vector<u8> raw(8 + 100 * 0x5C);
        u32 got = 0;
        rc = FSFILE_Read(f, &got, 0, raw.data(), (u32)raw.size());
        FSFILE_Close(f);
        if (R_FAILED(rc) || got != raw.size() || memcmp(raw.data(), "CFOG", 4) != 0) {
            log("mii: the 3DS's Mii database isn't one this version reads (%08lX, %lu bytes, \"%.4s\")",
                (unsigned long)rc, (unsigned long)got, (const char*)raw.data());
        } else {
            for (int i = 0; i < 100; i++) {
                const u8* m = &raw[8 + 0x5C * i];
                if (!(m[0x0C] | m[0x0D] | m[0x0E] | m[0x0F]))
                    continue;
                u8 w[0x4A];
                mii_3ds_to_wii(m, w);
                out.insert(out.end(), w, w + 0x4A);
            }
        }
    } else {
        log("mii: this 3DS has no Mii database (%08lX)", (unsigned long)rc);
    }
    FSUSER_CloseArchive(arch);
    return out;
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
    std::vector<u8> miis = read_3ds_miis();
    if (!miis.empty()) {  // (all the 3DS's Miis: the Mii Selector has nothing to add)
        const int n = (int)(miis.size() / 0x4A);
        std::vector<u8> synced = rfl_sync_miis(valid ? db : std::vector<u8>(), miis.data(), n);
        if (valid && synced == db)
            log("mii: the database has the 3DS's %d Miis", n);
        else if (write_mii_database(path, synced))
            log("mii: the 3DS's %d Miis are in the database", n);
        else
            log("mii: %s can't be written", path.c_str());
        return;
    }
    if (exists && !valid)
        log("mii: %s is not one this version writes: choose the Mii again", path.c_str());
    if (valid && !(hidKeysHeld() & KEY_SELECT))
        return;
    pick_mii(path, exists, valid, db);
}

// (the Homebrew Launcher's or the HOME Menu's: the app is launched again as it ends)
void platform_restart(const char* why) {
    aptSetChainloaderToSelf();
    platform_quit(why);
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

// back to the HOME Menu, as the Wii channel would go back to the Wii Menu. The GPU's thread, the
// I/O thread, the audio thread, the helper and the workers finish what they have in hand first (a
// worker: to its next pause point) and then sleep for good: what follows here (the GPU's and the
// sound's services closed, exit's destructors, libctru's services ended, the heap unmapped) went on
// under them, the GPU's thread still drawing the game's last frames (a crash as the app ended:
// erasing the save, which ends it at once after its fade). One that has crashed or hangs isn't
// waited for long.
static bool workers_park(u64 timeout_us);
static bool job_park(u64 timeout_us);
void platform_quit(const char* why) {
    log("quit: %s", why);
    const bool gpu = gx_park(500000), io = ios_async_park(500000), ax = dsp_ax_park(500000),
               job = job_park(500000), workers = workers_park(500000);
    if (!gpu || !io || !ax || !job || !workers)
        log("quit: these didn't stop:%s%s%s%s%s", gpu ? "" : " the GPU's thread", io ? "" : " the I/O thread",
            ax ? "" : " the audio thread", job ? "" : " the helper", workers ? "" : " a worker");
    s_quitting = true;
    // (each step logged as it starts: a quit that hangs says where)
    auto step = [](const char* what) {
        log("quit: %s", what);
        if (s_log)
            fflush(s_log);
    };
    if (s_audio)
        step("the sound"), ndspExit();
    // the graphics, unless the GPU's thread is still in them (waiting for a frame the GPU never
    // finished): the process's end frees them as well
    step(gpu ? "the graphics, then the app's end" : "the app's end (the graphics left as they are)");
    if (gpu)
        gfxExit();
    exit(0);
}

// The HOME Menu (and sleep) take the GPU while the app waits in aptMainLoop (on the game's thread):
// first the GPU's thread finishes what it was given and the frame it is drawing is closed, as for
// the keyboard (gx_end_frame_for_applet), so that no frame is sent half-made to a GPU that is no
// longer the app's. One was, after a HOME close on a New 3DS: the GPU's thread then waited for it
// for good (C3D_FrameBegin), the game's exit waited for the GPU's thread, and the quit hung. The
// hook goes in after citro3d's (C3D_Init's): libctru calls the hook registered last first, so this
// runs before citro3d waits for its queued frames.
static aptHookCookie s_suspend_cookie;
static void suspend_hook(APT_HookType type, void*) {
    if (type == APTHOOK_ONSUSPEND || type == APTHOOK_ONSLEEP) {
        log("apt: %s: the GPU's frame finished first", type == APTHOOK_ONSUSPEND ? "suspended" : "asleep");
        gx_end_frame_for_applet();
    }
}
void hook_suspend() { aptHook(&s_suspend_cookie, suspend_hook, nullptr); }

void platform_fatal(const char* msg) {
    s_bottom_claimed.store(false, std::memory_order_relaxed);  // (the error over whatever is there)
    s_console = true;
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

// ---- where the threads run
// The cores an application's threads may run on (libctru's threadCreate notes, 3dbrew; Luma3DS's
// process manager, sysmodules/pm/source/reslimit.c, for how the kernel shares the system core):
//   core 0, the application core: the main thread's;
//   core 1, the system core, where the system's modules run. An application gets time on it only
//     once it has asked for a share (APT_SetAppCpuTimeLimit, refused above its exheader's maximum;
//     with none, a thread can't even be made there). The CIA's exheader (cia/noma.rsf, MaxCpu 0x9E)
//     says the "multi" mode with at most 30%: the kernel then alternates the core between the
//     modules and the app's threads every 2 ms or less, so a thread there waits at most ~1.4 ms
//     for its turn. A .3dsx runs under another title's exheader, which may say the "single" mode
//     (one app thread on the core, in 25 ms slices: a wait of up to ~17 ms);
//   core 2 (New 3DS): all the application's (CanAccessCore2 in the exheader);
//   core 3 (New 3DS): the system's, never an application's.
// The New 3DS runs them at 804 MHz with its L2 cache (osSetSpeedupEnable, main()).
//
// The layout. On a New 3DS (real hardware: NOMA_CORES=auto, the default; anywhere: NOMA_CORES=multi):
//   core 0: the game (every guest thread); the I/O thread above it, which mostly waits for the SD
//           card;
//   core 1: the audio thread (the AX frames' mixing, runtime/hw/dsp_hle.cpp: a few percent of a core
//           at 804 MHz, but core 0's otherwise), with a 30% share of the core (a .3dsx: on core 2);
//   core 2: the GPU's thread; above it the helper's jobs (platform_job_start: the island's World,
//           short, taken back within the frame); below it the workers (a scene's load).
// In an emulator (Azahar, Citra: they answer svcGetSystemInfo 0x20000) the cores run one after
// another on one host thread, so a handoff between cores is a wait: there (and with
// NOMA_CORES=single) the frames are mixed inline and jobs run on the game's thread, as on the Old
// 3DS, whose system core has the GPU's thread instead. The GPU's thread stays on its core in every
// layout (it won Azahar's A/B as well). NOMA_AX_THREAD=0/1/2 puts the mixing inline, on core 1 or 2
// whatever the layout.
struct Cores {
    bool n3ds = false;
    bool old3ds_sim = false;  // a New 3DS run as an Old one (NOMA_OLD3DS)
    bool emulator = false;
    bool multi = false;  // the audio thread and the helper beside the GPU's thread
    int mhz = 0;         // the clock (Luma3DS says; 0: unknown)
    s32 prio = 0x30;     // the main thread's priority
};
static Cores s_cores;

static void cores_init() {
    APT_CheckNew3DS(&s_cores.n3ds);
    // NOMA_OLD3DS=1: the Old 3DS's layout (the GPU's thread on the system core, nothing on core 2)
    // and, on a New 3DS, its clock (268 MHz, no L2 cache: main() leaves the speed-up off), as near as
    // a New 3DS comes to an Old one; the input loses ZL, ZR and the C-stick (remote.cpp)
    s_cores.old3ds_sim = s_cores.n3ds && input_old3ds();
    if (s_cores.old3ds_sim)
        s_cores.n3ds = false;
    // the emulator's id in Citra (1) and Azahar (2); Luma3DS answers 0, a stock kernel an error
    s64 id = 0, mhz = 0;
    svcGetSystemInfo(&id, 0x20000, 0);
    s_cores.emulator = id > 0;
    if (R_SUCCEEDED(svcGetSystemInfo(&mhz, 0x10001, 0)) && mhz > 0 && mhz < 4000)
        s_cores.mhz = (int)mhz;  // (Luma3DS's New 3DS clock)
    svcGetThreadPriority(&s_cores.prio, CUR_THREAD_HANDLE);
    const char* e = getenv("NOMA_CORES");
    const bool single = e && !strcmp(e, "single"), multi = e && !strcmp(e, "multi");
    s_cores.multi = multi || (!single && s_cores.n3ds && !s_cores.emulator);
    char clock[24] = "";
    if (s_cores.mhz)
        snprintf(clock, sizeof(clock), " at %d MHz", s_cores.mhz);
    log("cores: %s 3DS%s%s%s, %s: %s", s_cores.n3ds ? "New" : "Old", clock,
        s_cores.old3ds_sim ? " (a New 3DS as an Old one: NOMA_OLD3DS, 268 MHz, no L2)" : "",
        s_cores.emulator ? " (an emulator)" : "", envIsHomebrew() ? ".3dsx" : "installed",
        s_cores.multi ? "the GPU's thread, the mixing and the jobs beside the game"
                      : "the GPU's thread beside the game; mixing and jobs on its core");
}

// The app's share of the system core, asked for once: `want`, else the most of 30, 25, 20, 15, 10
// that the exheader allows. 0: none (no thread can run there).
static u32 syscore_share(u32 want) {
    static int s_got = -1;
    if (s_got >= 0)
        return (u32)s_got;
    s_got = 0;
    for (u32 p : {(u32)want, (u32)30, (u32)25, (u32)20, (u32)15, (u32)10})
        if (p <= want && R_SUCCEEDED(APT_SetAppCpuTimeLimit(p))) {
            s_got = (int)p;
            break;
        }
    if (s_got)
        log("cores: %d%% of the system core for the app", s_got);
    else
        log("cores: no share of the system core for the app");
    return (u32)s_got;
}

// ---- the GPU's thread: on the New 3DS's third core (all the app's), else on the system core,
// just above the main thread's priority
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

// A thread of ours that runs native code which might name a profiler scope (NOMA_PROF_SCOPE) gets
// a stack of its own for it (not sampled): the guest's would be written from two threads.
static void own_profile_stack() {
#ifdef NOMA_PROFILE_CALLS
    t_prof = new ProfStack;
#endif
}

static void (*s_gpu_fn)();
bool platform_gpu_thread_start(void (*fn)()) {
    if (getenv("NOMA_GX_INLINE"))
        return false;
    const int core = s_cores.n3ds ? 2 : 1;
    if (core == 1 && !syscore_share(80))
        log("gx: no time on the system core for the GPU's thread");
    s_gpu_fn = fn;
    Thread t = threadCreate(
        [](void*) {
            catch_arm_exceptions();
            s_gpu_fn();
        },
        nullptr, 128 * 1024, s_cores.prio - 1, core, true);
    if (!t) {
        log("gx: no thread on core %d: commands decoded on the main thread", core);
        return false;
    }
    log("gx: the GPU's thread runs on core %d", core);
    return true;
}

// ---- the audio thread: where the layout puts it (above), above the main thread's priority (on
// core 2 above the GPU's thread and the helper too: a frame is short, and the game wants it by the
// next audio block, 3 ms on). Where the system core can't be had, core 2.
static void (*s_audio_fn)();
static std::atomic<int> s_audio_core{-1};  // where the thread found itself running
bool platform_audio_thread_start(void (*fn)()) {
    const char* e = getenv("NOMA_AX_THREAD");
    int core = e ? atoi(e) : !s_cores.multi ? 0 : envIsHomebrew() ? 2 : 1;
    if (!s_cores.n3ds || (core != 1 && core != 2))
        return false;
    if (core == 1 && !syscore_share(30))
        core = 2;
    s_audio_fn = fn;
    auto body = [](void*) {
        catch_arm_exceptions();
        s_audio_core.store(svcGetProcessorID(), std::memory_order_relaxed);
        log("ax: the frames are mixed on core %d", (int)svcGetProcessorID());
        s_audio_fn();
    };
    Thread t = threadCreate(body, nullptr, 64 * 1024, s_cores.prio - (core == 2 ? 3 : 2), core, true);
    if (!t && core == 1) {
        log("ax: no thread on the system core: on core 2");
        core = 2;
        t = threadCreate(body, nullptr, 64 * 1024, s_cores.prio - 3, core, true);
    }
    if (!t) {
        log("ax: no thread on core %d: the frames are mixed inline", core);
        return false;
    }
    return true;
}

// ---- the I/O thread (runtime/hw/ios/ios.cpp): on the main thread's core, above it; it mostly
// waits for the SD card in the FS service, and the guest runs meanwhile
static void (*s_io_fn)();
bool platform_io_thread_start(void (*fn)()) {
    if (getenv("NOMA_IO_INLINE"))
        return false;
    s_io_fn = fn;
    Thread t = threadCreate(
        [](void*) {
            catch_arm_exceptions();
            s_io_fn();
        },
        nullptr, 32 * 1024, s_cores.prio - 1, -2, true);
    if (!t) {
        log("ios: no I/O thread: reads are done at once");
        return false;
    }
    return true;
}

// ---- workers (platform_worker_start): below the main thread in priority, on the GPU thread's core
// (the New 3DS's third, else the system core), which is mostly idle while a scene loads under a
// black screen; on the main thread's core, in its waits, where that can't be had. Each has its own
// stack for the exception handler, freed with it. platform_quit stops them at their next pause
// point (workers_park).
struct Worker {
    void (*fn)(void*);
    void* arg;
    alignas(8) u8 exception_stack[16 << 10];
};
static std::atomic<int> s_workers{0}, s_workers_paused{0};  // running; asleep at a pause point for good
static std::atomic<bool> s_workers_park{false};

bool platform_worker_start(void (*fn)(void*), void* arg) {
    Worker* w = new Worker;
    w->fn = fn, w->arg = arg;
    auto body = [](void* p) {
        Worker* w = (Worker*)p;
        threadOnException(on_arm_exception, w->exception_stack + sizeof(w->exception_stack),
                          WRITE_DATA_TO_HANDLER_STACK);
        own_profile_stack();
        w->fn(w->arg);
        delete w;
        s_workers.fetch_sub(1, std::memory_order_release);
    };
    const int below = s_cores.prio < 0x3F ? s_cores.prio + 1 : s_cores.prio;
    s_workers.fetch_add(1, std::memory_order_acq_rel);
    Thread t = threadCreate(body, w, 64 * 1024, below, s_cores.n3ds ? 2 : 1, true);
    if (!t)
        t = threadCreate(body, w, 64 * 1024, below, -2, true);
    if (!t) {
        s_workers.fetch_sub(1, std::memory_order_release);
        delete w;
        return false;
    }
    return true;
}

void platform_worker_pause_point() {
    if (!s_workers_park.load(std::memory_order_acquire))
        return;
    s_workers_paused.fetch_add(1, std::memory_order_release);
    for (;;)
        svcSleepThread(1000000000000000000ll);
}

// every worker asleep at a pause point, or ended (false: not within timeout_us)
static bool workers_park(u64 timeout_us) {
    s_workers_park.store(true, std::memory_order_seq_cst);
    const u64 t0 = host_time_us();
    while (s_workers.load(std::memory_order_acquire) != s_workers_paused.load(std::memory_order_acquire)) {
        if (host_time_us() - t0 > timeout_us)
            return false;
        svcSleepThread(100000);
    }
    return true;
}

// ---- the helper (platform_job_start): one thread on the New 3DS's third core, above the GPU's
// thread (a job is short, and the game's thread will wait for it within the frame); made at the
// first job, only in the layout that has it
static HostSignal *s_job_go, *s_job_done;
static void (*s_job_fn)(void*);
static void* s_job_arg;
static int s_job_thread;  // 0: not made yet; 1: running; -1: none
static std::atomic<bool> s_job_busy{false}, s_job_park{false}, s_job_parked{false};

static void job_thread(void*) {
    catch_arm_exceptions();
    own_profile_stack();
    for (;;) {
        if (s_job_park.load(std::memory_order_acquire) && !s_job_busy.load(std::memory_order_acquire)) {
            s_job_parked.store(true, std::memory_order_release);
            for (;;)
                svcSleepThread(1000000000000000000ll);
        }
        platform_wait(s_job_go, 1000000);
        if (!s_job_busy.load(std::memory_order_acquire))
            continue;
        s_job_fn(s_job_arg);
        s_job_busy.store(false, std::memory_order_release);
        platform_signal(s_job_done);
    }
}

bool platform_job_start(void (*fn)(void*), void* arg) {
    if (s_job_thread == 0) {
        s_job_thread = -1;
        if (s_cores.multi && s_cores.n3ds && !getenv("NOMA_JOB_INLINE")) {
            s_job_go = platform_signal_new(), s_job_done = platform_signal_new();
            if (threadCreate(job_thread, nullptr, 64 * 1024, s_cores.prio - 2, 2, true))
                s_job_thread = 1;
            log("jobs: %s", s_job_thread > 0 ? "on core 2" : "no thread on core 2: on the game's thread");
        }
    }
    if (s_job_thread < 0 || s_job_park.load(std::memory_order_acquire))
        return false;
    platform_job_wait();
    s_job_fn = fn, s_job_arg = arg;
    s_job_busy.store(true, std::memory_order_release);
    platform_signal(s_job_go);
    return true;
}

void platform_job_wait() {
    if (!s_job_busy.load(std::memory_order_acquire))
        return;
    NOMA_PROF_SCOPE("(job wait)");
    while (s_job_busy.load(std::memory_order_acquire))
        platform_wait(s_job_done, 2000);
}

// the job in hand finished, then the helper asleep for good (false: not within timeout_us)
static bool job_park(u64 timeout_us) {
    if (s_job_thread <= 0)
        return true;
    s_job_park.store(true, std::memory_order_release);
    platform_signal(s_job_go);
    const u64 t0 = host_time_us();
    while (!s_job_parked.load(std::memory_order_acquire)) {
        if (host_time_us() - t0 > timeout_us)
            return false;
        svcSleepThread(100000);
    }
    return true;
}

const std::string& data_dir() {
    static const std::string s_dir = DATA_DIR;
    return s_dir;
}

bool platform_native_memory(u32* heap_free, u32* linear_free) {
    *heap_free = __ctru_heap_size - (u32)mallinfo().uordblks;
    *linear_free = (u32)linearSpaceFree();
    return true;
}

// ---- input: the 3DS controls as a Wii Remote
static WiimoteInput s_in;

// A stick (the circle pad, the C-stick: about 156 from the centre at the rim) as -1..1, y up: nothing
// within `dead` of the centre (a pad at rest isn't quite centred), and from there out to the rim
// the whole range, so a slight tilt is a slow walk rather than a jump to a fifth of full speed.
// A real circle pad doesn't come back to its centre exactly: let go after a push to the top left,
// it can rest 20 or so off it (with a dead zone of 20 the pointer crept on, and never left the
// screen), so it has 40; the C-stick 20. (The emulator's pads come back to 0.)
static const float CPAD_DEAD = 40.0f, CSTICK_DEAD = 20.0f;
static void read_stick(const circlePosition& c, float dead, float* x, float* y) {
    const float rim = 156.0f;
    float m = sqrtf((float)c.dx * c.dx + (float)c.dy * c.dy);
    if (m <= dead) {
        *x = *y = 0;
        return;
    }
    float s = std::min((m - dead) / (rim - dead), 1.0f) / m;
    *x = (float)c.dx * s, *y = (float)c.dy * s;
}

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
    // L, R and (New 3DS) ZL, ZR for native scenes: libctru's hidInit starts the New 3DS's irrst
    // service, and hidScanInput merges its ZL, ZR and C-stick into the keys
    u16 pad = 0;
    if (held & KEY_L) pad |= PAD_L;
    if (held & KEY_R) pad |= PAD_R;
    if (held & KEY_ZL) pad |= PAD_ZL;
    if (held & KEY_ZR) pad |= PAD_ZR;
    s_in.pad = pad;
    s_in.touching = (held & KEY_TOUCH) != 0;
    if (s_in.touching) {
        touchPosition t;
        hidTouchRead(&t);
        s_in.touch_x = t.px, s_in.touch_y = t.py;
        // (the pixel's middle)
        s_in.x = ((float)t.px + 0.5f) / 160.0f - 1.0f;
        s_in.y = ((float)t.py + 0.5f) / 120.0f - 1.0f;
    }
    // the circle pad: the stick in stick mode, else the D-pad (tilted more than half way): the
    // channel is worked by touch, with no Wii Remote pointer for the circle pad to move about (and no
    // hand on the screen); the D-pad moves the highlights of the menus and lists that have one
    circlePosition c;
    hidCircleRead(&c);
    float cx, cy;
    read_stick(c, CPAD_DEAD, &cx, &cy);
    if (input_stick_mode()) {
        s_in.stick_x = cx, s_in.stick_y = cy;
    } else {
        s_in.stick_x = s_in.stick_y = 0;
        s_in.buttons |= (cy > 0.5f ? WM_UP : 0) | (cy < -0.5f ? WM_DOWN : 0) | (cx < -0.5f ? WM_LEFT : 0) |
                        (cx > 0.5f ? WM_RIGHT : 0);
    }
    s_in.nudge_x = s_in.nudge_y = 0.0f;
    // the camera stick: the New 3DS's C-stick (an old 3DS reads it as centred)
    circlePosition cs;
    hidCstickRead(&cs);
    read_stick(cs, CSTICK_DEAD, &s_in.cam_x, &s_in.cam_y);
    // No pointer: the game's remote points only where a finger is (engine/input/remote.cpp
    // pointer_on), so nothing is highlighted once it lifts. Scripted runs have their own.
    s_in.pointing = input_script_active();
    // NOMA_INPUT in env.txt: scripted presses on top (unattended runs in an emulator); a scripted
    // P(x;y) moves the pointer once, and touching moves it on from there
    if (input_script_active())
        input_script_apply(s_in);
    wiimote_set_input(s_in);
}

// ---- video: the GPU renderer draws the top screen itself; with the software renderer
// (no GPU), the external frame buffer is scaled to the top screen here
static std::vector<u32> s_rgba;
static bool s_gpu;

// ---- the status area (see s_status_con), from the I/O thread
// a row of it, padded to the width and written without a newline, so that the console neither
// scrolls nor flushes the screen for it (with s_log_lock)
static void status_row(int y, const char* text) {
    char line[41];
    snprintf(line, sizeof(line), "%-40.40s", text);
    s_status_con.cursorX = 1, s_status_con.cursorY = y;
    fputs(line, stdout);
}

// Once a second: the game's frames a second (new pictures shown), then milliseconds a frame: the
// CPU's (the game's thread: the game and its sound) less its waits, the waits (every guest thread
// waiting: for the next retrace, the GPU, a read), the GPU's last frame; the heap in use.
// noma.log gets the same numbers every NOMA_STATUS_LOG seconds (10 by default; 0: never), over that
// time, so a run on the console leaves them behind ("status:" lines): a hardware run's frames a
// second and where their time went, with no one reading the screen.
static void status_log(u64 now, u32 frames, u32 idle, const AxLoad& ax) {
    static const u32 s_every = [] {
        const char* e = getenv("NOMA_STATUS_LOG");
        return e ? (u32)atoi(e) : 10u;
    }();
    static u64 s_t0, s_mix0, s_wait0;
    static u32 s_frames0, s_idle0;
    if (!s_every)
        return;
    if (s_t0 && now - s_t0 < (u64)s_every * SYSCLOCK_ARM11)
        return;
    if (s_t0) {
        const u32 n = frames - s_frames0;
        const double secs = (double)(now - s_t0) / SYSCLOCK_ARM11, ms = secs * 1000.0;
        const double waited = n ? (double)(u32)(idle - s_idle0) / 1000.0 / n : 0;
        const double mix = n ? (double)(ax.mix_us - s_mix0) / 1000.0 / n : 0;
        const double wait = n ? (double)(ax.wait_us - s_wait0) / 1000.0 / n : 0;
        const int core = ax.threaded ? s_audio_core.load(std::memory_order_relaxed) : 0;
        log("status: %.1f fps over %.0f s; ms a frame: CPU %.1f, waits %.1f, GPU %.1f (last frame); AX %.2f on c%d "
            "(game waited %.2f); heap %lu/%lu MB",
            n / secs, secs, n ? ms / n - waited : 0.0, waited, s_gpu ? (double)C3D_GetDrawingTime() : 0.0, mix, core,
            wait, (unsigned long)(mallinfo().uordblks >> 20), (unsigned long)(__ctru_heap_size >> 20));
    }
    s_t0 = now, s_frames0 = frames, s_idle0 = idle, s_mix0 = ax.mix_us, s_wait0 = ax.wait_us;
}

static void status_update(u64 now) {
    static u64 s_t0, s_mix0, s_wait0;
    static u32 s_frames0, s_idle0;
    if (!s_status_on || (s_t0 && now - s_t0 < SYSCLOCK_ARM11))
        return;
    const u32 frames = vi_game_frames(), idle = idle_time_us();
    if (!s_console || s_bottom_claimed.load(std::memory_order_relaxed)) {  // (the log still gets its lines)
        status_log(now, frames, idle, ax_load());
        return;
    }
    char row1[64] = "-- fps", row2[64];
    if (s_t0) {
        const u32 n = frames - s_frames0;
        const double ms = (double)(now - s_t0) * 1000.0 / SYSCLOCK_ARM11;
        const double waited = n ? (double)(u32)(idle - s_idle0) / 1000.0 / n : 0;
        if (n)
            snprintf(row1, sizeof(row1), "%4.1f fps  CPU %4.1f  idle %4.1f  GPU %4.1f", n * 1000.0 / ms,
                     ms / n - waited, waited, s_gpu ? (double)C3D_GetDrawingTime() : 0.0);
        else
            snprintf(row1, sizeof(row1), " 0.0 fps");
    }
    // Row 2 labels row 1's numbers, then the AX frames' mixing in ms a frame and the core it ran on
    // (a thread's: "w", how long the game waited for it), then the heap in use of its size in MB
    // (under malloc's lock; not the linear heap's free space: libctru walks its free list unlocked,
    // and the GPU's thread allocates textures from it meanwhile).
    const AxLoad ax = ax_load();
    status_log(now, frames, idle, ax);
    char axs[24];
    if (s_t0 && frames != s_frames0) {
        const u32 n = frames - s_frames0;
        const double mix = (double)(ax.mix_us - s_mix0) / 1000.0 / n, wait = (double)(ax.wait_us - s_wait0) / 1000.0 / n;
        const int core = ax.threaded ? s_audio_core.load(std::memory_order_relaxed) : 0;
        if (ax.threaded)
            snprintf(axs, sizeof(axs), "AX %.*f c%d w%.2f", mix < 10 ? 2 : 1, mix, core, wait);
        else
            snprintf(axs, sizeof(axs), "AX %.*f c0", mix < 10 ? 2 : 1, mix);
    } else {
        snprintf(axs, sizeof(axs), "AX --");
    }
    snprintf(row2, sizeof(row2), "ms a frame  %s  heap %lu/%lu", axs, (unsigned long)(mallinfo().uordblks >> 20),
             (unsigned long)(__ctru_heap_size >> 20));
    s_t0 = now, s_frames0 = frames, s_idle0 = idle, s_mix0 = ax.mix_us, s_wait0 = ax.wait_us;
    LightLock_Lock(&s_log_lock);
    PrintConsole* prev = consoleSelect(&s_status_con);
    fputs("\x1b[36m", stdout);  // (cyan)
    status_row(1, row1);
    status_row(2, row2);
    fputs("\x1b[0m", stdout);
    consoleSelect(prev);
    LightLock_Unlock(&s_log_lock);
    u16 w = 0, h = 0;
    u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);
    GSPGPU_FlushDataCache(fb, (u32)w * h * 2);  // (the console draws with the CPU: RGB565)
}

// the bottom screen all white (the console's frame buffer, RGB565)
static void bottom_white() {
    u16 w = 0, h = 0;
    u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);
    memset(fb, 0xFF, (size_t)w * h * 2);
    GSPGPU_FlushDataCache(fb, (u32)w * h * 2);
}

// NOMA_SHOT_EVERY (gx_pica.cpp's save_shot, after the top screen's NNNNN.bmp): the bottom screen as
// NNNNN_bottom.bmp, 320x240 (the console's frame buffer: RGB565, 240x320, column-major, rows bottom
// up). A picture of it as it is: the console writes it from other threads meanwhile.
void platform_save_bottom_shot(u32 n) {
    u16 fw = 0, fh = 0;
    const u16* fb = (const u16*)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &fw, &fh);
    if (!fb || fw != 240 || fh != 320)
        return;
    char path[64];
    snprintf(path, sizeof(path), "sdmc:/3ds/noma/shots/%05u_bottom.bmp", (unsigned)n);
    FILE* f = fopen(path, "wb");
    if (!f)
        return;
    const u32 SW = 320, SH = 240, row = SW * 3, size = 54 + row * SH;
    u8 hdr[54] = {'B', 'M'};
    auto put32 = [&](int at, u32 v) { memcpy(hdr + at, &v, 4); };
    put32(2, size);
    put32(10, 54);
    put32(14, 40);
    put32(18, SW);
    put32(22, SH);  // bottom-up rows
    hdr[26] = 1;
    hdr[28] = 24;
    put32(34, row * SH);
    fwrite(hdr, 1, 54, f);
    std::vector<u8> line(row);
    for (u32 y = SH; y-- > 0;) {  // BMP rows from the bottom; screen row y is column index 239 - y
        for (u32 x = 0; x < SW; x++) {
            const u16 p = fb[x * 240 + (239 - y)];
            const u8 r = (u8)((p >> 11) << 3), g = (u8)(((p >> 5) & 0x3F) << 2), b = (u8)((p & 0x1F) << 3);
            line[x * 3 + 0] = b | b >> 5, line[x * 3 + 1] = g | g >> 6, line[x * 3 + 2] = r | r >> 5;
        }
        fwrite(line.data(), 1, row, f);
    }
    fclose(f);
}

// The bottom screen for a native scene (engine/ui/screen.h). Claimed, the console stops drawing
// there: the GPU's frames go onto it (gx_pica.cpp's bottom render target, into the console's own
// RGB565 frame buffer). Released, the screen is cleared for the console, which carries on below.
void platform_bottom_claim(bool on) {
    LightLock_Lock(&s_log_lock);
    s_bottom_claimed.store(on, std::memory_order_relaxed);
    if (!on && !s_console) {
        bottom_white();
    } else if (!on) {
        PrintConsole* prev = consoleSelect(&s_status_con);
        consoleClear();
        consoleSelect(&s_log_con);
        consoleClear();
        if (prev != &s_status_con)
            consoleSelect(prev);
    }
    LightLock_Unlock(&s_log_lock);
}

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
    // (NOMA_OLD3DS=1 keeps a New 3DS at the Old 3DS's 268 MHz without the L2 cache; env.txt's
    // switches are set by now: constructor priority 101)
    osSetSpeedupEnable(!guest::input_old3ds());
    gfxInitDefault();
    gfxSetDoubleBuffering(GFX_TOP, true);
    // the bottom screen: the status area over the log (40 x 30 characters)
    consoleInit(GFX_BOTTOM, &guest::s_status_con);
    consoleInit(GFX_BOTTOM, &guest::s_log_con);
    consoleSetWindow(&guest::s_status_con, 1, 1, 40, guest::STATUS_ROWS);
    consoleSetWindow(&guest::s_log_con, 1, guest::STATUS_ROWS + 1, 40, 30 - guest::STATUS_ROWS);
    consoleSelect(&guest::s_log_con);
    guest::s_status_on = true;
    if (const char* e = getenv("NOMA_DEBUG_CONSOLE"))
        guest::s_console = *e && strcmp(e, "0");
    if (!guest::s_console)
        guest::bottom_white();
    guest::s_tick0 = svcGetSystemTick();
    std::string data = guest::DATA_DIR;
    // the last run's log kept (a crash's, or the run before the app started itself again: erasing
    // the save, choosing a Mii)
    remove((data + "/noma.prev.log").c_str());
    rename((data + "/noma.log").c_str(), (data + "/noma.prev.log").c_str());
    guest::s_log = fopen((data + "/noma.log").c_str(), "w");
    if (guest::s_log)
        setvbuf(guest::s_log, nullptr, _IOFBF, 16 << 10);
    if (guest::s_console)
        printf("Wii no Ma 3DS\n");
    guest::cores_init();
    std::vector<u8> dol = read_file(data + "/main.dol");
    if (dol.empty()) {
        printf("no %s/main.dol\n(see README: tools/stage_data.py)\n", data.c_str());
        guest::platform_fatal("missing game data");
    }
    guest::mii_setup(data + "/nand");
    guest::memory_init(guest::MEM2_SIZE);
    guest::log("memory: heap %u KB, linear heap %u KB (%u KB free after guest memory)", __ctru_heap_size >> 10,
               __ctru_linear_heap_size >> 10, (u32)(linearSpaceFree() >> 10));
    guest::audio_init();
    if (!guest::load_dol(dol))
        guest::platform_fatal("bad main.dol");
    std::vector<u8>().swap(dol);  // (in guest memory now: its 8.9 MB back to the heap for the session)
    guest::setup_low_memory(0x00010001, 0x4843494A);  // HCIJ
    guest::ios_init(data + "/nand", data + "/content");
    guest::hw_init();
    static guest::PicaRenderer pica;
    static guest::SoftRenderer soft;
    if (pica.init()) {
        guest::s_gpu = true;
        guest::gx_set_renderer(&pica);
        guest::hook_suspend();
        // the native g3d draws into the GPU renderer's frame (not with the software renderer, whose
        // native draws have no backend: their packets are only retired)
        if (engine::g3d::Backend* g3d = guest::g3d_pica_backend())
            engine::g3d::native_draw_set_backend(g3d);
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
