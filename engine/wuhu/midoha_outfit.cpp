// ミドハ's hanbok (midoha_outfit.h).
#include "engine/wuhu/midoha_outfit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "engine/ui/g3d.h"
#include "noma_decomp.h"

namespace engine::wuhu {

namespace g3d = engine::ui::g3d;
using engine::math::Mtx34;
using engine::math::Vec3;

namespace {

const char* const OUTFIT_FILE = "/wuhu/midoha_outfit.bin";
const char* const STAGE_COMMAND = "tools/mii/midoha_parts.py DATA";
const u32 SLOTS = 7;
const u32 VTXFMT = 6;
const u32 LOG_EVERY = 600;
// the sheen at this share of the body's (the painted shading carries the rest)
const u8 SHEEN = 153;
// the body's parts drawn: the hands (his skin, RFL's skin tone 1) and the shoes
const char* const HAND_NODES[] = {"Wrist_L", "Wrist_R"};
const char* const SHOE_NODES[] = {"Ankle_L", "Ankle_R"};
const u32 HAND_RGBA = 0xFFBC80FF, SHOE_RGBA = 0x261C29FF;
// the screen's lines the level is chosen for (the 3DS draws the 456 lines of the frame on 240)
#ifdef __3DS__
const f32 SCREEN_LINES = 240.0f;
#else
const f32 SCREEN_LINES = 456.0f;
#endif

// ---- GX's enums (the SDK's values)
enum : u32 {
    GX_VA_PNMTXIDX = 0, GX_VA_TEX0MTXIDX = 1, GX_VA_POS = 9, GX_VA_NRM = 10, GX_VA_TEX0 = 13,
    GX_NONE = 0, GX_DIRECT = 1, GX_INDEX16 = 3,
    GX_POS_XYZ = 1, GX_NRM_XYZ = 0, GX_TEX_ST = 1, GX_F32 = 4,
    GX_POS_MTX_ARRAY = 21, GX_NRM_MTX_ARRAY = 22, GX_TEX_MTX_ARRAY = 23,
    GX_COLOR0A0 = 4, GX_COLOR_NULL = 0xFF, GX_SRC_REG = 0, GX_DF_NONE = 0, GX_AF_NONE = 2,
    GX_TEXCOORD0 = 0, GX_TEXCOORD1 = 1, GX_TEXMAP0 = 0, GX_TEXMAP1 = 1,
    GX_TG_MTX3x4 = 0, GX_TG_MTX2x4 = 1, GX_TG_NRM = 1, GX_TG_TEX0 = 4,
    GX_TEXMTX0 = 30, GX_IDENTITY = 60, GX_PTTEXMTX0 = 64, GX_PTIDENTITY = 125, GX_MTX3x4 = 0,
    GX_CC_CPREV = 0, GX_CC_TEXC = 8, GX_CC_RASC = 10, GX_CC_KONST = 14, GX_CC_ZERO = 15,
    GX_CA_APREV = 0, GX_CA_KONST = 6, GX_CA_ZERO = 7,
    GX_TEV_ADD = 0, GX_TB_ZERO = 0, GX_CS_SCALE_1 = 0, GX_TEVPREV = 0,
    GX_TEV_KCSEL_K0 = 12, GX_TEV_KCSEL_K1 = 13, GX_TEV_KASEL_1 = 0, GX_KCOLOR0 = 0, GX_KCOLOR1 = 1,
    GX_TEV_SWAP0 = 0, GX_CH_RED = 0, GX_CH_GREEN = 1, GX_CH_BLUE = 2, GX_CH_ALPHA = 3,
    GX_ALWAYS = 7, GX_AOP_AND = 0, GX_LEQUAL = 3, GX_BM_NONE = 0, GX_BL_ONE = 1, GX_LO_CLEAR = 0,
    GX_CULL_BACK = 2, GX_TF_RGB565 = 4, GX_CLAMP = 0, GX_LINEAR = 1, GX_PNMTX0 = 0,
    GX_DRAW_TRIANGLES = 0x90,
};
// nw4r::g3d::ScnObj option: no opaque draw (ScnObj::SetScnObjOption, its vtable's +0x20)
const u32 OPTION_DISABLE_DRAW_OPA = 6;
// the env camera's projection (nw4r: s = nx / 2 + 1/2, t = -ny / 2 + 1/2), the post matrix
const Mtx34 ENV_PROJECTION = {{{0.5f, 0, 0, 0.5f}, {0, -0.5f, 0, 0.5f}, {0, 0, 1, 0}}};

u32 align32(u32 n) { return (n + 31) & ~31u; }
u32 rd32(const u8* p) {
    u32 v;
    std::memcpy(&v, p, 4);
    return v;
}
u16 rd16(const u8* p) {
    u16 v;
    std::memcpy(&v, p, 2);
    return v;
}
f32 rdf(const u8* p) {
    f32 v;
    std::memcpy(&v, p, 4);
    return v;
}

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
    if (!res_mdl || guest::load32(res_mdl) != 0x4D444C30)  // "MDL0"
        return false;
    const u32 version = guest::load32(res_mdl + 8);
    m->n_sections = version >= 10 ? 14 : 11;
    m->info = res_mdl + 0x10 + 4 * m->n_sections + 4;
    return guest::load32(m->info) == 0x40;
}

// a node by name (ResNodeData: +0x08 its name's offset from it, +0x10 its matrix id): its matrix
// id, -1 if there is none
s32 node_mtx(const Mdl& m, const char* name) {
    const u32 s = m.section(1);
    for (u32 i = 0; i < m.entries(s); i++) {
        const u32 nd = m.entry(s, i);
        if (guest::read_string(nd + (u32)(s32)guest::load32(nd + 0x08)) == name)
            return (s32)guest::load32(nd + 0x10);
    }
    return -1;
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

// a vertex's bytes in a display list, from the vertex descriptor (CP 0x50 low, 0x60 high): matrix
// indices one byte; indexed attributes one or two; direct ones are not expected here
struct VertexLayout {
    u32 size = 0;
    s32 pnmtx = -1;
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
    auto attr = [&](u32 kind) {
        if (kind == 1)
            l.ok = false;  // direct
        if (kind >= 2)
            at += kind == 3 ? 2 : 1;
    };
    for (u32 k = 0; k < 4; k++)
        attr(lo >> (9 + 2 * k) & 3);  // position, normal, colours
    for (u32 t = 0; t < 8; t++)
        attr(hi >> (2 * t) & 3);
    l.size = at;
    return l;
}

void put16(std::vector<u8>& v, u32 x) { v.push_back((u8)(x >> 8)), v.push_back((u8)x); }

// a set's matrices from its bones' (view x inverse bind): the position matrix, its normal matrix
// (the 3x3 part's inverse transpose) and the sheen's texture matrix (the same, as 3x4)
void set_matrices(const Mtx34* skin, const u8 bone[4], const f32 w[4], Mtx34* pos, Mtx34* nrm) {
    Mtx34 m = {};
    for (u32 k = 0; k < 4 && bone[k] != 0xFF; k++) {
        const Mtx34& s = skin[bone[k]];
        const f32 x = w[k];
        for (u32 i = 0; i < 3; i++)
            for (u32 j = 0; j < 4; j++)
                m.m[i][j] += x * s.m[i][j];
    }
    *pos = m;
    if (!engine::math::inverse_transpose(m, *nrm))
        *nrm = engine::math::identity34();
}

}  // namespace

MidohaOutfit::~MidohaOutfit() {
    if (block_)
        g::operator_delete(block_);
}

bool MidohaOutfit::make(u32 scn_mdl) {
    const char* e = std::getenv("NOMA_MIDOHA_OUTFIT");
    const std::string path = e && *e ? std::string(e) : guest::data_dir() + OUTFIT_FILE;
    std::vector<u8> f;
    if (FILE* fp = std::fopen(path.c_str(), "rb")) {
        std::fseek(fp, 0, SEEK_END);
        const long n = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        f.resize(n > 0 ? (size_t)n : 0);
        if (!f.empty() && std::fread(f.data(), 1, f.size(), fp) != f.size())
            f.clear();
        std::fclose(fp);
    }
    if (f.size() < 0x60 || std::memcmp(f.data(), "MDG1", 4) || rd32(&f[4]) != 1) {
        guest::log("midoha: no outfit (%s missing or not a garment: make it with %s): his body as a Mii's", path.c_str(),
                   STAGE_COMMAND);
        return false;
    }
    const u32 n_bones = rd32(&f[0x08]), n_sets = rd32(&f[0x0C]), n_verts = rd32(&f[0x10]), n_batches = rd32(&f[0x14]);
    tex_size_ = rd16(&f[0x18]);
    n_levels_ = rd32(&f[0x1C]);
    density_ = rdf(&f[0x20]);
    const u32 o_bones = rd32(&f[0x24]), o_sets = rd32(&f[0x28]), o_arrays = rd32(&f[0x2C]), o_batches = rd32(&f[0x30]);
    const u32 o_loads = rd32(&f[0x34]), o_dl = rd32(&f[0x38]), dl_size = rd32(&f[0x3C]), o_tex = rd32(&f[0x40]);
    const u32 tex_bytes = rd32(&f[0x44]), n_loads = rd32(&f[0x48]);
    const u32 array_bytes = align32(n_verts * 12) * 2 + align32(n_verts * 8);
    if (o_tex + tex_bytes > f.size() || o_dl + dl_size > f.size() || o_arrays + array_bytes > f.size() || !n_levels_ ||
        n_levels_ > 8 || (tex_size_ >> (n_levels_ - 1)) < 4) {
        guest::log("midoha: %s is damaged: no outfit", path.c_str());
        return false;
    }
    Mdl m;
    if (!scn_mdl || !open_mdl(guest::load32(scn_mdl + 0xE8), &m)) {
        guest::log("midoha: his body's model isn't one this reads: no outfit");
        return false;
    }
    // the bones: by name in the body, their inverse binds
    for (u32 i = 0; i < n_bones; i++) {
        const u8* b = &f[o_bones + 64 * i];
        char name[17] = {};
        std::memcpy(name, b, 16);
        const s32 id = node_mtx(m, name);
        if (id < 0) {
            guest::log("midoha: his body has no node %s: no outfit", name);
            return false;
        }
        bone_mtx_.push_back(id);
        if (!std::strcmp(name, "Chest"))
            chest_mtx_ = id;
        Mtx34 ib;
        for (u32 k = 0; k < 12; k++)
            ib.m[k / 4][k % 4] = rdf(b + 16 + 4 * k);
        inv_bind_.push_back(ib);
    }
    for (u32 i = 0; i < n_sets; i++) {
        const u8* s = &f[o_sets + 20 * i];
        Set st;
        for (u32 k = 0; k < 4; k++)
            st.bone[k] = s[k] < n_bones ? s[k] : 0xFF, st.w[k] = rdf(s + 4 + 4 * k);
        sets_.push_back(st);
    }
    for (u32 i = 0; i < n_loads; i++)
        loads_.push_back({f[o_loads + 4 * i], rd16(&f[o_loads + 4 * i + 2])});
    for (u32 i = 0; i < n_batches; i++) {
        const u8* b = &f[o_batches + 12 * i];
        Batch bt = {rd32(b), rd32(b + 4), rd16(b + 8), rd16(b + 10)};
        if (bt.dl + bt.size > dl_size || bt.first_load + bt.n_loads > n_loads) {
            guest::log("midoha: %s is damaged (batch %u): no outfit", path.c_str(), (unsigned)i);
            return false;
        }
        // (its triangles: the list's vertex count, big-endian after the command byte, over 3)
        n_tris_ += (u32)(f[o_dl + bt.dl + 1] << 8 | f[o_dl + bt.dl + 2]) / 3;
        batches_.push_back(bt);
    }
    skin_.resize(n_bones);
    // the body's hands and shoes
    std::vector<std::vector<u8>> body_dls;
    if (!make_body_lists(m.base, &body_dls))
        return false;
    // the block: arrays, the garment's lists, the texture levels, their objects, the body's lists,
    // the scratch matrices
    u32 size = 0;
    auto take = [&](u32 n) {
        const u32 a = size;
        size += align32(n);
        return a;
    };
    const u32 o_a = take(array_bytes), o_l = take(dl_size), o_t = take(tex_bytes), o_obj = take(0x20 * n_levels_);
    std::vector<u32> o_body;
    for (auto& d : body_dls)
        o_body.push_back(take((u32)d.size()));
    const u32 n_mtx = 32;
    const u32 o_set = take(n_sets * 3 * 0x30), o_bn = take(n_mtx * 0x24), o_bt = take(n_mtx * 0x30), o_c = take(0x40);
    block_ = g3d::new_aligned(size);
    if (!block_) {
        guest::log("midoha: out of memory for his outfit (%u bytes)", (unsigned)size);
        return false;
    }
    pos_ = block_ + o_a, nrm_ = pos_ + align32(n_verts * 12), uv_ = nrm_ + align32(n_verts * 12);
    dl_ = block_ + o_l;
    guest::write_g(pos_, &f[o_arrays], array_bytes);
    guest::write_g(dl_, &f[o_dl], dl_size);
    guest::write_g(block_ + o_t, &f[o_tex], tex_bytes);
    tex_obj_ = block_ + o_obj;
    for (u32 k = 0, at = 0; k < n_levels_; k++) {
        const u32 s = tex_size_ >> k;
        g::GXInitTexObj(tex_obj_ + 0x20 * k, block_ + o_t + at, s, s, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, 0);
        g::GXInitTexObjLOD(tex_obj_ + 0x20 * k, GX_LINEAR, GX_LINEAR, 0, 0, 0, 0.0, 0.0, 0.0);
        at += s * s * 2;
    }
    for (u32 i = 0, k = 0; i < n_body_; i++)
        for (u32 sd = 0; sd < 2; sd++, k++) {
            body_[i].dl[sd] = block_ + o_body[k];
            body_[i].size[sd] = (u32)body_dls[k].size();
            guest::write_g(body_[i].dl[sd], body_dls[k].data(), body_[i].size[sd]);
        }
    set_mtx_ = block_ + o_set, body_nrm_ = block_ + o_bn, body_tex_ = block_ + o_bt, consts_ = block_ + o_c;
    engine::math::store(consts_, ENV_PROJECTION);
    // the body material's sheen: its texture object 0 (ResMatData +0x40 the used maps' bits, +0x44
    // the GXTexObjs nw4r made when the file was bound)
    {
        const u32 mats = m.section(8);
        const u32 mat = m.entries(mats) ? m.entry(mats, 0) : 0;
        spec_obj_ = mat && (guest::load32(mat + 0x40) & 1) ? mat + 0x44 : 0;
    }
    // (the GPU reads all of it from memory: out of the CPU's cache)
    g::DCFlushRange(block_, size);
    // the body's own draw off (it still poses, and its matrices are still made for each frame)
    const u32 vt = guest::load32(scn_mdl);
    gcall<u32>(guest::load32(vt + 0x20), scn_mdl, OPTION_DISABLE_DRAW_OPA, 1u);
    guest::log("midoha: outfit: %u triangles, %u vertices, %u bones, %u weight sets, %u batches (%u matrix loads), "
               "texture %u square in %u levels (%.2f texels a unit), the body's hands and shoes from %u shapes%s; "
               "%u bytes",
               (unsigned)n_tris_, (unsigned)n_verts, (unsigned)n_bones, (unsigned)n_sets, (unsigned)n_batches,
               (unsigned)n_loads, (unsigned)tex_size_, (unsigned)n_levels_, density_, (unsigned)n_body_,
               spec_obj_ ? "" : " (no sheen: the body's material has no texture)", (unsigned)size);
    return true;
}

// The body's display lists, its hands' and shoes' triangles only (a triangle with a corner on one
// of their nodes), each shape's indexed matrix loads replayed in both (each draws under the same
// matrices).
bool MidohaOutfit::make_body_lists(u32 res_mdl, std::vector<std::vector<u8>>* dls) {
    Mdl m;
    open_mdl(res_mdl, &m);
    s32 keep_mtx[4];
    for (u32 k = 0; k < 2; k++)
        keep_mtx[k] = node_mtx(m, HAND_NODES[k]), keep_mtx[2 + k] = node_mtx(m, SHOE_NODES[k]);
    auto group_of = [&](u32 id) -> int {
        for (u32 k = 0; k < 4; k++)
            if ((s32)id == keep_mtx[k])
                return k < 2 ? 0 : 1;
        return -1;
    };
    const u32 shapes = m.section(10);
    for (u32 i = 0; i < m.entries(shapes) && n_body_ < 4; i++) {
        const u32 s = m.entry(shapes, i);
        BodyList b;
        b.mtx_id = (s32)guest::load32(s + 0x08);
        b.setup = s + 0x18 + (u32)(s32)guest::load32(s + 0x20);
        b.setup_size = guest::load32(s + 0x1C);
        const u32 prim = s + 0x24 + (u32)(s32)guest::load32(s + 0x2C), prim_size = guest::load32(s + 0x28);
        const u32 pos_arr = find_array(m, 2, (s16)guest::load16(s + 0x48));
        const u32 nrm_arr = find_array(m, 3, (s16)guest::load16(s + 0x4A));
        if (!pos_arr)
            continue;
        b.pos = pos_arr + (u32)(s32)guest::load32(pos_arr + 0x08);
        b.pos_stride = guest::load8(pos_arr + 0x1D);
        if (nrm_arr) {
            b.nrm = nrm_arr + (u32)(s32)guest::load32(nrm_arr + 0x08);
            b.nrm_stride = guest::load8(nrm_arr + 0x1D);
        }
        u32 vcd_lo = 0, vcd_hi = 0;
        for (u32 o = 0; o + 6 <= b.setup_size;) {
            const u8 c = guest::load8(b.setup + o);
            if (c == 0x08) {
                const u8 reg = guest::load8(b.setup + o + 1);
                const u32 v = guest::load32(b.setup + o + 2);
                if (reg == 0x50)
                    vcd_lo = v;
                else if (reg == 0x60)
                    vcd_hi = v;
                o += 6;
            } else if (c == 0x10) {
                o += 5 + 4 * (guest::load16(b.setup + o + 1) + 1);
            } else if (c == 0x61) {
                o += 5;
            } else {
                o += 1;
            }
        }
        const VertexLayout lay = layout_of(vcd_lo, vcd_hi);
        if (!lay.ok) {
            guest::log("midoha: a body shape's vertices aren't indexed: no outfit");
            return false;
        }
        u32 slot_mtx[10];
        for (u32& x : slot_mtx)
            x = b.mtx_id >= 0 ? (u32)b.mtx_id : 0;
        std::vector<u8> dl[2];
        for (u32 o = 0; o < prim_size;) {
            const u32 at = prim + o;
            const u8 c = guest::load8(at);
            if (c == 0x00 || c == 0x48) {
                o += 1;
            } else if (c == 0x20 || c == 0x28 || c == 0x30 || c == 0x38) {
                if (c == 0x20) {
                    const u32 idx = guest::load16(at + 1), addr = guest::load16(at + 3) & 0xFFF;
                    if (addr / 12 < 10)
                        slot_mtx[addr / 12] = idx;
                }
                for (auto& d : dl)
                    for (u32 k = 0; k < 5; k++)
                        d.push_back(guest::load8(at + k));
                o += 5;
            } else if (c == 0x08 || c == 0x10 || c == 0x61) {
                const u32 n = c == 0x08 ? 6 : c == 0x61 ? 5 : 5 + 4 * (guest::load16(at + 1) + 1);
                for (auto& d : dl)
                    for (u32 k = 0; k < n; k++)
                        d.push_back(guest::load8(at + k));
                o += n;
            } else if (c & 0x80) {
                const u32 op = c & 0xF8, vat = c & 7, count = guest::load16(at + 1);
                const u32 verts = at + 3;
                std::vector<u32> tri;
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
                std::vector<u32> part[2];
                for (u32 t = 0; t < tri.size(); t += 3) {
                    int g = -1;
                    for (u32 q = 0; q < 3 && g < 0; q++) {
                        const u32 v = verts + tri[t + q] * lay.size;
                        const u32 sl = lay.pnmtx >= 0 ? guest::load8(v + (u32)lay.pnmtx) / 3 : 0;
                        g = group_of(sl < 10 ? slot_mtx[sl] : 0);
                    }
                    if (g >= 0)
                        part[g].insert(part[g].end(), {tri[t], tri[t + 1], tri[t + 2]});
                }
                for (int sd = 0; sd < 2; sd++) {
                    const std::vector<u32>& st = part[sd];
                    for (u32 from = 0; from < st.size(); from += 0xFFF0) {
                        const u32 n = (u32)std::min<size_t>(0xFFF0, st.size() - from);
                        dl[sd].push_back((u8)(GX_DRAW_TRIANGLES | vat));
                        put16(dl[sd], n);
                        for (u32 k = 0; k < n; k++)
                            for (u32 x = 0; x < lay.size; x++)
                                dl[sd].push_back(guest::load8(verts + st[from + k] * lay.size + x));
                    }
                }
                o += 3 + count * lay.size;
            } else {
                guest::log("midoha: a body shape's display list has GX command %02X: no outfit", c);
                return false;
            }
        }
        for (auto& d : dl) {
            while (d.size() & 31)
                d.push_back(0);
            dls->push_back(std::move(d));
        }
        body_[n_body_++] = b;
    }
    return n_body_ > 0;
}

void MidohaOutfit::draw(u32 scn_mdl, f32 fovy_deg) {
    if (!block_ || !scn_mdl)
        return;
    const u64 t0 = guest::host_time_us();
    // this frame's view matrices (ScnMdlSimple: +0xF4 positions, two sets, +0x101 the current, +0x102
    // how many), by matrix id
    const u32 n = guest::load16(scn_mdl + 0x102), cur = guest::load8(scn_mdl + 0x101);
    const u32 view_pos = guest::load32(scn_mdl + 0xF4) + cur * align32(n * 0x30);
    // the bones' skinning matrices, the sets' matrices into the scratch
    for (size_t b = 0; b < skin_.size(); b++) {
        const u32 id = (u32)bone_mtx_[b];
        const Mtx34 v = id < n ? engine::math::load<Mtx34>(view_pos + 0x30 * id) : engine::math::identity34();
        skin_[b] = engine::math::concat(v, inv_bind_[b]);
    }
    for (size_t s = 0; s < sets_.size(); s++) {
        Mtx34 p, q;
        set_matrices(skin_.data(), sets_[s].bone, sets_[s].w, &p, &q);
        const u32 at = set_mtx_ + (u32)s * 0x90;
        engine::math::store(at, p);
        engine::math::store(at + 0x30, q);
    }
    // the body's normal and sheen matrices by matrix id (its lists load them by index)
    const u32 nb = std::min<u32>(n, 32);
    for (u32 id = 0; id < nb; id++) {
        Mtx34 q;
        if (!engine::math::inverse_transpose(engine::math::load<Mtx34>(view_pos + 0x30 * id), q))
            q = engine::math::identity34();
        engine::math::Mtx33 q3;
        for (u32 i = 0; i < 3; i++)
            for (u32 j = 0; j < 3; j++)
                q3.m[i][j] = q.m[i][j];
        engine::math::store(body_nrm_ + 0x24 * id, q3);
        engine::math::store(body_tex_ + 0x30 * id, q);
    }
    const u64 t1 = guest::host_time_us();
    // the texture's level: a texel to about a pixel at his chest (its view matrix's distance and
    // scale, the camera's field of view)
    u32 level = 0;
    {
        const s32 chest = chest_mtx_;
        if (chest >= 0 && (u32)chest < n) {
            const Mtx34 c = engine::math::load<Mtx34>(view_pos + 0x30 * (u32)chest);
            const f32 dist = -c.m[2][3];
            const f32 scale = std::sqrt(c.m[0][0] * c.m[0][0] + c.m[1][0] * c.m[1][0] + c.m[2][0] * c.m[2][0]);
            const f32 t = std::tan(fovy_deg * 3.14159265f / 360.0f);
            if (dist > 0 && t > 0) {
                const f32 px_per_unit = SCREEN_LINES * 0.5f / t * scale / dist;
                f32 texels_per_px = density_ / std::max(px_per_unit, 1e-3f);
                while (texels_per_px > 1.4f && level + 1 < n_levels_)
                    texels_per_px *= 0.5f, level++;
            }
        }
    }
    level_ = level;
    // the Mii light's tint (the actor manager's Mii model resource +0x34), as the body's materials
    const u32 model_res = guest::load32(GA_mii_ModelRes_sInstance);
    const u32 tint = model_res ? guest::load32(model_res + 0x34) : 0xFFFFFFFF;
    draw_body(view_pos, n, tint);
    draw_garment(tint, level);
    const u64 t2 = guest::host_time_us();
    cost_us_ += t2 - t0, matrix_us_ += t1 - t0;
    if (++cost_frames_ >= LOG_EVERY) {
        guest::log("midoha: outfit: %.3f ms a frame on the CPU (its matrices %.3f ms), texture level %u", cost_us_ / 1000.0 / cost_frames_,
                   matrix_us_ / 1000.0 / cost_frames_, (unsigned)level_);
        cost_us_ = matrix_us_ = 0, cost_frames_ = 0;
    }
}

// the GX state both draws share: one colour channel, unlit, its material colour the tint
static void common_state(u32 tint, u32 consts) {
    guest::store32(consts + 0x30, tint);
    g::GXSetNumChans(1);
    g::GXSetChanCtrl(GX_COLOR0A0, 0, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
    g::GXSetChanMatColor(GX_COLOR0A0, consts + 0x30);
    g::GXSetNumIndStages(0);
    g::GXSetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
    for (u32 s = 0; s < 2; s++) {
        g::GXSetTevDirect(s);
        g::GXSetTevSwapMode(s, GX_TEV_SWAP0, GX_TEV_SWAP0);
        g::GXSetTevKAlphaSel(s, GX_TEV_KASEL_1);
        g::GXSetTevColorOp(s, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, 1, GX_TEVPREV);
        g::GXSetTevAlphaOp(s, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, 1, GX_TEVPREV);
    }
    g::GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    g::GXSetZCompLoc(1);
    g::GXSetZMode(1, GX_LEQUAL, 1);
    g::GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, 0, GX_LO_CLEAR);
    g::GXSetCullMode(GX_CULL_BACK);
    g::GXSetColorUpdate(1);
    // texture coordinate 0: the sheen, from the normal (each vertex's matrix: TEX0MTXIDX), through the
    // env camera's projection after it
    g::GXLoadTexMtxImm(consts, GX_PTTEXMTX0, GX_MTX3x4);
    g::GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX3x4, GX_TG_NRM, GX_TEXMTX0, 1, GX_PTTEXMTX0);
}

// The hands and the shoes: the body's own lists (positions from its view matrices, the normal and
// sheen matrices ours), a colour each times the tint, plus the sheen.
void MidohaOutfit::draw_body(u32 view_pos, u32 n_mtx, u32 tint) {
    common_state(tint, consts_);
    g::GXSetNumTexGens(1);
    g::GXSetNumTevStages(1);
    if (spec_obj_)
        g::GXLoadTexObj(spec_obj_, GX_TEXMAP1);
    g::GXSetTevOrder(0, GX_TEXCOORD0, spec_obj_ ? GX_TEXMAP1 : 0xFF, GX_COLOR0A0);
    g::GXSetTevKColorSel(0, GX_TEV_KCSEL_K1);
    // texc + konst x rasc
    g::GXSetTevColorIn(0, GX_CC_ZERO, GX_CC_KONST, GX_CC_RASC, spec_obj_ ? GX_CC_TEXC : GX_CC_ZERO);
    g::GXSetTevAlphaIn(0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    g::GXSetArray(GX_POS_MTX_ARRAY, view_pos, 0x30);
    g::GXSetArray(GX_NRM_MTX_ARRAY, body_nrm_, 0x24);
    g::GXSetArray(GX_TEX_MTX_ARRAY, body_tex_, 0x30);
    for (int sd = 0; sd < 2; sd++) {
        guest::store32(consts_ + 0x34, sd == 0 ? HAND_RGBA : SHOE_RGBA);
        g::GXSetTevKColor(GX_KCOLOR1, consts_ + 0x34);
        for (u32 i = 0; i < n_body_; i++) {
            const BodyList& l = body_[i];
            if (!l.size[sd])
                continue;
            g::GXSetArray(GX_VA_POS, l.pos, l.pos_stride);
            if (l.nrm)
                g::GXSetArray(GX_VA_NRM, l.nrm, l.nrm_stride);
            g::GXInvalidateVtxCache();
            if (l.mtx_id >= 0 && (u32)l.mtx_id < n_mtx) {
                g::GXLoadPosMtxImm(view_pos + 0x30 * (u32)l.mtx_id, GX_PNMTX0);
                g::GXLoadNrmMtxImm(body_tex_ + 0x30 * (u32)l.mtx_id, GX_PNMTX0);
                g::GXLoadTexMtxImm(body_tex_ + 0x30 * (u32)l.mtx_id, GX_TEXMTX0, GX_MTX3x4);
                g::GXSetCurrentMtx(GX_PNMTX0);
            }
            g::GXCallDisplayList(l.setup, l.setup_size);
            g::GXCallDisplayList(l.dl[sd], l.size[sd]);
        }
    }
}

// The garment: its texture (coordinate 1, from the vertices') times the tint, plus the sheen; each
// batch's matrix loads, then its list.
void MidohaOutfit::draw_garment(u32 tint, u32 level) {
    common_state(tint, consts_);
    g::GXSetTexCoordGen2(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, 0, GX_PTIDENTITY);
    g::GXSetNumTexGens(2);
    g::GXSetNumTevStages(2);
    g::GXLoadTexObj(tex_obj_ + 0x20 * level, GX_TEXMAP0);
    if (spec_obj_)
        g::GXLoadTexObj(spec_obj_, GX_TEXMAP1);
    // stage 0: texc x rasc; stage 1: + sheen x konst
    g::GXSetTevOrder(0, GX_TEXCOORD1, GX_TEXMAP0, GX_COLOR0A0);
    g::GXSetTevColorIn(0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
    g::GXSetTevAlphaIn(0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    g::GXSetTevOrder(1, GX_TEXCOORD0, spec_obj_ ? GX_TEXMAP1 : 0xFF, GX_COLOR_NULL);
    guest::store32(consts_ + 0x38, (u32)SHEEN << 24 | (u32)SHEEN << 16 | (u32)SHEEN << 8 | 0xFF);
    g::GXSetTevKColor(GX_KCOLOR0, consts_ + 0x38);
    g::GXSetTevKColorSel(1, GX_TEV_KCSEL_K0);
    g::GXSetTevColorIn(1, GX_CC_ZERO, spec_obj_ ? GX_CC_TEXC : GX_CC_ZERO, GX_CC_KONST, GX_CC_CPREV);
    g::GXSetTevAlphaIn(1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    // the vertex format: matrix indices direct, the rest indexed f32
    g::GXClearVtxDesc();
    g::GXSetVtxDesc(GX_VA_PNMTXIDX, GX_DIRECT);
    g::GXSetVtxDesc(GX_VA_TEX0MTXIDX, GX_DIRECT);
    g::GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
    g::GXSetVtxDesc(GX_VA_NRM, GX_INDEX16);
    g::GXSetVtxDesc(GX_VA_TEX0, GX_INDEX16);
    g::GXSetVtxAttrFmt(VTXFMT, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    g::GXSetVtxAttrFmt(VTXFMT, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
    g::GXSetVtxAttrFmt(VTXFMT, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    g::GXSetArray(GX_VA_POS, pos_, 12);
    g::GXSetArray(GX_VA_NRM, nrm_, 12);
    g::GXSetArray(GX_VA_TEX0, uv_, 8);
    g::GXInvalidateVtxCache();
    for (const Batch& b : batches_) {
        for (u32 k = 0; k < b.n_loads; k++) {
            const Load& l = loads_[b.first_load + k];
            const u32 at = set_mtx_ + (u32)l.set * 0x90;
            g::GXLoadPosMtxImm(at, GX_PNMTX0 + 3 * l.slot);
            g::GXLoadNrmMtxImm(at + 0x30, GX_PNMTX0 + 3 * l.slot);
            g::GXLoadTexMtxImm(at + 0x30, GX_TEXMTX0 + 3 * l.slot, GX_MTX3x4);
        }
        g::GXCallDisplayList(dl_ + b.dl, b.size);
    }
}

}  // namespace engine::wuhu
