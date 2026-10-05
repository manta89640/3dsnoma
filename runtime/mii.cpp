// Miis for the Wii's face library (RFL): 3DS Miis (the system's Mii Maker database, or one from
// the Mii Selector) converted to the Wii's 74-byte RFLCharData, and a Mii database
// (/shared2/menu/FaceLib/RFL_DB.dat) holding them.
//
// RFL_DB.dat, as the channel's RFL code reads and formats it (0x1F1E0 bytes, big-endian):
//   0x0000 "RNOD"                      0x0004 100 Mii slots, 0x4A bytes each
//   0x1CEC flags (bit 31 set when formatted)
//   0x1D00 "RNHD", -1, -1              0x1D08 10000 entries of 12 bytes: 8 zero bytes, 0x7FFF, 0x7FFF
//   0x1F1DE CRC-16 (CCITT polynomial, data bits shifted in): the CRC of the whole file is 0
//
// The 3DS layout is libctru's MiiData (3dbrew "Mii"): little-endian bitfields; the Wii's is
// big-endian (WiiBrew "Mii Data"). The 3DS has more parts than the Wii (hair 132 vs 72, eyes 60
// vs 48, ...): the extra ones fall back to the last Wii part of that kind.
#include <cstring>
#include <string>
#include <vector>

#include "runtime.h"

namespace guest {

namespace {

u32 le16(const u8* p) { return p[0] | p[1] << 8; }
u32 le32(const u8* p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
u32 bitsof(u32 v, int lo, int n) { return (v >> lo) & ((1u << n) - 1); }
u32 clampv(u32 v, u32 max) { return v > max ? max : v; }
u32 clampv(u32 v, u32 min, u32 max) { return v < min ? min : v > max ? max : v; }

void put16(u8* p, u32 v) {
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}
void put32(u8* p, u32 v) {
    put16(p, v >> 16);
    put16(p + 2, v);
}

// the CRC RFL checks: per bit, shift left, add the polynomial if the top bit fell out, shift
// the data bit in at the bottom
u16 rfl_crc(const u8* d, size_t n, u16 crc = 0) {
    for (size_t i = 0; i < n; i++)
        for (int b = 7; b >= 0; b--) {
            bool top = (crc & 0x8000) != 0;
            crc = (u16)(crc << 1);
            if (top)
                crc ^= 0x1021;
            if ((d[i] >> b) & 1)
                crc ^= 1;
        }
    return crc;
}

}  // namespace

// a 3DS MiiData (0x5C bytes, as the Mii Selector returns it) -> RFLCharData (0x4A bytes)
void mii_3ds_to_wii(const u8* m, u8* w) {
    memset(w, 0, 0x4A);
    u32 details = le16(m + 0x18);
    u32 sex = bitsof(details, 0, 1), month = bitsof(details, 1, 4), day = bitsof(details, 5, 5);
    u32 color = clampv(bitsof(details, 10, 4), 11), fav = bitsof(details, 14, 1);
    put16(w + 0x00, sex << 14 | month << 10 | day << 5 | color << 1 | fav);
    for (int i = 0; i < 10; i++) {  // UTF-16LE -> UTF-16BE
        put16(w + 0x02 + 2 * i, le16(m + 0x1A + 2 * i));
        put16(w + 0x36 + 2 * i, le16(m + 0x48 + 2 * i));
    }
    w[0x16] = (u8)clampv(m[0x2E], 127);
    w[0x17] = (u8)clampv(m[0x2F], 127);
    // Mii ID: "made on this console" (top bits 100), the rest from the 3DS ID
    put32(w + 0x18, 0x80000000u | (le32(m + 0x0C) & 0x1FFFFFFF));
    // console ID: a checksum of the MAC's first three bytes, then its last three
    const u8* mac = m + 0x10;
    w[0x1C] = (u8)(mac[0] + mac[1] + mac[2]);
    w[0x1D] = mac[3], w[0x1E] = mac[4], w[0x1F] = mac[5];

    u32 face = m[0x30], face2 = m[0x31];
    u32 shape = clampv(bitsof(face, 1, 4), 7), skin = clampv(bitsof(face, 5, 3), 5);
    u32 wrinkles = bitsof(face2, 0, 4), makeup = bitsof(face2, 4, 4);
    u32 feature = clampv(makeup ? makeup : wrinkles, 11);
    put16(w + 0x20, shape << 13 | skin << 10 | feature << 6);

    u32 hair = clampv(m[0x32], 71), hair2 = m[0x33];
    put16(w + 0x22, hair << 9 | bitsof(hair2, 0, 3) << 6 | bitsof(hair2, 3, 1) << 5);

    // (RFL takes no Mii whose eyebrows sit above position 3: RFLiCheckValidInfo)
    u32 brow = le32(m + 0x38);
    put32(w + 0x24, clampv(bitsof(brow, 0, 5), 23) << 27 | clampv(bitsof(brow, 16, 4), 11) << 22 |
                        bitsof(brow, 5, 3) << 13 | clampv(bitsof(brow, 8, 4), 8) << 9 |
                        clampv(bitsof(brow, 24, 5), 3, 18) << 4 | clampv(bitsof(brow, 20, 4), 12));

    u32 eye = le32(m + 0x34);
    put32(w + 0x28, clampv(bitsof(eye, 0, 6), 47) << 26 | clampv(bitsof(eye, 16, 5), 7) << 21 |
                        clampv(bitsof(eye, 25, 5), 18) << 16 | clampv(bitsof(eye, 6, 3), 5) << 13 |
                        clampv(bitsof(eye, 9, 4), 7) << 9 | clampv(bitsof(eye, 21, 4), 12) << 5);

    u32 nose = le16(m + 0x3C);
    put16(w + 0x2C, clampv(bitsof(nose, 0, 5), 11) << 12 | clampv(bitsof(nose, 5, 4), 8) << 8 |
                        clampv(bitsof(nose, 9, 5), 18) << 3);

    u32 mouth = le16(m + 0x3E), mouth2 = le16(m + 0x40);
    put16(w + 0x2E, clampv(bitsof(mouth, 0, 6), 23) << 11 | clampv(bitsof(mouth, 6, 3), 2) << 9 |
                        clampv(bitsof(mouth, 9, 4), 8) << 5 | clampv(bitsof(mouth2, 0, 5), 18));

    u32 glasses = le16(m + 0x44);
    put16(w + 0x30, clampv(bitsof(glasses, 0, 4), 8) << 12 | clampv(bitsof(glasses, 4, 3), 5) << 9 |
                        clampv(bitsof(glasses, 7, 4), 7) << 5 | clampv(bitsof(glasses, 11, 5), 20));

    u32 beard = le16(m + 0x42);
    put16(w + 0x32, clampv(bitsof(mouth2, 5, 3), 3) << 14 | clampv(bitsof(beard, 0, 3), 3) << 12 |
                        bitsof(beard, 3, 3) << 9 | clampv(bitsof(beard, 6, 4), 8) << 5 |
                        clampv(bitsof(beard, 10, 5), 16));

    u32 mole = le16(m + 0x46);
    put16(w + 0x34, bitsof(mole, 0, 1) << 15 | clampv(bitsof(mole, 1, 4), 8) << 11 |
                        clampv(bitsof(mole, 10, 5), 30) << 6 | clampv(bitsof(mole, 5, 5), 16) << 1);
}

// the CRC at the end, over everything before it and two zero bytes: the whole file CRCs to 0
static void rfl_seal(std::vector<u8>& db) {
    u16 crc = rfl_crc(db.data(), 0x1F1DE);
    static const u8 zero[2] = {0, 0};
    crc = rfl_crc(zero, 2, crc);
    put16(&db[0x1F1DE], crc);
}

// a fresh database (formatted as RFL formats one) with `count` Miis in the first slots
std::vector<u8> rfl_make_database(const u8* const* miis, int count) {
    std::vector<u8> db(0x1F1E0, 0);
    memcpy(db.data(), "RNOD", 4);
    for (int i = 0; i < count && i < 100; i++)
        memcpy(&db[4 + 0x4A * i], miis[i], 0x4A);
    put32(&db[0x1CEC], 0x80000000u);
    memcpy(&db[0x1D00], "RNHD", 4);
    put16(&db[0x1D04], 0xFFFF);
    put16(&db[0x1D06], 0xFFFF);
    for (int i = 0; i < 10000; i++) {
        put16(&db[0x1D08 + 12 * i + 8], 0x7FFF);
        put16(&db[0x1D08 + 12 * i + 10], 0x7FFF);
    }
    rfl_seal(db);
    return db;
}

// `db` with `mii` in it: in the slot of the same Mii (Mii and console ID) if it's there, else in
// the first empty one (the last if all are taken); the other Miis stay where they are. A database
// this converter doesn't vouch for (or none) is started afresh.
std::vector<u8> rfl_add_mii(const std::vector<u8>& db, const u8* mii) {
    if (!rfl_database_valid(db))
        return rfl_make_database(&mii, 1);
    std::vector<u8> out = db;
    int slot = -1, empty = -1;
    for (int i = 0; i < 100 && slot < 0; i++) {
        const u8* m = &out[4 + i * 0x4A];
        if (!m[2] && !m[3])  // no name: an empty slot
            empty = empty < 0 ? i : empty;
        else if (memcmp(m + 0x18, mii + 0x18, 8) == 0)
            slot = i;
    }
    if (slot < 0)
        slot = empty >= 0 ? empty : 99;
    memcpy(&out[4 + slot * 0x4A], mii, 0x4A);
    rfl_seal(out);
    return out;
}

// `db` with all of `miis` (count x 0x4A bytes, the 3DS's Mii Maker database converted) in it: a
// Mii already there (same Mii and console ID) is updated in its own slot, a new one takes the first
// empty slot, or with none left the first slot of a Mii that isn't in `miis`. Every other Mii stays
// in its slot (the family may still name one the 3DS no longer has). A database this converter
// doesn't vouch for (or none) is started afresh.
std::vector<u8> rfl_sync_miis(const std::vector<u8>& db, const u8* miis, int count) {
    std::vector<u8> out = rfl_database_valid(db) ? db : rfl_make_database(nullptr, 0);
    auto slot = [&](int i) { return &out[4 + 0x4A * i]; };
    auto empty = [&](int i) { return !slot(i)[2] && !slot(i)[3]; };  // (no name)
    bool taken[100] = {};  // slots holding one of `miis`
    std::vector<int> place(count, -1);
    for (int k = 0; k < count; k++)
        for (int i = 0; i < 100 && place[k] < 0; i++)
            if (!taken[i] && !empty(i) && memcmp(slot(i) + 0x18, miis + 0x4A * k + 0x18, 8) == 0)
                place[k] = i, taken[i] = true;
    for (int k = 0; k < count; k++) {
        for (int i = 0; i < 100 && place[k] < 0; i++)
            if (!taken[i] && empty(i))
                place[k] = i, taken[i] = true;
        for (int i = 0; i < 100 && place[k] < 0; i++)
            if (!taken[i])
                place[k] = i, taken[i] = true;
    }
    for (int k = 0; k < count; k++)
        if (place[k] >= 0)
            memcpy(slot(place[k]), miis + 0x4A * k, 0x4A);
    rfl_seal(out);
    return out;
}

// a database as this converter writes them: intact, and every Mii in it with its face parts (an
// earlier converter left eyebrows to mole zero, which RFL's checks reject: "no Miis registered")
bool rfl_database_valid(const std::vector<u8>& db) {
    if (db.size() != 0x1F1E0 || memcmp(db.data(), "RNOD", 4) != 0 || rfl_crc(db.data(), db.size()) != 0)
        return false;
    for (int i = 0; i < 100; i++) {
        const u8* m = &db[4 + i * 0x4A];
        if (!m[2] && !m[3])  // no name: an empty slot
            continue;
        bool parts = false;
        for (int k = 0x24; k < 0x36; k++)
            parts |= m[k] != 0;
        if (!parts)
            return false;
    }
    return true;
}

}  // namespace guest
