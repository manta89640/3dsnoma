// Texture decoding. Every GX format is stored in tiles (blocks of 32 bytes, 64 for RGBA8):
//
//   0 I4 8x8   1 I8 8x4   2 IA4 8x4   3 IA8 4x4   4 RGB565 4x4   5 RGB5A3 4x4   6 RGBA8 4x4
//   8 C4 8x8   9 C8 8x4   10 C14X2 4x4   14 CMPR 8x8 (four DXT1-like 4x4 sub-blocks)
//
// Palettes (TLUTs) live in TMEM; formats 0 IA8, 1 RGB565, 2 RGB5A3. A texture can also be
// preloaded into TMEM (BP 0x60-0x63, GXPreLoadEntireTexture) and drawn from there: its SETIMAGE1
// has the "preloaded" bit and TMEM addresses (RGBA8: the AR halves of its 64-byte blocks in the
// even bank, the GB halves in the odd one) instead of a memory address in SETIMAGE3.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "regs.h"
#include "texture.h"

namespace guest {

namespace {

u8 s_tmem[1 << 20];
u32 s_tmem_serial = 1;  // moves on with every preload

struct Key {
    u32 addr, fmt, w, h, tlut_addr, tlut_fmt;
    bool operator==(const Key& o) const {
        return addr == o.addr && fmt == o.fmt && w == o.w && h == o.h && tlut_addr == o.tlut_addr &&
               tlut_fmt == o.tlut_fmt;
    }
};
struct KeyHash {
    size_t operator()(const Key& k) const {
        return (size_t)k.addr * 31 ^ k.fmt * 7 ^ k.w * 131 ^ k.h * 1031 ^ k.tlut_addr ^ k.tlut_fmt;
    }
};
struct Entry {
    GxTexture tex;
    u64 hash = 0;
    u32 checked_frame = ~0u;
    u32 used_frame = 0;
    u32 gen = 0;           // write generation of its memory when last hashed
    u32 hashed_frame = 0;  // the frame it was last hashed in
};
// A texture's memory is hashed again when the guest marked it written (cache flushes, IOS and GPU
// writes: memory.cpp's write generations), and every HASH_EVERY frames anyway (writes nothing
// marks: uncached stores)
const u32 HASH_EVERY = 120;
std::unordered_map<Key, Entry, KeyHash> s_cache;
u32 s_frame;

u32 rgba(u32 r, u32 g, u32 b, u32 a) { return r | g << 8 | b << 16 | a << 24; }
u32 x5(u32 v) { return (v << 3) | (v >> 2); }
u32 x6(u32 v) { return (v << 2) | (v >> 4); }
u32 x4(u32 v) { return v * 17; }
u32 x3(u32 v) { return (v << 5) | (v << 2) | (v >> 1); }

u32 rgb565(u16 v) { return rgba(x5(v >> 11), x6((v >> 5) & 63), x5(v & 31), 255); }
u32 rgb5a3(u16 v) {
    if (v & 0x8000)
        return rgba(x5((v >> 10) & 31), x5((v >> 5) & 31), x5(v & 31), 255);
    return rgba(x4((v >> 8) & 15), x4((v >> 4) & 15), x4(v & 15), x3((v >> 12) & 7));
}
u32 ia8(u16 v) {  // alpha in the high byte
    u32 i = v & 255, a = v >> 8;
    return rgba(i, i, i, a);
}

u32 tlut_color(u32 tlut_addr, u32 tlut_fmt, u32 index) {
    const u8* p = &s_tmem[(tlut_addr + 2 * index) & (sizeof(s_tmem) - 2)];
    u16 v = (u16)(p[0] << 8 | p[1]);
    switch (tlut_fmt) {
    case 0: return ia8(v);
    case 1: return rgb565(v);
    default: return rgb5a3(v);
    }
}

struct Block {
    u32 bw, bh, bytes;
};
Block block_of(u32 fmt) {
    switch (fmt) {
    case 0: case 8: case 14: return {8, 8, 32};
    case 1: case 2: case 9: return {8, 4, 32};
    case 6: return {4, 4, 64};
    default: return {4, 4, 32};
    }
}

u64 hash_bytes(const u8* p, u32 n) {
    u64 h = 0xCBF29CE484222325ull;
    u32 i = 0;
    for (; i + 8 <= n; i += 8) {
        u64 w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

void decode(const u8* src, u32 fmt, u32 w, u32 h, u32 tlut_addr, u32 tlut_fmt, u32* out) {
    Block b = block_of(fmt);
    u32 bx_n = (w + b.bw - 1) / b.bw, by_n = (h + b.bh - 1) / b.bh;
    auto put = [&](u32 x, u32 y, u32 c) {
        if (x < w && y < h)
            out[y * w + x] = c;
    };
    for (u32 by = 0; by < by_n; by++) {
        for (u32 bx = 0; bx < bx_n; bx++, src += b.bytes) {
            u32 x0 = bx * b.bw, y0 = by * b.bh;
            switch (fmt) {
            case 0:  // I4
                for (u32 i = 0; i < 64; i++) {
                    u32 n = (src[i / 2] >> ((i & 1) ? 0 : 4)) & 15;
                    put(x0 + i % 8, y0 + i / 8, rgba(x4(n), x4(n), x4(n), x4(n)));
                }
                break;
            case 1:  // I8
                for (u32 i = 0; i < 32; i++)
                    put(x0 + i % 8, y0 + i / 8, rgba(src[i], src[i], src[i], src[i]));
                break;
            case 2:  // IA4
                for (u32 i = 0; i < 32; i++) {
                    u32 in = x4(src[i] & 15), a = x4(src[i] >> 4);
                    put(x0 + i % 8, y0 + i / 8, rgba(in, in, in, a));
                }
                break;
            case 3:  // IA8
                for (u32 i = 0; i < 16; i++)
                    put(x0 + i % 4, y0 + i / 4, ia8((u16)(src[2 * i] << 8 | src[2 * i + 1])));
                break;
            case 4:  // RGB565
                for (u32 i = 0; i < 16; i++)
                    put(x0 + i % 4, y0 + i / 4, rgb565((u16)(src[2 * i] << 8 | src[2 * i + 1])));
                break;
            case 5:  // RGB5A3
                for (u32 i = 0; i < 16; i++)
                    put(x0 + i % 4, y0 + i / 4, rgb5a3((u16)(src[2 * i] << 8 | src[2 * i + 1])));
                break;
            case 6:  // RGBA8: AR pairs, then GB pairs
                for (u32 i = 0; i < 16; i++)
                    put(x0 + i % 4, y0 + i / 4,
                        rgba(src[2 * i + 1], src[32 + 2 * i], src[32 + 2 * i + 1], src[2 * i]));
                break;
            case 8:  // C4
                for (u32 i = 0; i < 64; i++) {
                    u32 n = (src[i / 2] >> ((i & 1) ? 0 : 4)) & 15;
                    put(x0 + i % 8, y0 + i / 8, tlut_color(tlut_addr, tlut_fmt, n));
                }
                break;
            case 9:  // C8
                for (u32 i = 0; i < 32; i++)
                    put(x0 + i % 8, y0 + i / 8, tlut_color(tlut_addr, tlut_fmt, src[i]));
                break;
            case 10:  // C14X2
                for (u32 i = 0; i < 16; i++) {
                    u32 n = (u32)(src[2 * i] << 8 | src[2 * i + 1]) & 0x3FFF;
                    put(x0 + i % 4, y0 + i / 4, tlut_color(tlut_addr, tlut_fmt, n));
                }
                break;
            case 14:  // CMPR
                for (u32 sb = 0; sb < 4; sb++) {
                    const u8* s = src + 8 * sb;
                    u16 c0 = (u16)(s[0] << 8 | s[1]), c1 = (u16)(s[2] << 8 | s[3]);
                    u32 r0 = x5(c0 >> 11), g0 = x6((c0 >> 5) & 63), b0 = x5(c0 & 31);
                    u32 r1 = x5(c1 >> 11), g1 = x6((c1 >> 5) & 63), b1 = x5(c1 & 31);
                    u32 pal[4];
                    pal[0] = rgba(r0, g0, b0, 255);
                    pal[1] = rgba(r1, g1, b1, 255);
                    if (c0 > c1) {
                        pal[2] = rgba((r1 * 3 + r0 * 5) >> 3, (g1 * 3 + g0 * 5) >> 3, (b1 * 3 + b0 * 5) >> 3, 255);
                        pal[3] = rgba((r0 * 3 + r1 * 5) >> 3, (g0 * 3 + g1 * 5) >> 3, (b0 * 3 + b1 * 5) >> 3, 255);
                    } else {
                        pal[2] = rgba((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 255);
                        pal[3] = rgba((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 0);
                    }
                    u32 sx = x0 + (sb & 1) * 4, sy = y0 + (sb >> 1) * 4;
                    for (u32 i = 0; i < 16; i++) {
                        u32 idx = (s[4 + i / 4] >> (6 - 2 * (i % 4))) & 3;
                        put(sx + i % 4, sy + i / 4, pal[idx]);
                    }
                }
                break;
            default:
                for (u32 i = 0; i < b.bw * b.bh; i++)
                    put(x0 + i % b.bw, y0 + i / b.bw, rgba(255, 0, 255, 255));
                break;
            }
        }
    }
}

}  // namespace

u32 gx_texture_bytes(u32 fmt, u32 w, u32 h) {
    Block b = block_of(fmt);
    return ((w + b.bw - 1) / b.bw) * ((h + b.bh - 1) / b.bh) * b.bytes;
}

void gx_load_tlut(u32 bp64, u32 bp65) {
    u32 src = 0x80000000 | ((bp64 << 5) & 0x1FFFFFFF);
    u32 tmem = (bp65 & 0x3FF) << 9;
    u32 n = ((bp65 >> 10) & 0x3FF) * 32;
    if (tmem + n > sizeof(s_tmem))
        n = sizeof(s_tmem) - tmem;
    read_g(&s_tmem[tmem], src, n);
}

void gx_preload(u32 bp60, u32 bp61, u32 bp62, u32 bp63) {
    const u32 LINE = 32;
    u32 src = 0x80000000 | ((bp60 & 0xFFFFFF) << 5 & 0x1FFFFFFF);
    u32 even = bits(bp61, 0, 15) * LINE, odd = bits(bp62, 0, 15) * LINE;
    u32 count = bits(bp63, 0, 15), type = bits(bp63, 15, 2);
    if (type != 3) {  // one bank
        u32 n = std::min<u32>(count * LINE, sizeof(s_tmem) - std::min<u32>(even, sizeof(s_tmem)));
        read_g(&s_tmem[even], src, n);
    } else {  // RGBA8: 64-byte blocks split between the banks
        for (u32 i = 0; i < count && even + LINE <= sizeof(s_tmem) && odd + LINE <= sizeof(s_tmem); i++) {
            read_g(&s_tmem[even], src, LINE);
            read_g(&s_tmem[odd], src + LINE, LINE);
            even += LINE, odd += LINE, src += 2 * LINE;
        }
    }
    s_tmem_serial++;
}

const GxTexture* gx_texture(u32 map) {
    u32 img0 = g_gx.bp[bp_tex(BP_TX_SETIMAGE0, map)];
    u32 img1 = g_gx.bp[bp_tex(BP_TX_SETIMAGE1, map)];
    u32 img3 = g_gx.bp[bp_tex(BP_TX_SETIMAGE3, map)];
    u32 tlut = g_gx.bp[bp_tex(BP_TX_SETTLUT, map)];
    Key k;
    k.w = bits(img0, 0, 10) + 1;
    k.h = bits(img0, 10, 10) + 1;
    k.fmt = bits(img0, 20, 4);
    bool paletted = k.fmt == 8 || k.fmt == 9 || k.fmt == 10;
    k.tlut_addr = paletted ? bits(tlut, 0, 10) << 9 : 0;
    k.tlut_fmt = paletted ? bits(tlut, 10, 2) : 0;
    u32 bytes = gx_texture_bytes(k.fmt, k.w, k.h);
    const bool preloaded = bits(img1, 21, 1);
    const u8* src;
    if (preloaded) {
        // from TMEM, gathered into the layout it has in memory (keyed by a TMEM pseudo-address)
        u32 even = bits(img1, 0, 15) * 32, odd = bits(g_gx.bp[bp_tex(BP_TX_SETIMAGE2, map)], 0, 15) * 32;
        k.addr = 0xF0000000 | even;
        static std::vector<u8> s_buf;
        s_buf.assign(bytes, 0);
        for (u32 at = 0; at < bytes; at += 32) {
            u32 from = k.fmt == 6 ? ((at / 64) * 32 + ((at & 32) ? odd : even)) : even + at;
            if (from + 32 <= sizeof(s_tmem))
                memcpy(&s_buf[at], &s_tmem[from], std::min<u32>(32, bytes - at));
        }
        src = s_buf.data();
    } else {
        k.addr = 0x80000000 | ((img3 & 0xFFFFFF) << 5);
        src = host_ptr(k.addr);
        if (!src)
            return nullptr;
    }
    Entry& e = s_cache[k];
    e.used_frame = s_frame;
    if (e.checked_frame != s_frame || (preloaded && e.gen != s_tmem_serial)) {
        NOMA_PROF_SCOPE("(gx texture check)");
        e.checked_frame = s_frame;
        u32 gen = preloaded ? s_tmem_serial : mem_generation(k.addr, bytes);
        if (!paletted && !e.tex.rgba.empty() && gen == e.gen && s_frame - e.hashed_frame < HASH_EVERY)
            return &e.tex;
        e.gen = gen;
        e.hashed_frame = s_frame;
        u64 h = hash_bytes(src, bytes);
        if (paletted)
            h ^= hash_bytes(&s_tmem[k.tlut_addr], k.fmt == 8 ? 32 : k.fmt == 9 ? 512 : 0x8000) * 31;
        if (h != e.hash || e.tex.rgba.empty()) {
            e.hash = h;
            e.tex.width = k.w;
            e.tex.height = k.h;
            e.tex.fmt = k.fmt;
            e.tex.tlut_fmt = k.tlut_fmt;
            e.tex.rgba.assign((size_t)k.w * k.h, 0);
            NOMA_PROF_SCOPE("(gx texture decode)");
            decode(src, k.fmt, k.w, k.h, k.tlut_addr, k.tlut_fmt, e.tex.rgba.data());
            static u32 s_ids;
            if (!e.tex.id)
                e.tex.id = ++s_ids;
            e.tex.version++;
        }
    }
    return &e.tex;
}

void gx_texture_invalidate(u32 addr, u32 size) {
    mem_mark_written(addr, size);
    addr = 0x80000000 | (addr & 0x1FFFFFFF);
    for (auto& kv : s_cache) {
        const Key& k = kv.first;
        u32 n = gx_texture_bytes(k.fmt, k.w, k.h);
        if (k.addr < addr + size && addr < k.addr + n)
            kv.second.checked_frame = ~0u;
    }
}

void gx_decode_texture(const u8* src, u32 fmt, u32 w, u32 h, u32* rgba) { decode(src, fmt, w, h, 0, 0, rgba); }

void gx_texture_frame() {
    s_frame++;
    for (auto it = s_cache.begin(); it != s_cache.end();) {
        if (s_frame - it->second.used_frame > 120)
            it = s_cache.erase(it);
        else
            ++it;
    }
}

}  // namespace guest
