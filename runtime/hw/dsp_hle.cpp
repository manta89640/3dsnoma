// What runs on the DSP, high-level emulated:
//  * the 128-byte init program of __OSInitAudioSystem: posts one mail when started;
//  * the DSP ROM: after a reset it announces 0x8071FEED and takes a task description
//    (0x80F3xxxx command/value pairs) from the CPU, then starts the uploaded microcode;
//  * the microcode (AX, for sound): announces itself with 0xDCD10000. Its command lists are
//    handled in dsp_ax.cpp.
#include <atomic>
#include <cstdlib>
#include <cstring>

#include "hw.h"

namespace guest {

void dsp_send_mail(u32 mail, bool interrupt);
bool dsp_mail_pending_to_cpu();

enum class DspState { Off, InitCode, Rom, Ucode };
static u32 s_cl_size;
static bool s_want_cl_addr;
static DspState s_state = DspState::Off;
static bool s_started;
static u32 s_pending_cmd;
static u32 s_task[8];  // iram mmem, iram addr, iram len, dram len, init vector...

void dsp_ax_wait();

void dsp_hle_reset(bool init_code) {
    dsp_ax_wait();
    log("dsp: reset (%s)", init_code ? "init code" : "rom");
    s_want_cl_addr = false;
    s_state = init_code ? DspState::InitCode : DspState::Rom;
    s_started = false;
    s_pending_cmd = 0;
}

// called when the DSP is un-halted or the CPU polls its mailbox
void dsp_hle_run() {
    if (s_started || dsp_mail_pending_to_cpu())
        return;
    switch (s_state) {
    case DspState::InitCode:
        s_started = true;
        dsp_send_mail(0x8071FEED, false);
        break;
    case DspState::Rom:
        s_started = true;
        dsp_send_mail(0x8071FEED, false);  // ready for a task
        break;
    default:
        break;
    }
}

// The AX microcode: 0xBABE0000 | size announces a command list, whose address follows; when it
// has run, the DSP yields (DSP_YIELD) and the CPU answers with a task mail 0xCDD1xxxx: resume
// (answered with DSP_RESUME), continue (nothing to do), new microcode, reset.
void dsp_ax_command_list(u32 list_addr, u32 words);
void dsp_send_mail_later(u32 mail, u64 delay_us = 30);

// The frames may be mixed on a thread of their own (platform_audio_thread_start: on the New 3DS the
// system core, or the third), as the Wii's DSP mixes beside its CPU: the command list goes to that
// thread, and the DSP yields once the frame is mixed. Guest code only reaches what a frame reads and
// writes (the voices' PBs, the aux and output buffers) at the next AI block or with its next mail,
// and both wait for the frame (dsp_ax_wait).
static HostSignal* s_ax_go;    // a frame to mix
static HostSignal* s_ax_idle;  // a frame mixed
static bool s_ax_thread_up;    // the thread runs (it waits for frames, or is parked)
static std::atomic<bool> s_ax_threaded{false};  // the frames go to it (else they are mixed inline)
static u32 s_ax_list, s_ax_words;      // the frame to mix, set before s_ax_busy
enum { AX_IDLE, AX_QUEUED, AX_MIXING };
static std::atomic<int> s_ax_busy{AX_IDLE};  // a frame for the thread (queued: not taken yet)
static std::atomic<int> s_ax_done{0};        // a frame is mixed and its yield not sent yet
static std::atomic<bool> s_ax_park{false}, s_ax_parked{false};  // (dsp_ax_park)

// What the mixing costs (ax_load: the 3DS's status line): its time, wherever it ran, and the
// guest's waits for the thread, in microseconds since the start.
static std::atomic<u64> s_mix_us{0}, s_wait_us{0};

static void mix(u32 list, u32 words) {
    const u64 t0 = host_time_us();
    dsp_ax_command_list(list, words);
    s_mix_us.fetch_add(host_time_us() - t0, std::memory_order_relaxed);
}

static void ax_thread() {
    profile_audio_thread();
    for (;;) {
        // (dsp_ax_park: a frame in hand is mixed first)
        if (s_ax_park.load(std::memory_order_acquire) && !s_ax_busy.load(std::memory_order_acquire)) {
            s_ax_parked.store(true, std::memory_order_release);
            for (;;)
                host_sleep_us(1000000000ull);
        }
        platform_wait(s_ax_go, 1000000);
        int queued = AX_QUEUED;
        if (!s_ax_busy.compare_exchange_strong(queued, AX_MIXING, std::memory_order_acq_rel))
            continue;  // (none, or dsp_ax_wait took it back)
        mix(s_ax_list, s_ax_words);
        s_ax_done.store(1, std::memory_order_relaxed);
        s_ax_busy.store(AX_IDLE, std::memory_order_release);
        platform_signal(s_ax_idle);
    }
}

AxLoad ax_load() {
    return {s_mix_us.load(std::memory_order_relaxed), s_wait_us.load(std::memory_order_relaxed),
            s_ax_threaded.load(std::memory_order_relaxed)};
}

// The app ends: the frame being mixed is finished, then the thread sleeps for good, touching nothing
// (what follows closes the sound service and runs exit's destructors). False if it hadn't within
// timeout_us.
bool dsp_ax_park(u64 timeout_us) {
    if (!s_ax_thread_up)
        return true;
    s_ax_park.store(true, std::memory_order_release);
    platform_signal(s_ax_go);
    const u64 t0 = host_time_us();
    while (!s_ax_parked.load(std::memory_order_acquire)) {
        if (host_time_us() - t0 > timeout_us)
            return false;
        host_sleep_us(100);
    }
    return true;
}

// a mixed frame's yield (from dsp_tick, every poll): at once, the mixing took the DSP's time
void dsp_ax_poll() {
    if (!s_ax_busy.load(std::memory_order_acquire) && s_ax_done.exchange(0, std::memory_order_acq_rel))
        dsp_send_mail_later(0xDCD10002, 0);  // DSP_YIELD: the frame is mixed
}

// The frame being mixed, finished (asleep meanwhile: on the 3DS a spinning wait would take the
// time from the game's other threads). A frame the thread hasn't even taken within TAKE_BACK_US
// (a core that doesn't run it: no share of the system core, its time slices) is taken back and
// mixed here, and the thread given up unless it was asked for (asked_for_thread): the game
// would otherwise wait for it without end.
static const u64 TAKE_BACK_US = 20000;
static bool asked_for_thread();
void dsp_ax_wait() {
    if (s_ax_busy.load(std::memory_order_acquire)) {
        NOMA_PROF_SCOPE("(ax wait)");
        const u64 t0 = host_time_us();
        u64 t1 = 0;
        while (s_ax_busy.load(std::memory_order_acquire)) {
            platform_wait(s_ax_idle, 2000);
            int queued = AX_QUEUED;
            if (host_time_us() - t0 > TAKE_BACK_US &&
                s_ax_busy.compare_exchange_strong(queued, AX_IDLE, std::memory_order_acq_rel)) {
                t1 = host_time_us();
                if (!asked_for_thread())
                    s_ax_threaded = false;
                log("ax: the audio thread hadn't taken its frame %u ms later: mixed here%s", (u32)((t1 - t0) / 1000),
                    s_ax_threaded ? "" : ", and so are the next ones");
                mix(s_ax_list, s_ax_words);
                s_ax_done.store(1, std::memory_order_relaxed);
                break;
            }
        }
        s_wait_us.fetch_add((t1 ? t1 : host_time_us()) - t0, std::memory_order_relaxed);
    }
    dsp_ax_poll();
}

// The thread is given up (the frames are mixed inline again) when, over CHECK_FRAMES frames, the
// game waited for it longer than the mixing took: a core that can't run it when a frame comes (the
// system core's time slices, platform/3ds/main.cpp) costs the game more than mixing inline would.
// In steady play the thread has finished before the game wants the frame, and the game doesn't wait.
// Not when the thread was asked for (NOMA_AX_THREAD, NOMA_CORES=multi).
static const u32 CHECK_FRAMES = 2000;  // (6 s of 3 ms frames)
static bool asked_for_thread() {
    static const bool s_asked = getenv("NOMA_AX_THREAD") ||
                                (getenv("NOMA_CORES") && !strcmp(getenv("NOMA_CORES"), "multi"));
    return s_asked;
}
static void check_thread() {
    static u32 s_frames;
    static u64 s_mix0, s_wait0;
    if (++s_frames < CHECK_FRAMES)
        return;
    s_frames = 0;
    const u64 m = s_mix_us.load(std::memory_order_relaxed), w = s_wait_us.load(std::memory_order_relaxed);
    const u64 mixed = m - s_mix0, waited = w - s_wait0;
    s_mix0 = m, s_wait0 = w;
    if (waited > mixed && waited > 20000 && !asked_for_thread()) {
        s_ax_threaded = false;
        log("ax: the game waited %u ms for the audio thread in the last %u frames, which took %u ms to mix: "
            "the frames are mixed inline from now on",
            (u32)(waited / 1000), CHECK_FRAMES, (u32)(mixed / 1000));
    }
}

static void ax_mail(u32 mail) {
    dsp_ax_wait();
    if (s_want_cl_addr) {
        s_want_cl_addr = false;
        if (s_ax_threaded)
            check_thread();
        if (s_ax_threaded) {
            s_ax_list = mail, s_ax_words = s_cl_size;
            s_ax_busy.store(AX_QUEUED, std::memory_order_release);
            platform_signal(s_ax_go);
            return;
        }
        mix(mail, s_cl_size);
        dsp_send_mail_later(0xDCD10002);  // DSP_YIELD: the frame is mixed
        return;
    }
    if ((mail & 0xFFFF0000u) == 0xBABE0000u) {
        s_cl_size = mail & 0xFFFF;
        s_want_cl_addr = true;
        return;
    }
    switch (mail) {
    case 0xCDD10000:  // resume the task: acknowledge
        dsp_send_mail_later(0xDCD10001);
        break;
    case 0xCDD10003:  // continue: the next command list follows
        break;
    case 0xCDD10002:  // reset: back to the ROM
        dsp_hle_reset(false);
        break;
    default: {
        static int n;
        if (n++ < 20)
            log("dsp: ax: unhandled mail %08X", mail);
        break;
    }
    }
}

void dsp_hle_mail_from_cpu(u32 mail) {
    static int s_logged;
    if (s_logged++ < 30)
        log("dsp: mail from cpu %08X (state %d)", mail, (int)s_state);
    if (s_state == DspState::Rom) {
        if (!s_pending_cmd && (mail & 0xFFFF0000u) == 0x80F30000u) {
            s_pending_cmd = mail;
            return;
        }
        u32 cmd = s_pending_cmd;
        s_pending_cmd = 0;
        switch (cmd) {
        case 0x80F3A001: s_task[0] = mail & 0x7FFFFFFF; break;  // IRAM image in main memory
        case 0x80F3C002: s_task[1] = mail & 0xFFFF; break;      // IRAM address
        case 0x80F3A002: s_task[2] = mail & 0xFFFF; break;      // IRAM length
        case 0x80F3B002: s_task[3] = mail & 0xFFFF; break;      // DRAM length
        case 0x80F3D001:                                        // start at the init vector
            s_task[4] = mail & 0xFFFF;
            log("dsp: microcode started (image %08X, %u bytes, entry %04X)", s_task[0], s_task[2],
                s_task[4]);
            s_state = DspState::Ucode;
            if (!s_ax_go) {
                s_ax_go = platform_signal_new();
                s_ax_idle = platform_signal_new();
                s_ax_thread_up = platform_audio_thread_start(ax_thread);
                s_ax_threaded = s_ax_thread_up;
            }
            dsp_send_mail(0xDCD10000, true);
            break;
        default:
            log("dsp: rom: unknown command %08X %08X", cmd, mail);
            break;
        }
        return;
    }
    if (s_state == DspState::Ucode)
        ax_mail(mail);
}

}  // namespace guest
