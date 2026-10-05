// IOS request dispatch: the fd table, IOS_Open path routing, and the request/reply format
//   0x00 command (1 open, 2 close, 3 read, 4 write, 5 seek, 6 ioctl, 7 ioctlv; 8 = reply)
//   0x04 result   0x08 fd   0x0C.. arguments (physical addresses)
#include "ios.h"
#include "../../load_stats.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#include <array>
#include <atomic>
#include <functional>
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
    lstats_init();
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

// ---- the I/O thread: what touches the SD card (the contents' reads, the NAND's files) is done
// there while the guest runs on, as IOS works beside the Wii's CPU: the request stays pending and
// only the guest thread that sent it waits. The jobs run in the order they came (IOS's file
// system is one thread too), and the replies go out from the device poll in that order.
namespace {
struct IoJob {
    // a content read: n bytes of f at off into guest memory (host address dst)
    FILE* f;
    u64 off;
    u8* dst;
    u32 n;
    u32 request;
    s32 result;
    u32 tag;                 // (NOMA_LOAD_STATS) the content index
    u64 queued, start, end;  // (NOMA_LOAD_STATS) host_time_us
    // or any work (f null): on the I/O thread, then `finish` on the CPU's thread with its result,
    // before the reply (which gets what `finish` returns)
    std::function<s32()> work;
    std::function<s32(s32)> finish;
    std::string what;  // (NOMA_LOAD_STATS) what it is, for a slow job's line
};
const u32 IO_JOBS = 64;
IoJob s_jobs[IO_JOBS];
std::atomic<u32> s_job_put{0}, s_job_done{0};  // queued (CPU's thread), finished (I/O thread)
u32 s_job_replied;                              // answered (CPU's thread)
HostSignal *s_io_wake, *s_io_done;
int s_io_state;  // 0: not started, 1: running, -1: none (reads are done at once)

void io_thread();
}  // namespace

// NOMA_READ_MBPS=N: reads take as long as at N MB/s, NOMA_READ_US=N: N microseconds more each (the
// 3DS's SD card on a fast host: Azahar's model of it, from hardware, is about 5.5 MB/s and 520 us)
size_t ios_read_at(FILE* f, u64 off, void* dst, u32 n) {
    static int s_mbps = getenv("NOMA_READ_MBPS") ? atoi(getenv("NOMA_READ_MBPS")) : 0;
    static int s_us = getenv("NOMA_READ_US") ? atoi(getenv("NOMA_READ_US")) : 0;
    u64 t0 = s_mbps > 0 || s_us > 0 ? host_time_us() : 0;
    size_t got = fseek(f, (long)off, SEEK_SET) == 0 ? fread(dst, 1, n, f) : 0;
    if (t0) {
        u64 until = t0 + (s_mbps > 0 ? (u64)n / (u64)s_mbps : 0) + (u64)s_us;  // (bytes / (MB/s) = us)
        if (host_time_us() < until)
            host_sleep_us(until - host_time_us());
    }
    return got;
}

// ---- small files read whole: CNT reads a compressed file's 32-byte header first (its size) and then
// the file from its start; csv files come 32 bytes at a time. Each was an SD access (~0.5 ms of
// latency in Azahar's hardware-derived model): when a small read begins a small file of an archive,
// the whole file is read at once and what follows of it is copied from there. The archives' tables
// come from the guest's own read of them (a content's first bytes). The I/O thread's alone.
namespace {
const u32 WHOLE_MAX = 32 << 10, WHOLE_ASK_MAX = 32;
struct ArcEntry {
    u32 off, size;
};
std::map<u32, std::vector<ArcEntry>> s_arc;  // content index -> its files by offset
struct Whole {
    u32 content = ~0u, off = 0, size = 0;
    std::vector<u8> data;
};
Whole s_whole[4];
u32 s_whole_next;

void arc_note(u32 content, const u8* d, u32 n) {  // a content's first n bytes, read for the guest
    auto be = [](const u8* p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; };
    if (n < 16 || be(d) != 0x55AA382D || s_arc.count(content))
        return;
    u32 root = be(d + 4), size = be(d + 8);
    if ((u64)root + size > n || size < 12)
        return;
    const u8* t = d + root;
    u32 count = be(t + 8);
    if ((u64)count * 12 > size)
        return;
    std::vector<ArcEntry>& v = s_arc[content];
    for (u32 i = 1; i < count; i++)
        if (t[i * 12] == 0)
            v.push_back({be(t + i * 12 + 4), be(t + i * 12 + 8)});
    std::sort(v.begin(), v.end(), [](const ArcEntry& a, const ArcEntry& b) { return a.off < b.off; });
}

// a content read: from a file read whole (true), or the read itself that starts one
bool whole_read(IoJob& j) {
    for (Whole& w : s_whole)
        if (w.content == j.tag && j.off >= w.off && j.off + j.n <= (u64)w.off + w.size) {
            ::memcpy(j.dst, w.data.data() + (j.off - w.off), j.n);
            j.result = (s32)j.n;
            return true;
        }
    auto it = s_arc.find(j.tag);
    if (j.n > WHOLE_ASK_MAX || it == s_arc.end())
        return false;
    const std::vector<ArcEntry>& v = it->second;
    auto e = std::lower_bound(v.begin(), v.end(), j.off, [](const ArcEntry& a, u64 off) { return a.off < off; });
    if (e == v.end() || e->off != j.off || e->size <= j.n || e->size > WHOLE_MAX)
        return false;
    // (to the archive's 32-byte alignment: CNT reads a file's size rounded up so)
    u32 size = (e->size + 31) & ~31u;
    Whole& w = s_whole[s_whole_next++ % 4];
    w.data.resize(size);
    u32 got = (u32)ios_read_at(j.f, e->off, w.data.data(), size);
    if (got < j.n) {
        w.content = ~0u;
        return false;
    }
    w.content = j.tag, w.off = e->off, w.size = got;
    ::memcpy(j.dst, w.data.data(), j.n);
    j.result = (s32)j.n;
    return true;
}

std::atomic<bool> s_io_park{false}, s_io_parked{false};  // (ios_async_park)

void io_thread() {
    for (;;) {
        u32 d = s_job_done.load(std::memory_order_relaxed);
        if (d == s_job_put.load(std::memory_order_acquire)) {
            if (s_io_park.load(std::memory_order_acquire) && d == s_job_put.load(std::memory_order_acquire)) {
                s_io_parked.store(true, std::memory_order_release);
                for (;;)
                    host_sleep_us(1000000000ull);
            }
            platform_wait(s_io_wake, 100000);
            platform_log_idle();
            continue;
        }
        IoJob& j = s_jobs[d % IO_JOBS];
        if (g_lstats)
            j.start = host_time_us();
        if (!j.f) {
            j.result = j.work();
        } else if (!whole_read(j)) {
            j.result = (s32)ios_read_at(j.f, j.off, j.dst, j.n);
            if (j.off == 0 && j.result > 0)
                arc_note(j.tag, j.dst, (u32)j.result);
        }
        if (g_lstats) {
            j.end = host_time_us();
            // NOMA_LOAD_STATS_JOBS=1: each job of 5 ms or more (a line costs ~2 ms in Azahar, which
            // the I/O thread takes from the guest's: they share a core)
            static bool s_jobs_log = getenv("NOMA_LOAD_STATS_JOBS") != nullptr;
            if (s_jobs_log && j.end - j.start >= 5000)
                log("lstats: io job %s took %.1f ms", j.f ? "(content read)" : j.what.c_str(),
                    (j.end - j.start) / 1000.0);
        }
        s_job_done.store(d + 1, std::memory_order_release);
        platform_signal(s_io_done);
    }
}
}  // namespace

static bool io_thread_up() {
    if (s_io_state == 0) {
        s_io_wake = platform_signal_new(), s_io_done = platform_signal_new();
        s_io_state = platform_io_thread_start(io_thread) ? 1 : -1;
    }
    return s_io_state > 0;
}

// the next job's slot (once one is free: replies sent)
static IoJob& next_job() {
    u32 put = s_job_put.load(std::memory_order_relaxed);
    while (put - s_job_replied >= IO_JOBS)
        ios_async_wait(1000), ios_async_poll();
    return s_jobs[put % IO_JOBS];
}

static void queue_job() {
    s_job_put.store(s_job_put.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    platform_signal(s_io_wake);
}

bool ios_async_read(u32 request_phys, FILE* f, u64 off, u32 addr, u32 n, u32 tag) {
    u8* p = host_ptr(addr);
    if (!io_thread_up() || !n || !p || host_ptr(addr + n - 1) != p + n - 1)
        return false;
    IoJob& j = next_job();
    j.f = f, j.off = off, j.dst = p, j.n = n, j.request = request_phys, j.result = 0, j.tag = tag;
    j.queued = g_lstats ? host_time_us() : 0;
    j.work = nullptr, j.finish = nullptr;
    queue_job();
    return true;
}

bool ios_async_call(u32 request_phys, std::function<s32()> work, std::function<s32(s32)> finish,
                    const std::string& what) {
    if (!io_thread_up())
        return false;
    IoJob& j = next_job();
    j.f = nullptr, j.off = 0, j.dst = nullptr, j.n = 0, j.request = request_phys, j.result = 0, j.tag = 0;
    j.queued = g_lstats ? host_time_us() : 0;
    j.work = std::move(work), j.finish = std::move(finish);
    j.what = g_lstats ? what : std::string();
    queue_job();
    return true;
}

void ios_async_poll() {
    u32 done = s_job_done.load(std::memory_order_acquire);
    for (; s_job_replied != done; s_job_replied++) {
        IoJob& j = s_jobs[s_job_replied % IO_JOBS];
        s32 result = j.finish ? j.finish(j.result) : j.result;
        ios_complete(j.request, result);
        if (g_lstats) {
            if (j.f)
                lstats_read(j.tag, j.off, j.n, j.queued, j.start, j.end, host_time_us());
            else
                lstats_job(j.queued, j.start, j.end, host_time_us());
        }
        j.work = nullptr, j.finish = nullptr;  // (what they hold goes now)
    }
}

bool ios_async_busy() { return s_job_replied != s_job_put.load(std::memory_order_relaxed); }

void ios_async_wait(u64 us) { platform_wait(s_io_done, us); }

void ios_async_drain() {
    while (ios_async_busy())
        ios_async_wait(1000), ios_async_poll();
}

bool ios_async_park(u64 timeout_us) {
    if (s_io_state <= 0)
        return true;
    const u64 t0 = host_time_us();
    while (ios_async_busy()) {
        if (host_time_us() - t0 > timeout_us)
            return false;
        ios_async_wait(1000), ios_async_poll();
    }
    s_io_park.store(true, std::memory_order_release);
    platform_signal(s_io_wake);
    while (!s_io_parked.load(std::memory_order_acquire)) {
        if (host_time_us() - t0 > timeout_us)
            return false;
        host_sleep_us(100);
    }
    return true;
}

// a handle's command (read, write, seek, ioctl, ioctlv; close is the caller's), with its arguments
// from the request; *now: false when the device answers later (ios_complete)
static s32 run_command(IosHandle* h, u32 cmd, const u32* a, bool* now) {
    *now = true;
    switch (cmd) {
    case 3:
        return h->read(phys_to_virt(a[0]), a[1]);
    case 4:
        return h->write(phys_to_virt(a[0]), a[1]);
    case 5:
        return h->seek((s32)a[0], a[1]);
    case 6: {
        s32 r = h->ioctl(a[0], phys_to_virt(a[1]), a[2], phys_to_virt(a[3]), a[4]);
        *now = h->reply_now();
        return r;
    }
    case 7: {
        u32 nin = a[1], nio = a[2], vecs = phys_to_virt(a[3]);
        std::vector<IoVec> in, io;
        for (u32 i = 0; i < nin + nio; i++) {
            IoVec v{phys_to_virt(load32(vecs + i * 8)), load32(vecs + i * 8 + 4)};
            (i < nin ? in : io).push_back(v);
        }
        s32 r = h->ioctlv(a[0], in, io);
        *now = h->reply_now();
        return r;
    }
    default:
        log("ios: unknown command %u", cmd);
        return IOS_EINVAL;
    }
}

// an opened handle into the fd table: the reply's value
static s32 opened(const std::string& path, u32 mode, std::unique_ptr<IosHandle> h, s32 err) {
    s32 result;
    if (h) {
        h->name = path;
        result = s_next_fd++;
        s_fds[result] = std::move(h);
    } else {
        result = err ? err : IOS_ENOENT;
    }
    log("ios: open %s (mode %u) -> %d", path.c_str(), mode, result);
    // NOMA_TRACE_OPEN=path (trace builds): print the recent calls when the guest opens it
    static const char* s_trace_open = getenv("NOMA_TRACE_OPEN");
    if (s_trace_open && path == s_trace_open)
        noma_trace_dump_global();
    // NOMA_DUMP_MEM_OPEN=path (host): write mem1.bin/mem2.bin each time the guest opens it
    static const char* s_dump_open = getenv("NOMA_DUMP_MEM_OPEN");
    if (s_dump_open && path == s_dump_open)
        memory_dump();
    return result;
}

void ios_execute(u32 request_phys) {
    NOMA_PROF_SCOPE("(ios)");
    // NOMA_LOAD_STATS: a request that keeps the CPU's thread (every guest thread) waiting 5 ms or more
    struct Slow {
        u32 req;
        u64 t0 = g_lstats ? host_time_us() : 0;
        std::string what;
        ~Slow() {
            if (t0 && host_time_us() - t0 >= 5000)
                log("lstats: ios request %s took %.1f ms (all guest threads waited)", what.c_str(),
                    (host_time_us() - t0) / 1000.0);
        }
    } slow{request_phys};
    u32 req = phys_to_virt(request_phys);
    u32 cmd = load32(req);
    s32 fd = (s32)load32(req + 0x08);
    u32 a[5];
    for (int i = 0; i < 5; i++)
        a[i] = load32(req + 0x0C + i * 4);
    static bool s_debug = getenv("NOMA_DEBUG_IOS") != nullptr;

    if (cmd == 1) {  // open
        std::string path = read_string(phys_to_virt(a[0]), 64);
        if (slow.t0)
            slow.what = "open " + path;
        u32 mode = a[1];
        // a NAND file: on the I/O thread (fs.cpp), but for /tmp's (in memory) when no job waits
        if (path.rfind("/dev/", 0) != 0 && (ios_async_busy() || !fs_in_memory(path))) {
            struct Opened {
                std::unique_ptr<IosHandle> h;
                s32 err = 0;
            };
            auto o = std::make_shared<Opened>();
            if (ios_async_call(request_phys, [o, path, mode] {
                    o->h = fs_open_file(path, mode, &o->err);
                    return 0;
                }, [o, path, mode](s32) { return opened(path, mode, std::move(o->h), o->err); }, slow.what))
                return;
        }
        s32 err;
        std::unique_ptr<IosHandle> h = open_path(path, mode, &err);
        reply(req, opened(path, mode, std::move(h), err));
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
    if (slow.t0) {
        char what[96];
        snprintf(what, sizeof(what), "%s cmd %u (%08X %08X %08X)", h->name.c_str(), cmd, a[0], a[1], a[2]);
        slow.what = what;
    }
    if (h->host_io() && (ios_async_busy() || !h->in_memory())) {
        // the file system's handles: every command on the I/O thread, after what came before (a
        // file in memory's only while jobs wait); a closed handle leaves the table now and goes
        // with its job (the last reference: closed and freed there)
        std::shared_ptr<IosHandle> closing;
        if (cmd == 2) {
            closing.reset(it->second.release());
            s_fds.erase(it);
        }
        std::array<u32, 5> args{a[0], a[1], a[2], a[3], a[4]};
        if (ios_async_call(request_phys, [h, closing = std::move(closing), cmd, args]() mutable {
                bool now;
                s32 r = cmd == 2 ? h->close() : run_command(h, cmd, args.data(), &now);
                closing.reset();
                return r;
            }, [cmd, args, name = h->name](s32 r) {
                if (s_debug && cmd >= 3)
                    log("ios: %s: cmd %u (%08X %08X %08X) -> %d", name.c_str(), cmd, args[0], args[1], args[2], r);
                return r;
            }, slow.what))
            return;
        if (cmd == 2) {  // (no I/O thread: the handle went with the job, freed unrun; their close() is IOS_OK)
            reply(req, IOS_OK);
            return;
        }
    }
    s32 result;
    bool now = true;
    if (cmd == 2) {  // close
        result = h->close();
        s_fds.erase(it);
    } else {
        result = run_command(h, cmd, a, &now);
    }
    if (s_debug && cmd >= 3)
        log("ios: %s: cmd %u (%08X %08X %08X) -> %d%s", h->name.c_str(), cmd, a[0], a[1], a[2], result,
            now ? "" : " (pending)");
    if (now)
        reply(req, result);
    // else it stays pending: the device answers later through ios_complete
}

}  // namespace guest
