// IPC between the PowerPC and IOS (Starlet). The decompiled SDK writes a request's physical
// address to PPCMSG and sets X1 in PPCCTRL; IOS acknowledges (Y2), later replies with the
// address in ARMMSG (Y1). Both raise the Hollywood IPC interrupt when enabled. Requests are
// handled synchronously by the IOS emulation (ios/ios.cpp); replies queue until the PPC has
// taken the previous one.
//
// A reply is posted IPC_LATENCY_US after the request at the earliest (at a device poll): on the
// console IOS takes a while to answer, and the SDK's callers rely on it. NWC24's shutdown
// function sets its "request in flight" flag after IOS_IoctlAsync returns; a reply delivered
// inside IOS_IoctlAsync (at its OSRestoreInterrupts) ran the callback first, and the flag
// stayed set. (Counting polls instead of time made replies wait for a busy thread's next poll.)
#include <deque>

#include "hw.h"
#include "ios/ios.h"

namespace guest {

static u32 s_ppcmsg, s_armmsg;
static u32 s_ctrl;  // as the PPC sees it: X1 0x01, Y2 0x02, Y1 0x04, X2 0x08, IY1 0x10, IY2 0x20
static u32 s_irq_flag, s_irq_mask;
struct Reply {
    u32 request;
    u64 ready_us;  // host time from which it may be posted
};
static std::deque<Reply> s_replies;
static const u64 IPC_LATENCY_US = 30;

// X1: the PPC sent a request; Y2: IOS acknowledged it; Y1: IOS posted a reply in ARMMSG;
// X2: the PPC took the reply; IY1/IY2: interrupt on Y1/Y2
enum : u32 { X1 = 0x01, Y2 = 0x02, Y1 = 0x04, X2 = 0x08, IY1 = 0x10, IY2 = 0x20 };
static const u32 IRQ_IPC = 0x40000000;

static void update_irq() {
    bool want = ((s_ctrl & Y1) && (s_ctrl & IY1)) || ((s_ctrl & Y2) && (s_ctrl & IY2));
    if (want)
        s_irq_flag |= IRQ_IPC;
    if (s_irq_flag & s_irq_mask & IRQ_IPC)
        pi_raise(PI_INT_ACR);
    else
        pi_clear(PI_INT_ACR);
}

static void deliver_reply() {
    if ((s_ctrl & Y1) || s_replies.empty() || host_time_us() < s_replies.front().ready_us)
        return;
    s_armmsg = s_replies.front().request;
    s_replies.pop_front();
    s_ctrl |= Y1;
    update_irq();
}

void ipc_reply(u32 request_phys) {
    s_replies.push_back({request_phys, host_time_us() + IPC_LATENCY_US});
    deliver_reply();
}

// called from poll_devices
void ipc_tick() { deliver_reply(); }

// a reply is waiting for its turn (the idle loop polls again at once instead of sleeping)
bool ipc_reply_waiting() { return !s_replies.empty() && !(s_ctrl & Y1); }

void ipc_init() {
    s_ctrl = 0;
    s_irq_flag = 0;
    s_irq_mask = 0;
}

u32 ipc_reg_read(u32 off) {
    switch (off) {
    case 0x00: return s_ppcmsg;
    case 0x04: return s_ctrl;
    case 0x08: return s_armmsg;
    case 0x30: return s_irq_flag;
    case 0x34: return s_irq_mask;
    default: return 0;
    }
}

void ipc_reg_write(u32 off, u32 v) {
    static int n;
    if (n++ < 40)
        log("ipc: write %02X = %08X", off, v);
    switch (off) {
    case 0x00:
        s_ppcmsg = v;
        break;
    case 0x04: {
        if (v & Y1)
            s_ctrl &= ~Y1;  // reply taken
        if (v & Y2)
            s_ctrl &= ~Y2;  // ack taken
        s_ctrl = (s_ctrl & (Y1 | Y2)) | (v & (IY1 | IY2));
        if (v & X1) {
            u32 req = s_ppcmsg;
            s_ctrl |= Y2;  // acknowledge
            update_irq();
            ios_execute(req);
        }
        deliver_reply();
        update_irq();
        break;
    }
    case 0x08:
        s_armmsg = v;
        break;
    case 0x30:  // write 1 to clear
        s_irq_flag &= ~v;
        update_irq();
        break;
    case 0x34:
        s_irq_mask = v;
        update_irq();
        break;
    default:
        break;
    }
}

}  // namespace guest
