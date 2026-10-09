// NOMA_CHECK_LYT_DRAW (draw.h): which of the two layout draws runs, and on the host the native draw of
// a layout held against the decompiled draw of the same layout at the same place in the GX stream.
//
//   (unset), 0  the native draw only
//   orig        the decompiled draw only (layout_LayoutWrapper_Draw_orig): timing A/B in one build,
//               the reference pictures
//   ab          the native draw in even game frames, the decompiled one in odd ones (draw.cpp)
//   1           (host) the state check. The frame is the decompiled draw's: the native walk's quads,
//               built but not sent, are queued as a record, then a BEGIN marker goes into the FIFO,
//               the decompiled draw, an END marker. The host's decoder splits every GX draw between
//               the markers into quads and matches them in order with the record's: where each one
//               lands, its colours and texture coordinates, and the GX state it is drawn with. It
//               runs in every frame, drawn or not (tools/flow_check.py --hooks: at the game's speed).
//   pixels      (host) the pixel check: the native packet, a MID marker, the decompiled draw, an END
//               marker. At the packet the EFB (colour and depth) is kept; at MID what the native
//               draw made of it is taken aside and the kept EFB put back; at END the decompiled
//               draw's pixels are compared with the native draw's over the pixels either one
//               changed, and the worst layout draws are written out as pictures. The frame goes on
//               as the decompiled draw left it. Only in the frames the host's renderer draws (the
//               others are counted).
// Both checks need the host's renderer, whose hooks see the GX draws and the packets: on the 3DS,
// and on the host with NOMA_NO_RENDER, 1 and pixels act as unset.
//
// NOMA_CHECK_LYT_DRAW_MUTATE=color|uv|pos|tev|tex breaks one field of each layout draw's native quads
// on purpose (the first vertex's red, the first textured vertex's s, the first vertex's x, the first
// state's stage 0 colour combiner, the first textured state's map 0 image address): the checks must
// report it at the first quad that has it.
//
// The markers are LYT_OPCODE commands whose reference has its low four bits set: the check's serial
// << 4 | the phase (BEGIN 1, MID 2, END 3); draw.cpp's decoder hands them to check_marker. A record is
// queued before anything of its layout draw goes into the FIFO: the host's decoder runs as soon as a
// 32-byte burst is complete.
//
// The state check's matching: a GX quad is the first of the next native quads (up to LOOKAHEAD on)
// that has every corner where the GX quad has it. Native quads passed over and GX quads that matched
// none are paired in order when there are as many of each (quads drawn elsewhere: their position is
// reported and the rest compared); otherwise the native ones are missing (the decompiled draw didn't
// draw them) and the GX ones extra, or drawn through the guest when the native walk left panes of
// that layout draw to the game's code (draw.cpp's fallbacks, which a walk for the checks only counts:
// the decompiled draw draws them here).
//
// What it compares for each quad: its corners' positions (the GX draw's vertices through
// gx_decode_vertices and gx_transform against the native view-space positions through the GX draw's
// projection, in clip space: equal, within 1e-4 of their size, or within 1/16 pixel in the window),
// colour channel 0 at each corner (exact), the texture coordinates the stages sample with (within
// 1e-4 of the texture's size in texels, and 1e-3 of a texel at least). And once for each GX draw and
// native state, the state the draw consumes: GENMODE's stage, texgen and channel counts, its cull and
// indirect stage count, and XF's texgen and channel counts; each active stage's colour and alpha
// combiners (the alpha one with its swap selects), its TREF half and its indirect command (off); the
// swap tables (KSEL bits 0-3 of all eight words); a stage's konst selections where it reads KONST; of
// C0-C2 and K0-K3 only the components the stages read (a colour register before a stage writes it);
// the texture words of the maps the stages sample (mode0, mode1, image0 and image3 whole, of image1 and
// image2 the preload bit 21; for a palette format the palette word, and the palette in TMEM against
// what the native load would have put there); ALPHACOMPARE, ZMODE and CMODE0 whole (a layout draw's
// blend mode writes all of CMODE0: blend and logic op enables, the factors, subtract, the logic op,
// and the dither, colour and alpha update bits it keeps). Last, what a packet takes from the stream at
// its command and no layout draw sets, against the stream's at BEGIN (where the packet would have
// been, the GX library's pending state sent first): the scissor and its offset, CMODE1, ZCONTROL, the
// viewport and the screen once for each GX draw, and the components of PREV the stages read.
//
// Reports name a layout by its wrapper's address (the wrapper keeps no trace of its .lex path, only a
// hash of it: app_ResCache_HashPath, proposed app_ResCache_HashPath; recording the path would take
// layout_LayoutWrapper_ctor and _ctorAttached patched) and a quad by its pane's name (Pane +0xBC, 16
// characters) and address.
//
// Not checked: the GX state a layout draw leaves for the draws after it (verify.md's followers
// check), which needs the decoder to keep a second register file (every register write outside the
// markers into both, those between them into the GPU's alone: runtime/hw/gx/cp.cpp).
#include "engine/lyt/draw.h"

#include <cstdlib>
#include <cstring>

#include "runtime/runtime.h"

#ifndef __3DS__
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "runtime/hw/gx/raster.h"
#include "runtime/hw/gx/regs.h"
#include "runtime/hw/gx/texture.h"
#include "runtime/hw/gx/vertex.h"
#include "runtime/hw/hw.h"

namespace g {
u32 layout_LayoutWrapper_Draw_orig(u32 r3);  // (the decompiled one)
}

bool write_png(const std::string& path, const u32* rgba, u32 w, u32 h);  // (platform/host/png.cpp)
#endif

namespace engine::lyt {

namespace {

CheckMode read_mode() {
    const char* e = getenv("NOMA_CHECK_LYT_DRAW");
    if (!e || !*e || !strcmp(e, "0"))
        return CheckMode::NONE;
    if (!strcmp(e, "orig")) {
        guest::log("lyt draw check: the decompiled layout draws only (NOMA_CHECK_LYT_DRAW=orig)");
        return CheckMode::ORIG;
    }
    if (!strcmp(e, "ab")) {
        guest::log("lyt draw check: the native layout draws in even game frames, the decompiled ones in odd ones");
        return CheckMode::AB;
    }
    const bool pixels = !strcmp(e, "pixels");
    if (!pixels && strcmp(e, "1")) {
        guest::log("lyt draw check: NOMA_CHECK_LYT_DRAW=%s is none of orig, ab, 1, pixels: the native draws only", e);
        return CheckMode::NONE;
    }
#ifdef __3DS__
    guest::log("lyt draw check: NOMA_CHECK_LYT_DRAW=%s is the host's: the native draws only", e);
    return CheckMode::NONE;
#else
    if (getenv("NOMA_NO_RENDER")) {
        guest::log("lyt draw check: NOMA_CHECK_LYT_DRAW=%s needs the host's renderer, which NOMA_NO_RENDER leaves "
                   "out (NOMA_DUMP_FRAMES=100000 NOMA_DUMP_FROM=100000000 installs it without drawing): the "
                   "native draws only",
                   e);
        return CheckMode::NONE;
    }
    if (pixels) {
        guest::log("lyt draw check: each layout draw's pixels, native against decompiled, in the frames drawn (the "
                   "frames are the decompiled draw's)");
        return CheckMode::PIXELS;
    }
    guest::log("lyt draw check: each layout draw's quads and their GX state, native against the decompiled draw's "
               "GX draws (the frames are the decompiled draw's)");
    return CheckMode::STATE;
#endif
}

// The native draw as layout_LayoutWrapper_Draw makes it: the 2D state and the orthographic projection
// (with DrawInfo's view matrix and rectangle) the packets are drawn with, then the packets. (A walk
// for the checks leaves those calls to the decompiled draw, which makes them again.)
void submit_native(const Builder& b) {
    g::layout_DrawInfo_SetupGX2D();
    g::layout_DrawInfo_SetProjection(guest::load32(GA_layout_sDrawInfo));
    packet_submit(b);
}

}  // namespace

CheckMode check_mode() {
    static const CheckMode m = read_mode();
    return m;
}

#ifdef __3DS__

// (check_mode never asks for a check here: no walk is built for one and no marker is written)
void check_draw(u32, const Builder& b) { submit_native(b); }
bool check_marker(u32) { return false; }
void check_gx_draw(u32, u32, u32, const u8*) {}
void check_packet(const LytPacket&) {}

#else

namespace {

using guest::bits;
using guest::GxOutVertex;

// ---------------------------------------------------------------- shared

constexpr u32 PHASE_BEGIN = 1, PHASE_MID = 2, PHASE_END = 3;

void write_marker(u32 serial, u32 phase) {
    guest::gx_fifo_write(0, LYT_OPCODE, 1);
    guest::gx_fifo_write(0, serial << 4 | phase, 4);
}

u32 s_serial;                       // (the CPU's thread) the layout draws checked so far (28 bits)
std::atomic<bool> s_drawing{true};  // HostRenderer::drawing: the frames decoded now are rasterised

std::string strf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string strf(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

// the layout draw's quads in a GX draw, as the state check splits it (quads; a strip or a fan of four
// as one quad)
u32 quads_in(u32 prim, u32 count) {
    if (prim == 0x80 || prim == 0x88)
        return count / 4;
    return (prim == 0x98 || prim == 0xA0) && count == 4 ? 1 : 0;
}

// ---- NOMA_CHECK_LYT_DRAW_MUTATE

enum class Mutate { NONE, COLOR, UV, POS, TEV, TEX };

Mutate read_mutate() {
    const char* e = getenv("NOMA_CHECK_LYT_DRAW_MUTATE");
    if (!e || !*e)
        return Mutate::NONE;
    static const struct {
        const char* name;
        Mutate m;
        const char* what;
    } k_mutations[] = {
        {"color", Mutate::COLOR, "the first vertex's red"},
        {"uv", Mutate::UV, "the first textured vertex's s"},
        {"pos", Mutate::POS, "the first vertex's x"},
        {"tev", Mutate::TEV, "the first state's stage 0 colour combiner"},
        {"tex", Mutate::TEX, "the first textured state's map 0 image address"},
    };
    for (const auto& k : k_mutations)
        if (!strcmp(e, k.name)) {
            guest::log("lyt draw check: %s of every layout draw broken on purpose (NOMA_CHECK_LYT_DRAW_MUTATE=%s)",
                       k.what, e);
            return k.m;
        }
    guest::log("lyt draw check: NOMA_CHECK_LYT_DRAW_MUTATE=%s is none of color, uv, pos, tev, tex: nothing broken", e);
    return Mutate::NONE;
}

Mutate mutation() {
    static const Mutate m = read_mutate();
    return m;
}

void mutate(std::vector<LytState>& states, const std::vector<LytRun>& runs, std::vector<LytVertex>& vertices) {
    switch (mutation()) {
    case Mutate::NONE:
        break;
    case Mutate::COLOR:
        if (!vertices.empty())
            vertices[0].color[0] ^= 0x40;
        break;
    case Mutate::POS:
        if (!vertices.empty())
            vertices[0].pos[0] += 2.0f;
        break;
    case Mutate::UV:
        for (const LytRun& r : runs)
            if (states[r.state].n_texgens && r.count) {
                vertices[r.first].tc[0][0] += 0.125f;
                return;
            }
        break;
    case Mutate::TEV:
        if (!states.empty())
            states[0].tev.color_env[0] ^= 0xF;  // (its input d)
        break;
    case Mutate::TEX:
        for (LytState& s : states)
            if (s.n_maps) {
                s.tex[0].image3 += 1;  // (32 bytes on)
                return;
            }
        break;
    }
}

// ---------------------------------------------------------------- the state check (=1)

enum Field : u32 {
    F_ORDER, F_PRIMITIVE, F_EXTRA, F_MISSING, F_POSITION, F_COLOUR, F_TEXCOORD, F_GENMODE, F_XF_COUNTS, F_TEV,
    F_TREF, F_INDIRECT, F_SWAP, F_KSEL, F_REGISTER, F_KONST, F_TEXTURE, F_PALETTE, F_ALPHA_COMPARE, F_ZMODE,
    F_CMODE0, F_INHERITED, F_N
};
const char* const k_field[] = {"order", "primitive", "extra", "missing", "position", "colour", "texcoord",
                               "genmode", "xf counts", "tev", "tref", "indirect", "swap", "ksel", "register",
                               "konst", "texture", "palette", "alpha compare", "zmode", "cmode0", "inherited"};
static_assert(sizeof(k_field) / sizeof(k_field[0]) == F_N, "a name for each field");

// how far on from the next native quad a GX quad's match is looked for (a text box's glyphs, a window
// or two: what one side may draw that the other doesn't)
constexpr u32 LOOKAHEAD = 256;

struct PaneName {
    u32 pane;
    char name[17];
};

// a layout draw's native quads, as the walk derived them (copied on the CPU's thread: by the time the
// decoder gets to them the guest may have changed the panes)
struct Record {
    u32 serial = 0, wrapper = 0, frame = 0, fallbacks = 0;
    std::vector<LytState> states;
    std::vector<LytVertex> vertices;
    std::vector<u32> quad_state;  // each quad's state
    std::vector<u32> quad_name;   // each quad's pane (in names)
    std::vector<PaneName> names;
    u32 quads() const { return (u32)quad_state.size(); }
    std::string pane(u32 q) const {
        if (q >= quad_name.size() || quad_name[q] >= names.size())
            return "?";
        const PaneName& n = names[quad_name[q]];
        return strf("%s (%08X)", n.name, n.pane);
    }
};

// what a GX draw consumes, as the decoder holds it at the draw (a copy: a quad may be compared after
// later draws, once it is known which native quad it is)
struct GxDraw {
    u32 index = 0;  // the layout draw's GX draws before it
    u32 bp[256];
    s32 reg[4][4], konst[4][4];
    f32 proj[6];
    bool ortho;
    f32 vp[6];
    f32 sox, soy;  // the scissor offset (raster.cpp's to_screen subtracts it)
    u32 num_colors, num_texgens;  // XF's
    bool bottom;   // drawn on the bottom screen
};

void take_draw(GxDraw& d) {
    const guest::GxState& g = guest::g_gx;
    memcpy(d.bp, g.bp, sizeof(d.bp));
    memcpy(d.reg, g.tev_reg, sizeof(d.reg));
    memcpy(d.konst, g.tev_konst, sizeof(d.konst));
    for (int i = 0; i < 6; i++) {
        d.proj[i] = guest::xf_f32(guest::XF_PROJECTION + i);
        d.vp[i] = guest::xf_f32(guest::XF_VIEWPORT + i);
    }
    d.ortho = g.xf[guest::XF_PROJECTION + 6] != 0;
    d.sox = (f32)(bits(g.bp[guest::BP_SCISSOR_OFFSET], 0, 10) * 2);
    d.soy = (f32)(bits(g.bp[guest::BP_SCISSOR_OFFSET], 10, 10) * 2);
    d.num_colors = g.xf[guest::XF_NUMCOLORS];
    d.num_texgens = g.xf[guest::XF_NUMTEXGENS];
    d.bottom = guest::gx_host_on_bottom();
}

// What a native packet takes from the stream at its command and no layout draw sets (callers.md
// §3.3): the scissor and its offset, CMODE1 (the destination alpha), ZCONTROL (the pixel format, where
// the depth test is made), the viewport, the screen, and the TEV's PREV. The state check holds the GX
// draws' against the stream's at the layout draw's BEGIN, where its packet would have been (the GX
// library's pending state sent first, as packet_submit sends it): a layout draw that changed any of
// them would draw differently from its packet.
const u32 k_inherited_bp[] = {guest::BP_SCISSOR_TL, guest::BP_SCISSOR_BR, guest::BP_SCISSOR_OFFSET, guest::BP_CMODE1,
                              guest::BP_ZCONTROL};
struct Inherited {
    u32 bp[5];
    f32 vp[6];
    bool bottom;
    s32 prev[4];
};

void take_inherited(Inherited& in) {
    const guest::GxState& g = guest::g_gx;
    for (u32 i = 0; i < 5; i++)
        in.bp[i] = g.bp[k_inherited_bp[i]];
    for (int i = 0; i < 6; i++)
        in.vp[i] = guest::xf_f32(guest::XF_VIEWPORT + i);
    in.bottom = guest::gx_host_on_bottom();
    memcpy(in.prev, g.tev_reg[0], sizeof(in.prev));
}

// a GX quad that matched no native quad yet, and its draw (in StateCheck::held_draws)
struct HeldQuad {
    GxOutVertex v[4];
    u32 draw;
};

struct StateCheck {
    std::mutex lock;             // (pending: with NOMA_GX_THREAD the decoder has a thread of its own)
    std::deque<Record> pending;  // layout draws whose BEGIN the decoder hasn't reached
    // the layout draw between its markers (the decoder's)
    bool in_call = false;
    Record call;
    u32 next = 0;   // its first native quad neither matched nor reported
    u32 draws = 0;  // its GX draws so far
    GxDraw draw;    // the GX draw being split
    bool draw_held = false;  // ... copied to held_draws (the last one)
    std::vector<GxDraw> held_draws;
    std::vector<HeldQuad> held;
    u32 cmp_draw = ~0u, cmp_state = ~0u;  // the GX draw and native state last compared
    Inherited begin;  // the stream's state at its BEGIN
    // totals
    u64 calls = 0, quads = 0, vertices = 0, mismatches = 0, through_guest = 0, fallbacks = 0;
    u64 inexact_pos = 0, inexact_tc = 0;
    u64 by_field[F_N] = {};
    std::unordered_map<u32, u64> by_layout;
    u32 logged = 0;
    bool field_logged[F_N] = {};
    u64 last_report = 0;
};
StateCheck s_state;

// a mismatch (n quads' for a run of them): the first 40 logged, then each field's first
void mismatch(Field f, u32 quad, u32 draw, const std::string& gx, const std::string& native, u64 n = 1) {
    StateCheck& c = s_state;
    c.mismatches += n, c.by_field[f] += n, c.by_layout[c.call.wrapper] += n;
    if (c.logged >= 40 && c.field_logged[f])
        return;
    c.logged++, c.field_logged[f] = true;
    const std::string through =
        c.call.fallbacks ? strf(" (%u panes of it left to the game's code)", c.call.fallbacks) : std::string();
    guest::log("lyt draw check: %s: layout %08X (game frame %u), pane %s, quad %u of %u (GX draw %u): GX %s, "
               "native %s%s",
               k_field[f], c.call.wrapper, c.call.frame, c.call.pane(quad).c_str(), quad, c.call.quads(), draw,
               gx.c_str(), native.c_str(), through.c_str());
}

// the markers out of order (no layout draw to name)
void order_mismatch(const std::string& gx, const std::string& native) {
    StateCheck& c = s_state;
    c.mismatches++, c.by_field[F_ORDER]++;
    if (c.logged >= 40 && c.field_logged[F_ORDER])
        return;
    c.logged++, c.field_logged[F_ORDER] = true;
    guest::log("lyt draw check: order: GX %s, native %s", gx.c_str(), native.c_str());
}

// every 10 s (at a layout draw's END): the totals, and the layouts with the most mismatches
void report() {
    StateCheck& c = s_state;
    const u64 now = guest::host_time_us();
    if (now - c.last_report < 10000000)
        return;
    c.last_report = now;
    std::string fields;
    for (u32 f = 0; f < F_N; f++)
        if (c.by_field[f])
            fields += strf("%s%s %llu", fields.empty() ? " (" : ", ", k_field[f], (unsigned long long)c.by_field[f]);
    if (!fields.empty())
        fields += ")";
    guest::log("lyt draw check: %llu layout draws, %llu GX quads (%llu vertices): %llu mismatches%s; %llu GX quads "
               "of %llu panes left to the game's code; within the tolerance but not bit-exact: %llu quads' "
               "positions, %llu quads' texture coordinates",
               (unsigned long long)c.calls, (unsigned long long)c.quads, (unsigned long long)c.vertices,
               (unsigned long long)c.mismatches, fields.c_str(), (unsigned long long)c.through_guest,
               (unsigned long long)c.fallbacks, (unsigned long long)c.inexact_pos, (unsigned long long)c.inexact_tc);
    std::vector<std::pair<u64, u32>> layouts;
    for (const auto& [w, n] : c.by_layout)
        layouts.push_back({n, w});
    std::sort(layouts.rbegin(), layouts.rend());
    for (size_t i = 0; i < layouts.size() && i < 8; i++)
        guest::log("lyt draw check:   layout %08X: %llu mismatches", layouts[i].second,
                   (unsigned long long)layouts[i].first);
}

// ---- where a quad lands

// the native position through the GX draw's projection, as gx_transform applies it (its expressions)
void project(const GxDraw& d, const f32* epos, f32 clip[4]) {
    const f32* p = d.proj;
    if (d.ortho) {
        clip[0] = p[0] * epos[0] + p[1];
        clip[1] = p[2] * epos[1] + p[3];
        clip[2] = p[4] * epos[2] + p[5];
        clip[3] = 1.0f;
    } else {
        clip[0] = p[0] * epos[0] + p[1] * epos[2];
        clip[1] = p[2] * epos[1] + p[3] * epos[2];
        clip[2] = p[4] * epos[2] + p[5];
        clip[3] = -epos[2];
    }
}

// clip space to the window as raster.cpp's to_screen (x, y in EFB pixels before the bottom screen's
// mapping, z in depth units)
void window(const GxDraw& d, const f32 clip[4], f32 out[3]) {
    const f32 iw = 1.0f / clip[3];
    out[0] = clip[0] * iw * d.vp[0] + d.vp[3] - d.sox;
    out[1] = clip[1] * iw * d.vp[1] + d.vp[4] - d.soy;
    out[2] = clip[2] * iw * d.vp[2] + d.vp[5];
}

// one corner: the GX draw's against the native (both in clip space): equal, within 1e-4 of their size,
// or in the window within 1/16 of a pixel (x, y) and a depth unit (z); `exact` cleared when not equal
bool same_corner(const GxDraw& d, const f32* g, const f32* n, bool& exact) {
    for (int i = 0; i < 4; i++) {
        if (g[i] == n[i])
            continue;
        exact = false;
        if (std::fabs(g[i] - n[i]) <= 1e-4f * std::max(std::fabs(g[i]), std::fabs(n[i])))
            continue;
        if (i == 3)
            return false;
        const f32 tol = i < 2 ? 1.0f / 16 : 1.0f;
        if (std::fabs((g[i] / g[3] - n[i] / n[3]) * d.vp[i]) <= tol)
            continue;
        return false;
    }
    return true;
}

// whether GX quad v lands where native quad q does, corner by corner; `exact`: all of them equal
bool lands(const GxDraw& d, const GxOutVertex* v, u32 q, bool& exact) {
    exact = true;
    for (u32 k = 0; k < 4; k++) {
        f32 n[4];
        project(d, s_state.call.vertices[4 * q + k].pos, n);
        if (!same_corner(d, v[k].clip, n, exact))
            return false;
    }
    return true;
}

std::string gx_box(const GxDraw& d, const GxOutVertex* v) {
    f32 x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (u32 k = 0; k < 4; k++) {
        f32 w[3];
        window(d, v[k].clip, w);
        x0 = std::min(x0, w[0]), x1 = std::max(x1, w[0]), y0 = std::min(y0, w[1]), y1 = std::max(y1, w[1]);
    }
    return strf("(%.2f,%.2f)-(%.2f,%.2f)", x0, y0, x1, y1);
}
std::string native_box(const GxDraw& d, u32 q) {
    f32 x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (u32 k = 0; k < 4; k++) {
        f32 c[4], w[3];
        project(d, s_state.call.vertices[4 * q + k].pos, c);
        window(d, c, w);
        x0 = std::min(x0, w[0]), x1 = std::max(x1, w[0]), y0 = std::min(y0, w[1]), y1 = std::max(y1, w[1]);
    }
    return strf("(%.2f,%.2f)-(%.2f,%.2f)", x0, y0, x1, y1);
}

// ---- the GX state a quad is drawn with, against its native state

u32 tref_half(const u32* tref, u32 stage) { return (tref[stage / 2] >> (12 * (stage & 1))) & 0xFFF; }
u32 ksel_half(const u32* ksel, u32 stage) { return (ksel[stage / 2] >> (4 + 10 * (stage & 1))) & 0x3FF; }

// texture map m, which a stage of the GX draw samples: mode0, mode1, image0 and image3 whole, of image1
// and image2 the preload bit (21), and for a palette format the palette word and what is in TMEM where
// it points (the native load against what the GX path loaded)
void compare_texture(const GxDraw& d, const LytState& s, u32 m, u32 q) {
    using namespace guest;
    const u32 img0 = d.bp[bp_tex(BP_TX_SETIMAGE0, m)];
    const u32 fmt = bits(img0, 20, 4);
    if (m >= std::min(s.n_maps, LYT_MAX_MAPS)) {
        mismatch(F_TEXTURE, q, d.index,
                 strf("map %u: image %08X, %ux%u, format %u", m, (d.bp[bp_tex(BP_TX_SETIMAGE3, m)] & 0xFFFFFF) << 5,
                      bits(img0, 0, 10) + 1, bits(img0, 10, 10) + 1, fmt),
                 strf("no map %u (%u maps)", m, s.n_maps));
        return;
    }
    const LytTex& t = s.tex[m];
    const struct {
        const char* name;
        u32 reg, mask, native;
    } words[] = {
        {"mode0", BP_TX_SETMODE0, 0xFFFFFF, t.mode0},  {"mode1", BP_TX_SETMODE1, 0xFFFFFF, t.mode1},
        {"image0", BP_TX_SETIMAGE0, 0xFFFFFF, t.image0}, {"image3", BP_TX_SETIMAGE3, 0xFFFFFF, t.image3},
        {"image1 bit 21", BP_TX_SETIMAGE1, 1u << 21, t.image1}, {"image2 bit 21", BP_TX_SETIMAGE2, 1u << 21, t.image2},
    };
    for (const auto& w : words) {
        const u32 g = d.bp[bp_tex(w.reg, m)];
        if ((g ^ w.native) & w.mask)
            mismatch(F_TEXTURE, q, d.index, strf("map %u %s %06X", m, w.name, g & w.mask), strf("%06X", w.native & w.mask));
    }
    if (fmt != 8 && fmt != 9 && fmt != 10)
        return;
    const u32 tlut = d.bp[bp_tex(BP_TX_SETTLUT, m)];
    if ((tlut ^ t.tlut) & 0xFFFFFF)
        mismatch(F_TEXTURE, q, d.index, strf("map %u tlut %06X", m, tlut), strf("%06X", t.tlut));
    const u32 reads = bits(tlut, 0, 10) << 9;
    if (!t.tlut_load[1]) {
        mismatch(F_PALETTE, q, d.index, strf("map %u reads a palette at TMEM %05X", m, reads), "no palette load");
        return;
    }
    const u32 at = (t.tlut_load[1] & 0x3FF) << 9;
    if (at != reads) {
        mismatch(F_PALETTE, q, d.index, strf("map %u reads its palette at TMEM %05X", m, reads),
                 strf("a palette loaded at %05X", at));
        return;
    }
    // (as gx_load_tlut reads it)
    const u32 src = 0x80000000u | ((t.tlut_load[0] << 5) & 0x1FFFFFFF);
    const u32 n = std::min<u32>(((t.tlut_load[1] >> 10) & 0x3FF) * 32, (1u << 20) - at);
    static std::vector<u8> s_palette;
    s_palette.resize(n);
    read_g(s_palette.data(), src, n);
    const u8* tmem = gx_tmem() + at;
    if (!memcmp(tmem, s_palette.data(), n))
        return;
    u32 i = 0;
    while (tmem[i] == s_palette[i])
        i++;
    mismatch(F_PALETTE, q, d.index, strf("map %u's palette (TMEM %05X) byte %u %02X", m, at, i, tmem[i]),
             strf("%02X (%u bytes from %08X)", s_palette[i], n, src));
}

// the state the GX draw consumes against native state s (what the file's header lists)
void compare_state(const GxDraw& d, const LytState& s, u32 q) {
    using namespace guest;
    const u32 gen = d.bp[BP_GENMODE];
    const u32 gx_stages = bits(gen, 10, 4) + 1;
    if (bits(gen, 0, 4) != s.n_texgens || bits(gen, 4, 3) != s.n_chans || gx_stages != s.tev.n_stages ||
        bits(gen, 14, 2) || bits(gen, 16, 3))
        mismatch(F_GENMODE, q, d.index,
                 strf("%06X: stages %u, texgens %u, channels %u, cull %u, indirect stages %u", gen, gx_stages,
                      bits(gen, 0, 4), bits(gen, 4, 3), bits(gen, 14, 2), bits(gen, 16, 3)),
                 strf("stages %u, texgens %u, channels %u, cull 0, indirect stages 0", s.tev.n_stages, s.n_texgens,
                      s.n_chans));
    if (d.num_texgens != s.n_texgens || d.num_colors != s.n_chans)
        mismatch(F_XF_COUNTS, q, d.index, strf("texgens %u, channels %u", d.num_texgens, d.num_colors),
                 strf("texgens %u, channels %u", s.n_texgens, s.n_chans));
    const u32 n = std::min(gx_stages, std::min<u32>(s.tev.n_stages, 16));
    u32 reg_read[4] = {}, written[4] = {}, konst_read[4] = {};  // components: bits 0-2 R, G, B, bit 3 A
    u32 maps_read = 0;
    for (u32 i = 0; i < n; i++) {
        const u32 ce = d.bp[BP_TEV_COLOR_ENV + 2 * i], ae = d.bp[BP_TEV_COLOR_ENV + 2 * i + 1];
        if (ce != s.tev.color_env[i])
            mismatch(F_TEV, q, d.index, strf("stage %u colour combiner %06X", i, ce), strf("%06X", s.tev.color_env[i]));
        if (ae != s.tev.alpha_env[i])
            mismatch(F_TEV, q, d.index, strf("stage %u alpha combiner %06X", i, ae), strf("%06X", s.tev.alpha_env[i]));
        const u32 tr = tref_half(&d.bp[BP_TREF], i);
        if (tr != tref_half(s.tev.tref, i))
            mismatch(F_TREF, q, d.index,
                     strf("stage %u %03X (map %u, coordinate %u, %s, channel %u)", i, tr, bits(tr, 0, 3), bits(tr, 3, 3),
                          bits(tr, 6, 1) ? "on" : "off", bits(tr, 7, 3)),
                     strf("%03X", tref_half(s.tev.tref, i)));
        if (bits(tr, 6, 1))
            maps_read |= 1u << bits(tr, 0, 3);
        // (the native draw draws every stage direct)
        if (d.bp[BP_IND_CMD + i])
            mismatch(F_INDIRECT, q, d.index, strf("stage %u indirect command %06X", i, d.bp[BP_IND_CMD + i]),
                     "000000 (direct)");
        // the stage's inputs a, b, c, d (a compare reads the same ones)
        bool konst_c = false, konst_a = false;
        for (u32 a = 0; a < 4; a++) {
            const u32 carg = bits(ce, 4 * a, 4), aarg = bits(ae, 4 + 3 * a, 3);
            if (carg < 8)
                reg_read[carg / 2] |= ((carg & 1) ? 0x8u : 0x7u) & ~written[carg / 2];
            if (aarg < 4)
                reg_read[aarg] |= 0x8u & ~written[aarg];
            konst_c |= carg == 14, konst_a |= aarg == 6;
        }
        const u32 gk = ksel_half(&d.bp[BP_TEV_KSEL], i), nk = ksel_half(s.tev.ksel, i);
        const u32 kc = bits(gk, 0, 5), ka = bits(gk, 5, 5);
        if (konst_c && kc != bits(nk, 0, 5))
            mismatch(F_KSEL, q, d.index, strf("stage %u colour konst %u", i, kc), strf("%u", bits(nk, 0, 5)));
        if (konst_a && ka != bits(nk, 5, 5))
            mismatch(F_KSEL, q, d.index, strf("stage %u alpha konst %u", i, ka), strf("%u", bits(nk, 5, 5)));
        // (selections 12-15: K0-K3's R, G, B; 16-31: one component of one, as raster.cpp's konst_color)
        if (konst_c && kc >= 12 && kc < 16)
            konst_read[kc - 12] |= 0x7;
        if (konst_c && kc >= 16)
            konst_read[(kc - 16) & 3] |= 1u << ((kc - 16) >> 2);
        if (konst_a && ka >= 16)
            konst_read[(ka - 16) & 3] |= 1u << ((ka - 16) >> 2);
        written[bits(ce, 22, 2)] |= 0x7, written[bits(ae, 22, 2)] |= 0x8;
    }
    bool swaps = false;
    for (u32 k = 0; k < 8; k++)
        swaps |= ((d.bp[BP_TEV_KSEL + k] ^ s.tev.ksel[k]) & 0xF) != 0;
    if (swaps) {
        std::string gx, native;
        for (u32 k = 0; k < 8; k++)
            gx += strf("%X", d.bp[BP_TEV_KSEL + k] & 0xF), native += strf("%X", s.tev.ksel[k] & 0xF);
        mismatch(F_SWAP, q, d.index, "swap tables (KSEL bits 0-3) " + gx, native);
    }
    // (PREV: what the packet would find in the stream, which a layout draw doesn't set)
    const s32* const native_reg[4] = {s_state.begin.prev, s.tev.reg[1], s.tev.reg[2], s.tev.reg[3]};
    static const char* const k_reg[4] = {"PREV", "C0", "C1", "C2"};
    for (u32 r = 0; r < 4; r++)
        for (u32 k = 0; k < 4; k++)
            if ((reg_read[r] >> k & 1) && d.reg[r][k] != native_reg[r][k]) {
                mismatch(F_REGISTER, q, d.index,
                         strf("%s %d %d %d %d", k_reg[r], d.reg[r][0], d.reg[r][1], d.reg[r][2], d.reg[r][3]),
                         strf("%d %d %d %d (components read: %X)%s", native_reg[r][0], native_reg[r][1],
                              native_reg[r][2], native_reg[r][3], reg_read[r],
                              r ? "" : ", the stream's at the layout draw"));
                break;
            }
    for (u32 r = 0; r < 4; r++)
        for (u32 k = 0; k < 4; k++)
            if ((konst_read[r] >> k & 1) && d.konst[r][k] != s.tev.konst[r][k]) {
                mismatch(F_KONST, q, d.index,
                         strf("K%u %d %d %d %d", r, d.konst[r][0], d.konst[r][1], d.konst[r][2], d.konst[r][3]),
                         strf("%d %d %d %d (components read: %X)", s.tev.konst[r][0], s.tev.konst[r][1],
                              s.tev.konst[r][2], s.tev.konst[r][3], konst_read[r]));
                break;
            }
    for (u32 m = 0; m < 8; m++)
        if (maps_read >> m & 1)
            compare_texture(d, s, m, q);
    if (d.bp[BP_ALPHACOMPARE] != s.alpha_compare)
        mismatch(F_ALPHA_COMPARE, q, d.index, strf("%06X", d.bp[BP_ALPHACOMPARE]), strf("%06X", s.alpha_compare));
    if (d.bp[BP_ZMODE] != s.zmode)
        mismatch(F_ZMODE, q, d.index, strf("%06X", d.bp[BP_ZMODE]), strf("%06X", s.zmode));
    if (d.bp[BP_CMODE0] != s.cmode0)
        mismatch(F_CMODE0, q, d.index, strf("%06X", d.bp[BP_CMODE0]), strf("%06X", s.cmode0));
}

// what the GX draw takes from the stream against what the layout draw's packet would have taken there
// (Inherited)
void compare_inherited(const GxDraw& d, u32 q) {
    const Inherited& b = s_state.begin;
    for (u32 i = 0; i < 5; i++) {
        const u32 r = k_inherited_bp[i];
        if (d.bp[r] != b.bp[i])
            mismatch(F_INHERITED, q, d.index, strf("BP %02X %06X", r, d.bp[r]),
                     strf("%06X, the stream's at the layout draw", b.bp[i]));
    }
    if (memcmp(d.vp, b.vp, sizeof(d.vp)))
        mismatch(F_INHERITED, q, d.index,
                 strf("viewport %g %g %g %g %g %g", d.vp[0], d.vp[1], d.vp[2], d.vp[3], d.vp[4], d.vp[5]),
                 strf("%g %g %g %g %g %g, the stream's at the layout draw", b.vp[0], b.vp[1], b.vp[2], b.vp[3],
                      b.vp[4], b.vp[5]));
    if (d.bottom != b.bottom)
        mismatch(F_INHERITED, q, d.index, d.bottom ? "the bottom screen" : "the top screen",
                 b.bottom ? "the bottom screen, the stream's at the layout draw"
                          : "the top screen, the stream's at the layout draw");
}

// GX quad v and native quad q (taken to be the same quad): the state (once for each GX draw and native
// state, what it takes from the stream once for each GX draw), colour channel 0 at each corner (exact; the native's colours, none past its channel count),
// and each texture coordinate a stage samples with (within 1e-4 of the texture's size in texels, and
// 1e-3 of a texel at least)
void compare_quad(const GxDraw& d, const GxOutVertex* v, u32 q) {
    using namespace guest;
    StateCheck& c = s_state;
    const Record& r = c.call;
    const u32 st = r.quad_state[q];
    const LytState& s = r.states[st];
    if (d.index != c.cmp_draw || st != c.cmp_state) {
        if (d.index != c.cmp_draw)
            compare_inherited(d, q);
        c.cmp_draw = d.index, c.cmp_state = st;
        compare_state(d, s, q);
    }
    for (u32 k = 0; k < 4; k++) {
        const u8* nc = r.vertices[4 * q + k].color;
        const f32* gc = v[k].color[0];
        bool same = true;
        for (u32 i = 0; i < 4; i++)
            same &= gc[i] == (s.n_chans ? (f32)nc[i] : 0.0f);
        if (!same) {
            mismatch(F_COLOUR, q, d.index,
                     strf("corner %u %02X%02X%02X%02X", k, (u32)gc[0], (u32)gc[1], (u32)gc[2], (u32)gc[3]),
                     s.n_chans ? strf("%02X%02X%02X%02X", nc[0], nc[1], nc[2], nc[3]) : std::string("no channel"));
            break;
        }
    }
    // the coordinates the stages sample with, each with the size of the first map sampled with it
    const u32 stages = bits(d.bp[BP_GENMODE], 10, 4) + 1;
    u32 size[8][2] = {};
    for (u32 i = 0; i < stages; i++) {
        const u32 tr = tref_half(&d.bp[BP_TREF], i), t = bits(tr, 3, 3);
        if (bits(tr, 6, 1) && !size[t][0]) {
            const u32 img0 = d.bp[bp_tex(BP_TX_SETIMAGE0, bits(tr, 0, 3))];
            size[t][0] = bits(img0, 0, 10) + 1, size[t][1] = bits(img0, 10, 10) + 1;
        }
    }
    bool exact = true;
    for (u32 t = 0; t < 8; t++) {
        if (!size[t][0])
            continue;
        const bool carried = t < LYT_MAX_COORDS && t < s.n_texgens;
        for (u32 k = 0; k < 4; k++) {
            f32 g[2] = {v[k].tex[t][0], v[k].tex[t][1]};
            const f32 qq = v[k].tex[t][2];
            if (qq != 0 && qq != 1.0f)  // (as the rasterizer divides)
                g[0] /= qq, g[1] /= qq;
            const f32 nt[2] = {carried ? r.vertices[4 * q + k].tc[t][0] : 0.0f,
                               carried ? r.vertices[4 * q + k].tc[t][1] : 0.0f};
            bool within = true;
            for (u32 i = 0; i < 2; i++) {
                if (g[i] == nt[i])
                    continue;
                exact = false;
                within &= std::fabs(g[i] - nt[i]) <= std::max(1e-4f, 1e-3f / (f32)size[t][i]);
            }
            if (!within) {
                mismatch(F_TEXCOORD, q, d.index, strf("corner %u coordinate %u (%.6f, %.6f)", k, t, g[0], g[1]),
                         carried ? strf("(%.6f, %.6f)", nt[0], nt[1]) : strf("none (%u carried)", s.n_texgens));
                break;
            }
        }
    }
    if (!exact)
        c.inexact_tc++;
}

void report_position(const GxDraw& d, const GxOutVertex* v, u32 q) {
    StateCheck& c = s_state;
    u32 k = 0;
    for (; k < 3; k++) {
        f32 n[4];
        bool exact = true;
        project(d, c.call.vertices[4 * q + k].pos, n);
        if (!same_corner(d, v[k].clip, n, exact))
            break;
    }
    f32 gw[3], nc[4], nw[3];
    window(d, v[k].clip, gw);
    project(d, c.call.vertices[4 * q + k].pos, nc);
    window(d, nc, nw);
    mismatch(F_POSITION, q, d.index, strf("corner %u at (%.3f, %.3f), depth %.1f", k, gw[0], gw[1], gw[2]),
             strf("(%.3f, %.3f), depth %.1f", nw[0], nw[1], nw[2]));
}

// the native quads [from, to), which no GX quad matched
void report_missing(u32 from, u32 to) {
    StateCheck& c = s_state;
    if (!c.draws) {
        take_draw(c.draw);  // (for where they'd land: the decompiled draw drew nothing)
        c.draw.index = 0;
    }
    mismatch(F_MISSING, from, c.draw.index, "none",
             strf("%u quads from this one, the first at %s", to - from, native_box(c.draw, from).c_str()), to - from);
}

// The GX quads held, against the native quads before native quad `to`, which none matched: paired in
// order when there are as many of each (or the layout draw left no pane to the game's code), the rest
// extra and missing; else they are the quads of panes left to the game's code.
void settle(u32 to) {
    StateCheck& c = s_state;
    const u32 m = (u32)c.held.size(), k = to - c.next;
    if (m == k || !c.call.fallbacks) {
        const u32 pairs = std::min(m, k);
        for (u32 i = 0; i < pairs; i++) {
            const HeldQuad& h = c.held[i];
            const GxDraw& d = c.held_draws[h.draw];
            report_position(d, h.v, c.next + i);
            compare_quad(d, h.v, c.next + i);
        }
        for (u32 i = pairs; i < m; i++) {
            const HeldQuad& h = c.held[i];
            const GxDraw& d = c.held_draws[h.draw];
            const u32 before = c.next + pairs;
            mismatch(F_EXTRA, before, d.index, "a quad at " + gx_box(d, h.v),
                     before < c.call.quads() ? "none before this one, at " + native_box(d, before)
                                             : std::string("none (its quads all drawn)"));
        }
        if (k > pairs)
            report_missing(c.next + pairs, to);
    } else {
        c.through_guest += m;
        if (k)
            report_missing(c.next, to);
    }
    c.next = to;
    c.held.clear(), c.held_draws.clear();
    c.draw_held = false;
}

// one GX quad of the layout draw (its GX draw's state in StateCheck::draw)
void gx_quad(const GxOutVertex* v) {
    StateCheck& c = s_state;
    c.quads++, c.vertices += 4;
    const u32 last = std::min(c.call.quads(), c.next + LOOKAHEAD);
    for (u32 j = c.next; j < last; j++) {
        bool exact;
        if (!lands(c.draw, v, j, exact))
            continue;
        if (!c.held.empty() || j > c.next)
            settle(j);
        c.inexact_pos += !exact;
        compare_quad(c.draw, v, j);
        c.next = j + 1;
        return;
    }
    if (!c.draw_held) {
        c.held_draws.push_back(c.draw);
        c.draw_held = true;
    }
    HeldQuad h;
    memcpy(h.v, v, sizeof(h.v));
    h.draw = (u32)c.held_draws.size() - 1;
    c.held.push_back(h);
}

void finish_call() {
    StateCheck& c = s_state;
    settle(c.call.quads());
    c.in_call = false;
}

void state_marker(u32 serial, u32 phase) {
    StateCheck& c = s_state;
    if (phase == PHASE_END) {
        if (c.in_call && c.call.serial == serial)
            finish_call();
        else
            order_mismatch(strf("marker %u: a layout draw's END", serial), "no layout draw begun for it");
        report();
        return;
    }
    if (phase != PHASE_BEGIN) {
        order_mismatch(strf("marker %u: a MID", serial), "the state check has none");
        return;
    }
    if (c.in_call) {
        order_mismatch(strf("marker %u: a layout draw's BEGIN", serial),
                       strf("layout draw %u (layout %08X) not ended", c.call.serial, c.call.wrapper));
        finish_call();
    }
    Record r;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(c.lock);
        while (!c.pending.empty() && c.pending.front().serial != serial)
            c.pending.pop_front();
        if (!c.pending.empty()) {
            r = std::move(c.pending.front());
            c.pending.pop_front();
            found = true;
        }
    }
    if (!found) {
        order_mismatch(strf("marker %u: a layout draw's BEGIN", serial), "no layout draw queued for it");
        return;
    }
    c.call = std::move(r);
    c.in_call = true;
    c.next = c.draws = 0;
    c.held.clear(), c.held_draws.clear();
    c.draw_held = false;
    c.cmp_draw = c.cmp_state = ~0u;
    take_inherited(c.begin);
    c.calls++;
    c.fallbacks += c.call.fallbacks;
}

void state_gx_draw(u32 prim, u32 vat, u32 count, const u8* data) {
    StateCheck& c = s_state;
    take_draw(c.draw);
    c.draw.index = c.draws++;
    c.draw_held = false;
    static std::vector<guest::GxVertex> in;
    static std::vector<GxOutVertex> out;
    in.resize(count), out.resize(count);
    guest::gx_decode_vertices(vat, count, data, in.data());
    guest::gx_transform(in.data(), count, out.data());
    if (prim == 0x80 || prim == 0x88) {
        for (u32 i = 0; i + 3 < count; i += 4)
            gx_quad(&out[i]);
        if (count % 4)
            mismatch(F_PRIMITIVE, c.next, c.draw.index, strf("quads with %u vertices", count), "whole quads");
    } else if ((prim == 0x98 || prim == 0xA0) && count == 4) {
        // a strip's or a fan's four corners in GX_QUADS's order
        const GxOutVertex v[4] = {out[0], out[1], prim == 0x98 ? out[3] : out[2], prim == 0x98 ? out[2] : out[3]};
        gx_quad(v);
    } else {
        mismatch(F_PRIMITIVE, c.next, c.draw.index, strf("primitive %02X with %u vertices", prim, count), "quads");
    }
}

// ---------------------------------------------------------------- the pixel check (=pixels)

struct PixRecord {
    u32 serial = 0, wrapper = 0, frame = 0, quads = 0, fallbacks = 0;
};

// a layout draw whose pictures are on disk, kept while it is among the 20 worst
struct Kept {
    f64 psnr;
    bool by_xor;  // (also written for its coverage: kept anyway)
    std::string base;
};

struct PixCheck {
    std::mutex lock;
    std::deque<PixRecord> pending;  // layout draws whose packet or MID the decoder hasn't reached
    // the layout draw being compared (the decoder's): from its packet (its MID when the native draw has
    // no quads) to its END; `mid`: the decompiled draw's part; `drawn`: in a frame the renderer draws
    bool active = false, mid = false, drawn = false, bottom = false;
    PixRecord rec;
    const u32* efb = nullptr;  // the EFB it is drawn into (another one at MID or END: the screen changed)
    u32 gx_quads = 0;
    u32 width = 0, height = 0;
    std::vector<u32> snap_color, snap_depth, nat_color, nat_depth;
    // totals
    u64 draws = 0, compared = 0, differ = 0, differ_through_guest = 0, untouched = 0, undrawn = 0, moved = 0,
        order = 0;
    u32 lines = 0, seq = 0;
    bool unwritable = false;
    f64 worst_psnr = 99;
    u32 worst_psnr_at[2] = {0, 0};  // layout, game frame
    f64 worst_xor = 0;
    u32 worst_xor_at[2] = {0, 0};
    std::vector<Kept> kept;  // by PSNR, the worst first
    u64 last_report = 0;
};
PixCheck s_pix;

// every 10 s (at a layout draw's END)
void pixel_report() {
    PixCheck& c = s_pix;
    const u64 now = guest::host_time_us();
    if (now - c.last_report < 10000000)
        return;
    c.last_report = now;
    guest::log("lyt pixels: %llu layout draws: %llu compared, %llu differ, %llu of those with panes left to the "
               "game's code, %llu changed no pixel, %llu in frames not drawn (counted only), %llu across a screen "
               "switch, %llu out of order; worst PSNR %.1f dB (layout %08X, game frame %u), largest coverage XOR "
               "%.2f%% (layout %08X, game frame %u)",
               (unsigned long long)c.draws, (unsigned long long)c.compared, (unsigned long long)c.differ,
               (unsigned long long)c.differ_through_guest, (unsigned long long)c.untouched,
               (unsigned long long)c.undrawn, (unsigned long long)c.moved, (unsigned long long)c.order, c.worst_psnr,
               c.worst_psnr_at[0], c.worst_psnr_at[1], c.worst_xor, c.worst_xor_at[0], c.worst_xor_at[1]);
}

// a packet or a marker out of order (the first 40 logged)
void pixel_order(const std::string& what) {
    PixCheck& c = s_pix;
    if (c.order++ < 40)
        guest::log("lyt pixels: order: %s", what.c_str());
}

// the layout draw starts: the EFB as it is now is what both draws start from
void pixels_begin(const PixRecord& r) {
    PixCheck& c = s_pix;
    c.active = true, c.mid = false, c.rec = r, c.gx_quads = 0;
    c.drawn = s_drawing.load();
    if (!c.drawn)
        return;
    const guest::HostEfb e = guest::gx_host_efb();
    c.width = e.width, c.height = e.height;
    const size_t n = (size_t)e.width * e.height;
    c.snap_color.assign(e.color, e.color + n);
    c.snap_depth.assign(e.depth, e.depth + n);
    c.efb = e.color;
    c.bottom = guest::gx_host_on_bottom();
}

// the queued layout draw a packet (serial 0: the first one, which must be of `wrapper`) or a MID marker
// (its serial) belongs to; layout draws queued before it are passed over (their markers never came)
bool pixels_pop(u32 serial, u32 wrapper, PixRecord& r) {
    PixCheck& c = s_pix;
    std::lock_guard<std::mutex> lock(c.lock);
    if (serial)
        while (!c.pending.empty() && c.pending.front().serial != serial)
            c.pending.pop_front();
    if (c.pending.empty() || (!serial && c.pending.front().wrapper != wrapper))
        return false;
    r = c.pending.front();
    c.pending.pop_front();
    return true;
}

// the crops (gx, native, diff) of a layout draw's rectangle and 8 pixels around it
void write_pictures(const std::string& base, const u32* ref, s32 x0, s32 y0, s32 x1, s32 y1) {
    PixCheck& c = s_pix;
    x0 = std::max<s32>(x0 - 8, 0), y0 = std::max<s32>(y0 - 8, 0);
    x1 = std::min<s32>(x1 + 8, (s32)c.width - 1), y1 = std::min<s32>(y1 + 8, (s32)c.height - 1);
    const u32 w = (u32)(x1 - x0 + 1), h = (u32)(y1 - y0 + 1);
    std::vector<u32> gx(w * h), nat(w * h), diff(w * h);
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            const size_t i = (size_t)(y0 + y) * c.width + (u32)x0 + x;
            const u32 a = ref[i], b = c.nat_color[i];
            u32 d = 0xFF000000u;
            for (int ch = 0; ch < 3; ch++)
                d |= (u32)std::min(255, std::abs((int)(a >> (8 * ch) & 255) - (int)(b >> (8 * ch) & 255)) * 4) << (8 * ch);
            gx[y * w + x] = a | 0xFF000000u, nat[y * w + x] = b | 0xFF000000u, diff[y * w + x] = d;
        }
    bool ok = write_png(base + "_gx.png", gx.data(), w, h);
    ok &= write_png(base + "_native.png", nat.data(), w, h);
    ok &= write_png(base + "_diff.png", diff.data(), w, h);
    if (!ok && !c.unwritable) {
        c.unwritable = true;
        guest::log("lyt pixels: can't write %s_*.png (NOMA_CHECK_LYT_OUT): no pictures", base.c_str());
    }
}

// END: the decompiled draw's pixels (the EFB) against the native draw's, over the pixels either one
// changed: PSNR over R, G, B and the share of pixels a channel of which is more than 16 off, as
// platform/host/selftest_g3drender.cpp's compare(); the coverage XOR (pixels only one of them changed)
void pixels_compare() {
    PixCheck& c = s_pix;
    const guest::HostEfb e = guest::gx_host_efb();
    const u32* ref = e.color;
    const u32* ref_depth = e.depth;
    const size_t n = (size_t)c.width * c.height;
    u64 touched = 0, by_gx = 0, by_native = 0, one_only = 0, off = 0, colours = 0, depths = 0;
    f64 se = 0;
    s32 x0 = (s32)c.width, y0 = (s32)c.height, x1 = -1, y1 = -1;
    for (size_t i = 0; i < n; i++) {
        const bool g = ref[i] != c.snap_color[i] || ref_depth[i] != c.snap_depth[i];
        const bool nat = c.nat_color[i] != c.snap_color[i] || c.nat_depth[i] != c.snap_depth[i];
        if (!g && !nat)
            continue;
        touched++, by_gx += g, by_native += nat, one_only += g != nat;
        const s32 x = (s32)(i % c.width), y = (s32)(i / c.width);
        x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
        depths += ref_depth[i] != c.nat_depth[i];
        if (ref[i] == c.nat_color[i])
            continue;
        colours++;
        int worst = 0;
        for (int ch = 0; ch < 3; ch++) {
            const int d = (int)(ref[i] >> (8 * ch) & 255) - (int)(c.nat_color[i] >> (8 * ch) & 255);
            se += (f64)d * d;
            worst = std::max(worst, std::abs(d));
        }
        off += worst > 16;
    }
    if (!touched) {
        c.untouched++;
        return;
    }
    c.compared++;
    if (!colours && !depths)
        return;
    c.differ++;
    c.differ_through_guest += c.rec.fallbacks != 0;
    const f64 mse = se / (3.0 * (f64)touched);
    const f64 psnr = mse > 0 ? 10 * std::log10(255.0 * 255.0 / mse) : 99.0;
    const f64 off_pct = 100.0 * (f64)off / (f64)touched, xor_pct = 100.0 * (f64)one_only / (f64)touched;
    if (psnr < c.worst_psnr)
        c.worst_psnr = psnr, c.worst_psnr_at[0] = c.rec.wrapper, c.worst_psnr_at[1] = c.rec.frame;
    if (xor_pct > c.worst_xor)
        c.worst_xor = xor_pct, c.worst_xor_at[0] = c.rec.wrapper, c.worst_xor_at[1] = c.rec.frame;
    if (c.lines < 400) {
        const std::string through =
            c.rec.fallbacks ? strf(", %u panes left to the game's code: drawn by the decompiled draw only",
                                   c.rec.fallbacks)
                            : std::string();
        guest::log("lyt pixels: layout %08X game frame %u (%s screen, quads: GX %u, native %u%s): %llu px touched "
                   "(GX %llu / native %llu, coverage XOR %.2f%%), PSNR %.1f dB, off %.2f%%, colour differs at %llu "
                   "px, depth at %llu px, bbox (%d,%d)-(%d,%d)",
                   c.rec.wrapper, c.rec.frame, c.bottom ? "bottom" : "top", c.gx_quads, c.rec.quads, through.c_str(),
                   (unsigned long long)touched, (unsigned long long)by_gx, (unsigned long long)by_native, xor_pct, psnr,
                   off_pct, (unsigned long long)colours, (unsigned long long)depths, x0, y0, x1, y1);
        if (++c.lines == 400)
            guest::log("lyt pixels: (the layout draws that differ from here on are only counted)");
    }
    // the 20 worst by PSNR, and any whose coverage differs by more than 0.5%, as pictures
    const bool worst = c.kept.size() < 20 || psnr < c.kept.back().psnr, by_xor = xor_pct > 0.5;
    if (!worst && !by_xor)
        return;
    static const std::string s_dir = getenv("NOMA_CHECK_LYT_OUT") ? getenv("NOMA_CHECK_LYT_OUT") : "lytcheck";
    mkdir(s_dir.c_str(), 0755);
    const std::string base =
        s_dir + strf("/%06u_%05u_%08X%s", c.rec.frame, c.seq++, c.rec.wrapper, c.bottom ? "_bottom" : "");
    write_pictures(base, ref, x0, y0, x1, y1);
    if (!worst)
        return;
    c.kept.insert(std::upper_bound(c.kept.begin(), c.kept.end(), psnr, [](f64 p, const Kept& k) { return p < k.psnr; }),
                  Kept{psnr, by_xor, base});
    if (c.kept.size() > 20) {
        const Kept out = c.kept.back();
        c.kept.pop_back();
        if (!out.by_xor)
            for (const char* kind : {"_gx.png", "_native.png", "_diff.png"})
                unlink((out.base + kind).c_str());
    }
}

void pixel_marker(u32 serial, u32 phase) {
    PixCheck& c = s_pix;
    if (phase == PHASE_MID) {
        if (!c.active || c.rec.serial != serial) {
            if (c.active) {
                pixel_order(strf("marker %u: a layout draw's MID while layout draw %u (layout %08X) isn't ended",
                                 serial, c.rec.serial, c.rec.wrapper));
                c.active = false;
            }
            // (no packet came: the native draw has no quads, the EFB as it is is what it leaves)
            PixRecord r;
            if (!pixels_pop(serial, 0, r)) {
                pixel_order(strf("marker %u: a layout draw's MID, none queued for it", serial));
                return;
            }
            pixels_begin(r);
        } else if (c.mid) {
            pixel_order(strf("marker %u: a layout draw's second MID", serial));
            return;
        }
        c.mid = true;
        if (!c.drawn)
            return;
        const guest::HostEfb e = guest::gx_host_efb();
        if (e.color != c.efb) {
            c.drawn = false, c.moved++;
            return;
        }
        if (!s_drawing.load()) {
            // (the frame isn't drawn from here on: the native draw's pixels stay)
            c.drawn = false;
            return;
        }
        const size_t n = (size_t)c.width * c.height;
        c.nat_color.assign(e.color, e.color + n);
        c.nat_depth.assign(e.depth, e.depth + n);
        memcpy(e.color, c.snap_color.data(), n * sizeof(u32));
        memcpy(e.depth, c.snap_depth.data(), n * sizeof(u32));
        return;
    }
    if (phase != PHASE_END) {
        pixel_order(strf("marker %u: a BEGIN (the state check's)", serial));
        return;
    }
    if (!c.active || !c.mid || c.rec.serial != serial) {
        pixel_order(strf("marker %u: a layout draw's END, which isn't the one being compared", serial));
        c.active = false;
        return;
    }
    c.active = false;
    c.draws++;
    if (!c.drawn || !s_drawing.load())
        c.undrawn++;
    else if (guest::gx_host_efb().color != c.efb)
        c.moved++;
    else
        pixels_compare();
    pixel_report();
}

}  // namespace

// (platform/host/main.cpp, at each frame shown) whether the renderer rasterises the frames decoded
// from here on: the pixel check compares the layout draws of the frames it draws and counts the others
void check_host_drawing(bool drawing) { s_drawing.store(drawing); }

void check_draw(u32 wrapper, const Builder& b) {
    const CheckMode m = check_mode();
    if (m != CheckMode::STATE && m != CheckMode::PIXELS) {
        submit_native(b);
        return;
    }
    const u32 serial = ++s_serial & 0x0FFFFFFFu;
    const u32 frame = guest::vi_game_frames();
    if (m == CheckMode::STATE) {
        Record r;
        r.serial = serial, r.wrapper = wrapper, r.frame = frame, r.fallbacks = b.fallbacks;
        r.states = b.states;
        r.vertices = b.vertices;
        r.quad_state.assign(b.vertices.size() / 4, 0);
        for (const LytRun& run : b.runs)
            for (u32 v = run.first; v + 3 < run.first + run.count; v += 4)
                r.quad_state[v / 4] = run.state;
        r.quad_name.assign(r.quad_state.size(), 0);
        for (u32 q = 0; q < r.quad_state.size() && q < b.quad_pane.size(); q++) {
            const u32 pane = b.quad_pane[q];
            if (r.names.empty() || r.names.back().pane != pane) {
                PaneName n{pane, {}};  // (Pane +0xBC: the name, 16 characters)
                for (u32 i = 0; i < 16 && pane; i++)
                    if (!(n.name[i] = (char)guest::load8(pane + 0xBC + i)))
                        break;
                r.names.push_back(n);
            }
            r.quad_name[q] = (u32)r.names.size() - 1;
        }
        mutate(r.states, b.runs, r.vertices);
        {
            std::lock_guard<std::mutex> lock(s_state.lock);
            s_state.pending.push_back(std::move(r));
        }
        // (the GX library's pending state first, as packet_submit sends it before a packet: what the
        // decoder holds at BEGIN is what the packet would have been drawn with)
        if (guest::load32(GA___GXData))
            g::__GXSetDirtyState();
        write_marker(serial, PHASE_BEGIN);
        g::layout_LayoutWrapper_Draw_orig(wrapper);
        write_marker(serial, PHASE_END);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(s_pix.lock);
        s_pix.pending.push_back({serial, wrapper, frame, (u32)b.vertices.size() / 4, b.fallbacks});
    }
    if (mutation() != Mutate::NONE) {
        static Builder s_broken;
        s_broken = b;
        mutate(s_broken.states, s_broken.runs, s_broken.vertices);
        submit_native(s_broken);
    } else {
        submit_native(b);
    }
    write_marker(serial, PHASE_MID);
    g::layout_LayoutWrapper_Draw_orig(wrapper);
    write_marker(serial, PHASE_END);
}

bool check_marker(u32 ref) {
    const u32 phase = ref & 0xF;
    const CheckMode m = check_mode();
    if (phase < PHASE_BEGIN || phase > PHASE_END || (m != CheckMode::STATE && m != CheckMode::PIXELS))
        return false;
    if (m == CheckMode::STATE)
        state_marker(ref >> 4, phase);
    else
        pixel_marker(ref >> 4, phase);
    return true;
}

void check_gx_draw(u32 prim, u32 vat, u32 count, const u8* data) {
    if (s_pix.active && s_pix.mid)
        s_pix.gx_quads += quads_in(prim, count);
    if (!s_state.in_call || !count)
        return;
    NOMA_PROF_SCOPE("(lyt draw check)");
    state_gx_draw(prim, vat, count, data);
}

void check_packet(const LytPacket& p) {
    if (check_mode() != CheckMode::PIXELS)
        return;
    PixCheck& c = s_pix;
    if (c.active) {
        // (before its MID: the rest of the native draw's quads, a builder bigger than an arena entry)
        if (c.mid)
            pixel_order(strf("a native packet of layout %08X inside the decompiled draw of layout draw %u",
                             p.wrapper, c.rec.serial));
        return;
    }
    PixRecord r;
    if (!pixels_pop(0, p.wrapper, r)) {
        pixel_order(strf("a native packet of layout %08X, none queued for it", p.wrapper));
        return;
    }
    pixels_begin(r);
}

#endif

}  // namespace engine::lyt
