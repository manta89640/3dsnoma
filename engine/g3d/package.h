// A loaded NG3D package (engine/g3d/ng3d_format.h): the file in one block of native memory, checked
// and indexed. No GPU work happens here: a backend (backend.h) uploads what it needs at prepare time
// and keeps its resources in `backend_data`.
#pragma once

#include <cstdint>
#include <string>

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
    // the environment (zeroed defaults if the package has none)
    const fmt::Env& env() const { return *env_; }
    // bytes at a file offset (texel, vertex and blob data)
    const u8* at(u32 offset) const { return data_ + offset; }

    // by name: index, or -1
    int find_model(const char* name) const;
    int find_texture(const char* name) const;
    int find_material(const char* name) const;
    int find_anim(const char* name) const;
    const fmt::Marker* find_marker(const char* name) const;

    // the backend's resources for this package (backend.h), owned by the backend
    void* backend_data = nullptr;

private:
    Package() = default;
    template <class T>
    Span<T> array(u32 tag) const {
        const fmt::Section* s = section(tag);
        return s ? Span<T>{(const T*)(data_ + s->offset), s->count} : Span<T>{};
    }
    const fmt::Section* section(u32 tag) const;
    bool check(std::string* error);

    u8* data_ = nullptr;
    u32 size_ = 0;
    const fmt::Env* env_ = nullptr;
};

}  // namespace engine::g3d
