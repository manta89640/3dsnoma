// The places the exploration menu can warp to (docs/wuhu_design.md §8): the island package's
// markers of kind 3, in the package's order, each with the name the menu lists and the caption
// shows on arrival.
#pragma once

#include <string>
#include <vector>

#include "runtime/runtime.h"

namespace engine::g3d {
class Package;
}

namespace engine::wuhu {

// a marker's kind: a place to warp to (engine/g3d/ng3d_format.h's Marker; 0 spawn, 1 landmark,
// 2 camera preset)
const u32 MARKER_WARP = 3;

struct Place {
    u32 marker;            // its index among the package's markers (IslandContent::warp's)
    std::u16string name;   // in the channel's font
};

// The package's warp places. A name is the marker's (UTF-8), without the landmark's number the
// staging may keep before it (the island's LandMarkData names its places "2_プライベート・...");
// a character the channel's font (wbf1) lacks is set as its full- or half-width twin when the font
// has that, else as "？", and logged; a marker whose name is blank is left out (logged). Reads the
// font, so only once it is loaded.
std::vector<Place> warp_places(const engine::g3d::Package& package);

}  // namespace engine::wuhu
