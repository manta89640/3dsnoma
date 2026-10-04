// A PICA200 vertex shader run on the CPU (pica_vsh.cpp): the shader binary picasso assembles
// (DVLB with one vertex DVLE), its uniforms, and one vertex through it. For checking the GX
// vertex shader (platform/3ds/shaders/gx.v.pica) against the reference transform on the host:
// floats are the host's 32-bit ones, not the GPU's 24-bit.
#pragma once
#include <cstddef>

#include "../hw.h"

namespace guest {

struct PicaShader;

// parse a .shbin (the bytes are kept by the caller); null if it isn't one
PicaShader* pica_shader_load(const void* data, size_t size);
void pica_shader_free(PicaShader* s);
// a uniform's register by name: float vectors 0-95, integer vectors 0x60-0x63, bools 0x68-0x77
// (as shaderInstanceGetUniformLocation numbers them); -1 if the shader has none by that name
int pica_shader_uniform(const PicaShader* s, const char* name);
// the output register (o0-o15) that carries an output semantic (DVLE_outputAttribute_t:
// 0 position, 2 color, 3 texcoord0, 5 texcoord1, 6 texcoord2); -1 if none
int pica_shader_output(const PicaShader* s, int semantic);

// the uniforms as the shader sees them
struct PicaUniforms {
    f32 f[96][4];
    s32 i[4][4];
    u32 b;  // bit per bool uniform
};
// the DVLE's constants into the uniforms (the rest untouched)
void pica_shader_constants(const PicaShader* s, PicaUniforms& u);

// run the shader on one vertex: in = v0-v15 (as the attribute loaders deliver them), out = o0-o15
void pica_vsh_run(const PicaShader* s, const PicaUniforms& u, const f32 in[16][4], f32 out[16][4]);

}  // namespace guest
