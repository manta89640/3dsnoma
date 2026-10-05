// Materials of the native g3d at run time: a package's GX register image (fmt::Material) as the state
// a GPU with fixed combiners takes -- the combiners (engine/gx/tev_combiner, the GX path's own
// translation), up to three texture units with their sampling and texture coordinates, colour
// channel 0's lighting, the pixel pipeline (alpha test, blending, depth, culling), fog and the pass.
// Encodings are the PICA's (citro3d's GPU_* numbering) where the GPU takes the value as it is; the
// host's software backend evaluates the same values with the PICA's semantics.
#pragma once

#include "engine/g3d/package.h"
#include "engine/gx/tev_combiner.h"

namespace engine::g3d {

using s8 = int8_t;

// where a texture unit's coordinates come from (after the texgen's source, before its matrix)
enum class TexCoordSource : u8 {
    UV0,          // the vertex's first texture coordinate
    UV1,          // its second
    ENV_CAMERA,   // the normal in view space: (0.5 + 0.5 nx, 0.5 - 0.5 ny) (nw4r's camera env map)
    PROJECTION,   // the position on the screen: (0.5 + 0.5 x/w, 0.5 - 0.5 y/w) (camera projection)
};

struct TexUnit {
    u16 texture = fmt::NONE16;  // package texture (NONE16: the unit isn't used)
    u8 wrap_s = 0, wrap_t = 0;  // PICA: 0 clamp to edge, 2 repeat, 3 mirrored repeat
    bool mag_linear = true, min_linear = true;
    u8 mip = 0;                 // between levels: 0 none (level 0 only), 1 nearest, 2 linear
    f32 lod_bias = 0, min_lod = 0, max_lod = 0;  // as GX's (a backend corrects the bias for its screen)
    // its coordinates
    TexCoordSource source = TexCoordSource::UV0;
    s8 matrix = -1;             // texture matrix (index among the material's, < 3 when animated), -1: none
    bool projective = false;    // a 3x4 texgen (s/q, t/q): GX's MTX3x4 from the position
    // GX's dual transform: normalize, then the post matrix (the material's index, -1: none). nw4r
    // makes its env and projection maps with it; TexCoordSource already says what they compute.
    bool normalize = false;
    s8 post_matrix = -1;
};

// colour channel 0 (the one the island's materials use; channel 1 is never lit there)
struct Lighting {
    bool lit = false;              // colour lit (else the material colour as is)
    bool mat_from_vertex = false;  // material colour: the vertex colour, else mat_color
    bool amb_from_vertex = false;  // ambient: the vertex colour, else amb_color (or the light set's)
    bool alpha_lit = false, alpha_mat_from_vertex = false;
    u8 light_mask = 0;             // GX lights 0-7 (without a light set)
    u8 diffuse = 2;                // GX diffuse function: 0 none (1), 1 signed, 2 clamped N.L
    bool spot_attn = false;        // GX attenuation on, spot/distance (else specular/none)
    s16 light_set = -1;            // the package's light set (its lights and ambient), -1: none
    u8 mat_color[4] = {255, 255, 255, 255}, amb_color[4] = {0, 0, 0, 0};
};

// the pixel pipeline, PICA encodings
struct PixelState {
    bool alpha_test = false;
    u8 alpha_func = 1;             // GPU_TESTFUNC (1 always)
    u8 alpha_ref = 0;
    bool blend = false;            // blending (else: logic op if `logic`, else replace)
    u8 blend_eq = 0;               // GPU_BLENDEQUATION (0 add, 2 reverse subtract)
    u8 blend_src = 1, blend_dst = 0;  // GPU_BLENDFACTOR (1 one, 0 zero)
    bool logic = false;
    u8 logic_op = 3;               // GPU_LOGICOP (3 copy)
    bool color_write = true, alpha_write = false;
    bool depth_test = true;
    u8 depth_func = 5;             // GPU_TESTFUNC (5 lequal)
    bool depth_write = true;
    u8 cull_gx = 2;                // GX's cull mode: 0 none, 1 front, 2 back, 3 all (nothing drawn)
    u8 cull = 2;                   // GPU_CULLMODE as the GX path maps it: 0 none, 1 front, 2 back (CCW)
    bool const_alpha = false;      // the frame buffer's alpha gets const_alpha_value
    u8 const_alpha_value = 0;
};

struct Material {
    u16 index = 0;                 // in the package
    engine::gx::Combiners comb;    // its TEV with the packaged registers (luminance textures remapped)
    u32 lum_units = 0;             // units with a luminance texture (remap again after a re-translation)
    TexUnit unit[3];
    f32 tex_mtx[3][2][4] = {};     // its first three texture matrices, from the package's SRT (2x4)
    u32 animated_tex_mtx = 0;      // bit i: tex_mtx[i] has an SRT animation (the world updates it)
    bool animated_color = false;   // a colour animation drives its registers (re-translated per frame)
    Lighting light;
    PixelState pixel;
    bool fog = false;
    u8 fog_index = 0;
    bool translucent = false;      // drawn in the translucent pass
    s8 priority = 0;               // nw4r draw priority (lower first)
    bool indirect = false;         // the source had indirect stages (dropped: the scroll stays)
    u32 approximated = 0;          // what the translation approximated (combiners, texgens, matrix modes)
};

// Overlays: translucent materials that test depth without writing it, drawn over a surface they lie
// on (the island's _blend and Kiwa strips, 0.03-0.4 package units above the terrain they colour,
// over vertices partly shared with it).
//
// The native draws share the GX path's 24-bit depth buffer and its mapping (window depth =
// f (d - n) / (d (f - n)), the Mii's), the camera's near plane at 1.5 package units: a step of the
// buffer spans d^2 / (1.5 * 2^24) units at view distance d, and a strip's lift is less than one from
// about 600 units on. That alone doesn't make them fight: depths computed exactly and truncated keep
// their order, and LEQUAL lets the strip through (the software backend computes vertex depth in
// double and shows every strip without a bias: NOMA_SELFTEST=g3drender). Rounding does: a float's
// vertex depth is off by a step or two, the PICA's (z/w in float24, 17 bits) by about a hundred,
// against a lift of 0.03 steps at 5,000 units.
//
// So an overlay is tested OVERLAY_DEPTH_BIAS steps nearer the camera than it is (the PICA backend
// through its depth offset, the software one in its test; its own depth is never written), but never
// more than OVERLAY_DEPTH_BIAS_LIMIT package units of view distance nearer: a bias in steps grows as
// d^2 in distance (256 steps are 0.024 units at 50 and 10 at 1000, 250 at 5000), and from the
// overview camera an unlimited one lets the volcano's inner blend through its outer wall.
constexpr u32 OVERLAY_DEPTH_BIAS = 256;
constexpr float OVERLAY_DEPTH_BIAS_LIMIT = 4.0f;
inline bool is_overlay(const Material& m) { return m.translucent && m.pixel.depth_test && !m.pixel.depth_write; }
// The bias in steps at a depth where one package unit of view distance makes `steps_per_unit` steps
// (GX's perspective: |XF 0x1025 (p23) x the viewport's z scale| x world_scale / w^2, w the view depth
// in guest units).
inline float overlay_depth_bias(float steps_per_unit) {
    const float limit = OVERLAY_DEPTH_BIAS_LIMIT * steps_per_unit;
    return limit < (float)OVERLAY_DEPTH_BIAS ? limit : (float)OVERLAY_DEPTH_BIAS;
}

// the TEV register image of a package material (with registers and konst colours as given)
engine::gx::TevRegs tev_regs(const fmt::Material& m, const s16 (*reg)[4], const s16 (*konst)[4]);

// a package material at run time
void build_material(const Package& package, u32 index, Material* out);

// the GX texture map -> PICA wrap mode, filter (helpers the backends share)
u8 pica_wrap(u8 gx_wrap);

}  // namespace engine::g3d
