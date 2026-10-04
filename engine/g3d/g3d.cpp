// nw4r::g3d pieces, native:
//  - detail::Copy32ByteBlocks: the 32-byte-block copy g3d uses for matrix and light arrays (an FPU
//    double loop on the Wii: here a memory copy)
//  - LightSetting::Import: a scene's lights copied from another setting, every frame
//  - Camera's matrix getters and Fog::CopyTo: ScnRoot's global settings copy all 32 cameras' and
//    fogs' matrices and data into the draw state before every opaque and translucent pass
#include "noma_decomp.h"
#include "../snd/guest_obj.h"

namespace engine::g3d {

using engine::snd::GuestObj;

namespace {

// the bytes the original copies: whole 32-byte blocks, forward, 8 at a time
void copy_32byte_blocks(u32 dst, u32 src, u32 size) {
    u32 n = size & ~31u;
    if (n == 0)
        return;
    if (dst > src && dst - src < n) {
        // overlapping with the destination ahead: the original's forward order repeats bytes
        for (u32 i = 0; i < n; i += 8) {
            u32 a = guest::load32(src + i), b = guest::load32(src + i + 4);
            guest::store32(dst + i, a);
            guest::store32(dst + i + 4, b);
        }
        return;
    }
    guest::memcpy_g(dst, src, n);
}

// LightSetting: {u16 numLightObj; u16 numLightSet; LightObj*; AmbLightObj*; LightSetData*}
enum : u32 { LS_NUM_OBJ = 0x0, LS_NUM_SET = 0x2, LS_OBJ = 0x4, LS_AMB = 0x8, LS_SET = 0xC };
// LightObj: {u32 flags; GXLightObj obj (0x40)}; AmbLightObj: 4 bytes; LightSetData: 0xC bytes
const u32 LIGHT_OBJ_SIZE = 0x44, LIGHT_SET_SIZE = 0xC;
const u32 LIGHT_ENABLED = 4;

void light_setting_import(u32 self, u32 rhs) {
    GuestObj d(self), s(rhs);
    if (d.get16(LS_NUM_OBJ) < s.get16(LS_NUM_OBJ) || d.get16(LS_NUM_SET) < s.get16(LS_NUM_SET))
        return;
    for (u32 i = 0; i < s.get16(LS_NUM_OBJ); i++) {
        // LightObj::operator=: an enabled light is copied, a disabled one only disables
        u32 so = s.get32(LS_OBJ) + i * LIGHT_OBJ_SIZE;
        u32 flags = guest::load32(so);
        if (flags & LIGHT_ENABLED) {
            u32 dobj = d.get32(LS_OBJ) + i * LIGHT_OBJ_SIZE;
            if (dobj != so) {
                guest::store32(dobj, flags);
                copy_32byte_blocks(dobj + 4, so + 4, 0x40);
            }
        } else {
            u32 dobj = d.get32(LS_OBJ) + i * LIGHT_OBJ_SIZE;
            guest::store32(dobj, guest::load32(dobj) & ~LIGHT_ENABLED);
        }
        u32 sa = s.get32(LS_AMB) + i * 4, da = d.get32(LS_AMB) + i * 4;
        for (u32 k = 0; k < 4; k++)
            guest::store8(da + k, guest::load8(sa + k));
    }
    for (u32 i = 0; i < s.get16(LS_NUM_SET); i++) {
        u32 ss = s.get32(LS_SET) + i * LIGHT_SET_SIZE, ds = d.get32(LS_SET) + i * LIGHT_SET_SIZE;
        u32 w0 = guest::load32(ss), w1 = guest::load32(ss + 4);
        guest::store32(ds, w0);
        guest::store32(ds + 4, w1);
        guest::store8(ds + 8, guest::load8(ss + 8));
        guest::store16(ds + 9, guest::load16(ss + 9));
        guest::store8(ds + 0xB, guest::load8(ss + 0xB));
    }
}

}  // namespace

// a Camera is a pointer to its data (the matrices, flags at 0x70, the projection's parameters)
inline f64 camf(u32 cam, u32 off) {
    u32 b = guest::load32(cam + off);
    f32 f;
    ::memcpy(&f, &b, 4);
    return f;
}

// Camera::GetCameraMtx(mtx): the view matrix, made again first if the camera changed
u32 camera_get_camera_mtx(u32 camera, u32 dst) {
    const u32 cam = dst ? guest::load32(camera) : 0;
    if (!cam)
        return camera;
    if (!(guest::load32(cam + 0x70) & 8))
        g::nw4r_g3d_Camera_UpdateCameraMtx(camera);
    g::PSMTXCopy(cam, dst);
    return cam;
}

// Camera::GetProjectionMtx(mtx): the projection (orthographic, frustum or perspective), made once
// and kept at 0x30 until the camera changes
void camera_get_projection_mtx(u32 camera, u32 dst) {
    const u32 cam = dst ? guest::load32(camera) : 0;
    if (!cam)
        return;
    const u32 flags = guest::load32(cam + 0x70);
    if (!(flags & 0x80)) {
        if (flags & 0x40)
            g::C_MTXOrtho(cam + 0x30, camf(cam, 0xBC), camf(cam, 0xC0), camf(cam, 0xC4), camf(cam, 0xC8), camf(cam, 0xB4),
                          camf(cam, 0xB8));
        else if (flags & 0x10)
            g::C_MTXFrustum(cam + 0x30, camf(cam, 0xBC), camf(cam, 0xC0), camf(cam, 0xC4), camf(cam, 0xC8),
                            camf(cam, 0xB4), camf(cam, 0xB8));
        else
            g::C_MTXPerspective(cam + 0x30, camf(cam, 0xAC), camf(cam, 0xB0), camf(cam, 0xB4), camf(cam, 0xB8));
        guest::store32(cam + 0x70, guest::load32(cam + 0x70) | 0x80);
    }
    g::nw4r_math_MTX44Copy(dst, cam + 0x30);
}

// Camera::GetEnvironmentTexMtx(mtx): the identity with the camera's texture scale and offset
void camera_get_environment_tex_mtx(u32 camera, u32 dst) {
    const u32 cam = dst ? guest::load32(camera) : 0;
    if (!cam)
        return;
    g::PSMTXIdentity(dst, dst);
    guest::store32(dst + 0x0, guest::load32(cam + 0xCC));
    guest::store32(dst + 0xC, guest::load32(cam + 0xD4));
    guest::store32(dst + 0x14, guest::load32(cam + 0xD0) ^ 0x80000000u);  // (a float negated: its sign bit)
    guest::store32(dst + 0x1C, guest::load32(cam + 0xD8));
    guest::store32(dst + 0x28, guest::load32(GA_lbl_80886A58));
    guest::store32(dst + 0x2C, guest::load32(GA_lbl_80886A60));
}

// Fog::CopyTo(data): the fog's 48 bytes
void fog_copy_to(u32 fog, u32 dst) {
    const u32 src = dst ? guest::load32(fog) : 0;
    if (src)
        guest::memcpy_g(dst, src, 0x30);
}

}  // namespace engine::g3d

// ---- the guest ABI (decl.h)
namespace g {

void nw4r_g3d_detail_Copy32ByteBlocks(u32 dst, u32 src, u32 size) {
    engine::g3d::copy_32byte_blocks(dst, src, size);
}

u32 nw4r_g3d_Camera_GetCameraMtx(u32 camera, u32 dst) { return engine::g3d::camera_get_camera_mtx(camera, dst); }
void nw4r_g3d_Camera_GetProjectionMtx(u32 camera, u32 dst) { engine::g3d::camera_get_projection_mtx(camera, dst); }
void nw4r_g3d_Camera_GetEnvironmentTexMtx(u32 camera, u32 dst) { engine::g3d::camera_get_environment_tex_mtx(camera, dst); }
void nw4r_g3d_Fog_CopyTo(u32 fog, u32 dst) { engine::g3d::fog_copy_to(fog, dst); }

void nw4r_g3d_LightSetting_Import(u32 self, u32 rhs) {
    engine::g3d::light_setting_import(self, rhs);
}

}  // namespace g
