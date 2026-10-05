// NG3D packages: loading and checking (package.h).
#include "engine/g3d/package.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "engine/g3d/texture.h"

namespace engine::g3d {

namespace {

// what a package without ENVR gets: nothing to draw beyond its cells and instances
const fmt::Env k_default_env = [] {
    fmt::Env e = {};
    e.sky_model = e.sky_anim = e.sea_model = e.sea_anim_srt = e.sea_anim_clr = fmt::NONE16;
    e.world_anim_srt = e.world_anim_clr = fmt::NONE16;
    e.mii_units_per_unit = 1.0f;
    return e;
}();

bool fail(std::string* error, const char* format, ...) __attribute__((format(printf, 2, 3)));
bool fail(std::string* error, const char* format, ...) {
    if (error) {
        char buf[256];
        va_list ap;
        va_start(ap, format);
        vsnprintf(buf, sizeof(buf), format, ap);
        va_end(ap);
        *error = buf;
    }
    return false;
}

bool finite(const f32* v, int n) {
    for (int i = 0; i < n; i++)
        if (!std::isfinite(v[i]))
            return false;
    return true;
}

// bytes of a vertex of this layout (VtxAttr order: POS or POS16, NRM, CLR, UV0, UV1)
u32 vertex_bytes(u32 attrs, u32 flags) {
    const u32 uv = (flags & fmt::VB_UV_FLOAT) ? 8 : 4;
    return (attrs & fmt::VA_POS ? 12 : 0) + (attrs & fmt::VA_POS16 ? 8 : 0) + (attrs & fmt::VA_NRM ? 4 : 0) +
           (attrs & fmt::VA_CLR ? 4 : 0) + (attrs & fmt::VA_UV0 ? uv : 0) + (attrs & fmt::VA_UV1 ? uv : 0);
}

}  // namespace

Package* Package::load(void* data, u32 size, std::string* error) {
    Package* p = new Package();
    p->data_ = (u8*)data;
    p->size_ = size;
    if (!data || !p->check(error)) {
        delete p;  // (frees data)
        return nullptr;
    }
    p->index_names();
    return p;
}

Package::~Package() { free(data_); }

const fmt::Section* Package::find_section(u32 tag) const {
    const fmt::Header* h = (const fmt::Header*)data_;
    const fmt::Section* s = (const fmt::Section*)(data_ + sizeof(fmt::Header));
    for (u32 i = 0; i < h->n_sections; i++)
        if (s[i].tag == tag)
            return &s[i];
    return nullptr;
}

const char* Package::str(u32 offset) const {
    const fmt::Section* s = section(fmt::SEC_STRS);
    return s && offset < s->size ? (const char*)(data_ + s->offset + offset) : "";
}

Package::TextureImage Package::texture_image(u32 index) const {
    if (index < converted_of_.size() && converted_of_[index] >= 0)
        return converted_[(u32)converted_of_[index]].image;
    const fmt::Texture& t = textures()[index];
    return {t.format, t.levels, t.width, t.height, t.src_width, t.src_height, data_ + t.data, t.size};
}

// ---------------------------------------------------------------- the checks
bool Package::check(std::string* error) {
    if (size_ < sizeof(fmt::Header))
        return fail(error, "%u bytes: smaller than a header", size_);
    if ((uintptr_t)data_ & 3)
        return fail(error, "the data isn't 4-byte aligned in memory");
    const fmt::Header& h = *(const fmt::Header*)data_;
    if (h.magic != fmt::MAGIC)
        return fail(error, "not an NG3D package (magic %08X)", h.magic);
    if (h.version == 1)
        return fail(error, "NG3D version 1, from before the island's speed work (version %u: compact positions, "
                           "prelit materials, cell visibility, detail batches): stage the island again "
                           "(tools/wuhu/stage_wuhu.py)",
                    fmt::VERSION);
    if (h.version != fmt::VERSION)
        return fail(error, "version %u (this reads %u)", h.version, fmt::VERSION);
    if (h.file_size != size_)
        return fail(error, "the header says %u bytes, the file has %u", h.file_size, size_);
    const uint64_t table_end = sizeof(fmt::Header) + (uint64_t)h.n_sections * sizeof(fmt::Section);
    if (table_end > size_)
        return fail(error, "%u sections: the table runs past the end", h.n_sections);
    const fmt::Section* secs = (const fmt::Section*)(data_ + sizeof(fmt::Header));
    std::vector<const fmt::Section*> order;
    for (u32 i = 0; i < h.n_sections; i++) {
        const fmt::Section& s = secs[i];
        char tag[5];
        memcpy(tag, &s.tag, 4), tag[4] = 0;
        if (s.offset % 32)
            return fail(error, "section %s at %u: not 32-byte aligned", tag, s.offset);
        if (s.offset < table_end || s.offset > size_ || s.size > size_ - s.offset)
            return fail(error, "section %s (%u bytes at %u) outside the file", tag, s.size, s.offset);
        for (u32 j = 0; j < i; j++)
            if (secs[j].tag == s.tag)
                return fail(error, "section %s twice", tag);
        order.push_back(&s);
    }
    // (each tag once: the slots the accessors read from here on)
    for (u32 i = 0; i < h.n_sections; i++)
        if (tag_slot(secs[i].tag) >= 0)
            slot_[tag_slot(secs[i].tag)] = &secs[i];
    std::sort(order.begin(), order.end(), [](const fmt::Section* a, const fmt::Section* b) { return a->offset < b->offset; });
    for (size_t i = 1; i < order.size(); i++)
        if (order[i - 1]->offset + order[i - 1]->size > order[i]->offset)
            return fail(error, "sections overlap at %u", order[i]->offset);
    // array sections: room for their records
    struct ArraySec {
        u32 tag, record;
    };
    static const ArraySec k_arrays[] = {
        {fmt::SEC_TEXS, sizeof(fmt::Texture)}, {fmt::SEC_MATS, sizeof(fmt::Material)}, {fmt::SEC_TMTX, sizeof(fmt::TexMtx)},
        {fmt::SEC_VTXB, sizeof(fmt::VertexBuffer)}, {fmt::SEC_IDAT, 2}, {fmt::SEC_BTCH, sizeof(fmt::Batch)},
        {fmt::SEC_NODE, sizeof(fmt::Node)}, {fmt::SEC_MODL, sizeof(fmt::Model)}, {fmt::SEC_CELL, sizeof(fmt::Cell)},
        {fmt::SEC_CBAT, 2}, {fmt::SEC_INST, sizeof(fmt::Instance)}, {fmt::SEC_ANIM, sizeof(fmt::Anim)},
        {fmt::SEC_TRAK, sizeof(fmt::Track)}, {fmt::SEC_KEYS, sizeof(fmt::Key)}, {fmt::SEC_LITE, sizeof(fmt::Light)},
        {fmt::SEC_LSET, sizeof(fmt::LightSet)}, {fmt::SEC_FOGS, sizeof(fmt::Fog)}, {fmt::SEC_MARK, sizeof(fmt::Marker)},
        {fmt::SEC_COLL, sizeof(fmt::Collision)}, {fmt::SEC_CYLS, sizeof(fmt::Cylinder)}, {fmt::SEC_ENVR, sizeof(fmt::Env)},
    };
    for (const ArraySec& a : k_arrays)
        if (const fmt::Section* s = section(a.tag)) {
            if ((uint64_t)s->count * a.record > s->size) {
                char tag[5];
                memcpy(tag, &a.tag, 4), tag[4] = 0;
                return fail(error, "section %s: %u records of %u bytes don't fit in %u", tag, s->count, a.record, s->size);
            }
        }
    // strings: NUL first (offset 0 = "") and last (every string ends inside)
    if (const fmt::Section* s = section(fmt::SEC_STRS)) {
        if (!s->size || data_[s->offset] != 0 || data_[s->offset + s->size - 1] != 0)
            return fail(error, "STRS doesn't start and end with a NUL");
    }
    // the environment: one record (an empty ENVR is none)
    const fmt::Section* envr = section(fmt::SEC_ENVR);
    if (envr && envr->count > 1)
        return fail(error, "ENVR holds %u records (one expected)", envr->count);
    env_ = envr && envr->count ? (const fmt::Env*)(data_ + envr->offset) : &k_default_env;
    return check_textures(error) && check_geometry(error) && check_models(error) && check_world(error) &&
           check_anims(error) && check_env(error);
}

// a name: 0, or the start of a string in STRS (after a NUL)
#define CHECK_NAME(what, i, off)                                                                       \
    do {                                                                                               \
        const fmt::Section* strs_ = section(fmt::SEC_STRS);                                            \
        if ((off) != 0 && (!strs_ || (off) >= strs_->size || data_[strs_->offset + (off) - 1] != 0))   \
            return fail(error, "%s %u: its name (%u) isn't a string of STRS", what, (u32)(i), (u32)(off)); \
    } while (0)

bool Package::check_textures(std::string* error) {
    const fmt::Section* tdat = section(fmt::SEC_TDAT);
    Span<fmt::Texture> texs = textures();
    converted_of_.assign(texs.count, -1);
    for (u32 i = 0; i < texs.count; i++) {
        const fmt::Texture& t = texs[i];
        CHECK_NAME("texture", i, t.name);
        if (!tdat || t.data < tdat->offset || t.data > tdat->offset + tdat->size || t.size > tdat->offset + tdat->size - t.data)
            return fail(error, "texture %u (%s): its texels (%u bytes at %u) are outside TDAT", i, str(t.name), t.size, t.data);
        if (t.data % 128)
            return fail(error, "texture %u (%s): its texels aren't 128-byte aligned", i, str(t.name));
        if (!pica_format_valid(t.format))
            return fail(error, "texture %u (%s): format %u", i, str(t.name), t.format);
        if (!t.levels)
            return fail(error, "texture %u (%s): no levels", i, str(t.name));
        if (!t.src_width || !t.src_height || t.src_width > t.width || t.src_height > t.height)
            return fail(error, "texture %u (%s): source size %ux%u for %ux%u", i, str(t.name), t.src_width, t.src_height,
                        t.width, t.height);
        if (t.flags & fmt::TEX_GX_RAW) {
            ConvertedTexture c;
            std::string why;
            if (!convert_gx_texture(t, data_ + t.data, t.size, &c, &why))
                return fail(error, "texture %u (%s), GX format %u: %s", i, str(t.name), t.gx_format, why.c_str());
            Converted cv;
            cv.texture = i;
            cv.texels = std::move(c.texels);
            cv.image = {c.format, c.levels, c.width, c.height, t.src_width, t.src_height, nullptr, (u32)cv.texels.size()};
            converted_of_[i] = (int)converted_.size();
            converted_.push_back(std::move(cv));
            continue;
        }
        if (!is_pow2(t.width) || !is_pow2(t.height) || t.width < 8 || t.height < 8 || t.width > 1024 || t.height > 1024)
            return fail(error, "texture %u (%s): %ux%u is not a PICA size", i, str(t.name), t.width, t.height);
        if (t.levels > pica_max_levels(t.width, t.height))
            return fail(error, "texture %u (%s): %u levels of %ux%u go below 8x8", i, str(t.name), t.levels, t.width, t.height);
        const u32 need = pica_texture_bytes(t.format, t.width, t.height, t.levels);
        if (t.size != need)
            return fail(error, "texture %u (%s): %u bytes, its levels take %u", i, str(t.name), t.size, need);
    }
    // (the converted texels are in place now: the vector doesn't move any more)
    for (Converted& c : converted_)
        c.image.texels = c.texels.data();
    return true;
}

bool Package::check_geometry(std::string* error) {
    const u32 n_tex = textures().count, n_tmtx = tex_mtx().count, n_lset = light_sets().count, n_fog = fogs().count;
    Span<fmt::Material> mats = materials();
    for (u32 i = 0; i < mats.count; i++) {
        const fmt::Material& m = mats[i];
        CHECK_NAME("material", i, m.name);
        const char* nm = str(m.name);
        if (m.n_stages < 1 || m.n_stages > fmt::MAX_TEV_STAGES || m.n_texgens > fmt::MAX_TEXGENS || m.n_chans > 2 ||
            m.cull > 3)
            return fail(error, "material %u (%s): %u stages, %u texgens, %u channels, cull %u", i, nm, m.n_stages,
                        m.n_texgens, m.n_chans, m.cull);
        if ((m.flags & fmt::MAT_LIGHT_SET) && m.light_set >= n_lset)
            return fail(error, "material %u (%s): light set %u of %u", i, nm, m.light_set, n_lset);
        if ((m.flags & fmt::MAT_FOG) && m.fog >= n_fog)
            return fail(error, "material %u (%s): fog %u of %u", i, nm, m.fog, n_fog);
        if ((u32)m.tex_mtx_first + m.n_tex_mtx > n_tmtx)
            return fail(error, "material %u (%s): texture matrices %u+%u of %u", i, nm, m.tex_mtx_first, m.n_tex_mtx, n_tmtx);
        for (u32 k = 0; k < fmt::MAX_TEX_MAPS; k++) {
            const fmt::TexMap& tm = m.maps[k];
            if (tm.texture != fmt::NONE16 && tm.texture >= n_tex)
                return fail(error, "material %u (%s): map %u's texture %u of %u", i, nm, k, tm.texture, n_tex);
            if (tm.wrap_s > 2 || tm.wrap_t > 2 || tm.min_filter > 5 || tm.mag_filter > 5 || !finite(&tm.lod_bias, 3))
                return fail(error, "material %u (%s): map %u's sampling", i, nm, k);
        }
        for (u32 k = 0; k < fmt::MAX_TEXGENS; k++) {
            const fmt::TexGen& g = m.texgens[k];
            if (k < m.n_texgens && g.type > 10)
                return fail(error, "material %u (%s): texgen %u's type %u", i, nm, k, g.type);
            if ((g.matrix != fmt::NONE16 && g.matrix >= m.n_tex_mtx) || (g.post_mtx != fmt::NONE16 && g.post_mtx >= m.n_tex_mtx))
                return fail(error, "material %u (%s): texgen %u's matrices %u/%u of %u", i, nm, k, g.matrix, g.post_mtx,
                            m.n_tex_mtx);
        }
    }
    Span<fmt::TexMtx> tm = tex_mtx();
    for (u32 i = 0; i < tm.count; i++)
        if (tm[i].mode > 2 || tm[i].map_mode > 4 || !finite(tm[i].scale, 5) || !finite(&tm[i].effect[0][0], 12))
            return fail(error, "texture matrix %u: mode %u, map mode %u", i, tm[i].mode, tm[i].map_mode);
    const fmt::Section* vdat = section(fmt::SEC_VDAT);
    Span<fmt::VertexBuffer> vbs = vertex_buffers();
    for (u32 i = 0; i < vbs.count; i++) {
        const fmt::VertexBuffer& v = vbs[i];
        // positions: f32, or s16 with the buffer's origin and scale (one of the two)
        const bool p32 = v.attrs & fmt::VA_POS, p16 = v.attrs & fmt::VA_POS16;
        if (p32 == p16 || (v.attrs & ~63u) || (v.flags & ~(u32)fmt::VB_UV_FLOAT))
            return fail(error, "vertex buffer %u: attributes %02X, flags %02X", i, v.attrs, v.flags);
        if (!finite(v.pos_origin, 3) || !std::isfinite(v.pos_scale) || (p16 && !(v.pos_scale > 0)))
            return fail(error, "vertex buffer %u: position origin and scale (%g)", i, (double)v.pos_scale);
        if (v.stride < vertex_bytes(v.attrs, v.flags) || v.stride % 4)
            return fail(error, "vertex buffer %u: stride %u for a %u-byte layout", i, v.stride, vertex_bytes(v.attrs, v.flags));
        if (v.count > 65536 || v.data % 16)
            return fail(error, "vertex buffer %u: %u vertices at %u", i, v.count, v.data);
        const uint64_t bytes = (uint64_t)v.count * v.stride;
        if (!vdat || v.data < vdat->offset || v.data + bytes > (uint64_t)vdat->offset + vdat->size)
            return fail(error, "vertex buffer %u: its %u vertices are outside VDAT", i, v.count);
        if (!finite(v.uv_scale, 2))
            return fail(error, "vertex buffer %u: UV scale", i);
    }
    Span<u16> idx = indices();
    Span<fmt::Batch> bts = batches();
    for (u32 i = 0; i < bts.count; i++) {
        const fmt::Batch& b = bts[i];
        if (b.material >= mats.count || b.vbuf >= vbs.count)
            return fail(error, "batch %u: material %u of %u, vertex buffer %u of %u", i, b.material, mats.count, b.vbuf, vbs.count);
        if (b.n_indices % 3 || (uint64_t)b.first_index + b.n_indices > idx.count)
            return fail(error, "batch %u: indices %u+%u of %u", i, b.first_index, b.n_indices, idx.count);
        const u32 nv = vbs[b.vbuf].count;
        for (u32 k = 0; k < b.n_indices; k++)
            if (idx[b.first_index + k] >= nv)
                return fail(error, "batch %u: index %u is vertex %u of %u", i, k, idx[b.first_index + k], nv);
        if (!finite(b.bbox, 6))
            return fail(error, "batch %u: its box", i);
        if (b.node != fmt::NONE16 && b.node >= nodes().count)
            return fail(error, "batch %u: node %u", i, b.node);
        if (b.flags & ~(u32)fmt::BATCH_DETAIL)
            return fail(error, "batch %u: flags %04X", i, b.flags);
    }
    return true;
}

bool Package::check_models(std::string* error) {
    Span<fmt::Node> nds = nodes();
    Span<fmt::Model> mds = models();
    const u32 n_batches = batches().count;
    for (u32 i = 0; i < nds.count; i++) {
        CHECK_NAME("node", i, nds[i].name);
        if ((u32)nds[i].first_batch + nds[i].n_batches > n_batches)
            return fail(error, "node %u (%s): batches %u+%u of %u", i, str(nds[i].name), nds[i].first_batch,
                        nds[i].n_batches, n_batches);
        if (!finite(nds[i].scale, 9) || !finite(&nds[i].bind[0][0], 12))
            return fail(error, "node %u (%s): its transform", i, str(nds[i].name));
    }
    for (u32 i = 0; i < mds.count; i++) {
        const fmt::Model& m = mds[i];
        CHECK_NAME("model", i, m.name);
        const char* nm = str(m.name);
        if ((u32)m.first_node + m.n_nodes > nds.count || (u32)m.first_batch + m.n_batches > n_batches)
            return fail(error, "model %u (%s): nodes %u+%u of %u, batches %u+%u of %u", i, nm, m.first_node, m.n_nodes,
                        nds.count, m.first_batch, m.n_batches, n_batches);
        // parents come before their children (a node's world matrix in one pass)
        for (u32 k = 0; k < m.n_nodes; k++) {
            const u16 p = nds[m.first_node + k].parent;
            if (p != fmt::NONE16 && p >= k)
                return fail(error, "model %u (%s): node %u's parent %u doesn't come before it", i, nm, k, p);
        }
        if (!finite(m.bbox, 6) || !std::isfinite(m.radius) || !std::isfinite(m.lod_distance))
            return fail(error, "model %u (%s): its bounds", i, nm);
        // the LOD chain ends
        u32 at = i, steps = 0;
        while (mds[at].lod_next != fmt::NONE16) {
            if (mds[at].lod_next >= mds.count)
                return fail(error, "model %u (%s): LOD model %u of %u", at, str(mds[at].name), mds[at].lod_next, mds.count);
            at = mds[at].lod_next;
            if (++steps > mds.count)
                return fail(error, "model %u (%s): its LOD chain loops", i, nm);
        }
    }
    return true;
}

bool Package::check_world(std::string* error) {
    Span<fmt::Cell> cls = cells();
    Span<u16> cb = cell_batches();
    const u32 n_batches = batches().count;
    for (u32 i = 0; i < cb.count; i++)
        if (cb[i] >= n_batches)
            return fail(error, "CBAT %u: batch %u of %u", i, cb[i], n_batches);
    for (u32 i = 0; i < cls.count; i++) {
        const fmt::Cell& c = cls[i];
        CHECK_NAME("cell", i, c.group);
        if (c.n_lods < 1 || c.n_lods > fmt::MAX_CELL_LODS)
            return fail(error, "cell %u: %u detail levels", i, c.n_lods);
        for (u32 l = 0; l < c.n_lods; l++) {
            if ((uint64_t)c.first[l] + c.count[l] > cb.count)
                return fail(error, "cell %u: level %u's batches %u+%u of %u", i, l, c.first[l], c.count[l], cb.count);
            if (!std::isfinite(c.lod_distance[l]) || (l && c.lod_distance[l] < c.lod_distance[l - 1]))
                return fail(error, "cell %u: level %u's distance", i, l);
        }
        if (!finite(c.bbox, 6))
            return fail(error, "cell %u: its box", i);
    }
    Span<fmt::Instance> ins = instances();
    for (u32 i = 0; i < ins.count; i++) {
        const fmt::Instance& n = ins[i];
        if (n.model >= models().count || (n.anim != fmt::NONE16 && n.anim >= anims().count))
            return fail(error, "instance %u: model %u of %u, animation %u of %u", i, n.model, models().count, n.anim,
                        anims().count);
        if (!finite(&n.mtx[0][0], 12) || !std::isfinite(n.anim_offset) || !std::isfinite(n.cull_distance))
            return fail(error, "instance %u: its matrix", i);
    }
    Span<fmt::Marker> mk = markers();
    for (u32 i = 0; i < mk.count; i++) {
        CHECK_NAME("marker", i, mk[i].name);
        if (mk[i].kind > fmt::MK_WARP || !finite(mk[i].position, 6))
            return fail(error, "marker %u (%s): kind %u", i, str(mk[i].name), mk[i].kind);
    }
    const fmt::Section* blob = section(fmt::SEC_BLOB);
    Span<fmt::Collision> cs = collisions();
    for (u32 i = 0; i < cs.count; i++) {
        CHECK_NAME("collision", i, cs[i].name);
        if (!blob || cs[i].data < blob->offset || (uint64_t)cs[i].data + cs[i].size > (uint64_t)blob->offset + blob->size)
            return fail(error, "collision %u (%s): its %u bytes are outside BLOB", i, str(cs[i].name), cs[i].size);
    }
    Span<fmt::Cylinder> cy = cylinders();
    for (u32 i = 0; i < cy.count; i++)
        if (!finite(&cy[i].x, 5) || cy[i].radius < 0)
            return fail(error, "cylinder %u", i);
    return true;
}

bool Package::check_anims(std::string* error) {
    Span<fmt::Anim> as = anims();
    Span<fmt::Track> ts = tracks();
    Span<fmt::Key> ks = keys();
    for (u32 i = 0; i < as.count; i++) {
        CHECK_NAME("animation", i, as[i].name);
        if ((uint64_t)as[i].first_track + as[i].n_tracks > ts.count || !(as[i].frames > 0) || !std::isfinite(as[i].frames))
            return fail(error, "animation %u (%s): tracks %u+%u of %u, %g frames", i, str(as[i].name), as[i].first_track,
                        as[i].n_tracks, ts.count, (double)as[i].frames);
    }
    for (u32 i = 0; i < ts.count; i++) {
        const fmt::Track& t = ts[i];
        if (t.interp > fmt::IP_HERMITE || !t.n_keys || (uint64_t)t.first_key + t.n_keys > ks.count)
            return fail(error, "track %u: interpolation %u, keys %u+%u of %u", i, t.interp, t.first_key, t.n_keys, ks.count);
        if (t.target <= fmt::TT_NODE_TRANS_Z) {
            if (t.model >= models().count || t.index >= models()[t.model].n_nodes)
                return fail(error, "track %u: node %u of model %u", i, t.index, t.model);
        } else if (t.target >= fmt::TT_TEXMTX_SCALE_S && t.target <= fmt::TT_TEXMTX_TRANS_T) {
            if (t.index >= tex_mtx().count)
                return fail(error, "track %u: texture matrix %u of %u", i, t.index, tex_mtx().count);
        } else if (t.target >= fmt::TT_MAT_REG_R && t.target <= fmt::TT_MAT_REG_A) {
            if (t.index >= materials().count || t.reg > fmt::CR_KONST3 || (t.reg > fmt::CR_TEV2 && t.reg < fmt::CR_KONST0))
                return fail(error, "track %u: material %u of %u, register %u", i, t.index, materials().count, t.reg);
        } else {
            return fail(error, "track %u: target %u", i, t.target);
        }
        for (u32 k = 0; k < t.n_keys; k++) {
            const fmt::Key& key = ks[t.first_key + k];
            if (!finite(&key.frame, 3) || (k && key.frame < ks[t.first_key + k - 1].frame))
                return fail(error, "track %u: key %u (frame %g) out of order", i, k, (double)key.frame);
        }
    }
    return true;
}

bool Package::check_env(std::string* error) {
    Span<fmt::Light> ls = lights();
    for (u32 i = 0; i < ls.count; i++) {
        CHECK_NAME("light", i, ls[i].name);
        if (ls[i].kind > fmt::LK_POINT || !finite(ls[i].position, 6))
            return fail(error, "light %u (%s): kind %u", i, str(ls[i].name), ls[i].kind);
    }
    Span<fmt::LightSet> sets = light_sets();
    for (u32 i = 0; i < sets.count; i++) {
        CHECK_NAME("light set", i, sets[i].name);
        for (u16 l : sets[i].lights)
            if (l != fmt::NONE16 && l >= ls.count)
                return fail(error, "light set %u (%s): light %u of %u", i, str(sets[i].name), l, ls.count);
    }
    Span<fmt::Fog> fs = fogs();
    for (u32 i = 0; i < fs.count; i++)
        if ((fs[i].type != fmt::FOG_NONE && fs[i].type != fmt::FOG_LINEAR) || !finite(&fs[i].start, 2))
            return fail(error, "fog %u: type %u", i, fs[i].type);
    const fmt::Env& e = *env_;
    const u32 n_models = models().count, n_anims = anims().count;
    auto model_ok = [&](u16 m) { return m == fmt::NONE16 || m < n_models; };
    auto anim_ok = [&](u16 a) { return a == fmt::NONE16 || a < n_anims; };
    if (!model_ok(e.sky_model) || !model_ok(e.sea_model) || !anim_ok(e.sky_anim) || !anim_ok(e.sea_anim_srt) ||
        !anim_ok(e.sea_anim_clr) || !anim_ok(e.world_anim_srt) || !anim_ok(e.world_anim_clr))
        return fail(error, "ENVR: a model or animation index is out of range");
    if (!finite(e.world_bbox, 6) || !(e.mii_units_per_unit > 0) || !std::isfinite(e.mii_units_per_unit) ||
        !(e.cell_size >= 0) || !finite(e.grid_origin, 2))
        return fail(error, "ENVR: bounds, scale or grid");
    if (!(e.detail_distance >= 0) || !std::isfinite(e.detail_distance))
        return fail(error, "ENVR: detail distance %g", (double)e.detail_distance);
    // the visibility rows (CVIS): one a cell, each with a bit for every cell, the section exactly
    // that size; no section without the rows
    const fmt::Section* cvis = section(fmt::SEC_CVIS);
    if (e.pvs_row_bytes) {
        const u32 n_cells = cells().count;
        if (e.pvs_row_bytes < (n_cells + 7) / 8)
            return fail(error, "ENVR: visibility rows of %u bytes for %u cells", e.pvs_row_bytes, n_cells);
        if (!cvis || (uint64_t)n_cells * e.pvs_row_bytes != cvis->size)
            return fail(error, "CVIS: %u bytes for %u cells' rows of %u bytes", cvis ? cvis->size : 0, n_cells,
                        e.pvs_row_bytes);
        if (!(e.pvs_max_height > 0) || !std::isfinite(e.pvs_max_height))
            return fail(error, "ENVR: visibility height %g", (double)e.pvs_max_height);
    } else if (cvis && cvis->size) {
        return fail(error, "CVIS without ENVR's pvs_row_bytes");
    }
    return true;
}

#undef CHECK_NAME

// ---------------------------------------------------------------- names
void Package::index_names() {
    auto add = [&](std::unordered_map<std::string_view, int>& map, u32 name, int i) {
        const char* s = str(name);
        if (*s)
            map.emplace(std::string_view(s), i);  // (the first of a name wins)
    };
    for (u32 i = 0; i < models().count; i++)
        add(models_by_name_, models()[i].name, (int)i);
    for (u32 i = 0; i < textures().count; i++)
        add(textures_by_name_, textures()[i].name, (int)i);
    for (u32 i = 0; i < materials().count; i++)
        add(materials_by_name_, materials()[i].name, (int)i);
    for (u32 i = 0; i < anims().count; i++)
        add(anims_by_name_, anims()[i].name, (int)i);
    for (u32 i = 0; i < markers().count; i++)
        add(markers_by_name_, markers()[i].name, (int)i);
}

namespace {
int find_in(const std::unordered_map<std::string_view, int>& map, const char* name) {
    auto it = map.find(std::string_view(name ? name : ""));
    return it == map.end() ? -1 : it->second;
}
}  // namespace

int Package::find_model(const char* name) const { return find_in(models_by_name_, name); }
int Package::find_texture(const char* name) const { return find_in(textures_by_name_, name); }
int Package::find_material(const char* name) const { return find_in(materials_by_name_, name); }
int Package::find_anim(const char* name) const { return find_in(anims_by_name_, name); }
const fmt::Marker* Package::find_marker(const char* name) const {
    int i = find_in(markers_by_name_, name);
    return i < 0 ? nullptr : &markers()[(u32)i];
}

}  // namespace engine::g3d
