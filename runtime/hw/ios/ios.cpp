// IOS request dispatch: the fd table, IOS_Open path routing, and the request/reply format
//   0x00 command (1 open, 2 close, 3 read, 4 write, 5 seek, 6 ioctl, 7 ioctlv; 8 = reply)
//   0x04 result   0x08 fd   0x0C.. arguments (physical addresses)
#include "ios.h"

#include <cstdlib>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#include <atomic>
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

// ---- the I/O thread: reads while the guest runs on, as IOS reads beside the Wii's CPU. The
// requests stay pending; the replies go out from the device poll, in the order of the reads.
namespace {
struct IoJob {
    FILE* f;
    u64 off;
    u8* dst;
    u32 n;
    u32 request;
    s32 result;
};
const u32 IO_JOBS = 64;
IoJob s_jobs[IO_JOBS];
std::atomic<u32> s_job_put{0}, s_job_done{0};  // queued (CPU's thread), finished (I/O thread)
u32 s_job_replied;                              // answered (CPU's thread)
HostSignal *s_io_wake, *s_io_done;
int s_io_state;  // 0: not started, 1: running, -1: none (reads are done at once)

void io_thread();
}  // namespace

// NOMA_READ_MBPS=N: reads take as long as at N MB/s (the 3DS's SD card on a fast host)
size_t ios_read_at(FILE* f, u64 off, void* dst, u32 n) {
    static int s_mbps = getenv("NOMA_READ_MBPS") ? atoi(getenv("NOMA_READ_MBPS")) : 0;
    u64 t0 = s_mbps > 0 ? host_time_us() : 0;
    size_t got = fseek(f, (long)off, SEEK_SET) == 0 ? fread(dst, 1, n, f) : 0;
    if (s_mbps > 0) {
        u64 until = t0 + (u64)n / (u64)s_mbps;  // (bytes / (MB/s) = microseconds)
        if (host_time_us() < until)
            host_sleep_us(until - host_time_us());
    }
    return got;
}

namespace {
void io_thread() {
    for (;;) {
        u32 d = s_job_done.load(std::memory_order_relaxed);
        if (d == s_job_put.load(std::memory_order_acquire)) {
            platform_wait(s_io_wake, 100000);
            continue;
        }
        IoJob& j = s_jobs[d % IO_JOBS];
        j.result = (s32)ios_read_at(j.f, j.off, j.dst, j.n);
        s_job_done.store(d + 1, std::memory_order_release);
        platform_signal(s_io_done);
    }
}
}  // namespace

bool ios_async_read(u32 request_phys, FILE* f, u64 off, u32 addr, u32 n) {
    if (s_io_state == 0) {
        s_io_wake = platform_signal_new(), s_io_done = platform_signal_new();
        s_io_state = platform_io_thread_start(io_thread) ? 1 : -1;
    }
    u8* p = host_ptr(addr);
    if (s_io_state < 0 || !n || !p || host_ptr(addr + n - 1) != p + n - 1)
        return false;
    u32 put = s_job_put.load(std::memory_order_relaxed);
    while (put - s_job_replied >= IO_JOBS)
        ios_async_wait(1000), ios_async_poll();
    s_jobs[put % IO_JOBS] = {f, off, p, n, request_phys, 0};
    s_job_put.store(put + 1, std::memory_order_release);
    platform_signal(s_io_wake);
    return true;
}

void ios_async_poll() {
    u32 done = s_job_done.load(std::memory_order_acquire);
    for (; s_job_replied != done; s_job_replied++) {
        const IoJob& j = s_jobs[s_job_replied % IO_JOBS];
        ios_complete(j.request, j.result);
    }
}

bool ios_async_busy() { return s_job_replied != s_job_put.load(std::memory_order_relaxed); }

void ios_async_wait(u64 us) { platform_wait(s_io_done, us); }

void ios_async_drain() {
    while (ios_async_busy())
        ios_async_wait(1000), ios_async_poll();
}

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
