// Frame-time spikes and loading (see load_stats.h). Everything but lstats_texture runs on the
// CPU's thread (the I/O thread's reads are reported when their replies go out).
#include "load_stats.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "runtime.h"

namespace guest {

bool g_lstats;

namespace {

u64 s_threshold_us = 50000;
u64 s_every_us = 10000000;
bool s_log_reads;

// a content's files (a U8 archive), by offset: which file a read is in
struct ArcFile {
    u32 off, size;
    std::string name;
};
std::map<u32, std::vector<ArcFile>> s_arcs;

const char* file_at(u32 content, u64 off) {
    auto it = s_arcs.find(content);
    if (it == s_arcs.end() || it->second.empty())
        return "?";
    const std::vector<ArcFile>& v = it->second;
    auto f = std::upper_bound(v.begin(), v.end(), (u32)off, [](u32 o, const ArcFile& a) { return o < a.off; });
    if (f == v.begin())
        return "(header)";
    --f;
    return off < (u64)f->off + f->size ? f->name.c_str() : "(between files)";
}

std::vector<ArcFile> read_u8_names(const char* path) {
    std::vector<ArcFile> out;
    FILE* f = fopen(path, "rb");
    if (!f)
        return out;
    u8 h[16];
    auto be = [](const u8* p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; };
    if (fread(h, 1, 16, f) == 16 && be(h) == 0x55AA382D) {
        u32 root = be(h + 4), size = be(h + 8);
        std::vector<u8> t(size);
        if (size < (16u << 20) && fseek(f, (long)root, SEEK_SET) == 0 && fread(t.data(), 1, size, f) == size) {
            u32 n = be(t.data() + 8);
            const char* names = (const char*)t.data() + n * 12;
            // directories: a node index where each ends, to build paths
            std::vector<std::pair<u32, std::string>> dirs{{n, ""}};
            for (u32 i = 1; i < n && i * 12 + 12 <= size; i++) {
                while (dirs.size() > 1 && i >= dirs.back().first)
                    dirs.pop_back();
                const u8* e = t.data() + i * 12;
                u32 name_off = be(e) & 0xFFFFFF;
                std::string name = n * 12 + name_off < size ? names + name_off : "?";
                std::string full = dirs.back().second + "/" + name;
                if (e[0] == 1)
                    dirs.push_back({be(e + 8), full});
                else
                    out.push_back({be(e + 4), be(e + 8), full});
            }
        }
    }
    fclose(f);
    std::sort(out.begin(), out.end(), [](const ArcFile& a, const ArcFile& b) { return a.off < b.off; });
    return out;
}

struct CxStat {
    u32 calls = 0, in = 0, out = 0;
    u64 us = 0;
    void add(u32 i, u32 o, u64 u) { calls++, in += i, out += o, us += u; }
};

// what happened in one window (a frame, a burst)
struct Window {
    u64 t0 = 0;
    u32 reads = 0, read_bytes = 0;
    u64 read_wait = 0, read_wait_max = 0, read_io = 0;
    CxStat lz, hf;
    u32 jobs = 0;  // other I/O jobs (the file system's requests, contents opened and closed)
    u64 job_io = 0, job_wait = 0;
    u32 allocs = 0, alloc_bytes = 0;
    u32 tex = 0, tex_bytes = 0;
    u64 tex_us = 0;
    std::vector<std::string> files;  // the first few files read
    u32 file_count = 0;
    void file(const char* name) {
        for (const std::string& f : files)
            if (f == name)
                return;
        file_count++;
        if (files.size() < 3)
            files.push_back(name);
    }
    std::string files_text() const {
        std::string s;
        for (const std::string& f : files)
            s += (s.empty() ? "" : " ") + f;
        if (file_count > files.size())
            s += " +" + std::to_string(file_count - files.size());
        return s;
    }
};

struct ThreadTime {
    u32 thread;
    u64 us;
};

Window s_frame;  // the frame being made
std::vector<ThreadTime> s_threads;
u64 s_idle;
u64 s_last_frame;

// texture uploads (from the GPU's thread)
std::atomic<u32> s_tex_n{0}, s_tex_bytes{0};
std::atomic<u64> s_tex_us{0};
u32 s_tex_seen_n, s_tex_seen_bytes;
u64 s_tex_seen_us;

// the loading burst going on
Window s_burst;
bool s_in_burst;
u64 s_burst_last;  // its last read or decompression
u32 s_burst_frames;
u64 s_burst_worst;
u32 s_bursts;

// frame times since the last summary, and all along
std::vector<u32> s_period;
u64 s_period_start;
const u32 BUCKETS[] = {20, 34, 50, 67, 100, 150, 250, 500, 1000};
const u32 NB = sizeof(BUCKETS) / sizeof(BUCKETS[0]) + 1;
u32 s_hist[NB];
u32 s_frames_total;
u64 s_worst_total;
u32 s_worst_frame;

// the decompression running (its own time leaves out other threads' time inside its poll points)
u32 s_cx_thread;
u64 s_cx_other;

u32 current_thread() { return load32(0x800000E4); }

double ms(u64 us) { return (double)us / 1000.0; }

void end_burst() {
    if (!s_in_burst)
        return;
    s_in_burst = false;
    s_bursts++;
    const Window& b = s_burst;
    log("lstats: burst %u at %.2f s: %.0f ms, %u reads %u KB (io %.0f ms, guest waited %.0f ms, longest %.1f), "
        "lz %u KB->%u KB %.0f ms, huffman %u KB->%u KB %.0f ms, fs %u (io %.0f ms, waited %.0f), %u allocs %u KB, "
        "%u textures %u KB %.0f ms; %u frames, worst %.0f ms; %s",
        s_bursts, b.t0 / 1e6, ms(s_burst_last - b.t0), b.reads, b.read_bytes >> 10, ms(b.read_io), ms(b.read_wait),
        ms(b.read_wait_max), b.lz.in >> 10, b.lz.out >> 10, ms(b.lz.us), b.hf.in >> 10, b.hf.out >> 10, ms(b.hf.us),
        b.jobs, ms(b.job_io), ms(b.job_wait), b.allocs, b.alloc_bytes >> 10, b.tex, b.tex_bytes >> 10, ms(b.tex_us),
        s_burst_frames, ms(s_burst_worst), b.files_text().c_str());
}

void activity(u64 at) {
    if (s_in_burst && at > s_burst_last + 1000000)
        end_burst();
    if (!s_in_burst) {
        s_in_burst = true;
        s_burst = Window();
        s_burst.t0 = at;
        s_burst_frames = 0, s_burst_worst = 0;
    }
    s_burst_last = std::max(s_burst_last, at);
}

template <class F>
void both(F f) {
    f(s_frame);
    if (s_in_burst)
        f(s_burst);
}

u32 percentile(std::vector<u32>& v, u32 pct) {
    if (v.empty())
        return 0;
    size_t i = std::min<size_t>(v.size() - 1, v.size() * pct / 100);
    std::nth_element(v.begin(), v.begin() + (long)i, v.end());
    return v[i];
}

void summary(u64 now) {
    std::vector<u32> v = s_period;
    u64 sum = 0;
    u32 worst = 0;
    for (u32 d : v)
        sum += d, worst = std::max(worst, d);
    char hist[160];
    int at = 0;
    for (u32 i = 0; i < NB; i++)
        at += snprintf(hist + at, sizeof(hist) - (size_t)at, "%s%s%u:%u", i ? " " : "", i < NB - 1 ? "<" : ">=",
                       i < NB - 1 ? BUCKETS[i] : BUCKETS[NB - 2], s_hist[i]);
    u32 p50 = percentile(v, 50), p90 = percentile(v, 90), p99 = percentile(v, 99);
    log("lstats: %.0f s: %zu frames %.1f fps, ms p50 %.1f p90 %.1f p99 %.1f max %.1f; all %u frames [%s] worst "
        "%.0f ms (frame %u)",
        (now - s_period_start) / 1e6, v.size(),
        sum ? v.size() * 1e6 / (double)sum : 0.0, p50 / 1000.0, p90 / 1000.0, p99 / 1000.0, worst / 1000.0,
        s_frames_total, hist, ms(s_worst_total), s_worst_frame);
    s_period.clear();
    s_period_start = now;
}

}  // namespace

void lstats_init() {
    const char* e = getenv("NOMA_LOAD_STATS");
    if (!e || !*e || !strcmp(e, "0"))
        return;
    g_lstats = true;
    int t = atoi(e);
    if (t > 1)
        s_threshold_us = (u64)t * 1000;
    if (const char* every = getenv("NOMA_LOAD_STATS_EVERY"))
        s_every_us = (u64)atoi(every) * 1000000;
    s_log_reads = getenv("NOMA_LOAD_STATS_READS") != nullptr;
    s_last_frame = s_period_start = s_frame.t0 = host_time_us();
    log("lstats: frames over %.0f ms logged, a summary every %.0f s", ms(s_threshold_us), s_every_us / 1e6);
}

void* lstats_parse_names(const char* host_path) {
    return g_lstats ? new std::vector<ArcFile>(read_u8_names(host_path)) : nullptr;
}

void lstats_content_open(u32 index, void* names) {
    std::unique_ptr<std::vector<ArcFile>> v((std::vector<ArcFile>*)names);
    if (v && !s_arcs.count(index))
        s_arcs[index] = std::move(*v);
}

void lstats_frame() {
    if (!g_lstats)
        return;
    u64 now = host_time_us();
    u64 dt = now - s_last_frame;
    s_last_frame = now;
    s_frames_total++;
    u32 b = 0;
    while (b < NB - 1 && dt >= BUCKETS[b] * 1000ull)
        b++;
    s_hist[b]++;
    s_period.push_back((u32)dt);
    if (dt > s_worst_total)
        s_worst_total = dt, s_worst_frame = s_frames_total;
    // the GPU thread's uploads since the last frame
    u32 tn = s_tex_n.load(std::memory_order_relaxed), tb = s_tex_bytes.load(std::memory_order_relaxed);
    u64 tu = s_tex_us.load(std::memory_order_relaxed);
    u32 dn = tn - s_tex_seen_n, db = tb - s_tex_seen_bytes;
    u64 du = tu - s_tex_seen_us;
    s_tex_seen_n = tn, s_tex_seen_bytes = tb, s_tex_seen_us = tu;
    both([&](Window& w) { w.tex += dn, w.tex_bytes += db, w.tex_us += du; });
    if (s_in_burst) {
        s_burst_frames++;
        s_burst_worst = std::max(s_burst_worst, dt);
    }
    if (dt >= s_threshold_us) {
        const Window& w = s_frame;
        std::sort(s_threads.begin(), s_threads.end(), [](const ThreadTime& a, const ThreadTime& b) { return a.us > b.us; });
        char th[160];
        int at = 0;
        for (size_t i = 0; i < s_threads.size() && i < 4; i++)
            at += snprintf(th + at, sizeof(th) - (size_t)at, "%s%08X:%.1f", i ? " " : "", s_threads[i].thread,
                           ms(s_threads[i].us));
        th[at] = 0;
        log("lstats: frame %u %.1f ms at %.2f s: threads %s idle %.1f; reads %u %u KB (waited %.1f, io %.1f) lz %u "
            "%u->%u KB %.1f ms, huffman %u %u->%u KB %.1f ms, fs %u (io %.1f, waited %.1f), allocs %u %u KB, tex %u %u KB "
            "%.1f ms; %s",
            s_frames_total, ms(dt), now / 1e6, th, ms(s_idle), w.reads, w.read_bytes >> 10, ms(w.read_wait),
            ms(w.read_io), w.lz.calls, w.lz.in >> 10, w.lz.out >> 10, ms(w.lz.us), w.hf.calls, w.hf.in >> 10,
            w.hf.out >> 10, ms(w.hf.us), w.jobs, ms(w.job_io), ms(w.job_wait), w.allocs, w.alloc_bytes >> 10, w.tex,
            w.tex_bytes >> 10, ms(w.tex_us), w.files_text().c_str());
    }
    profile_frame_end(dt >= s_threshold_us);
    s_frame = Window();
    s_frame.t0 = now;
    s_threads.clear();
    s_idle = 0;
    if (s_in_burst && now > s_burst_last + 1000000)
        end_burst();
    if (now - s_period_start >= s_every_us)
        summary(now);
}

void lstats_thread(u32 thread, u64 us) {
    if (!g_lstats)
        return;
    if (s_cx_thread && thread != s_cx_thread)
        s_cx_other += us;
    for (ThreadTime& t : s_threads)
        if (t.thread == thread) {
            t.us += us;
            return;
        }
    s_threads.push_back({thread, us});
}

void lstats_idle(u64 us) {
    if (!g_lstats)
        return;
    s_idle += us;
    if (s_cx_thread)
        s_cx_other += us;
}

void lstats_read(u32 content, u64 off, u32 n, u64 queued, u64 start, u64 end, u64 replied) {
    if (!g_lstats)
        return;
    activity(queued);
    activity(replied);
    const char* name = file_at(content, off);
    both([&](Window& w) {
        w.reads++, w.read_bytes += n;
        w.read_wait += replied - queued, w.read_io += end - start;
        w.read_wait_max = std::max(w.read_wait_max, replied - queued);
        w.file(name);
    });
    if (s_log_reads)
        log("lstats: read %u @%llu %u B %s: queued %.1f ms, io %.1f ms, replied after %.1f ms", content,
            (unsigned long long)off, n, name, ms(start - queued), ms(end - start), ms(replied - queued));
}

u64 lstats_cx_begin() {
    if (!g_lstats)
        return 0;
    s_cx_thread = current_thread();
    s_cx_other = 0;
    return host_time_us();
}

void lstats_cx_end(u64 t0, bool huffman, u32 in, u32 out) {
    if (!g_lstats || !t0)
        return;
    u64 now = host_time_us(), d = now - t0;
    d = d > s_cx_other ? d - s_cx_other : 0;
    s_cx_thread = 0;
    activity(now);
    both([&](Window& w) { (huffman ? w.hf : w.lz).add(in, out, d); });
}

void lstats_job(u64 queued, u64 start, u64 end, u64 replied) {
    if (!g_lstats)
        return;
    activity(queued);
    activity(replied);
    both([&](Window& w) { w.jobs++, w.job_io += end - start, w.job_wait += replied - queued; });
}

void lstats_alloc(u32 n) {
    if (!g_lstats)
        return;
    both([&](Window& w) { w.allocs++, w.alloc_bytes += n; });
}

void lstats_texture(u32 bytes, u64 us) {
    if (!g_lstats)
        return;
    s_tex_n.fetch_add(1, std::memory_order_relaxed);
    s_tex_bytes.fetch_add(bytes, std::memory_order_relaxed);
    s_tex_us.fetch_add(us, std::memory_order_relaxed);
}

}  // namespace guest
