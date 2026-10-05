// The room's accessories (posters, for now), kept in the save (engine/system/save.h), and the
// room's poster, shown only when there is a poster to show (engine/system/room_accessories.cpp).
#pragma once

#include <string>
#include <vector>

namespace engine::system {

struct RoomAccessories {
    std::vector<std::string> posters;  // the posters hung in the room: their pictures' names
};

// the accessories in the save, read when first asked for (none if the save has none yet)
const RoomAccessories& room_accessories();
// `a` into the save; false if the save isn't there yet or can't be written
bool room_accessories_save(const RoomAccessories& a);

}  // namespace engine::system
