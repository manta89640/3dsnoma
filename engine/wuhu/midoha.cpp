// ミドハ's head and dress (midoha.h).
#include "engine/wuhu/midoha.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "engine/g3d/native_draw.h"
#include "engine/ui/g3d.h"
#include "noma_decomp.h"

namespace engine::wuhu {

namespace g3d = engine::ui::g3d;
using engine::math::Mtx34;
using engine::math::Vec3;

namespace {

const char* const LOOK_FILE = "/wuhu/midoha.ng3d";
const char* const OUTFIT_FILE = "/wuhu/midoha_outfit.bin";
const char* const STAGE_COMMAND = "tools/mii/midoha_parts.py DATA";

// ---- GX's enums (the SDK's values)
enum : u32 {
    GX_VA_POS = 9, GX_VA_NRM = 10, GX_POS_MTX_ARRAY = 21, GX_NRM_MTX_ARRAY = 22, GX_TEX_MTX_ARRAY = 23,
    GX_COLOR0A0 = 4, GX_SRC_REG = 0, GX_DF_NONE = 0, GX_AF_NONE = 2,
    GX_TEXCOORD0 = 0, GX_TEXMAP0 = 0, GX_TG_MTX2x4 = 1, GX_TG_POS = 0, GX_TEXMTX0 = 30, GX_PTIDENTITY = 125,
    GX_MTX2x4 = 1, GX_PNMTX0 = 0,
    GX_CC_TEXC = 8, GX_CC_RASC = 10, GX_CC_ZERO = 15, GX_CA_KONST = 6, GX_CA_ZERO = 7,
    GX_TEV_ADD = 0, GX_TB_ZERO = 0, GX_CS_SCALE_1 = 0, GX_TEVPREV = 0, GX_TEV_KASEL_1 = 0,
    GX_TEV_SWAP0 = 0, GX_CH_RED = 0, GX_CH_GREEN = 1, GX_CH_BLUE = 2, GX_CH_ALPHA = 3,
    GX_ALWAYS = 7, GX_AOP_AND = 0, GX_LEQUAL = 3, GX_BM_NONE = 0, GX_BL_ONE = 1, GX_LO_CLEAR = 0,
    GX_CULL_BACK = 2, GX_TF_RGB565 = 4, GX_CLAMP = 0, GX_LINEAR = 1,
    GX_DRAW_TRIANGLES = 0x90,
};
// nw4r::g3d::ScnObj option: no opaque draw (ScnObj::SetScnObjOption, its vtable's +0x20)
const u32 OPTION_DISABLE_DRAW_OPA = 6;

u32 align32(u32 n) { return (n + 31) & ~31u; }

// ---- the body's model (MDL0, big-endian, in guest memory): what nw4r::g3d's ResMdl holds
// (tools/g3d/brres.py reads the same file)
struct Mdl {
    u32 base = 0, n_sections = 0, info = 0;
    u32 section(u32 i) const {
        if (i >= n_sections)
            return 0;
        const s32 o = (s32)guest::load32(base + 0x10 + 4 * i);
        return o ? base + (u32)o : 0;
    }
    // a ResDic's entries' data: entry i (from 1) at +8 + 16 i, its data at +12, relative to the dic
    u32 entries(u32 sec) const { return sec ? guest::load32(sec + 4) : 0; }
    u32 entry(u32 sec, u32 i) const { return sec + (u32)(s32)guest::load32(sec + 8 + 16 * (i + 1) + 12); }
};

bool open_mdl(u32 res_mdl, Mdl* m) {
    m->base = res_mdl;
    if (guest::load32(res_mdl) != 0x4D444C30)  // "MDL0"
        return false;
    const u32 version = guest::load32(res_mdl + 8);
    m->n_sections = version >= 10 ? 14 : 11;
    m->info = res_mdl + 0x10 + 4 * m->n_sections + 4;
    return guest::load32(m->info) == 0x40;
}

f32 load_f(u32 a) { return engine::math::load_f32(a); }

// an array's value (ResVtxPosData / ResVtxNrmData: +0x08 the data, +0x14 components (pos: 0 xy,
// 1 xyz), +0x18 type (0 u8, 1 s8, 2 u16, 3 s16, 4 f32), +0x1C fraction bits, +0x1D stride)
Vec3 array_vec3(u32 arr, u32 index) {
    const u32 data = arr + (u32)(s32)guest::load32(arr + 0x08), type = guest::load32(arr + 0x18);
    const u32 n = guest::load32(arr + 0x14) ? 3 : 2, frac = guest::load8(arr + 0x1C), stride = guest::load8(arr + 0x1D);
    const u32 at = data + index * stride;
    f32 v[3] = {0, 0, 0};
    const f32 scale = 1.0f / (f32)(1u << frac);
    for (u32 c = 0; c < n; c++) {
        switch (type) {
        case 0: v[c] = guest::load8(at + c) * scale; break;
        case 1: v[c] = (s8)guest::load8(at + c) * scale; break;
        case 2: v[c] = guest::load16(at + 2 * c) * scale; break;
        case 3: v[c] = (s16)guest::load16(at + 2 * c) * scale; break;
        default: v[c] = load_f(at + 4 * c); break;
        }
    }
    return {v[0], v[1], v[2]};
}

// the array of `sec` whose index (+0x10) is `id`
u32 find_array(const Mdl& m, u32 sec, s32 id) {
    const u32 s = m.section(sec);
    for (u32 i = 0; i < m.entries(s); i++) {
        const u32 a = m.entry(s, i);
        if ((s32)guest::load32(a + 0x10) == id)
            return a;
    }
    return 0;
}

// a vertex attribute's bytes in a display list, from the vertex descriptor (CP 0x50 low, 0x60
// high): matrix indices one byte; indexed attributes one or two; direct ones are not expected here
// (0: unknown)
struct VertexLayout {
    u32 size = 0;              // bytes a vertex
    s32 pnmtx = -1, pos = -1;  // their offsets in it (-1: absent)
    u32 pos_bytes = 0;
    bool ok = false;
};
VertexLayout layout_of(u32 lo, u32 hi) {
    VertexLayout l;
    u32 at = 0;
    if (lo & 1)
        l.pnmtx = (s32)at, at++;
    for (u32 t = 0; t < 8; t++)
        if (lo >> (1 + t) & 1)
            at++;
    l.ok = true;
    auto attr = [&](u32 kind, bool is_pos) {
        if (kind == 1)
            l.ok = false;  // direct
        if (kind >= 2) {
            if (is_pos)
                l.pos = (s32)at, l.pos_bytes = kind == 3 ? 2 : 1;
            at += kind == 3 ? 2 : 1;
        }
    };
    attr(lo >> 9 & 3, true);   // position
    attr(lo >> 11 & 3, false);  // normal
    attr(lo >> 13 & 3, false);  // colour 0
    attr(lo >> 15 & 3, false);  // colour 1
    for (u32 t = 0; t < 8; t++)
        attr(hi >> (2 * t) & 3, false);
    l.size = at;
    return l;
}

void put16(std::vector<u8>& v, u32 x) { v.push_back((u8)(x >> 8)), v.push_back((u8)x); }

}  // namespace

const midoha::Candidate* midoha_candidate() {
    const char* e = std::getenv("NOMA_WUHU_MIDOHA");
    if (!e || !*e || !std::strcmp(e, "0"))
        return nullptr;
    // (1: the recommended one, the last)
    const midoha::Candidate& last = midoha::CANDIDATES[midoha::N_CANDIDATES - 1];
    const char want = !std::strcmp(e, "1") ? last.letter : (char)(e[0] & ~0x20);
    for (const midoha::Candidate& c : midoha::CANDIDATES)
        if (c.letter == want)
            return &c;
    guest::log("midoha: NOMA_WUHU_MIDOHA=%s: no such candidate (A to %c): %c", e, last.letter, last.letter);
    return &last;
}

// ---------------------------------------------------------------- the dress

MidohaOutfit::~MidohaOutfit() {
    if (block_)
        g::operator_delete(block_);
}

// The outfit file (tools/mii/midoha_outfit.py): "MDOF", u16 width, u16 height (little-endian), the
// projection (f32 x at u = 0 and 1, y at v = 0 and 1, Mii units, bind pose), then the front's and
// the back's texels, GX RGB565 (big-endian, 4x4 tiles), each width x height. The back's are seen
// from the front, through the body (u from the same x).
bool MidohaOutfit::make(u32 scn_mdl) {
    std::string path = guest::data_dir() + OUTFIT_FILE;
    std::vector<u8> file;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        file.resize(n > 0 ? (size_t)n : 0);
        if (!file.empty() && std::fread(file.data(), 1, file.size(), f) != file.size())
            file.clear();
        std::fclose(f);
    }
    u16 w = 0, h = 0;
    f32 proj[4] = {};
    if (file.size() >= 24 && !std::memcmp(file.data(), "MDOF", 4)) {
        std::memcpy(&w, &file[4], 2), std::memcpy(&h, &file[6], 2), std::memcpy(proj, &file[8], 16);
    }
    const u32 tex_bytes = (u32)w * h * 2;
    if (!w || !h || (w & 3) || (h & 3) || file.size() < 24 + 2 * (size_t)tex_bytes) {
        guest::log("midoha: no outfit (%s missing or damaged: make it with %s): his body as a Mii's", path.c_str(),
                   STAGE_COMMAND);
        return false;
    }
    Mdl m;
    if (!scn_mdl || !open_mdl(guest::load32(scn_mdl + 0xE8), &m)) {
        guest::log("midoha: his body's model isn't one this reads: no outfit");
        return false;
    }
    // the draw matrices: matrix id -> node (-1: an envelope, its vertices in model space), the
    // nodes' bind matrices (ResNodeData +0x10 matrix id, +0x70 the bind world matrix)
    const u32 mtx_table = m.info + (u32)(s32)guest::load32(m.info + 0x24);
    const u32 n_mtx = guest::load32(mtx_table);
    std::vector<Mtx34> bind(n_mtx, engine::math::identity34());
    {
        const u32 s = m.section(1);
        for (u32 i = 0; i < m.entries(s); i++) {
            const u32 nd = m.entry(s, i), id = guest::load32(nd + 0x10);
            if (id < n_mtx && (s32)guest::load32(mtx_table + 4 + 4 * id) >= 0)
                bind[id] = engine::math::load<Mtx34>(nd + 0x70);
        }
    }
    // each shape's lists, split by the bind pose's facing (GX's front faces wind clockwise: the
    // normal is -(b - a) x (c - a))
    struct Built {
        List list;
        std::vector<u8> dl[2];
    };
    std::vector<Built> built;
    const u32 shapes = m.section(10);
    u32 tris[2] = {0, 0};
    for (u32 i = 0; i < m.entries(shapes) && built.size() < 4; i++) {
        const u32 s = m.entry(shapes, i);
        Built b;
        b.list.mtx_id = (s32)guest::load32(s + 0x08);
        b.list.setup = s + 0x18 + (u32)(s32)guest::load32(s + 0x20);
        b.list.setup_size = guest::load32(s + 0x1C);
        const u32 prim = s + 0x24 + (u32)(s32)guest::load32(s + 0x2C), prim_size = guest::load32(s + 0x28);
        const u32 pos_arr = find_array(m, 2, (s16)guest::load16(s + 0x48));
        const u32 nrm_arr = find_array(m, 3, (s16)guest::load16(s + 0x4A));
        if (!pos_arr)
            continue;
        b.list.pos = pos_arr + (u32)(s32)guest::load32(pos_arr + 0x08);
        b.list.pos_stride = guest::load8(pos_arr + 0x1D);
        if (nrm_arr) {
            b.list.nrm = nrm_arr + (u32)(s32)guest::load32(nrm_arr + 0x08);
            b.list.nrm_stride = guest::load8(nrm_arr + 0x1D);
        }
        // its vertex descriptor, from its setup list (CP 0x50, 0x60)
        u32 vcd_lo = 0, vcd_hi = 0;
        for (u32 o = 0; o + 6 <= b.list.setup_size;) {
            const u8 c = guest::load8(b.list.setup + o);
            if (c == 0x08) {
                const u8 reg = guest::load8(b.list.setup + o + 1);
                const u32 v = guest::load32(b.list.setup + o + 2);
                if (reg == 0x50)
                    vcd_lo = v;
                else if (reg == 0x60)
                    vcd_hi = v;
                o += 6;
            } else if (c == 0x10) {
                o += 5 + 4 * (guest::load16(b.list.setup + o + 1) + 1);
            } else if (c == 0x61) {
                o += 5;
            } else {
                o += 1;
            }
        }
        const VertexLayout lay = layout_of(vcd_lo, vcd_hi);
        if (!lay.ok || lay.pos < 0) {
            guest::log("midoha: a body shape's vertices aren't indexed: no outfit");
            return false;
        }
        u32 slot_mtx[10];
        for (u32& x : slot_mtx)
            x = b.list.mtx_id >= 0 ? (u32)b.list.mtx_id : 0;
        auto corner = [&](u32 v) -> Vec3 {
            const u32 slot = lay.pnmtx >= 0 ? guest::load8(v + (u32)lay.pnmtx) / 3 : 0;
            const u32 id = slot < 10 ? slot_mtx[slot] : 0;
            const u32 index = lay.pos_bytes == 2 ? guest::load16(v + (u32)lay.pos) : guest::load8(v + (u32)lay.pos);
            const Vec3 p = array_vec3(pos_arr, index);
            return id < n_mtx ? engine::math::transform(bind[id], p) : p;
        };
        for (u32 o = 0; o < prim_size;) {
            const u32 at = prim + o;
            const u8 c = guest::load8(at);
            if (c == 0x00 || c == 0x48) {
                o += 1;
            } else if (c == 0x20 || c == 0x28 || c == 0x30 || c == 0x38) {
                // an indexed matrix load: both lists replay it, so that each draws under the same
                // matrices (a position matrix's XF address / 12 is its slot)
                if (c == 0x20) {
                    const u32 idx = guest::load16(at + 1), addr = guest::load16(at + 3) & 0xFFF;
                    if (addr / 12 < 10)
                        slot_mtx[addr / 12] = idx;
                }
                for (auto& d : b.dl)
                    for (u32 k = 0; k < 5; k++)
                        d.push_back(guest::load8(at + k));
                o += 5;
            } else if (c == 0x08 || c == 0x10 || c == 0x61) {
                const u32 n = c == 0x08 ? 6 : c == 0x61 ? 5 : 5 + 4 * (guest::load16(at + 1) + 1);
                for (auto& d : b.dl)
                    for (u32 k = 0; k < n; k++)
                        d.push_back(guest::load8(at + k));
                o += n;
            } else if (c & 0x80) {
                const u32 op = c & 0xF8, vat = c & 7, count = guest::load16(at + 1);
                const u32 verts = at + 3;
                std::vector<u32> tri;  // corner vertex numbers, three a triangle
                if (op == 0x90) {
                    for (u32 k = 0; k + 2 < count; k += 3)
                        tri.insert(tri.end(), {k, k + 1, k + 2});
                } else if (op == 0x98) {
                    for (u32 k = 0; k + 2 < count; k++)
                        if (k & 1)
                            tri.insert(tri.end(), {k + 1, k, k + 2});
                        else
                            tri.insert(tri.end(), {k, k + 1, k + 2});
                } else if (op == 0xA0) {
                    for (u32 k = 1; k + 1 < count; k++)
                        tri.insert(tri.end(), {0, k, k + 1});
                } else if (op == 0x80) {
                    for (u32 k = 0; k + 3 < count; k += 4)
                        tri.insert(tri.end(), {k, k + 1, k + 2, k, k + 2, k + 3});
                }
                std::vector<u32> side_tris[2];
                for (u32 t = 0; t < tri.size(); t += 3) {
                    const Vec3 a = corner(verts + tri[t] * lay.size), bb = corner(verts + tri[t + 1] * lay.size);
                    const Vec3 cc = corner(verts + tri[t + 2] * lay.size);
                    const Vec3 n = engine::math::cross(bb - a, cc - a);
                    const int side = -n.z >= 0 ? 0 : 1;
                    side_tris[side].insert(side_tris[side].end(), {tri[t], tri[t + 1], tri[t + 2]});
                }
                for (int sd = 0; sd < 2; sd++) {
                    const std::vector<u32>& st = side_tris[sd];
                    for (u32 from = 0; from < st.size(); from += 0xFFF0) {
                        const u32 n = (u32)std::min<size_t>(0xFFF0, st.size() - from);
                        std::vector<u8>& d = b.dl[sd];
                        d.push_back((u8)(GX_DRAW_TRIANGLES | vat));
                        put16(d, n);
                        for (u32 k = 0; k < n; k++)
                            for (u32 x = 0; x < lay.size; x++)
                                d.push_back(guest::load8(verts + st[from + k] * lay.size + x));
                    }
                    tris[sd] += (u32)st.size() / 3;
                }
                o += 3 + count * lay.size;
            } else {
                guest::log("midoha: a body shape's display list has GX command %02X: no outfit", c);
                return false;
            }
        }
        built.push_back(std::move(b));
    }
    if (built.empty()) {
        guest::log("midoha: his body has no shapes this reads: no outfit");
        return false;
    }
    // the block: the two textures, their texture objects, the texture matrices (one per draw matrix:
    // the projection times its bind matrix), the lists
    u32 size = 0;
    auto take = [&](u32 n) {
        const u32 a = size;
        size += align32(n);
        return a;
    };
    const u32 o_tex0 = take(tex_bytes), o_tex1 = take(tex_bytes), o_obj = take(0x40), o_mtx = take(n_mtx * 0x30);
    std::vector<u32> o_dl;
    for (Built& b : built)
        for (auto& d : b.dl) {
            while (d.size() & 31)
                d.push_back(0);  // (GX's lists are whole 32-byte bursts)
            o_dl.push_back(take((u32)d.size()));
        }
    block_ = g3d::new_aligned(size);
    if (!block_) {
        guest::log("midoha: out of memory for his outfit (%u bytes)", (unsigned)size);
        return false;
    }
    const u32 tex[2] = {block_ + o_tex0, block_ + o_tex1};
    tex_obj_ = block_ + o_obj, tex_mtx_ = block_ + o_mtx;
    guest::write_g(tex[0], &file[24], tex_bytes);
    guest::write_g(tex[1], &file[24 + tex_bytes], tex_bytes);
    for (u32 i = 0; i < 2; i++) {
        g::GXInitTexObj(tex_obj_ + 0x20 * i, tex[i], w, h, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, 0);
        g::GXInitTexObjLOD(tex_obj_ + 0x20 * i, GX_LINEAR, GX_LINEAR, 0, 0, 0, 0.0, 0.0, 0.0);
    }
    // u = (x - x0) / (x1 - x0), v = (y - y0) / (y1 - y0): the projection's rows, then times each
    // draw matrix's bind matrix (its vertices' positions are in its node's space)
    const f32 su = 1.0f / (proj[1] - proj[0]), sv = 1.0f / (proj[3] - proj[2]);
    const Mtx34 p = {{{su, 0, 0, -proj[0] * su}, {0, sv, 0, -proj[2] * sv}, {0, 0, 0, 1}}};
    for (u32 i = 0; i < n_mtx; i++)
        engine::math::store(tex_mtx_ + 0x30 * i, engine::math::concat(p, bind[i]));
    u32 k = 0;
    n_lists_ = 0;
    for (Built& b : built) {
        List l = b.list;
        for (int sd = 0; sd < 2; sd++) {
            l.dl[sd] = block_ + o_dl[k++];
            l.size[sd] = (u32)b.dl[sd].size();
            guest::write_g(l.dl[sd], b.dl[sd].data(), l.size[sd]);
        }
        lists_[n_lists_++] = l;
    }
    // (the GPU reads the lists and texels from memory: flush them out of the CPU's cache)
    g::DCFlushRange(block_, size);
    // the body's own draw off (it still poses, and its matrices are still made for each frame)
    const u32 vt = guest::load32(scn_mdl);
    gcall<u32>(guest::load32(vt + 0x20), scn_mdl, OPTION_DISABLE_DRAW_OPA, 1u);
    guest::log("midoha: outfit %ux%u on %u body shapes: %u triangles facing front, %u back; %u draw matrices; %u "
               "bytes",
               (unsigned)w, (unsigned)h, (unsigned)n_lists_, (unsigned)tris[0], (unsigned)tris[1], (unsigned)n_mtx,
               (unsigned)size);
    return true;
}

void MidohaOutfit::draw(u32 scn_mdl) const {
    if (!block_ || !scn_mdl)
        return;
    // this frame's view matrices (ScnMdlSimple: +0xF4 positions, +0xF8 normals, two sets each, +0x101
    // the current, +0x102 how many), by draw matrix
    const u32 n = guest::load16(scn_mdl + 0x102), cur = guest::load8(scn_mdl + 0x101);
    const u32 view_pos = guest::load32(scn_mdl + 0xF4) + cur * align32(n * 0x30);
    const u32 view_nrm = guest::load32(scn_mdl + 0xF8) ? guest::load32(scn_mdl + 0xF8) + cur * align32(n * 0x24) : 0;
    // the Mii light's tint (the actor manager's Mii model resource +0x34), as the body's materials
    // take it: the texture times it, unlit
    const u32 model_res = guest::load32(GA_mii_ModelRes_sInstance);
    {
        GFRAME(0x10);
        const u32 c = (u32)&GS(u32, 0x8);
        GS(u32, 0x8) = model_res ? guest::load32(model_res + 0x34) : 0xFFFFFFFF;
        g::GXSetChanMatColor(GX_COLOR0A0, c);
    }
    g::GXSetNumChans(1);
    g::GXSetChanCtrl(GX_COLOR0A0, 0, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
    g::GXSetNumTexGens(1);
    g::GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_POS, GX_TEXMTX0, 0, GX_PTIDENTITY);
    g::GXSetNumTevStages(1);
    g::GXSetNumIndStages(0);
    g::GXSetTevDirect(0);
    g::GXSetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
    g::GXSetTevSwapMode(0, GX_TEV_SWAP0, GX_TEV_SWAP0);
    g::GXSetTevOrder(0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    g::GXSetTevKAlphaSel(0, GX_TEV_KASEL_1);
    g::GXSetTevColorIn(0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
    g::GXSetTevAlphaIn(0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    g::GXSetTevColorOp(0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, 1, GX_TEVPREV);
    g::GXSetTevAlphaOp(0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, 1, GX_TEVPREV);
    g::GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    g::GXSetZCompLoc(1);
    g::GXSetZMode(1, GX_LEQUAL, 1);
    g::GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, 0, GX_LO_CLEAR);
    g::GXSetCullMode(GX_CULL_BACK);
    g::GXSetColorUpdate(1);
    // the matrices the lists load by index: the body's view matrices, our texture matrices
    g::GXSetArray(GX_POS_MTX_ARRAY, view_pos, 0x30);
    if (view_nrm)
        g::GXSetArray(GX_NRM_MTX_ARRAY, view_nrm, 0x24);
    g::GXSetArray(GX_TEX_MTX_ARRAY, tex_mtx_, 0x30);
    for (u32 i = 0; i < n_lists_; i++) {
        const List& l = lists_[i];
        g::GXSetArray(GX_VA_POS, l.pos, l.pos_stride);
        if (l.nrm)
            g::GXSetArray(GX_VA_NRM, l.nrm, l.nrm_stride);
        g::GXInvalidateVtxCache();
        if (l.mtx_id >= 0 && (u32)l.mtx_id < n) {
            // one matrix for the whole shape: loaded here, as nw4r::g3d does for such a shape
            // (its normals aren't loaded: the dress is unlit)
            g::GXLoadPosMtxImm(view_pos + 0x30 * (u32)l.mtx_id, GX_PNMTX0);
            g::GXLoadTexMtxImm(tex_mtx_ + 0x30 * (u32)l.mtx_id, GX_TEXMTX0, GX_MTX2x4);
            g::GXSetCurrentMtx(GX_PNMTX0);
        }
        g::GXCallDisplayList(l.setup, l.setup_size);
        for (int sd = 0; sd < 2; sd++) {
            if (!l.size[sd])
                continue;
            g::GXLoadTexObj(tex_obj_ + 0x20 * sd, GX_TEXMAP0);
            g::GXCallDisplayList(l.dl[sd], l.size[sd]);
        }
    }
}

// ---------------------------------------------------------------- the head

MidohaLook::~MidohaLook() {
    if (prepared_)
        engine::g3d::native_draw_release(*package_);
    world_.reset();
    package_.reset();
}

bool MidohaLook::load() {
    const char* e = std::getenv("NOMA_MIDOHA_PACKAGE");
    const std::string path = e && *e ? std::string(e) : guest::data_dir() + LOOK_FILE;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        guest::log("midoha: there is no %s (make it: %s): his head as a Mii's", path.c_str(), STAGE_COMMAND);
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    void* data = n > 0 ? std::malloc((size_t)n) : nullptr;
    const bool read = data && std::fread(data, 1, (size_t)n, f) == (size_t)n;
    std::fclose(f);
    if (!read) {
        std::free(data);
        guest::log("midoha: %s can't be read", path.c_str());
        return false;
    }
    std::string error;
    engine::g3d::Package* p = engine::g3d::Package::load(data, (u32)n, &error);  // (takes the block)
    if (!p || p->instances().count == 0) {
        guest::log("midoha: %s: %s", path.c_str(), p ? "not his head (no instance)" : error.c_str());
        delete p;
        return false;
    }
    package_.reset(p);
    world_ = std::make_unique<engine::g3d::World>(*package_);
    for (const engine::g3d::Material& m : world_->materials())
        translucent_ |= m.translucent;
    engine::g3d::native_draw_prepare(*package_);
    prepared_ = true;
    u32 tris = 0;
    for (const engine::g3d::fmt::Batch& b : package_->batches())
        tris += b.n_indices / 3;
    guest::log("midoha: head pieces from %s: %u bytes, %u models, %u triangles, %u textures", path.c_str(),
               (unsigned)n, (unsigned)package_->models().count, (unsigned)tris, (unsigned)package_->textures().count);
    return true;
}

void MidohaLook::update(const Mtx34& head, const engine::g3d::Camera& camera, f32 frame) {
    if (!world_)
        return;
    for (u32 i = 0; i < package_->instances().count; i++)
        world_->set_instance_matrix(i, head);
    world_->update(frame);
    world_->set_camera(camera);
}

// (a pass with nothing in it isn't sent: the packet ring is shared with the island's two a frame)
void MidohaLook::emit(engine::g3d::Pass pass) {
    if (world_ && (pass == engine::g3d::Pass::OPAQUE || translucent_))
        world_->emit(pass);
}

// ---------------------------------------------------------------- the face

namespace {

const char* const FACE_FILE = "/wuhu/midoha_face.bin";
const u32 GX_TF_RGB5A3 = 5;

// an expression's face texture in his char model (0: none), its size and format (GXTexObj: +0x8
// width - 1, height - 1, format; +0xC the image's address / 32)
struct FaceTexture {
    u32 image = 0, width = 0, height = 0, format = 0;
};
FaceTexture face_texture(u32 actor, u32 expression) {
    FaceTexture t;
    const u32 obj = guest::load32(engine::ui::g3d::actor_face(actor) + 0x6C + 4 * expression);
    if (!obj)
        return t;
    const u32 image0 = guest::load32(obj + 0x8), image3 = guest::load32(obj + 0xC);
    t.width = (image0 & 0x3FF) + 1, t.height = (image0 >> 10 & 0x3FF) + 1, t.format = image0 >> 20 & 0xF;
    t.image = 0x80000000u | (image3 & 0xFFFFFF) << 5;
    return t;
}

}  // namespace

// The file (tools/mii/midoha_face.py): "MDFC", u16 width, u16 height, u8 images, u8 image of each of
// the 7 expressions, then the images, GX RGB5A3 (4x4 tiles, big-endian), width x height each.
bool MidohaFace::load() {
    const char* e = std::getenv("NOMA_MIDOHA_FACE");
    if (e && !std::strcmp(e, "0")) {
        guest::log("midoha: face: RFL's own (NOMA_MIDOHA_FACE=0)");
        return false;
    }
    const std::string path = e && *e ? std::string(e) : guest::data_dir() + FACE_FILE;
    std::vector<u8> file;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        file.resize(n > 0 ? (size_t)n : 0);
        if (!file.empty() && std::fread(file.data(), 1, file.size(), f) != file.size())
            file.clear();
        std::fclose(f);
    }
    const u32 head = 4 + 2 + 2 + 1 + N_EXPRESSIONS;
    u16 w = 0, h = 0;
    u8 n_images = 0;
    if (file.size() >= head && !std::memcmp(file.data(), "MDFC", 4))
        std::memcpy(&w, &file[4], 2), std::memcpy(&h, &file[6], 2), n_images = file[8];
    const u32 bytes = (u32)w * h * 2;
    bool ok = w && h && !(w & 3) && !(h & 3) && n_images && file.size() >= head + (size_t)n_images * bytes;
    for (u32 i = 0; ok && i < N_EXPRESSIONS; i++)
        ok = file[9 + i] < n_images;
    if (!ok) {
        guest::log("midoha: face: %s missing or damaged (make it: tools/mii/midoha_face.py DATA): his Wii parts",
                   path.c_str());
        return false;
    }
    width_ = w, height_ = h, image_bytes_ = bytes;
    std::memcpy(image_of_, &file[9], N_EXPRESSIONS);
    images_.assign(file.begin() + head, file.begin() + head + (size_t)n_images * bytes);
    guest::log("midoha: face: %ux%u, %u images from %s", (unsigned)w, (unsigned)h, (unsigned)n_images, path.c_str());
    return true;
}

void MidohaFace::apply(u32 actor) {
    if (images_.empty() || !actor)
        return;
    for (u32 x = 0; x < N_EXPRESSIONS; x++) {
        const FaceTexture t = face_texture(actor, x);
        if (!t.image)
            continue;
        if (t.width != width_ || t.height != height_ || t.format != GX_TF_RGB5A3) {
            if (!warned_)
                guest::log("midoha: face: RFL's face texture is %ux%u (format %u), the file's %ux%u: his Wii parts",
                           (unsigned)t.width, (unsigned)t.height, (unsigned)t.format, (unsigned)width_,
                           (unsigned)height_);
            warned_ = true;
            return;
        }
        // ours still there? (a word every 4 KB: an EFB copy writes the whole image)
        const u8* ours = &images_[(size_t)image_of_[x] * image_bytes_];
        bool same = true;
        for (u32 at = 0; same && at < image_bytes_; at += 4096) {
            u32 word;
            std::memcpy(&word, ours + at, 4);
            same = guest::load32(t.image + at) == __builtin_bswap32(word);
        }
        if (same)
            continue;
        guest::write_g(t.image, ours, image_bytes_);
        if (writes_++ < 14)
            guest::log("midoha: face: expression %u written (image %u at %08X)", (unsigned)x, (unsigned)image_of_[x],
                       (unsigned)t.image);
    }
}

bool MidohaFace::dump(u32 actor, const std::string& dir) {
    u32 n = 0;
    for (u32 x = 0; x < N_EXPRESSIONS; x++) {
        const FaceTexture t = face_texture(actor, x);
        if (!t.image || t.width > 1024 || t.height > 1024)
            continue;
        const u32 bytes = t.width * t.height * 2;  // (RGB5A3)
        std::vector<u8> out(10 + bytes);
        std::memcpy(&out[0], "MDFD", 4);
        const u16 w = (u16)t.width, h = (u16)t.height, f = (u16)t.format;
        std::memcpy(&out[4], &w, 2), std::memcpy(&out[6], &h, 2), std::memcpy(&out[8], &f, 2);
        guest::read_g(&out[10], t.image, bytes);
        char path[64];
        std::snprintf(path, sizeof(path), "/midoha_face_rfl_%u.bin", (unsigned)x);
        if (FILE* fp = std::fopen((dir + path).c_str(), "wb")) {
            std::fwrite(out.data(), 1, out.size(), fp);
            std::fclose(fp);
            n++;
        }
    }
    guest::log("midoha: face: RFL's %u face textures written to %s", (unsigned)n, dir.c_str());
    return n > 0;
}

}  // namespace engine::wuhu
