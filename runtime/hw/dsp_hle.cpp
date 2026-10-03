// What runs on the DSP, high-level emulated:
//  * the 128-byte init program of __OSInitAudioSystem: posts one mail when started;
//  * the DSP ROM: after a reset it announces 0x8071FEED and takes a task description
//    (0x80F3xxxx command/value pairs) from the CPU, then starts the uploaded microcode;
//  * the microcode (AX, for sound): announces itself with 0xDCD10000. Its command lists are
//    handled in dsp_ax.cpp.
#include <atomic>

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

// The frames may be mixed on a thread of their own (platform_audio_thread_start: the New 3DS's
// system core), as the Wii's DSP mixes beside its CPU: the command list goes to that thread, and
// the DSP yields once the frame is mixed. Guest code only reaches what a frame reads and writes
// (the voices' PBs, the aux and output buffers) at the next AI block or with its next mail, and
// both wait for the frame (dsp_ax_wait).
static HostSignal* s_ax_go;    // a frame to mix
static HostSignal* s_ax_idle;  // a frame mixed
static bool s_ax_threaded;
static u32 s_ax_list, s_ax_words;      // the frame to mix, set before s_ax_busy
static std::atomic<int> s_ax_busy{0};  // the thread is mixing a frame
static std::atomic<int> s_ax_done{0};  // a frame is mixed and its yield not sent yet

static void ax_thread() {
    profile_audio_thread();
    for (;;) {
        platform_wait(s_ax_go, 1000000);
        if (!s_ax_busy.load(std::memory_order_acquire))
            continue;
        dsp_ax_command_list(s_ax_list, s_ax_words);
        s_ax_done.store(1, std::memory_order_relaxed);
        s_ax_busy.store(0, std::memory_order_release);
        platform_signal(s_ax_idle);
    }
}

// a mixed frame's yield (from dsp_tick, every poll): at once, the mixing took the DSP's time
void dsp_ax_poll() {
    if (!s_ax_busy.load(std::memory_order_acquire) && s_ax_done.exchange(0, std::memory_order_acq_rel))
        dsp_send_mail_later(0xDCD10002, 0);  // DSP_YIELD: the frame is mixed
}

// the frame being mixed, finished (asleep meanwhile: on the 3DS a spinning wait would take the
// time from the game's other threads)
void dsp_ax_wait() {
    if (s_ax_busy.load(std::memory_order_acquire)) {
        NOMA_PROF_SCOPE("(ax wait)");
        while (s_ax_busy.load(std::memory_order_acquire))
            platform_wait(s_ax_idle, 2000);
    }
    dsp_ax_poll();
}

static void ax_mail(u32 mail) {
    dsp_ax_wait();
    if (s_want_cl_addr) {
        s_want_cl_addr = false;
        if (s_ax_threaded) {
            s_ax_list = mail, s_ax_words = s_cl_size;
            s_ax_busy.store(1, std::memory_order_release);
            platform_signal(s_ax_go);
            return;
        }
        dsp_ax_command_list(mail, s_cl_size);
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
                s_ax_threaded = platform_audio_thread_start(ax_thread);
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
