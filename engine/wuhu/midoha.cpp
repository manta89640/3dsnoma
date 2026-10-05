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
    const char want = !std::strcmp(e, "1") ? 'A' : (char)(e[0] & ~0x20);
    for (const midoha::Candidate& c : midoha::CANDIDATES)
        if (c.letter == want)
            return &c;
    guest::log("midoha: NOMA_WUHU_MIDOHA=%s: no such candidate (A, B or C): A", e);
    return &midoha::CANDIDATES[0];
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
    if (!p || p->instances().count == 0 || p->textures().count != 0) {
        guest::log("midoha: %s: %s", path.c_str(), p ? "not his head (an instance, no textures)" : error.c_str());
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
    guest::log("midoha: head pieces from %s: %u bytes, %u models, %u triangles", path.c_str(), (unsigned)n,
               (unsigned)package_->models().count, (unsigned)tris);
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

}  // namespace engine::wuhu
