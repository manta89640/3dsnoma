// The room's accessories, kept in the save's "ROOM" section (engine/system/save.h), and the
// room's poster shown only when there is a poster to show (see room_accessories.h).
//
// The section: records of u32 kind, u32 bytes, the data (padded to 4 bytes), little-endian;
// kind 1: a poster (its picture's name). A save without the section gets an empty one when the
// accessories are first read.
#include "engine/system/room_accessories.h"

#include <cstring>

#include "engine/system/save.h"
#include "noma_decomp.h"
#include "runtime/runtime.h"

namespace engine::system {

namespace {

const u32 SECTION = save::tag("ROOM"), KIND_POSTER = 1;

bool s_loaded;
RoomAccessories s_acc;

u32 rd32(const u8* p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
void put32(std::vector<u8>& v, u32 x) {
    for (int i = 0; i < 4; i++)
        v.push_back((u8)(x >> 8 * i));
}

RoomAccessories decode(const std::vector<u8>& d) {
    RoomAccessories a;
    for (size_t at = 0; at + 8 <= d.size();) {
        u32 kind = rd32(&d[at]), len = rd32(&d[at + 4]);
        if (len > d.size() - at - 8)
            break;
        const char* data = (const char*)&d[at + 8];
        if (kind == KIND_POSTER)
            a.posters.emplace_back(data, strnlen(data, len));
        at += 8 + ((len + 3) & ~size_t(3));
    }
    return a;
}

std::vector<u8> encode(const RoomAccessories& a) {
    std::vector<u8> d;
    for (const std::string& p : a.posters) {
        put32(d, KIND_POSTER), put32(d, (u32)p.size());
        d.insert(d.end(), p.begin(), p.end());
        d.resize((d.size() + 3) & ~size_t(3));
    }
    return d;
}

}  // namespace

const RoomAccessories& room_accessories() {
    if (!s_loaded) {
        s_loaded = true;
        if (save::has_section(SECTION)) {
            s_acc = decode(save::section(SECTION));
            guest::log("room: the save's accessories: %zu posters", s_acc.posters.size());
        } else if (save::set_section(SECTION, encode(s_acc))) {
            guest::log("room: the save had no accessories: an empty section added");
        }
    }
    return s_acc;
}

bool room_accessories_save(const RoomAccessories& a) {
    if (!save::set_section(SECTION, encode(a)))
        return false;
    s_acc = a, s_loaded = true;
    return true;
}

}  // namespace engine::system

namespace g {

u32 g3d_living_Poster_ctor_orig(u32 r3, u32 r4, u32 r5, u32 r6);

// The room's poster (g3d::living::Poster: a model on the wall showing the pictures of the
// server's poster list, today.xml's posterinfo, their count at +0x598): with none from the server
// (offline: none) and none among the room's accessories, it isn't drawn. Its model (+0x14, an
// nw4r ScnMdl) leaves the scene's gathering (ScnObj option 1); the poster itself stays, as the
// room's other code holds it.
u32 g3d_living_Poster_ctor(u32 r3, u32 r4, u32 r5, u32 r6) {
    u32 r = g3d_living_Poster_ctor_orig(r3, r4, r5, r6);
    if ((s32)guest::load32(r3 + 0x598) <= 0 && engine::system::room_accessories().posters.empty()) {
        if (u32 mdl = guest::load32(r3 + 0x14))
            nw4r_g3d_ScnObj_SetScnObjOption(mdl, 1, 1);
        guest::store8(r3 + 0x2C + 0x11, 0);
        guest::log("room: no posters to show: the poster is hidden");
    }
    return r;
}

}  // namespace g
