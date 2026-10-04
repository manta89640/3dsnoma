// Writing NG3D packages (engine/g3d/ng3d_format.h) from C++: the host's tools (tools/ng3d_bake.cpp)
// and tests. Sections go in in the order they are given, each on its boundary (32 bytes; TDAT 128);
// a field that holds a file offset (a texture's, a vertex buffer's, a KCL file's data) is written
// relative to the section it points into and registered with relocate(), which makes it absolute
// once the layout is known.
#pragma once

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/g3d/ng3d_format.h"

namespace engine::g3d {

using u8 = uint8_t;
using u32 = uint32_t;

class PackageWriter {
public:
    // a section: its bytes, its record count (0 for data sections), its alignment in the file
    void section(u32 tag, const void* data, u32 size, u32 count, u32 align = 32);
    template <class T>
    void array(u32 tag, const std::vector<T>& records) {
        section(tag, records.data(), (u32)(records.size() * sizeof(T)), (u32)records.size());
    }
    // the u32 `at` bytes into section `tag` is an offset into section `target`: make it absolute
    void relocate(u32 tag, u32 at, u32 target);
    // the file
    std::vector<u8> finish() const;

private:
    struct Sec {
        u32 tag, count, align;
        std::vector<u8> bytes;
    };
    struct Reloc {
        u32 tag, at, target;
    };
    std::vector<Sec> secs_;
    std::vector<Reloc> relocs_;
};

// a string table (STRS): offset 0 is "", each string once
class StringTable {
public:
    StringTable() : bytes_(1, 0) {}
    u32 add(const std::string& s);
    const std::vector<u8>& bytes() const { return bytes_; }

private:
    std::vector<u8> bytes_;
    std::unordered_map<std::string, u32> at_;
};

}  // namespace engine::g3d
