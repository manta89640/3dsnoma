// /dev/es: title information and the channel's own contents (what the CNT library reads).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>

#include "ios.h"
#include "../../load_stats.h"

namespace guest {

static const u64 TITLE_ID = 0x000100014843494Aull;  // 00010001-HCIJ
static const bool s_debug = getenv("NOMA_DEBUG_ES") != nullptr;

// (the position is kept here: the file itself may be in use by the I/O thread)
struct ContentHandle {
    FILE* f = nullptr;
    u32 index = 0;
    u64 pos = 0, size = 0;
};

// A ticket view (0xD8 bytes: a version word, then the ticket from its ticket id on, which leaves
// out the title key) for the channel's ticket: every content accessible, no play limits.
static void write_ticket_view(u32 addr) {
    memset_g(addr, 0, 0xD8);
    store64(addr + 0x04, 0x0001000148434900ull);  // ticket id
    store64(addr + 0x10, TITLE_ID);
    store16(addr + 0x18, 0xFFFF);  // access mask
    store32(addr + 0x1C, 0xFFFFFFFF);  // permitted title mask
    memset_g(addr + 0x56, 0xFF, 0x40);  // content access permissions
}

class EsDevice : public IosHandle {
public:
    std::map<u32, ContentHandle> contents;
    u32 next_cfd = 1;
    bool pending = false;  // the read is on the I/O thread: it answers later

    ~EsDevice() override {
        ios_async_drain();
        for (auto& c : contents)
            if (c.second.f)
                fclose(c.second.f);
    }

    // the file opened on the I/O thread (an SD card access), the handle made when the reply goes
    s32 open_content(u32 index) {
        char name[64];
        snprintf(name, sizeof(name), "/%08x.app", index);
        struct Opened {
            std::string path;
            FILE* f = nullptr;
            u64 size = 0;
            void* names = nullptr;  // (NOMA_LOAD_STATS) its archive's files
        };
        auto o = std::make_shared<Opened>();
        o->path = content_dir() + name;
        auto work = [o] {
            if ((o->f = fopen(o->path.c_str(), "rb"))) {
                fseek(o->f, 0, SEEK_END);
                o->size = (u64)ftell(o->f);
                rewind(o->f);
                o->names = lstats_parse_names(o->path.c_str());
            }
            return 0;
        };
        auto finish = [this, o, index](s32) -> s32 {
            if (!o->f) {
                log("es: content %u missing (%s)", index, o->path.c_str());
                return ES_ENOENT;
            }
            u32 cfd = next_cfd++;
            contents[cfd] = {o->f, index, 0, o->size};
            lstats_content_open(index, o->names);
            log("es: open content %u -> cfd %u", index, cfd);
            return (s32)cfd;
        };
        if (ios_async_call(cur_request, work, finish, g_lstats ? "es open " + o->path : std::string())) {
            pending = true;
            return 0;
        }
        work();
        return finish(0);
    }

    bool reply_now() override { return !pending; }

    s32 ioctlv(u32 cmd, const std::vector<IoVec>& in, const std::vector<IoVec>& io) override {
        pending = false;
        switch (cmd) {
        case 0x20:  // GetTitleID
            store64(io[0].addr, TITLE_ID);
            return IOS_OK;
        case 0x07:  // GetDeviceID
            store32(io[0].addr, 0x0403AC68);
            return IOS_OK;
        case 0x1D: {  // GetTitleDir (data directory)
            u64 tid = load64(in[0].addr);
            char path[64];
            snprintf(path, sizeof(path), "/title/%08x/%08x/data", (u32)(tid >> 32), (u32)tid);
            write_g(io[0].addr, path, (u32)strlen(path) + 1);
            return IOS_OK;
        }
        case 0x09:  // OpenContent (current title): in[0] = index
            return open_content(load32(in[0].addr));
        case 0x24:  // OpenTitleContent: title id, ticket view, index
            return open_content(load32(in[2].addr));
        case 0x0A: {  // ReadContent: in[0] = cfd, out = buffer
            auto it = contents.find(load32(in[0].addr));
            if (it == contents.end())
                return ES_EINVAL;
            ContentHandle& c = it->second;
            u64 at = c.pos;
            u32 n = (u32)std::min<u64>(io[0].size, c.size > at ? c.size - at : 0);
            c.pos += n;
            static int s_n;
            if (s_n++ < 40 || s_debug)
                log("es: read content cfd %u at %llu: %u bytes -> %u", load32(in[0].addr), (unsigned long long)at,
                    io[0].size, n);
            if (ios_async_read(cur_request, c.f, at, io[0].addr, n, c.index)) {
                pending = true;
                return 0;
            }
            ios_async_drain();
            u64 t0 = g_lstats ? host_time_us() : 0;
            std::vector<u8> tmp(n);
            size_t got = ios_read_at(c.f, at, tmp.data(), n);
            write_g(io[0].addr, tmp.data(), (u32)got);
            if (t0) {
                u64 t1 = host_time_us();
                lstats_read(c.index, at, n, t0, t0, t1, t1);
            }
            return (s32)got;
        }
        case 0x0B: {  // CloseContent: after the reads before it (the I/O thread's jobs run in order)
            auto it = contents.find(load32(in[0].addr));
            if (it == contents.end())
                return ES_EINVAL;
            FILE* f = it->second.f;
            contents.erase(it);
            if (ios_async_call(cur_request, [f] {
                    fclose(f);
                    return (s32)IOS_OK;
                }, nullptr, "es close")) {
                pending = true;
                return 0;
            }
            ios_async_drain();
            fclose(f);
            return IOS_OK;
        }
        case 0x23: {  // SeekContent: cfd, offset, whence
            auto it = contents.find(load32(in[0].addr));
            if (it == contents.end())
                return ES_EINVAL;
            s32 off = (s32)load32(in[1].addr);
            u32 whence = load32(in[2].addr);
            ContentHandle& c = it->second;
            s64 to = (s64)(whence == 0 ? 0 : whence == 1 ? c.pos : c.size) + off;
            if (to >= 0)  // (a seek before the start fails and leaves the position)
                c.pos = (u64)to;
            static int s_n;
            if (s_n++ < 40 || s_debug)
                log("es: seek content cfd %u to %d (whence %u) -> %llu", load32(in[0].addr), off, whence,
                    (unsigned long long)c.pos);
            return (s32)c.pos;
        }
        case 0x16:  // GetConsumption: no limits
            if (io.size() > 1)
                store32(io[1].addr, 0);
            return IOS_OK;
        case 0x12:  // GetViewCnt (tickets of a title)
            store32(io[0].addr, load64(in[0].addr) == TITLE_ID ? 1 : 0);
            return IOS_OK;
        case 0x13:  // GetViews: title id, count -> views
            if (load64(in[0].addr) != TITLE_ID)
                return ES_ENOENT;
            write_ticket_view(io[0].addr);
            return IOS_OK;
        case 0x1B:  // DIGetTicketView: the running title's ticket view
            write_ticket_view(io[0].addr);
            return IOS_OK;
        case 0x0E:  // GetTitleCnt
        case 0x0C:  // GetOwnedTitleCnt
            store32(io[0].addr, 1);
            return IOS_OK;
        case 0x0F:  // GetTitles
        case 0x0D:
            store64(io[0].addr, TITLE_ID);
            return IOS_OK;
        default:
            return IosHandle::ioctlv(cmd, in, io);
        }
    }
};

std::unique_ptr<IosHandle> es_open_device() { return std::make_unique<EsDevice>(); }

// ---------------------------------------------------------------------------- /dev/stm
class StmDevice;
static StmDevice* s_event_hook;  // the event hook device, while it has a request waiting

class StmDevice : public IosHandle {
public:
    bool eventhook = false;
    bool pending = false;
    u32 hook = 0, hook_out = 0;  // the event hook's request and its output buffer, while waiting
    ~StmDevice() override {
        if (s_event_hook == this)
            s_event_hook = nullptr;
    }
    s32 ioctl(u32 cmd, u32 in, u32 in_len, u32 out, u32 out_len) override {
        pending = false;
        if (eventhook && cmd == 0x1000) {
            pending = true;  // answered when the power button is pressed (stm_power_event)
            hook = cur_request, hook_out = out, s_event_hook = this;
            return IOS_OK;
        }
        switch (cmd) {
        case 0x2001:  // hot reset (OSReturnToMenu, OSRestart)
        case 0x2002:  // hot reset for power down
            platform_quit("the game reset the console (back to the menu)");
            return IOS_OK;
        case 0x2003:  // shut down
        case 0x2004:  // idle (standby with WiiConnect24)
            platform_quit("the game turned the console off");
            return IOS_OK;
        case 0x3002:  // release the event hook (its request is left unanswered)
            if (s_event_hook)
                s_event_hook->hook = 0, s_event_hook = nullptr;
            return IOS_OK;
        case 0x3001:  // get idle mode
        case 0x5001:  // VI dimming
        case 0x6001:  // LED flash
        case 0x6002:  // LED mode
            return IOS_OK;
        default:
            return IosHandle::ioctl(cmd, in, in_len, out, out_len);
        }
    }
    bool reply_now() override { return !pending; }
};

std::unique_ptr<IosHandle> stm_open_device(const std::string& path) {
    auto h = std::make_unique<StmDevice>();
    h->eventhook = path == "/dev/stm/eventhook";
    return h;
}

// The power button: the event hook answers with event 0x800, and the SDK calls the game's power
// callback (OSSetPowerCallback), which ends the game as it would end on the Wii.
bool stm_power_event() {
    StmDevice* d = s_event_hook;
    if (!d || !d->hook)
        return false;
    log("stm: the power button");
    store32(d->hook_out, 0x800);
    ios_complete(d->hook, IOS_OK);
    d->hook = 0, s_event_hook = nullptr;
    return true;
}

}  // namespace guest
