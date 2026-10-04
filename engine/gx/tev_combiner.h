// GX's TEV (texture environment) as the PICA200's texture combiners.
//
// One translation for every renderer that puts GX materials on a GPU with fixed combiners: the GX
// path on the 3DS (platform/3ds/gx_pica.cpp: the TEV registers the command stream set), the native
// g3d's materials (engine/g3d/material.cpp: the register image a package holds), and the host's
// software backend, which evaluates the result with the PICA's arithmetic so that a material looks
// the same on both. Nothing here depends on a platform: the output is in the PICA's own encodings
// (the numbers citro3d calls GPU_TEVSRC, GPU_TEVOP_*, GPU_COMBINEFUNC and GPU_TEVSCALE, as the GPU's
// TEXENV registers take them).
//
// A GX stage computes d +- lerp(a, b, c), plus a bias, times a scale, into one of four registers;
// a PICA stage computes one of a few functions of three inputs into the next stage (and, for the
// first four, optionally into a buffer the stage after next can read). One GX stage becomes one or
// two PICA stages: the lerp is simplified where its inputs are constants (one input, a product,
// an interpolation), a bias that doesn't fold into ADD_SIGNED takes a stage of its own, register
// constants and konst colours share each stage's one constant colour, and a register a later
// stage reads is found as PREVIOUS (the stage before) or in the combiner buffer. What can't be
// expressed is approximated and counted (Combiners::unsupported).
#pragma once

#include <cstdint>

namespace engine::gx {

using u8 = uint8_t;
using s8 = int8_t;
using u32 = uint32_t;
using s32 = int32_t;

// ---- the PICA's encodings
enum CombinerSrc : u8 {
    SRC_PRIMARY_COLOR = 0x0,     // the vertex colour
    SRC_FRAGMENT_PRIMARY = 0x1,  // fragment lighting's colours (unused here)
    SRC_FRAGMENT_SECONDARY = 0x2,
    SRC_TEXTURE0 = 0x3,
    SRC_TEXTURE1 = 0x4,
    SRC_TEXTURE2 = 0x5,
    SRC_TEXTURE3 = 0x6,          // (procedural texture)
    SRC_PREVIOUS_BUFFER = 0xD,   // the combiner buffer
    SRC_CONSTANT = 0xE,          // the stage's constant colour
    SRC_PREVIOUS = 0xF,          // the stage before's result
};
enum CombinerOpRgb : u8 {
    OP_RGB_SRC_COLOR = 0x0, OP_RGB_ONE_MINUS_SRC_COLOR = 0x1,
    OP_RGB_SRC_ALPHA = 0x2, OP_RGB_ONE_MINUS_SRC_ALPHA = 0x3,
    OP_RGB_SRC_R = 0x4, OP_RGB_ONE_MINUS_SRC_R = 0x5,
    OP_RGB_SRC_G = 0x8, OP_RGB_ONE_MINUS_SRC_G = 0x9,
    OP_RGB_SRC_B = 0xC, OP_RGB_ONE_MINUS_SRC_B = 0xD,
};
enum CombinerOpA : u8 {
    OP_A_SRC_ALPHA = 0x0, OP_A_ONE_MINUS_SRC_ALPHA = 0x1,
    OP_A_SRC_R = 0x2, OP_A_ONE_MINUS_SRC_R = 0x3,
    OP_A_SRC_G = 0x4, OP_A_ONE_MINUS_SRC_G = 0x5,
    OP_A_SRC_B = 0x6, OP_A_ONE_MINUS_SRC_B = 0x7,
};
enum CombinerFunc : u8 {
    FUNC_REPLACE = 0,       // a
    FUNC_MODULATE = 1,      // a * b
    FUNC_ADD = 2,           // a + b
    FUNC_ADD_SIGNED = 3,    // a + b - 0.5
    FUNC_INTERPOLATE = 4,   // a * c + b * (1 - c)
    FUNC_SUBTRACT = 5,      // a - b
    FUNC_DOT3_RGB = 6,
    FUNC_DOT3_RGBA = 7,
    FUNC_MULTIPLY_ADD = 8,  // a * b + c
    FUNC_ADD_MULTIPLY = 9,  // (a + b) * c
};
enum CombinerScale : u8 { SCALE_1 = 0, SCALE_2 = 1, SCALE_4 = 2 };

// ---- input: the TEV's registers as GX has them
struct TevRegs {
    u32 n_stages = 1;          // GENMODE's TEV stage count (1-16)
    u32 color_env[16] = {};    // BP 0xC0 + 2i: the colour combiner of stage i (24-bit value)
    u32 alpha_env[16] = {};    // BP 0xC1 + 2i: the alpha combiner
    u32 tref[8] = {};          // BP 0x28-0x2F: texture map, coordinate and colour channel per stage, two a word
    u32 ksel[8] = {};          // BP 0xF6-0xFD: konst selections (and the swap tables), two stages a word
    s32 reg[4][4] = {};        // PREV, C0, C1, C2: RGBA, 11-bit signed (BP 0xE0-0xE7, register type)
    s32 konst[4][4] = {};      // K0-K3 (BP 0xE0-0xE7, konst type)
};

// ---- output: six PICA combiner stages
struct CombinerStage {
    u8 src_rgb[3], src_a[3];   // CombinerSrc per input
    u8 op_rgb[3], op_a[3];     // CombinerOpRgb / CombinerOpA per input
    u8 func_rgb, func_a;       // CombinerFunc
    u8 scale_rgb, scale_a;     // CombinerScale
    u32 konst;                 // the constant colour, 0xAABBGGRR
};
// a texture unit: the GX texture map it samples and the texture coordinate (texgen) it uses
struct CombinerUnit {
    s8 map = -1, coord = -1;   // -1: unused
};
struct Combiners {
    CombinerStage stage[6];    // all six set: the ones after the TEV's pass the result on
    u8 n_stages = 0;           // PICA stages the TEV took
    u8 buf_rgb = 0, buf_a = 0; // combiner buffer update masks (bit i: stage i's result goes in it)
    CombinerUnit unit[3];
    bool overflow = false;     // the TEV needed more than six stages (the rest was left out)
    u32 unsupported = 0;       // things approximated (compare modes, overflow, constants that
                               // conflict in a stage, a buffer read the PICA can't make)
};

// Translate the TEV; units are given out in the order the stages first sample a (map, coord) pair
// (a fourth pair reads as 1.0).
void tev_to_combiners(const TevRegs& tev, Combiners& out);

// A texture stored as luminance only (the PICA's L4 and L8, which GX's I4 and I8 become) samples
// with alpha 1, where GX's intensity texture has alpha = intensity: every input that reads such a
// texture's alpha reads its red instead. lum_units: bit u for each such texture unit.
void remap_luminance(CombinerStage stage[6], u32 lum_units);

// a stage as the GPU's TEXENVi registers take it: SOURCE, OPERAND, COMBINER, COLOR, SCALE (the
// layout of citro3d's C3D_TexEnv)
void combiner_words(const CombinerStage& s, u32 words[5]);

}  // namespace engine::gx
