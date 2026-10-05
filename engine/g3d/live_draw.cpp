// The channel's 3D models drawn natively (see live_draw.h).
//
// nw4r::g3d's draw of one model, as the channel's g3d (NW4R G3D, Dec 22 2009) does it: ScnMdl /
// ScnMdlSimple::G3dProc, given DRAW_OPA or DRAW_XLU by its ScnRoot's pass, calls
// DrawResMdlDirectly(resMdl, viewPos MTX34[], viewNrm MTX33[], viewEnv MTX34[], byteCodeOpa,
// byteCodeXlu, DrawResMdlReplacement*, drawMode). It walks the model's DrawOpa / DrawXlu byte code
// (nw4r_g3d_DrawByteCode, ...Replace with a ScnMdl's per-instance buffers; by view depth through
// nw4r_g3d_MakeDrawSortList and std::sort when drawMode asks: bit 1 opaque, 2 translucent), and
// for each draw that is visible (its node's flag 0x100, or the replacement's byte; the shape's flag
// 2 clear) calls nw4r_g3d_Draw1Mat1ShpDirectly: the material's GX state (from its display lists,
// texture objects, channels, texture SRTs and the G3DState caches; none when it is the previous
// draw's material), then the shape: its matrices (a single-matrix shape's from the view arrays at
// its current matrix, into slot 0; a skinned shape loads its own by index from its primitive list,
// the vertices naming theirs), and its primitive display list.
//
// This first part re-derives what those draws leave in the GPU's registers natively and checks it
// against the game's own (NOMA_CHECK_G3D_LIVE, host): every primitive's vertices as the shape's
// lists give them (live_shape.cpp, written apart from the GX decoder) and the matrices they use,
// draw by draw, in the order the game draws them.
#include "engine/g3d/live_draw.h"

#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/g3d/live_material.h"
#include "engine/g3d/live_res.h"
#include "engine/g3d/live_shape.h"
#include "noma_decomp.h"
#include "runtime/hw/gx/gx.h"
#include "runtime/hw/hw.h"
#include "runtime/hw/gx/vertex.h"
#include "runtime/runtime.h"

namespace g {
void nw4r_g3d_DrawResMdlDirectly_orig(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7, u32 r8, u32 r9, u32 r10);
}

namespace engine::g3d::live {


using ::guest::gp;

namespace {

// NOMA_CHECK_G3D_LIVE=1: the game's own draw, the derivation checked against it; =native: the native
// draw, checked against the derivation (what it sends, as the GPU's decoder takes it)
// =ab: the native draw in even game frames, the game's in odd ones (frames dumped two in a row show
// the same scene drawn both ways)
enum class Mode { NATIVE, CHECK_GAME, CHECK_NATIVE, AB };
Mode mode() {
    static const Mode m = [] {
        const char* e = getenv("NOMA_CHECK_G3D_LIVE");
        if (!e || !strcmp(e, "0"))
            return Mode::NATIVE;
        if (!strcmp(e, "ab")) {
            ::guest::log("g3d live: the native model draws in even game frames, the game's in odd ones");
            return Mode::AB;
        }
        if (!strcmp(e, "native")) {
            ::guest::log("g3d live: checking the native model draws against their derivation");
            return Mode::CHECK_NATIVE;
        }
        ::guest::log("g3d live: checking the native derivation of every model draw against the game's");
        return Mode::CHECK_GAME;
    }();
    return m;
}

// ---- the decoded shapes, kept while their primitive list is unchanged
struct ShapeEntry {
    u32 gen = 0;
    DecodedShape shape;
};
std::unordered_map<u32, ShapeEntry> s_shapes;

[[maybe_unused]] const DecodedShape& shape_of(u32 s) {
    const u32 list = shp::dl_addr(s, shp::PRIM), size = shp::dl_size(s, shp::PRIM);
    const u32 gen = list ? ::guest::mem_generation(list, size) : 0;
    auto it = s_shapes.find(s);
    if (it == s_shapes.end() || it->second.gen != gen) {
        ShapeEntry e;
        e.gen = gen;
        e.shape = decode_shape(s);
        it = s_shapes.insert_or_assign(s, std::move(e)).first;
    }
    return it->second.shape;
}

// ---- one model draw's draws, in the game's order
struct Draw {
    u32 mat, shp, node;
    u32 ctrl;  // Draw1Mat1ShpDirectly's: 0x10 no material (the previous draw's), 8 lighting off, 1 no sync
    // the arrays it reads in place of the shape's own: a ScnMdl's own vertex buffers (the
    // replacement's tables, by the shape's array ids: position, normal, colour 0 and 1)
    ArrayOverride arrays[4];
    // fur (the material has fur data): the shape is drawn once a layer, the position array that
    // layer's (the shape's ResFurPos: positions at +0x8, stride +0x1D, layers +0x20, bytes a layer +0x24)
    u32 fur_pos = 0;
    u32 layers() const { return fur_pos ? load32(fur_pos + 0x20) : 1; }
    ArrayOverride layer_pos(u32 layer) const {
        return {rel(fur_pos, fur_pos + 0x8) + layer * load32(fur_pos + 0x24), load8(fur_pos + 0x1D)};
    }
};

// a vertex array's (ResVtxPos/Nrm: data +0x8, stride +0x1D; ResVtxClr: stride +0x1C) GXSetArray
ArrayOverride vtx_array(u32 res, bool color) {
    return res ? ArrayOverride{rel(res, res + 0x8), load8(res + (color ? 0x1C : 0x1D))} : ArrayOverride{};
}

// a draw of material `mt`, shape `s`, node `n`, as Draw1Mat1ShpDirectly takes it
Draw make_draw(u32 mt, u32 s, u32 n, u32 ctrl, u32 repl) {
    Draw d{mt, s, n, ctrl};
    if (mat::fur(mt)) {
        // (ResShp +0x62: the model's fur position index; +0x4 the model, its fur dictionary +0x2C)
        const s16 fp = (s16)load16(s + 0x62);
        const u32 m = rel(s, s + 0x4);
        d.fur_pos = fp >= 0 && m ? dic_data(rel(m, m + 0x2C), (u32)fp) : 0;
    } else if (repl && !(load32(repl) & 1)) {
        const s16 id[4] = {(s16)load16(s + 0x48), (s16)load16(s + 0x4A), (s16)load16(s + 0x4C), (s16)load16(s + 0x4E)};
        const u32 table[4] = {load32(repl + 0x34), load32(repl + 0x38), load32(repl + 0x3C), load32(repl + 0x3C)};
        for (int a = 0; a < 4; a++)
            if (table[a] && id[a] >= 0)
                d.arrays[a] = vtx_array(load32(table[a] + 4 * (u32)id[a]), a >= 2);
    }
    return d;
}

// the visible draws of a byte code list, in its order (nw4r_g3d_DrawByteCode / ...Replace)
void walk_byte_code(u32 m, u32 code, u32 repl, u32 draw_mode, std::vector<Draw>& out) {
    const u32 base = 1 | (draw_mode & 8 ? 8 : 0) | (draw_mode & 4 ? 0x10 : 0);
    const u32 vis = repl ? load32(repl + repl::VIS) : 0;
    u32 last_mat = 0;
    for (u32 p = code; load8(p) != bc::END; p += 8) {
        const bc::Draw d = bc::draw(p);
        const u32 n = mdl::node(m, d.node), s = mdl::shape(m, d.shp);
        const bool visible = vis ? load8(vis + (n ? node::id(n) : 0)) != 0 : n && (node::flags(n) & node::VISIBLE);
        if (!visible || (shp::flags(s) & shp::INVISIBLE))
            continue;
        const u32 mt = mdl::material(m, d.mat);
        out.push_back(make_draw(mt, s, n, base | (mt == last_mat ? 0x10u : 0), repl));
        last_mat = mt;
    }
}

// ... and of a list sorted by view depth (the game's own sort: std::sort isn't stable, so the
// order of equal keys is its algorithm's)
void walk_sorted(u32 mdl_handle, u32 m, u32 vpos, u32 code, u32 repl, u32 draw_mode, bool xlu, std::vector<Draw>& out) {
    GFRAME(0x20);
    const u32 count_at = (u32)&GS(u32, 0x8), handle = (u32)&GS(u32, 0xC);
    GS(u32, 0xC) = load32(mdl_handle);
    const u32 list = g::nw4r_g3d_MakeDrawSortList(count_at, handle, vpos, code, repl);
    const u32 n = load32(count_at);
    g::nw4r_g3d_SortDrawList(list, list + n * 0xC, xlu ? GA_nw4r_g3d_DrawSortLessXlu : GA_nw4r_g3d_DrawSortLessOpa);
    const u32 base = 1 | (draw_mode & 8 ? 8 : 0) | (draw_mode & 4 ? 0x10 : 0);
    u32 last_mat = 0;
    for (u32 i = 0; i < n; i++) {
        const u32 e = list + i * 0xC;
        const u32 s = mdl::shape(m, load16(e + 0xA));
        if (shp::flags(s) & shp::INVISIBLE)
            continue;
        const u32 mt = mdl::material(m, load16(e + 8));
        out.push_back(make_draw(mt, s, mdl::node(m, load16(e + 6)), base | (mt == last_mat ? 0x10u : 0), repl));
        last_mat = mt;
    }
}

void model_draws(u32 r3, u32 vpos, u32 opa, u32 xlu, u32 repl, u32 draw_mode, std::vector<Draw>& out) {
    const u32 m = load32(r3);
    if (opa) {
        if (draw_mode & 1)
            walk_sorted(r3, m, vpos, opa, repl, draw_mode, false, out);
        else
            walk_byte_code(m, opa, repl, draw_mode, out);
    }
    if (xlu) {
        if (draw_mode & 2)
            walk_sorted(r3, m, vpos, xlu, repl, draw_mode, true, out);
        else
            walk_byte_code(m, xlu, repl, draw_mode, out);
    }
}

// ---- the native draw: each draw's registers (the derivation's writes) and its shape's display
// lists into the GX FIFO, in the order the game's Draw1Mat1ShpDirectly sends them. A register this
// model draw has already sent with the same value is left out (other GX code may run between model
// draws: nothing is taken as sent from one to the next).

struct Sent {
    u32 bp[256], cp[256], xf_mem[0x700], xf_reg[0x60];
    bool bp_ok[256], cp_ok[256], xf_mem_ok[0x700], xf_reg_ok[0x60];
    u32 vcd[3];
    bool vcd_ok;
    void clear() {
        memset(bp_ok, 0, sizeof(bp_ok)), memset(cp_ok, 0, sizeof(cp_ok));
        memset(xf_mem_ok, 0, sizeof(xf_mem_ok)), memset(xf_reg_ok, 0, sizeof(xf_reg_ok));
        vcd_ok = false;
    }
};

void put_bp(u32 cmd) {
    ::guest::gather_put<1>(0x61);
    ::guest::gather_put<4>(cmd);
}
void put_cp(u32 reg, u32 v, Sent& s) {
    if (s.cp_ok[reg] && s.cp[reg] == v)
        return;
    s.cp[reg] = v, s.cp_ok[reg] = true;
    ::guest::gather_put<1>(0x08);
    ::guest::gather_put<1>(reg);
    ::guest::gather_put<4>(v);
}
void put_xf(u32 addr, const u32* v, u32 n) {
    ::guest::gather_put<1>(0x10);
    ::guest::gather_put<4>((n - 1) << 16 | addr);
    for (u32 i = 0; i < n; i++)
        ::guest::gather_put<4>(v[i]);
}
// LOAD_INDX_A/B (op 0x20 / 0x28): `n` words of matrix `index` of the CP's array into XF `addr`
void put_indexed(u32 op, u32 index, u32 addr, u32 n) {
    ::guest::gather_put<1>(op);
    ::guest::gather_put<4>(index << 16 | (n - 1) << 12 | addr);
}
void put_call(u32 list, u32 size) {
    ::guest::gather_put<1>(0x40);
    ::guest::gather_put<4>(list);
    ::guest::gather_put<4>(size);
}

// The derivation's writes into the GX FIFO: BP and XF loads (XF words in runs of up to 16), a
// register already sent with the same value in this model draw left out, a display list called
// (what it sets is then not known as sent)
struct FifoSink : GxSink {
    Sent& s;
    u32 run[16], run_addr = 0, run_n = 0;
    bool masked = false;  // the next BP write has a mask
    explicit FifoSink(Sent& sent) : s(sent) {}
    void flush() {
        if (run_n)
            put_xf(run_addr, run, run_n), run_n = 0;
    }
    void bp_write(u32 cmd) override {
        flush();
        const u32 reg = cmd >> 24, v = cmd & 0xFFFFFF;
        if (reg == 0xFE) {
            put_bp(cmd);
            masked = true;
            return;
        }
        if (masked) {
            // (the register is then only partly known)
            put_bp(cmd);
            s.bp_ok[reg] = masked = false;
            return;
        }
        if (s.bp_ok[reg] && s.bp[reg] == v)
            return;
        s.bp[reg] = v, s.bp_ok[reg] = true;
        put_bp(cmd);
    }
    void xf_write(u32 addr, u32 v) override {
        u32* slot = addr >= 0x1000 ? &s.xf_reg[addr - 0x1000] : &s.xf_mem[addr];
        bool* ok = addr >= 0x1000 ? &s.xf_reg_ok[addr - 0x1000] : &s.xf_mem_ok[addr];
        if (*ok && *slot == v) {
            flush();
            return;
        }
        *slot = v, *ok = true;
        if (run_n && (addr != run_addr + run_n || run_n == 16))
            flush();
        if (!run_n)
            run_addr = addr;
        run[run_n++] = v;
    }
    bool apply_dl(u32 addr, u32 size) override {
        flush();
        put_call(addr, size);
        memset(s.bp_ok, 0, sizeof(s.bp_ok));
        memset(s.xf_reg_ok, 0, sizeof(s.xf_reg_ok));
        return true;
    }
};

// the shape: its vertex format (the pre-primitive list; from its arrays on when the format is
// the last shape's, as nw4r skips it), the arrays the draw reads instead of its own, a
// single-matrix shape's matrices (slot 0, from the view arrays), the matrix index registers'
// defaults in CP too when they are the defaults, the primitive list
void emit_shape(const Draw& d, bool mtx_defaults, Sent& s) {
    const u32 shp = d.shp;
    const u32 vcd[3] = {load32(shp + 0xC), load32(shp + 0x10), load32(shp + 0x14)};
    const u32 pre = shp::dl_addr(shp, shp::PRE_PRIM), pre_n = shp::dl_size(shp, shp::PRE_PRIM);
    if (s.vcd_ok && !memcmp(vcd, s.vcd, sizeof(vcd)))
        put_call(pre + 0x20, pre_n - 0x20);
    else
        put_call(pre, pre_n);
    memcpy(s.vcd, vcd, sizeof(vcd)), s.vcd_ok = true;
    for (u32 r = 0x50; r < 0xAC; r++)  // (what the list set: the vertex format, the attribute arrays)
        s.cp_ok[r] = false;
    for (u32 r = 0xB0; r < 0xBC; r++)
        s.cp_ok[r] = false;
    for (u32 a = 0; a < 4; a++)
        if (d.arrays[a].base)
            put_cp(0xA0 + a, d.arrays[a].base & 0x3FFFFFFF, s), put_cp(0xB0 + a, d.arrays[a].stride, s);
    const s32 cur = shp::cur_mtx(shp);
    if (cur >= 0) {
        put_indexed(0x20, (u32)cur, 0, 12);
        if (shp::attrs(shp) & 0x400)
            put_indexed(0x28, (u32)cur, 0x400, 9);
    }
    if (mtx_defaults)
        put_cp(0x30, 0x3CF3CF00u, s), put_cp(0x40, 0x00F3CF3Cu, s);
    put_call(shp::dl_addr(shp, shp::PRIM), shp::dl_size(shp, shp::PRIM));
}

// (for now) a draw the derivation can't do, drawn by the game's own Draw1Mat1ShpDirectly, which
// trusts G3DState's caches: they are emptied first, and the view arrays given to it
void draw_by_game(const Draw& d, u32 vpos, u32 vnrm, u32 venv, u32 repl, Sent& s) {
    g::nw4r_g3d_G3DState_Invalidate(0x7FF);
    g::nw4r_g3d_G3DState_SetViewPosNrmMtxArray(vpos, vnrm, venv);
    GFRAME(0x60);
    const u32 mat = (u32)&GS(u32, 0x8), shpv = (u32)&GS(u32, 0xC), node = (u32)&GS(u32, 0x10);
    const u32 swap = (u32)&GS(u8, 0x18);  // Draw1Mat1ShpSwap, 0x38 bytes
    GS(u32, 0x8) = d.mat, GS(u32, 0xC) = d.shp, GS(u32, 0x10) = d.node;
    if (repl)
        g::nw4r_g3d_MakeDraw1Mat1ShpSwap(swap, repl, mat::id(d.mat));
    const u32 op = g::nw4r_g3d_SetupIndTexMtxOp(mat, node, shpv);
    g::nw4r_g3d_Draw1Mat1ShpDirectly(mat, shpv, 0, 0, d.ctrl & ~0x10u, repl ? swap : 0, op);
    g::nw4r_g3d_G3DState_SetViewPosNrmMtxArray(0, 0, 0);
    s.clear();
}

// the model draw: its draws' registers and shapes
struct Frame {
    std::vector<Draw> draws;
    Sent sent;
};
Frame s_frame;

void draw_native(u32 r3, u32 vpos, u32 vnrm, u32 venv, u32 opa, u32 xlu, u32 repl, u32 draw_mode) {
    Frame& f = s_frame;
    f.draws.clear();
    model_draws(r3, vpos, opa, xlu, repl, draw_mode, f.draws);
    Sent& s = f.sent;
    s.clear();
    // the view matrix arrays (GXSetArray 0x15-0x17: the indexed loads' arrays), their DMA from the
    // locked cache done
    auto view_arrays = [&] {
        put_cp(0xAC, vpos & 0x3FFFFFFF, s), put_cp(0xBC, 0x30, s);
        put_cp(0xAD, vnrm & 0x3FFFFFFF, s), put_cp(0xBD, 0x24, s);
        put_cp(0xAE, venv & 0x3FFFFFFF, s), put_cp(0xBE, 0x30, s);
    };
    view_arrays();
    if (!(draw_mode & 0x10))
        g::LCQueueWait(0);
    FifoSink out(s);
    LightCache lc = light_cache_now();
    MatShared sh = mat_shared_now();
    for (const Draw& d : f.draws) {
        if (derive_material(d.mat, mat_sources(d.mat, repl), d.ctrl, lc, sh, out)) {
            const bool defaults = derive_shape(d.shp, vpos, vnrm, sh, out);
            out.flush();
            emit_shape(d, defaults, s);
        } else {
            out.flush();
            draw_by_game(d, vpos, vnrm, venv, repl, s);
            view_arrays();
            lc = light_cache_now();
            sh = mat_shared_now();
        }
    }
    out.flush();
    put_cp(0xAC, 0, s), put_cp(0xBC, 0, s), put_cp(0xAD, 0, s), put_cp(0xBD, 0, s), put_cp(0xAE, 0, s), put_cp(0xBE, 0, s);
    // G3DState as the draws left the GPU: the light cache moved on as they moved it; nothing else
    // it keeps (textures, TEV, gen mode...) is still true
    g::nw4r_g3d_G3DState_Invalidate(0x5FF);
    light_cache_store(lc);
}

// ---- NOMA_CHECK_G3D_LIVE

const u32 CHECK_MARK = 0xC4EC0000u;  // the check's own 0x50 ids: CHECK_MARK | (call & 0x7FFF) << 1 | end

struct CheckCall {
    u32 serial;
    u32 mdl;
    std::string name;  // (taken at the call: a scene change frees the model)
    u32 vpos, vnrm, venv;
    std::vector<Draw> draws;
    // the derivation's register writes, draw by draw (draw k's end at writes_end[k]), and whether
    // it could derive the draw's material
    std::vector<GxRegs::Write> writes;
    std::vector<u32> writes_end;
    std::vector<u8> derived;
};

struct Check {
    std::deque<CheckCall> pending;  // calls whose GX output the decoder hasn't reached yet
    bool in_call = false;
    CheckCall call;
    size_t draw = 0, prim = 0;
    u32 layer = 0;
    // the current draw's (or fur layer's) shape, decoded as it starts (its arrays read when the
    // GPU's decoder reads them)
    bool have_shape = false;
    DecodedShape shape;
    // the matrix slots the current skinned draw's loads filled: (array, XF address) -> index
    std::unordered_map<u32, u16> slots;
    // the registers as the derivation's writes so far in this call leave them
    GxRegs regs;
    std::unordered_map<u32, u64> reg_mismatches;  // (space << 16 | address) -> count (each logged once)
    u64 reg_compares = 0, words_compared = 0;
    u32 serial = 0;
    // totals
    u64 calls = 0, draws = 0, prims = 0, vertices = 0, mismatches = 0, undecoded = 0;
    std::unordered_map<std::string, u64> by_model;
    std::unordered_map<std::string, u64> models_seen;  // model -> calls (listed once)
    bool listed = false;
    u32 logged = 0;
    u64 last_report = 0;
};
Check s_check;

void write_marker(u32 id) {
    ::guest::gx_fifo_write(0, 0x50, 1);
    ::guest::gx_fifo_write(0, id, 4);
}

std::string model_name(u32 m) {
    const u32 n = rel(m, m + 0x48);
    return n ? ::guest::read_string(n, 64) : std::string("?");
}

void mismatch(const char* what, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void mismatch(const char* what, const char* fmt, ...) {
    Check& c = s_check;
    c.mismatches++;
    c.by_model[c.call.name]++;
    if (c.logged >= 40)
        return;
    c.logged++;
    char detail[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    const Draw* d = c.draw < c.call.draws.size() ? &c.call.draws[c.draw] : nullptr;
    ::guest::log("g3d live check: %s: model %s, draw %zu (shape %u, material %u), layer %u, primitive %zu: %s", what,
                 c.call.name.c_str(), c.draw, d ? shp::id(d->shp) : 0, d ? mat::id(d->mat) : 0, c.layer, c.prim, detail);
}

void report(bool force) {
    Check& c = s_check;
    const u64 now = ::guest::host_time_us();
    if (!force && now - c.last_report < 10000000)
        return;
    c.last_report = now;
    ::guest::log("g3d live check: %llu model draws, %llu shape draws, %llu primitives, %llu vertices: %llu "
                 "mismatches, %llu shapes not decoded",
                 (unsigned long long)c.calls, (unsigned long long)c.draws, (unsigned long long)c.prims,
                 (unsigned long long)c.vertices, (unsigned long long)c.mismatches, (unsigned long long)c.undecoded);
    for (const auto& [name, n] : c.by_model)
        ::guest::log("g3d live check:   %s: %llu mismatches", name.c_str(), (unsigned long long)n);
    ::guest::log("g3d live check: %llu draws' registers compared (%llu register words), %zu registers ever differed",
                 (unsigned long long)c.reg_compares, (unsigned long long)c.words_compared, c.reg_mismatches.size());
    if (!c.listed && c.calls > 1000) {
        c.listed = true;
        std::string list;
        for (const auto& [name, n] : c.models_seen)
            list += name + " " + std::to_string(n) + ", ";
        ::guest::log("g3d live check: models drawn so far: %s", list.c_str());
    }
}

// the words of a matrix in a view array (big-endian f32 bits), against XF memory
void compare_matrix(const char* what, u32 array, u32 index, u32 stride, u32 xf_addr, u32 words) {
    if (!array) {
        mismatch(what, "no view array for matrix %u", index);
        return;
    }
    for (u32 k = 0; k < words; k++) {
        const u32 want = load32(array + index * stride + 4 * k), got = ::guest::g_gx.xf[xf_addr + k];
        if (want != got) {
            mismatch(what, "XF %03X+%u is %08X, the view array's matrix %u has %08X", xf_addr, k, got, index, want);
            return;
        }
    }
}

// a register the derivation knows that the GPU holds differently (each logged the first time)
void reg_mismatch(u32 space, u32 addr, u32 got, u32 want, u32 known) {
    Check& c = s_check;
    if (c.reg_mismatches[space << 16 | addr]++ == 0) {
        static const char* const k_space[] = {"BP", "XF", "TEV register", "TEV konst"};
        mismatch("register", "%s %03X: the GPU's %08X, the derivation's %08X (bits %08X)", k_space[space], addr, got, want,
                 known);
    } else {
        c.mismatches++;
    }
}

// every register the derivation knows, against the GPU's (as the decoder holds them for this draw)
void compare_regs(const GxRegs& w) {
    const ::guest::GxState& g = ::guest::g_gx;
    s_check.reg_compares++;
    for (u32 r = 0; r < 256; r++)
        s_check.words_compared += w.bp_known[r] != 0;
    for (u32 a = 0; a < 0x60; a++)
        s_check.words_compared += w.xf_known[a];
    for (u32 a = 0; a < 0x700; a++)
        s_check.words_compared += w.xf_mem_known[a];
    for (u32 r = 0; r < 256; r++)
        if (w.bp_known[r] && ((g.bp[r] ^ w.bp[r]) & w.bp_known[r]))
            reg_mismatch(0, r, g.bp[r], w.bp[r], w.bp_known[r]);
    for (u32 i = 0; i < 4; i++)
        for (u32 k = 0; k < 4; k++) {
            if ((w.tev_reg_known[i] >> k) & 1 && g.tev_reg[i][k] != w.tev_reg[i][k])
                reg_mismatch(2, i * 4 + k, (u32)g.tev_reg[i][k], (u32)w.tev_reg[i][k], 0x7FF);
            if ((w.tev_konst_known[i] >> k) & 1 && g.tev_konst[i][k] != w.tev_konst[i][k])
                reg_mismatch(3, i * 4 + k, (u32)g.tev_konst[i][k], (u32)w.tev_konst[i][k], 0x7FF);
        }
    for (u32 a = 0; a < 0x60; a++)
        if (w.xf_known[a] && g.xf[0x1000 + a] != w.xf[a])
            reg_mismatch(1, 0x1000 + a, g.xf[0x1000 + a], w.xf[a], ~0u);
    for (u32 a = 0; a < 0x700; a++)
        if (w.xf_mem_known[a] && g.xf[a] != w.xf_mem[a])
            reg_mismatch(1, a, g.xf[a], w.xf_mem[a], ~0u);
}

void check_vertices(const CheckCall& call, const Draw& d, const DecodedShape& s, const ShapePrim& pr,
                    const ::guest::GxVertex* got) {
    const s32 cur = shp::cur_mtx(d.shp);
    const bool nrm = s.attrs & (1u << 10);
    for (u32 i = 0; i < pr.count; i++) {
        const ShapeVertex& w = s.verts[pr.first + i];
        const ::guest::GxVertex& v = got[i];
        if (s.attrs & (1u << 9))
            for (int k = 0; k < 3; k++)
                if (memcmp(&w.pos[k], &v.pos[k], 4)) {
                    mismatch("position", "vertex %u: %g, the GPU's %g (component %d)", i, w.pos[k], v.pos[k], k);
                    return;
                }
        if (nrm)
            for (int k = 0; k < 3; k++)
                if (memcmp(&w.nrm[k], &v.nrm[0][k], 4)) {
                    mismatch("normal", "vertex %u: %g, the GPU's %g", i, w.nrm[k], v.nrm[0][k]);
                    return;
                }
        for (int ch = 0; ch < 2; ch++)
            if ((s.attrs & (1u << (11 + ch))) && memcmp(w.clr[ch], v.color[ch], 4)) {
                mismatch("colour", "vertex %u channel %d differs", i, ch);
                return;
            }
        for (int t = 0; t < 8; t++)
            if ((s.attrs & (1u << (13 + t))) && memcmp(w.uv[t], v.tex[t], 8)) {
                mismatch("texcoord", "vertex %u set %d: %g %g, the GPU's %g %g", i, t, w.uv[t][0], w.uv[t][1],
                         v.tex[t][0], v.tex[t][1]);
                return;
            }
        // the matrices it is drawn with
        const u32 slot = v.pos_mtx;
        if (w.pnmtx != 0xFF && w.pnmtx != slot) {
            mismatch("matrix index", "vertex %u: %u, the GPU's %u", i, w.pnmtx, slot);
            return;
        }
        if (cur >= 0) {
            // a single-matrix shape: its current matrix in slot 0 (and its normal matrix)
            if (slot != 0) {
                mismatch("matrix index", "vertex %u of a single-matrix shape uses slot %u", i, slot);
                return;
            }
            if (i == 0) {
                compare_matrix("position matrix", call.vpos, (u32)cur, 0x30, 0, 12);
                if (nrm && call.vnrm)
                    compare_matrix("normal matrix", call.vnrm, (u32)cur, 0x24, 0x400, 9);
            }
        } else {
            auto it = s_check.slots.find(0u << 16 | slot * 4);
            if (it == s_check.slots.end()) {
                mismatch("matrix", "vertex %u: slot %u was never loaded", i, slot);
                return;
            }
            compare_matrix("position matrix", call.vpos, it->second, 0x30, slot * 4, 12);
            if (nrm && call.vnrm) {
                const u32 na = 0x400 + (slot & 31) * 3;
                auto jt = s_check.slots.find(1u << 16 | na);
                if (jt == s_check.slots.end())
                    mismatch("normal matrix", "vertex %u: normal slot %03X was never loaded", i, na);
                else
                    compare_matrix("normal matrix", call.vnrm, jt->second, 0x24, na, 9);
            }
            for (int t = 0; t < 8; t++)
                if (w.texmtx[t] != 0xFF) {
                    if (w.texmtx[t] != v.tex_mtx[t]) {
                        mismatch("texture matrix index", "vertex %u set %d: %u, the GPU's %u", i, t, w.texmtx[t],
                                 v.tex_mtx[t]);
                        return;
                    }
                    auto kt = s_check.slots.find(2u << 16 | w.texmtx[t] * 4u);
                    if (kt != s_check.slots.end())
                        compare_matrix("texture matrix", call.venv, kt->second, 0x30, w.texmtx[t] * 4u, 12);
                }
        }
    }
}

}  // namespace

namespace {

// the draw (and fur layer) the next primitive belongs to, its shape decoded; false: none left
bool next_shape(Check& c) {
    for (;;) {
        if (c.draw >= c.call.draws.size())
            return false;
        const Draw& d = c.call.draws[c.draw];
        if (!c.have_shape) {
            ArrayOverride arrays[4];
            memcpy(arrays, d.arrays, sizeof(arrays));
            if (d.fur_pos)
                arrays[0] = d.layer_pos(c.layer);
            c.shape = decode_shape(d.shp, arrays);
            c.have_shape = true, c.prim = 0;
            c.slots.clear();
            if (c.shape.error) {
                c.undecoded++;
                mismatch("undecoded", "%s", c.shape.error);
            }
        }
        if (!c.shape.error && c.prim < c.shape.prims.size())
            return true;
        // this layer's (or draw's) primitives are done
        c.have_shape = false;
        if (++c.layer < d.layers())
            continue;
        c.layer = 0, c.draw++;
        if (c.shape.error) {
            // its primitives can't be told from the next draw's: the rest of the call goes unchecked
            c.in_call = false;
            return false;
        }
    }
}

}  // namespace

bool check_marker(u32 id) {
    if ((id & 0xFFFF0000u) != CHECK_MARK)
        return false;
    Check& c = s_check;
    const bool end = id & 1;
    if (!end) {
        if (c.pending.empty() || (c.pending.front().serial & 0x7FFF) != ((id >> 1) & 0x7FFF)) {
            mismatch("order", "a model draw's begin marker %08X with no such call queued", id);
            return true;
        }
        c.call = std::move(c.pending.front());
        c.pending.pop_front();
        c.in_call = true, c.draw = 0, c.prim = 0, c.layer = 0, c.have_shape = false;
        c.regs.clear();
        return true;
    }
    if (c.in_call) {
        if (next_shape(c))
            mismatch("missing", "the game drew none of the rest (%zu draws)", c.call.draws.size() - c.draw);
        c.in_call = false;
    }
    report(false);
    return true;
}

void check_gx_draw(u32 prim, u32 vat, u32 count, const u8* data) {
    Check& c = s_check;
    if (!c.in_call)
        return;
    if (!next_shape(c)) {
        if (c.in_call)
            mismatch("extra", "the game drew a primitive (%02X, %u vertices) the derivation has no draw for", prim, count);
        return;
    }
    const Draw& d = c.call.draws[c.draw];
    const DecodedShape& s = c.shape;
    if (c.prim == 0 && c.layer == 0) {
        c.draws++;
        // the draw's material and matrix registers
        const u32 from = c.draw ? c.call.writes_end[c.draw - 1] : 0, to = c.call.writes_end[c.draw];
        for (u32 i = from; i < to; i++)
            c.regs.apply(c.call.writes[i]);
        if (c.call.derived[c.draw])
            compare_regs(c.regs);
        else
            c.regs.clear();  // (what the game's own setup changed isn't known)
    }
    const ShapePrim& pr = s.prims[c.prim];
    for (u32 k = 0; k < pr.n_loads; k++) {
        const ShapeMtxLoad& l = s.loads[pr.loads_first + k];
        c.slots[(u32)l.array << 16 | l.xf_addr] = l.index;
    }
    c.prims++;
    c.vertices += count;
    if (pr.type != prim || pr.vat != vat || pr.count != count) {
        mismatch("primitive", "the game's %02X vat %u x%u, the derivation's %02X vat %u x%u", prim, vat, count, pr.type,
                 pr.vat, pr.count);
    } else {
        std::vector<::guest::GxVertex> got(count);
        ::guest::gx_decode_vertices(vat, count, data, got.data());
        check_vertices(c.call, d, s, pr, got.data());
    }
    c.prim++;
}

}  // namespace engine::g3d::live

namespace g {

// nw4r::g3d::DrawResMdlDirectly(const ResMdl&, viewPos, viewNrm, viewEnv, byteCodeOpa, byteCodeXlu,
// DrawResMdlReplacement*, drawMode)
void nw4r_g3d_DrawResMdlDirectly(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7, u32 r8, u32 r9, u32 r10) {
    using namespace engine::g3d::live;
    const Mode md = mode();
    if (md == Mode::NATIVE || (md == Mode::AB && !(::guest::vi_game_frames() & 1))) {
        draw_native(r3, r4, r5, r6, r7, r8, r9, r10);
        return;
    }
    if (md == Mode::AB) {
        nw4r_g3d_DrawResMdlDirectly_orig(r3, r4, r5, r6, r7, r8, r9, r10);
        return;
    }
    Check& c = s_check;
    CheckCall call;
    call.serial = ++c.serial;
    call.mdl = load32(r3);
    call.name = model_name(call.mdl);
    call.vpos = r4, call.vnrm = r5, call.venv = r6;
    c.calls++;
    c.models_seen[call.name]++;
    const u32 id = CHECK_MARK | (call.serial & 0x7FFF) << 1;
    // the derivation, queued before anything goes into the FIFO (on a host the decoder may run as
    // soon as a command is written)
    model_draws(r3, r4, r7, r8, r9, r10, call.draws);
    {
        GxRegs regs;
        regs.clear();
        regs.log = &call.writes;
        LightCache lc = light_cache_now();
        MatShared sh = mat_shared_now();
        for (const Draw& d : call.draws) {
            const bool ok = derive_material(d.mat, mat_sources(d.mat, r9), d.ctrl, lc, sh, regs);
            derive_shape(d.shp, r4, r5, sh, regs);
            call.derived.push_back(ok);
            call.writes_end.push_back((u32)call.writes.size());
        }
    }
    c.pending.push_back(std::move(call));
    write_marker(id);
    if (md == Mode::CHECK_NATIVE)
        draw_native(r3, r4, r5, r6, r7, r8, r9, r10);
    else
        nw4r_g3d_DrawResMdlDirectly_orig(r3, r4, r5, r6, r7, r8, r9, r10);
    write_marker(id | 1);
}

}  // namespace g
