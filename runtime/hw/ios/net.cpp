// The network side of IOS: /dev/net/ncd/manage (connection settings and link state),
// /dev/net/ip/top (BSD sockets, on the host's sockets) and /dev/net/kd (WiiConnect24).
//
// Offline (NOMA_OFFLINE, the default: net.h) the connection is configured but its link is down:
// the console has no address and every socket request fails as with the cable out.
//
// Sockets keep the Wii's semantics on host sockets that are always non-blocking: a call that
// would block a blocking guest socket stays pending and is retried from net_tick(), so the other
// guest threads keep running meanwhile (all of them share one host thread).
//
// The Wii's structures are big-endian; sockaddr_in is {u8 len, u8 family, u16 port, u32 addr}.
// Errors are negative values from the Wii's own errno list (SO_E*).
#include "net.h"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifdef __3DS__
#include <3ds.h>
#endif
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ios.h"

namespace guest {

bool net_offline() {
    static bool offline = [] {
        const char* v = getenv("NOMA_OFFLINE");
        bool off = !v || strcmp(v, "0") != 0;
        log("net: %s", off ? "offline (NOMA_OFFLINE=0 goes online)" : "online");
        return off;
    }();
    return offline;
}

namespace {

bool s_debug = getenv("NOMA_DEBUG_NET") != nullptr;

// ---------------------------------------------------------------------------- errors
enum : s32 {
    SO_EAGAIN = 6, SO_EALREADY = 7, SO_EBADF = 8, SO_ECONNABORTED = 13, SO_ECONNREFUSED = 14,
    SO_ECONNRESET = 15, SO_EHOSTUNREACH = 23, SO_EINPROGRESS = 26, SO_EINVAL = 28, SO_EISCONN = 30,
    SO_EMSGSIZE = 35, SO_ENETDOWN = 38, SO_ENETUNREACH = 40, SO_ENOBUFS = 42, SO_ENOMEM = 49,
    SO_ENOPROTOOPT = 51, SO_ENOTCONN = 56, SO_EOPNOTSUPP = 63, SO_EPIPE = 66, SO_ETIMEDOUT = 76,
};

s32 wii_error(int e) {
    switch (e) {
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
        return -SO_EAGAIN;
    case EALREADY: return -SO_EALREADY;
    case EBADF: return -SO_EBADF;
    case ECONNABORTED: return -SO_ECONNABORTED;
    case ECONNREFUSED: return -SO_ECONNREFUSED;
    case ECONNRESET: return -SO_ECONNRESET;
    case EHOSTUNREACH: return -SO_EHOSTUNREACH;
    case EINPROGRESS: return -SO_EINPROGRESS;
    case EISCONN: return -SO_EISCONN;
    case EMSGSIZE: return -SO_EMSGSIZE;
    case ENETDOWN: return -SO_ENETDOWN;
    case ENETUNREACH: return -SO_ENETUNREACH;
    case ENOBUFS: return -SO_ENOBUFS;
    case ENOMEM: return -SO_ENOMEM;
    case ENOPROTOOPT: return -SO_ENOPROTOOPT;
    case ENOTCONN: return -SO_ENOTCONN;
    case EOPNOTSUPP: return -SO_EOPNOTSUPP;
    case EPIPE: return -SO_EPIPE;
    case ETIMEDOUT: return -SO_ETIMEDOUT;
    default: return -SO_EINVAL;
    }
}

// ---------------------------------------------------------------------------- addresses
// the console's address on its (emulated) network: a fixed private one
const u32 HOST_IP = 0xC0A80164;  // 192.168.1.100

void read_sockaddr(u32 a, sockaddr_in* out) {
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(load16(a + 2));
    out->sin_addr.s_addr = htonl(load32(a + 4));
}

void write_sockaddr(u32 a, const sockaddr_in& in) {
    store8(a, 8);
    store8(a + 1, 2);  // AF_INET
    store16(a + 2, ntohs(in.sin_port));
    store32(a + 4, ntohl(in.sin_addr.s_addr));
}

// ---------------------------------------------------------------------------- sockets
struct Sock {
    int fd = -1;
    bool blocking = true;
    bool connecting = false;  // a non-blocking connect in progress (for the next connect call)
};

std::map<s32, Sock> s_socks;
s32 s_next_sock = 3;

Sock* sock(u32 fd) {
    auto it = s_socks.find((s32)fd);
    return it == s_socks.end() ? nullptr : &it->second;
}

// a request kept pending while a blocking guest call waits; retried from net_tick
struct Pending {
    u32 request;  // physical
    std::function<bool(s32*)> retry;  // true when done, with the result
    u64 deadline_us;                  // 0: none
};
std::vector<Pending> s_pending;

class IpTopDevice;

// ---------------------------------------------------------------------------- /dev/net/ip/top
class IpTopDevice : public IosHandle {
public:
    bool pending = false;
    bool reply_now() override {
        bool p = pending;
        pending = false;
        return !p;
    }

    // run `op` now; if it would block a blocking socket, keep the request pending
    s32 maybe_block(Sock* s, std::function<bool(s32*)> op, u64 timeout_us = 0) {
        s32 r;
        if (op(&r) || !s || !s->blocking)
            return r;
        pending = true;
        s_pending.push_back({cur_request, op, timeout_us ? host_time_us() + timeout_us : 0});
        return 0;
    }

    s32 ioctl(u32 cmd, u32 in, u32 in_len, u32 out, u32 out_len) override {
        s32 r = ioctl_(cmd, in, in_len, out, out_len);
        if (s_debug)
            log("net: ioctl %u -> %d%s", cmd, r, pending ? " (pending)" : "");
        return r;
    }

    s32 ioctl_(u32 cmd, u32 in, u32 in_len, u32 out, u32 out_len) {
        switch (cmd) {
        case 0x1F:  // startup
        case 0x1E:  // setinterface
        case 0x14:
            return 0;
        case 0x10:  // gethostid
            return (s32)HOST_IP;
        case 0x0F: {  // socket(af, type, protocol)
            u32 type = load32(in + 4);
            int fd = ::socket(AF_INET, type == 2 ? SOCK_DGRAM : SOCK_STREAM, 0);
            if (fd < 0)
                return wii_error(errno);
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
            s32 id = s_next_sock++;
            s_socks[id].fd = fd;
            return id;
        }
        case 0x03: {  // close
            Sock* s = sock(load32(in));
            if (!s)
                return -SO_EBADF;
            ::close(s->fd);
            s_socks.erase((s32)load32(in));
            return 0;
        }
        case 0x02:    // bind(fd, has_addr, sockaddr)
        case 0x04: {  // connect
            Sock* s = sock(load32(in));
            if (!s)
                return -SO_EBADF;
            sockaddr_in sa;
            read_sockaddr(in + 8, &sa);
            if (cmd == 0x02)
                return ::bind(s->fd, (sockaddr*)&sa, sizeof(sa)) < 0 ? wii_error(errno) : 0;
            if (s_debug)
                log("net: connect %s:%u", inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));
            int fd = s->fd;
            if (::connect(fd, (sockaddr*)&sa, sizeof(sa)) == 0)
                return 0;
            if (errno != EINPROGRESS)
                return wii_error(errno);
            if (!s->blocking)
                return -SO_EINPROGRESS;
            // wait for the connection
            return maybe_block(s, [fd](s32* r) {
                pollfd p{fd, POLLOUT, 0};
                if (::poll(&p, 1, 0) <= 0) {
                    *r = -SO_EAGAIN;
                    return false;
                }
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                *r = err ? wii_error(err) : 0;
                return true;
            });
        }
        case 0x0A: {  // listen
            Sock* s = sock(load32(in));
            if (!s)
                return -SO_EBADF;
            return ::listen(s->fd, (int)load32(in + 4)) < 0 ? wii_error(errno) : 0;
        }
        case 0x01: {  // accept
            Sock* s = sock(load32(in));
            if (!s)
                return -SO_EBADF;
            int lfd = s->fd;
            return maybe_block(s, [lfd, out, out_len](s32* r) {
                sockaddr_storage ss{};
                socklen_t len = sizeof(ss);
                int fd = ::accept(lfd, (sockaddr*)&ss, &len);
                const sockaddr_in& sa = *(const sockaddr_in*)&ss;
                if (fd < 0) {
                    *r = wii_error(errno);
                    return errno != EAGAIN && errno != EWOULDBLOCK;
                }
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
                s32 id = s_next_sock++;
                s_socks[id].fd = fd;
                if (out && out_len >= 8)
                    write_sockaddr(out, sa);
                *r = id;
                return true;
            });
        }
        case 0x05: {  // fcntl(fd, cmd, arg): F_GETFL 3, F_SETFL 4, O_NONBLOCK 4
            Sock* s = sock(load32(in));
            if (!s)
                return -SO_EBADF;
            u32 c = load32(in + 4), arg = load32(in + 8);
            if (c == 3)
                return s->blocking ? 0 : 4;
            if (c == 4) {
                s->blocking = !(arg & 4);
                return 0;
            }
            return -SO_EINVAL;
        }
        case 0x0E: {  // shutdown
            Sock* s = sock(load32(in));
            if (!s)
                return -SO_EBADF;
            ::shutdown(s->fd, (int)load32(in + 4));
            return 0;
        }
        case 0x09: {  // setsockopt(fd, level, name, optlen, optval[20])
            Sock* s = sock(load32(in));
            if (!s)
                return -SO_EBADF;
            u32 level = load32(in + 4), name = load32(in + 8);
            int v = (int)load32(in + 0x10);
            if (level == 0xFFFF && name == 4)
                setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v));
            else if (level == 6 && name == 0x2001)
                setsockopt(s->fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v));
            else if (level == 0xFFFF && name == 0x1001)
                setsockopt(s->fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v));
            else if (level == 0xFFFF && name == 0x1002)
                setsockopt(s->fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v));
            return 0;
        }
        case 0x08: {  // getsockopt: SO_ERROR (0x1009) and the rest as 0
            Sock* s = sock(load32(in));
            if (!s)
                return -SO_EBADF;
            int v = 0;
            if (load32(in + 4) == 0xFFFF && load32(in + 8) == 0x1009) {
                socklen_t len = sizeof(v);
                getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &v, &len);
                v = v ? -wii_error(v) : 0;
            }
            if (out && out_len >= 0x14) {
                store32(out + 0x0C, 4);
                store32(out + 0x10, (u32)v);
            }
            return 0;
        }
        case 0x06:    // getpeername
        case 0x07: {  // getsockname
            Sock* s = sock(load32(in));
            if (!s)
                return -SO_EBADF;
            sockaddr_storage ss{};
            socklen_t len = sizeof(ss);
            int rr = cmd == 6 ? getpeername(s->fd, (sockaddr*)&ss, &len) : getsockname(s->fd, (sockaddr*)&ss, &len);
            sockaddr_in sa = *(const sockaddr_in*)&ss;
            if (rr < 0)
                return wii_error(errno);
            if (cmd == 7 && sa.sin_addr.s_addr == 0)
                sa.sin_addr.s_addr = htonl(HOST_IP);
            if (out && out_len >= 8)
                write_sockaddr(out, sa);
            return 0;
        }
        case 0x0B: {  // poll(timeout ms): pollfd {s32 fd, s32 events, s32 revents}[] in out
            s64 timeout = (s64)load64(in);
            u32 n = out_len / 12;
            auto do_poll = [out, n](s32* r) {
                std::vector<pollfd> p(n);
                for (u32 i = 0; i < n; i++) {
                    Sock* s = sock(load32(out + 12 * i));
                    u32 ev = load32(out + 12 * i + 4);
                    p[i].fd = s ? s->fd : -1;
                    p[i].events = (short)(((ev & 0x3) ? POLLIN : 0) | ((ev & 0x8) ? POLLOUT : 0));
                    p[i].revents = 0;
                }
                int k = ::poll(p.data(), n, 0);
                s32 count = 0;
                for (u32 i = 0; i < n; i++) {
                    u32 rev = 0;
                    if (p[i].revents & POLLIN) rev |= 0x1;
                    if (p[i].revents & POLLOUT) rev |= 0x8;
                    if (p[i].revents & POLLERR) rev |= 0x20;
                    if (p[i].revents & POLLHUP) rev |= 0x40;
                    if (p[i].revents & POLLNVAL) rev |= 0x80;
                    store32(out + 12 * i + 8, rev);
                    if (rev)
                        count++;
                }
                *r = k < 0 ? wii_error(errno) : count;
                return count > 0 || k < 0;
            };
            s32 r;
            if (do_poll(&r) || timeout == 0)
                return r;
            pending = true;
            s_pending.push_back({cur_request, do_poll, timeout > 0 ? host_time_us() + (u64)timeout * 1000 : 0});
            return 0;
        }
        case 0x11: {  // gethostbyname(name) -> a hostent in out (pointers into out)
            std::string name = read_string(in, in_len);
            hostent* he = ::gethostbyname(name.c_str());
            if (s_debug)
                log("net: gethostbyname %s -> %s", name.c_str(), he ? "ok" : "failed");
            if (!he || he->h_addrtype != AF_INET || out_len < 0x460)
                return -1;
            // hostent {char* name, char** aliases, s16 addrtype, s16 length, char** addr_list}
            u32 base = out;
            u32 p_name = base + 0x10, p_aliases = base + 0x110, p_list = base + 0x340, p_addrs = base + 0x360;
            u32 nlen = std::min<u32>((u32)strlen(he->h_name), 0xFF);
            write_g(p_name, he->h_name, nlen);
            store8(p_name + nlen, 0);
            store32(p_aliases, 0);
            int i = 0;
            for (; he->h_addr_list[i] && i < 8; i++) {
                u32 a;
                memcpy(&a, he->h_addr_list[i], 4);
                store32(p_addrs + 4 * i, ntohl(a));
                store32(p_list + 4 * i, p_addrs + 4 * i);
            }
            store32(p_list + 4 * i, 0);
            store32(base + 0, p_name);
            store32(base + 4, p_aliases);
            store16(base + 8, 2);
            store16(base + 10, 4);
            store32(base + 12, p_list);
            return 0;
        }
        case 0x15: {  // inet_aton(string) -> u32 in out
            std::string s = read_string(in, in_len);
            in_addr a;
            if (!inet_aton(s.c_str(), &a))
                return 0;
            store32(out, ntohl(a.s_addr));
            return 1;
        }
        default:
            log("net: /dev/net/ip/top ioctl 0x%X (not emulated)", cmd);
            return -SO_EINVAL;
        }
    }

    s32 ioctlv(u32 cmd, const std::vector<IoVec>& in, const std::vector<IoVec>& io) override {
        s32 r = ioctlv_(cmd, in, io);
        if (s_debug)
            log("net: ioctlv %u -> %d%s", cmd, r, pending ? " (pending)" : "");
        return r;
    }

    s32 ioctlv_(u32 cmd, const std::vector<IoVec>& in, const std::vector<IoVec>& io) {
        switch (cmd) {
        case 0x0D: {  // sendto: in[0] data, in[1] {fd, flags, has_dest, sockaddr}
            if (in.size() < 2)
                return -SO_EINVAL;
            u32 prm = in[1].addr;
            Sock* s = sock(load32(prm));
            if (!s)
                return -SO_EBADF;
            int fd = s->fd;
            u32 data = in[0].addr, len = in[0].size;
            bool has_dest = load32(prm + 8) != 0;
            sockaddr_in sa;
            if (has_dest)
                read_sockaddr(prm + 12, &sa);
            return maybe_block(s, [fd, data, len, has_dest, sa](s32* r) {
                std::vector<u8> buf(len);
                read_g(buf.data(), data, len);
                int flags = 0;
#ifdef MSG_NOSIGNAL
                flags |= MSG_NOSIGNAL;
#endif
                ssize_t n = has_dest ? ::sendto(fd, buf.data(), len, flags, (const sockaddr*)&sa, sizeof(sa))
                                     : ::send(fd, buf.data(), len, flags);
                if (s_debug && n > 0) {
                    std::string t((const char*)buf.data(), std::min<size_t>((size_t)n, 300));
                    for (char& ch : t)
                        if (ch < 32 && ch != '\n')
                            ch = '.';
                    log("net: send %d bytes: %s", (int)n, t.c_str());
                }
                if (n < 0) {
                    *r = wii_error(errno);
                    return errno != EAGAIN && errno != EWOULDBLOCK;
                }
                *r = (s32)n;
                return true;
            });
        }
        case 0x0C: {  // recvfrom: in[0] {fd, flags}; io[0] data, io[1] sockaddr (optional)
            if (in.empty() || io.empty())
                return -SO_EINVAL;
            Sock* s = sock(load32(in[0].addr));
            if (!s)
                return -SO_EBADF;
            int fd = s->fd;
            u32 data = io[0].addr, len = io[0].size;
            u32 from = io.size() > 1 && io[1].size >= 8 ? io[1].addr : 0;
            int peek = (load32(in[0].addr + 4) & 2) ? MSG_PEEK : 0;
            return maybe_block(s, [fd, data, len, from, peek](s32* r) {
                std::vector<u8> buf(len ? len : 1);
                // ask for the sender only when the guest wants it (UDP): libctru's recvfrom
                // copied a stream socket's address past the end of a sockaddr_in on the 3DS
                sockaddr_storage ss{};
                socklen_t sl = sizeof(ss);
                ssize_t n = from ? ::recvfrom(fd, buf.data(), len, peek, (sockaddr*)&ss, &sl)
                                 : ::recv(fd, buf.data(), len, peek);
                if (n < 0) {
                    *r = wii_error(errno);
                    return errno != EAGAIN && errno != EWOULDBLOCK;
                }
                write_g(data, buf.data(), (u32)n);
                if (from && ss.ss_family == AF_INET)
                    write_sockaddr(from, *(const sockaddr_in*)&ss);
                if (s_debug) {
                    std::string t((const char*)buf.data(), std::min<size_t>((size_t)n, 2000));
                    for (char& ch : t)
                        if (ch < 32 && ch != '\n')
                            ch = '.';
                    log("net: recv %d bytes: %s", (int)n, t.c_str());
                }
                *r = (s32)n;
                return true;
            });
        }
        case 0x18: {  // getaddrinfo(node, service, hints) -> addrinfo list in io[0]
            if (in.empty() || io.empty())
                return -1;
            std::string node = in[0].size ? read_string(in[0].addr, in[0].size) : "";
            addrinfo hints{};
            hints.ai_family = AF_INET;
            addrinfo* res = nullptr;
            if (s_debug)
                log("net: getaddrinfo %s ...", node.c_str());
            int rc = getaddrinfo(node.c_str(), nullptr, &hints, &res);
            if (s_debug)
                log("net: getaddrinfo %s -> %d", node.c_str(), rc);
            if (rc != 0 || !res)
                return -305;  // EAI_NONAME
            // addrinfo {s32 flags, family, socktype, protocol; u32 addrlen; char* canonname;
            // sockaddr* addr; addrinfo* next}: 0x20 bytes each from the start of the buffer, their
            // sockaddrs (0x1C bytes each) from 0x460
            u32 base = io[0].addr, n = 0;
            for (addrinfo* a = res; a; a = a->ai_next) {
                if (a->ai_family != AF_INET || 0x20 * (n + 1) > 0x460 || 0x460 + 0x1C * (n + 1) > io[0].size)
                    continue;
                u32 e = base + 0x20 * n, sa = base + 0x460 + 0x1C * n;
                memset_g(e, 0, 0x20);
                memset_g(sa, 0, 0x1C);
                store32(e + 4, 2);
                store32(e + 8, a->ai_socktype == SOCK_DGRAM ? 2 : 1);
                store32(e + 0x0C, (u32)a->ai_protocol);
                store32(e + 0x10, 8);
                store32(e + 0x18, sa);
                write_sockaddr(sa, *(sockaddr_in*)a->ai_addr);
                if (n)
                    store32(e - 0x20 + 0x1C, e);
                if (s_debug)
                    log("net:   %s", inet_ntoa(((sockaddr_in*)a->ai_addr)->sin_addr));
                n++;
            }
            freeaddrinfo(res);
            return n ? 0 : -305;
        }
        case 0x1C: {  // getinterfaceopt(0xFFFE, option) -> io[0] value, io[1] length
            if (in.empty() || io.empty() || load32(in[0].addr) != 0xFFFE)
                return -SO_EINVAL;
            u32 opt = load32(in[0].addr + 4), v = io[0].addr;
            switch (opt) {
            case 0xB003:  // DNS servers
                store32(v, 0x08080808);
                if (io[0].size >= 8)
                    store32(v + 4, 0x08080404);
                break;
            case 0x1003: store32(v, 0); break;  // last error
            case 0x1004: {                      // MAC address
                static const u8 mac[6] = {0x00, 0x17, 0xAB, 0x12, 0x34, 0x56};
                write_g(v, mac, 6);
                break;
            }
            case 0x1005: store32(v, 1); break;     // link up
            case 0x3001: store32(v, 0x10); break;
            case 0x4002: store32(v, 1); break;     // one address
            case 0x4003:                           // address, netmask, broadcast
                if (io.size() > 1)
                    store32(io[1].addr, 0xC);
                store32(v, HOST_IP);
                if (io[0].size >= 12)
                    store32(v + 4, 0xFFFFFF00), store32(v + 8, (HOST_IP & 0xFFFFFF00) | 0xFF);
                break;
            case 0x4005: store32(v, 0x20); break;
            case 0x6003: case 0x600A: case 0x600C: store32(v, 0x80); break;
            case 0xB002: store32(v, 2); break;
            default:
                log("net: interface option %04X", opt);
                break;
            }
            return 0;
        }
        case 0x1D:  // setinterfaceopt
            return 0;
        default:
            log("net: /dev/net/ip/top ioctlv 0x%X (not emulated)", cmd);
            return -SO_EINVAL;
        }
    }
};

// offline: no link and so no address; every socket request fails as the network is down
class OfflineIpTopDevice : public IosHandle {
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
// One wired connection, configured with DHCP and tested; its link is up only online.
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
                store32(io[0].addr + 4, net_offline() ? 2 : 3);
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
    if (path == "/dev/net/ip/top") {
        if (net_offline())
            return std::make_unique<OfflineIpTopDevice>();
        return std::make_unique<IpTopDevice>();
    }
    return std::make_unique<NetDevice>();
}

// retry the socket calls guest threads are blocked in
void net_tick() {
    if (s_pending.empty())
        return;
    u64 now = host_time_us();
    for (size_t i = 0; i < s_pending.size();) {
        Pending& p = s_pending[i];
        s32 r = 0;
        bool done = p.retry(&r);
        if (!done && p.deadline_us && now >= p.deadline_us)
            done = true;  // a poll that timed out: no events (r holds 0 or the count)
        if (done) {
            u32 req = p.request;
            s_pending.erase(s_pending.begin() + i);
            ios_complete(req, r);
        } else {
            i++;
        }
    }
}

}  // namespace guest
