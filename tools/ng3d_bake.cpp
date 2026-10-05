// ng3d-bake: an NG3D package's GX textures (fmt::TEX_GX_RAW: texels as Nintendo shipped them) in
// their PICA format, offline, so that the 3DS loads them as they are (no conversion at load time:
// CMPR -> ETC1 is the slow part, and pica_tex.h's conversion shares its work tables with the GX
// renderer's). Every level the PICA takes is converted (engine/g3d/texture.cpp, the GX path's own
// conversion), the flag cleared, the texel data written again (each texture 128-byte aligned) and the
// file offsets of the other sections moved with their sections. The output is loaded and checked
// before it is written.
//
//   ng3d-bake in.ng3d out.ng3d
//
// Built with the host build (CMake target ng3d-bake).
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "engine/g3d/package.h"
#include "engine/g3d/package_writer.h"
#include "engine/g3d/texture.h"

using namespace engine::g3d;

namespace {

bool read_file(const char* path, std::vector<u8>& out) {
    FILE* f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    const bool ok = n >= 0 && fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

Package* load(const std::vector<u8>& bytes, std::string* error) {
    void* block = malloc(bytes.size() ? bytes.size() : 1);
    memcpy(block, bytes.data(), bytes.size());
    return Package::load(block, (u32)bytes.size(), error);
}

const char* tag_name(u32 tag) {
    static char s[5];
    memcpy(s, &tag, 4), s[4] = 0;
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: ng3d-bake in.ng3d out.ng3d\n");
        return 2;
    }
    std::vector<u8> in;
    if (!read_file(argv[1], in)) {
        fprintf(stderr, "ng3d-bake: can't read %s\n", argv[1]);
        return 1;
    }
    std::string error;
    Package* pkg = load(in, &error);  // (converts the GX textures)
    if (!pkg) {
        fprintf(stderr, "ng3d-bake: %s: %s\n", argv[1], error.c_str());
        return 1;
    }
    const fmt::Header& h = *(const fmt::Header*)pkg->bytes();
    const fmt::Section* table = (const fmt::Section*)(pkg->bytes() + sizeof(fmt::Header));
    auto section = [&](u32 tag) -> const fmt::Section* {
        for (u32 i = 0; i < h.n_sections; i++)
            if (table[i].tag == tag)
                return &table[i];
        return nullptr;
    };

    // the textures and their texel data, new
    const Span<fmt::Texture> texs = pkg->textures();
    std::vector<fmt::Texture> new_texs(texs.begin(), texs.end());
    std::vector<u8> tdat;
    u32 converted = 0, bytes_before = 0, bytes_after = 0;
    u32 kb_by_format[16] = {};
    for (u32 i = 0; i < texs.count; i++) {
        const Package::TextureImage img = pkg->texture_image(i);
        fmt::Texture& t = new_texs[i];
        if (t.flags & fmt::TEX_GX_RAW) {
            converted++;
            bytes_before += t.size;
            bytes_after += img.size;
        }
        t.format = img.format, t.levels = img.levels, t.width = img.width, t.height = img.height;
        t.flags &= (u8)~fmt::TEX_GX_RAW;
        tdat.resize((tdat.size() + 127) / 128 * 128);
        t.data = (u32)tdat.size();  // (relative to TDAT: relocated)
        t.size = img.size;
        tdat.insert(tdat.end(), img.texels, img.texels + img.size);
        kb_by_format[img.format & 15] += img.size;
    }

    // every section in the order the input has them: TEXS and TDAT new, the vertex buffers' and
    // KCL files' offsets made relative to their sections (relocated), the rest as it was
    PackageWriter w;
    const fmt::Section* vdat = section(fmt::SEC_VDAT);
    const fmt::Section* blob = section(fmt::SEC_BLOB);
    for (u32 i = 0; i < h.n_sections; i++) {
        const fmt::Section& s = table[i];
        const u8* bytes = pkg->bytes() + s.offset;
        switch (s.tag) {
        case fmt::SEC_TEXS:
            w.array(fmt::SEC_TEXS, new_texs);
            for (u32 k = 0; k < new_texs.size(); k++)
                w.relocate(fmt::SEC_TEXS, k * (u32)sizeof(fmt::Texture) + (u32)offsetof(fmt::Texture, data), fmt::SEC_TDAT);
            break;
        case fmt::SEC_TDAT: w.section(fmt::SEC_TDAT, tdat.data(), (u32)tdat.size(), 0, 128); break;
        case fmt::SEC_VTXB: {
            std::vector<fmt::VertexBuffer> vbs(pkg->vertex_buffers().begin(), pkg->vertex_buffers().end());
            for (u32 k = 0; k < vbs.size(); k++) {
                vbs[k].data -= vdat->offset;
                w.relocate(fmt::SEC_VTXB, k * (u32)sizeof(fmt::VertexBuffer) + (u32)offsetof(fmt::VertexBuffer, data),
                           fmt::SEC_VDAT);
            }
            w.array(fmt::SEC_VTXB, vbs);
            break;
        }
        case fmt::SEC_COLL: {
            std::vector<fmt::Collision> cs(pkg->collisions().begin(), pkg->collisions().end());
            for (u32 k = 0; k < cs.size(); k++) {
                cs[k].data -= blob->offset;
                w.relocate(fmt::SEC_COLL, k * (u32)sizeof(fmt::Collision) + (u32)offsetof(fmt::Collision, data),
                           fmt::SEC_BLOB);
            }
            w.array(fmt::SEC_COLL, cs);
            break;
        }
        default: {
            // (a section this tool doesn't know might hold file offsets it can't move)
            static const u32 k_known[] = {fmt::SEC_STRS, fmt::SEC_MATS, fmt::SEC_TMTX, fmt::SEC_VDAT, fmt::SEC_IDAT,
                                          fmt::SEC_BTCH, fmt::SEC_NODE, fmt::SEC_MODL, fmt::SEC_CELL, fmt::SEC_CBAT,
                                          fmt::SEC_INST, fmt::SEC_ANIM, fmt::SEC_TRAK, fmt::SEC_KEYS, fmt::SEC_ENVR,
                                          fmt::SEC_LITE, fmt::SEC_LSET, fmt::SEC_FOGS, fmt::SEC_MARK, fmt::SEC_CYLS,
                                          fmt::SEC_BLOB, fmt::SEC_CVIS};
            bool known = false;
            for (u32 t : k_known)
                known |= t == s.tag;
            if (!known)
                fprintf(stderr, "ng3d-bake: warning: section %s copied as it is (unknown to this tool)\n", tag_name(s.tag));
            w.section(s.tag, bytes, s.size, s.count);
            break;
        }
        }
    }
    const std::vector<u8> out = w.finish();

    // the output must load, with the same textures (now in place) and everything else as it was
    Package* check = load(out, &error);
    if (!check) {
        fprintf(stderr, "ng3d-bake: the baked package doesn't load: %s\n", error.c_str());
        return 1;
    }
    bool same = check->converted_textures() == 0 && check->textures().count == texs.count;
    for (u32 i = 0; same && i < texs.count; i++) {
        const Package::TextureImage a = pkg->texture_image(i), b = check->texture_image(i);
        same = a.format == b.format && a.levels == b.levels && a.width == b.width && a.height == b.height &&
               a.size == b.size && memcmp(a.texels, b.texels, a.size) == 0;
    }
    for (u32 i = 0; same && i < pkg->vertex_buffers().count; i++) {
        const fmt::VertexBuffer &a = pkg->vertex_buffers()[i], &b = check->vertex_buffers()[i];
        same = memcmp(pkg->at(a.data), check->at(b.data), a.count * a.stride) == 0;
    }
    for (u32 i = 0; same && i < pkg->collisions().count; i++) {
        const fmt::Collision &a = pkg->collisions()[i], &b = check->collisions()[i];
        same = a.size == b.size && memcmp(pkg->at(a.data), check->at(b.data), a.size) == 0;
    }
    same = same && check->batches().count == pkg->batches().count && check->materials().count == pkg->materials().count;
    if (!same) {
        fprintf(stderr, "ng3d-bake: the baked package differs from the input beyond its textures\n");
        return 1;
    }
    FILE* f = fopen(argv[2], "wb");
    if (!f || fwrite(out.data(), 1, out.size(), f) != out.size()) {
        fprintf(stderr, "ng3d-bake: can't write %s\n", argv[2]);
        return 1;
    }
    fclose(f);
    printf("ng3d-bake: %s -> %s: %u textures, %u converted from GX (%u KB -> %u KB); %zu -> %zu bytes\n", argv[1],
           argv[2], texs.count, converted, bytes_before >> 10, bytes_after >> 10, in.size(), out.size());
    static const char* const k_names[14] = {"RGBA8", "RGB8", "RGBA5551", "RGB565", "RGBA4", "LA8", "HILO8",
                                            "L8",    "A8",   "LA4",      "L4",     "A4",    "ETC1", "ETC1A4"};
    printf("ng3d-bake: texel KB by format:");
    for (u32 f2 = 0; f2 < 14; f2++)
        if (kb_by_format[f2])
            printf(" %s %u", k_names[f2], kb_by_format[f2] >> 10);
    printf("\n");
    delete check;
    delete pkg;
    return 0;
}
