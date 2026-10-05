// The channel's save: siskin.bin in its NAND data directory. The game's own data fills the first
// GAME_BYTES; what the port keeps in the save (the room's accessories, ...) goes after it, in
// sections of its own. This is the one place that knows the file and its layout: the port's data
// reaches the save only through here (engine/system/save.cpp).
#pragma once

#include <vector>

#include "runtime/guest.h"

namespace engine::save {

// the file, as the game names it on the NAND
extern const char NAND_PATH[];
// the save's banner (what a Wii's data management shows for it), which the game makes with the
// file at its first start: the file missing and the banner there is a broken save to the game (it
// says so, then makes the file again)
extern const char BANNER_PATH[];
// the game's part: its save manager (lbl_808829D0) makes the file at this size, two copies of its
// regions, and reads and writes the regions at their offsets (it never asks the file's length)
const u32 GAME_BYTES = 0xF0000;

// A section's tag: four characters, e.g. tag("ROOM").
constexpr u32 tag(const char (&s)[5]) {
    return (u32)(u8)s[0] | (u32)(u8)s[1] << 8 | (u32)(u8)s[2] << 16 | (u32)(u8)s[3] << 24;
}
// whether the save has the section, and its data (empty if it hasn't)
bool has_section(u32 tag);
std::vector<u8> section(u32 tag);
// the section's data replaced (or added); false if the save isn't there yet (the game makes it at
// its first start) or can't be written
bool set_section(u32 tag, const std::vector<u8>& data);

// The whole save (the game's and the port's) erased when the app next ends: the game first ends
// as it does for the Wii Menu (its last writes done), then the file and its banner go, and the
// next start is a first start. erase_if_marked, on the way out: true if the save went.
void erase_at_exit();
bool erase_if_marked();

}  // namespace engine::save
