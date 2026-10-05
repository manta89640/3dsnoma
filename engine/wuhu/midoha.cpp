// ミドハ's head and dress (midoha.h).
#include "engine/wuhu/midoha.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "engine/g3d/native_draw.h"
#include "engine/ui/g3d.h"
#include "noma_decomp.h"

namespace engine::wuhu {

namespace g3d = engine::ui::g3d;
using engine::math::Mtx34;
using engine::math::Vec3;

namespace {

const char* const LOOK_FILE = "/wuhu/midoha.ng3d";
const char* const STAGE_COMMAND = "tools/mii/midoha_parts.py DATA";

}  // namespace

const midoha::Candidate* midoha_candidate() {
    const char* e = std::getenv("NOMA_WUHU_MIDOHA");
    if (!e || !*e || !std::strcmp(e, "0"))
        return nullptr;
    // (1: the recommended one, the last)
    const midoha::Candidate& last = midoha::CANDIDATES[midoha::N_CANDIDATES - 1];
    const char want = !std::strcmp(e, "1") ? last.letter : (char)(e[0] & ~0x20);
    for (const midoha::Candidate& c : midoha::CANDIDATES)
        if (c.letter == want)
            return &c;
    guest::log("midoha: NOMA_WUHU_MIDOHA=%s: no such candidate (A to %c): %c", e, last.letter, last.letter);
    return &last;
}

// ---------------------------------------------------------------- the head

MidohaLook::~MidohaLook() {
    if (prepared_)
        engine::g3d::native_draw_release(*package_);
    world_.reset();
    package_.reset();
}

bool MidohaLook::load() {
    const char* e = std::getenv("NOMA_MIDOHA_PACKAGE");
    const std::string path = e && *e ? std::string(e) : guest::data_dir() + LOOK_FILE;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        guest::log("midoha: there is no %s (make it: %s): his head as a Mii's", path.c_str(), STAGE_COMMAND);
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    void* data = n > 0 ? std::malloc((size_t)n) : nullptr;
    const bool read = data && std::fread(data, 1, (size_t)n, f) == (size_t)n;
    std::fclose(f);
    if (!read) {
        std::free(data);
        guest::log("midoha: %s can't be read", path.c_str());
        return false;
    }
    std::string error;
    engine::g3d::Package* p = engine::g3d::Package::load(data, (u32)n, &error);  // (takes the block)
    if (!p || p->instances().count == 0) {
        guest::log("midoha: %s: %s", path.c_str(), p ? "not his head (no instance)" : error.c_str());
        delete p;
        return false;
    }
    package_.reset(p);
    world_ = std::make_unique<engine::g3d::World>(*package_);
    for (const engine::g3d::Material& m : world_->materials())
        translucent_ |= m.translucent;
    engine::g3d::native_draw_prepare(*package_);
    prepared_ = true;
    u32 tris = 0;
    for (const engine::g3d::fmt::Batch& b : package_->batches())
        tris += b.n_indices / 3;
    guest::log("midoha: head pieces from %s: %u bytes, %u models, %u triangles, %u textures", path.c_str(),
               (unsigned)n, (unsigned)package_->models().count, (unsigned)tris, (unsigned)package_->textures().count);
    return true;
}

void MidohaLook::update(const Mtx34& head, const engine::g3d::Camera& camera, f32 frame) {
    if (!world_)
        return;
    for (u32 i = 0; i < package_->instances().count; i++)
        world_->set_instance_matrix(i, head);
    world_->update(frame);
    world_->set_camera(camera);
}

// (a pass with nothing in it isn't sent: the packet ring is shared with the island's two a frame)
void MidohaLook::emit(engine::g3d::Pass pass) {
    if (world_ && (pass == engine::g3d::Pass::OPAQUE || translucent_))
        world_->emit(pass);
}

// ---------------------------------------------------------------- the face

namespace {

const char* const FACE_FILE = "/wuhu/midoha_face.bin";
const u32 GX_TF_RGB5A3 = 5;

// an expression's face texture in his char model (0: none), its size and format (GXTexObj: +0x8
// width - 1, height - 1, format; +0xC the image's address / 32)
struct FaceTexture {
    u32 image = 0, width = 0, height = 0, format = 0;
};
FaceTexture face_texture(u32 actor, u32 expression) {
    FaceTexture t;
    const u32 obj = guest::load32(engine::ui::g3d::actor_face(actor) + 0x6C + 4 * expression);
    if (!obj)
        return t;
    const u32 image0 = guest::load32(obj + 0x8), image3 = guest::load32(obj + 0xC);
    t.width = (image0 & 0x3FF) + 1, t.height = (image0 >> 10 & 0x3FF) + 1, t.format = image0 >> 20 & 0xF;
    t.image = 0x80000000u | (image3 & 0xFFFFFF) << 5;
    return t;
}

}  // namespace

// The file (tools/mii/midoha_face.py): "MDFC", u16 width, u16 height, u8 images, u8 image of each of
// the 7 expressions, then the images, GX RGB5A3 (4x4 tiles, big-endian), width x height each.
bool MidohaFace::load() {
    const char* e = std::getenv("NOMA_MIDOHA_FACE");
    if (e && !std::strcmp(e, "0")) {
        guest::log("midoha: face: RFL's own (NOMA_MIDOHA_FACE=0)");
        return false;
    }
    const std::string path = e && *e ? std::string(e) : guest::data_dir() + FACE_FILE;
    std::vector<u8> file;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        file.resize(n > 0 ? (size_t)n : 0);
        if (!file.empty() && std::fread(file.data(), 1, file.size(), f) != file.size())
            file.clear();
        std::fclose(f);
    }
    const u32 head = 4 + 2 + 2 + 1 + N_EXPRESSIONS;
    u16 w = 0, h = 0;
    u8 n_images = 0;
    if (file.size() >= head && !std::memcmp(file.data(), "MDFC", 4))
        std::memcpy(&w, &file[4], 2), std::memcpy(&h, &file[6], 2), n_images = file[8];
    const u32 bytes = (u32)w * h * 2;
    bool ok = w && h && !(w & 3) && !(h & 3) && n_images && file.size() >= head + (size_t)n_images * bytes;
    for (u32 i = 0; ok && i < N_EXPRESSIONS; i++)
        ok = file[9 + i] < n_images;
    if (!ok) {
        guest::log("midoha: face: %s missing or damaged (make it: tools/mii/midoha_face.py DATA): his Wii parts",
                   path.c_str());
        return false;
    }
    width_ = w, height_ = h, image_bytes_ = bytes;
    std::memcpy(image_of_, &file[9], N_EXPRESSIONS);
    images_.assign(file.begin() + head, file.begin() + head + (size_t)n_images * bytes);
    guest::log("midoha: face: %ux%u, %u images from %s", (unsigned)w, (unsigned)h, (unsigned)n_images, path.c_str());
    return true;
}

void MidohaFace::apply(u32 actor) {
    if (images_.empty() || !actor)
        return;
    for (u32 x = 0; x < N_EXPRESSIONS; x++) {
        const FaceTexture t = face_texture(actor, x);
        if (!t.image)
            continue;
        if (t.width != width_ || t.height != height_ || t.format != GX_TF_RGB5A3) {
            if (!warned_)
                guest::log("midoha: face: RFL's face texture is %ux%u (format %u), the file's %ux%u: his Wii parts",
                           (unsigned)t.width, (unsigned)t.height, (unsigned)t.format, (unsigned)width_,
                           (unsigned)height_);
            warned_ = true;
            return;
        }
        // ours still there? (a word every 4 KB: an EFB copy writes the whole image)
        const u8* ours = &images_[(size_t)image_of_[x] * image_bytes_];
        bool same = true;
        for (u32 at = 0; same && at < image_bytes_; at += 4096) {
            u32 word;
            std::memcpy(&word, ours + at, 4);
            same = guest::load32(t.image + at) == __builtin_bswap32(word);
        }
        if (same)
            continue;
        guest::write_g(t.image, ours, image_bytes_);
        if (writes_++ < 14)
            guest::log("midoha: face: expression %u written (image %u at %08X)", (unsigned)x, (unsigned)image_of_[x],
                       (unsigned)t.image);
    }
}

bool MidohaFace::dump(u32 actor, const std::string& dir) {
    u32 n = 0;
    for (u32 x = 0; x < N_EXPRESSIONS; x++) {
        const FaceTexture t = face_texture(actor, x);
        if (!t.image || t.width > 1024 || t.height > 1024)
            continue;
        const u32 bytes = t.width * t.height * 2;  // (RGB5A3)
        std::vector<u8> out(10 + bytes);
        std::memcpy(&out[0], "MDFD", 4);
        const u16 w = (u16)t.width, h = (u16)t.height, f = (u16)t.format;
        std::memcpy(&out[4], &w, 2), std::memcpy(&out[6], &h, 2), std::memcpy(&out[8], &f, 2);
        guest::read_g(&out[10], t.image, bytes);
        char path[64];
        std::snprintf(path, sizeof(path), "/midoha_face_rfl_%u.bin", (unsigned)x);
        if (FILE* fp = std::fopen((dir + path).c_str(), "wb")) {
            std::fwrite(out.data(), 1, out.size(), fp);
            std::fclose(fp);
            n++;
        }
    }
    guest::log("midoha: face: RFL's %u face textures written to %s", (unsigned)n, dir.c_str());
    return n > 0;
}

}  // namespace engine::wuhu
