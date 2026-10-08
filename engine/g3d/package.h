// A loaded NG3D package (engine/g3d/ng3d_format.h): the file in one block of native memory, checked
// and indexed. No GPU work happens here: a backend (native_draw.h) uploads what it needs at prepare
// time and keeps its resources in `backend_data`.
//
// What load() checks, so that nothing that reads a package has to: the header, the section table
// (every section inside the file, 32-byte aligned, none overlapping another or the table, no tag
// twice), every array section large enough for its records, every string reference inside STRS
// (which starts and ends with a NUL), every index (textures, materials, texture matrices, vertex
// buffers, batches, nodes, models, cells, instances, animations, tracks, keys, lights, light sets,
// fogs) and every offset (texel data in TDAT and 128-byte aligned, vertex data in VDAT, KCL files
// in BLOB), every index buffer entry against its vertex buffer, the formats (texture formats and
// sizes, GX -> PICA conversions, vertex layouts and strides, compact positions' origin and scale),
// node trees (a parent comes before its children), LOD chains (no loops), keys (in frame order),
// the cells' visibility rows (CVIS: a row a cell, a bit for every cell), the shadows (CAST: each
// caster a run of a caster batch's indices; SHDW: one, its light in LITE) and the sea (SEAW: its
// map's size, its batches). The first problem found is
// the reason given. A version 1 package (from before compact positions, prelit materials and the
// visibility rows) is refused with that reason: the island is staged again.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/g3d/ng3d_format.h"

namespace engine::g3d {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using s16 = int16_t;
using f32 = float;

template <class T>
struct Span {
    const T* data = nullptr;
    u32 count = 0;
    const T& operator[](u32 i) const { return data[i]; }
    const T* begin() const { return data; }
    const T* end() const { return data + count; }
    bool empty() const { return count == 0; }
};

class Package {
public:
    // Takes `data` (allocated with malloc, `size` bytes; freed with free by the package). Checks the
    // header, the section table and every offset and index the sections hold. nullptr (and `error`
    // set) if the file is not a valid package; the data is freed then too.
    static Package* load(void* data, u32 size, std::string* error);
    ~Package();
    Package(const Package&) = delete;
    Package& operator=(const Package&) = delete;

    u32 size() const { return size_; }
    const u8* bytes() const { return data_; }
    // a string of STRS ("" for 0)
    const char* str(u32 offset) const;

    Span<fmt::Texture> textures() const { return array<fmt::Texture>(fmt::SEC_TEXS); }
    Span<fmt::Material> materials() const { return array<fmt::Material>(fmt::SEC_MATS); }
    Span<fmt::TexMtx> tex_mtx() const { return array<fmt::TexMtx>(fmt::SEC_TMTX); }
    Span<fmt::VertexBuffer> vertex_buffers() const { return array<fmt::VertexBuffer>(fmt::SEC_VTXB); }
    Span<fmt::Batch> batches() const { return array<fmt::Batch>(fmt::SEC_BTCH); }
    Span<fmt::Node> nodes() const { return array<fmt::Node>(fmt::SEC_NODE); }
    Span<fmt::Model> models() const { return array<fmt::Model>(fmt::SEC_MODL); }
    Span<fmt::Cell> cells() const { return array<fmt::Cell>(fmt::SEC_CELL); }
    Span<u16> cell_batches() const { return array<u16>(fmt::SEC_CBAT); }
    Span<fmt::Instance> instances() const { return array<fmt::Instance>(fmt::SEC_INST); }
    Span<fmt::Anim> anims() const { return array<fmt::Anim>(fmt::SEC_ANIM); }
    Span<fmt::Track> tracks() const { return array<fmt::Track>(fmt::SEC_TRAK); }
    Span<fmt::Key> keys() const { return array<fmt::Key>(fmt::SEC_KEYS); }
    Span<fmt::Light> lights() const { return array<fmt::Light>(fmt::SEC_LITE); }
    Span<fmt::LightSet> light_sets() const { return array<fmt::LightSet>(fmt::SEC_LSET); }
    Span<fmt::Fog> fogs() const { return array<fmt::Fog>(fmt::SEC_FOGS); }
    Span<fmt::Marker> markers() const { return array<fmt::Marker>(fmt::SEC_MARK); }
    Span<fmt::Collision> collisions() const { return array<fmt::Collision>(fmt::SEC_COLL); }
    Span<fmt::Cylinder> cylinders() const { return array<fmt::Cylinder>(fmt::SEC_CYLS); }
    Span<u16> indices() const { return array<u16>(fmt::SEC_IDAT); }
    // the placed objects that cast into the shadow map (none if the package has no CAST), and the
    // shadows' setting (nullptr if it has no SHDW: no shadow map)
    Span<fmt::Caster> casters() const { return array<fmt::Caster>(fmt::SEC_CAST); }
    const fmt::ShadowEnv* shadow_env() const { return shadow_env_; }
    // the environment (if the package has none: no sky or sea, no animations, k = 1, no grid)
    const fmt::Env& env() const { return *env_; }
    // cell i's visibility row (bit j, LSB first: cell j may be seen from cell i's walkable space, at
    // eye heights up to Env::pvs_max_height over the ground), or nullptr if the package has none
    const u8* visibility_row(u32 cell) const {
        return env_->pvs_row_bytes && cell < cells().count ? data_ + section(fmt::SEC_CVIS)->offset +
                                                                 (size_t)cell * env_->pvs_row_bytes
                                                           : nullptr;
    }

    // the sea's waves (fmt::Sea), or nullptr if the package has none; and its amplitude map
    // (amp_size[0] x amp_size[1] bytes, a row of x a z after another)
    const fmt::Sea* sea() const { return sea_; }
    const u8* sea_amplitude() const { return sea_ ? (const u8*)(sea_ + 1) : nullptr; }

    // A texture as the GPU takes it: its PICA format and every level, in place in the file, or
    // converted at load (fmt::TEX_GX_RAW, engine/g3d/texture.h) and kept beside it.
    struct TextureImage {
        u8 format, levels;
        u16 width, height;          // level 0, powers of two
        u16 src_width, src_height;  // the image the texture coordinates address (<= width, height)
        const u8* texels;
        u32 size;                   // bytes, all levels
    };
    TextureImage texture_image(u32 index) const;
    u32 converted_textures() const { return (u32)converted_.size(); }
    // bytes at a file offset (texel, vertex and blob data)
    const u8* at(u32 offset) const { return data_ + offset; }

    // by name: index, or -1
    int find_model(const char* name) const;
    int find_texture(const char* name) const;
    int find_material(const char* name) const;
    int find_anim(const char* name) const;
    const fmt::Marker* find_marker(const char* name) const;

    // the backend's resources for this package (native_draw.h's Backend), owned by the backend
    void* backend_data = nullptr;

private:
    Package() = default;
    template <class T>
    Span<T> array(u32 tag) const {
        const fmt::Section* s = section(tag);
        return s ? Span<T>{(const T*)(data_ + s->offset), s->count} : Span<T>{};
    }
    // The sections the accessors read are found once, when the section table has been checked: each
    // known tag has a slot (a constant for the accessors' constant tags), so cells(), batches() and the
    // rest are a load, not a search of the table (they are called in the World's loops: the search was
    // nearly half of its instructions on the 3DS). Other tags are searched for.
    static constexpr int tag_slot(u32 tag) {
        switch (tag) {
        case fmt::SEC_STRS: return 0;   case fmt::SEC_TEXS: return 1;   case fmt::SEC_TDAT: return 2;
        case fmt::SEC_MATS: return 3;   case fmt::SEC_TMTX: return 4;   case fmt::SEC_VTXB: return 5;
        case fmt::SEC_VDAT: return 6;   case fmt::SEC_IDAT: return 7;   case fmt::SEC_BTCH: return 8;
        case fmt::SEC_NODE: return 9;   case fmt::SEC_MODL: return 10;  case fmt::SEC_CELL: return 11;
        case fmt::SEC_CBAT: return 12;  case fmt::SEC_INST: return 13;  case fmt::SEC_ANIM: return 14;
        case fmt::SEC_TRAK: return 15;  case fmt::SEC_KEYS: return 16;  case fmt::SEC_ENVR: return 17;
        case fmt::SEC_LITE: return 18;  case fmt::SEC_LSET: return 19;  case fmt::SEC_FOGS: return 20;
        case fmt::SEC_MARK: return 21;  case fmt::SEC_COLL: return 22;  case fmt::SEC_CYLS: return 23;
        case fmt::SEC_BLOB: return 24;  case fmt::SEC_CVIS: return 25;  case fmt::SEC_CAST: return 26;
        case fmt::SEC_SHDW: return 27;  default: return -1;
        }
    }
    static constexpr int N_SLOTS = 28;
    const fmt::Section* section(u32 tag) const {
        const int i = tag_slot(tag);
        return i >= 0 ? slot_[i] : find_section(tag);
    }
    const fmt::Section* find_section(u32 tag) const;  // (the table searched)
    bool check(std::string* error);

    bool check_textures(std::string* error);
    bool check_geometry(std::string* error);
    bool check_models(std::string* error);
    bool check_world(std::string* error);
    bool check_anims(std::string* error);
    bool check_env(std::string* error);
    bool check_sea(std::string* error);
    void index_names();

    u8* data_ = nullptr;
    u32 size_ = 0;
    const fmt::Section* slot_[N_SLOTS] = {};  // (tag_slot's sections: nullptr where there's none)
    const fmt::Env* env_ = nullptr;
    const fmt::ShadowEnv* shadow_env_ = nullptr;
    const fmt::Sea* sea_ = nullptr;
    // textures converted at load: index -> its image (texels in `converted_`)
    struct Converted {
        u32 texture;
        TextureImage image;
        std::vector<u8> texels;
    };
    std::vector<Converted> converted_;
    std::vector<int> converted_of_;  // per texture: index into converted_, or -1
    std::unordered_map<std::string_view, int> models_by_name_, textures_by_name_, materials_by_name_,
        anims_by_name_, markers_by_name_;
};

}  // namespace engine::g3d
