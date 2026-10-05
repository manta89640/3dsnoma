// The island's package and collision, off the guest's thread (island_load.h).
#include "engine/wuhu/island_load.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace engine::wuhu {

namespace fmt = engine::g3d::fmt;
using engine::g3d::Package;

namespace {

// the file is read in pieces this big (progress, and the SD card's own pace)
const u32 PIECE = 256 << 10;
const char* const STAGE_COMMAND = "tools/wuhu/stage_wuhu.py DATA --bake build-host/ng3d-bake";

// How many textures still hold GX texels (fmt::TEX_GX_RAW), from the section table alone (what is
// wrong with a damaged file Package::load says).
u32 gx_raw_textures(const u8* d, u32 size) {
    fmt::Header h;
    if (size < sizeof(h))
        return 0;
    std::memcpy(&h, d, sizeof(h));
    if (h.magic != fmt::MAGIC)
        return 0;
    for (u32 i = 0; i < h.n_sections; i++) {
        const u64 at = sizeof(h) + (u64)i * sizeof(fmt::Section);
        if (at + sizeof(fmt::Section) > size)
            break;
        fmt::Section s;
        std::memcpy(&s, d + at, sizeof(s));
        if (s.tag != fmt::SEC_TEXS)
            continue;
        u32 n = 0;
        for (u32 t = 0; t < s.count; t++) {
            const u64 o = (u64)s.offset + (u64)t * sizeof(fmt::Texture);
            if (o + sizeof(fmt::Texture) > size)
                break;
            n += d[o + offsetof(fmt::Texture, flags)] & fmt::TEX_GX_RAW;
        }
        return n;
    }
    return 0;
}

f32 ms_since(u64 t0) { return (f32)(guest::host_time_us() - t0) / 1000.0f; }

}  // namespace

PackageLoad::PackageLoad(std::string path) : path_(std::move(path)) {}

PackageLoad::~PackageLoad() {
    while (started_ && !done())
        guest::host_sleep_us(1000);
}

void PackageLoad::start() {
    if (started_)
        return;
    started_ = true;
    if (!guest::platform_worker_start(&PackageLoad::run, this)) {
        guest::log("wuhu: no worker thread: the island is read on the game's thread");
        work();
    }
}

void PackageLoad::run(void* self) { ((PackageLoad*)self)->work(); }

// (guest::platform_worker_pause_point between pieces: the app ending stops the worker there)
void PackageLoad::work() {
    guest::platform_worker_pause_point();
    u64 t0 = guest::host_time_us();
    u8* data = nullptr;
    u32 size = 0;
    FILE* f = std::fopen(path_.c_str(), "rb");
    if (!f) {
        error = "there is no " + path_ + " (stage it: " + STAGE_COMMAND + ")";
    } else {
        std::fseek(f, 0, SEEK_END);
        long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        size = n > 0 ? (u32)n : 0;
        total_.store(size, std::memory_order_relaxed);
        data = size ? (u8*)std::malloc(size) : nullptr;
        if (!data) {
            error = path_ + (size ? ": not enough memory for it" : ": empty");
        } else {
            u32 at = 0;
            while (at < size) {
                size_t want = std::min(PIECE, size - at), got = std::fread(data + at, 1, want, f);
                at += (u32)got;
                read_.store(at, std::memory_order_relaxed);
                if (got != want)
                    break;
                guest::platform_worker_pause_point();
            }
            if (at != size)
                error = path_ + ": read " + std::to_string(at) + " of " + std::to_string(size) + " bytes";
        }
        std::fclose(f);
    }
    read_ms = ms_since(t0);

    guest::platform_worker_pause_point();
    u64 t1 = guest::host_time_us();
    if (error.empty()) {
        if (u32 raw = gx_raw_textures(data, size)) {
            error = path_ + " isn't baked (" + std::to_string(raw) + " textures still GX's): stage it again with " +
                    STAGE_COMMAND;
        } else {
            Package* p = Package::load(data, size, &error);  // (takes the block, even when it refuses it)
            data = nullptr;
            if (p)
                package.reset(p);
            else
                error = path_ + ": " + error;
        }
    }
    std::free(data);
    if (package && !(package->env().mii_units_per_unit > 0)) {
        error = path_ + ": its environment gives no k (Mii units per island unit)";
        package.reset();
    }
    check_ms = ms_since(t1);

    guest::platform_worker_pause_point();
    u64 t2 = guest::host_time_us();
    if (package) {
        auto w = std::make_unique<engine::collision::World>();
        bool ok = true;
        for (const fmt::Collision& c : package->collisions())
            ok &= w->add_kcl(package->at(c.data), c.size, package->str(c.name));
        for (const fmt::Cylinder& c : package->cylinders())
            w->add_cylinder({c.x, c.z, c.y0, c.y1, c.radius});
        w->finish();
        if (!ok || w->triangles() == 0) {
            error = path_ + ": its collision can't be read";
            package.reset();
        } else {
            collision = std::move(w);
        }
    }
    collision_ms = ms_since(t2);
    done_.store(true, std::memory_order_release);
}

}  // namespace engine::wuhu
