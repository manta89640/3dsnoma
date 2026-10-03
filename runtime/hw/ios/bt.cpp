// /dev/usb/oh1/57e/305: the Wii's Bluetooth controller (a Broadcom BCM2045 on USB), with one
// Wii Remote attached. The SDK's Bluetooth stack (BTE), WUD, WPAD and KPAD all run decompiled
// on top of this; we answer at the controller's boundary:
//   control transfers  HCI commands (answered with Command Complete/Status events)
//   interrupt in 0x81  HCI events
//   bulk out 0x02      ACL data to the remote: L2CAP signalling and HID output reports
//   bulk in 0x82       ACL data from the remote: L2CAP and HID input reports
// Multi-byte HCI/L2CAP fields are little-endian. The remote connects as a paired one would:
// it pages the console once page scan is on, then opens the HID control (PSM 0x11) and
// interrupt (PSM 0x13) channels; WPAD then talks to it with HID reports (see wiimote.cpp).
#include <cstring>
#include <deque>
#include <vector>

#include "bt.h"
#include "ios.h"

namespace guest {

namespace {

const u8 CONTROLLER_BDADDR[6] = {0x11, 0x02, 0x19, 0x79, 0x00, 0xFF};
const u16 WIIMOTE_HANDLE = 0x100;
const u16 ACL_MTU = 339;
const u16 ACL_PACKETS = 10;

u16 rd16(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }
void wr16(std::vector<u8>& v, u16 x) {
    v.push_back((u8)x);
    v.push_back((u8)(x >> 8));
}

enum L2capState { L2_CLOSED, L2_WAIT_CONNECT_RSP, L2_OPEN };

struct Channel {
    u16 psm = 0;
    u16 local_cid = 0;   // the remote's end
    u16 remote_cid = 0;  // the console's end
    L2capState state = L2_CLOSED;
    bool we_configured = false;    // our config request was accepted
    bool they_configured = false;  // we accepted the console's config request
    bool config_sent = false;
    bool complete() const { return state == L2_OPEN && we_configured && they_configured; }
};

class BtController : public IosHandle {
public:
    BtController() { s_instance = this; }
    ~BtController() override {
        if (s_instance == this)
            s_instance = nullptr;
    }

    static BtController* s_instance;

    s32 ioctlv(u32 cmd, const std::vector<IoVec>& in, const std::vector<IoVec>& io) override {
        keep_ = false;
        static int s_n;
        if (s_n++ < 40) {
            char b[256];
            int k = snprintf(b, sizeof(b), "bt: ioctlv %u in", cmd);
            for (auto& v : in)
                k += snprintf(b + k, sizeof(b) - k, " %08X/%u=%02X", v.addr, v.size, v.size ? load8(v.addr) : 0);
            k += snprintf(b + k, sizeof(b) - k, " io");
            for (auto& v : io)
                k += snprintf(b + k, sizeof(b) - k, " %08X/%u", v.addr, v.size);
            log("%s", b);
        }
        switch (cmd) {
        case 0: {  // control message: an HCI command
            if (in.size() < 5 || io.empty())
                return IOS_EINVAL;
            u16 len = (u16)(load8(in[4].addr) | (load8(in[4].addr + 1) << 8));
            std::vector<u8> pkt(len);
            read_g(pkt.data(), io[0].addr, len);
            hci_command(pkt);
            return len;
        }
        case 1: {  // bulk message: ACL data
            if (in.size() < 2 || io.empty())
                return IOS_EINVAL;
            u8 ep = load8(in[0].addr);
            u16 len = load16(in[1].addr);
            if (ep == 0x02) {
                std::vector<u8> pkt(len);
                read_g(pkt.data(), io[0].addr, len);
                acl_out(pkt);
                return len;
            }
            acl_in_.push_back({cur_request, io[0].addr, io[0].size});
            keep_ = true;
            return 0;
        }
        case 2: {  // interrupt message: room for an HCI event
            if (in.size() < 2 || io.empty())
                return IOS_EINVAL;
            evt_in_.push_back({cur_request, io[0].addr, io[0].size});
            keep_ = true;
            return 0;
        }
        default:
            return IosHandle::ioctlv(cmd, in, io);
        }
    }

    bool reply_now() override { return !keep_; }

    // ---- periodic work (from poll_devices)
    void tick(u64 now_us) {
        if (!remote_connected_ && !connect_requested_ && (scan_enable_ & 2) && now_us > 300000) {
            // the remote pages the console (someone pressed a button on it)
            std::vector<u8> e(WIIMOTE_BDADDR, WIIMOTE_BDADDR + 6);
            e.push_back(0x04);  // class of device: peripheral, joystick
            e.push_back(0x25);
            e.push_back(0x00);
            e.push_back(0x01);  // link type: ACL
            event(0x04, e);
            connect_requested_ = true;
            log("bt: Wii Remote requests a connection");
        }
        if (remote_connected_)
            l2cap_progress();
        if (hid_ready() && now_us - last_report_us_ >= 10000) {
            last_report_us_ = now_us;
            std::vector<u8> rep;
            if (wiimote_poll_report(rep))
                send_hid(rep);
        }
        flush();
    }

    // ---- HID reports from the remote (input reports, without the 0xA1 header)
    void send_hid(const std::vector<u8>& report) {
        if (!hid_ready())
            return;
        std::vector<u8> p;
        p.push_back(0xA1);
        p.insert(p.end(), report.begin(), report.end());
        l2cap_send(intr_.remote_cid, p);
    }

private:
    struct Pending {
        u32 request;
        u32 buf;
        u32 size;
    };
    std::deque<Pending> evt_in_, acl_in_;
    std::deque<std::vector<u8>> events_, acl_;
    bool keep_ = false;
    u8 scan_enable_ = 0;
    bool connect_requested_ = false, remote_connected_ = false;
    Channel ctrl_{0x11, 0x40}, intr_{0x13, 0x41};
    u8 next_ident_ = 1;
    u64 last_report_us_ = 0;
    bool hold_channels_ = false;  // the console closed our channels: don't reopen them
    bool auth_pending_ = false;   // waiting for the console's Link Key Request Reply

    bool hid_ready() const { return remote_connected_ && ctrl_.complete() && intr_.complete(); }

    // ---- delivery to the pending transfers (only from tick(), so that a command's own reply
    // reaches the console before the events it causes)
    void flush() {
        while (!events_.empty() && !evt_in_.empty()) {
            Pending p = evt_in_.front();
            evt_in_.pop_front();
            const std::vector<u8>& e = events_.front();
            u32 n = (u32)e.size() < p.size ? (u32)e.size() : p.size;
            write_g(p.buf, e.data(), n);
            static int s_ev;
            if (s_ev++ < 60) {
                char b[128];
                int k = 0;
                for (u32 i = 0; i < n && i < 24; i++)
                    k += snprintf(b + k, sizeof(b) - k, "%02X ", e[i]);
                log("bt: event -> %08X: %s", p.request, b);
            }
            events_.pop_front();
            ios_complete(p.request, (s32)n);
        }
        // ACL only once the events before it went out (connection complete before data)
        while (events_.empty() && !acl_.empty() && !acl_in_.empty()) {
            Pending p = acl_in_.front();
            acl_in_.pop_front();
            const std::vector<u8>& a = acl_.front();
            u32 n = (u32)a.size() < p.size ? (u32)a.size() : p.size;
            write_g(p.buf, a.data(), n);
            static int s_acl;
            if (s_acl++ < 80) {
                char b[128];
                int k = 0;
                for (u32 i = 0; i < n && i < 24; i++)
                    k += snprintf(b + k, sizeof(b) - k, "%02X ", a[i]);
                log("bt: ACL -> console: %s", b);
            }
            acl_.pop_front();
            ios_complete(p.request, (s32)n);
        }
    }

    void event(u8 code, const std::vector<u8>& params) {
        std::vector<u8> e;
        e.push_back(code);
        e.push_back((u8)params.size());
        e.insert(e.end(), params.begin(), params.end());
        events_.push_back(e);
    }

    void command_complete(u16 opcode, const std::vector<u8>& ret) {
        std::vector<u8> p;
        p.push_back(1);  // command packets the host may send
        wr16(p, opcode);
        p.insert(p.end(), ret.begin(), ret.end());
        event(0x0E, p);
    }

    void command_status(u16 opcode, u8 status = 0) {
        std::vector<u8> p;
        p.push_back(status);
        p.push_back(1);
        wr16(p, opcode);
        event(0x0F, p);
    }

    void connection_complete() {
        std::vector<u8> p;
        p.push_back(0);  // success
        wr16(p, WIIMOTE_HANDLE);
        p.insert(p.end(), WIIMOTE_BDADDR, WIIMOTE_BDADDR + 6);
        p.push_back(1);  // ACL
        p.push_back(0);  // no encryption
        event(0x03, p);
        remote_connected_ = true;
        hold_channels_ = false;
        ctrl_ = Channel{0x11, 0x40};
        intr_ = Channel{0x13, 0x41};
        log("bt: Wii Remote connected (handle %03X)", WIIMOTE_HANDLE);
    }

    void hci_command(const std::vector<u8>& pkt) {
        if (pkt.size() < 3)
            return;
        u16 op = rd16(&pkt[0]);
        const u8* prm = &pkt[3];
        size_t plen = pkt.size() - 3;
        static int s_logged;
        if (s_logged++ < 80) {
            char hex[64] = "";
            for (size_t i = 0; i < pkt.size() && i < 16; i++)
                snprintf(hex + 3 * i, sizeof(hex) - 3 * i, "%02X ", pkt[i]);
            log("bt: HCI command %04X (%zu bytes): %s", op, plen, hex);
        }
        std::vector<u8> ok{0};
        switch (op) {
        case 0x0C03:  // Reset
            scan_enable_ = 0;
            command_complete(op, ok);
            break;
        case 0x1001: {  // Read Local Version Information
            std::vector<u8> r{0, 0x03};
            wr16(r, 0x40A7);
            r.push_back(0x03);
            wr16(r, 0x000F);
            wr16(r, 0x430E);
            command_complete(op, r);
            break;
        }
        case 0x1003:  // Read Local Supported Features
            command_complete(op, {0, 0xFF, 0xFF, 0x8D, 0xFE, 0x9B, 0xF9, 0x00, 0x80});
            break;
        case 0x1005: {  // Read Buffer Size
            std::vector<u8> r{0};
            wr16(r, ACL_MTU);
            r.push_back(64);
            wr16(r, ACL_PACKETS);
            wr16(r, 0);
            command_complete(op, r);
            break;
        }
        case 0x1009: {  // Read BD_ADDR
            std::vector<u8> r{0};
            r.insert(r.end(), CONTROLLER_BDADDR, CONTROLLER_BDADDR + 6);
            command_complete(op, r);
            break;
        }
        case 0x0C1A:  // Write Scan Enable
            scan_enable_ = plen ? prm[0] : 0;
            command_complete(op, ok);
            break;
        case 0x0C0D: {  // Read Stored Link Key: the remote was paired with this console
            bool all = plen >= 7 && prm[6] == 1;
            bool ours = all || (plen >= 6 && memcmp(prm, WIIMOTE_BDADDR, 6) == 0);
            if (ours) {
                std::vector<u8> e{1};  // Return Link Keys: bdaddr + key
                e.insert(e.end(), WIIMOTE_BDADDR, WIIMOTE_BDADDR + 6);
                e.insert(e.end(), WIIMOTE_LINK_KEY, WIIMOTE_LINK_KEY + 16);
                event(0x15, e);
            }
            std::vector<u8> r{0};
            wr16(r, 255);
            wr16(r, ours ? 1 : 0);
            command_complete(op, r);
            break;
        }
        case 0x0C12: {  // Delete Stored Link Key
            std::vector<u8> r{0};
            wr16(r, 0);
            command_complete(op, r);
            break;
        }
        case 0x0C37: {  // Write Link Supervision Timeout: returns the handle
            std::vector<u8> r{0};
            wr16(r, plen >= 2 ? rd16(prm) : 0);
            command_complete(op, r);
            break;
        }
        case 0x080D: {  // Write Link Policy Settings
            std::vector<u8> r{0};
            wr16(r, plen >= 2 ? rd16(prm) : 0);
            command_complete(op, r);
            break;
        }
        case 0x0409:  // Accept Connection Request: bdaddr, role
            command_status(op);
            if (plen >= 7 && prm[6] == 0) {  // the host wants to be master: role change
                std::vector<u8> r{0};
                r.insert(r.end(), WIIMOTE_BDADDR, WIIMOTE_BDADDR + 6);
                r.push_back(0);
                event(0x12, r);
            }
            connection_complete();
            break;
        case 0x040A:  // Reject Connection Request
            command_status(op);
            connect_requested_ = false;
            break;
        case 0x0405:  // Create Connection (the console paging a remote)
            command_status(op);
            if (plen >= 6 && memcmp(prm, WIIMOTE_BDADDR, 6) == 0) {
                connection_complete();
            } else {
                std::vector<u8> p{0x04};  // page timeout
                wr16(p, 0);
                p.insert(p.end(), prm, prm + 6);
                p.push_back(1);
                p.push_back(0);
                event(0x03, p);
            }
            break;
        case 0x0406: {  // Disconnect
            command_status(op);
            std::vector<u8> p{0};
            wr16(p, plen >= 2 ? rd16(prm) : WIIMOTE_HANDLE);
            p.push_back(0x16);  // terminated by the local host
            event(0x05, p);
            remote_connected_ = false;
            connect_requested_ = false;
            log("bt: Wii Remote disconnected by the host");
            break;
        }
        case 0x040B: {  // Link Key Request Reply: the console knows the key, authenticated
            std::vector<u8> r{0};
            r.insert(r.end(), prm, prm + 6);
            command_complete(op, r);
            if (auth_pending_) {
                auth_pending_ = false;
                bool ok = plen >= 22 && memcmp(prm + 6, WIIMOTE_LINK_KEY, 16) == 0;
                std::vector<u8> p{(u8)(ok ? 0x00 : 0x05)};  // 0x05: authentication failure
                wr16(p, WIIMOTE_HANDLE);
                event(0x06, p);
                log("bt: authentication %s", ok ? "done" : "failed (wrong link key)");
            }
            break;
        }
        case 0x040C: {  // Link Key Request Negative Reply
            std::vector<u8> r{0};
            r.insert(r.end(), prm, prm + 6);
            command_complete(op, r);
            if (auth_pending_) {
                auth_pending_ = false;
                std::vector<u8> p{0x06};  // PIN or key missing
                wr16(p, WIIMOTE_HANDLE);
                event(0x06, p);
                log("bt: authentication failed (the console has no key for the remote)");
            }
            break;
        }
        case 0x0411: {  // Authentication Requested: the controller asks the host for the key
            command_status(op);
            std::vector<u8> p(WIIMOTE_BDADDR, WIIMOTE_BDADDR + 6);
            event(0x17, p);  // Link Key Request
            auth_pending_ = true;
            break;
        }
        case 0x0413: {  // Set Connection Encryption
            command_status(op);
            std::vector<u8> p{0};
            wr16(p, plen >= 2 ? rd16(prm) : WIIMOTE_HANDLE);
            p.push_back(plen >= 3 ? prm[2] : 1);
            event(0x08, p);  // Encryption Change
            break;
        }
        case 0x0419: {  // Remote Name Request
            command_status(op);
            std::vector<u8> p{0};
            p.insert(p.end(), prm, prm + 6);
            const char* nm = "Nintendo RVL-CNT-01";
            std::vector<u8> name(248, 0);
            memcpy(name.data(), nm, strlen(nm));
            p.insert(p.end(), name.begin(), name.end());
            event(0x07, p);
            break;
        }
        case 0x041B: {  // Read Remote Supported Features
            command_status(op);
            std::vector<u8> p{0};
            wr16(p, WIIMOTE_HANDLE);
            const u8 f[8] = {0xBC, 0x02, 0x04, 0x38, 0x08, 0x00, 0x00, 0x00};
            p.insert(p.end(), f, f + 8);
            event(0x0B, p);
            break;
        }
        case 0x041D: {  // Read Remote Version Information
            command_status(op);
            std::vector<u8> p{0};
            wr16(p, WIIMOTE_HANDLE);
            p.push_back(0x02);
            wr16(p, 0x000F);
            wr16(p, 0x0229);
            event(0x0C, p);
            break;
        }
        case 0x041F: {  // Read Clock Offset
            command_status(op);
            std::vector<u8> p{0};
            wr16(p, WIIMOTE_HANDLE);
            wr16(p, 0x3818);
            event(0x1C, p);
            break;
        }
        case 0x0803: {  // Sniff Mode
            command_status(op);
            std::vector<u8> p{0};
            wr16(p, WIIMOTE_HANDLE);
            p.push_back(2);  // sniff
            wr16(p, plen >= 4 ? rd16(prm + 2) : 0);
            event(0x14, p);
            break;
        }
        case 0x0401:  // Inquiry: nobody new around
            command_status(op);
            event(0x01, {0});
            break;
        case 0x0402:  // Inquiry Cancel
            command_complete(op, ok);
            break;
        case 0x040F: {  // Change Connection Packet Type
            command_status(op);
            std::vector<u8> p{0};
            wr16(p, WIIMOTE_HANDLE);
            wr16(p, plen >= 4 ? rd16(prm + 2) : 0xCC18);
            event(0x1D, p);
            break;
        }
        default:
            // settings the controller only stores (event filter, page timeout, class of device,
            // local name, PIN type, host buffer size, scan types, vendor commands, ...)
            if ((op >> 10) == 0x01 || (op >> 10) == 0x02) {
                command_status(op);  // link control/policy commands not modelled
                log("bt: HCI command %04X answered with a bare Command Status", op);
            } else {
                command_complete(op, ok);
            }
            break;
        }
    }

    // ---- ACL from the console: L2CAP
    void acl_out(const std::vector<u8>& pkt) {
        if (pkt.size() < 4)
            return;
        static int s_out;
        if (s_out++ < 80) {
            char b[128];
            int k = 0;
            for (size_t i = 0; i < pkt.size() && i < 24; i++)
                k += snprintf(b + k, sizeof(b) - k, "%02X ", pkt[i]);
            log("bt: ACL <- console: %s", b);
        }
        u16 handle = rd16(&pkt[0]) & 0x0FFF;
        u16 len = rd16(&pkt[2]);
        // the controller frees the buffer: Number Of Completed Packets
        std::vector<u8> nc{1};
        wr16(nc, handle);
        wr16(nc, 1);
        event(0x13, nc);
        if (handle != WIIMOTE_HANDLE || !remote_connected_ || pkt.size() < 4u + len || len < 4)
            return;
        const u8* l2 = &pkt[4];
        u16 l2len = rd16(l2), cid = rd16(l2 + 2);
        if (l2len + 4u > len)
            return;
        const u8* data = l2 + 4;
        if (cid == 0x0001)
            l2cap_signal(data, l2len);
        else if (cid == ctrl_.local_cid)
            hid_control(data, l2len);
        else if (cid == intr_.local_cid)
            hid_interrupt(data, l2len);
    }

    void l2cap_send(u16 cid, const std::vector<u8>& payload) {
        std::vector<u8> a;
        wr16(a, (u16)(WIIMOTE_HANDLE | (0x2 << 12)));  // packet start, point to point
        wr16(a, (u16)(payload.size() + 4));
        wr16(a, (u16)payload.size());
        wr16(a, cid);
        a.insert(a.end(), payload.begin(), payload.end());
        acl_.push_back(a);
    }

    void signal(u8 code, u8 ident, const std::vector<u8>& data) {
        std::vector<u8> p;
        p.push_back(code);
        p.push_back(ident);
        wr16(p, (u16)data.size());
        p.insert(p.end(), data.begin(), data.end());
        l2cap_send(0x0001, p);
    }

    Channel* channel_by_local(u16 cid) {
        if (cid == ctrl_.local_cid)
            return &ctrl_;
        if (cid == intr_.local_cid)
            return &intr_;
        return nullptr;
    }

    // The remote opens its channels itself, control first.
    void l2cap_progress() {
        if (hold_channels_)
            return;
        for (Channel* c : {&ctrl_, &intr_}) {
            if (c == &intr_ && !ctrl_.complete())
                return;
            if (c->state == L2_CLOSED) {
                std::vector<u8> d;
                wr16(d, c->psm);
                wr16(d, c->local_cid);
                signal(0x02, next_ident_++, d);
                c->state = L2_WAIT_CONNECT_RSP;
                return;
            }
            if (c->state == L2_OPEN && !c->config_sent) {
                std::vector<u8> d;
                wr16(d, c->remote_cid);
                wr16(d, 0);  // flags
                d.push_back(0x01);  // option: MTU
                d.push_back(2);
                wr16(d, 185);
                signal(0x04, next_ident_++, d);
                c->config_sent = true;
                return;
            }
            if (!c->complete())
                return;
        }
    }

    void l2cap_signal(const u8* d, u16 n) {
        while (n >= 4) {
            u8 code = d[0], ident = d[1];
            u16 len = rd16(d + 2);
            if (len + 4u > n)
                return;
            const u8* p = d + 4;
            switch (code) {
            case 0x03: {  // Connection Response: dcid (console), scid (ours), result, status
                u16 dcid = rd16(p), scid = rd16(p + 2), result = rd16(p + 4);
                Channel* c = channel_by_local(scid);
                if (c && result == 0) {
                    c->remote_cid = dcid;
                    c->state = L2_OPEN;
                    log("bt: L2CAP PSM %02X open (console cid %04X)", c->psm, dcid);
                }
                break;
            }
            case 0x04: {  // Configuration Request from the console: accept whatever it asks
                u16 dcid = rd16(p);
                Channel* c = channel_by_local(dcid);
                std::vector<u8> r;
                wr16(r, c ? c->remote_cid : 0);
                wr16(r, 0);  // flags
                wr16(r, 0);  // success
                if (len > 4)
                    r.insert(r.end(), p + 4, p + len);
                signal(0x05, ident, r);
                if (c)
                    c->they_configured = true;
                break;
            }
            case 0x05: {  // Configuration Response to ours
                u16 scid = rd16(p), result = rd16(p + 4);
                Channel* c = channel_by_local(scid);
                if (c && result == 0)
                    c->we_configured = true;
                if (c && c->complete())
                    log("bt: L2CAP PSM %02X configured", c->psm);
                break;
            }
            case 0x06: {  // Disconnection Request
                u16 dcid = rd16(p), scid = rd16(p + 2);
                std::vector<u8> r;
                wr16(r, dcid);
                wr16(r, scid);
                signal(0x07, ident, r);
                if (Channel* c = channel_by_local(dcid)) {
                    log("bt: L2CAP PSM %02X closed by the console", c->psm);
                    *c = Channel{c->psm, c->local_cid};
                    hold_channels_ = true;
                }
                break;
            }
            case 0x02: {  // Connection Request from the console: refuse (no SDP etc.)
                std::vector<u8> r;
                wr16(r, 0);
                wr16(r, rd16(p + 2));
                wr16(r, 0x0002);  // PSM not supported
                wr16(r, 0);
                signal(0x03, ident, r);
                break;
            }
            case 0x01:  // Command Reject
            case 0x07:  // Disconnection Response
                break;
            default:
                log("bt: L2CAP signal %02X ignored", code);
                break;
            }
            d += 4 + len;
            n -= 4 + len;
        }
    }

    // HID on the control channel: SET_REPORT (output) gets a handshake
    void hid_control(const u8* d, u16 n) {
        if (n < 1)
            return;
        u8 type = d[0] >> 4;
        if (type == 0x5 && n >= 2) {  // SET_REPORT
            wiimote_output(d + 1, n - 1);
            l2cap_send(ctrl_.remote_cid, {0x00});  // handshake: successful
        } else if (type == 0xA && n >= 2) {  // DATA
            wiimote_output(d + 1, n - 1);
        } else {
            l2cap_send(ctrl_.remote_cid, {0x03});  // handshake: unsupported request
        }
    }

    void hid_interrupt(const u8* d, u16 n) {
        if (n >= 2 && d[0] == 0xA2)
            wiimote_output(d + 1, n - 1);
    }

    void wiimote_output(const u8* rep, u16 n) {
        std::vector<std::vector<u8>> replies;
        wiimote_handle_output(rep, n, replies);
        for (auto& r : replies)
            send_hid(r);
    }
};

BtController* BtController::s_instance = nullptr;

}  // namespace

std::unique_ptr<IosHandle> bt_open_device() { return std::unique_ptr<IosHandle>(new BtController()); }

void bt_tick(u64 now_us) {
    if (BtController::s_instance)
        BtController::s_instance->tick(now_us);
}

}  // namespace guest
