// Writing NG3D packages (package_writer.h).
#include "engine/g3d/package_writer.h"

namespace engine::g3d {

void PackageWriter::section(u32 tag, const void* data, u32 size, u32 count, u32 align) {
    Sec s;
    s.tag = tag, s.count = count, s.align = align < 32 ? 32 : align;
    s.bytes.assign((const u8*)data, (const u8*)data + size);
    secs_.push_back(std::move(s));
}

void PackageWriter::relocate(u32 tag, u32 at, u32 target) { relocs_.push_back({tag, at, target}); }

std::vector<u8> PackageWriter::finish() const {
    auto align = [](u32 v, u32 a) { return (v + a - 1) / a * a; };
    const u32 table = sizeof(fmt::Header) + (u32)secs_.size() * sizeof(fmt::Section);
    std::vector<u32> where(secs_.size());
    u32 pos = table;
    for (size_t i = 0; i < secs_.size(); i++) {
        pos = align(pos, secs_[i].align);
        where[i] = pos;
        pos += (u32)secs_[i].bytes.size();
    }
    const u32 size = align(pos, 32);
    std::vector<u8> out(size, 0);
    fmt::Header h = {};
    h.magic = fmt::MAGIC, h.version = fmt::VERSION, h.file_size = size, h.n_sections = (u32)secs_.size();
    memcpy(out.data(), &h, sizeof(h));
    for (size_t i = 0; i < secs_.size(); i++) {
        const fmt::Section s = {secs_[i].tag, where[i], (u32)secs_[i].bytes.size(), secs_[i].count};
        memcpy(out.data() + sizeof(h) + i * sizeof(s), &s, sizeof(s));
        if (!secs_[i].bytes.empty())
            memcpy(out.data() + where[i], secs_[i].bytes.data(), secs_[i].bytes.size());
    }
    auto find = [&](u32 tag) -> int {
        for (size_t i = 0; i < secs_.size(); i++)
            if (secs_[i].tag == tag)
                return (int)i;
        return -1;
    };
    for (const Reloc& r : relocs_) {
        const int s = find(r.tag), t = find(r.target);
        if (s < 0 || t < 0 || r.at + 4 > secs_[(size_t)s].bytes.size())
            continue;  // (nothing to point at: left relative, which the loader rejects)
        u32 v;
        memcpy(&v, out.data() + where[(size_t)s] + r.at, 4);
        v += where[(size_t)t];
        memcpy(out.data() + where[(size_t)s] + r.at, &v, 4);
    }
    return out;
}

u32 StringTable::add(const std::string& s) {
    if (s.empty())
        return 0;
    auto it = at_.find(s);
    if (it != at_.end())
        return it->second;
    const u32 off = (u32)bytes_.size();
    bytes_.insert(bytes_.end(), s.begin(), s.end());
    bytes_.push_back(0);
    at_[s] = off;
    return off;
}

}  // namespace engine::g3d
