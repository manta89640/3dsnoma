// A shape as static GPU buffers (see live_mesh.h). GX primitives become triangle lists with GX's
// winding (a strip's odd triangles turned back, a fan's and a quad's around their first vertex);
// a skinned shape's matrices are followed through its primitive list's loads (a slot holds the
// view matrix last loaded into it), and its triangles go into batches greedily, a batch's bones at
// most max_bones (a triangle has at most three).
#include "engine/g3d/live_mesh.h"

#include <cstring>
#include <unordered_map>

namespace engine::g3d::live {

namespace {

// one vertex's bytes in the mesh's layout, with its bone in the batch
struct Key {
    u8 b[96];
    u32 n;
    bool operator==(const Key& o) const { return n == o.n && !memcmp(b, o.b, n); }
};
struct KeyHash {
    size_t operator()(const Key& k) const {
        size_t h = 1469598103934665603ull;
        for (u32 i = 0; i < k.n; i++)
            h = (h ^ k.b[i]) * 1099511628211ull;
        return h;
    }
};

// a decoded vertex resolved: its index in the shape's vertices and the view matrix it is drawn with
struct Corner {
    u32 v;
    u16 bone;
};

}  // namespace

LiveMesh build_mesh(const DecodedShape& s, s32 cur_mtx, u32 max_bones) {
    LiveMesh m;
    if (s.error)
        return m.error = s.error, m;
    const bool skinned = cur_mtx < 0;
    if (skinned && !(s.attrs & 1))
        return m.error = "no current matrix and no matrix indices", m;
    m.skinned = skinned;
    // the layout
    u32 off = 0;
    auto take = [&](u32 attr, u32 bit, s16& at, u32 size) {
        if (s.attrs & bit) {
            m.attrs |= attr;
            at = (s16)off;
            off += size;
        }
    };
    take(MESH_POS, 1u << 9, m.off_pos, 12);
    take(MESH_NRM, 1u << 10, m.off_nrm, 12);
    take(MESH_CLR0, 1u << 11, m.off_clr[0], 4);
    take(MESH_CLR1, 1u << 12, m.off_clr[1], 4);
    for (int t = 0; t < 8; t++)
        take(MESH_UV0 << t, 1u << (13 + t), m.off_uv[t], 8);
    if (skinned)
        m.attrs |= MESH_BONE, m.off_bone = (s16)off, off += 4;
    m.stride = off;
    if (!(m.attrs & MESH_POS))
        return m.error = "no positions", m;

    // the triangles' corners, with their matrices
    std::vector<Corner> corners;
    std::unordered_map<u32, u16> slots;  // (array << 16 | XF address) -> view matrix index
    bool tex_bones = true, has_tex_mtx = false;
    for (const ShapePrim& pr : s.prims) {
        for (u32 k = 0; k < pr.n_loads; k++) {
            const ShapeMtxLoad& l = s.loads[pr.loads_first + k];
            slots[(u32)l.array << 16 | l.xf_addr] = l.index;
        }
        auto bone_of = [&](u32 vi, u16& bone) {
            if (!skinned) {
                bone = (u16)cur_mtx;
                return true;
            }
            const ShapeVertex& v = s.verts[vi];
            auto it = slots.find(0u << 16 | (u32)v.pnmtx * 4);
            if (v.pnmtx == 0xFF || it == slots.end())
                return false;
            bone = it->second;
            for (int t = 0; t < 8; t++)
                if (v.texmtx[t] != 0xFF) {
                    has_tex_mtx = true;
                    auto jt = slots.find(2u << 16 | (u32)v.texmtx[t] * 4);
                    if (jt == slots.end() || jt->second != bone)
                        tex_bones = false;
                }
            return true;
        };
        auto corner = [&](u32 i, Corner& c) {
            c.v = pr.first + i;
            return bone_of(c.v, c.bone);
        };
        auto tri = [&](u32 a, u32 b, u32 c) {
            Corner x[3];
            if (!corner(a, x[0]) || !corner(b, x[1]) || !corner(c, x[2]))
                return false;
            corners.insert(corners.end(), x, x + 3);
            return true;
        };
        bool ok = true;
        switch (pr.type) {
        case 0x80:  // quads
            for (u32 i = 0; ok && i + 3 < pr.count; i += 4)
                ok = tri(i, i + 1, i + 2) && tri(i, i + 2, i + 3);
            break;
        case 0x90:  // triangles
            for (u32 i = 0; ok && i + 2 < pr.count; i += 3)
                ok = tri(i, i + 1, i + 2);
            break;
        case 0x98:  // strip: the odd triangles turned back
            for (u32 i = 0; ok && i + 2 < pr.count; i++)
                ok = i & 1 ? tri(i + 1, i, i + 2) : tri(i, i + 1, i + 2);
            break;
        case 0xA0:  // fan
            for (u32 i = 1; ok && i + 1 < pr.count; i++)
                ok = tri(0, i, i + 1);
            break;
        default:
            return m.error = "lines or points", m;
        }
        if (!ok)
            return m.error = "a vertex whose matrix the list never loaded", m;
    }
    if (skinned && has_tex_mtx && !tex_bones)
        return m.error = "texture matrices that don't follow the position matrices", m;
    m.tex_bones = skinned && has_tex_mtx;

    // batches: vertices once each (by their bytes and bone), bones at most max_bones a batch
    std::unordered_map<Key, u16, KeyHash> index_of;
    std::vector<u16> batch_bones;
    LiveMesh::Batch b{};
    auto close = [&]() {
        if (b.n_indices) {
            b.n_vertices = (u32)(m.vertices.size() / m.stride) - b.first_vertex;
            b.first_bone = (u32)m.bones.size(), b.n_bones = (u32)batch_bones.size();
            m.bones.insert(m.bones.end(), batch_bones.begin(), batch_bones.end());
            m.batches.push_back(b);
        }
        b = LiveMesh::Batch{};
        b.first_index = (u32)m.indices.size();
        b.first_vertex = (u32)(m.vertices.size() / m.stride);
        batch_bones.clear();
        index_of.clear();
    };
    close();
    auto local_bone = [&](u16 bone) {
        for (u32 i = 0; i < batch_bones.size(); i++)
            if (batch_bones[i] == bone)
                return (s32)i;
        return -1;
    };
    for (size_t t = 0; t < corners.size(); t += 3) {
        if ((m.vertices.size() / m.stride) - b.first_vertex > 0xFFFC)
            close();
        if (skinned) {
            u32 more = 0;
            u16 seen[3];
            for (int k = 0; k < 3; k++) {
                const u16 bone = corners[t + k].bone;
                bool counted = local_bone(bone) >= 0;
                for (u32 j = 0; j < more; j++)
                    counted |= seen[j] == bone;
                if (!counted)
                    seen[more++] = bone;
            }
            if (batch_bones.size() + more > max_bones)
                close();
            for (int k = 0; k < 3; k++)
                if (local_bone(corners[t + k].bone) < 0)
                    batch_bones.push_back(corners[t + k].bone);
        }
        for (int k = 0; k < 3; k++) {
            const Corner& c = corners[t + k];
            const ShapeVertex& v = s.verts[c.v];
            Key key;
            memset(key.b, 0, sizeof(key.b));
            key.n = m.stride;
            if (m.off_pos >= 0)
                memcpy(key.b + m.off_pos, v.pos, 12);
            if (m.off_nrm >= 0)
                memcpy(key.b + m.off_nrm, v.nrm, 12);
            for (int ch = 0; ch < 2; ch++)
                if (m.off_clr[ch] >= 0)
                    memcpy(key.b + m.off_clr[ch], v.clr[ch], 4);
            for (int u = 0; u < 8; u++)
                if (m.off_uv[u] >= 0)
                    memcpy(key.b + m.off_uv[u], v.uv[u], 8);
            if (m.off_bone >= 0)
                key.b[m.off_bone] = (u8)local_bone(c.bone);
            auto it = index_of.find(key);
            u16 idx;
            if (it != index_of.end()) {
                idx = it->second;
            } else {
                idx = (u16)((m.vertices.size() / m.stride) - b.first_vertex);
                m.vertices.insert(m.vertices.end(), key.b, key.b + m.stride);
                index_of.emplace(key, idx);
            }
            m.indices.push_back(idx);
            b.n_indices++;
        }
    }
    close();
    return m;
}

}  // namespace engine::g3d::live
