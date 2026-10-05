// The host's backend for native draws (native_draw.h): the native g3d's packets drawn in software
// into the host's EFB, the frame buffer runtime/hw/gx/raster.cpp draws the GX commands into, so that
// what the World draws and what the channel draws through GX (the Mii, the layouts) share pixels and
// depth on the host as they do on the 3DS.
//
// What it draws is what the PICA backend (platform/3ds/g3d_pica.cpp) has the 3DS GPU draw, with the
// GPU's rules: the runtime materials' combiners (engine/gx/tev_combiner's six stages in the PICA's
// encodings) evaluated with the PICA's arithmetic, the combiner buffer included; textures as the
// package gives them to the GPU (ETC1, the luminance formats) decoded to RGBA8 once, at prepare;
// colour channel 0 lit per vertex by the packet's light sets; one alpha comparison; the PICA's blend
// factors and equations. Where it lands is GX's: a vertex goes through the packet's view (guest
// units) times S(world_scale) times its item's matrix, then the GX projection and viewport current at
// the packet's command (GxView), to window coordinates and 24-bit depth exactly as raster.cpp places
// a GX draw's vertices (to_screen), so a native draw and a GX draw at the same point agree on depth.
// Culling is raster.cpp's (GX's winding, the viewport's mirroring), clipping its near and far planes
// (and a guard band), the scissor and its offset the registers' (guest::gx_host_efb).
//
// Per pixel: perspective-correct colour and texture coordinates; the level of detail from the
// coordinates' screen-space derivatives, bilinear within a level, trilinear between two when the
// material's filter asks; projection-mapped units (the water's camera projection) from the pixel's
// own screen position; linear fog by view depth for the materials that have it; overlays (material.h
// is_overlay) pulled toward the eye as the PICA backend pulls them (native_draw.h overlay_pull). Vertex
// depth is computed in double, so a strip lying just above the terrain stays in front of it however far
// away it is (the pull is there for the 3DS GPU's 24-bit floats: here it makes the frames alike).
// Perspective projections only (the native g3d's): a packet under an orthographic one draws nothing.
//
// The rows of the scissor box are shared out among a few threads, each drawing every triangle in
// order on its own rows: a pixel sees its triangles in the packet's order, as on a GPU, and a frame
// is drawn the same whatever the thread count (NOMA_SOFT3D_THREADS, default: up to 8).
#pragma once

#include <vector>

#include "engine/g3d/native_draw.h"
#include "hw/gx/raster.h"

namespace engine::g3d {

class SoftBackend : public Backend {
public:
    SoftBackend();
    ~SoftBackend() override;
    SoftBackend(const SoftBackend&) = delete;
    SoftBackend& operator=(const SoftBackend&) = delete;

    // every texture level decoded to RGBA8 (kept in Package::backend_data until release)
    void prepare(Package& package) override;
    // into the host's EFB (guest::gx_host_efb(): its scissor and offset as the registers have them)
    void draw(const Packet& packet, const GxView& view) override;
    void release(Package& package) override;

    // into a frame buffer laid out as the host's EFB (tests)
    void draw(const Packet& packet, const GxView& view, const guest::HostEfb& target);

    // what the draws since reset_stats() did
    struct Stats {
        u32 items = 0;        // items drawn
        u32 triangles = 0;    // triangles in them
        u32 culled = 0;       // culled (facing) or wholly outside the frustum or the scissor
        u32 clipped = 0;      // cut by the near or far plane or the guard band
        u64 fragments = 0;    // pixels covered
        u64 written = 0;      // pixels that passed the depth and alpha tests
    };
    const Stats& stats() const { return stats_; }
    void reset_stats() { stats_ = Stats(); }

    // Test hooks: the pull overlays are drawn with (the fraction of their distance, and the most it may
    // be in package units at their furthest corner; native_draw.h's OVERLAY_DEPTH_PULL and
    // OVERLAY_PULL_LIMIT unless set), and the number of threads (0: as NOMA_SOFT3D_THREADS or the
    // machine says).
    void set_overlay_pull(double pull, double limit);
    void set_threads(u32 n) { threads_ = n; }

    struct Impl;

private:
    Impl* impl_;
    Stats stats_;
    u32 threads_ = 0;
};

}  // namespace engine::g3d
