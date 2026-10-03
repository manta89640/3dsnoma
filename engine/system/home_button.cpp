// The Wii's HOME Button Menu (the SDK's HBM library, behind the channel's homebutton manager):
// not part of the 3DS port. The 3DS's own HOME button suspends the app, so the menu is never
// loaded (its layouts, the Huffman-packed sound archives and the HOME icon) and never opens.
//
// The manager object itself stays (homebutton_Manager_Init registers it): scenes still write its
// flags, allowing or forbidding HOME (+0x12, +0x14). Nothing else reads its state but the methods
// replaced here: open (+0x10), loaded (+0x15) and the "can't use HOME now" icon (+0x13) stay 0.
#include "noma_decomp.h"

namespace g {

void homebutton_Manager_LoadIcon(u32) {}
void homebutton_Manager_Load(u32) {}
void homebutton_Manager_Open(u32) {}
void homebutton_Manager_Update(u32) {}
void homebutton_Manager_Draw(u32) {}

}  // namespace g
