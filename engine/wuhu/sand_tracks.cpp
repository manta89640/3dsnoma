// Footprints and tyre tracks in the sand (sand_tracks.h).
#include "engine/wuhu/sand_tracks.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "noma_decomp.h"
#include "runtime/runtime.h"

namespace engine::wuhu {

namespace {

using engine::collision::Hit;
using engine::math::Mtx34;

constexpr f32 DEG = 3.14159265f / 180.0f;
constexpr f32 MARK_SECONDS = 4.0f;
constexpr f32 LIFT = 0.05f;  // over the sand (island units), before the pull toward the eye

// GX SDK values for the pass
enum : u32 {
    GX_VA_POS = 9, GX_VA_CLR0 = 11, GX_DIRECT = 1, GX_POS_XYZ = 1, GX_F32 = 4, GX_CLR_RGBA = 1, GX_RGBA8 = 5,
    GX_VTXFMT0 = 0, GX_COLOR0A0 = 4, GX_SRC_REG = 0, GX_SRC_VTX = 1, GX_DF_NONE = 0, GX_AF_NONE = 2,
    GX_TEXCOORD_NULL = 0xFF, GX_TEXMAP_NULL = 0xFF, GX_CC_RASC = 10, GX_CC_ZERO = 15, GX_CA_RASA = 5,
    GX_CA_ZERO = 7, GX_TEV_ADD = 0, GX_TB_ZERO = 0, GX_CS_SCALE_1 = 0, GX_TEVPREV = 0, GX_ALWAYS = 7,
    GX_AOP_AND = 0, GX_LEQUAL = 3, GX_BM_NONE = 0, GX_BM_BLEND = 1, GX_BL_ONE = 1, GX_BL_ZERO = 0,
    GX_BL_SRCALPHA = 4, GX_BL_INVSRCALPHA = 5, GX_LO_CLEAR = 0, GX_CULL_NONE = 0, GX_PNMTX0 = 0,
    GX_TRIANGLES = 0x90,
};

bool sand(u16 a, Vec3 n) { return a == engine::collision::KCL_SAND && n.y > 0.05f; }

// a footprint: a tapered heel and a rounded toe, as a fan (x across, z along, unit size)
const f32 FOOT[6][2] = {{-0.36f, -0.5f}, {0.36f, -0.5f}, {0.5f, 0.225f}, {0.275f, 0.5f}, {-0.275f, 0.5f}, {-0.5f, 0.225f}};
const f32 PATCH[4][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
// the shades: (r, g, b, alpha at their freshest)
const u8 SHADE[2][4] = {{110, 91, 62, 105}, {92, 81, 64, 125}};

}  // namespace

bool SandTracks::stamp(Vec3 at, f32 yaw, const f32 (*corners)[2], u32 n, Mark* m) const {
    const f32 c = std::cos(yaw * DEG), s = std::sin(yaw * DEG);
    Vec3 v[6];
    for (u32 i = 0; i < n; i++) {
        // (x along the facing's right-hand x axis (cos, -sin), z along the facing (sin, cos))
        const f32 x = at.x + c * corners[i][0] + s * corners[i][1], z = at.z - s * corners[i][0] + c * corners[i][1];
        Hit h;
        if (!collision_.ground(x, z, at.y + 2.0f, at.y - 2.0f, sand, &h))
            return false;
        v[i] = {x, h.point.y + LIFT, z};
    }
    for (u32 i = 1; i + 1 < n; i++)
        m->v[m->n++] = v[0], m->v[m->n++] = v[i], m->v[m->n++] = v[i + 1];
    return true;
}

void SandTracks::add(const Mark& m) {
    if (marks_.size() >= MAX_MARKS)
        marks_.erase(marks_.begin());
    marks_.push_back(m);
}

void SandTracks::walk(Vec3 before, Vec3 now, f32 walked, f32 yaw, f32 height, bool on_foot, u32 frame) {
    if (!on_foot || walked <= 1e-3f) {
        if (!on_foot)
            trail_ = false;
        return;
    }
    if (!trail_)
        trail_ = true, walked_ = 0;
    const f32 step = std::max(0.45f, 0.28f * height);
    const f32 w = 0.047f * height, l = 0.12f * height;
    const f32 c = std::cos(yaw * DEG), s = std::sin(yaw * DEG);
    for (f32 d = step - std::fmod(walked_, step); d <= walked; d += step) {
        const f32 t = d / walked;
        Vec3 p = before + (now - before) * t;
        // (left and right feet in turn, a little to either side of the path)
        const bool left = ((u32)std::floor((walked_ + d) / step) & 1) == 0;
        const f32 off = 0.055f * height * (left ? 1.0f : -1.0f);
        p.x += c * off, p.z -= s * off;
        f32 corners[6][2];
        for (int i = 0; i < 6; i++)
            corners[i][0] = FOOT[i][0] * w * 2, corners[i][1] = FOOT[i][1] * l;
        Mark m{frame, 0, 0, {}};
        if (stamp(p, yaw, corners, 6, &m))
            add(m);
    }
    walked_ += walked;
}

void SandTracks::drive(Vec3 before, Vec3 now, f32 yaw, f32 track, f32 tyre, u32 frame) {
    const f32 dx = now.x - before.x, dz = now.z - before.z, dist = std::sqrt(dx * dx + dz * dz);
    if (dist <= 1e-3f)
        return;
    if (!car_trail_)
        car_trail_ = true, driven_ = 0;
    const f32 every = std::max(1.0f, 2.5f * tyre);
    const f32 c = std::cos(yaw * DEG), s = std::sin(yaw * DEG);
    for (f32 d = every - std::fmod(driven_, every); d <= dist; d += every) {
        const Vec3 p = before + (now - before) * (d / dist);
        Mark m{frame, 0, 1, {}};
        for (f32 side : {-0.5f, 0.5f}) {
            f32 corners[4][2];
            for (int i = 0; i < 4; i++)
                corners[i][0] = side * track + PATCH[i][0] * tyre, corners[i][1] = PATCH[i][1] * every * 1.1f;
            if (!stamp(p, yaw, corners, 4, &m))
                break;
        }
        if (m.n == 12)
            add(m);
    }
    driven_ += dist;
}

void SandTracks::draw(const engine::g3d::Camera& camera, f32 k, u32 frame) {
    const u32 life = (u32)(MARK_SECONDS * 60.0f);
    marks_.erase(std::remove_if(marks_.begin(), marks_.end(), [&](const Mark& m) { return frame - m.frame >= life; }),
                 marks_.end());
    if (marks_.empty())
        return;
    u32 vertices = 0;
    for (const Mark& m : marks_)
        vertices += m.n;
    // the island's camera, in the guest's units, as the native draws and the Mii have it
    const engine::g3d::Camera cam = engine::g3d::scaled(camera, k);
    const Mtx34 view = engine::g3d::look_at(cam);
    GFRAME(0x40);
    const u32 matrix = (u32)&GS(u32, 0x8);
    guest::write_g(matrix, &view, sizeof(view));
    g::GXLoadPosMtxImm(matrix, GX_PNMTX0);
    g::GXSetCurrentMtx(GX_PNMTX0);
    g::GXSetNumTexGens(0);
    g::GXSetNumChans(1);
    g::GXSetChanCtrl(GX_COLOR0A0, 0, GX_SRC_REG, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
    g::GXSetNumTevStages(1);
    g::GXSetNumIndStages(0);
    g::GXSetTevDirect(0);
    g::GXSetTevOrder(0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    g::GXSetTevColorIn(0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
    g::GXSetTevAlphaIn(0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
    g::GXSetTevColorOp(0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, 1, GX_TEVPREV);
    g::GXSetTevAlphaOp(0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, 1, GX_TEVPREV);
    g::GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    g::GXSetZCompLoc(1);
    g::GXSetZMode(1, GX_LEQUAL, 0);
    g::GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    g::GXSetCullMode(GX_CULL_NONE);
    g::GXSetColorUpdate(1);
    g::GXClearVtxDesc();
    g::GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    g::GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    g::GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    g::GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    g::GXBegin(GX_TRIANGLES, GX_VTXFMT0, (u16)vertices);
    auto putf = [](f32 v) {
        u32 bits;
        std::memcpy(&bits, &v, sizeof(bits));
        guest::gather_put<4>(bits);
    };
    for (const Mark& m : marks_) {
        const f32 age = (f32)(frame - m.frame) / (f32)life;
        const u8* sh = SHADE[m.shade];
        // (fading over the last half of its life)
        const u8 alpha = (u8)(std::min(1.0f, 2.0f * (1.0f - age)) * sh[3]);
        for (u32 i = 0; i < m.n; i++) {
            // pulled toward the eye by a 1024th of its distance, at most 4 units (overlay_pull's rule)
            Vec3 p = m.v[i];
            const Vec3 to_eye = camera.eye - p;
            const f32 d = length(to_eye);
            if (d > 1e-3f)
                p = p + to_eye * (std::min(d / 1024.0f, 4.0f) / d);
            putf(p.x * k), putf(p.y * k), putf(p.z * k);
            guest::gather_put<1>(sh[0]), guest::gather_put<1>(sh[1]), guest::gather_put<1>(sh[2]);
            guest::gather_put<1>(alpha);
        }
    }
    g::GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    g::GXSetZMode(1, GX_LEQUAL, 1);
}

}  // namespace engine::wuhu
