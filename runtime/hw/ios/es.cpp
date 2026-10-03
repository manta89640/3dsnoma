// /dev/es: title information and the channel's own contents (what the CNT library reads).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>

#include "ios.h"

namespace guest {

static const u64 TITLE_ID = 0x000100014843494Aull;  // 00010001-HCIJ
static const bool s_debug = getenv("NOMA_DEBUG_ES") != nullptr;

struct ContentHandle {
    FILE* f = nullptr;
    u32 index = 0;
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

    ~EsDevice() override {
        for (auto& c : contents)
            if (c.second.f)
                fclose(c.second.f);
    }

    s32 open_content(u32 index) {
        char name[64];
        snprintf(name, sizeof(name), "/%08x.app", index);
        FILE* f = fopen((content_dir() + name).c_str(), "rb");
        if (!f) {
            log("es: content %u missing (%s%s)", index, content_dir().c_str(), name);
            return ES_ENOENT;
        }
        u32 cfd = next_cfd++;
        contents[cfd] = {f, index};
        log("es: open content %u -> cfd %u", index, cfd);
        return (s32)cfd;
    }

    s32 ioctlv(u32 cmd, const std::vector<IoVec>& in, const std::vector<IoVec>& io) override {
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
            std::vector<u8> tmp(io[0].size);
            long at = ftell(it->second.f);
            size_t n = fread(tmp.data(), 1, io[0].size, it->second.f);
            write_g(io[0].addr, tmp.data(), (u32)n);
            static int s_n;
            if (s_n++ < 40 || s_debug)
                log("es: read content cfd %u at %ld: %u bytes -> %zu", load32(in[0].addr), at, io[0].size, n);
            return (s32)n;
        }
        case 0x0B: {  // CloseContent
            auto it = contents.find(load32(in[0].addr));
            if (it == contents.end())
                return ES_EINVAL;
            fclose(it->second.f);
            contents.erase(it);
            return IOS_OK;
        }
        case 0x23: {  // SeekContent: cfd, offset, whence
            auto it = contents.find(load32(in[0].addr));
            if (it == contents.end())
                return ES_EINVAL;
            s32 off = (s32)load32(in[1].addr);
            u32 whence = load32(in[2].addr);
            fseek(it->second.f, off, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END);
            static int s_n;
            if (s_n++ < 40 || s_debug)
                log("es: seek content cfd %u to %d (whence %u) -> %ld", load32(in[0].addr), off, whence, ftell(it->second.f));
            return (s32)ftell(it->second.f);
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
class StmDevice : public IosHandle {
public:
    bool eventhook = false;
    bool pending = false;
    s32 ioctl(u32 cmd, u32 in, u32 in_len, u32 out, u32 out_len) override {
        pending = false;
        if (eventhook && cmd == 0x1000) {
            pending = true;  // answered when the power/reset button is pressed (never here)
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
        case 0x3001:  // get idle mode
        case 0x3002:  // release the event hook
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

}  // namespace guest
