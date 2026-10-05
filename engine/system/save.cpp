// The channel's save and the port's sections in it (see save.h).
//
// After the game's GAME_BYTES, little-endian:
//   0x00 "NOMASAVE"   0x08 u32 version (1)   0x0C u32 the bytes that follow
//   then the sections: u32 tag, u32 bytes, the data (padded to 4 bytes)
// The block is read when first asked for and kept; a change writes all of it again. A save the
// game makes afresh (the file anew, at GAME_BYTES) has no block until a section is set. The file
// is touched on the CPU's thread, after the I/O thread's jobs (the game's own NAND requests).
#include "engine/system/save.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "runtime/hw/ios/ios.h"
#include "runtime/runtime.h"

namespace engine::save {

const char NAND_PATH[] = "/title/00010001/4843494a/data/siskin.bin";
const char BANNER_PATH[] = "/title/00010001/4843494a/data/banner.bin";

namespace {

const char MAGIC[8] = {'N', 'O', 'M', 'A', 'S', 'A', 'V', 'E'};
const u32 VERSION = 1, MAX_BYTES = 4u << 20;

bool s_read, s_erase;
std::map<u32, std::vector<u8>> s_sections;

u32 rd32(const u8* p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
void put32(std::vector<u8>& v, u32 x) {
    for (int i = 0; i < 4; i++)
        v.push_back((u8)(x >> 8 * i));
}

void read_block() {
    if (s_read)
        return;
    s_read = true;
    guest::ios_async_drain();
    FILE* f = fopen(guest::nand_host_path(NAND_PATH).c_str(), "rb");
    if (!f)
        return;
    u8 hdr[16];
    bool ok = fseek(f, GAME_BYTES, SEEK_SET) == 0 && fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) &&
              memcmp(hdr, MAGIC, 8) == 0 && rd32(hdr + 8) == VERSION && rd32(hdr + 12) <= MAX_BYTES;
    std::vector<u8> body(ok ? rd32(hdr + 12) : 0);
    ok = ok && fread(body.data(), 1, body.size(), f) == body.size();
    fclose(f);
    if (!ok)
        return;
    for (size_t at = 0; at + 8 <= body.size();) {
        u32 t = rd32(&body[at]), len = rd32(&body[at + 4]);
        if (len > body.size() - at - 8) {
            guest::log("save: the port's section %08X runs past its block: the rest left out", t);
            break;
        }
        s_sections[t].assign(body.begin() + at + 8, body.begin() + at + 8 + len);
        at += 8 + ((len + 3) & ~size_t(3));
    }
}

}  // namespace

bool has_section(u32 t) {
    read_block();
    return s_sections.count(t) != 0;
}

std::vector<u8> section(u32 t) {
    read_block();
    auto it = s_sections.find(t);
    return it == s_sections.end() ? std::vector<u8>() : it->second;
}

bool set_section(u32 t, const std::vector<u8>& data) {
    read_block();
    std::map<u32, std::vector<u8>> next = s_sections;
    next[t] = data;
    std::vector<u8> body;
    for (const auto& [tag, bytes] : next) {
        put32(body, tag), put32(body, (u32)bytes.size());
        body.insert(body.end(), bytes.begin(), bytes.end());
        body.resize((body.size() + 3) & ~size_t(3));
    }
    std::vector<u8> block(MAGIC, MAGIC + 8);
    put32(block, VERSION), put32(block, (u32)body.size());
    block.insert(block.end(), body.begin(), body.end());
    // (the game's part has to be there: a block can't come before the game has made the file)
    guest::ios_async_drain();
    FILE* f = fopen(guest::nand_host_path(NAND_PATH).c_str(), "r+b");
    if (!f)
        return false;
    bool ok = fseek(f, 0, SEEK_END) == 0 && ftell(f) >= (long)GAME_BYTES && fseek(f, GAME_BYTES, SEEK_SET) == 0 &&
              fwrite(block.data(), 1, block.size(), f) == block.size();
    fclose(f);
    if (ok)
        s_sections.swap(next);
    return ok;
}

void erase_at_exit() {
    s_erase = true;
    guest::log("save: to be erased when the app ends");
}

bool erase_if_marked() {
    if (!s_erase)
        return false;
    s_erase = false;
    guest::ios_async_drain();
    // the file first, then its banner (a banner left behind: the game takes the save for a broken
    // one, says so and makes the file again; a file that stays keeps its banner)
    std::string host = guest::nand_host_path(NAND_PATH), banner = guest::nand_host_path(BANNER_PATH);
    bool gone = remove(host.c_str()) == 0;
    bool banner_gone = gone && (remove(banner.c_str()) == 0 || errno == ENOENT);
    s_sections.clear(), s_read = true;
    if (gone)
        guest::log("save: %s erased, %s", host.c_str(), banner_gone ? "its banner too" : "its banner couldn't be");
    else
        guest::log("save: %s couldn't be erased", host.c_str());
    return gone;
}

}  // namespace engine::save
