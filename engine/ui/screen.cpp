#include "engine/ui/screen.h"

#include <algorithm>
#include <cmath>

#include "engine/input/remote.h"
#include "runtime/hw/gx/gx.h"
#include "runtime/hw/hw.h"

namespace engine::ui::screen {

namespace {

// the layouts' space to the frame's pixels: x -304..304 to 0..640, y 228..-228 to 0..456 (the
// channel's 2D projection over its 640x456 viewport)
const f32 TO_EFB_X = 640.0f / 608.0f;

void write_screen(u32 screen, u32 x, u32 y, u32 w, u32 h) {
    guest::gx_fifo_write(0, guest::SCREEN_OPCODE, 1);
    guest::gx_fifo_write(0, screen, 1);
    for (u32 v : {x, y, w, h})
        guest::gx_fifo_write(0, std::min(v, (u32)0xFFFF), 2);
}

u32 efb(f32 v) { return (u32)std::max(0L, std::lround(v)); }

}  // namespace

void to_bottom(const Region& r) {
    write_screen(guest::SCREEN_BOTTOM, efb((r.left + 304.0f) * TO_EFB_X), efb(228.0f - r.top), efb(r.width * TO_EFB_X),
                 efb(r.height));
}

void to_top() { write_screen(guest::SCREEN_TOP, 0, 0, 0, 0); }

bool s_claimed;

void claim(const Region& r) {
    s_claimed = true;
    guest::platform_bottom_claim(true);
    guest::input_set_touch_region(true, r.left, r.top, r.width, r.height);
}

void release() {
    s_claimed = false;
    squeeze(true);
    guest::input_set_touch_region(false, 0, 0, 0, 0);
    guest::platform_bottom_claim(false);
}

bool claimed() { return s_claimed; }

void squeeze(bool on) {
    const u32 draw_info = guest::load32(GA_layout_sDrawInfo);
    if (!draw_info)
        return;
    const bool wide = guest::load8(guest::load32(GA_app_VideoMode_sInstance) + 0x8) != 0;
    const u8 f = guest::load8(draw_info + 0x50);
    guest::store8(draw_info + 0x50, on && wide ? (u8)(f | 0x20) : (u8)(f & ~0x20));
}

}  // namespace engine::ui::screen
