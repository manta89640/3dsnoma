// OSCalcCRC16: the CRC-16 of a block, as the SDK computes it, half a byte at a time through a table of
// 16 (__OSCRC16Table in the DOL): the channel checks its 480 KB save file with it when it reads it
// and when it writes it (~25 emulated instructions a byte). Here a byte at a time through a table of
// 256 made from the game's 16, which gives the same steps: for a byte b, the low half then the high
// half,
//   c1 = c >> 4 ^ t[(c ^ b) & 15],  c2 = c1 >> 4 ^ t[(c1 ^ b >> 4) & 15]
// is c >> 8 ^ T[(c ^ b) & 255] with T[x] = t[x & 15] >> 4 ^ t[(x >> 4 ^ t[x & 15]) & 15]. The poll
// points stay those of the loop it replaces (one per 4 bytes), charged a block at a time.
#include "noma_decomp.h"
#include "runtime.h"

namespace engine::os {

namespace {

const u32 NIBBLE_TABLE = GA___OSCRC16Table;  // u16[16], big-endian

struct ByteTable {
    u16 nibble[16];
    u16 byte[256];
};
ByteTable s_table;
bool s_made;

const ByteTable& table() {
    u16 t[16];
    for (u32 i = 0; i < 16; i++)
        t[i] = guest::load16(NIBBLE_TABLE + i * 2);
    if (!s_made || ::memcmp(t, s_table.nibble, sizeof(t)) != 0) {  // (read from the guest each time)
        ::memcpy(s_table.nibble, t, sizeof(t));
        for (u32 x = 0; x < 256; x++)
            s_table.byte[x] = (u16)(t[x & 15] >> 4 ^ t[(x >> 4 ^ t[x & 15]) & 15]);
        s_made = true;
    }
    return s_table;
}

}  // namespace

u32 crc16(u32 data, u32 size) {
    const u16* T = table().byte;
    u32 c = 0;
    const u8* p = guest::host_ptr(data);
    if (size && p && guest::host_ptr(data + size - 1) == p + (size - 1)) {
        for (u32 left = size; left;) {
            u32 n = left < 0x4000 ? left : 0x4000;  // 16 KB between poll points
            for (u32 i = 0; i < n; i++)
                c = c >> 8 ^ T[(c ^ p[i]) & 0xFF];
            p += n, left -= n;
            if ((guest::g_loop_budget -= (s32)(n >> 2)) < 0)
                guest::loop_poll();
        }
    } else {
        for (u32 i = 0; i < size; i++) {
            c = c >> 8 ^ T[(c ^ guest::load8(data + i)) & 0xFF];
            if ((i & 3) == 3 && --guest::g_loop_budget < 0)
                guest::loop_poll();
        }
    }
    return c & 0xFFFF;
}

}  // namespace engine::os

// ---- the guest ABI (decl.h)
namespace g {

u32 OSCalcCRC16(u32 data, u32 size) { return engine::os::crc16(data, size); }

}  // namespace g
