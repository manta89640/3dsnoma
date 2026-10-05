// The room bar's 出発 button: the way to Wuhu Island. runtime/patches/menus.cpp makes it as the bar
// makes its others, from the TV programmes' layout (Move_Btn_Tv_Btn.lex, the bar's right end cap,
// hanging from Loc_Tv_Btn: offline the TV programmes' own button there is hidden); here it gets its
// icon and label (engine/ui/bar_button.h).
#pragma once

#include "runtime/runtime.h"

namespace engine::wuhu {

// at the start (make_lasting): the textures' block
void reserve_bar_button();
// the button the bar has just made: its icon and label
void dress_bar_button(u32 button);

}  // namespace engine::wuhu
