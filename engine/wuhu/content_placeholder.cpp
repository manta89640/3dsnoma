// A stand-in for the island's content (content.h) until the gameplay's is there: it "loads" for a
// while (NOMA_WUHU_LOAD_MS, 1500 by default: long enough for the loading rings to show) and then
// shows the island's sky colour over the whole screen (#83D5FF, its day fog: a full-screen white
// picture, Common/Common_White_Bg.lex, given that colour), so that the scene's shell (the way in
// and out, the rings, the caption, the window) can be run and checked on its own, over something
// as light as the island will be. (The gameplay's content replaces this file.)
#include <algorithm>
#include <cstdlib>

#include "engine/ui/lyt.h"
#include "engine/wuhu/content.h"

namespace engine::wuhu {

namespace {

namespace lyt = engine::ui::lyt;

const lyt::Rgb SKY = {0x83, 0xD5, 0xFF};

class Placeholder : public IslandContent {
public:
    ~Placeholder() override { lyt::layout_delete(sky_); }
    void start_loading() override {
        const char* e = std::getenv("NOMA_WUHU_LOAD_MS");
        load_us_ = (u64)(e ? std::atoi(e) : 1500) * 1000;
        start_us_ = guest::host_time_us();
        sky_ = lyt::layout_new("layout/Common/Common_White_Bg.lex");
        lyt::paint(lyt::find_pane(sky_, "Pic_Bg"), SKY);
        lyt::layout_update(sky_);
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

private:
    u64 start_us_ = 0, load_us_ = 0;
    u32 sky_ = 0;
};

}  // namespace

IslandContent* make_content() { return new Placeholder(); }

}  // namespace engine::wuhu
