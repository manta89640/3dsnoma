// A minimal PNG writer (stored deflate blocks, no compression) for frame dumps.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/runtime.h"

namespace {

u32 crc_table[256];

u32 crc32(const u8* p, size_t n, u32 c = 0xFFFFFFFF) {
    if (!crc_table[1])
        for (u32 i = 0; i < 256; i++) {
            u32 v = i;
            for (int k = 0; k < 8; k++)
                v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    for (size_t i = 0; i < n; i++)
        c = crc_table[(c ^ p[i]) & 255] ^ (c >> 8);
    return c;
}

void put32(std::vector<u8>& v, u32 x) {
    v.push_back((u8)(x >> 24)), v.push_back((u8)(x >> 16)), v.push_back((u8)(x >> 8)), v.push_back((u8)x);
}

void chunk(std::vector<u8>& out, const char* type, const std::vector<u8>& data) {
    put32(out, (u32)data.size());
    size_t at = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32(out, crc32(&out[at], out.size() - at) ^ 0xFFFFFFFF);
}

}  // namespace

// rgba: R in bits 0-7 ... A in bits 24-31
bool write_png(const std::string& path, const u32* rgba, u32 w, u32 h) {
    std::vector<u8> raw;
    raw.reserve((size_t)(w * 3 + 1) * h);
    for (u32 y = 0; y < h; y++) {
        raw.push_back(0);
        for (u32 x = 0; x < w; x++) {
            u32 c = rgba[(size_t)y * w + x];
            raw.push_back((u8)c), raw.push_back((u8)(c >> 8)), raw.push_back((u8)(c >> 16));
        }
    }
    std::vector<u8> z = {0x78, 0x01};
    u32 a = 1, b = 0;
    for (u8 c : raw)
        a = (a + c) % 65521, b = (b + a) % 65521;
    for (size_t i = 0; i < raw.size(); i += 65535) {
        size_t n = std::min<size_t>(65535, raw.size() - i);
        z.push_back(i + n == raw.size() ? 1 : 0);
        z.push_back((u8)n), z.push_back((u8)(n >> 8)), z.push_back((u8)~n), z.push_back((u8)(~n >> 8));
        z.insert(z.end(), raw.begin() + i, raw.begin() + i + n);
    }
    put32(z, b << 16 | a);
    std::vector<u8> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<u8> ihdr;
    put32(ihdr, w), put32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit RGB
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    FILE* f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return true;
}
