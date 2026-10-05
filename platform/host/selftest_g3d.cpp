// NOMA_SELFTEST=g3d: the native g3d's core (engine/g3d, engine/gx/tev_combiner) on packages built
// here: loading and every check of a corrupted file, texture conversion (each GX format through
// pica_tex.h to its PICA format, decoded back and compared with runtime/hw/gx/texture.cpp's GX
// decode; CMPR as ETC1 by PSNR), the PICA tiling round trip, animation curves against hand-computed
// values, materials (prelit ones too), a world's culling, detail levels and draw order (the opaque
// items grouped by material), its visibility rows (the eye's cell and height), detail batches, the
// animation rate by size and the combiners' translations kept, and the native-draw packet ring (order,
// wrap-around, a renderer that never runs, release). With NOMA_G3D_PACKAGE=path (default:
// wuhu/island_v2.ng3d, else wuhu/island.ng3d in the working directory, if there) the real package is
// loaded and drawn from each of its view_* markers, and its WorldStats and the World's time are
// logged (a version 1 package is refused: noted, not failed). NOMA_G3D_WRITE_TEST=path writes the
// test scene's package (for tools/ng3d_bake).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "runtime.h"
#include "engine/collision/kcl.h"
#include "engine/g3d/anim.h"
#include "engine/g3d/material.h"
#include "engine/g3d/native_draw.h"
#include "engine/g3d/package.h"
#include "engine/g3d/package_writer.h"
#include "engine/g3d/texture.h"
#include "engine/g3d/world.h"
#include "hw/gx/gx.h"
#include "hw/gx/texture.h"
#include "platform/3ds/pica_tex.h"

namespace guest {

extern void (*g_gather_capture)(const u8* burst);

namespace {

namespace g3 = engine::g3d;
namespace fm = engine::g3d::fmt;
using engine::math::Vec3;

int s_failures, s_checks;
void expect(bool ok, const char* what, ...) __attribute__((format(printf, 2, 3)));
void expect(bool ok, const char* what, ...) {
    s_checks++;
    if (ok)
        return;
    s_failures++;
    char buf[400];
    va_list ap;
    va_start(ap, what);
    vsnprintf(buf, sizeof(buf), what, ap);
    va_end(ap);
    log("selftest g3d: FAILED: %s", buf);
}

struct Rng {
    u32 s;
    u32 operator()() {
        s ^= s << 13, s ^= s >> 17, s ^= s << 5;
        return s;
    }
    f32 unit() { return (f32)((*this)() & 0xFFFFFF) / 16777216.0f; }
};

g3::Package* load_bytes(const std::vector<u8>& bytes, std::string* error) {
    void* block = malloc(bytes.size() ? bytes.size() : 1);
    memcpy(block, bytes.data(), bytes.size());
    return g3::Package::load(block, (u32)bytes.size(), error);
}

// ---------------------------------------------------------------- a package built here
// One vertex: position, normal, colour, uv0 (s16, scale 1/256)
struct TestVertex {
    f32 pos[3];
    s8 nrm[4];
    u8 clr[4];
    s16 uv[2];
};
static_assert(sizeof(TestVertex) == 24, "TestVertex");

struct TestPackage {
    g3::StringTable strs;
    std::vector<fm::Texture> texs;
    std::vector<std::vector<u8>> texels;
    std::vector<fm::Material> mats;
    std::vector<fm::TexMtx> tmtx;
    std::vector<fm::VertexBuffer> vbs;
    std::vector<std::vector<u8>> vdata;
    std::vector<u16> idx;
    std::vector<fm::Batch> batches;
    std::vector<fm::Node> nodes;
    std::vector<fm::Model> models;
    std::vector<fm::Cell> cells;
    std::vector<u16> cbat;
    std::vector<fm::Instance> insts;
    std::vector<fm::Anim> anims;
    std::vector<fm::Track> tracks;
    std::vector<fm::Key> keys;
    std::vector<fm::Light> lights;
    std::vector<fm::LightSet> lsets;
    std::vector<fm::Fog> fogs;
    std::vector<fm::Marker> marks;
    std::vector<fm::Collision> colls;
    std::vector<std::vector<u8>> blobs;
    std::vector<u8> cvis;  // the visibility rows (env.pvs_row_bytes a cell)
    fm::Env env = {};
    bool has_env = true;

    TestPackage() {
        env.sky_model = env.sky_anim = env.sea_model = env.sea_anim_srt = env.sea_anim_clr = fm::NONE16;
        env.world_anim_srt = env.world_anim_clr = fm::NONE16;
        env.mii_units_per_unit = 1.0f;
    }

    std::vector<u8> bytes() {
        g3::PackageWriter w;
        w.section(fm::SEC_STRS, strs.bytes().data(), (u32)strs.bytes().size(), 0);
        // texel data: each texture 128-aligned in TDAT
        std::vector<u8> tdat;
        for (size_t i = 0; i < texs.size(); i++) {
            tdat.resize((tdat.size() + 127) / 128 * 128);
            texs[i].data = (u32)tdat.size();
            texs[i].size = (u32)texels[i].size();
            tdat.insert(tdat.end(), texels[i].begin(), texels[i].end());
        }
        w.array(fm::SEC_TEXS, texs);
        for (size_t i = 0; i < texs.size(); i++)
            w.relocate(fm::SEC_TEXS, (u32)(i * sizeof(fm::Texture) + offsetof(fm::Texture, data)), fm::SEC_TDAT);
        w.array(fm::SEC_MATS, mats);
        w.array(fm::SEC_TMTX, tmtx);
        std::vector<u8> vdat;
        for (size_t i = 0; i < vbs.size(); i++) {
            vdat.resize((vdat.size() + 15) / 16 * 16);
            vbs[i].data = (u32)vdat.size();
            vdat.insert(vdat.end(), vdata[i].begin(), vdata[i].end());
        }
        w.array(fm::SEC_VTXB, vbs);
        for (size_t i = 0; i < vbs.size(); i++)
            w.relocate(fm::SEC_VTXB, (u32)(i * sizeof(fm::VertexBuffer) + offsetof(fm::VertexBuffer, data)), fm::SEC_VDAT);
        w.array(fm::SEC_IDAT, idx);
        w.array(fm::SEC_BTCH, batches);
        w.array(fm::SEC_NODE, nodes);
        w.array(fm::SEC_MODL, models);
        w.array(fm::SEC_CELL, cells);
        w.array(fm::SEC_CBAT, cbat);
        w.array(fm::SEC_INST, insts);
        w.array(fm::SEC_ANIM, anims);
        w.array(fm::SEC_TRAK, tracks);
        w.array(fm::SEC_KEYS, keys);
        if (has_env)
            w.section(fm::SEC_ENVR, &env, sizeof(env), 1);
        w.array(fm::SEC_LITE, lights);
        w.array(fm::SEC_LSET, lsets);
        w.array(fm::SEC_FOGS, fogs);
        w.array(fm::SEC_MARK, marks);
        std::vector<u8> blob;
        for (size_t i = 0; i < colls.size(); i++) {
            blob.resize((blob.size() + 31) / 32 * 32);
            colls[i].data = (u32)blob.size();
            colls[i].size = (u32)blobs[i].size();
            blob.insert(blob.end(), blobs[i].begin(), blobs[i].end());
        }
        w.array(fm::SEC_COLL, colls);
        for (size_t i = 0; i < colls.size(); i++)
            w.relocate(fm::SEC_COLL, (u32)(i * sizeof(fm::Collision) + offsetof(fm::Collision, data)), fm::SEC_BLOB);
        w.section(fm::SEC_VDAT, vdat.data(), (u32)vdat.size(), 0);
        w.section(fm::SEC_TDAT, tdat.data(), (u32)tdat.size(), 0, 128);
        w.section(fm::SEC_BLOB, blob.data(), (u32)blob.size(), 0);
        if (!cvis.empty())
            w.section(fm::SEC_CVIS, cvis.data(), (u32)cvis.size(), 0);
        return w.finish();
    }

    u16 add_texture(const char* name, u32 w, u32 h, u8 format, u8 levels, u8 flags, u8 gx_format, std::vector<u8> data) {
        fm::Texture t = {};
        t.name = strs.add(name);
        t.width = (u16)w, t.height = (u16)h, t.format = format, t.levels = levels, t.flags = flags;
        t.gx_format = gx_format, t.src_width = (u16)w, t.src_height = (u16)h;
        texs.push_back(t);
        texels.push_back(std::move(data));
        return (u16)(texs.size() - 1);
    }

    // a material: vertex colour x texture map 0 (GX: one stage, colour = ras * tex, alpha = ras * tex)
    u16 add_material(const char* name, u16 texture, u32 flags, s8 priority = 0) {
        fm::Material m = {};
        m.name = strs.add(name);
        m.flags = flags;
        m.n_stages = 1, m.n_texgens = 1, m.n_chans = 1;
        m.cull = 2;
        // colour: a=0 (ZERO 15), b=TEXC (8), c=RASC (10), d=ZERO (15): lerp(0, tex, ras) = tex * ras
        m.color_env[0] = 15 | 10 << 4 | 8 << 8 | 15 << 12 | 1 << 19;
        // alpha: a=ZERO (7), b=TEXA (4), c=RASA (5), d=ZERO (7)
        m.alpha_env[0] = 7 << 4 | 5 << 7 | 4 << 10 | 7 << 13 | 1 << 19;
        m.tref[0] = 0 | 0 << 3 | 1 << 6 | 0 << 7;  // map 0, coord 0, enabled, colour channel 0
        m.alpha_compare = 7 << 16 | 7 << 19;        // always
        m.zmode = 1 | 3 << 1 | 0x10;                // test LEQUAL, update
        m.blend = (flags & fm::MAT_XLU) ? (1 | 0x08 | 4 << 8 | 5 << 5) : 0x08;
        m.chan_color = 1 | 2 | 1 << 2 | 2 << 7;     // vertex colour, lit, light 0, clamped
        m.chan_alpha = 1;
        memset(m.mat_color, 255, 4);
        memset(m.amb_color, 64, 4);
        for (auto& mp : m.maps)
            mp.texture = fm::NONE16;
        m.maps[0].texture = texture;
        m.maps[0].wrap_s = m.maps[0].wrap_t = 1;
        m.maps[0].min_filter = 5, m.maps[0].mag_filter = 1;
        m.maps[0].lod_bias = -1.0f, m.maps[0].max_lod = 10;
        for (auto& g : m.texgens)
            g.matrix = g.post_mtx = fm::NONE16, g.type = 1, g.source = 4;
        m.priority = priority;
        mats.push_back(m);
        return (u16)(mats.size() - 1);
    }

    // a vertex buffer of a box (8 corners, 12 triangles) around (c +- e), and a batch drawing it
    u16 add_box_batch(Vec3 c, Vec3 e, u16 material, u16 node = fm::NONE16) {
        fm::VertexBuffer vb = {};
        vb.count = 8, vb.attrs = fm::VA_POS | fm::VA_NRM | fm::VA_CLR | fm::VA_UV0, vb.stride = 24;
        vb.uv_scale[0] = vb.uv_scale[1] = 1.0f / 256;
        std::vector<u8> d(8 * sizeof(TestVertex));
        for (int i = 0; i < 8; i++) {
            TestVertex v = {};
            v.pos[0] = c.x + (i & 1 ? e.x : -e.x), v.pos[1] = c.y + (i & 2 ? e.y : -e.y), v.pos[2] = c.z + (i & 4 ? e.z : -e.z);
            v.nrm[1] = 127;
            memset(v.clr, 200, 4);
            v.uv[0] = (s16)((i & 1) * 256), v.uv[1] = (s16)((i >> 1 & 1) * 256);
            memcpy(&d[i * sizeof(v)], &v, sizeof(v));
        }
        vbs.push_back(vb);
        vdata.push_back(d);
        static const u16 k_tris[36] = {0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5, 0, 4, 5, 0, 5, 1,
                                       2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3};
        fm::Batch b = {};
        b.material = material, b.vbuf = (u16)(vbs.size() - 1);
        b.first_index = (u32)idx.size(), b.n_indices = 36;
        idx.insert(idx.end(), k_tris, k_tris + 36);
        const f32 box[6] = {c.x - e.x, c.y - e.y, c.z - e.z, c.x + e.x, c.y + e.y, c.z + e.z};
        memcpy(b.bbox, box, sizeof(box));
        b.node = node;
        batches.push_back(b);
        return (u16)(batches.size() - 1);
    }

    // the same box with compact positions (fmt::VA_POS16: s16 x4 from its centre in steps of 1/64:
    // exact for these corners), normal, colour, uv0
    u16 add_box_batch16(Vec3 c, Vec3 e, u16 material) {
        const u16 b = add_box_batch(c, e, material);
        fm::VertexBuffer& vb = vbs.back();
        std::vector<u8>& d = vdata.back();
        std::vector<u8> out(8 * 20);
        const f32 scale = 1.0f / 64;
        for (int i = 0; i < 8; i++) {
            TestVertex v;
            memcpy(&v, &d[i * sizeof(v)], sizeof(v));
            const s16 p[4] = {(s16)std::lround((v.pos[0] - c.x) / scale), (s16)std::lround((v.pos[1] - c.y) / scale),
                              (s16)std::lround((v.pos[2] - c.z) / scale), 0};
            memcpy(&out[i * 20], p, 8);
            memcpy(&out[i * 20 + 8], v.nrm, 4);
            memcpy(&out[i * 20 + 12], v.clr, 4);
            memcpy(&out[i * 20 + 16], v.uv, 4);
        }
        d = out;
        vb.attrs = fm::VA_POS16 | fm::VA_NRM | fm::VA_CLR | fm::VA_UV0, vb.stride = 20;
        vb.pos_origin[0] = c.x, vb.pos_origin[1] = c.y, vb.pos_origin[2] = c.z, vb.pos_scale = scale;
        return b;
    }

    u16 add_track(u8 target, u8 interp, u16 index, u16 model, u8 reg, std::vector<fm::Key> ks) {
        fm::Track t = {};
        t.target = target, t.interp = interp, t.index = index, t.model = model, t.reg = reg;
        t.first_key = (u32)keys.size(), t.n_keys = (u32)ks.size();
        keys.insert(keys.end(), ks.begin(), ks.end());
        tracks.push_back(t);
        return (u16)(tracks.size() - 1);
    }
};

// A scene: a 3x3 grid of 1000-unit cells around the origin (each two detail levels: a box, then a
// flatter one from 1500 on, with compact positions; the middle cell also a translucent overlay), a
// tree model (two nodes, the
// second a Y billboard) with a low-detail model from 800 on, a row of tree instances, a sky, a
// scrolling and a colour-animated material, an animated instance, lights, a fog, markers.
struct Scene {
    TestPackage p;
    u16 mat_ground, mat_xlu, mat_scroll, mat_tree, mat_sky, mat_xlu_front;
    u16 tex_cmpr, tex_i8, tex_pica;
    u16 model_tree, model_tree_l, model_sky;
    std::vector<u16> cell_lod0, cell_lod1;  // batch per cell at each level
    u16 xlu_batch, xlu_front_batch;

    Scene() {
        Rng r{1234};
        // textures: a GX CMPR 32x32 with 3 levels (-> ETC1), a GX I8 16x16 (-> L8), a PICA RGBA8 16x16
        std::vector<u8> cmpr(g3::gx_texture_bytes(14, 32, 32, 3));
        for (size_t i = 0; i < cmpr.size(); i += 8) {  // (texture-like: each block's colours close)
            const u32 c0 = r() & 0xFFFF;
            const u32 c1 = (u32)std::min(31u, (c0 >> 11) + 1 + r() % 3) << 11 | std::min(63u, (c0 >> 5 & 63) + 1 + r() % 6) << 5 |
                           std::min(31u, (c0 & 31) + 1 + r() % 3);
            cmpr[i] = (u8)(c1 >> 8), cmpr[i + 1] = (u8)c1, cmpr[i + 2] = (u8)(c0 >> 8), cmpr[i + 3] = (u8)c0;
            for (int k = 4; k < 8; k++)
                cmpr[i + k] = (u8)r();
        }
        tex_cmpr = p.add_texture("tex_cmpr", 32, 32, fm::TF_ETC1, 3, fm::TEX_GX_RAW, 14, cmpr);
        std::vector<u8> i8(g3::gx_texture_bytes(1, 16, 16, 1));
        for (u8& b : i8)
            b = (u8)r();
        tex_i8 = p.add_texture("tex_i8", 16, 16, fm::TF_L8, 1, fm::TEX_GX_RAW, 1, i8);
        std::vector<u8> rgba(g3::pica_texture_bytes(fm::TF_RGBA8, 16, 16, 2));
        for (u8& b : rgba)
            b = (u8)r();
        tex_pica = p.add_texture("tex_pica", 16, 16, fm::TF_RGBA8, 2, 0, 6, rgba);
        // materials
        mat_ground = p.add_material("mt_ground", tex_cmpr, 0);
        mat_xlu = p.add_material("mt_overlay", tex_pica, fm::MAT_XLU, 0);
        mat_xlu_front = p.add_material("mt_overlay_first", tex_pica, fm::MAT_XLU, -1);
        mat_scroll = p.add_material("mt_water", tex_i8, fm::MAT_FOG);
        mat_tree = p.add_material("mt_tree", tex_pica, fm::MAT_LIGHT_SET);
        mat_sky = p.add_material("mt_sky", tex_pica, 0);
        p.mats[mat_tree].light_set = 0;
        p.mats[mat_scroll].fog = 0;
        // mt_water: a texture matrix (SRT), animated; and C1 animated (the colour lerps ras * C1)
        fm::TexMtx tm = {};
        tm.scale[0] = tm.scale[1] = 1;
        p.tmtx.push_back(tm);
        p.mats[mat_scroll].tex_mtx_first = 0, p.mats[mat_scroll].n_tex_mtx = 1;
        p.mats[mat_scroll].texgens[0].matrix = 0;
        p.mats[mat_scroll].color_env[0] = 15 | 10 << 4 | 2 << 8 | 15 << 12 | 1 << 19;  // ras * C0... (b = C0)
        p.mats[mat_scroll].tev_reg[1][0] = p.mats[mat_scroll].tev_reg[1][1] = p.mats[mat_scroll].tev_reg[1][2] = 100;
        // the grid of cells
        for (int z = -1; z <= 1; z++)
            for (int x = -1; x <= 1; x++) {
                const Vec3 c = {x * 1000.0f, 0, z * 1000.0f};
                const u16 b0 = p.add_box_batch(c, {490, 50, 490}, (x == 0 && z == 0) ? mat_scroll : mat_ground);
                const u16 b1 = p.add_box_batch16(c, {490, 10, 490}, mat_ground);  // (compact positions)
                fm::Cell cl = {};
                const f32 box[6] = {c.x - 500, -60, c.z - 500, c.x + 500, 60, c.z + 500};
                memcpy(cl.bbox, box, sizeof(box));
                cl.n_lods = 2;
                cl.first[0] = (u32)p.cbat.size(), cl.count[0] = 1;
                p.cbat.push_back(b0);
                cl.first[1] = (u32)p.cbat.size(), cl.count[1] = 1;
                p.cbat.push_back(b1);
                cl.lod_distance[0] = 0, cl.lod_distance[1] = 1500, cl.lod_distance[2] = 0;
                cl.group = p.strs.add(x == 1 && z == 1 ? "grp_corner" : "grp_main");
                p.cells.push_back(cl);
                cell_lod0.push_back(b0), cell_lod1.push_back(b1);
            }
        // the translucent overlays in the middle cell (one far, one near: back to front; the near one
        // with a lower priority, so first anyway), both ahead of the world test's eye (a cell's batches
        // are culled by their own boxes)
        xlu_batch = p.add_box_batch({0, 60, -300}, {100, 1, 100}, mat_xlu);
        xlu_front_batch = p.add_box_batch({0, 60, -150}, {100, 1, 100}, mat_xlu_front);
        p.cells[4].count[0] = 3;
        p.cbat.erase(p.cbat.begin() + p.cells[4].first[0] + 1);  // (its lod-1 batch moves after the overlays)
        p.cbat.insert(p.cbat.begin() + p.cells[4].first[0] + 1, {xlu_batch, xlu_front_batch});
        // (the cells after it moved by one entry... rebuild all firsts)
        {
            p.cbat.clear();
            for (size_t i = 0; i < p.cells.size(); i++) {
                fm::Cell& cl = p.cells[i];
                cl.first[0] = (u32)p.cbat.size();
                p.cbat.push_back(cell_lod0[i]);
                if (i == 4)
                    p.cbat.push_back(xlu_batch), p.cbat.push_back(xlu_front_batch);
                cl.count[0] = (u16)(p.cbat.size() - cl.first[0]);
                cl.first[1] = (u32)p.cbat.size();
                p.cbat.push_back(cell_lod1[i]);
                cl.count[1] = 1;
            }
        }
        // the tree: trunk node and a Y-billboard crown above it; its low-detail twin
        auto add_tree_model = [&](const char* name, f32 crown) -> u16 {
            fm::Model m = {};
            m.name = p.strs.add(name);
            m.first_node = (u16)p.nodes.size(), m.n_nodes = 2;
            m.first_batch = (u16)p.batches.size();
            for (int k = 0; k < 2; k++) {
                fm::Node n = {};
                n.name = p.strs.add(k ? "crown" : "trunk");
                n.parent = k ? 0 : fm::NONE16;
                n.flags = fm::NODE_VISIBLE | (k ? fm::NODE_BILLBOARD : 0);
                n.billboard = k ? 6 : 0;  // persp-Y
                n.scale[0] = n.scale[1] = n.scale[2] = 1;
                n.translate[1] = k ? 40.0f : 0.0f;
                const f32 bind[3][4] = {{1, 0, 0, 0}, {0, 1, 0, k ? 40.0f : 0.0f}, {0, 0, 1, 0}};
                memcpy(n.bind, bind, sizeof(bind));
                n.first_batch = (u16)(p.batches.size() + k), n.n_batches = 1;
                p.nodes.push_back(n);
            }
            p.add_box_batch({0, 20, 0}, {3, 20, 3}, mat_tree, 0);
            p.add_box_batch({0, 0, 0}, {crown, crown, 1}, mat_tree, 1);
            m.n_batches = 2;
            const f32 bb[6] = {-crown, 0, -crown, crown, 40 + crown, crown};
            memcpy(m.bbox, bb, sizeof(bb));
            m.radius = std::sqrt(crown * crown * 2 + (20 + crown) * (20 + crown));
            m.lod_next = fm::NONE16;
            p.models.push_back(m);
            return (u16)(p.models.size() - 1);
        };
        model_tree = add_tree_model("tree_H", 15);
        model_tree_l = add_tree_model("tree_L", 14);
        p.models[model_tree].lod_next = model_tree_l, p.models[model_tree].lod_distance = 800;
        // the sky: one node, one batch
        {
            fm::Model m = {};
            m.name = p.strs.add("sky");
            m.first_node = (u16)p.nodes.size(), m.n_nodes = 1, m.first_batch = (u16)p.batches.size(), m.n_batches = 1;
            fm::Node n = {};
            n.name = p.strs.add("sky_root"), n.parent = fm::NONE16, n.flags = fm::NODE_VISIBLE;
            n.scale[0] = n.scale[1] = n.scale[2] = 1;
            const f32 bind[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
            memcpy(n.bind, bind, sizeof(bind));
            n.first_batch = m.first_batch, n.n_batches = 1;
            p.nodes.push_back(n);
            p.add_box_batch({0, 0, 0}, {5000, 5000, 5000}, mat_sky, 0);
            const f32 bb[6] = {-5000, -5000, -5000, 5000, 5000, 5000};
            memcpy(m.bbox, bb, sizeof(bb));
            m.radius = 8700, m.lod_next = fm::NONE16;
            p.models.push_back(m);
            model_sky = (u16)(p.models.size() - 1);
            p.env.sky_model = model_sky;
        }
        // trees in a row along -z (in front of a camera at the origin looking -z), one far off
        for (int i = 0; i < 6; i++) {
            fm::Instance in = {};
            in.model = model_tree, in.anim = fm::NONE16, in.flags = fm::INST_STATIC;
            const f32 mtx[3][4] = {{1, 0, 0, 30}, {0, 1, 0, 0}, {0, 0, 1, -200.0f - 300.0f * i}};
            memcpy(in.mtx, mtx, sizeof(mtx));
            p.insts.push_back(in);
        }
        p.insts[5].cull_distance = 1000;  // (at 1700: beyond its own draw distance)
        // animations: the water's scroll (SRT0: translate s, linear 0 -> 1 over 100 frames, looping);
        // its C0 red (CLR0: steps); a tree's trunk swinging (node rotation z, hermite)
        {
            fm::Anim a = {};
            a.name = p.strs.add("water_srt"), a.frames = 100, a.loop = 1;
            a.first_track = (u32)p.tracks.size(), a.n_tracks = 1;
            p.add_track(fm::TT_TEXMTX_TRANS_S, fm::IP_LINEAR, 0, fm::NONE16, 0, {{0, 0, 0}, {100, 1, 0}});
            p.anims.push_back(a);
            p.env.world_anim_srt = (u16)(p.anims.size() - 1);
        }
        {
            fm::Anim a = {};
            a.name = p.strs.add("water_clr"), a.frames = 4, a.loop = 1;
            a.first_track = (u32)p.tracks.size(), a.n_tracks = 1;
            p.add_track(fm::TT_MAT_REG_R, fm::IP_STEP, mat_scroll, fm::NONE16, fm::CR_TEV0,
                        {{0, 0, 0}, {1, 255, 0}, {2, 128, 0}, {3, 50, 0}});
            p.anims.push_back(a);
            p.env.world_anim_clr = (u16)(p.anims.size() - 1);
        }
        {
            fm::Anim a = {};
            a.name = p.strs.add("tree_sway"), a.frames = 60, a.loop = 1;
            a.first_track = (u32)p.tracks.size(), a.n_tracks = 1;
            p.add_track(fm::TT_NODE_ROT_Z, fm::IP_HERMITE, 0, model_tree, 0, {{0, 0, 0}, {30, 10, 0}, {60, 0, 0}});
            p.anims.push_back(a);
            p.insts[0].anim = (u16)(p.anims.size() - 1);
            p.insts[0].flags |= fm::INST_ANIMATED;
        }
        // environment
        fm::Light l = {};
        l.name = p.strs.add("sun"), l.kind = fm::LK_POINT;
        l.rgba[0] = l.rgba[1] = l.rgba[2] = l.rgba[3] = 255;
        l.position[0] = 10000, l.position[1] = 48000, l.position[2] = 60000;
        p.lights.push_back(l);
        fm::LightSet ls = {};
        ls.name = p.strs.add("Map_Lightset");
        for (auto& x : ls.lights)
            x = fm::NONE16;
        ls.lights[0] = 0;
        ls.ambient[0] = 0x78, ls.ambient[1] = 0x82, ls.ambient[2] = 0x87, ls.ambient[3] = 255;
        p.lsets.push_back(ls);
        fm::Fog f = {};
        f.type = fm::FOG_LINEAR, f.rgba[0] = 0x83, f.rgba[1] = 0xD5, f.rgba[2] = 0xFF, f.rgba[3] = 255;
        f.start = 27978, f.end = 65527;
        p.fogs.push_back(f);
        auto marker = [&](const char* name, u32 kind, Vec3 pos, Vec3 dir) {
            fm::Marker m = {};
            m.name = p.strs.add(name), m.kind = kind;
            m.position[0] = pos.x, m.position[1] = pos.y, m.position[2] = pos.z;
            m.dir[0] = dir.x, m.dir[1] = dir.y, m.dir[2] = dir.z;
            p.marks.push_back(m);
        };
        marker("spawn", 0, {0, 0, 0}, {0, 0, -1});
        marker("view_town", 2, {0, 100, 0}, {0, 100, -1000});
        // a KCL-ish blob
        p.colls.push_back({p.strs.add("ground.kcl"), 0, 0});
        p.blobs.push_back(std::vector<u8>(100, 0x5A));
        const f32 wb[6] = {-1500, -100, -1500, 1500, 100, 1500};
        memcpy(p.env.world_bbox, wb, sizeof(wb));
        p.env.cell_size = 1000, p.env.n_cells_x = p.env.n_cells_z = 3;
        p.env.grid_origin[0] = p.env.grid_origin[1] = -1500;
    }
};

// ---------------------------------------------------------------- loading and its checks
void test_load() {
    Scene sc;
    const std::vector<u8> good = sc.p.bytes();
    std::string error;
    g3::Package* pkg = load_bytes(good, &error);
    expect(pkg != nullptr, "the test package loads (%s)", error.c_str());
    if (!pkg)
        return;
    expect(pkg->textures().count == 3 && pkg->materials().count == 6 && pkg->cells().count == 9 &&
               pkg->instances().count == 6 && pkg->models().count == 3 && pkg->anims().count == 3,
           "counts");
    expect(pkg->converted_textures() == 2, "two GX textures converted at load (%u)", pkg->converted_textures());
    const g3::Package::TextureImage ci = pkg->texture_image(sc.tex_cmpr);
    expect(ci.format == fm::TF_ETC1 && ci.levels == 3 && ci.width == 32 && ci.size == g3::pica_texture_bytes(fm::TF_ETC1, 32, 32, 3),
           "CMPR 32x32 x3 -> ETC1 levels %u size %u", ci.levels, ci.size);
    expect(pkg->find_model("tree_L") == sc.model_tree_l && pkg->find_material("mt_water") == sc.mat_scroll &&
               pkg->find_texture("tex_i8") == sc.tex_i8 && pkg->find_anim("tree_sway") == 2 &&
               pkg->find_model("nope") == -1 && pkg->find_marker("view_town") != nullptr,
           "find by name");
    expect(!strcmp(pkg->str(pkg->models()[0].name), "tree_H"), "strings");
    delete pkg;

    // without ENVR: defaults
    {
        Scene s2;
        s2.p.has_env = false;
        g3::Package* q = load_bytes(s2.p.bytes(), &error);
        expect(q && q->env().sky_model == fm::NONE16 && q->env().mii_units_per_unit == 1.0f, "no ENVR: defaults");
        delete q;
    }

    // corruptions: each must be refused, with a reason
    auto sec = [](std::vector<u8>& b, u32 tag) -> fm::Section* {
        fm::Header* h = (fm::Header*)b.data();
        fm::Section* s = (fm::Section*)(b.data() + sizeof(fm::Header));
        for (u32 i = 0; i < h->n_sections; i++)
            if (s[i].tag == tag)
                return &s[i];
        return nullptr;
    };
    struct Case {
        const char* what;
        std::vector<u8> bytes;
    };
    std::vector<Case> cases;
    auto add_raw = [&](const char* what, auto f) {
        std::vector<u8> b = good;
        f(b);
        cases.push_back({what, b});
    };
    auto add_scene = [&](const char* what, auto f) {
        Scene s;
        f(s);
        cases.push_back({what, s.p.bytes()});
    };
    add_raw("bad magic", [](std::vector<u8>& b) { b[0] = 'X'; });
    add_raw("other version", [](std::vector<u8>& b) { ((fm::Header*)b.data())->version = 99; });
    add_raw("version 1", [](std::vector<u8>& b) { ((fm::Header*)b.data())->version = 1; });
    add_raw("file size", [](std::vector<u8>& b) { ((fm::Header*)b.data())->file_size += 32; });
    add_raw("truncated", [](std::vector<u8>& b) { b.resize(b.size() - 64); ((fm::Header*)b.data())->file_size = (u32)b.size(); });
    add_raw("section table past the end", [](std::vector<u8>& b) { ((fm::Header*)b.data())->n_sections = 100000; });
    add_raw("section misaligned", [&](std::vector<u8>& b) { sec(b, fm::SEC_MATS)->offset += 4; });
    add_raw("section outside", [&](std::vector<u8>& b) { sec(b, fm::SEC_MATS)->size = 0x7FFFFFFF; });
    add_raw("sections overlap", [&](std::vector<u8>& b) { sec(b, fm::SEC_MATS)->offset = sec(b, fm::SEC_TEXS)->offset; });
    add_raw("a tag twice", [&](std::vector<u8>& b) { sec(b, fm::SEC_MATS)->tag = fm::SEC_TEXS; });
    add_raw("records don't fit", [&](std::vector<u8>& b) { sec(b, fm::SEC_BTCH)->count += 1000; });
    add_raw("STRS without its NUL", [&](std::vector<u8>& b) { fm::Section* s = sec(b, fm::SEC_STRS); b[s->offset + s->size - 1] = 'x'; });
    add_scene("name outside STRS", [](Scene& s) { s.p.models[0].name = 100000; });
    add_scene("name in a string's middle", [](Scene& s) { s.p.models[0].name += 1; });
    add_scene("texture format", [](Scene& s) { s.p.texs[2].format = 14; });
    add_scene("texture size not a PICA size", [](Scene& s) { s.p.texs[2].width = 24; });
    add_scene("texture levels below 8x8", [](Scene& s) { s.p.texs[2].levels = 3; });
    add_scene("texture bytes", [](Scene& s) { s.p.texels[2].resize(s.p.texels[2].size() - 16); });
    add_scene("texture GX conversion unsupported", [](Scene& s) { s.p.texs[1].format = fm::TF_RGB565; });
    add_scene("texture GX bytes short", [](Scene& s) { s.p.texels[0].resize(64); });
    add_scene("texture GX levels past 1x1", [](Scene& s) { s.p.texs[1].levels = 200; });
    add_scene("texture source size", [](Scene& s) { s.p.texs[2].src_width = 17; });
    add_raw("texture outside TDAT", [&](std::vector<u8>& b) {
        fm::Section* s = sec(b, fm::SEC_TEXS);
        ((fm::Texture*)(b.data() + s->offset))[0].data = 0;
    });
    add_raw("texture not 128-aligned", [&](std::vector<u8>& b) {
        fm::Section* s = sec(b, fm::SEC_TEXS);
        ((fm::Texture*)(b.data() + s->offset))[2].data += 32;
    });
    add_scene("material map's texture", [](Scene& s) { s.p.mats[0].maps[1].texture = 3; });
    add_scene("material stages", [](Scene& s) { s.p.mats[0].n_stages = 17; });
    add_scene("material light set", [](Scene& s) { s.p.mats[s.mat_tree].light_set = 1; });
    add_scene("material fog", [](Scene& s) { s.p.mats[s.mat_scroll].fog = 1; });
    add_scene("material texture matrices", [](Scene& s) { s.p.mats[0].tex_mtx_first = 1, s.p.mats[0].n_tex_mtx = 1; });
    add_scene("texgen matrix", [](Scene& s) { s.p.mats[0].texgens[0].matrix = 0; });
    add_scene("texture matrix mode", [](Scene& s) { s.p.tmtx[0].mode = 7; });
    add_scene("vertex layout", [](Scene& s) { s.p.vbs[0].attrs = fm::VA_NRM; });
    add_scene("vertex stride", [](Scene& s) { s.p.vbs[0].stride = 20; });
    auto compact = [](Scene& s) -> fm::VertexBuffer& { return s.p.vbs[s.p.batches[s.cell_lod1[0]].vbuf]; };
    add_scene("both position kinds", [&](Scene& s) { compact(s).attrs |= fm::VA_POS; });
    add_scene("compact stride", [&](Scene& s) { compact(s).stride = 16; });
    add_scene("compact scale", [&](Scene& s) { compact(s).pos_scale = 0; });
    add_scene("compact origin", [&](Scene& s) { compact(s).pos_origin[1] = INFINITY; });
    add_scene("an unknown attribute", [](Scene& s) { s.p.vbs[0].attrs |= 64; });
    auto rows = [](Scene& s, u32 bytes, u32 size) {
        s.p.env.pvs_row_bytes = bytes, s.p.env.pvs_max_height = 500;
        s.p.cvis.assign(size, 0xFF);
    };
    add_scene("visibility rows short", [&](Scene& s) { rows(s, 2, 17); });
    add_scene("visibility rows narrow", [&](Scene& s) { rows(s, 1, 9); });
    add_scene("visibility rows missing", [&](Scene& s) { rows(s, 2, 0); });
    add_scene("visibility height", [&](Scene& s) { rows(s, 2, 18), s.p.env.pvs_max_height = NAN; });
    add_scene("detail distance", [](Scene& s) { s.p.env.detail_distance = -1; });
    add_raw("vertices outside VDAT", [&](std::vector<u8>& b) {
        fm::Section* s = sec(b, fm::SEC_VTXB);
        ((fm::VertexBuffer*)(b.data() + s->offset))[3].count = 100000;
    });
    add_scene("batch material", [](Scene& s) { s.p.batches[0].material = 100; });
    add_scene("batch indices", [](Scene& s) { s.p.batches[0].n_indices = 37; });
    add_scene("batch index range", [](Scene& s) { s.p.batches.back().first_index = (u32)s.p.idx.size() - 30; });
    add_scene("index past its vertices", [](Scene& s) { s.p.idx[5] = 8; });
    add_scene("batch box", [](Scene& s) { s.p.batches[0].bbox[0] = NAN; });
    add_scene("node parent after it", [](Scene& s) { s.p.nodes[0].parent = 1; });
    add_scene("node batches", [](Scene& s) { s.p.nodes[0].n_batches = 1000; });
    add_scene("model nodes", [](Scene& s) { s.p.models[0].n_nodes = 100; });
    add_scene("LOD chain loops", [](Scene& s) { s.p.models[s.model_tree_l].lod_next = s.model_tree; });
    add_scene("LOD model", [](Scene& s) { s.p.models[0].lod_next = 77; });
    add_scene("cell levels", [](Scene& s) { s.p.cells[0].n_lods = 4; });
    add_scene("cell batches", [](Scene& s) { s.p.cells[0].count[1] = 1000; });
    add_scene("cell batch index", [](Scene& s) { s.p.cbat[0] = 5000; });
    add_scene("cell level distances", [](Scene& s) { s.p.cells[0].lod_distance[1] = -5; });
    add_scene("instance model", [](Scene& s) { s.p.insts[0].model = 9; });
    add_scene("instance animation", [](Scene& s) { s.p.insts[0].anim = 9; });
    add_scene("animation tracks", [](Scene& s) { s.p.anims[0].n_tracks = 50; });
    add_scene("animation length", [](Scene& s) { s.p.anims[0].frames = 0; });
    add_scene("track target", [](Scene& s) { s.p.tracks[0].target = 12; });
    add_scene("track keys", [](Scene& s) { s.p.tracks[0].n_keys = 0; });
    add_scene("keys out of order", [](Scene& s) { s.p.keys[1].frame = -1; });
    add_scene("node track's node", [](Scene& s) { s.p.tracks[2].index = 5; });
    add_scene("colour track's register", [](Scene& s) { s.p.tracks[1].reg = 7; });
    add_scene("texture matrix track", [](Scene& s) { s.p.tracks[0].index = 4; });
    add_scene("light set's light", [](Scene& s) { s.p.lsets[0].lights[1] = 3; });
    add_scene("light kind", [](Scene& s) { s.p.lights[0].kind = 5; });
    add_scene("fog type", [](Scene& s) { s.p.fogs[0].type = 1; });
    add_scene("marker kind", [](Scene& s) { s.p.marks[0].kind = 9; });
    add_scene("env sky model", [](Scene& s) { s.p.env.sky_model = 40; });
    add_scene("env animation", [](Scene& s) { s.p.env.world_anim_srt = 40; });
    add_scene("env scale", [](Scene& s) { s.p.env.mii_units_per_unit = 0; });
    add_raw("KCL outside BLOB", [&](std::vector<u8>& b) {
        fm::Section* s = sec(b, fm::SEC_COLL);
        ((fm::Collision*)(b.data() + s->offset))[0].size = 0x100000;
    });
    u32 refused = 0;
    for (const Case& c : cases) {
        std::string why;
        g3::Package* q = load_bytes(c.bytes, &why);
        expect(q == nullptr && !why.empty(), "corrupted (%s) refused", c.what);
        if (!strcmp(c.what, "version 1"))
            expect(why.find("stage the island again") != std::string::npos, "a version 1 package: %s", why.c_str());
        if (!q)
            refused++;
        if (s_failures == 0 && getenv("NOMA_G3D_VERBOSE"))
            log("selftest g3d:   %-34s -> %s", c.what, why.c_str());
        delete q;
    }
    log("selftest g3d: load: the test package loads (%zu bytes); %u of %zu corrupted files refused with a reason",
        good.size(), refused, cases.size());
    // random damage: never a crash (each load either refuses or checks out); NOMA_G3D_FUZZ=N files
    Rng r{77};
    u32 accepted = 0, n = getenv("NOMA_G3D_FUZZ") ? (u32)atoi(getenv("NOMA_G3D_FUZZ")) : 3000;
    for (u32 i = 0; i < n; i++) {
        std::vector<u8> b = good;
        const u32 flips = 1 + r() % 4;
        for (u32 k = 0; k < flips; k++) {
            const u32 at = r() % (u32)b.size();
            b[at] ^= (u8)(1u << (r() % 8));
        }
        g3::Package* q = load_bytes(b, nullptr);
        if (q) {
            accepted++;
            // what loads must be safe to use: a world over it, culled and built
            g3::World w(*q);
            w.update(10);
            g3::Packet pk;
            w.build_packet(g3::Pass::OPAQUE, &pk);
            w.build_packet(g3::Pass::TRANSLUCENT, &pk);
        }
        delete q;
    }
    log("selftest g3d: load: %u randomly damaged files: %u refused, %u loaded and used (damage the checks can't see: "
        "texel and vertex values, floats in range)",
        n, n - accepted, accepted);
}

// ---------------------------------------------------------------- textures
void test_textures() {
    namespace pt = guest::pica_tex;
    Rng r{4242};
    // GX -> PICA (texture.cpp, pica_tex.h), decoded back (texture.cpp) against GX's decode (texture.cpp of
    // the runtime): every format the conversion takes, POT sizes 8-256 with all their levels, and a few
    // sizes that aren't powers of two (padded)
    struct Pair {
        u32 gx;
        u8 pica;
        bool rgb_only;  // (L4/L8: alpha 255 on the PICA, the intensity on GX)
    };
    const Pair pairs[] = {{0, fm::TF_L4, true},  {0, fm::TF_LA4, false},  {1, fm::TF_L8, true},
                          {1, fm::TF_LA8, false}, {2, fm::TF_LA4, false},  {3, fm::TF_LA8, false},
                          {4, fm::TF_RGB565, false}, {5, fm::TF_RGBA8, false}, {6, fm::TF_RGBA8, false},
                          {14, fm::TF_RGBA5551, false}};
    u32 textures = 0, levels = 0, bad = 0;
    std::vector<u8> src;
    std::vector<u32> got, want;
    for (const Pair& pr : pairs)
        for (int t = 0; t < 12; t++) {
            const bool npot = t >= 10;
            const u32 w = npot ? 8 + r() % 60 : 8u << (r() % 6), h = npot ? 8 + r() % 60 : 8u << (r() % 6);
            u32 most = 1;  // (GX levels down to 1x1)
            while ((w >> most) | (h >> most))
                most++;
            const u32 nl = npot ? 1 : std::min(most, 1 + r() % 6);
            src.resize(g3::gx_texture_bytes(pr.gx, w, h, nl));
            for (u8& b : src)
                b = (u8)r();
            fm::Texture tex = {};
            tex.width = (u16)w, tex.height = (u16)h, tex.format = pr.pica, tex.levels = (u8)nl, tex.gx_format = (u8)pr.gx;
            g3::ConvertedTexture ct;
            std::string why;
            if (!g3::convert_gx_texture(tex, src.data(), (u32)src.size(), &ct, &why)) {
                expect(false, "convert GX %u -> PICA %u %ux%u: %s", pr.gx, pr.pica, w, h, why.c_str());
                continue;
            }
            textures++;
            u32 at = 0;
            for (u32 l = 0; l < ct.levels; l++) {
                const u32 lw = w >> l, lh = h >> l;
                const g3::TexLevel pl = g3::pica_level(ct.format, ct.width, ct.height, l);
                got.assign((size_t)pl.width * pl.height, 0);
                g3::pica_decode(ct.format, ct.texels.data() + pl.offset, pl.width, pl.height, got.data());
                want.assign((size_t)lw * lh, 0);
                gx_decode_texture(src.data() + at, pr.gx, lw, lh, want.data());
                at += g3::gx_level_bytes(pr.gx, lw, lh);
                levels++;
                u32 diff = 0;
                for (u32 y = 0; y < pl.height; y++)
                    for (u32 x = 0; x < pl.width; x++) {
                        // (the padding repeats the edge texels)
                        u32 c = want[std::min(y, lh - 1) * lw + std::min(x, lw - 1)];
                        if (pr.pica == fm::TF_RGBA5551) {  // (CMPR's colours at 5 bits, alpha at 1)
                            auto q = [](u32 v) { return pt::detail::x5(v >> 3); };
                            c = q(c & 255) | q(c >> 8 & 255) << 8 | q(c >> 16 & 255) << 16 | (c >> 31 ? 255u : 0u) << 24;
                        }
                        const u32 m = pr.rgb_only ? 0xFFFFFF : 0xFFFFFFFF;
                        diff += (got[y * pl.width + x] & m) != (c & m);
                    }
                if (diff && bad++ < 4)
                    log("selftest g3d: GX %u -> PICA %u %ux%u level %u: %u texels differ", pr.gx, pr.pica, w, h, l, diff);
            }
        }
    expect(bad == 0, "GX -> PICA -> RGBA8 equals GX's decode (%u levels differ)", bad);
    log("selftest g3d: textures: %u GX textures (%u levels) converted and decoded back: %s", textures, levels,
        bad ? "DIFFERENT" : "same as GX's decode");

    // CMPR -> ETC1/ETC1A4 (lossy): texture-like blocks (close endpoints), decoded, PSNR against GX's
    // decode; ETC1A4's alpha exact
    double se = 0;
    u64 samples = 0;
    u32 alpha_bad = 0;
    for (int t = 0; t < 60; t++) {
        const u32 w = 8u << (r() % 5), h = 8u << (r() % 5);
        const bool alpha = t % 3 == 0;
        src.resize(g3::gx_texture_bytes(14, w, h, 1));
        for (size_t i = 0; i < src.size(); i += 8) {
            u32 c0 = r() & 0xFFFF;
            const int cr = (c0 >> 11) & 31, cg = (c0 >> 5) & 63, cb = c0 & 31;
            u32 c1 = (u32)(std::min(31, cr + (int)(r() % 4)) << 11 | std::min(63, cg + (int)(r() % 8)) << 5 |
                           std::min(31, cb + (int)(r() % 4)));
            if ((c0 < c1) != (alpha && (i / 8) % 2))  // (alpha: some blocks in three-colour mode)
                std::swap(c0, c1);
            src[i] = (u8)(c0 >> 8), src[i + 1] = (u8)c0, src[i + 2] = (u8)(c1 >> 8), src[i + 3] = (u8)c1;
            for (int k = 4; k < 8; k++)
                src[i + k] = (u8)r();
        }
        fm::Texture tex = {};
        tex.width = (u16)w, tex.height = (u16)h, tex.levels = 1, tex.gx_format = 14;
        tex.format = alpha ? fm::TF_ETC1A4 : fm::TF_ETC1;
        g3::ConvertedTexture ct;
        if (!g3::convert_gx_texture(tex, src.data(), (u32)src.size(), &ct, nullptr))
            continue;
        got.assign((size_t)w * h, 0), want.assign((size_t)w * h, 0);
        g3::pica_decode(ct.format, ct.texels.data(), w, h, got.data());
        gx_decode_texture(src.data(), 14, w, h, want.data());
        for (size_t i = 0; i < got.size(); i++) {
            for (int k = 0; k < 3; k++) {
                const int e = (int)(got[i] >> (8 * k) & 255) - (int)(want[i] >> (8 * k) & 255);
                se += e * e, samples++;
            }
            if (alpha)
                alpha_bad += (got[i] >> 24) != (want[i] >> 24);
        }
    }
    const double psnr = 10 * std::log10(255.0 * 255.0 * (double)samples / se);
    expect(psnr >= 35.0 && alpha_bad == 0, "CMPR as ETC1: %.1f dB, %u alpha differences", psnr, alpha_bad);
    log("selftest g3d: textures: CMPR -> ETC1/ETC1A4 decoded: %.1f dB against GX's decode, %u alpha differences", psnr,
        alpha_bad);

    // the PICA tiling: images tiled by pica_tex.h's from_rgba, decoded back, for the lossless formats
    const u8 lossless[] = {fm::TF_RGBA8, fm::TF_LA8, fm::TF_LA4, fm::TF_L8};
    u32 tiled_bad = 0, tiled = 0;
    for (u8 f : lossless)
        for (int t = 0; t < 8; t++) {
            const u32 w = 8u << (r() % 6), h = 8u << (r() % 6);
            std::vector<u32> img((size_t)w * h);
            for (u32& c : img) {
                c = r();
                if (f == fm::TF_LA8 || f == fm::TF_L8)  // (one luminance: R = G = B)
                    c = (c & 0xFF0000FF) | (c & 255) << 8 | (c & 255) << 16;
                if (f == fm::TF_LA4)
                    c = ((c & 15) * 17) * 0x010101u | (((c >> 24) & 15) * 17) << 24;
                if (f == fm::TF_L8)
                    c |= 0xFF000000u;
            }
            std::vector<u8> out(g3::pica_level_bytes(f, w, h));
            const pt::Fmt pf = f == fm::TF_RGBA8 ? pt::RGBA8 : f == fm::TF_LA8 ? pt::LA8 : f == fm::TF_LA4 ? pt::LA4 : pt::L8;
            pt::from_rgba(img.data(), w, h, out.data(), w, h, pf, false);
            got.assign((size_t)w * h, 0);
            g3::pica_decode(f, out.data(), w, h, got.data());
            tiled++;
            tiled_bad += got != img;
        }
    expect(tiled_bad == 0, "PICA tiling round trip (%u of %u differ)", tiled_bad, tiled);
    log("selftest g3d: textures: %u images tiled (pica_tex from_rgba) and decoded back: %s", tiled,
        tiled_bad ? "DIFFERENT" : "same");
    expect(g3::pica_max_levels(1024, 8) == 1 && g3::pica_max_levels(64, 64) == 4 && g3::pica_max_levels(256, 32) == 3,
           "level counts");
}

// ---------------------------------------------------------------- animation curves
void test_anim() {
    auto eval = [](u8 interp, std::vector<fm::Key> ks, f32 f) {
        fm::Track t = {};
        t.interp = interp, t.first_key = 0, t.n_keys = (u32)ks.size();
        return g3::eval_track(t, ks.data(), f);
    };
    auto near = [](f32 a, f32 b) { return std::fabs(a - b) < 1e-4f; };
    // hermite, flat ends: smoothstep
    expect(near(eval(fm::IP_HERMITE, {{0, 0, 0}, {10, 10, 0}}, 5), 5.0f), "hermite midpoint");
    expect(near(eval(fm::IP_HERMITE, {{0, 0, 0}, {10, 10, 0}}, 2.5f), 1.5625f), "hermite at t=1/4: 10 (3 - 1/2) / 16");
    // hermite with slopes matching the line: the line
    expect(near(eval(fm::IP_HERMITE, {{0, 0, 1}, {10, 10, 1}}, 3.7f), 3.7f), "hermite along a line");
    // keys (0, 1, slope 2), (4, 3, slope -1) at frame 1: t = 1/4, h00 = 27/32, h10 = 9/64, h01 = 5/32,
    // h11 = -3/64; v = 27/32 + (9/64)(4)(2) + (5/32)(3) + (-3/64)(4)(-1) = 2.625
    expect(near(eval(fm::IP_HERMITE, {{0, 1, 2}, {4, 3, -1}}, 1), 2.625f), "hermite with slopes: %f",
           (double)eval(fm::IP_HERMITE, {{0, 1, 2}, {4, 3, -1}}, 1));
    expect(near(eval(fm::IP_LINEAR, {{0, 0, 0}, {4, 8, 0}}, 1), 2.0f), "linear");
    expect(near(eval(fm::IP_STEP, {{0, 5, 0}, {2, 9, 0}}, 1.99f), 5.0f) && near(eval(fm::IP_STEP, {{0, 5, 0}, {2, 9, 0}}, 2), 9.0f),
           "step");
    expect(near(eval(fm::IP_HERMITE, {{2, 7, 0}, {4, 9, 0}}, -5), 7.0f) && near(eval(fm::IP_HERMITE, {{2, 7, 0}, {4, 9, 0}}, 50), 9.0f),
           "before the first key, after the last");
    // a jump: two keys on one frame, the later one from that frame on
    expect(near(eval(fm::IP_LINEAR, {{0, 0, 0}, {5, 5, 0}, {5, 100, 0}, {10, 110, 0}}, 5), 100.0f) &&
               near(eval(fm::IP_LINEAR, {{0, 0, 0}, {5, 5, 0}, {5, 100, 0}, {10, 110, 0}}, 4.5f), 4.5f),
           "a jump");
    expect(near(eval(fm::IP_HERMITE, {{3, 42, 5}}, 99), 42.0f), "one key: constant");
    // loop and clamp
    expect(near(g3::anim_frame(250, 100, true), 50) && near(g3::anim_frame(-30, 100, true), 70) &&
               near(g3::anim_frame(300, 100, true), 0) && near(g3::anim_frame(250, 100, false), 100) &&
               near(g3::anim_frame(-3, 100, false), 0),
           "anim frames");
    // matrices: rotate 90 degrees about y takes x to -z; T R S
    const f32 s[3] = {2, 2, 2}, rot[3] = {0, 90, 0}, tr[3] = {1, 2, 3};
    const engine::math::Mtx34 m = g3::srt_matrix(s, rot, tr);
    const Vec3 v = engine::math::transform(m, {1, 0, 0});
    expect(near(v.x, 1) && near(v.y, 2) && near(v.z, 1), "srt matrix: (1,0,0) -> (%f %f %f)", (double)v.x, (double)v.y,
           (double)v.z);
    f32 tm[2][4];
    g3::tex_srt_matrix(0, 1, 1, 0, 0, 0, tm);
    expect(near(tm[0][0], 1) && near(tm[0][3], 0) && near(tm[1][1], 1) && near(tm[1][3], 0), "texture SRT identity");
    g3::tex_srt_matrix(0, 1, 1, 0, 0.25f, 0.5f, tm);
    expect(near(tm[0][3], -0.25f) && near(tm[1][3], 0.5f), "texture SRT translation (s -= ts, t += tt)");
    g3::tex_srt_matrix(0, 2, 2, 0, 0, 0, tm);
    expect(near(tm[0][0], 2) && near(tm[0][3], 0) && near(tm[1][3], -1), "texture SRT scale about (0, 1)");
    log("selftest g3d: animation: hermite (hand-computed 1.5625, 2.625, the line), linear, step, jumps, ends, loops, "
        "node and texture SRT matrices checked");
}

// ---------------------------------------------------------------- materials
void test_materials(const g3::Package& pkg, const Scene& sc) {
    g3::Material m;
    g3::build_material(pkg, sc.mat_ground, &m);
    const engine::gx::CombinerStage& s0 = m.comb.stage[0];
    expect(m.comb.n_stages == 1 && s0.func_rgb == engine::gx::FUNC_MODULATE &&
               ((s0.src_rgb[0] == engine::gx::SRC_TEXTURE0 && s0.src_rgb[1] == engine::gx::SRC_PRIMARY_COLOR) ||
                (s0.src_rgb[1] == engine::gx::SRC_TEXTURE0 && s0.src_rgb[0] == engine::gx::SRC_PRIMARY_COLOR)),
           "vertex colour x texture: one MODULATE stage (func %u, src %u %u)", s0.func_rgb, s0.src_rgb[0], s0.src_rgb[1]);
    expect(m.unit[0].texture == sc.tex_cmpr && m.unit[0].wrap_s == 2 && m.unit[0].mip == 2 && m.unit[0].min_linear &&
               m.unit[0].source == g3::TexCoordSource::UV0 && m.unit[1].texture == fm::NONE16,
           "texture unit 0");
    expect(m.light.lit && m.light.mat_from_vertex && m.light.light_mask == 1 && m.light.light_set == -1, "lighting");
    expect(m.pixel.depth_test && m.pixel.depth_func == 5 && m.pixel.depth_write && !m.pixel.blend && !m.pixel.alpha_test &&
               m.pixel.cull == 1,
           "pixel state (GX_CULL_BACK: the PICA's GPU_CULL_FRONT_CCW, as gx_pica.cpp maps it)");
    expect(!m.translucent && !m.fog && m.approximated == 0, "pass, fog");
    g3::build_material(pkg, sc.mat_xlu, &m);
    expect(m.translucent && m.pixel.blend && m.pixel.blend_src == 6 && m.pixel.blend_dst == 7, "translucent blending");
    g3::build_material(pkg, sc.mat_scroll, &m);
    // an L8 texture: what read its alpha reads red
    expect(m.lum_units == 1 && m.fog && m.unit[0].matrix == 0, "luminance unit, fog, matrix");
    bool alpha_read = false;
    for (int k = 0; k < 3; k++)
        alpha_read |= m.comb.stage[0].src_a[k] == engine::gx::SRC_TEXTURE0 && m.comb.stage[0].op_a[k] == engine::gx::OP_A_SRC_ALPHA;
    expect(!alpha_read, "luminance remap: no alpha read of the L8 texture left");
    g3::build_material(pkg, sc.mat_tree, &m);
    expect(m.light.light_set == 0 && !m.prelit, "light set");
    // prelit (its lighting in the vertex colours): unlit, colour and alpha the vertex's
    {
        Scene s2;
        s2.p.mats[s2.mat_tree].flags |= fm::MAT_PRELIT;
        std::string error;
        g3::Package* q = load_bytes(s2.p.bytes(), &error);
        expect(q != nullptr, "a prelit material loads (%s)", error.c_str());
        if (q) {
            g3::build_material(*q, s2.mat_tree, &m);
            expect(m.prelit && !m.light.lit && !m.light.alpha_lit && m.light.mat_from_vertex &&
                       m.light.alpha_mat_from_vertex && m.light.light_set == -1,
                   "prelit: unlit, the vertex's colour and alpha");
            delete q;
        }
    }
    log("selftest g3d: materials: combiners, units, lighting (prelit too), pixel state, luminance remap checked");
}

// ---------------------------------------------------------------- the world
void test_world(const g3::Package& pkg, const Scene& sc) {
    g3::World w(pkg);
    g3::Camera cam;
    cam.eye = {0, 100, 0}, cam.target = {0, 100, -1000}, cam.aspect = 5.0f / 3.0f, cam.near_ = 10, cam.far_ = 70000;
    w.update(0);
    w.set_camera(cam);
    g3::Packet opa, xlu;
    w.build_packet(g3::Pass::OPAQUE, &opa);
    w.build_packet(g3::Pass::TRANSLUCENT, &xlu);
    const g3::WorldStats st = w.stats();
    // looking -z from the middle (45 x 5:3): the far row (z = -1500 .. -500) whole, of the middle row
    // the middle cell (its neighbours lie wholly outside the side planes there), the near row behind
    expect(st.cells_total == 9 && st.cells_visible == 4, "culling: %u of %u cells", st.cells_visible, st.cells_total);
    // instances: 6 trees at z = -200 .. -1700, the last beyond its own draw distance (1000)
    expect(st.instances_total == 6 && st.instances_visible == 5, "instances: %u of %u", st.instances_visible,
           st.instances_total);
    // the sky first, flagged, centred on the eye
    expect(!opa.items.empty() && (opa.items[0].flags & g3::DRAW_SKY) && opa.items[0].batch == pkg.models()[sc.model_sky].first_batch,
           "the sky first");
    if (!opa.items.empty()) {
        const f32* sm = &opa.matrices[12 * opa.items[0].matrix];
        expect(sm[3] == 0 && sm[7] == 100 && sm[11] == 0, "the sky follows the eye (%f %f %f)", (double)sm[3],
               (double)sm[7], (double)sm[11]);
    }
    // levels: the cells' boxes at distance < 1500 at level 0, the farther ones (none here) at 1; the
    // trees beyond 800 as tree_L
    u32 lod0 = 0, lod1 = 0, tree_h = 0, tree_l = 0;
    const fm::Model& th = pkg.models()[sc.model_tree];
    const fm::Model& tl = pkg.models()[sc.model_tree_l];
    for (const g3::DrawItem& it : opa.items) {
        lod0 += std::count(sc.cell_lod0.begin(), sc.cell_lod0.end(), it.batch) > 0;
        lod1 += std::count(sc.cell_lod1.begin(), sc.cell_lod1.end(), it.batch) > 0;
        tree_h += it.batch >= th.first_batch && it.batch < th.first_batch + th.n_batches;
        tree_l += it.batch >= tl.first_batch && it.batch < tl.first_batch + tl.n_batches;
    }
    expect(lod0 == 4 && lod1 == 0, "cell levels near: %u at 0, %u at 1", lod0, lod1);
    expect(tree_h == 2 * 2 && tree_l == 3 * 2, "tree levels: %u tree_H items, %u tree_L (trees at 200, 500 near)", tree_h,
           tree_l);
    // front to back: the opaque items' cells by distance (the sky aside)
    // translucent: priority first (the near overlay at -1 first), then back to front
    expect(xlu.items.size() == 2 && xlu.items[0].batch == sc.xlu_front_batch && xlu.items[1].batch == sc.xlu_batch,
           "translucent order: priority, then back to front (%zu items)", xlu.items.size());
    expect(st.items_opaque == opa.items.size() && st.items_translucent == xlu.items.size() && st.triangles ==
           (opa.items.size() + xlu.items.size()) * 12, "stats: %u + %u items, %u triangles", st.items_opaque,
           st.items_translucent, st.triangles);
    // a cell's batches by their own boxes: turned round, the eye's own cell is still drawn, but not
    // its two overlays, which are behind it now
    {
        g3::World w2(pkg);
        g3::Camera c2 = cam;
        c2.target = {0, 100, 1000};
        w2.update(0);
        w2.set_camera(c2);
        g3::Packet o2, x2;
        w2.build_packet(g3::Pass::OPAQUE, &o2);
        w2.build_packet(g3::Pass::TRANSLUCENT, &x2);
        const bool middle = std::any_of(o2.items.begin(), o2.items.end(), [&](const g3::DrawItem& it) { return it.batch == sc.cell_lod0[4]; });
        expect(middle && x2.items.empty(), "batches by their boxes: the eye's cell %s, %zu of its overlays behind the eye drawn",
               middle ? "drawn" : "not drawn", x2.items.size());
    }
    // a frozen world (no update, the camera still: not culled again) counts each pass once
    {
        g3::Packet o2, x2;
        w.build_packet(g3::Pass::OPAQUE, &o2);
        w.build_packet(g3::Pass::TRANSLUCENT, &x2);
        expect(w.stats().triangles == st.triangles && o2.items.size() == opa.items.size(),
               "a frozen world's next frame: %u triangles (%u before)", w.stats().triangles, st.triangles);
    }
    // the opaque order: front to back without the grouping by material, and with it each material's
    // items together, each group front to back (checked with the camera moved so the cells' distances
    // differ)
    {
        g3::Camera c2 = cam;
        c2.eye = {0, 100, 1400}, c2.target = {0, 100, 0};
        g3::Packet p2;
        auto cell_items = [&](std::vector<f32>* d, std::vector<u16>* mat) {
            d->clear(), mat->clear();
            for (const g3::DrawItem& it : p2.items) {
                const auto pos = std::find(sc.cell_lod0.begin(), sc.cell_lod0.end(), it.batch);
                const auto pos1 = std::find(sc.cell_lod1.begin(), sc.cell_lod1.end(), it.batch);
                if (pos != sc.cell_lod0.end() || pos1 != sc.cell_lod1.end()) {
                    const fm::Cell& c = pkg.cells()[(u32)((pos != sc.cell_lod0.end() ? pos - sc.cell_lod0.begin() : pos1 - sc.cell_lod1.begin()))];
                    const f32 dz = std::max(std::max(c.bbox[2] - 1400.0f, 1400.0f - c.bbox[5]), 0.0f);
                    const f32 dx = std::max(std::max(c.bbox[0], -c.bbox[3]), 0.0f);
                    const f32 dy = std::max(std::max(c.bbox[1] - 100.0f, 100.0f - c.bbox[4]), 0.0f);
                    d->push_back(std::sqrt(dx * dx + dy * dy + dz * dz));
                    mat->push_back(pkg.batches()[it.batch].material);
                }
            }
        };
        std::vector<f32> d;
        std::vector<u16> dm;
        w.set_features(g3::WF_ALL & ~g3::WF_MATERIAL_SORT);
        w.set_camera(c2);
        w.build_packet(g3::Pass::OPAQUE, &p2);
        cell_items(&d, &dm);
        expect(d.size() == 9 && std::is_sorted(d.begin(), d.end()), "opaque cells front to back (%zu cells)", d.size());
        w.set_features(g3::WF_ALL);
        w.build_packet(g3::Pass::OPAQUE, &p2);
        cell_items(&d, &dm);
        bool grouped = d.size() == 9;
        for (size_t i = 1; i < dm.size(); i++) {
            if (dm[i] != dm[i - 1] && std::find(dm.begin(), dm.begin() + (long)i, dm[i]) != dm.begin() + (long)i)
                grouped = false;  // (a material seen before, after another)
            if (dm[i] == dm[i - 1] && d[i] < d[i - 1])
                grouped = false;  // (a group not front to back)
        }
        expect(grouped, "opaque cells grouped by material, each group front to back (%zu cells)", d.size());
        u32 changes = 0, changes_before = 0;
        for (size_t i = 1; i < dm.size(); i++)
            changes += dm[i] != dm[i - 1];
        w.set_features(g3::WF_ALL & ~g3::WF_MATERIAL_SORT);
        w.build_packet(g3::Pass::OPAQUE, &p2);
        cell_items(&d, &dm);
        for (size_t i = 1; i < dm.size(); i++)
            changes_before += dm[i] != dm[i - 1];
        expect(changes < changes_before, "fewer material changes grouped (%u) than front to back (%u)", changes,
               changes_before);
        w.set_features(g3::WF_ALL);
        w.build_packet(g3::Pass::OPAQUE, &p2);
        // from z = 1400: the far row (z = -1000, at 1900) is at its level 1
        u32 far_l1 = 0;
        for (const g3::DrawItem& it : p2.items)
            far_l1 += it.batch == sc.cell_lod1[0] || it.batch == sc.cell_lod1[1] || it.batch == sc.cell_lod1[2];
        expect(far_l1 == 3, "far cells at level 1 (%u)", far_l1);
        // draw distance and hidden groups
        w.set_draw_distance(1000, 0);
        w.build_packet(g3::Pass::OPAQUE, &p2);
        // (from z = 1400: the near row's three, and the middle row's middle cell at 900)
        expect(w.stats().cells_visible == 4, "cells' draw distance: %u visible", w.stats().cells_visible);
        w.set_draw_distance(0, 0);
        w.set_group_visible("grp_corner", false);
        w.build_packet(g3::Pass::OPAQUE, &p2);
        expect(w.stats().cells_visible == 8, "a hidden group: %u visible", w.stats().cells_visible);
        w.set_group_visible("grp_corner", true);
        w.set_group_visible("tree_H", false);
        w.build_packet(g3::Pass::OPAQUE, &p2);
        expect(w.stats().instances_visible == 0, "hidden model: %u instances", w.stats().instances_visible);
        w.set_group_visible("tree_H", true);
        w.set_lod_scale(10);
        w.set_camera(cam);
        w.build_packet(g3::Pass::OPAQUE, &p2);
        u32 tl2 = 0;
        for (const g3::DrawItem& it : p2.items)
            tl2 += it.batch >= tl.first_batch && it.batch < tl.first_batch + tl.n_batches;
        expect(tl2 == 0, "LOD scale 10: no tree_L (%u)", tl2);
        w.set_lod_scale(1);
    }
    // animation: the water's texture matrix scrolls (linear 0 -> 1 over 100 frames, looping), its C0
    // red steps (0, 255, 128, 50 a frame) and goes into the combiners' constant
    w.set_camera(cam);
    w.update(25);
    g3::Packet pa;
    w.build_packet(g3::Pass::OPAQUE, &pa);
    const g3::MatDynamic* md = nullptr;
    for (const g3::MatDynamic& d : pa.mat_states)
        if (d.material == sc.mat_scroll)
            md = &d;
    expect(md != nullptr, "the animated material's dynamic block is in the packet");
    if (md) {
        expect(std::fabs(md->tex_mtx[0][0][3] - (-0.25f)) < 1e-4f, "scroll at frame 25: translate s 0.25 -> %f",
               (double)md->tex_mtx[0][0][3]);
        expect(md->tev_reg[1][0] == 255, "colour step at frame 25 (frame 1 of 4): %d", md->tev_reg[1][0]);
        // (C0 x ras: one MODULATE with C0 (255, 100, 100) as the stage's constant)
        expect((md->comb.stage[0].konst & 0xFFFFFF) == 0x6464FF, "re-translated combiners use the animated register "
               "(konst %08X)", md->comb.stage[0].konst);
        u32 uses = 0;
        for (const g3::DrawItem& it : pa.items)
            uses += it.mat_state != 0xFFFF && pa.mat_states[it.mat_state].material == sc.mat_scroll;
        expect(uses == 1, "the water's item points at it (%u)", uses);
    }
    w.update(27);
    w.build_packet(g3::Pass::OPAQUE, &pa);
    for (const g3::MatDynamic& d : pa.mat_states)
        if (d.material == sc.mat_scroll)
            expect(d.tev_reg[1][0] == 50 && std::fabs(d.tex_mtx[0][0][3] + 0.27f) < 1e-4f, "frame 27: %d", d.tev_reg[1][0]);
    // the animated tree: its trunk rotated about z at frame 15 (hermite 0 -> 10 over 30: 5 degrees at 15)
    {
        const fm::Model& m = pkg.models()[sc.model_tree];
        w.update(15);
        w.build_packet(g3::Pass::OPAQUE, &pa);
        bool found = false;
        for (const g3::DrawItem& it : pa.items)
            if (it.batch == m.first_batch && !found) {
                const f32* mm = &pa.matrices[12 * it.matrix];
                const f32 ang = std::atan2(mm[4], mm[0]) * 180.0f / 3.14159265f;
                // (the nearest tree is instance 0: z = -200)
                if (std::fabs(mm[11] + 200) < 1e-3f) {
                    found = true;
                    expect(std::fabs(ang - 5.0f) < 1e-3f, "the swaying tree at frame 15: %f degrees", (double)ang);
                }
            }
        expect(found, "the animated tree is drawn");
        // its crown is a billboard turned to the eye: its z axis points at (0, 100, 0) in xz
        for (const g3::DrawItem& it : pa.items)
            if (it.batch == m.first_batch + 1) {
                const f32* mm = &pa.matrices[12 * it.matrix];
                const Vec3 pos = {mm[3], mm[7], mm[11]}, zax = {mm[2], mm[6], mm[10]};
                Vec3 to = Vec3{0, 100, 0} - pos;
                to.y = 0;
                to = engine::math::normalize(to);
                expect(std::fabs(engine::math::dot(zax, to) - 1) < 1e-3f && std::fabs(zax.y) < 1e-5f,
                       "billboard faces the eye");
                break;
            }
    }
    // the scaled camera: guest units = k x package units
    {
        g3::Camera c = cam;
        const g3::Camera s = g3::scaled(c, 2.5f);
        const engine::math::Mtx34 v1 = g3::look_at(c), v2 = g3::look_at(s);
        expect(std::fabs(v2.m[1][3] - 2.5f * v1.m[1][3]) < 1e-3f && std::fabs(v2.m[0][0] - v1.m[0][0]) < 1e-6f,
               "scaled camera");
        const Vec3 p = engine::math::transform(v1, {0, 100, -500});
        expect(std::fabs(p.z + 500) < 1e-3f && std::fabs(p.x) < 1e-3f && std::fabs(p.y) < 1e-3f, "look-at: -z forward");
    }
    log("selftest g3d: world: culling (%u/%u cells, %u/%u instances; a cell's batches by their boxes), levels, sky, opaque front to back, "
        "translucent by priority then back to front, draw distance, groups, LOD scale, material and node "
        "animation, billboards checked",
        st.cells_visible, st.cells_total, st.instances_visible, st.instances_total);
}

// ---------------------------------------------------------------- visibility rows, detail batches
// The test scene's 3x3 grid with visibility rows: from the middle cell (4) cells 1 and 6 hidden; the
// eye's row applies within pvs_max_height (500) over the ground (the scene's, or the cell box's floor,
// -60), not above it, not outside the grid; WF_VISIBILITY off: none. Then detail batches: the far
// overlay (the middle cell's, 300 away) flagged as detail, detail_distance 150: culled from the
// middle (204 away), drawn from 63 away, and with the LOD scale (2) or the feature off.
void test_visibility() {
    Scene sc;
    sc.p.env.pvs_row_bytes = 2, sc.p.env.pvs_max_height = 500;
    sc.p.cvis.assign(18, 0xFF);
    sc.p.cvis[4 * 2 + 0] &= (u8) ~(1u << 1);  // row 4: cell 1 hidden ...
    sc.p.cvis[4 * 2 + 0] &= (u8) ~(1u << 6);  // ... and cell 6
    sc.p.batches[sc.xlu_batch].flags |= fm::BATCH_DETAIL;
    sc.p.env.detail_distance = 150;
    std::string error;
    g3::Package* pkg = load_bytes(sc.p.bytes(), &error);
    expect(pkg != nullptr, "a package with visibility rows and detail batches loads (%s)", error.c_str());
    if (!pkg)
        return;
    expect(pkg->visibility_row(4) && pkg->visibility_row(4)[0] == (u8)~(1u << 1 | 1u << 6) && !pkg->visibility_row(9),
           "the rows as written");
    g3::World w(*pkg);
    g3::Packet p;
    g3::Camera cam;
    // looking down -z from the middle, wide: every cell in the frustum but the near row behind
    cam.eye = {0, 100, 0}, cam.target = {0, 100, -1000}, cam.fovy_deg = 90, cam.aspect = 2.5f, cam.near_ = 10;
    cam.far_ = 70000;
    auto frame = [&](f32 ground) {
        w.set_camera(cam, ground);
        w.build_packet(g3::Pass::OPAQUE, &p);
        return w.stats();
    };
    w.update(0);
    w.set_features(g3::WF_ALL & ~g3::WF_VISIBILITY);
    const g3::WorldStats all = frame(NAN);
    w.set_features(g3::WF_ALL);
    const g3::WorldStats box = frame(NAN);       // (the floor: the cell's box, -60: 160 under the eye)
    const g3::WorldStats ground = frame(80);     // the ground 20 under the eye
    cam.eye.y = 600, cam.target.y = 600;
    const g3::WorldStats high = frame(80);       // 520 over the ground: the rows don't apply
    const g3::WorldStats high_box = frame(NAN);  // 660 over the box's floor
    cam.eye = {0, 100, -1400}, cam.target = {0, 100, -2400};
    const g3::WorldStats edge = frame(NAN);      // cell 1 (no row hides anything there)
    cam.eye = {0, 100, -1600}, cam.target = {0, 100, -2600};
    const g3::WorldStats outside = frame(NAN);   // beyond the grid
    // (cell 1 is in the frustum from the middle; cell 6 behind the eye: only 1 counts as hidden)
    expect(all.pvs_cell == -1 && all.cells_pvs_hidden == 0, "rows off: none applied");
    expect(box.pvs_cell == 4 && box.cells_pvs_hidden == 1 && box.cells_visible == all.cells_visible - 1,
           "the middle cell's row (box floor): %d, %u hidden, %u of %u visible", box.pvs_cell, box.cells_pvs_hidden,
           box.cells_visible, all.cells_visible);
    expect(ground.pvs_cell == 4 && ground.cells_pvs_hidden == 1, "the middle cell's row (the ground given)");
    expect(high.pvs_cell == -1 && high_box.pvs_cell == -1 && high.cells_pvs_hidden == 0,
           "an eye higher than the rows' height: none (%d, %d)", high.pvs_cell, high_box.pvs_cell);
    expect(edge.pvs_cell == 1 && edge.cells_pvs_hidden == 0, "cell 1's row: %d, %u hidden", edge.pvs_cell,
           edge.cells_pvs_hidden);
    expect(outside.pvs_cell == -1, "outside the grid: none (%d)", outside.pvs_cell);
    // detail batches (translucent pass): the overlay at z = -300 (300 from the eye), the other at +300
    cam.eye = {0, 100, 0}, cam.target = {0, 100, -1000};
    auto has = [&](u16 batch) {
        for (const g3::DrawItem& it : p.items)
            if (it.batch == batch)
                return true;
        return false;
    };
    w.set_camera(cam);
    w.build_packet(g3::Pass::TRANSLUCENT, &p);
    const bool far_culled = !has(sc.xlu_batch) && w.stats().details_culled == 1;
    cam.eye.z = -150, cam.target.z = -1150;
    w.set_camera(cam);
    w.build_packet(g3::Pass::TRANSLUCENT, &p);
    const bool near_drawn = has(sc.xlu_batch) && w.stats().details_culled == 0;
    cam.eye.z = 0, cam.target.z = -1000;
    w.set_camera(cam);
    w.set_lod_scale(2);
    w.build_packet(g3::Pass::TRANSLUCENT, &p);
    const bool scaled = has(sc.xlu_batch);
    w.set_lod_scale(1);
    w.set_features(g3::WF_ALL & ~g3::WF_DETAIL);
    w.build_packet(g3::Pass::TRANSLUCENT, &p);
    const bool off = has(sc.xlu_batch);
    w.set_features(g3::WF_ALL);
    expect(far_culled && near_drawn && scaled && off,
           "detail batch: culled beyond the distance %d, drawn nearer %d, with the LOD scale %d, the feature off %d",
           far_culled, near_drawn, scaled, off);
    log("selftest g3d: visibility rows: the middle cell's row hides %u cell (%u of %u visible, %u without), not "
        "above the rows' height, not outside the grid; detail batches culled by distance (the LOD scale, the switch)",
        box.cells_pvs_hidden, box.cells_visible, box.cells_total, all.cells_visible);
    delete pkg;
}

// ---------------------------------------------------------------- the animation rate, the combiners kept
// Three swaying trees in view, 200, 2500 and 8000 away (their meshes 61 units about their origin:
// 177, 14 and 4.4 pixels across on the 3DS's screen at 45 degrees; their crowns move a fraction of a
// pixel a frame): over
// 12 updates the near one posed at each, the middle one every second, the far one every fourth; one
// behind the eye never; WF_ANIM_LOD off: all at each. Props carried out of view by their animation,
// or under a pixel across, not drawn. The water's colour (C0 red, steps 0, 255, 128, 50 a frame): translated at each new
// value in the first loop, then found among the kept translations; no change, no translation.
void test_anim_rate() {
    Scene sc;
    sc.p.insts.clear();
    const f32 zs[4] = {-200, -2500, -8000, 400};  // (the last behind the eye)
    for (f32 z : zs) {
        fm::Instance in = {};
        in.model = sc.model_tree, in.flags = fm::INST_ANIMATED, in.anim = 2;  // tree_sway
        const f32 mtx[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, z}};
        memcpy(in.mtx, mtx, sizeof(mtx));
        sc.p.insts.push_back(in);
    }
    sc.p.models[sc.model_tree].lod_next = fm::NONE16;  // (one model at every distance)
    std::string error;
    g3::Package* pkg = load_bytes(sc.p.bytes(), &error);
    expect(pkg != nullptr, "the swaying trees load (%s)", error.c_str());
    if (!pkg)
        return;
    const u16 trunk = pkg->models()[sc.model_tree].first_batch;
    g3::Camera cam;
    cam.eye = {0, 20, 0}, cam.target = {0, 20, -1000}, cam.aspect = 5.0f / 3.0f, cam.near_ = 10, cam.far_ = 70000;
    for (int pass = 0; pass < 2; pass++) {
        g3::World w(*pkg);
        if (pass)
            w.set_features(g3::WF_ALL & ~g3::WF_ANIM_LOD);
        g3::Packet p;
        f32 last[3] = {NAN, NAN, NAN};
        u32 changed[3] = {}, poses = 0;
        for (int f = 0; f < 13; f++) {
            w.update((f32)f);
            w.set_camera(cam);
            w.build_packet(g3::Pass::OPAQUE, &p);
            if (f)
                poses += w.stats().poses;
            for (const g3::DrawItem& it : p.items) {
                if (it.batch != trunk)
                    continue;
                const f32* m = &p.matrices[12 * it.matrix];
                for (int i = 0; i < 3; i++)
                    if (std::fabs(m[11] - zs[i]) < 1e-3f) {
                        if (f && m[1] != last[i])
                            changed[i]++;
                        last[i] = m[1];
                    }
            }
        }
        if (!pass) {
            // (the first update poses each, the second measures their speed: 0)
            expect(changed[0] == 12 && changed[1] >= 6 && changed[1] <= 7 && changed[2] >= 3 && changed[2] <= 4,
                   "animation rate by size: %u, %u, %u poses of 12 (want 12, 6, 3), %u in all", changed[0],
                   changed[1], changed[2], poses);
            log("selftest g3d: animation rate: trees 177, 14 and 4.4 pixels across posed at %u, %u, %u of 12 "
                "updates; one behind the eye never",
                changed[0], changed[1], changed[2]);
        } else {
            expect(changed[0] == 12 && changed[1] == 12 && changed[2] == 12 && poses == 36,
                   "animation rate off: %u, %u, %u of 12, %u in all", changed[0], changed[1], changed[2], poses);
        }
    }
    delete pkg;
    // animated props drawn where their meshes are as posed: a tree whose animation carries it 3000
    // units to the side (out of view, though its sphere at its matrix is in view), one 40,000 away
    // (0.9 pixels across); WF_DETAIL off: both drawn
    {
        Scene s3;
        s3.p.insts.clear();
        fm::Anim a = {};
        a.name = s3.p.strs.add("tree_away"), a.frames = 10, a.loop = 1;
        a.first_track = (u32)s3.p.tracks.size(), a.n_tracks = 1;
        s3.p.add_track(fm::TT_NODE_TRANS_X, fm::IP_STEP, 0, s3.model_tree, 0, {{0, 3000, 0}});
        s3.p.anims.push_back(a);
        const u16 away = (u16)(s3.p.anims.size() - 1);
        const f32 at[2][2] = {{0, -500}, {0, -40000}};
        const u16 anims[2] = {away, 2};
        for (int i = 0; i < 2; i++) {
            fm::Instance in = {};
            in.model = s3.model_tree, in.flags = fm::INST_ANIMATED, in.anim = anims[i];
            const f32 mtx[3][4] = {{1, 0, 0, at[i][0]}, {0, 1, 0, 0}, {0, 0, 1, at[i][1]}};
            memcpy(in.mtx, mtx, sizeof(mtx));
            s3.p.insts.push_back(in);
        }
        s3.p.models[s3.model_tree].lod_next = fm::NONE16;
        g3::Package* q = load_bytes(s3.p.bytes(), &error);
        expect(q != nullptr, "the props load (%s)", error.c_str());
        if (q) {
            g3::World w(*q);
            g3::Packet p;
            w.update(0);
            w.set_camera(cam);
            w.build_packet(g3::Pass::OPAQUE, &p);
            const g3::WorldStats on = w.stats();
            w.set_features(g3::WF_ALL & ~g3::WF_DETAIL);
            w.build_packet(g3::Pass::OPAQUE, &p);
            const g3::WorldStats off = w.stats();
            expect(on.instances_visible == 0 && on.instances_small == 2 && off.instances_visible == 2,
                   "props by where their meshes are and their size: %u drawn, %u not (%u with the switch off)",
                   on.instances_visible, on.instances_small, off.instances_visible);
            delete q;
        }
    }
    // the combiners kept: the water (the middle cell, in view from the middle)
    Scene s2;
    pkg = load_bytes(s2.p.bytes(), &error);
    if (!pkg)
        return;
    g3::World w(*pkg);
    g3::Packet p;
    cam.eye = {0, 100, 0}, cam.target = {0, 100, -1000};
    u32 made = 0, kept = 0;
    std::vector<u32> konst;
    for (int f = 0; f < 8; f++) {
        w.update((f32)f);
        w.set_camera(cam);
        w.build_packet(g3::Pass::OPAQUE, &p);
        made += w.stats().translations, kept += w.stats().translations_cached;
        for (const g3::MatDynamic& d : p.mat_states)
            if (d.material == s2.mat_scroll)
                konst.push_back(d.comb.stage[0].konst & 0xFF);
    }
    w.update(7.5f);  // (a step track: the same value as at 7)
    w.set_camera(cam);
    // (as the island's frame line reads them, after the update and before the draws: the last frame's,
    // 50 found kept)
    const g3::WorldStats last = w.stats();
    w.build_packet(g3::Pass::OPAQUE, &p);
    const u32 still = w.stats().translations + w.stats().translations_cached;
    const std::vector<u32> want = {0, 255, 128, 50, 0, 255, 128, 50};
    expect(made == 4 && kept == 4 && still == 0 && konst == want,
           "combiners: %u translated, %u kept (want 4, 4), %u for an unchanged value; the constants follow", made,
           kept, still);
    expect(last.translations == 0 && last.translations_cached == 1,
           "combiners: the last frame's counts until the next frame's packets (%u, %u; want 0, 1)", last.translations,
           last.translations_cached);
    log("selftest g3d: combiners: a looping colour animation translated %u times in 8 frames, %u found kept", made,
        kept);
    delete pkg;
}

// ---------------------------------------------------------------- the world's cost, at the island's scale
// 49 more cells of 4,000 units with 8 batches each (the plan's 39 cells, 330 cell draws) and 515 trees
// (WSR's tree count) scattered over 28,000 x 28,000: the host's time for a frame's update, cull and both
// packets (the 3DS's ARM11 is roughly 10-20 times slower a core: an estimate, not a measurement)
void test_world_cost() {
    Scene sc;
    Rng r{99};
    for (int z = -3; z <= 3; z++)
        for (int x = -3; x <= 3; x++) {
            fm::Cell cl = {};
            const Vec3 c = {x * 4000.0f, 0, z * 4000.0f};
            const f32 box[6] = {c.x - 2000, -100, c.z - 2000, c.x + 2000, 1500, c.z + 2000};
            memcpy(cl.bbox, box, sizeof(box));
            cl.n_lods = 1;
            cl.first[0] = (u32)sc.p.cbat.size(), cl.count[0] = 8;
            for (int k = 0; k < 8; k++)
                sc.p.cbat.push_back(sc.p.add_box_batch({c.x + (k % 4) * 900.0f - 1350, 50, c.z + (k / 4) * 1800.0f - 900},
                                                       {400, 50, 800}, k == 7 ? sc.mat_xlu : sc.mat_ground));
            cl.group = sc.p.strs.add("grp_big");
            sc.p.cells.push_back(cl);
        }
    for (int i = 0; i < 515; i++) {
        fm::Instance in = {};
        in.model = sc.model_tree, in.anim = fm::NONE16, in.flags = fm::INST_STATIC;
        const f32 mtx[3][4] = {{1, 0, 0, r.unit() * 28000 - 14000}, {0, 1, 0, 0}, {0, 0, 1, r.unit() * 28000 - 14000}};
        memcpy(in.mtx, mtx, sizeof(mtx));
        sc.p.insts.push_back(in);
    }
    std::string error;
    g3::Package* pkg = load_bytes(sc.p.bytes(), &error);
    expect(pkg != nullptr, "the big scene loads: %s", error.c_str());
    if (!pkg)
        return;
    g3::World w(*pkg);
    g3::Packet opa, xlu;
    g3::Camera cam;
    cam.aspect = 400.0f / 240.0f, cam.near_ = 1.5f, cam.far_ = 80000;
    const int frames = 200;
    const auto t0 = std::chrono::steady_clock::now();
    u32 items = 0;
    for (int f = 0; f < frames; f++) {
        const f32 a = f * 0.03f;
        cam.eye = {0, 300, 0}, cam.target = {std::cos(a) * 5000, 0, std::sin(a) * 5000};
        w.update((f32)f);
        w.set_camera(cam);
        w.build_packet(g3::Pass::OPAQUE, &opa);
        w.build_packet(g3::Pass::TRANSLUCENT, &xlu);
        items += (u32)(opa.items.size() + xlu.items.size());
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / frames;
    const g3::WorldStats& st = w.stats();
    log("selftest g3d: cost: %u cells, %u instances: %.3f ms a frame on the host for update, cull and both packets "
        "(last frame: %u/%u cells, %u/%u instances, %u items; %u items a frame on average)",
        st.cells_total, st.instances_total, ms, st.cells_visible, st.cells_total, st.instances_visible,
        st.instances_total, st.items_opaque + st.items_translucent, items / frames);
    delete pkg;
}

// ---------------------------------------------------------------- the packet ring
struct TestBackend : g3::Backend {
    std::vector<std::string> calls;
    void prepare(g3::Package&) override { calls.push_back("prepare"); }
    void draw(const g3::Packet& p, const g3::GxView&) override {
        calls.push_back("draw " + std::to_string(p.id) + (p.pass == g3::Pass::OPAQUE ? " opa" : " xlu"));
    }
    void release(g3::Package&) override { calls.push_back("release"); }
};
struct TestRenderer : GxRenderer {
    bool drawing = true;
    void native(u32 id) override { engine::g3d::native_draw_execute(id, drawing); }
};
std::deque<u8> s_stream;  // what the gather pipe sent, not decoded yet
bool s_inline;            // decode each burst as it comes (a renderer keeping up)
void decode_stream() {
    std::vector<u8> b(s_stream.begin(), s_stream.end());
    const u32 used = gx_decode(b.data(), (u32)b.size(), false);
    s_stream.erase(s_stream.begin(), s_stream.begin() + used);
}
void capture(const u8* burst) {
    s_stream.insert(s_stream.end(), burst, burst + 32);
    if (s_inline)
        decode_stream();
}

void test_native_draw(g3::Package& pkg) {
    TestBackend be;
    TestRenderer rr;
    g3::native_draw_set_backend(&be);
    gx_set_renderer(&rr);
    g_gather_capture = capture;
    const g3::NativeDrawStats s0 = g3::native_draw_stats();
    // order: five packets, the renderer behind (the stream decoded afterwards): prepare once, then
    // the draws in order
    s_inline = false;
    u32 first = 0;
    for (int i = 0; i < 5; i++) {
        g3::Packet* p = g3::native_draw_begin();
        if (!i)
            first = p->id;
        p->package = &pkg;
        p->pass = i & 1 ? g3::Pass::TRANSLUCENT : g3::Pass::OPAQUE;
        expect(p->matrices.size() == 12 && p->matrices[0] == 1 && p->items.empty(), "a reset packet");
        g3::native_draw_submit(p);
    }
    // (the last command may still be in the pipe: flush it as GXFlush would)
    while (g_gather_n)
        gx_fifo_write(0, 0, 1);
    decode_stream();
    std::string got;
    for (const std::string& c : be.calls)
        got += c + ";";
    std::string want = "prepare;";
    for (int i = 0; i < 5; i++)
        want += "draw " + std::to_string(first + i) + (i & 1 ? " xlu;" : " opa;");
    expect(got == want, "order: %s (want %s)", got.c_str(), want.c_str());
    // wrap-around: 40 more, the renderer keeping up (decoded as each burst lands)
    be.calls.clear();
    s_inline = true;
    for (int i = 0; i < 40; i++) {
        g3::Packet* p = g3::native_draw_begin();
        p->package = &pkg;
        g3::native_draw_submit(p);
    }
    while (g_gather_n)
        gx_fifo_write(0, 0, 1);
    expect(be.calls.size() == 40, "wrap-around: %zu of 40 drawn", be.calls.size());
    // a frame not drawn: retired, not drawn
    rr.drawing = false;
    be.calls.clear();
    {
        g3::Packet* p = g3::native_draw_begin();
        p->package = &pkg;
        g3::native_draw_submit(p);
        while (g_gather_n)
            gx_fifo_write(0, 0, 1);
    }
    expect(be.calls.empty(), "an undrawn frame's packet is retired");
    rr.drawing = true;
    // the renderer never runs: the ring fills; the ninth packet takes the first one back
    s_inline = false;
    be.calls.clear();
    const g3::NativeDrawStats s1 = g3::native_draw_stats();
    u32 ids[9];
    for (int i = 0; i < 9; i++) {
        g3::Packet* p = g3::native_draw_begin();
        ids[i] = p->id;
        p->package = &pkg;
        g3::native_draw_submit(p);
    }
    const g3::NativeDrawStats s2 = g3::native_draw_stats();
    expect(s2.lost == s1.lost + 1 && s2.waits == s1.waits + 1, "a full ring: one packet given up on (%u lost, %u waits)",
           s2.lost - s1.lost, s2.waits - s1.waits);
    // ... and when the stream does run, the stale command is ignored, the others drawn in order
    while (g_gather_n)
        gx_fifo_write(0, 0, 1);
    decode_stream();
    const g3::NativeDrawStats s3 = g3::native_draw_stats();
    std::string late;
    for (const std::string& c : be.calls)
        late += c + ";";
    std::string late_want;
    for (int i = 1; i < 9; i++)
        late_want += "draw " + std::to_string(ids[i]) + " opa;";
    expect(late == late_want && s3.stale == s2.stale + 1, "late stream: %s, %u stale", late.c_str(), s3.stale - s2.stale);
    // release with the renderer keeping up: on its thread, once
    be.calls.clear();
    s_inline = true;
    g3::native_draw_release(pkg);
    expect(be.calls.size() == 1 && be.calls[0] == "release", "release (renderer running): %zu calls", be.calls.size());
    // prepared (the renderer running), then released with the renderer stopped: released here, the
    // queued draw given up on
    be.calls.clear();
    g3::native_draw_prepare(pkg);
    while (g_gather_n)
        gx_fifo_write(0, 0, 1);
    s_inline = false;
    {
        g3::Packet* p = g3::native_draw_begin();
        p->package = &pkg;
        g3::native_draw_submit(p);
    }
    g3::native_draw_release(pkg);
    std::string rel;
    for (const std::string& c : be.calls)
        rel += c + ";";
    expect(rel == "prepare;release;", "release (renderer stopped): %s", rel.c_str());
    decode_stream();  // (the late commands: stale)
    expect(be.calls.size() == 2, "nothing runs after the release (%zu calls)", be.calls.size());
    const g3::NativeDrawStats s4 = g3::native_draw_stats();
    log("selftest g3d: native draw: %u packets submitted, %u executed, %u drawn, %u stale, %u given up on, %u waits: "
        "order, wrap-around, undrawn frames, a full ring, release checked",
        s4.submitted - s0.submitted, s4.executed - s0.executed, s4.drawn - s0.drawn, s4.stale - s0.stale,
        s4.lost - s0.lost, s4.waits - s0.waits);
    g_gather_capture = nullptr;
    gx_set_renderer(nullptr);
    g3::native_draw_set_backend(nullptr);
    s_stream.clear();
}

// ---------------------------------------------------------------- the real package
void test_real_package(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        log("selftest g3d: no package at %s (NOMA_G3D_PACKAGE): the real-package part skipped", path.c_str());
        return;
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void* block = malloc((size_t)n);
    const bool read_ok = fread(block, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!read_ok) {
        free(block);
        expect(false, "read %s", path.c_str());
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::string error;
    u32 version = 0;
    if (n >= 8)
        memcpy(&version, (const u8*)block + 4, 4);
    g3::Package* pkg = g3::Package::load(block, (u32)n, &error);
    const double load_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!pkg && version == 1) {  // (refused, rightly: the island is to be staged again)
        log("selftest g3d: %s: %s: the real-package part skipped", path.c_str(), error.c_str());
        return;
    }
    expect(pkg != nullptr, "the real package loads: %s", error.c_str());
    if (!pkg)
        return;
    u32 tris = 0, kb_tex = 0;
    for (const fm::Batch& b : pkg->batches())
        tris += b.n_indices / 3;
    for (u32 i = 0; i < pkg->textures().count; i++)
        kb_tex += pkg->texture_image(i).size >> 10;
    log("selftest g3d: %s: %ld bytes, loaded and checked in %.1f ms (%u textures converted from GX); %u textures (%u KB), "
        "%u materials, %u vertex buffers, %u batches (%u triangles), %u models, %u cells, %u instances, %u animations, "
        "%u markers, %u KCL files",
        path.c_str(), n, load_ms, pkg->converted_textures(), pkg->textures().count, kb_tex, pkg->materials().count,
        pkg->vertex_buffers().count, pkg->batches().count, tris, pkg->models().count, pkg->cells().count,
        pkg->instances().count, pkg->anims().count, pkg->markers().count, pkg->collisions().count);
    // textures converted at load: level 0 decoded against GX's decode of the source, by GX format
    // (CMPR as ETC1 is lossy: PSNR; the others must be exact, L4/L8 in colour only)
    if (pkg->converted_textures()) {
        double se[16] = {};
        u64 samples[16] = {}, diff[16] = {}, alpha_diff[16] = {}, count[16] = {};
        std::vector<u32> got, want;
        for (u32 i = 0; i < pkg->textures().count; i++) {
            const fm::Texture& t = pkg->textures()[i];
            if (!(t.flags & fm::TEX_GX_RAW))
                continue;
            const g3::Package::TextureImage img = pkg->texture_image(i);
            const u32 gf = t.gx_format & 15;
            got.assign((size_t)img.width * img.height, 0), want.assign((size_t)t.width * t.height, 0);
            g3::pica_decode(img.format, img.texels, img.width, img.height, got.data());
            gx_decode_texture(pkg->at(t.data), t.gx_format, t.width, t.height, want.data());
            count[gf]++;
            const bool lum = img.format == fm::TF_L4 || img.format == fm::TF_L8;
            for (u32 y = 0; y < t.height; y++)
                for (u32 x = 0; x < t.width; x++) {
                    u32 g = got[y * img.width + x], c = want[y * t.width + x];
                    if (img.format == fm::TF_RGBA5551) {
                        auto q = [](u32 v) { return guest::pica_tex::detail::x5(v >> 3); };
                        c = q(c & 255) | q(c >> 8 & 255) << 8 | q(c >> 16 & 255) << 16 | (c >> 31 ? 255u : 0u) << 24;
                    }
                    for (int k = 0; k < 3; k++) {
                        const int e = (int)(g >> (8 * k) & 255) - (int)(c >> (8 * k) & 255);
                        se[gf] += e * e;
                    }
                    samples[gf] += 3;
                    diff[gf] += (g & 0xFFFFFF) != (c & 0xFFFFFF);
                    // (ETC1 drops alpha; ETC1A4 keeps 4 bits)
                    if (!lum && img.format != fm::TF_ETC1)
                        alpha_diff[gf] += (img.format == fm::TF_ETC1A4 ? (g >> 28) != (c >> 28) : (g >> 24) != (c >> 24));
                }
        }
        static const char* const k_gx[16] = {"I4", "I8", "IA4", "IA8", "RGB565", "RGB5A3", "RGBA8", "?", "C4", "C8", "C14X2",
                                             "?", "?", "?", "CMPR", "?"};
        for (u32 f = 0; f < 16; f++)
            if (count[f]) {
                const double psnr = se[f] ? 10 * std::log10(255.0 * 255.0 * (double)samples[f] / se[f]) : 999;
                log("selftest g3d:   %u %s textures converted: level 0 against GX's decode: %llu texels' colour differ (%.1f "
                    "dB), %llu alphas differ",
                    (u32)count[f], k_gx[f], (unsigned long long)diff[f], psnr, (unsigned long long)alpha_diff[f]);
                if (f != 14)
                    expect(diff[f] == 0 && alpha_diff[f] == 0, "%s conversions exact", k_gx[f]);
                else
                    expect(psnr >= 30 && alpha_diff[f] == 0, "CMPR as ETC1 on the package's textures: %.1f dB", psnr);
            }
    }
    const auto t1 = std::chrono::steady_clock::now();
    g3::World w(*pkg);
    const double world_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    u32 approximated = 0, indirect = 0;
    std::string which;
    for (const g3::Material& m : w.materials()) {
        indirect += m.indirect;
        if (!m.approximated)
            continue;
        if (approximated++ < 12)
            which += std::string(approximated > 1 ? ", " : "") + pkg->str(pkg->materials()[m.index].name) + " (" +
                     std::to_string(m.comb.unsupported) + " in the combiners, " + std::to_string(m.comb.n_stages) +
                     " stages)";
    }
    log("selftest g3d: world made in %.1f ms; k = %g; %u of %u materials approximate something%s%s; %u had indirect "
        "stages (dropped)",
        world_ms, (double)w.world_scale(), approximated, (u32)w.materials().size(), approximated ? ": " : "",
        which.c_str(), indirect);
    u32 prelit = 0, compact = 0, detail = 0;
    for (const g3::Material& m : w.materials())
        prelit += m.prelit;
    for (const fm::VertexBuffer& vb : pkg->vertex_buffers())
        compact += (vb.attrs & fm::VA_POS16) != 0;
    for (const fm::Batch& b : pkg->batches())
        detail += (b.flags & fm::BATCH_DETAIL) != 0;
    const fm::Env& env = pkg->env();
    log("selftest g3d: version 2: %u of %u materials prelit, %u of %u vertex buffers compact, %u detail batches "
        "(culled beyond %g), visibility rows %s (%u bytes a cell, up to %g over the ground)",
        prelit, (u32)w.materials().size(), compact, pkg->vertex_buffers().count, detail, (double)env.detail_distance,
        env.pvs_row_bytes ? "present" : "none", env.pvs_row_bytes, (double)env.pvs_max_height);
    // the ground under an eye, as the island scene finds it (its collision's highest surface below facing
    // up, not a fence: the stager's floors, within the visibility rows' height; none: below that), for
    // the rows
    engine::collision::World col;
    for (const fm::Collision& c : pkg->collisions())
        col.add_kcl(pkg->at(c.data), c.size, pkg->str(c.name));
    for (const fm::Cylinder& c : pkg->cylinders())
        col.add_cylinder({c.x, c.z, c.y0, c.y1, c.radius});
    col.finish();
    auto ground_under = [&](Vec3 eye) {
        if (!env.pvs_row_bytes)
            return (f32)NAN;
        const f32 reach = env.pvs_max_height + 1.0f;
        engine::collision::Hit hit;
        auto floor = [](u16 a, Vec3 n) { return n.y > 0.05f && !engine::collision::kcl_is_fence(a); };
        return col.ground(eye.x, eye.z, eye.y, eye.y - reach, floor, &hit) ? hit.point.y : eye.y - reach;
    };
    // each view: the frame (its stats), then the World's time a frame (update, cull, both packets:
    // the mean of 200, the animations running) with the speed-ups and without; the material changes in
    // the items' order (what the backend sends again)
    u32 views = 0;
    double sum_on = 0, sum_off = 0;
    for (const fm::Marker& m : pkg->markers()) {
        const char* name = pkg->str(m.name);
        if (strncmp(name, "view_", 5) != 0)
            continue;
        g3::Camera cam;
        cam.eye = {m.position[0], m.position[1], m.position[2]};
        cam.target = m.kind == 2 ? Vec3{m.dir[0], m.dir[1], m.dir[2]} : cam.eye + Vec3{m.dir[0], m.dir[1], m.dir[2]};
        cam.aspect = 400.0f / 240.0f, cam.near_ = 1.5f, cam.far_ = 80000;
        g3::Packet opa, xlu;
        double ms[2] = {0, 0};
        u32 changes[2] = {0, 0};
        g3::WorldStats st[2];
        for (int on = 1; on >= 0; on--) {
            w.set_features(on ? g3::WF_ALL : 0);
            const int frames = 200;
            const auto t2 = std::chrono::steady_clock::now();
            const f32 ground = ground_under(cam.eye);
            for (int f = 0; f < frames; f++) {
                w.update(600.0f + f);
                w.set_camera(cam, ground);
                w.build_packet(g3::Pass::OPAQUE, &opa);
                w.build_packet(g3::Pass::TRANSLUCENT, &xlu);
            }
            ms[on] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t2).count() / frames;
            st[on] = w.stats();
            for (const g3::Packet* p : {&opa, &xlu})
                for (size_t i = 0; i < p->items.size(); i++)
                    changes[on] += !i || pkg->batches()[p->items[i].batch].material !=
                                             pkg->batches()[p->items[i - 1].batch].material;
        }
        const g3::WorldStats& s = st[1];
        log("selftest g3d: %s: cells %u/%u (%u hidden by cell %d's row), instances %u/%u, items %u opaque + %u "
            "translucent (%u details culled), %u triangles, %zu matrices, %zu animated materials, %u material "
            "changes; the World %.3f ms a frame (host; %.3f without the speed-ups: %u items, %u triangles, %u "
            "material changes)",
            name, s.cells_visible, s.cells_total, s.cells_pvs_hidden, s.pvs_cell, s.instances_visible,
            s.instances_total, s.items_opaque, s.items_translucent, s.details_culled, s.triangles,
            opa.matrices.size() / 12, opa.mat_states.size(), changes[1], ms[1], ms[0],
            st[0].items_opaque + st[0].items_translucent, st[0].triangles, changes[0]);
        sum_on += ms[1], sum_off += ms[0];
        views++;
    }
    w.set_features(g3::WF_ALL);
    if (views)
        log("selftest g3d: the World a frame, the mean of %u views: %.3f ms with the speed-ups, %.3f without (host; "
            "the 3DS's ARM11 at 804 MHz some 20-30 times slower: an estimate)",
            views, sum_on / views, sum_off / views);
    if (!views)
        log("selftest g3d: the package has no view_* markers");
    delete pkg;
}

}  // namespace

void selftest_g3d() {
    s_failures = s_checks = 0;
    // NOMA_G3D_WRITE_TEST=path: the test scene's package as a file (for tools/ng3d_bake, and for this
    // test's real-package part)
    if (const char* out = getenv("NOMA_G3D_WRITE_TEST")) {
        Scene sc;
        const std::vector<u8> b = sc.p.bytes();
        FILE* f = fopen(out, "wb");
        if (f) {
            fwrite(b.data(), 1, b.size(), f);
            fclose(f);
        }
        log("selftest g3d: the test package written to %s (%zu bytes)", out, b.size());
    }
    test_load();
    test_textures();
    test_anim();
    {
        Scene sc;
        std::string error;
        g3::Package* pkg = load_bytes(sc.p.bytes(), &error);
        if (pkg) {
            test_materials(*pkg, sc);
            test_world(*pkg, sc);
            test_visibility();
            test_anim_rate();
            test_native_draw(*pkg);
            delete pkg;
        }
    }
    test_world_cost();
    const char* path = getenv("NOMA_G3D_PACKAGE");
    if (!path) {  // (the restaged island while it sits beside the old one)
        FILE* f = fopen("wuhu/island_v2.ng3d", "rb");
        path = f ? "wuhu/island_v2.ng3d" : "wuhu/island.ng3d";
        if (f)
            fclose(f);
    }
    test_real_package(path);
    log("selftest g3d: %d checks, %d failed: %s", s_checks, s_failures, s_failures ? "MISMATCH" : "ok");
}

}  // namespace guest
