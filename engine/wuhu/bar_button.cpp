// The room bar's 出発 button (see bar_button.h): a palm tree on a small island for its icon, Wuhu
// Island's sign, and 出発 ("departure") set from the channel's font for its label, both in the bar's
// two-tone style (engine/ui/bar_button.h).
#include "engine/wuhu/bar_button.h"

#include "engine/ui/art.h"
#include "engine/ui/bar_button.h"

namespace engine::wuhu {

namespace {

namespace art = engine::ui::art;

// The palm: its trunk leaning to the right out of a low island mound, four fronds arching from the
// crown (two drooping, two rising) and two coconuts under it; in the bar icons' chunky silhouette
// (the trunk 7 px thick at its foot, the fronds up to 9), the ink reaching to 2 to 4 px from the
// texture's edges as the bar's own icons do. Its ink covers about 800 px of the 50x50, between the
// TV programmes' 697 and the theater's 1409. (Designed and previewed beside the bar's icons with
// the research tool ui_art.py, icon_palm; the same shapes here.)
art::Coverage draw_icon() {
    const u32 size = engine::ui::BAR_ICON_SIZE;
    const f32 crown_x = 30.0f, crown_y = 13.5f;
    art::Samples icon(size, size);
    // the trunk, from the mound up to the crown, thinning from 3.6 to 2.2 px in radius
    art::Stroke{20.5f, 42.0f, 19.5f, 26.0f, crown_x, crown_y, 3.6f, 2.2f}.mark(icon);
    // the mound: the top of an ellipse whose centre is below the texture, cut level 3 px above
    // the bottom edge
    const art::Ellipse mound{24.5f, 50.0f, 22.0f, 10.5f};
    icon.mark(mound.cx - mound.rx, mound.cy - mound.ry, mound.cx + mound.rx, 47.0f,
              [&](f32 x, f32 y) { return y <= 47.0f && mound.inside(x, y); });
    // the fronds: from the crown, pulled toward a control point, to their tips, thick at the crown
    // for longer (taper 0.75) and ending in a 0.6 px point
    struct Frond {
        f32 cx, cy, tx, ty, width;
    };
    const Frond fronds[] = {
        {17.0f, 3.0f, 3.5f, 15.5f, 4.4f},    // left, drooping
        {24.0f, 0.5f, 12.0f, 4.5f, 3.6f},    // up and to the left
        {39.0f, 1.5f, 46.5f, 9.0f, 3.8f},    // up and to the right
        {45.0f, 10.0f, 46.5f, 24.5f, 4.4f},  // right, drooping
    };
    for (const Frond& f : fronds)
        art::Stroke{crown_x, crown_y, f.cx, f.cy, f.tx, f.ty, f.width, 0.6f, 0.75f}.mark(icon);
    // the coconuts, under the crown
    art::Ellipse{27.0f, 18.0f, 3.1f, 3.1f}.mark(icon);
    art::Ellipse{33.0f, 18.5f, 3.1f, 3.1f}.mark(icon);
    return icon.coverage();
}

engine::ui::BarButtonArt s_art("wuhu", &draw_icon, u"出発");

}  // namespace

void reserve_bar_button() { s_art.reserve(); }

void dress_bar_button(u32 button) { s_art.dress(button); }

}  // namespace engine::wuhu
