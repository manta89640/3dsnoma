// nw4r::lyt's draw, native (see draw.h): the walk of a layout's panes, the two entry points that
// replace layout::LayoutWrapper's Draw and Update, the arena the packets go through to the renderer,
// and NOMA_LYT_STATS.
//
// The walk does what the decompiled draw does, call for call, without its virtual calls and its GX
// traffic: layout_LayoutWrapper_Draw (the gate, the 2D state and the projection), nw4r's Layout::Draw
// (the root pane), Pane::Draw (the visible bit at draw time, DrawSelf, the children in list order),
// the game's layout::MaskPane and layout::MaskedPane (Pane::Draw with the colour updates and the Z
// mode switched around the children), and the DrawSelf of each class: Picture (one quad), Window
// (the content, then 1, 4 or 8 frame quads) and TextBox (draw_text.cpp). Each class is recognised by
// the code address in its vtable (tree.md §2.1: the program has these seven pane classes and no
// other); anything else, and anything a native part says it can't do, is drawn by the game's own
// code (a fallback: the quads built so far go out first, then the guest's Draw or DrawSelf, with the
// GX state the mask panes set; the walk goes on after it with a fresh packet).
//
// The GX state the walk keeps itself is what the mask panes change: CMODE0's colour and alpha update
// and the Z mode (DrawCtx). Like the GX library's, it is one state the panes change as they are
// drawn, not a stack: a mask pane sets fixed values after its children, and the panes after it are
// drawn with them.
//
// Floating point: positions, texture coordinates and frame sizes are computed in the decompiled
// functions' order, one expression for each of theirs (the host's compiler fuses a * b + c within an
// expression, the 3DS's doesn't), and the transform unit's two (draw.h mtx_apply, texgen below) in
// runtime/hw/gx/xform.cpp's shape, so that the host's renderer gets the floats the decompiled draw's
// vertices would have given it.
#include "engine/lyt/draw.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "engine/math/vecmath.h"
#include "noma_decomp.h"
#include "runtime/hw/gx/gx.h"
#include "runtime/hw/hw.h"
#include "runtime/runtime.h"

namespace g {
// (the decompiled ones)
u32 layout_LayoutWrapper_Draw_orig(u32 r3);
u32 layout_LayoutWrapper_Update_orig(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7, u32 r8, u32 r9, u32 r10, f64 f1, f64 f2,
                                     f64 f3, f64 f4, f64 f5, f64 f6, f64 f7, f64 f8);
}  // namespace g

namespace engine::lyt {

using engine::math::load_f32;
using guest::load16;
using guest::load32;
using guest::load8;

namespace {

// ---------------------------------------------------------------- what the walk knows

// The classes, by the code their vtables name (tree.md §2.1, callers.md §1.4)
constexpr u32 LAYOUT_DRAW = 0x8045A730;     // nw4r::lyt::Layout::Draw (Layout vtable +0x34)
constexpr u32 PANE_DRAW = 0x80459090;       // Pane::Draw (pane vtable +0x14)
constexpr u32 MASK_DRAW = 0x801188CC;       // layout::MaskPane::Draw
constexpr u32 MASKED_DRAW = 0x801187BC;     // layout::MaskedPane::Draw
constexpr u32 PANE_SELF = 0x80459120;       // Pane::DrawSelf (+0x18): nothing (Pane, the mask panes)
constexpr u32 BOUNDING_SELF = 0x8045F200;   // Bounding::DrawSelf: nothing
constexpr u32 PICTURE_SELF = 0x8045B1F0;
constexpr u32 WINDOW_SELF = 0x8045D130;
constexpr u32 TEXTBOX_SELF = 0x8045B880;
constexpr u32 PANE_LOADMTX = 0x80459610;    // (+0x70) the pane's global matrix as PNMTX0
constexpr u32 TEXTBOX_LOADMTX = 0x8045C680; // the same with its Y column negated (draw_text.cpp's)
constexpr u32 WINDOW_CONTENT = 0x8045D430;  // Window +0x7C DrawContent, +0x80 DrawFrame, +0x84
constexpr u32 WINDOW_FRAME1 = 0x8045D540;   // DrawFrame4, +0x88 DrawFrame8
constexpr u32 WINDOW_FRAME4 = 0x8045DB50;
constexpr u32 WINDOW_FRAME8 = 0x8045E290;
constexpr u32 MATERIAL_SETUP_GX = 0x80460EF0;  // Material vtable +0x0C (the only Material class)

// What the decompiled DrawSelf functions leave in r3 for Pane::Draw to return: a quad's, what
// DrawQuad leaves there (engine/lyt/quad.cpp); a text box's, its TextWriter (the destructor,
// nw4r_ut_TextWriterBaseW_dtor, returns it), which lives in the guest stack frame of TextBox::DrawSelf (GFRAME 0x1C0,
// the writer at +0xCC), below the frames of the mask panes it is drawn inside (GFRAME 0x20 each;
// Layout::Draw and Pane::Draw have none)
constexpr u32 QUAD_RETURN = 0xCC010000;
constexpr u32 TEXTBOX_FRAME = 0x1C0, TEXTBOX_WRITER = 0xCC, MASK_FRAME = 0x20;

// GXCompare
constexpr u32 GX_NEVER = 0, GX_LEQUAL = 3, GX_GEQUAL = 6, GX_ALWAYS = 7;
// __GXData: the shadows of CMODE0 (BP 0x41) and ZMODE (BP 0x40), and the flag GXBeginDisplayList
// sets while a display list is recorded (GXEndDisplayList clears it)
constexpr u32 GXDATA_CMODE0 = 0x220, GXDATA_ZMODE = 0x228, GXDATA_IN_DISP_LIST = 0x5F8;
// CMODE0's dither, colour update and alpha update bits (the ones GXSetBlendMode keeps)
constexpr u32 CMODE0_KEEP = 0x1C, CMODE0_COLOR_UPDATE = 0x8, CMODE0_ALPHA_UPDATE = 0x10;

// ---------------------------------------------------------------- small pieces of the decompiled draw

// detail::IsModulateVertexColor (nw4r_lyt_detail_IsModulateVertexColor): the vertex colours (4, or none) or the alpha change
// the picture
bool modulate(const u32* vc, u8 alpha) {
    if (alpha != 0xFF)
        return true;
    return vc && (vc[0] != 0xFFFFFFFF || vc[1] != 0xFFFFFFFF || vc[2] != 0xFFFFFFFF || vc[3] != 0xFFFFFFFF);
}

// DrawQuad_Alpha's colour: the alpha byte times alpha / 255, rounded down (the original's multiply by
// 0x80808081, as quad.cpp has it)
u32 times_alpha(u32 v, u32 alpha) {
    if (alpha == 0xFF)
        return v;
    s32 t = (s32)((v & 0xFF) * alpha);
    s32 q = (s32)(((s64)(s32)0x80808081 * t) >> 32) + t;
    q >>= 7;
    return (v & ~0xFFu) | (u8)(q + (s32)((u32)q >> 31));
}

// Pane::GetVtxPos (nw4r_lyt_Pane_GetVtxPos): the top left corner of the pane's rectangle in its own
// space, by its base position (H = % 3, V = / 3)
void vtx_pos(u32 p, f32& x, f32& y) {
    const u8 base = load8(p + 0xBA);
    switch (base % 3) {
    case 1: x = -load_f32(p + 0x4C) * 0.5f; break;
    case 2: x = -load_f32(p + 0x4C); break;
    default: x = 0.0f; break;
    }
    switch (base / 3) {
    case 1: y = load_f32(p + 0x50) * 0.5f; break;
    case 2: y = load_f32(p + 0x50); break;
    default: y = 0.0f; break;
    }
}

// a material's texture map count (+0x3C, bits 28-31)
bool has_texture(u32 mat) { return (load32(mat + 0x3C) >> 28) != 0; }

// detail::GetTextureSize(material, 0) (nw4r_lyt_detail_GetTextureSize): texture map 0's size, or
// (0, 0) when the material has no texture
void texture_size(u32 mat, f32 out[2]) {
    if (!has_texture(mat)) {
        out[0] = out[1] = 0.0f;
        return;
    }
    const u32 map = load32(mat + 0x40);
    out[1] = (f32)(u32)load16(map + 0xA);
    out[0] = (f32)(u32)load16(map + 0x8);
}

// a material the native SetupGX describes (material_state): nw4r's (its SetupGX in the vtable)
bool nw4r_material(u32 mat) { return load32(load32(mat) + 0xC) == MATERIAL_SETUP_GX; }

// A pane's texture coordinate sets (TexCoords: 0x20 bytes, the corners LT, RT, LB, RB, s and t
// each): the ones of the `n` the vertices carry (at most eight) that the material's texgens read.
// Returns how many were read: a texgen reading a set past them reads what a vertex without it has.
u32 read_sets(u32 coords, u32 n, const LytMaterial& lm, f32 out[8][4][2]) {
    u32 used = 0;
    for (u32 k = 0; k < lm.n_gens && k < LYT_MAX_COORDS; k++)
        used = std::max(used, lm.gen[k].src + 1);
    n = std::min<u32>({n, used, 8u});
    for (u32 k = 0; k < n; k++)
        for (u32 c = 0; c < 4; c++)
            for (u32 a = 0; a < 2; a++)
                out[k][c][a] = load_f32(coords + k * 0x20 + c * 8 + a * 4);
    return n;
}

// texture coordinate k of a vertex: its generator's matrix times (s, t, 1, 1) of the set it reads
// (as the transform unit computes it: runtime/hw/gx/xform.cpp, the same expression)
void texgen(const LytTexGen& gen, const f32 st[2], f32 out[2]) {
    const f32 src[4] = {st[0], st[1], 1.0f, 1.0f};
    for (int i = 0; i < 2; i++)
        out[i] = gen.m[i][0] * src[0] + gen.m[i][1] * src[1] + gen.m[i][2] * src[2] + gen.m[i][3] * src[3];
}

// One window frame quad's texture coordinates (one set: LT, RT, LB, RB, s and t each), as Window's
// DrawFrame, DrawFrame4 and DrawFrame8 make them for the quad at the window's corner `k` (0 LT, 1 RT,
// 2 LB, 3 RB, the corner the frame texture is anchored at): the flip's (nw4r_lyt_Window_sTextureFlipInfo: 6 entries of
// 10 bytes, the (s, t) of the corners LT, RT, LB, RB, then ix, iy) corner k at the quad's corner k,
// and towards the other corners the texture at its own size, the quad's width along axis ix and its
// height along iy (a rotation swaps them); past the texture's edge it is clamped, which is how one
// corner's texture makes the edge beside it.
void frame_coords(u32 flip, u32 k, f32 pw, f32 ph, const f32 tex[2], f32 tc[4][2]) {
    static const u8 k_across[4] = {1, 0, 3, 2};  // the corner beside k: LT-RT, LB-RB
    static const u8 k_along[4] = {2, 3, 0, 1};   // the corner above or below k: LT-LB, RT-RB
    const u32 e = GA_nw4r_lyt_Window_sTextureFlipInfo + flip * 10;
    const u32 ix = load8(e + 8), iy = load8(e + 9);
    const u32 h = k_across[k], v = k_along[k], o = k_along[h];
    auto c = [e](u32 corner, u32 axis) { return (u32)load8(e + corner * 2 + axis); };
    tc[k][ix] = tc[v][ix] = (f32)c(k, ix);
    tc[k][iy] = tc[h][iy] = (f32)c(k, iy);
    const f32 s = (f32)c(k, ix) + (pw / ((f32)(s32)(c(h, ix) - c(k, ix)) * tex[ix]));
    const f32 t = (f32)c(k, iy) + (ph / ((f32)(s32)(c(v, iy) - c(k, iy)) * tex[iy]));
    tc[h][ix] = tc[o][ix] = s;
    tc[v][iy] = tc[o][iy] = t;
}

// a flip the table has (the decompiled code would read past it for another)
bool known_flip(u32 flip) { return flip <= 5; }

// the Z mode word (BP 0x40) GXSetZMode sends: the bits it keeps from its shadow are none GXInit
// sets and nothing else writes
u32 zmode_word(u32 enable, u32 func, u32 update) { return (enable & 1) | ((func << 1) & 0xE) | ((update << 4) & 0x10); }

// ---------------------------------------------------------------- the fallbacks

enum Fallback { FB_LAYOUT, FB_PANE, FB_SELF, FB_LOADMTX, FB_MATERIAL, FB_PICTURE, FB_WINDOW, FB_TEXT, FB_KINDS };
const char* const k_fallback_what[FB_KINDS] = {
    "a Layout class of its own (its Draw)",
    "a pane class of its own (its Draw)",
    "a pane class of its own (its DrawSelf)",
    "a pane with a LoadMtx of its own",
    "a material of a class of its own",
    "a picture's material the native state doesn't describe",
    "a window the native draw doesn't do",
    "a text box the native draw doesn't do",
};
bool s_fallback_logged[FB_KINDS];

// the first of each kind is logged, with the object's vtable and (a pane's) name (+0xBC, as
// Pane::FindPaneByName compares it)
void log_fallback(Fallback kind, u32 obj, u32 code, bool pane) {
    if (s_fallback_logged[kind])
        return;
    s_fallback_logged[kind] = true;
    const std::string name = pane ? guest::read_string(obj + 0xBC, 16) : std::string();
    guest::log("lyt draw: %s: drawn by the game's code (%s %08X%s%s, vtable %08X, code %08X)", k_fallback_what[kind],
               pane ? "pane" : "object", obj, pane ? " " : "", name.c_str(), load32(obj), code);
}

// ---------------------------------------------------------------- the arena

// The packets' bytes, a ring of 1 MB allocated once. Positions count up (u32, wrapping); a packet
// goes in at a position whose offset in the ring is 16-byte aligned, as an entry: a head of the
// arena's own (16 bytes: the position, the entry's size, a tag), then the LytPacket block. The
// command carries the position (its low 4 bits 0: references with any of them set are the checks'
// markers). An entry that doesn't fit before the ring's end goes at its start.
//
// The CPU's thread writes an entry, then publishes its end (release), then writes the command. The
// decoder's thread takes a reference only between the end of the last entry it decoded (`read`, the
// space before it free again) and the end published (acquire), and only if the entry's head names
// it: a stale reference (a command decoded twice, a packet given up on) or a garbage one is dropped
// and counted, never drawn. Packets are decoded in the order they were written (the FIFO's), so one
// read position frees them all.
constexpr u32 ARENA_BYTES = 1u << 20;
constexpr u32 ARENA_MASK = ARENA_BYTES - 1;
// the largest entry: a builder bigger than this goes out as several packets (it leaves room for
// three more, so that the CPU's thread can write the next while the decoder draws one)
constexpr u32 ENTRY_MAX = ARENA_BYTES / 4;
constexpr u32 ENTRY_TAG = 0x4C595450;  // "LYTP"

struct EntryHead {
    u32 ref;    // the entry's position (what its command carries)
    u32 bytes;  // the entry's size, this head included (a multiple of 16)
    u32 tag;    // ENTRY_TAG
    u32 pad;
};
static_assert(sizeof(EntryHead) == 16, "the packet after the head is 16-byte aligned");

constexpr u32 align16(u32 n) { return (n + 15) & ~15u; }
constexpr u32 PACKET_HEAD = align16((u32)sizeof(LytPacket));

struct Arena {
    u8* mem = nullptr;
    u32 write = 0;                  // (the CPU's thread) where the next entry goes
    u32 serial = 0;                 // (the CPU's thread) packets written
    std::atomic<u32> published{0};  // the end of the last entry whose command may be decoded
    std::atomic<u32> read{0};       // the end of the last entry decoded: the space before it is free
    // counts (NOMA_LYT_STATS): the CPU's thread's, and the decoder's
    u32 waits = 0, lost = 0;
    u64 bytes = 0;
    std::atomic<u32> decoded{0}, stale{0};
};
Arena s_arena;

// what the pipe holds goes to the FIFO now: NOPs to the end of its 32-byte burst (GXFlush's way)
void flush_pipe() {
    while (guest::g_gather_n)
        guest::gx_fifo_write(0, 0, 1);
}

// the command: opcode and the entry's position, big-endian, through the write-gather pipe
void write_command(u32 ref) {
    guest::gx_fifo_write(0, LYT_OPCODE, 1);
    guest::gx_fifo_write(0, ref, 4);
}

// Room for an entry of `need` bytes: where it goes (its position in `at`). When the ring is full the
// decoder is behind: the pipe is flushed and the CPU's thread waits for the GPU's to decode all it
// has (gx_sync). If the space still isn't free then, no decoder is running the FIFO: the packets not
// decoded never will be, and their space is taken back.
u8* reserve(u32 need, u32& at) {
    Arena& a = s_arena;
    if (!a.mem) {
        // (once, for the program's life; 16-byte aligned)
        static u8* const s_block = (u8*)::malloc(ARENA_BYTES + 15);
        if (!s_block)
            guest::fatal("lyt draw: no memory for the packets' arena (%u bytes)", ARENA_BYTES);
        a.mem = (u8*)(((uintptr_t)s_block + 15) & ~(uintptr_t)15);
    }
    at = a.write;
    const u32 off = at & ARENA_MASK;
    if (off + need > ARENA_BYTES)
        at += ARENA_BYTES - off;
    if (at + need - a.read.load(std::memory_order_acquire) > ARENA_BYTES) {
        a.waits++;
        flush_pipe();
        guest::gx_sync();
        if (at + need - a.read.load(std::memory_order_acquire) > ARENA_BYTES) {
            const u32 outstanding = a.serial - a.decoded.load(std::memory_order_relaxed) - a.lost;
            if (a.lost == 0)
                guest::log("lyt draw: %u packets were never decoded (no renderer running the FIFO?): given up on",
                           outstanding);
            a.lost += outstanding;
            a.read.store(a.write, std::memory_order_release);
        }
    }
    return a.mem + (at & ARENA_MASK);
}

// a packet's part of a builder: runs [first_run, first_run + n_runs) clipped to the vertices
// [v0, v1), with the states [lo, hi] they use
struct Piece {
    u32 first_run = 0, n_runs = 0;
    u32 v0 = 0, v1 = 0;
    u32 lo = 0, hi = 0;
};

// an entry's bytes before its vertices
u32 entry_fixed(u32 n_states, u32 n_runs) {
    return (u32)sizeof(EntryHead) + PACKET_HEAD + align16(n_states * (u32)sizeof(LytState)) +
           align16(n_runs * (u32)sizeof(LytRun));
}

void write_packet(const Builder& b, const Piece& p) {
    Arena& a = s_arena;
    const u32 n_states = p.hi - p.lo + 1, n_vertices = p.v1 - p.v0;
    const u32 states_at = PACKET_HEAD;
    const u32 runs_at = states_at + align16(n_states * (u32)sizeof(LytState));
    const u32 vertices_at = runs_at + align16(p.n_runs * (u32)sizeof(LytRun));
    const u32 bytes = vertices_at + n_vertices * (u32)sizeof(LytVertex);
    const u32 need = align16((u32)sizeof(EntryHead) + bytes);
    u32 at;
    u8* e = reserve(need, at);
    EntryHead* head = (EntryHead*)e;
    LytPacket* pk = (LytPacket*)(e + sizeof(EntryHead));
    u8* base = (u8*)pk;
    head->ref = at, head->bytes = need, head->tag = ENTRY_TAG, head->pad = 0;
    pk->bytes = bytes;
    pk->serial = ++a.serial;
    pk->wrapper = b.wrapper;
    pk->n_states = n_states, pk->n_runs = p.n_runs, pk->n_vertices = n_vertices;
    pk->states_at = states_at, pk->runs_at = runs_at, pk->vertices_at = vertices_at;
    ::memcpy(base + states_at, &b.states[p.lo], n_states * sizeof(LytState));
    LytRun* runs = (LytRun*)(base + runs_at);
    for (u32 i = 0; i < p.n_runs; i++) {
        const LytRun& r = b.runs[p.first_run + i];
        const u32 first = std::max(r.first, p.v0), end = std::min(r.first + r.count, p.v1);
        runs[i] = {r.state - p.lo, first - p.v0, end - first};
    }
    ::memcpy(base + vertices_at, &b.vertices[p.v0], n_vertices * sizeof(LytVertex));
    // published before the command: on a host without a GPU thread the decoder runs inline and
    // may take it before gx_fifo_write returns
    a.write = at + need;
    a.bytes += need;
    a.published.store(a.write, std::memory_order_release);
    write_command(at);
}

// ---------------------------------------------------------------- NOMA_LYT_STATS

// NOMA_LYT_STATS=N: a line every N game frames: the CPU's time a frame in the layouts' draws (the
// whole of each layout_LayoutWrapper_Draw: the walk and the packets, or the decompiled draw) and
// updates (layout_LayoutWrapper_Update: animation and matrices), the mean, median and worst frame;
// and a frame's counts. "Drawn" panes are the visible pictures, windows and text boxes.
struct Stats {
    u32 every = 0;
    bool started = false;
    u32 frame = 0;                   // the game frame being counted
    u64 draw_t = 0, update_t = 0;    // its time (host_timebase ticks)
    std::vector<u64> draws, updates; // the period's frames'
    u64 calls = 0, orig = 0, lists = 0, fallbacks = 0, panes = 0, drawn = 0, quads = 0, glyphs = 0, states = 0,
        runs = 0;
    // the arena's counts when the period started
    u32 packets0 = 0, waits0 = 0, lost0 = 0, stale0 = 0;
    u64 bytes0 = 0;
};

u32 stats_every() {
    static const u32 s_every = [] {
        const char* e = ::getenv("NOMA_LYT_STATS");
        const int n = e ? ::atoi(e) : 0;
        if (n > 0)
            guest::log("lyt stats: a line every %d game frames", n);
        return n > 0 ? (u32)n : 0u;
    }();
    return s_every;
}

Stats& stats() {
    static Stats s_stats = [] {
        Stats s;
        s.every = stats_every();
        s.draws.reserve(s.every), s.updates.reserve(s.every);
        return s;
    }();
    return s_stats;
}

void stats_period_start(Stats& s) {
    const Arena& a = s_arena;
    s.draws.clear(), s.updates.clear();
    s.calls = s.orig = s.lists = s.fallbacks = s.panes = s.drawn = s.quads = s.glyphs = s.states = s.runs = 0;
    s.packets0 = a.serial, s.waits0 = a.waits, s.lost0 = a.lost, s.bytes0 = a.bytes;
    s.stale0 = a.stale.load(std::memory_order_relaxed);
}

// the mean, median and worst of a period's frames, in ms
void summary(std::vector<u64>& v, double out[3]) {
    const double ms = 1000.0 / (double)guest::TB_HZ;
    u64 sum = 0;
    for (u64 t : v)
        sum += t;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    const u64 median = v[v.size() / 2];
    out[0] = (double)sum * ms / (double)v.size();
    out[1] = (double)median * ms;
    out[2] = (double)*std::max_element(v.begin(), v.end()) * ms;
}

void stats_log(Stats& s) {
    const Arena& a = s_arena;
    const double n = (double)s.draws.size();
    double d[3], u[3];
    summary(s.draws, d);
    summary(s.updates, u);
    guest::log("lyt stats: %u frames: draw %.2f ms a frame (median %.2f, worst %.2f), update %.2f ms (median %.2f, "
               "worst %.2f); a frame: %.1f layout draws (%.1f by the game's code, %.1f of them while a display list "
               "was recorded), %.1f panes visited, %.1f drawn, %.2f by the game's code (fallbacks), %.1f quads (%.1f "
               "glyphs), %.1f states, %.1f runs, %.1f packets (%.1f KB); %u arena waits, %u packets lost, %u stale "
               "commands",
               (u32)s.draws.size(), d[0], d[1], d[2], u[0], u[1], u[2], (double)s.calls / n,
               (double)(s.orig + s.lists) / n, (double)s.lists / n, (double)s.panes / n, (double)s.drawn / n,
               (double)s.fallbacks / n, (double)s.quads / n, (double)s.glyphs / n, (double)s.states / n,
               (double)s.runs / n, (double)(a.serial - s.packets0) / n, (double)(a.bytes - s.bytes0) / 1024.0 / n,
               a.waits - s.waits0, a.lost - s.lost0, a.stale.load(std::memory_order_relaxed) - s.stale0);
    stats_period_start(s);
}

// at each draw and update: a new game frame closes the one counted; every N frames, the line
void stats_frame(Stats& s) {
    const u32 f = guest::vi_game_frames();
    if (!s.started) {
        s.started = true;
        s.frame = f;
        stats_period_start(s);
        return;
    }
    if (f == s.frame)
        return;
    s.draws.push_back(s.draw_t), s.updates.push_back(s.update_t);
    s.draw_t = s.update_t = 0;
    // game frames with no layout drawn or updated in them cost nothing
    for (u32 k = s.frame + 1; k != f && s.draws.size() < s.every; k++)
        s.draws.push_back(0), s.updates.push_back(0);
    s.frame = f;
    if (s.draws.size() >= s.every)
        stats_log(s);
}

// what a walk built (at each packet it sends, and at its end)
void stats_built(Stats* s, const Builder& b) {
    if (!s)
        return;
    s->quads += b.vertices.size() / 4;
    s->states += b.states.size();
    s->runs += b.runs.size();
}

// ---------------------------------------------------------------- the walk

// One layout draw's walk: from the layout's root down, the quads into b. Native (`native`): what the
// walk can't do is drawn by the game's code in place, between packets; in the check modes nothing is
// drawn or sent (the fallbacks are only counted).
class Walk {
public:
    Walk(Builder& b, u32 di, bool native, Stats* stats)
        : b_(b), di_(di), native_(native), stats_(stats), gx_(load32(GA___GXData)), sp_(guest::g_sp) {
        // the colour and alpha updates and dither as the GX library has them now (GX not set up:
        // updates on); the Z mode as layout_DrawInfo_SetupGX2D leaves it, off
        const u32 cmode0 = gx_ ? load32(gx_ + GXDATA_CMODE0) : CMODE0_COLOR_UPDATE | CMODE0_ALPHA_UPDATE;
        cur_.cmode0_keep = cmode0 & CMODE0_KEEP;
        cur_.zmode = zmode_word(0, GX_NEVER, 0);
    }

    // nw4r::lyt::Layout::Draw by the layout's vtable: the root pane's Draw
    u32 layout(u32 layout) {
        const u32 draw = load32(load32(layout) + 0x34);
        if (draw != LAYOUT_DRAW) {
            note(FB_LAYOUT, layout, draw, false);
            return guest_draw(draw, layout);
        }
        const u32 root = load32(layout + 0x10);
        return root ? pane(root) : 0;
    }

    // Native: what the walk built goes out, and the GX library is left as the decompiled draw
    // leaves it for the GX code after the layout: the colour updates and Z mode the mask panes set
    // last, and the TEV's indirect and swap state as a default material leaves it (callers.md §3.3)
    void finish() {
        if (!native_)
            return;
        flush();
        sync_library();
        g::GXSetNumIndStages(0);
        g::GXSetTevDirect(0);                    // GX_TEVSTAGE0
        g::GXSetTevSwapMode(0, 0, 0);            // GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0
        g::GXSetTevSwapModeTable(0, 0, 1, 2, 3); // GX_TEV_SWAP0: GX_CH_RED, GREEN, BLUE, ALPHA
    }

private:
    Builder& b_;
    const u32 di_;
    const bool native_;
    Stats* const stats_;
    const u32 gx_;    // __GXData
    // the guest stack pointer the decompiled walk would be at: the caller's, less the frames of the
    // mask panes being drawn (for a text box's r3, and for the game's code drawing a pane in them)
    u32 sp_;
    DrawCtx cur_;     // the mask panes' state the panes are drawn with now
    // what the mask panes set that the GX library hasn't been told (native: told before the game's
    // code draws, and at the end)
    u32 pending_ = 0;
    enum : u32 { PENDING_COLOR = 1, PENDING_ALPHA = 2, PENDING_Z = 4 };

    // ---- the panes

    // the pane's Draw, by its vtable; r3 as the decompiled Draw leaves it
    u32 pane(u32 p) {
        if (--guest::g_loop_budget < 0)
            guest::loop_poll();
        b_.panes++;
        const u32 vt = load32(p);
        const u32 draw = load32(vt + 0x14);
        switch (draw) {
        case PANE_DRAW:
            // Pane::Draw: when visible, DrawSelf, then the children
            if (!(load8(p + 0xBB) & 1))
                return p;
            return children(p, draw_self(p, vt));
        case MASK_DRAW: {
            // layout::MaskPane::Draw: its children write depth only (colour and alpha updates off,
            // Z compare ALWAYS with update), then colour, alpha and Z back to the layouts' (fixed
            // values: whatever was set before the mask, the panes after it are drawn with these)
            if (!(load8(p + 0xBB) & 1))
                return 0;
            const u32 sp = sp_;
            sp_ -= MASK_FRAME;
            draw_self(p, vt);
            cur_.cmode0_keep &= ~(CMODE0_COLOR_UPDATE | CMODE0_ALPHA_UPDATE);
            cur_.zmode = zmode_word(1, GX_ALWAYS, 1);
            pending_ |= PENDING_COLOR | PENDING_ALPHA | PENDING_Z;
            children(p, 0);
            cur_.cmode0_keep |= CMODE0_COLOR_UPDATE | CMODE0_ALPHA_UPDATE;
            cur_.zmode = zmode_word(0, GX_NEVER, 0);
            pending_ |= PENDING_COLOR | PENDING_ALPHA | PENDING_Z;
            sp_ = sp;
            return 0;
        }
        case MASKED_DRAW: {
            // layout::MaskedPane::Draw: when enabled (+0xD8), its children compare against the
            // depth a mask wrote (+0xD9: GEQUAL, shown where the mask is; else LEQUAL, hidden there),
            // without writing it; then Z off
            if (!(load8(p + 0xBB) & 1))
                return 0;
            const u32 sp = sp_;
            sp_ -= MASK_FRAME;
            draw_self(p, vt);
            if (load8(p + 0xD8)) {
                cur_.zmode = zmode_word(1, load8(p + 0xD9) ? GX_GEQUAL : GX_LEQUAL, 0);
                pending_ |= PENDING_Z;
            }
            children(p, 0);
            cur_.zmode = zmode_word(0, GX_NEVER, 0);
            pending_ |= PENDING_Z;
            sp_ = sp;
            return 0;
        }
        default:
            // a class of its own: its Draw (the pane and its children) in the game's code
            note(FB_PANE, p, draw, true);
            return guest_draw(draw, p);
        }
    }

    // the children's Draw in list order (+0x14: the first node, the list itself its end; a node is
    // its pane + 4, the next node read after the child is drawn, as the decompiled loop does)
    u32 children(u32 p, u32 r) {
        const u32 end = p + 0x14;
        for (u32 node = load32(end); node != end; node = load32(node))
            r = pane(node - 4);
        return r;
    }

    // DrawSelf by the vtable; r3 as the decompiled one leaves it (the empty ones keep the pane)
    u32 draw_self(u32 p, u32 vt) {
        const u32 self = load32(vt + 0x18);
        switch (self) {
        case PANE_SELF:
        case BOUNDING_SELF:
            return p;
        case PICTURE_SELF:
            b_.drawn++, b_.pane = p;
            return picture(p, vt);
        case WINDOW_SELF:
            b_.drawn++, b_.pane = p;
            return window(p, vt);
        case TEXTBOX_SELF:
            b_.drawn++, b_.pane = p;
            return textbox(p, vt);
        default:
            note(FB_SELF, p, self, true);
            return guest_draw(self, p);
        }
    }

    // (the DrawSelf workers stay out of the recursive pane walk's frames: their locals are large)

    // Picture::DrawSelf (nw4r_lyt_Picture_DrawSelf): one quad, the pane's rectangle with its vertex
    // colours (+0xD8: LT, RT, LB, RB) and texture coordinate sets (+0xE9 how many, +0xEC where)
    __attribute__((noinline)) u32 picture(u32 p, u32 vt) {
        const u32 mat = load32(p + 0x28);
        if (!mat)
            return p;
        if (load32(vt + 0x70) != PANE_LOADMTX)
            return self_by_guest(FB_LOADMTX, p, vt, load32(vt + 0x70));
        if (!nw4r_material(mat))
            return self_by_guest(FB_MATERIAL, p, vt, load32(load32(mat) + 0xC));
        const u8 alpha = load8(p + 0xB9);
        u32 vc[4];
        for (u32 i = 0; i < 4; i++)
            vc[i] = load32(p + 0xD8 + 4 * i);
        LytMaterial lm;
        if (!material_state(mat, modulate(vc, alpha), alpha, lm))
            return self_by_guest(FB_PICTURE, p, vt, mat);
        f32 mtx[3][4];
        pane_matrix(p, mtx);
        f32 sets[8][4][2];
        Quad q;
        vtx_pos(p, q.x, q.y);
        q.w = load_f32(p + 0x4C), q.h = load_f32(p + 0x50);
        q.n_sets = read_sets(load32(p + 0xEC), load8(p + 0xE9), lm, sets);
        q.sets = sets;
        u32 colors[4];
        q.colors = lm.use_vtx ? alpha_colors(vc, alpha, colors) : nullptr;
        quad(add_state(lm), lm, mtx, q);
        return QUAD_RETURN;
    }

    // TextBox::DrawSelf: draw_text.cpp's (the matrix's Y negated by its own LoadMtx). With a string,
    // a font and a material it leaves r3 at its TextWriter, else at the pane.
    __attribute__((noinline)) u32 textbox(u32 p, u32 vt) {
        if (load32(vt + 0x70) != TEXTBOX_LOADMTX)
            return self_by_guest(FB_LOADMTX, p, vt, load32(vt + 0x70));
        const Mark m = mark();
        if (!draw_textbox(b_, p, cur_)) {
            rollback(m);
            return self_by_guest(FB_TEXT, p, vt, p);
        }
        return load32(p + 0xD8) && load32(p + 0xE4) && load32(p + 0x28) ? sp_ - TEXTBOX_FRAME + TEXTBOX_WRITER : p;
    }

    // Window::DrawSelf (nw4r_lyt_Window_DrawSelf)
    __attribute__((noinline)) u32 window(u32 p, u32 vt) {
        if (load32(vt + 0x70) != PANE_LOADMTX)
            return self_by_guest(FB_LOADMTX, p, vt, load32(vt + 0x70));
        if (load32(vt + 0x7C) != WINDOW_CONTENT || load32(vt + 0x80) != WINDOW_FRAME1 ||
            load32(vt + 0x84) != WINDOW_FRAME4 || load32(vt + 0x88) != WINDOW_FRAME8)
            return self_by_guest(FB_WINDOW, p, vt, vt);
        const Mark m = mark();
        u32 r;
        if (!window_quads(p, r)) {
            rollback(m);
            return self_by_guest(FB_WINDOW, p, vt, p);
        }
        return r;
    }

    // The window's quads (false: a material or flip the native draw doesn't do; the caller takes
    // back what was added). The frame sizes from the corner frames' textures; the content
    // (DrawContent), then the frames by their count: DrawFrame (1: one material, its texture in all
    // four corners, flipped by fixed entries), DrawFrame4 (each corner its own) or DrawFrame8
    // (corners and edges); another count draws no frame. r: r3 as the decompiled DrawSelf leaves it.
    bool window_quads(u32 p, u32& r) {
        const u32 n = load8(p + 0x104), frames = load32(p + 0x100);
        const u8 alpha = load8(p + 0xB9);
        f32 l = 0.0f, rt = 0.0f, t = 0.0f, bt = 0.0f;  // the frame sizes: left, right, top, bottom
        if (n == 1) {
            f32 s0[2];
            texture_size(load32(frames + 4), s0);
            l = s0[0], rt = s0[0], t = s0[1], bt = s0[1];
        } else if (n == 4 || n == 8) {
            f32 s0[2], s3[2];
            texture_size(load32(frames + 4), s0);
            texture_size(load32(frames + 0x1C), s3);
            l = s0[0], rt = s3[0], t = s0[1], bt = s3[1];
        }
        f32 bx, by;
        vtx_pos(p, bx, by);
        const f32 w = load_f32(p + 0x4C), h = load_f32(p + 0x50);
        f32 mtx[3][4];
        pane_matrix(p, mtx);

        // DrawContent: the rectangle inside the frames, inflated by +0xD8 (left), +0xDC (right),
        // +0xE0 (top), +0xE4 (bottom), its colours +0xE8 and coordinate sets +0xF9, +0xFC
        const u32 cmat = load32(p + 0x28);
        if (!cmat || !nw4r_material(cmat))
            return false;
        u32 vc[4];
        for (u32 i = 0; i < 4; i++)
            vc[i] = load32(p + 0xE8 + 4 * i);
        LytMaterial lm;
        if (!material_state(cmat, modulate(vc, alpha), alpha, lm))
            return false;
        {
            const f32 il = load_f32(p + 0xD8), ir = load_f32(p + 0xDC), it = load_f32(p + 0xE0), ib = load_f32(p + 0xE4);
            f32 sets[8][4][2];
            Quad q;
            const f32 x = bx + l;
            q.x = x - il;
            q.y = it + (by - t);
            q.w = ir + ((il + (w - l)) - rt);
            q.h = ib + ((it + (h - t)) - bt);
            q.n_sets = read_sets(load32(p + 0xFC), load8(p + 0xF9), lm, sets);
            q.sets = sets;
            u32 colors[4];
            q.colors = lm.use_vtx ? alpha_colors(vc, alpha, colors) : nullptr;
            quad(add_state(lm), lm, mtx, q);
        }

        // the frames: white vertex colours (where SetupGX asks for them) with the pane's alpha, one
        // texture coordinate set from the corner helpers; a frame whose material has no texture
        // isn't drawn
        const u32 a = times_alpha(0xFFFFFFFF, alpha);
        const u32 white[4] = {a, a, a, a};
        const bool mod = modulate(nullptr, alpha);
        Frame f(this, mtx, white);
        switch (n) {
        case 1: {
            // DrawFrame: frame 0's material once for the four quads, its flip unused (entries 0, 1, 4
            // and 2: none, H, 180 degrees, V)
            const u32 mat = load32(frames + 4);
            if (!has_texture(mat)) {
                r = p;
                return true;
            }
            if (!nw4r_material(mat) || !material_state(mat, mod, alpha, f.lm))
                return false;
            f.state = add_state(f.lm);
            texture_size(mat, f.tex);
            f.quad(bx, by, w - rt, t, 0, 0);
            f.quad((bx + w) - rt, by, rt, h - bt, 1, 1);
            f.quad(bx + l, bt + (by - h), w - l, bt, 4, 3);
            f.quad(bx, by - t, l, h - t, 2, 2);
            r = QUAD_RETURN;
            return true;
        }
        case 4: {
            // DrawFrame4: each corner frame (8 bytes: u8 flip, Material*) over the edge beside it,
            // in the order LT, RT, RB, LB
            if (!f.frame(frames, 0, mod, alpha) || !f.quad(bx, by, w - rt, t, f.flip, 0))
                return false;
            if (!f.frame(frames, 1, mod, alpha) || !f.quad((bx + w) - rt, by, rt, h - bt, f.flip, 1))
                return false;
            if (!f.frame(frames, 3, mod, alpha) || !f.quad(bx + l, bt + (by - h), w - l, bt, f.flip, 3))
                return false;
            if (!f.frame(frames, 2, mod, alpha) || !f.quad(bx, by - t, l, h - t, f.flip, 2))
                return false;
            // (without a quad drawn last, r3 holds what it was loaded with: frame 2's material)
            r = f.drawn ? QUAD_RETURN : load32(frames + 0x14);
            return true;
        }
        case 8: {
            // DrawFrame8: the corners at their textures' size and the edges between them (frames 4
            // L, 5 R, 6 T, 7 B), in the order LT, T, RT, R, RB, B, LB, L
            if (!f.frame(frames, 0, mod, alpha) || !f.quad(bx, by, l, t, f.flip, 0))
                return false;
            if (!f.frame(frames, 6, mod, alpha) || !f.quad(bx + l, by, (w - l) - rt, t, f.flip, 0))
                return false;
            if (!f.frame(frames, 1, mod, alpha) || !f.quad((bx + w) - rt, by, rt, t, f.flip, 1))
                return false;
            if (!f.frame(frames, 5, mod, alpha) || !f.quad((bx + w) - rt, by - t, rt, (h - t) - bt, f.flip, 1))
                return false;
            if (!f.frame(frames, 3, mod, alpha) || !f.quad((bx + w) - rt, bt + (by - h), rt, bt, f.flip, 3))
                return false;
            if (!f.frame(frames, 7, mod, alpha) || !f.quad(bx + l, bt + (by - h), (w - l) - rt, bt, f.flip, 3))
                return false;
            if (!f.frame(frames, 2, mod, alpha) || !f.quad(bx, bt + (by - h), l, bt, f.flip, 2))
                return false;
            if (!f.frame(frames, 4, mod, alpha) || !f.quad(bx, by - t, l, (h - t) - bt, f.flip, 2))
                return false;
            // (as DrawFrame4: frame 4's material)
            r = f.drawn ? QUAD_RETURN : load32(frames + 0x24);
            return true;
        }
        default:
            // (DrawContent's r3)
            r = QUAD_RETURN;
            return true;
        }
    }

    // one window's frames: the frame being drawn (its material's state, texture size and flip)
    struct Frame {
        Frame(Walk* walk, const f32 (*m)[4], const u32* colors) : w(walk), mtx(m), white(colors) {}
        Walk* w;
        const f32 (*mtx)[4];
        const u32* white;
        LytMaterial lm;
        u32 state = 0;
        f32 tex[2] = {0.0f, 0.0f};
        u32 flip = 0;
        bool skip = false;   // its material has no texture: not drawn
        bool drawn = false;  // the last frame was drawn

        // frame i (DrawFrame4, DrawFrame8): its material's state (false: one the native draw
        // doesn't do), or skip when it has no texture
        bool frame(u32 frames, u32 i, bool mod, u8 alpha) {
            const u32 mat = load32(frames + 8 * i + 4);
            skip = !has_texture(mat);
            drawn = !skip;
            if (skip)
                return true;
            flip = load8(frames + 8 * i);
            if (!known_flip(flip) || !nw4r_material(mat) || !material_state(mat, mod, alpha, lm))
                return false;
            state = w->add_state(lm);
            texture_size(mat, tex);
            return true;
        }
        // the frame's quad at (x, y), qw x qh, its texture anchored at the window's corner k
        bool quad(f32 x, f32 y, f32 qw, f32 qh, u32 fl, u32 k) {
            if (skip)
                return true;
            f32 tc[1][4][2];
            frame_coords(fl, k, qw, qh, tex, tc[0]);
            Quad q;
            q.x = x, q.y = y, q.w = qw, q.h = qh;
            q.n_sets = 1;
            q.sets = tc;
            q.colors = lm.use_vtx ? white : nullptr;
            w->quad(state, lm, mtx, q);
            return true;
        }
    };

    // ---- the quads

    // DrawQuad's quad: base (top left) and size in the pane's space, the texture coordinate sets the
    // vertices carry (each the corners LT, RT, LB, RB), the vertex colours (LT, RT, LB, RB, the alpha
    // multiplied) or none
    struct Quad {
        f32 x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
        u32 n_sets = 0;
        const f32 (*sets)[4][2] = nullptr;
        const u32* colors = nullptr;
    };

    // the run's state: the material's, with the mask panes' Z mode and colour updates
    u32 add_state(LytMaterial& lm) {
        lm.state.zmode = cur_.zmode;
        lm.state.cmode0 = (lm.state.cmode0 & ~CMODE0_KEEP) | cur_.cmode0_keep;
        return b_.add_state(lm.state);
    }

    // the vertex colours as DrawQuad_Alpha sends them
    static const u32* alpha_colors(const u32 vc[4], u8 alpha, u32 out[4]) {
        for (u32 i = 0; i < 4; i++)
            out[i] = times_alpha(vc[i], alpha);
        return out;
    }

    // One quad as GX would have drawn it: DrawQuad's four vertices (top left, top right, bottom
    // right, bottom left: positions x, x + w, y, y - h), through the pane's matrix, with their
    // rasterized colours and their texture coordinates after the material's texgens (a set the
    // vertices don't carry reads (0, 0), as the vertex reader leaves an attribute that isn't sent:
    // runtime/hw/gx/vertex.cpp)
    void quad(u32 state, const LytMaterial& lm, const f32 mtx[3][4], const Quad& q) {
        static const u32 k_corner[4] = {0, 1, 3, 2};  // the vertices' corners in TexCoords' order
        static const f32 k_none[2] = {0.0f, 0.0f};
        LytVertex* v = b_.add_quads(state, 1);
        const f32 x1 = q.x + q.w;
        const f32 y1 = q.y - q.h;
        const f32 px[4] = {q.x, x1, x1, q.x};
        const f32 py[4] = {q.y, q.y, y1, y1};
        u32 vc[4];
        if (q.colors)
            for (u32 i = 0; i < 4; i++)
                vc[i] = q.colors[k_corner[i]];
        u8 rgba[4][4];
        material_colors(lm, q.colors ? vc : nullptr, rgba);
        for (u32 i = 0; i < 4; i++) {
            const f32 pos[4] = {px[i], py[i], 0.0f, 1.0f};
            mtx_apply(mtx, pos, v[i].pos);
            ::memcpy(v[i].color, rgba[i], 4);
            for (u32 k = 0; k < lm.n_gens && k < LYT_MAX_COORDS; k++) {
                const LytTexGen& gen = lm.gen[k];
                const f32* st = gen.src < q.n_sets ? q.sets[gen.src][k_corner[i]] : k_none;
                texgen(gen, st, v[i].tc[k]);
            }
        }
    }

    // ---- what the guest draws

    void note(Fallback kind, u32 obj, u32 code, bool pane) {
        b_.fallbacks++;
        log_fallback(kind, obj, code, pane);
    }

    // a pane's DrawSelf by the game's code (the walk goes on with its children)
    u32 self_by_guest(Fallback kind, u32 p, u32 vt, u32 what) {
        note(kind, p, what, true);
        return guest_draw(load32(vt + 0x18), p);
    }

    // The game's code draws obj (fn: its Draw or DrawSelf), native: after the quads built so far,
    // with the GX library holding the mask panes' state and the guest stack where the decompiled
    // walk would have it; what it leaves of that state is what the walk goes on with, as the
    // decompiled walk would. In the check modes nothing is drawn: the fallback is only counted (and
    // r3 is unknown: 0).
    u32 guest_draw(u32 fn, u32 obj) {
        if (!native_)
            return 0;
        flush();
        sync_library();
        const u32 sp = guest::g_sp;
        guest::g_sp = sp_;
        const u32 r = gcall<u32>(fn, obj, di_);
        guest::g_sp = sp;
        if (gx_) {
            cur_.cmode0_keep = load32(gx_ + GXDATA_CMODE0) & CMODE0_KEEP;
            cur_.zmode = load32(gx_ + GXDATA_ZMODE) & 0xFFFFFF;
        }
        return r;
    }

    // the quads built so far go out (native)
    void flush() {
        if (b_.empty())
            return;
        stats_built(stats_, b_);
        packet_submit(b_);
        b_.states.clear(), b_.runs.clear(), b_.vertices.clear(), b_.quad_pane.clear();
    }

    // the GX library told what the mask panes set since it last was (the calls the decompiled walk
    // made at the mask panes)
    void sync_library() {
        if (!gx_ || !pending_)
            return;
        if (pending_ & PENDING_COLOR)
            g::GXSetColorUpdate((cur_.cmode0_keep & CMODE0_COLOR_UPDATE) ? 1 : 0);
        if (pending_ & PENDING_ALPHA)
            g::GXSetAlphaUpdate((cur_.cmode0_keep & CMODE0_ALPHA_UPDATE) ? 1 : 0);
        if (pending_ & PENDING_Z)
            g::GXSetZMode(cur_.zmode & 1, (cur_.zmode >> 1) & 7, (cur_.zmode >> 4) & 1);
        pending_ = 0;
    }

    // ---- taking back what a pane added (a part the native draw can't do, found halfway)
    struct Mark {
        size_t states, runs, vertices, quad_pane;
        u32 last_count, glyphs;
    };
    Mark mark() const {
        return {b_.states.size(), b_.runs.size(), b_.vertices.size(), b_.quad_pane.size(),
                b_.runs.empty() ? 0u : b_.runs.back().count, b_.glyphs};
    }
    void rollback(const Mark& m) {
        b_.states.resize(m.states);
        b_.runs.resize(m.runs);
        if (m.runs)
            b_.runs.back().count = m.last_count;
        b_.vertices.resize(m.vertices);
        b_.quad_pane.resize(m.quad_pane);
        b_.glyphs = m.glyphs;
    }
};

// ---------------------------------------------------------------- the entry's bookkeeping

// A layout draw can happen inside another (the game's code a fallback runs draws one): each walk in
// progress has a builder of its own.
constexpr u32 MAX_WALKS = 4;
Builder s_builders[MAX_WALKS];
u32 s_walks;   // walks in progress
u32 s_draws;   // layout draws in progress (the outermost is timed)
u32 s_updates; // layout updates in progress

// a layout draw in progress: counted, the outermost timed (NOMA_LYT_STATS)
struct DrawScope {
    Stats* timed;
    u64 t0;
    explicit DrawScope(Stats* s) : timed(s && s_draws == 0 ? s : nullptr), t0(timed ? guest::host_timebase() : 0) {
        s_draws++;
    }
    ~DrawScope() {
        s_draws--;
        if (timed)
            timed->draw_t += guest::host_timebase() - t0;
    }
};

// the draw by the walk: native, or built for the checks
u32 walk_draw(u32 wrapper, u32 di, bool check, Stats* s) {
    Builder& b = s_builders[s_walks++];
    b.reset();
    b.wrapper = wrapper;
    b.record_panes = check;
    u32 r;
    if (check) {
        // (the decompiled draw, which check_draw runs, sets the 2D state and the projection itself)
        Walk w(b, di, false, s);
        r = w.layout(load32(wrapper + 0xC));
        check_draw(wrapper, b);
    } else {
        NOMA_PROF_SCOPE("(lyt draw)");
        // the 2D state and the orthographic projection the packets are drawn with (and DrawInfo's
        // view matrix and rectangle), left for the GX code after the layout as the decompiled draw
        // leaves them
        g::layout_DrawInfo_SetupGX2D();
        g::layout_DrawInfo_SetProjection(di);
        Walk w(b, di, true, s);
        r = w.layout(load32(wrapper + 0xC));
        w.finish();
    }
    if (s) {
        stats_built(s, b);
        s->panes += b.panes, s->drawn += b.drawn, s->glyphs += b.glyphs, s->fallbacks += b.fallbacks;
    }
    s_walks--;
    return r;
}

}  // namespace

// ---------------------------------------------------------------- draw.h

// What Pane::LoadMtx loads as PNMTX0: the global matrix, times DrawInfo's view matrix first when
// DrawInfo's flag 0x80 asks for it (nothing in the channel sets it), with the native PSMTXConcat's
// arithmetic
void pane_matrix(u32 pane, f32 out[3][4]) {
    using namespace engine::math;
    Mtx34 m = load<Mtx34>(pane + 0x84);
    const u32 di = load32(GA_layout_sDrawInfo);
    if (di && (load8(di + 0x50) & 0x80))
        m = concat(load<Mtx34>(di + 0x4), m);
    ::memcpy(out, m.m, sizeof(m.m));
}

// The builder's runs go out as one packet, or as several when it is bigger than an entry (whole
// quads each, in order). The GX library's pending state first, as GXBegin would send it: the
// projection and viewport the packet is drawn with are the GPU's registers at its command.
void packet_submit(const Builder& b) {
    if (b.empty())
        return;
    if (load32(GA___GXData))
        g::__GXSetDirtyState();
    const u32 vertex = (u32)sizeof(LytVertex);
    Piece p;
    bool open = false;
    for (u32 r = 0; r < (u32)b.runs.size(); r++) {
        const LytRun& run = b.runs[r];
        if (!run.count) {
            // (a run with no quads: nothing to draw; a packet's runs are consecutive ones of the
            // builder, so one doesn't go past it)
            if (open)
                write_packet(b, p);
            open = false;
            continue;
        }
        u32 v = run.first;
        const u32 end = run.first + run.count;
        while (v < end) {
            if (!open) {
                p = Piece{r, 0, v, v, run.state, run.state};
                open = true;
            }
            const u32 lo = std::min(p.lo, run.state), hi = std::max(p.hi, run.state);
            const u32 used = entry_fixed(hi - lo + 1, p.n_runs + 1) + (p.v1 - p.v0) * vertex;
            const u32 fit = used < ENTRY_MAX ? (ENTRY_MAX - used) / (4 * vertex) : 0;
            if (fit == 0) {
                // (never with the piece empty: an entry holds a state, a run and many quads)
                write_packet(b, p);
                open = false;
                continue;
            }
            const u32 take = std::min(4 * fit, end - v);
            p.n_runs++, p.lo = lo, p.hi = hi;
            v += take;
            p.v1 = v;
            if (v < end) {
                write_packet(b, p);
                open = false;
            }
        }
    }
    if (open)
        write_packet(b, p);
}

void decode(u32 ref, guest::GxRenderer& renderer) {
    Arena& a = s_arena;
    if (ref & 15) {
        if (!check_marker(ref))
            a.stale.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const u32 end = a.published.load(std::memory_order_acquire);
    const u32 read = a.read.load(std::memory_order_relaxed);
    if (ref - read >= end - read) {
        a.stale.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const EntryHead* head = (const EntryHead*)(a.mem + (ref & ARENA_MASK));
    if (head->ref != ref || head->tag != ENTRY_TAG || head->bytes < sizeof(EntryHead) + PACKET_HEAD ||
        head->bytes > end - ref) {
        a.stale.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    renderer.lyt(*(const LytPacket*)(head + 1));
    a.decoded.fetch_add(1, std::memory_order_relaxed);
    a.read.store(ref + head->bytes, std::memory_order_release);
}

}  // namespace engine::lyt

// ---------------------------------------------------------------- the guest ABI (decl.h)
namespace g {

// layout::LayoutWrapper::Draw: every layout the program draws. Gated off (DrawInfo +0x5C clear:
// an offscreen capture of the scene), nothing, as the decompiled one, which returns the DrawInfo
// then; the decompiled draw when NOMA_CHECK_LYT_DRAW asks for it (orig; ab in odd game frames) and
// while the game records a display list (a command recorded into it would run at the list's call);
// else the walk, natively or for the checks. The return value is r3 as the decompiled draw leaves
// it: the last pane's Draw's (0 without a root pane, 0 after a mask pane, an invisible pane itself,
// its DrawSelf's for a pane without children, the game's code's own); callers pass it on, nobody
// reads it.
u32 layout_LayoutWrapper_Draw(u32 wrapper) {
    using namespace engine::lyt;
    Stats* s = stats_every() ? &stats() : nullptr;
    if (s)
        stats_frame(*s);
    const u32 di = guest::load32(GA_layout_sDrawInfo);
    if (!guest::load8(di + 0x5C))
        return di;
    DrawScope scope(s);
    if (s)
        s->calls++;
    const CheckMode mode = check_mode();
    if (mode == CheckMode::ORIG || (mode == CheckMode::AB && (guest::vi_game_frames() & 1)) || s_walks == MAX_WALKS) {
        if (s)
            s->orig++;
        return layout_LayoutWrapper_Draw_orig(wrapper);
    }
    const u32 gx = guest::load32(GA___GXData);
    if (gx && guest::load8(gx + GXDATA_IN_DISP_LIST)) {
        if (s)
            s->lists++;
        return layout_LayoutWrapper_Draw_orig(wrapper);
    }
    return walk_draw(wrapper, di, mode == CheckMode::STATE || mode == CheckMode::PIXELS, s);
}

// layout::LayoutWrapper::Update (the animations, then the matrices): the decompiled one, timed for
// NOMA_LYT_STATS
u32 layout_LayoutWrapper_Update(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7, u32 r8, u32 r9, u32 r10, f64 f1, f64 f2, f64 f3,
                                f64 f4, f64 f5, f64 f6, f64 f7, f64 f8) {
    using namespace engine::lyt;
    if (!stats_every())
        return layout_LayoutWrapper_Update_orig(r3, r4, r5, r6, r7, r8, r9, r10, f1, f2, f3, f4, f5, f6, f7, f8);
    Stats& s = stats();
    stats_frame(s);
    const bool outer = s_updates++ == 0;
    const u64 t0 = outer ? guest::host_timebase() : 0;
    const u32 r = layout_LayoutWrapper_Update_orig(r3, r4, r5, r6, r7, r8, r9, r10, f1, f2, f3, f4, f5, f6, f7, f8);
    if (outer)
        s.update_t += guest::host_timebase() - t0;
    s_updates--;
    return r;
}

}  // namespace g
