// The SDK's streaming decompression (CX), native: the game feeds compressed data to it in pieces
// as it reads files, and a context in guest memory carries the decoder's state from one piece to
// the next. CXReadUncompLZ: Nintendo's LZ77 ("LZ10", and the extended "LZ11" lengths);
// CXReadUncompHuffman: 4- or 8-bit symbols, a tree of 2-byte nodes, 32-bit little-endian words.
// Each has a fast path where its input allows (LZ: whole flag groups, runs of literals at once,
// matches a word at a time; Huffman: a table for a code's first 10 bits) that leaves the output
// and the context exactly as the streaming code, kept in engine/cx/cx_reference.h, would:
// NOMA_SELFTEST=cx compares them on random streams fed in random pieces, and tools/armbench
// (--cx) on the 3DS's code and the game's own files.
#include "noma_decomp.h"
#include "runtime.h"

namespace g {

namespace {

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

// A poll point, as decompiled loops have at every iteration (NOMA_LOOP), charged with the work
// done since the last one. The SDK's loops had one at every bit or byte; without them a piece of
// input (a font's glyph sheet is a single piece of ~100 KB) ran for tens of milliseconds on the
// 3DS with no interrupt taken: the retrace and the IOS replies waited, and every thread they wake,
// the game's main thread among them.
inline void poll_point(s32 units) {
    if ((guest::g_loop_budget -= units) < 0)
        guest::loop_poll();
}

// the input a flag group can take at most: its flag byte and 8 matches of 4 bytes (LZ11's longest)
const u32 FAST_LZ_INPUT = 1 + 8 * 4;

// n bytes from dist back: a word at a time when the word read was all written before
inline void copy_match(u8* d, u32 dist, u32 n) {
    const u8* s = d - dist;
    if (dist == 1) {
        ::memset(d, *s, n);
        return;
    }
    if (dist < 4) {
        for (; n; n--)
            *d++ = *s++;
        return;
    }
    for (; n >= 4; n -= 4, d += 4, s += 4) {
        u32 w;
        ::memcpy(&w, s, 4);
        ::memcpy(d, &w, 4);
    }
    if (n & 2) {
        u16 h;
        ::memcpy(&h, s, 2);
        ::memcpy(d, &h, 2);
        d += 2, s += 2;
    }
    if (n & 1)
        *d = *s;
}

s32 read_lz(LzState& s, const u8* src, u32 len) {
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
    u8* polled = dst;  // the output at the last poll point
    auto ret = [&](s32 r) {  // the context keeps where the output got to, as the SDK's does
        s.dest += (u32)(dst - dst0);
        return r;
    };
    while (s.dest_count > 0) {
        if (!s.flag_index && len >= FAST_LZ_INPUT && s.length_flg == 3) {
            // whole flag groups while all their input is here, decoded as the streaming code
            // below would, with the state in registers; a match longer than the output left
            // goes to that code (an error, or the forced size's end)
            const u8* const src0 = src;
            const u8* const last = src + len - FAST_LZ_INPUT;  // the last group that can start
            const bool ex = s.ex_format;
            s32 count = s.dest_count;
            u32 flags = 0, n = 0;  // the flags at bits 7 down, n of them unused
            while (n == 0 && count > 0 && src <= last) {
                poll_point(8 + (s32)((dst - polled) >> 4));
                polled = dst;
                flags = *src++;
                n = 8;
                for (;;) {
                    // the literals before the next match, all at once (a 1 after the n flags stops the count)
                    u32 k = (u32)__builtin_clz(flags << 24 | 0x80000000u >> n);
                    if (k) {
                        if ((s32)k >= count) {  // the output ends among them, its last flag kept
                            k = (u32)count;
                            for (u32 i = 0; i < k; i++)
                                dst[i] = src[i];
                            dst += k, src += k, count = 0;
                            flags = (flags << (k - 1)) & 0xFF, n -= k - 1;
                            break;
                        }
                        for (u32 i = 0; i < k; i++)
                            dst[i] = src[i];
                        dst += k, src += k, count -= (s32)k;
                        flags = (flags << k) & 0xFF, n -= k;
                        if (n == 0)
                            break;
                    }
                    u32 b0 = src[0], length, disp, used;
                    if (!ex) {
                        length = (b0 >> 4) + 3, disp = (b0 & 0xF) << 8 | src[1], used = 2;
                    } else if ((b0 >> 4) == 1) {
                        length = ((b0 & 0xF) << 12 | src[1] << 4 | src[2] >> 4) + 0x111;
                        disp = (src[2] & 0xF) << 8 | src[3], used = 4;
                    } else if ((b0 >> 4) == 0) {
                        length = ((b0 & 0xF) << 4 | src[1] >> 4) + 0x11;
                        disp = (src[1] & 0xF) << 8 | src[2], used = 3;
                    } else {
                        length = (b0 >> 4) + 1, disp = (b0 & 0xF) << 8 | src[1], used = 2;
                    }
                    if ((s32)length > count)
                        break;
                    src += used;
                    count -= (s32)length;
                    copy_match(dst, disp + 1, length);
                    dst += length;
                    if (count == 0)
                        break;  // (the streaming code leaves the flags as they are then too)
                    flags = (flags << 1) & 0xFF;
                    if (--n == 0)
                        break;
                }
            }
            s.flags = (u8)flags;
            s.flag_index = (u8)n;
            s.dest_count = count;
            len -= (u32)(src - src0);
            continue;
        }
        if (!s.flag_index) {
            if (!len)
                break;
            poll_point(8 + (s32)((dst - polled) >> 4));  // 8 tokens, and long matches by their size
            polled = dst;
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

}  // namespace

u32 CXiVerifyHuffmanTable(u32 table, u32 bit_size);  // CXiVerifyHuffmanTable

namespace {

// The first HUFF_LUT_BITS bits of a symbol, looked up instead of walked a bit at a time: from the
// root, the leaf they reach (its symbol and length) or the node after all of them. Built for the
// tree a context holds (once it has passed CXiVerifyHuffmanTable), and kept while the context
// holds the same tree: the glyph sheets of a font come in pieces, each sheet with its own tree.
const u32 HUFF_LUT_BITS = 10;
enum : u32 { HUFF_LEAF = 0x100 };  // an entry: symbol or node (offset in the context) << 16, leaf, bits
struct HuffLut {
    u32 ctx;
    u8 tree[0x200];  // the tree it was made from: the size byte, then the nodes
    u32 entry[1u << HUFF_LUT_BITS];
};
HuffLut s_lut;

void lut_fill(const u8* tree, u32 ctx, u32 p, u32 depth, u32 code) {
    u8 node = tree[p - ctx];
    for (u32 bit = 0; bit < 2; bit++) {
        u32 child = (p & ~1u) + ((node & 0x3F) + 1) * 2 + bit, c = code << 1 | bit, d = depth + 1;
        if ((node << bit) & 0x80) {
            u32 e = (u32)tree[child - ctx] << 16 | HUFF_LEAF | d;
            for (u32 i = c << (HUFF_LUT_BITS - d), n = 1u << (HUFF_LUT_BITS - d); n; n--, i++)
                s_lut.entry[i] = e;
        } else if (d == HUFF_LUT_BITS) {
            s_lut.entry[c] = (child - ctx) << 16 | d;
        } else {
            lut_fill(tree, ctx, child, d, c);
        }
    }
}

// the table for the tree at ctx (tree: the context in host memory), or none for a tree that
// doesn't verify (it is then walked as it is)
const HuffLut* huff_lut(u32 ctx, const u8* tree, u8 bit_size) {
    const u8* t = tree + HF_TREE;
    u32 size = (t[0] + 1u) * 2;
    if (s_lut.ctx == ctx && !::memcmp(s_lut.tree, t, size))
        return &s_lut;
    s_lut.ctx = 0;
    if (!CXiVerifyHuffmanTable(ctx + HF_TREE, bit_size))
        return nullptr;
    ::memcpy(s_lut.tree, t, size);
    lut_fill(tree, ctx, ctx + HF_TREE + 1, 0, 0);
    s_lut.ctx = ctx;
    return &s_lut;
}

}  // namespace

// r3: the context, r4: the compressed bytes, r5: how many. The context tells how far it got.
void CXReadUncompHuffman(u32 ctx, u32 data, u32 len) {
    NOMA_PROF_SCOPE("(cx huffman)");
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
            if (!CXiVerifyHuffmanTable(ctx + HF_TREE, bit_size))
                return;
        }
    }

    // the symbols: each input bit picks a child, until a leaf (the first bits from the table)
    u32 src_tmp = guest::load32(ctx + HF_SRC_TMP), dest_tmp = guest::load32(ctx + HF_DEST_TMP);
    u8 src_cnt = guest::load8(ctx + HF_SRC_TMP_CNT), dest_cnt = guest::load8(ctx + HF_DEST_TMP_CNT);
    u32 dest = guest::load32(ctx + HF_DEST);
    const u32 root = ctx + HF_TREE + 1;
    const u8* tree = bytes(ctx);  // the context (with its tree) as host memory
    if (!tree)
        return;
    auto node_at = [&](u32 a) { return tree[a - ctx]; };
    const HuffLut* lut = dest_count > 0 && len ? huff_lut(ctx, tree, bit_size) : nullptr;
    // (working copies in registers; the u8 counters wrap as the context's bytes would)
    u32 sbits = src_cnt, dbits = dest_cnt, tp = tree_p;
    const u32 bs = bit_size, sym_shift = 32 - bit_size;
    while (dest_count > 0) {
        if (sbits < 32) {  // fill the input word (little-endian)
            if (!len)
                break;
            if (sbits == 0 && len >= 4) {  // all of it, as four bytes would
                poll_point(32);
                src_tmp = src[0] | src[1] << 8 | src[2] << 16 | (u32)src[3] << 24;
                src += 4, len -= 4, sbits = 32;
            } else {
                poll_point(8);  // a byte's 8 bits
                src_tmp |= (u32)*src++ << sbits;
                sbits = (sbits + 8) & 0xFF;
                len--;
                continue;
            }
        }
        // a symbol into the output word; a whole word stored (and dest_count lowered: false when it
        // reaches 0)
        auto emit = [&](u32 sym) {
            dest_tmp = (dest_tmp >> bs) | (sym << sym_shift);
            dbits = (dbits + bs) & 0xFF;
            if (dest_count <= (s32)((dbits >> 3) & 0x1F)) {  // the last word: align it
                dest_tmp >>= 32 - dbits;
                dbits = 32;
            }
            if (dbits == 32) {
                guest::store32(dest, __builtin_bswap32(dest_tmp));
                dest += 4;
                dest_count -= 4;
                dbits = 0;
            }
            return dest_count > 0;
        };
        while (sbits && dest_count > 0) {
            if (lut && tp == root) {
                // whole symbols from the table, until it needs more bits than are left or the
                // output ends; a code longer than the table goes on from its node, a bit at a time
                for (;;) {
                    u32 e = lut->entry[src_tmp >> (32 - HUFF_LUT_BITS)], n = e & 0xFF;
                    if (n > sbits)
                        break;
                    src_tmp <<= n;
                    sbits -= n;
                    if (!(e & HUFF_LEAF)) {
                        tp = ctx + (e >> 16);
                        break;
                    }
                    if (!emit(e >> 16))
                        break;
                }
                if (!sbits || dest_count <= 0)
                    break;
            }
            u32 bit = src_tmp >> 31;
            src_tmp <<= 1;
            sbits--;
            u8 node = node_at(tp);
            bool leaf = ((node << bit) & 0x80) != 0;
            tp = (tp & ~1u) + ((node & 0x3F) + 1) * 2 + bit;
            if (!leaf)
                continue;
            u32 sym = node_at(tp);
            tp = root;
            emit(sym);
        }
    }
    src_cnt = (u8)sbits, dest_cnt = (u8)dbits, tree_p = tp;
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
u32 CXReadUncompLZ(u32 ctx, u32 data, u32 len) {
    NOMA_PROF_SCOPE("(cx lz)");
    LzState s;
    s.load(ctx);
    const u8* src = bytes(data);
    if (!src || !len)
        return len ? (u32)-1 : (u32)s.dest_count;
    s32 r = read_lz(s, src, len);
    s.save(ctx);
    return (u32)r;
}

}  // namespace g
