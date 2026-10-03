// What runs on the DSP, high-level emulated:
//  * the 128-byte init program of __OSInitAudioSystem: posts one mail when started;
//  * the DSP ROM: after a reset it announces 0x8071FEED and takes a task description
//    (0x80F3xxxx command/value pairs) from the CPU, then starts the uploaded microcode;
//  * the microcode (AX, for sound): announces itself with 0xDCD10000. Its command lists are
//    handled in dsp_ax.cpp (not yet: mails are logged).
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

void dsp_hle_reset(bool init_code) {
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
void dsp_send_mail_later(u32 mail);

static void ax_mail(u32 mail) {
    if (s_want_cl_addr) {
        s_want_cl_addr = false;
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
