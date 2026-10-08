// The network side of IOS: /dev/net/ncd/manage (connection settings and link state),
// /dev/net/ip/top (BSD sockets) and /dev/net/kd (WiiConnect24).
//
// The port is offline (the user's call for the 3DS redesign: no online mode at all): the
// connection is configured but its link is down, the console has no address, and every socket
// request fails as with the cable out. The channel's online steps are skipped in its own flow
// (runtime/patches/offline.cpp).
//
// The Wii's structures are big-endian. Errors are negative values from the Wii's own errno list
// (SO_E*).
#include "net.h"

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "ios.h"

namespace guest {

namespace {

bool s_debug = getenv("NOMA_DEBUG_NET") != nullptr;

// the Wii's errno for a network that is down
enum : s32 { SO_ENETDOWN = 38 };

// ---------------------------------------------------------------------------- /dev/net/ip/top
// no link and so no address: every socket request fails as the network is down
class IpTopDevice : public IosHandle {
public:
    s32 ioctl(u32 cmd, u32 in, u32 in_len, u32 out, u32 out_len) override {
        if (s_debug)
            log("net: offline: ioctl 0x%X", cmd);
        switch (cmd) {
        case 0x1F: case 0x1E: case 0x14: return 0;  // startup, setinterface
        case 0x10: return 0;                        // gethostid: no address
        default: return -SO_ENETDOWN;
        }
    }
    s32 ioctlv(u32 cmd, const std::vector<IoVec>& in, const std::vector<IoVec>& io) override {
        if (s_debug)
            log("net: offline: ioctlv 0x%X", cmd);
        if (cmd == 0x1D)  // setinterfaceopt
            return 0;
        return cmd == 0x18 ? -305 : -SO_ENETDOWN;  // getaddrinfo: EAI_NONAME
    }
};

// ---------------------------------------------------------------------------- /dev/net/ncd/manage
// One wired connection, configured with DHCP and tested; its link is down.
class NcdDevice : public IosHandle {
public:
    s32 ioctlv(u32 cmd, const std::vector<IoVec>& in, const std::vector<IoVec>& io) override {
        switch (cmd) {
        case 0x03: {  // GetConfig: io[0] the configuration, io[1] the result
            if (io.size() < 2)
                return IOS_EINVAL;
            memset_g(io[0].addr, 0, io[0].size);
            store32(io[0].addr + 0, 0);  // version
            store8(io[0].addr + 4, 2);   // connected through the wired interface
            store8(io[0].addr + 6, 7);   // WiiConnect24: mail and downloads
            // connection 1: wired | DNS by DHCP | IP by DHCP | test passed | selected
            store8(io[0].addr + 8, 0x01 | 0x02 | 0x04 | 0x20 | 0x80);
            store16(io[0].addr + 8 + 0x1A, 1460);  // MTU
            store32(io[1].addr, 0);
            store32(io[1].addr + 4, 0);
            return 0;
        }
        case 0x04:  // SetConfig
            if (io.size() >= 1)
                store32(io[0].addr, 0), store32(io[0].addr + 4, 0);
            return 0;
        case 0x07:  // GetLinkStatus: 3 wired link up; 2 no link (1 busy, 4/5 wireless down/up)
            if (!io.empty()) {
                store32(io[0].addr, 0);
                store32(io[0].addr + 4, 2);
            }
            return 0;
        case 0x08: {  // GetWirelessMacAddress
            static const u8 mac[6] = {0x00, 0x17, 0xAB, 0x12, 0x34, 0x56};
            if (io.size() >= 2) {
                store32(io[0].addr, 0);
                store32(io[0].addr + 4, 0);
                write_g(io[1].addr, mac, 6);
            }
            return 0;
        }
        default:
            log("net: /dev/net/ncd/manage ioctlv 0x%X", cmd);
            if (!io.empty() && io[0].size >= 8)
                store32(io[0].addr, 0), store32(io[0].addr + 4, 0);
            return 0;
        }
    }
};

// ---------------------------------------------------------------------------- /dev/net/kd
// The WiiConnect24 daemon. Its requests report their result in the first word of the output
// buffer. The scheduler, sockets and settings work; downloads and mail don't go out (yet).
class KdDevice : public IosHandle {
public:
    bool time = false;
    s32 ioctl(u32 cmd, u32 in, u32 in_len, u32 out, u32 out_len) override {
        s32 result = 0;
        if (time) {
            switch (cmd) {
            case 0x14: {  // GetUniversalTime: seconds since 2000-01-01 UTC
                u64 t = (u64)::time(nullptr) - 946684800ull;
                if (out_len >= 12)
                    store64(out + 4, t);
                break;
            }
            case 0x15: case 0x16: case 0x17: case 0x18:  // set time / RTC counter / time status
                break;
            default:
                log("ios: %s: ioctl 0x%X", name.c_str(), cmd);
                break;
            }
        } else {
            switch (cmd) {
            case 0x01: case 0x02: case 0x03:  // suspend / try suspend / resume the scheduler
            case 0x05:                        // schedule span
            case 0x06: case 0x07:             // socket startup / cleanup
            case 0x08: case 0x09:             // lock / unlock the socket
            case 0x1F: case 0x20: case 0x21: case 0x22:  // filter / debug / wakeup / script mode
            case 0x28:                                   // shutdown request
                break;
            case 0x04:  // time triggers
                if (out_len >= 12)
                    store32(out + 4, 0), store32(out + 8, 0);
                break;
            case 0x1E:  // scheduler status
                if (out_len >= 8)
                    store32(out + 4, 0);
                break;
            case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E:  // mail / download now
                result = -31;
                break;
            default:
                log("ios: %s: ioctl 0x%X", name.c_str(), cmd);
                break;
            }
        }
        if (out_len >= 4)
            store32(out, (u32)result);
        if (s_debug)
            log("net: %s ioctl 0x%X (in %u bytes) -> %d", name.c_str(), cmd, in_len, result);
        return 0;
    }
};

// everything else under /dev/net (SSL, WiFi, ...): present, but every request fails
class NetDevice : public IosHandle {
public:
    s32 ioctl(u32 cmd, u32 in, u32 in_len, u32 out, u32 out_len) override {
        log("ios: %s: ioctl 0x%X (not emulated)", name.c_str(), cmd);
        return -1;
    }
    s32 ioctlv(u32 cmd, const std::vector<IoVec>& in, const std::vector<IoVec>& io) override {
        log("ios: %s: ioctlv 0x%X (not emulated)", name.c_str(), cmd);
        return -1;
    }
};

}  // namespace

std::unique_ptr<IosHandle> net_open_device(const std::string& path, s32* err) {
    if (path == "/dev/net/kd/request" || path == "/dev/net/kd/time") {
        auto h = std::make_unique<KdDevice>();
        h->time = path == "/dev/net/kd/time";
        return h;
    }
    if (path == "/dev/net/ncd/manage")
        return std::make_unique<NcdDevice>();
    if (path == "/dev/net/ip/top")
        return std::make_unique<IpTopDevice>();
    return std::make_unique<NetDevice>();
}

}  // namespace guest
