// The room bar's buttons the port adds (the Sound Player's サウンド, Wuhu Island's 出発). The bar
// makes each as it makes its own (runtime/patches/menus.cpp: layout_living_MoveBtn_AddTransitButton,
// from one of the bar's own button layouts: the same face, frame, animations and sounds); here it
// gets an icon and a label of its own, in the bar's two-tone style (art.h), both measured against
// the bar's own textures (Move_Btn_Icon_*.tpl, Move_Btn_Text_*.tpl).
// The textures are drawn once, the first time the room is built, into a block reserved at the
// start (the room is built again every time it comes back, and a block of its own each time would
// keep the room's memory under it from being given back: the heaps are stacks), and never written
// again.
#pragma once

#include <vector>

#include "engine/ui/art.h"
#include "runtime/runtime.h"

namespace engine::ui {

// The bar's icons are 50x50 textures (いろんな間's 50x48) drawn at 45x45 (Pic_Icon): a chunky
// silhouette reaching to about 2 px from the texture's edges, outlined 1.5 to 1.9 px (BarButtonArt
// outlines it). An icon is drawn into a BAR_ICON_SIZE square.
const u32 BAR_ICON_SIZE = 50;

class BarButtonArt {
public:
    using IconMaker = art::Coverage (*)();

    // log_name: the log's prefix; icon: draws the icon's shape (0 to 1 a pixel, BAR_ICON_SIZE
    // square, without its outline); label: the label's text, set from the channel's font
    BarButtonArt(const char* log_name, IconMaker icon, const char16_t* label) : log_name_(log_name), icon_(icon) {
        for (; *label; label++)
            label_.push_back((u16)*label);
    }

    // at the start (in a make_lasting of engine::system::host_scene_boot): the textures' block
    void reserve();
    // the button the bar has just made (a layout::Button): its icon and label, their textures drawn
    // the first time (the system font is loaded by then)
    void dress(u32 button);

private:
    art::Coverage draw_label(u32 font) const;

    const char* log_name_;
    IconMaker icon_;
    std::vector<u16> label_;  // UTF-16
    u32 texels_ = 0;
    u32 label_w_ = 0;  // 0: not drawn yet
};

}  // namespace engine::ui
