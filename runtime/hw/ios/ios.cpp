// IOS request dispatch: the fd table, IOS_Open path routing, and the request/reply format
//   0x00 command (1 open, 2 close, 3 read, 4 write, 5 seek, 6 ioctl, 7 ioctlv; 8 = reply)
//   0x04 result   0x08 fd   0x0C.. arguments (physical addresses)
#include "ios.h"

#include <cstdlib>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#include <map>

namespace guest {
void noma_trace_dump_global();
void memory_dump();

static std::map<s32, std::unique_ptr<IosHandle>> s_fds;
static s32 s_next_fd = 1;
static std::string s_nand_root, s_content_dir;

static void remove_tree(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d)
        return;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == "..")
            continue;
        std::string p = dir + "/" + n;
        struct stat st;
        if (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            remove_tree(p);
            rmdir(p.c_str());
        } else {
            unlink(p.c_str());
        }
    }
    closedir(d);
}

void ios_init(const std::string& nand_root, const std::string& content_dir) {
    s_nand_root = nand_root;
    s_content_dir = content_dir;
    // /tmp is in memory (fs.cpp), empty at every start as IOS leaves it; one on the card is from an
    // earlier version
    remove_tree(nand_root + "/tmp");
    rmdir((nand_root + "/tmp").c_str());
}

std::string nand_host_path(const std::string& nand_path) {
    std::string p = nand_path;
    // keep the NAND tree under the root; refuse ".." components
    if (p.find("..") != std::string::npos)
        return s_nand_root + "/__invalid__";
    return s_nand_root + p;
}

const std::string& content_dir() { return s_content_dir; }

s32 IosHandle::ioctl(u32 cmd, u32 in, u32 in_len, u32 out, u32 out_len) {
    log("ios: %s: unhandled ioctl 0x%X (in %u bytes, out %u bytes)", name.c_str(), cmd, in_len, out_len);
    return IOS_EINVAL;
}

s32 IosHandle::ioctlv(u32 cmd, const std::vector<IoVec>& in, const std::vector<IoVec>& io) {
    log("ios: %s: unhandled ioctlv 0x%X (%zu in, %zu io)", name.c_str(), cmd, in.size(), io.size());
    return IOS_EINVAL;
}

static std::unique_ptr<IosHandle> open_path(const std::string& path, u32 mode, s32* err) {
    *err = IOS_OK;
    if (path == "/dev/fs")
        return fs_open_device();
    if (path == "/dev/es")
        return es_open_device();
    if (path == "/dev/di")
        return di_open_device();
    if (path == "/dev/usb/hid")  // USB HID, nothing plugged in
        return usb_hid_open_device();
    if (path.rfind("/dev/stm/", 0) == 0)
        return stm_open_device(path);
    if (path.rfind("/dev/net/", 0) == 0)
        return net_open_device(path, err);
    if (path.rfind("/dev/", 0) == 0) {
        // the other USB devices, SD, AES/SHA, WiFi...: not present
        *err = IOS_ENOENT;
        return nullptr;
    }
    return fs_open_file(path, mode, err);
}

static void reply(u32 req, s32 result) {
    u32 cmd = load32(req);
    store32(req + 0x04, (u32)result);
    store32(req + 0x08, cmd);
    store32(req + 0x00, 8);  // IPC_REPLY
    ipc_reply(req & 0x7FFFFFFF);
}

void ios_complete(u32 request_phys, s32 result) { reply(phys_to_virt(request_phys), result); }

void ios_execute(u32 request_phys) {
    NOMA_PROF_SCOPE("(ios)");
    u32 req = phys_to_virt(request_phys);
    u32 cmd = load32(req);
    s32 fd = (s32)load32(req + 0x08);
    u32 a[5];
    for (int i = 0; i < 5; i++)
        a[i] = load32(req + 0x0C + i * 4);
    s32 result = IOS_EINVAL;
    bool now = true;

    if (cmd == 1) {  // open
        std::string path = read_string(phys_to_virt(a[0]), 64);
        s32 err;
        std::unique_ptr<IosHandle> h = open_path(path, a[1], &err);
        if (h) {
            h->name = path;
            result = s_next_fd++;
            s_fds[result] = std::move(h);
        } else {
            result = err ? err : IOS_ENOENT;
        }
        log("ios: open %s (mode %u) -> %d", path.c_str(), a[1], result);
        // NOMA_TRACE_OPEN=path (trace builds): print the recent calls when the guest opens it
        static const char* s_trace_open = getenv("NOMA_TRACE_OPEN");
        if (s_trace_open && path == s_trace_open)
            noma_trace_dump_global();
        // NOMA_DUMP_MEM_OPEN=path (host): write mem1.bin/mem2.bin each time the guest opens it
        static const char* s_dump_open = getenv("NOMA_DUMP_MEM_OPEN");
        if (s_dump_open && path == s_dump_open)
            memory_dump();
        reply(req, result);
        return;
    }

    auto it = s_fds.find(fd);
    if (it == s_fds.end()) {
        log("ios: command %u on bad fd %d", cmd, fd);
        reply(req, IOS_EINVAL);
        return;
    }
    IosHandle* h = it->second.get();
    h->cur_request = request_phys;
    switch (cmd) {
    case 2:  // close
        result = h->close();
        s_fds.erase(it);
        break;
    case 3:
        result = h->read(phys_to_virt(a[0]), a[1]);
        break;
    case 4:
        result = h->write(phys_to_virt(a[0]), a[1]);
        break;
    case 5:
        result = h->seek((s32)a[0], a[1]);
        break;
    case 6:
        result = h->ioctl(a[0], phys_to_virt(a[1]), a[2], phys_to_virt(a[3]), a[4]);
        now = h->reply_now();
        break;
    case 7: {
        u32 nin = a[1], nio = a[2], vecs = phys_to_virt(a[3]);
        std::vector<IoVec> in, io;
        for (u32 i = 0; i < nin + nio; i++) {
            IoVec v{phys_to_virt(load32(vecs + i * 8)), load32(vecs + i * 8 + 4)};
            (i < nin ? in : io).push_back(v);
        }
        result = h->ioctlv(a[0], in, io);
        now = h->reply_now();
        break;
    }
    default:
        log("ios: unknown command %u", cmd);
        break;
    }
    static bool s_debug = getenv("NOMA_DEBUG_IOS") != nullptr;
    if (s_debug && cmd >= 3)
        log("ios: %s: cmd %u (%08X %08X %08X) -> %d%s", h->name.c_str(), cmd, a[0], a[1], a[2], result,
            now ? "" : " (pending)");
    if (now)
        reply(req, result);
    // else it stays pending: the device answers later through ios_complete
}

}  // namespace guest
