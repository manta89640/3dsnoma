// A stand-in for the island's content (content.h), NOMA_WUHU_PLACEHOLDER=1: it "loads" for a while
// (NOMA_WUHU_LOAD_MS, 1500 by default: long enough for the loading rings to show) and then shows
// the island's sky colour over the whole screen (#83D5FF, its day fog: a full-screen white picture,
// Common/Common_White_Bg.lex, given that colour), so that the scene's shell (the way in and out, the
// rings, the caption, the window) can be run and checked on its own, without the island's package
// (island.cpp's content needs your Wii Sports Resort's, staged), over something as light as the
// island.
//
// For the exploration menu's places it has no island package of its own: NOMA_WUHU_TEST_PACKAGE
// names a package file to load as it starts (any NG3D package: what the menu reads of it is its
// markers), and a warp then only logs the marker and gives the sky another colour, so that the
// frames show the warp happened.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "engine/g3d/package.h"
#include "engine/ui/lyt.h"
#include "engine/wuhu/content.h"

namespace engine::wuhu {

namespace {

namespace lyt = engine::ui::lyt;

const lyt::Rgb SKY = {0x83, 0xD5, 0xFF};
// the colours a warp gives the sky, in turn
const lyt::Rgb WARP_SKIES[] = {{0xFF, 0xD5, 0x83}, {0xA8, 0xE6, 0x8C}, {0xE0, 0xA8, 0xE6}, {0x83, 0xD5, 0xFF}};

// the test package's file, read whole (nullptr and logged if it can't be)
engine::g3d::Package* load_test_package(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        guest::log("wuhu: placeholder content: can't open %s", path);
        return nullptr;
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    void* data = size > 0 ? std::malloc((size_t)size) : nullptr;
    if (!data || std::fread(data, 1, (size_t)size, f) != (size_t)size) {
        std::fclose(f);
        std::free(data);
        guest::log("wuhu: placeholder content: can't read %s", path);
        return nullptr;
    }
    std::fclose(f);
    std::string error;
    engine::g3d::Package* p = engine::g3d::Package::load(data, (u32)size, &error);
    if (!p)
        guest::log("wuhu: placeholder content: %s: %s", path, error.c_str());
    else
        guest::log("wuhu: placeholder content: test package %s, %u markers", path, (unsigned)p->markers().count);
    return p;
}

class Placeholder : public IslandContent {
public:
    ~Placeholder() override {
        lyt::layout_delete(sky_);
        delete package_;
    }
    void start_loading() override {
        const char* e = std::getenv("NOMA_WUHU_LOAD_MS");
        load_us_ = (u64)(e ? std::atoi(e) : 1500) * 1000;
        start_us_ = guest::host_time_us();
        sky_ = lyt::layout_new("layout/Common/Common_White_Bg.lex");
        lyt::paint(lyt::find_pane(sky_, "Pic_Bg"), SKY);
        lyt::layout_update(sky_);
        if (const char* path = std::getenv("NOMA_WUHU_TEST_PACKAGE"))
            package_ = load_test_package(path);
        guest::log("wuhu: placeholder content: loading for %u ms", (unsigned)(load_us_ / 1000));
    }
    f32 progress() override {
        u64 t = guest::host_time_us() - start_us_;
        return load_us_ ? (f32)std::min<u64>(t, load_us_) / (f32)load_us_ : 1.0f;
    }
    bool ready() const override { return guest::host_time_us() - start_us_ >= load_us_; }
    void update(const Controls&, bool) override {}
    void draw_opaque() override { lyt::layout_draw(sky_); }
    void draw_translucent() override {}
    const engine::g3d::Package* package() const override { return package_; }
    void warp(u32 marker) override {
        if (!package_ || marker >= package_->markers().count)
            return;
        const engine::g3d::fmt::Marker& m = package_->markers()[marker];
        guest::log("wuhu: placeholder content: warp to marker %u \"%s\" (%.1f, %.1f, %.1f)", (unsigned)marker,
                   package_->str(m.name), m.position[0], m.position[1], m.position[2]);
        lyt::paint(lyt::find_pane(sky_, "Pic_Bg"), WARP_SKIES[warps_++ % 4]);
        lyt::layout_update(sky_);
    }

private:
    u64 start_us_ = 0, load_us_ = 0;
    u32 sky_ = 0;
    engine::g3d::Package* package_ = nullptr;
    u32 warps_ = 0;
};

}  // namespace

IslandContent* make_placeholder_content() { return new Placeholder(); }

}  // namespace engine::wuhu
