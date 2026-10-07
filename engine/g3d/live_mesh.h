// A shape of the game's models as static GPU buffers (engine/g3d/live_draw.cpp): its primitive
// list's triangles (u16 indices, in the list's order) over its vertices (each once per batch),
// and the matrices they are drawn with: a single-matrix shape's one, or a skinned shape's in
// batches of at most `max_bones` bones (view matrices), each vertex naming its bone in the batch.
// A skinned shape's vertices whose texture coordinates have their own matrices (the Mii body's
// environment map: a matrix a bone, loaded from the view texture matrix array) take their bone's.
#pragma once

#include <vector>

#include "engine/g3d/live_shape.h"

namespace engine::g3d::live {

// vertex attributes, in this order in a vertex: f32 positions (3), f32 normals (3), u8 colours
// (RGBA) 0 and 1, f32 texture coordinates (2) 0-7, u8 bone (and 3 pad bytes)
enum : u32 {
    MESH_POS = 1, MESH_NRM = 2, MESH_CLR0 = 4, MESH_CLR1 = 8,
    MESH_UV0 = 16,  // MESH_UV0 << n: set n
    MESH_BONE = 1u << 24,
};

struct LiveMesh {
    const char* error = nullptr;  // why the shape can't be drawn this way (nullptr: it can)
    u32 attrs = 0;                // MESH_*
    u32 stride = 0;
    s16 off_pos = -1, off_nrm = -1, off_clr[2] = {-1, -1}, off_uv[8] = {-1, -1, -1, -1, -1, -1, -1, -1}, off_bone = -1;
    std::vector<u8> vertices;
    std::vector<u16> indices;
    struct Batch {
        u32 first_index, n_indices;  // in `indices`
        u32 first_vertex, n_vertices;  // the vertices its indices use (indices count from first_vertex)
        u32 first_bone, n_bones;       // in `bones`
    };
    std::vector<Batch> batches;
    // the view matrices (the view arrays' index) of the batches' bones; a single-matrix shape's
    // one batch has none (its current matrix)
    std::vector<u16> bones;
    bool skinned = false;
    bool tex_bones = false;  // the texture matrices follow the bones (texture matrix array, by the same index)
    u32 triangles() const { return (u32)indices.size() / 3; }
};

// the mesh of decoded shape `s` (a single-matrix shape when cur_mtx >= 0)
LiveMesh build_mesh(const DecodedShape& s, s32 cur_mtx, u32 max_bones);

}  // namespace engine::g3d::live
