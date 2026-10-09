// nw4r::lyt's draw, native: the channel's 2D layouts drawn without the decompiled pane walk and
// without the GX FIFO's per-pane traffic.
//
// The game draws a layout with layout::LayoutWrapper::Draw (layout_LayoutWrapper_Draw, the one
// funnel every layout draw in the program goes through). Decompiled, that walked the pane tree
// through virtual calls, set each pane's material through some forty GX calls (Material::SetupGX)
// and sent each quad and each glyph through the write-gather pipe; the renderer then decoded all of
// it again. Here the walk is native (draw.cpp): it reads the panes, materials and strings in guest
// memory, works out what GX would have been told (the state of each run of quads, as the GX
// registers' values: draw_material.cpp, draw_text.cpp) and the quads' final vertices, and puts the
// lot into one packet. One FIFO command, LYT_OPCODE + the packet's reference (big-endian u32), takes
// it to the renderer at its place in the stream (between the 3D, the screen switches, the fades),
// which draws it straight away: the 3DS's GPU (platform/3ds/gx_pica.cpp, as PICA draws with the GX
// path's own shader and state caches), the host's software renderer (runtime/hw/gx/raster.cpp, by
// the same per-pixel GX pipeline as GX draws: the reference).
//
// What a packet doesn't carry is the GX state at its place in the stream: the projection (the
// layout's orthographic one: the entry still calls layout_DrawInfo_SetProjection and sends the GX
// library's pending state first, __GXSetDirtyState), the viewport, the scissor, the screen (0x51)
// and the EFB's pixel format. Those are the renderer's g_gx at the command, as for a GX draw there.
//
// Packets are copied into a byte arena the decoder's thread reads them from; the space is free again
// once the command is decoded (packet_done). The CPU's thread runs up to about a frame ahead of the
// 3DS's GPU thread: everything the guest may change before then (matrices, colours, strings, TEV
// colours) is in the packet; texture images and palettes are read by address at execution, as GX
// reads them.
//
// NOMA_CHECK_LYT_DRAW (draw_check.cpp): orig (the decompiled draw only), ab (native in even game
// frames, decompiled in odd ones), 1 (host: the state and vertices of each quad against the
// decompiled draw's GX draws), pixels (host: the native draw's pixels against the decompiled
// draw's). NOMA_LYT_STATS=N: a line every N game frames with the layouts' CPU time and counts.
#pragma once

#include <cstring>
#include <vector>

#include "engine/gx/tev_combiner.h"
#include "noma_decomp.h"

namespace guest {
struct GxRenderer;
}

namespace engine::lyt {

constexpr u8 LYT_OPCODE = 0x52;   // not a GX command; the decoder knows it as ours
constexpr u32 LYT_MAX_MAPS = 3;   // texture maps a run may use (the PICA has three units)
constexpr u32 LYT_MAX_COORDS = 3; // texture coordinates (texgen outputs) a vertex carries

// ---------------------------------------------------------------- the packet

// One GX texture map as GXLoadTexObj would have loaded it into map m: the BP registers' values
// (24-bit, without the register number). A palette format also carries the palette load GXLoadTlut
// would have sent first (BP 0x64 then 0x65; tlut_load[1] == 0: none): the renderer does that load
// before it looks the texture up, as the decoder would have.
struct LytTex {
    u32 mode0 = 0;              // BP 0x80+m (0xA0+m-4 for m >= 4): wrap s/t, filters, LOD bias...
    u32 mode1 = 0;              // BP 0x84+m: min/max LOD
    u32 image0 = 0;             // BP 0x88+m: width - 1, height - 1, format
    u32 image1 = 0, image2 = 0; // BP 0x8C+m, 0x90+m: TMEM (bit 21, preloaded, always clear)
    u32 image3 = 0;             // BP 0x94+m: the image's physical address >> 5
    u32 tlut = 0;               // BP 0x98+m: palette TMEM address >> 9, format << 10
    u32 tlut_load[2] = {0, 0};  // BP 0x64, 0x65
};

// The GX state a run of quads is drawn with: everything a layout draw sets per pane. Plain words, all
// of it zeroed before it is filled (states are compared with memcmp).
struct LytState {
    // GENMODE's TEV stage count, the stages' colour and alpha combiners (the alpha ones with their
    // swap selects), TREF, KSEL (with the swap tables in its low bits), the colour registers
    // (reg[0] PREV is never set by a layout: 0; reg[1..3] C0-C2) and the konst colours; entries
    // past n_stages zero
    gx::TevRegs tev;
    u32 n_texgens = 0;          // GENMODE's texgen count: the texture coordinates the vertices carry
    u32 n_chans = 1;            // GENMODE's colour channel count (1: the vertex colour is COLOR0A0)
    u32 n_maps = 0;             // texture maps 0 .. n_maps - 1 below (what TREF's map numbers name)
    LytTex tex[LYT_MAX_MAPS];
    u32 alpha_compare = 0;      // BP 0xF3
    u32 zmode = 0;              // BP 0x40: 0 outside mask panes
    u32 cmode0 = 0;             // BP 0x41: blend / logic op, dither, colour and alpha update
};

// One vertex, as GX's transform unit would have output it before the projection: the position in
// view space (the pane's global matrix times the pane-local position; the projection is the GX
// state's), the rasterized colour (colour channel 0 with lighting off: the vertex colour, or the
// material colour where the channel takes it from the register: R, G, B, A), the texture
// coordinates after their texgen matrices (GX's s, t: t = 0 at the image's top row; coordinate k
// for texgen k < LytState::n_texgens).
struct LytVertex {
    f32 pos[3];
    u8 color[4];
    f32 tc[LYT_MAX_COORDS][2];
};

// Quads drawn with one state: vertices [first, first + count), four a quad in GX_QUADS order (the
// corners as DrawQuad sends them: top left, top right, bottom right, bottom left of the pane).
struct LytRun {
    u32 state;
    u32 first;
    u32 count;
};

// The packet: this header, then its states, runs and vertices, one block in the arena.
struct LytPacket {
    u32 bytes;                  // the whole block's size
    u32 serial;                 // counts packets from 1
    u32 wrapper;                // the layout::LayoutWrapper drawn (guest address; for checks and stats)
    u32 n_states, n_runs, n_vertices;
    u32 states_at, runs_at, vertices_at;  // byte offsets from the header
    const LytState* states() const { return (const LytState*)((const u8*)this + states_at); }
    const LytRun* runs() const { return (const LytRun*)((const u8*)this + runs_at); }
    const LytVertex* vertices() const { return (const LytVertex*)((const u8*)this + vertices_at); }
};

// ---------------------------------------------------------------- building a packet (CPU thread)

// A layout draw's quads as the walk produces them. Reused from draw to draw (no allocation once
// warm).
struct Builder {
    std::vector<LytState> states;
    std::vector<LytRun> runs;
    std::vector<LytVertex> vertices;
    u32 wrapper = 0;
    // counts (NOMA_LYT_STATS): panes visited, panes drawn, glyph quads, panes drawn through the guest
    u32 panes = 0, drawn = 0, glyphs = 0, fallbacks = 0;
    // for the checks' reports (record_panes set by the walk when a check is on): the pane being
    // drawn (the walk sets it), and the pane of each quad
    bool record_panes = false;
    u32 pane = 0;
    std::vector<u32> quad_pane;

    void reset() {
        states.clear(), runs.clear(), vertices.clear(), quad_pane.clear();
        wrapper = 0, panes = drawn = glyphs = fallbacks = 0, pane = 0;
    }
    bool empty() const { return vertices.empty(); }
    // the state's index: the last one added when it is the same, else a new one
    u32 add_state(const LytState& s) {
        if (!states.empty() && !::memcmp(&states.back(), &s, sizeof s))
            return (u32)states.size() - 1;
        states.push_back(s);
        return (u32)states.size() - 1;
    }
    // n quads (4n vertices, zeroed) for the caller to fill, drawn with state `state`: added to the
    // last run when it has that state, else in a run of their own
    LytVertex* add_quads(u32 state, u32 n) {
        u32 first = (u32)vertices.size();
        vertices.resize(first + 4 * n);
        ::memset(&vertices[first], 0, 4 * n * sizeof(LytVertex));
        if (!runs.empty() && runs.back().state == state && runs.back().first + runs.back().count == first)
            runs.back().count += 4 * n;
        else
            runs.push_back({state, first, 4 * n});
        if (record_panes)
            quad_pane.insert(quad_pane.end(), n, pane);
        return &vertices[first];
    }
};

// What the walk hands a pane's draw: the GX state outside the material that is in effect for it.
struct DrawCtx {
    u32 cmode0_keep;  // CMODE0's dither, colour update and alpha update bits (2-4) in effect
    u32 zmode;        // BP 0x40 in effect: 0, or a mask pane's (draw.cpp)
};

// The pane's global matrix (pane + 0x84: three rows of four f32) times (x, y, z, 1), as the transform
// unit does it (runtime/hw/gx/xform.cpp mul34: the same expression, so that the host's floats come
// out the same).
inline void mtx_apply(const f32 m[3][4], const f32 v[4], f32 out[3]) {
    for (int r = 0; r < 3; r++)
        out[r] = m[r][0] * v[0] + m[r][1] * v[1] + m[r][2] * v[2] + m[r][3] * v[3];
}

// a pane's global matrix (+0x84) from guest memory
void pane_matrix(u32 pane, f32 out[3][4]);

// ---- draw_material.cpp: Material::SetupGX(m, modulate, alpha) as state

// a texture coordinate generator: coordinate k = m (two rows of 2x4) times (s, t, 1, 1) of the
// pane's texture coordinate set `src` (as xform.cpp's texgen: the same expression)
struct LytTexGen {
    u32 src;          // the pane's texture coordinate set (GX_TG_TEX0 + src)
    f32 m[2][4];
};
struct LytMaterial {
    LytState state;       // all but zmode and cmode0's bits 2-4 (the caller's: DrawCtx)
    bool use_vtx;         // SetupGX's result: the pane's vertex colours are sent (else none are)
    bool color_from_vtx;  // the channel's colour from the vertex (else the material colour below)
    bool alpha_from_vtx;  // the channel's alpha from the vertex
    u32 mat_color;        // the material colour (R in the high byte), with the alpha multiplied
    u32 n_gens;
    LytTexGen gen[LYT_MAX_COORDS];
};
// false: the material needs what the native draw doesn't do (more maps or coordinates than the PICA
// has, indirect stages); the caller then draws the pane through the guest's own DrawSelf
bool material_state(u32 material, bool modulate, u8 alpha, LytMaterial& out);
// a GXTexObj in guest memory (as GXInitTexObj and its kin made it) as GXLoadTexObj would load it
// into a map (with the palette load for a palette format, from the GX library's TLUT region)
LytTex tex_from_obj(u32 obj);
// a quad's rasterized colours as GX would have them: vertex colours vc (4 RGBA words as DrawQuad
// sends them, the alpha already multiplied; null: none sent) through the material's channel
void material_colors(const LytMaterial& m, const u32* vc, u8 out[4][4]);

// ---- draw_text.cpp: TextBox::DrawSelf

// the text box's glyphs, appended to b (with CharWriter's state); false: something the native draw
// doesn't do (the caller draws the pane through the guest)
bool draw_textbox(Builder& b, u32 pane, const DrawCtx& ctx);

// ---------------------------------------------------------------- the arena (draw.cpp)

// CPU thread: send __GXSetDirtyState, copy b into the arena as a packet and put its command into the
// FIFO. Waits for the decoder when the arena is full.
void packet_submit(const Builder& b);
// The decoder's thread (runtime/hw/gx/cp.cpp, at LYT_OPCODE): the packet `ref` names goes to the
// renderer (GxRenderer::lyt), and its space is free again; check markers go to check_marker.
void decode(u32 ref, guest::GxRenderer& renderer);

// ---------------------------------------------------------------- checks (draw_check.cpp)

enum class CheckMode { NONE, ORIG, AB, STATE, PIXELS };
CheckMode check_mode();  // NOMA_CHECK_LYT_DRAW, read once
// With STATE or PIXELS: the native packet `b` (built, not sent) and the decompiled draw of `wrapper`
// (layout_LayoutWrapper_Draw_orig, which this calls), compared on the host as the mode says.
void check_draw(u32 wrapper, const Builder& b);
// the decoder met a check marker (a reference with its low bits set): true if it was one
bool check_marker(u32 ref);
// the host renderer, at each GX draw (the decompiled draw's, between markers) and at each packet
void check_gx_draw(u32 prim, u32 vat, u32 count, const u8* data);
void check_packet(const LytPacket& p);
// the host renderer: whether it rasterises the frames decoded from here on (the pixel check runs only
// in those)
void check_host_drawing(bool drawing);

}  // namespace engine::lyt
