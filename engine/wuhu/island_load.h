// The island's package and collision, made off the guest's thread (docs/wuhu_design.md §5): the
// file, 13 MB, would take the SD card some seconds on the 3DS, and the collision some tens of
// milliseconds to build; on the guest's thread either would stop the game (its sound with it) for
// that long. A worker (platform_worker_start) reads the file in pieces (progress()), checks that it is
// baked, loads it (engine/g3d/package.h: checked and indexed) and builds the collision from its KCL
// files and tree trunks (engine/collision/kcl.h); the guest's thread asks done() once a frame and
// then takes what was made.
//
// A package still holding GX textures (made without --bake) is refused: converting them here would run
// pica_tex.h's work tables on this thread while the 3DS's GPU thread uses them for its own uploads.
//
// The island's sound effects (DATA/wuhu/sounds.nsfx, about 0.7 MB: sounds.h) are read on the same
// worker after the package, for the same reason: not the game's thread.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "engine/collision/kcl.h"
#include "engine/g3d/package.h"
#include "runtime/runtime.h"

namespace engine::wuhu {

class PackageLoad {
public:
    // the package at `path`, and the sound bank at `sounds_path` (none if empty)
    explicit PackageLoad(std::string path, std::string sounds_path = std::string());
    // (waits for the worker, if it is still at it)
    ~PackageLoad();
    PackageLoad(const PackageLoad&) = delete;
    PackageLoad& operator=(const PackageLoad&) = delete;

    // on a worker, or here and now where none can start
    void start();
    bool done() const { return done_.load(std::memory_order_acquire); }
    // how far: the file's bytes read so far, of how many (0 while unknown)
    u32 bytes_read() const { return read_.load(std::memory_order_relaxed); }
    u32 bytes_total() const { return total_.load(std::memory_order_relaxed); }

    // once done(): what was made (both null if it failed, `error` saying why), and what it took
    std::unique_ptr<engine::g3d::Package> package;
    std::unique_ptr<engine::collision::World> collision;
    std::string error;
    f32 read_ms = 0, check_ms = 0, collision_ms = 0;
    // the sound bank's bytes as they are (empty if it can't be had, `sounds_error` saying why), read
    // whether or not the package could be
    std::vector<u8> sounds;
    std::string sounds_error;

private:
    static void run(void* self);
    void work();

    void read_sounds();

    const std::string path_, sounds_path_;
    std::atomic<u32> read_{0}, total_{0};
    std::atomic<bool> done_{false};
    bool started_ = false;
};

}  // namespace engine::wuhu
