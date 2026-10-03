// CX's streaming decompression a byte and a bit at a time, as engine/cx/uncompress.cpp first had
// it (checked against the SDK's code by the game's own data): the definition that its fast paths
// have to match, output and context alike. Only NOMA_SELFTEST=cx uses it.
#pragma once

#include "noma_decomp.h"
#include "runtime.h"

namespace g {
u32 CXiVerifyHuffmanTable(u32 table, u32 bit_size);
}

namespace engine::cx::reference {

// CXUncompContextLZ, in guest memory
enum : u32 {
    LZ_DEST = 0x00,         // u8*: where the next byte goes
    LZ_DEST_COUNT = 0x04,   // s32: bytes still to produce
    LZ_FORCE = 0x08,        // s32: produce only this many (0: the header's size)
    LZ_LENGTH = 0x0C,       // s32: a match's length (and while reading it, its bits so far)
    LZ_LENGTH_FLG = 0x10,   // u8: match bytes still to read
    LZ_FLAGS = 0x11,        // u8: the current flag byte
    LZ_FLAG_INDEX = 0x12,   // u8: its flags still unused
    LZ_HEADER_SIZE = 0x13,  // u8: header bytes still to read (8 at the start)
    LZ_EX_FORMAT = 0x14,    // u8: extended lengths
};

struct LzState {
    u32 dest;
    s32 dest_count, force, length;
    u8 length_flg, flags, flag_index, header_size, ex_format;

    void load(u32 c) {
        dest = guest::load32(c + LZ_DEST);
        dest_count = (s32)guest::load32(c + LZ_DEST_COUNT);
        force = (s32)guest::load32(c + LZ_FORCE);
        length = (s32)guest::load32(c + LZ_LENGTH);
        length_flg = guest::load8(c + LZ_LENGTH_FLG);
        flags = guest::load8(c + LZ_FLAGS);
        flag_index = guest::load8(c + LZ_FLAG_INDEX);
        header_size = guest::load8(c + LZ_HEADER_SIZE);
        ex_format = guest::load8(c + LZ_EX_FORMAT);
    }
    void save(u32 c) const {
        guest::store32(c + LZ_DEST, dest);
        guest::store32(c + LZ_DEST_COUNT, (u32)dest_count);
        guest::store32(c + LZ_FORCE, (u32)force);
        guest::store32(c + LZ_LENGTH, (u32)length);
        guest::store8(c + LZ_LENGTH_FLG, length_flg);
        guest::store8(c + LZ_FLAGS, flags);
        guest::store8(c + LZ_FLAG_INDEX, flag_index);
        guest::store8(c + LZ_HEADER_SIZE, header_size);
        guest::store8(c + LZ_EX_FORMAT, ex_format);
    }
};

// the guest's bytes at a (inside one of MEM1/MEM2: a block of host memory)
inline u8* bytes(u32 a) { return guest::host_ptr(a); }

inline s32 read_lz(LzState& s, const u8* src, u32 len) {
    if (s.header_size) {
        if (s.header_size == 8) {
            if ((src[0] & 0xF0) != 0x10)
                return -1;
            s.ex_format = src[0] & 0xF;
            if (s.ex_format > 1)
                return -1;
        }
        // the header: type, then the size in 3 bytes (little-endian), or 0 and the size in 4 more
        u32 used = 0;
        for (u32 left = len;;) {
            u8 hs = --s.header_size;
            if (hs <= 3)
                s.dest_count |= (s32)((u32)src[used] << ((3 - hs) * 8));
            else if (hs <= 6)
                s.dest_count |= (s32)((u32)src[used] << ((6 - hs) * 8));
            used++;
            if (s.header_size == 4 && s.dest_count > 0)
                s.header_size = 0;
            if (--left == 0 || s.header_size == 0)
                break;
        }
        if (!s.header_size && s.force > 0 && s.force < s.dest_count)
            s.dest_count = s.force;
        len -= used, src += used;
        if (!len)
            return s.header_size ? -1 : s.dest_count;
    }
    u8* const dst0 = bytes(s.dest);
    u8* dst = dst0;
    if (!dst)
        return -1;
    auto ret = [&](s32 r) {  // the context keeps where the output got to, as the SDK's does
        s.dest += (u32)(dst - dst0);
        return r;
    };
    while (s.dest_count > 0) {
        if (!s.flag_index) {
            if (!len)
                break;
            s.flags = *src++, len--;
            s.flag_index = 8;
            continue;
        }
        if (!len)
            break;
        if (!(s.flags & 0x80)) {  // a literal byte
            *dst++ = *src++, len--;
            s.dest_count--;
        } else {  // a match: its length (one byte, or up to three extended), then the distance
            bool done = false;
            while (!done) {
                if (s.length_flg == 0) {
                    s32 l = s.length;
                    u32 disp = (*src++ & 0xFFu) | (((u32)l << 8) & 0xF00);
                    len--;
                    s.length = l >> 4;
                    s.length_flg = 3;
                    if (s.length > s.dest_count) {
                        if (!s.force)
                            return ret(-4);
                        s.length = s.dest_count;
                    }
                    for (; s.length > 0; s.length--, s.dest_count--, dst++)
                        *dst = dst[-(s32)(disp + 1)];
                    done = true;
                    break;
                }
                u8 flg = --s.length_flg;
                if (!s.ex_format) {
                    s.length = *src++ + 0x30;
                    s.length_flg = 0;
                } else if (flg == 2) {
                    u8 b = *src++;
                    s.length = b;
                    if ((b >> 4) == 1) {
                        s.length = ((b << 16) & 0xF0000) + 0x1110;
                    } else if ((b >> 4) == 0) {
                        s.length_flg = 1;
                        s.length = ((b << 8) & 0xF00) + 0x110;
                    } else {
                        s.length = b + 0x10;
                        s.length_flg = 0;
                    }
                } else if (flg == 1) {
                    s.length += *src++ << 8;
                } else if (flg == 0) {
                    s.length += *src++;
                }
                if (--len == 0)
                    return ret(s.dest_count);
            }
        }
        if (s.dest_count == 0)
            break;
        s.flags <<= 1;
        s.flag_index--;
    }
    if (s.dest_count > 0)
        return ret(s.dest_count);
    if (!s.force && len > 0x20)
        return ret(-3);
    return ret(0);
}

// CXUncompContextHuffman, in guest memory
enum : u32 {
    HF_DEST = 0x00,         // u32*: where the next word goes
    HF_DEST_COUNT = 0x04,   // s32: bytes still to produce
    HF_FORCE = 0x08,        // s32: produce only this many (0: the header's size)
    HF_TREE_P = 0x0C,       // u8*: while reading the tree, where its next byte goes; then the node
    HF_SRC_TMP = 0x10,      // u32: input bits, the next one at bit 31
    HF_DEST_TMP = 0x14,     // u32: output symbols so far (each new one enters at the top)
    HF_TREE_SIZE = 0x18,    // s16: tree bytes still to read (negative: its size byte first)
    HF_SRC_TMP_CNT = 0x1A,  // u8: bits in HF_SRC_TMP
    HF_DEST_TMP_CNT = 0x1B, // u8: bits in HF_DEST_TMP
    HF_BIT_SIZE = 0x1C,     // u8: bits per symbol (4 or 8)
    HF_HEADER_SIZE = 0x1D,  // u8: header bytes still to read (8 at the start)
    HF_TREE = 0x20,         // the tree: its size byte, then the root node at +0x21
};

// r3: the context, r4: the compressed bytes, r5: how many. The context tells how far it got.
inline void read_uncomp_huffman(u32 ctx, u32 data, u32 len) {
    const u8* src = bytes(data);
    if (!src || !len)
        return;
    u8 header_size = guest::load8(ctx + HF_HEADER_SIZE);
    s32 dest_count = (s32)guest::load32(ctx + HF_DEST_COUNT);
    s32 force = (s32)guest::load32(ctx + HF_FORCE);
    u8 bit_size = guest::load8(ctx + HF_BIT_SIZE);
    if (header_size) {
        if (header_size == 8) {
            bit_size = src[0] & 0xF;
            guest::store8(ctx + HF_BIT_SIZE, bit_size);
            if ((src[0] & 0xF0) != 0x20 || (bit_size != 4 && bit_size != 8))
                return;
        }
        u32 used = 0;
        for (u32 left = len;;) {
            u8 hs = --header_size;
            if (hs <= 3)
                dest_count |= (s32)((u32)src[used] << ((3 - hs) * 8));
            else if (hs <= 6)
                dest_count |= (s32)((u32)src[used] << ((6 - hs) * 8));
            used++;
            if (header_size == 4 && dest_count > 0)
                header_size = 0;
            if (--left == 0 || header_size == 0)
                break;
        }
        if (!header_size && force > 0 && force < dest_count)
            dest_count = force;
        guest::store8(ctx + HF_HEADER_SIZE, header_size);
        guest::store32(ctx + HF_DEST_COUNT, (u32)dest_count);
        len -= used, src += used;
        if (!len)
            return;
    }

    u32 tree_p = guest::load32(ctx + HF_TREE_P);
    s16 tree_size = (s16)guest::load16(ctx + HF_TREE_SIZE);
    if (tree_size < 0) {  // the tree's size byte: (n + 1) * 2 bytes with it
        u8 b = *src++;
        len--;
        tree_size = (s16)((b + 1) * 2 - 1);
        guest::store8(tree_p, b);
        tree_p++;
    }
    // the tree, into the context
    while (tree_size > 0) {
        if (!len) {
            guest::store32(ctx + HF_TREE_P, tree_p);
            guest::store16(ctx + HF_TREE_SIZE, (u16)tree_size);
            return;
        }
        guest::store8(tree_p, *src++);
        len--;
        tree_p++;
        if (--tree_size == 0) {
            tree_p = ctx + HF_TREE + 1;
            guest::store32(ctx + HF_TREE_P, tree_p);
            guest::store16(ctx + HF_TREE_SIZE, 0);
            if (!g::CXiVerifyHuffmanTable(ctx + HF_TREE, bit_size))
                return;
        }
    }

    // the symbols: each input bit picks a child, until a leaf
    u32 src_tmp = guest::load32(ctx + HF_SRC_TMP), dest_tmp = guest::load32(ctx + HF_DEST_TMP);
    u8 src_cnt = guest::load8(ctx + HF_SRC_TMP_CNT), dest_cnt = guest::load8(ctx + HF_DEST_TMP_CNT);
    u32 dest = guest::load32(ctx + HF_DEST);
    const u32 root = ctx + HF_TREE + 1;
    const u8* tree = bytes(ctx);  // the context (with its tree) as host memory
    if (!tree)
        return;
    auto node_at = [&](u32 a) { return tree[a - ctx]; };
    while (dest_count > 0) {
        if (src_cnt < 32) {  // fill the input word (little-endian)
            if (!len)
                break;
            src_tmp |= (u32)*src++ << src_cnt;
            src_cnt += 8;
            len--;
            continue;
        }
        while (src_cnt && dest_count > 0) {
            u32 bit = src_tmp >> 31;
            src_tmp <<= 1;
            src_cnt--;
            u8 node = node_at(tree_p);
            bool leaf = ((node << bit) & 0x80) != 0;
            tree_p = (tree_p & ~1u) + ((node & 0x3F) + 1) * 2 + bit;
            if (!leaf)
                continue;
            dest_tmp = (dest_tmp >> bit_size) | ((u32)node_at(tree_p) << (32 - bit_size));
            tree_p = root;
            dest_cnt += bit_size;
            if (dest_count <= (s32)((dest_cnt >> 3) & 0x1F)) {  // the last word: align it
                dest_tmp >>= 32 - dest_cnt;
                dest_cnt = 32;
            }
            if (dest_cnt == 32) {
                guest::store32(dest, __builtin_bswap32(dest_tmp));
                dest += 4;
                dest_count -= 4;
                dest_cnt = 0;
            }
        }
    }
    guest::store32(ctx + HF_DEST, dest);
    guest::store32(ctx + HF_DEST_COUNT, (u32)dest_count);
    guest::store32(ctx + HF_TREE_P, tree_p);
    guest::store32(ctx + HF_SRC_TMP, src_tmp);
    guest::store32(ctx + HF_DEST_TMP, dest_tmp);
    guest::store8(ctx + HF_SRC_TMP_CNT, src_cnt);
    guest::store8(ctx + HF_DEST_TMP_CNT, dest_cnt);
}

// r3: the context, r4: the compressed bytes, r5: how many. Returns the bytes still to come (0:
// done), or an error: -1 not LZ data, -3 data past its end, -4 a match past the end.
inline u32 read_uncomp_lz(u32 ctx, u32 data, u32 len) {
    LzState s;
    s.load(ctx);
    const u8* src = bytes(data);
    if (!src || !len)
        return len ? (u32)-1 : (u32)s.dest_count;
    s32 r = read_lz(s, src, len);
    s.save(ctx);
    return (u32)r;
}

}  // namespace engine::cx::reference
