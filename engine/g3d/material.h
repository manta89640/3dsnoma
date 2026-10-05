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

// colour channel 0 (the one the island's materials use; channel 1 is never lit there). A prelit
// material (fmt::MAT_PRELIT: its lighting baked into the vertex colours) is unlit, its colour and
// alpha the vertex's: the backends need no normals and run no light loop for it.
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
    bool prelit = false;           // fmt::MAT_PRELIT: lit by its vertex colours (Lighting: unlit)
    u32 approximated = 0;          // what the translation approximated (combiners, texgens, matrix modes)
};

// Overlays: translucent materials that test depth without writing it, drawn over a surface they lie
// on (the island's _blend and Kiwa strips, 0.03-0.4 package units above the terrain they colour,
// over vertices partly shared with it).
//
// An overlay: translucent, depth tested, writing no depth (the edge layers and strips that lie a
// fraction of a unit over the ground): both backends pull it toward the eye (native_draw.h
// overlay_pull) so that it isn't lost in the ground at a distance.
inline bool is_overlay(const Material& m) { return m.translucent && m.pixel.depth_test && !m.pixel.depth_write; }

// the TEV register image of a package material (with registers and konst colours as given)
engine::gx::TevRegs tev_regs(const fmt::Material& m, const s16 (*reg)[4], const s16 (*konst)[4]);

// a package material at run time
void build_material(const Package& package, u32 index, Material* out);

// the GX texture map -> PICA wrap mode, filter (helpers the backends share)
u8 pica_wrap(u8 gx_wrap);

}  // namespace engine::g3d
