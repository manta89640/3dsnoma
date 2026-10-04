// A PICA200 vertex shader interpreter, after the shader binary format picasso writes and the
// instruction set as 3dbrew documents it (and Citra runs it).
//
//   DVLB: 'DVLB', DVLE count, DVLE offsets; DVLP: 'DVLP', version, code offset, code words,
//   operand descriptor offset, descriptor count; DVLE: 'DVLE', version/type, main offset, main
//   end, ..., constant table, label table, output table, uniform table, symbol table.
//   Instructions are 32-bit words: the opcode in the top bits, the destination and source
//   registers, an index register for one wide source, and the operand descriptor index: a
//   descriptor holds the destination mask and each source's swizzle and sign.
#include "pica_vsh.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace guest {

struct PicaShader {
    std::vector<u32> code;
    std::vector<u32> opdesc;
    u32 entry = 0, entry_end = 0;
    struct Uniform {
        std::string name;
        u32 reg;  // 0x10 + register: float 0x10-0x6F, integer 0x70-0x73, bool 0x78-0x87 (as the DVLE)
    };
    std::vector<Uniform> uniforms;
    struct Const {
        u16 type, id;
        u32 data[4];
    };
    std::vector<Const> consts;
    struct Output {
        u16 type, reg;
        u8 mask;
    };
    std::vector<Output> outputs;
};

namespace {

u32 rd32(const u8* p, size_t at) {
    u32 v;
    memcpy(&v, p + at, 4);
    return v;
}

// 24-bit float (1 sign, 7 exponent, 16 mantissa bits) -> float
f32 f24_to_f32(u32 h) {
    h &= 0xFFFFFF;
    if (!h)
        return 0.0f;
    u32 sign = h >> 23, exp = (h >> 16) & 0x7F, mant = h & 0xFFFF;
    f32 v;
    if (exp == 0) {
        v = std::ldexp((f32)mant, -16 - 62);
    } else {
        u32 bits = sign << 31 | (exp + 64) << 23 | mant << 7;
        memcpy(&v, &bits, 4);
        return v;
    }
    return sign ? -v : v;
}

}  // namespace

PicaShader* pica_shader_load(const void* data, size_t size) {
    const u8* p = (const u8*)data;
    if (size < 12 || memcmp(p, "DVLB", 4) != 0)
        return nullptr;
    u32 ndvle = rd32(p, 4);
    if (!ndvle || 8 + 4 * ndvle + 40 > size)
        return nullptr;
    size_t dvlp = 8 + 4 * ndvle;
    if (memcmp(p + dvlp, "DVLP", 4) != 0)
        return nullptr;
    PicaShader* s = new PicaShader;
    u32 code_off = rd32(p, dvlp + 8), code_n = rd32(p, dvlp + 12);
    u32 desc_off = rd32(p, dvlp + 16), desc_n = rd32(p, dvlp + 20);
    if (dvlp + code_off + 4 * code_n > size || dvlp + desc_off + 8 * desc_n > size) {
        delete s;
        return nullptr;
    }
    s->code.resize(code_n);
    for (u32 i = 0; i < code_n; i++)
        s->code[i] = rd32(p, dvlp + code_off + 4 * i);
    s->opdesc.resize(desc_n);
    for (u32 i = 0; i < desc_n; i++)
        s->opdesc[i] = rd32(p, dvlp + desc_off + 8 * i);
    // the first DVLE
    size_t d = rd32(p, 8);
    if (d + 64 > size || memcmp(p + d, "DVLE", 4) != 0) {
        delete s;
        return nullptr;
    }
    s->entry = rd32(p, d + 8);
    s->entry_end = rd32(p, d + 12);
    u32 const_off = rd32(p, d + 24), const_n = rd32(p, d + 28);
    u32 out_off = rd32(p, d + 40), out_n = rd32(p, d + 44);
    u32 unif_off = rd32(p, d + 48), unif_n = rd32(p, d + 52);
    u32 sym_off = rd32(p, d + 56), sym_n = rd32(p, d + 60);
    for (u32 i = 0; i < const_n && d + const_off + 20 * (i + 1) <= size; i++) {
        PicaShader::Const c;
        size_t at = d + const_off + 20 * i;
        c.type = (u16)(rd32(p, at) & 0xFFFF), c.id = (u16)(rd32(p, at) >> 16);
        for (int k = 0; k < 4; k++)
            c.data[k] = rd32(p, at + 4 + 4 * k);
        s->consts.push_back(c);
    }
    for (u32 i = 0; i < out_n && d + out_off + 8 * (i + 1) <= size; i++) {
        size_t at = d + out_off + 8 * i;
        PicaShader::Output o;
        o.type = (u16)(rd32(p, at) & 0xFFFF), o.reg = (u16)(rd32(p, at) >> 16), o.mask = (u8)rd32(p, at + 4);
        s->outputs.push_back(o);
    }
    const char* syms = (const char*)p + d + sym_off;
    for (u32 i = 0; i < unif_n && d + unif_off + 8 * (i + 1) <= size; i++) {
        size_t at = d + unif_off + 8 * i;
        u32 sym = rd32(p, at), regs = rd32(p, at + 4);
        PicaShader::Uniform u;
        if (sym < sym_n) {
            size_t len = 0;
            while (sym + len < sym_n && syms[sym + len])
                len++;
            u.name = std::string(syms + sym, len);
        }
        u.reg = regs & 0xFFFF;
        s->uniforms.push_back(u);
    }
    return s;
}

void pica_shader_free(PicaShader* s) { delete s; }

int pica_shader_uniform(const PicaShader* s, const char* name) {
    for (const auto& u : s->uniforms)
        if (u.name == name)
            return (int)u.reg - 0x10;
    return -1;
}

int pica_shader_output(const PicaShader* s, int semantic) {
    for (const auto& o : s->outputs)
        if (o.type == semantic)
            return o.reg;
    return -1;
}

void pica_shader_constants(const PicaShader* s, PicaUniforms& u) {
    for (const auto& c : s->consts) {
        if (c.type == 2 && c.id < 96) {  // float24
            for (int k = 0; k < 4; k++)
                u.f[c.id][k] = f24_to_f32(c.data[k]);
        } else if (c.type == 1 && c.id < 4) {  // u8 x4
            for (int k = 0; k < 4; k++)
                u.i[c.id][k] = (s32)((c.data[0] >> (8 * k)) & 0xFF);
        } else if (c.type == 0 && c.id < 16) {
            u.b = (u.b & ~(1u << c.id)) | ((c.data[0] & 1) << c.id);
        }
    }
}

// ---------------------------------------------------------------- execution
namespace {

enum Op : u32 {
    OP_ADD = 0x00, OP_DP3, OP_DP4, OP_DPH, OP_DST, OP_EX2, OP_LG2, OP_LITP, OP_MUL, OP_SGE, OP_SLT, OP_FLR, OP_MAX,
    OP_MIN, OP_RCP, OP_RSQ,
    OP_MOVA = 0x12, OP_MOV = 0x13,
    OP_DPHI = 0x18, OP_DSTI, OP_SGEI, OP_SLTI,
    OP_BREAK = 0x20, OP_NOP, OP_END, OP_BREAKC, OP_CALL, OP_CALLC, OP_CALLU, OP_IFU, OP_IFC, OP_LOOP, OP_EMIT,
    OP_SETEMIT, OP_JMPC, OP_JMPU, OP_CMP0, OP_CMP1,
    OP_MADI_FIRST = 0x30, OP_MAD_FIRST = 0x38,
};

struct Machine {
    const PicaShader& sh;
    const PicaUniforms& u;
    const f32 (*in)[4];
    f32 (*out)[4];
    f32 tmp[16][4];
    s32 a0[2] = {0, 0}, al = 0;
    bool cmpx = false, cmpy = false;
    struct Frame {
        u8 kind;  // 0 call/if, 1 loop
        u32 end, ret, body;
        s32 remaining, step;
    };
    std::vector<Frame> stack;

    Machine(const PicaShader& s, const PicaUniforms& un, const f32 i[16][4], f32 o[16][4]) : sh(s), u(un), in(i), out(o) {
        memset(tmp, 0, sizeof(tmp));
    }

    // a source operand: register 0x00-0x0F input, 0x10-0x1F temporary, 0x20-0x7F uniform (+ index)
    void source(u32 reg, u32 idx, u32 sw9, f32* v) const {
        const f32* src;
        static const f32 zero[4] = {0, 0, 0, 0};
        if (reg < 0x10) {
            src = in[reg];
        } else if (reg < 0x20) {
            src = tmp[reg - 0x10];
        } else {
            s32 r = (s32)reg - 0x20;
            if (idx == 1)
                r += a0[0];
            else if (idx == 2)
                r += a0[1];
            else if (idx == 3)
                r += al;
            src = r >= 0 && r < 96 ? u.f[r] : zero;
        }
        bool neg = sw9 & 1;
        u32 sw = sw9 >> 1;
        for (int c = 0; c < 4; c++) {
            u32 sel = (sw >> (6 - 2 * c)) & 3;
            v[c] = neg ? -src[sel] : src[sel];
        }
    }

    void dest(u32 reg, u32 mask, const f32* v) {
        f32* d = reg < 0x10 ? out[reg] : reg < 0x20 ? tmp[reg - 0x10] : nullptr;
        if (!d)
            return;
        for (int c = 0; c < 4; c++)
            if (mask & (8u >> c))
                d[c] = v[c];
    }

    bool cond(u32 inst) const {
        bool refx = (inst >> 25) & 1, refy = (inst >> 24) & 1;
        u32 op = (inst >> 22) & 3;
        bool x = cmpx == refx, y = cmpy == refy;
        switch (op) {
        case 0: return x || y;
        case 1: return x && y;
        case 2: return x;
        default: return y;
        }
    }

    static bool compare(u32 op, f32 a, f32 b) {
        switch (op) {
        case 0: return a == b;
        case 1: return a != b;
        case 2: return a < b;
        case 3: return a <= b;
        case 4: return a > b;
        case 5: return a >= b;
        default: return true;
        }
    }

    void run() {
        u32 pc = sh.entry;
        const u32 n = (u32)sh.code.size();
        for (u32 guard = 0; guard < 1000000; guard++) {
            // the end of a block
            while (!stack.empty() && pc == stack.back().end) {
                Frame& f = stack.back();
                if (f.kind == 1 && f.remaining > 0) {
                    f.remaining--;
                    al += f.step;
                    pc = f.body;
                } else {
                    pc = f.ret;
                    stack.pop_back();
                }
            }
            if (pc >= n)
                return;
            u32 inst = sh.code[pc];
            u32 op = inst >> 26;
            if (op >= OP_MAD_FIRST || (op >= OP_MADI_FIRST && op < OP_MAD_FIRST)) {
                // mad: dst 24-28, idx 22-23, src1 17-21 (narrow), src2 10-16 (wide) / 12-16, src3 5-9 / 5-11 (wide), desc 0-4
                bool inv = op < OP_MAD_FIRST;
                u32 desc = sh.opdesc[inst & 0x1F];
                u32 dst = (inst >> 24) & 0x1F, idx = (inst >> 22) & 3;
                u32 s1 = (inst >> 17) & 0x1F, s2, s3;
                if (!inv)
                    s2 = (inst >> 10) & 0x7F, s3 = (inst >> 5) & 0x1F;
                else
                    s2 = (inst >> 12) & 0x1F, s3 = (inst >> 5) & 0x7F;
                f32 a[4], b[4], c[4], r[4];
                source(s1, 0, (desc >> 4) & 0x1FF, a);
                source(s2, inv ? 0 : idx, (desc >> 13) & 0x1FF, b);
                source(s3, inv ? idx : 0, (desc >> 22) & 0x1FF, c);
                for (int k = 0; k < 4; k++)
                    r[k] = a[k] * b[k] + c[k];
                dest(dst, desc & 0xF, r);
                pc++;
                continue;
            }
            if (op == OP_CMP0 || op == OP_CMP1) {
                u32 desc = sh.opdesc[inst & 0x7F];
                u32 s1 = (inst >> 12) & 0x7F, s2 = (inst >> 7) & 0x1F, idx = (inst >> 19) & 3;
                u32 opy = (inst >> 21) & 7, opx = (inst >> 24) & 7;
                f32 a[4], b[4];
                source(s1, idx, (desc >> 4) & 0x1FF, a);
                source(s2, 0, (desc >> 13) & 0x1FF, b);
                cmpx = compare(opx, a[0], b[0]);
                cmpy = compare(opy, a[1], b[1]);
                pc++;
                continue;
            }
            switch (op) {
            case OP_NOP: pc++; break;
            case OP_END: return;
            case OP_BREAK:
            case OP_BREAKC: {
                if (op == OP_BREAKC && !cond(inst)) {
                    pc++;
                    break;
                }
                while (!stack.empty() && stack.back().kind != 1)
                    stack.pop_back();
                if (stack.empty())
                    return;
                pc = stack.back().ret;
                stack.pop_back();
                break;
            }
            case OP_CALL:
            case OP_CALLC:
            case OP_CALLU: {
                u32 dst = (inst >> 10) & 0xFFF, num = inst & 0xFF;
                bool go = op == OP_CALL || (op == OP_CALLC ? cond(inst) : ((u.b >> ((inst >> 22) & 0xF)) & 1));
                if (!go) {
                    pc++;
                    break;
                }
                stack.push_back({0, dst + num, pc + 1, dst, 0, 0});
                pc = dst;
                break;
            }
            case OP_IFU:
            case OP_IFC: {
                u32 dst = (inst >> 10) & 0xFFF, num = inst & 0xFF;
                bool go = op == OP_IFC ? cond(inst) : ((u.b >> ((inst >> 22) & 0xF)) & 1);
                if (go) {
                    stack.push_back({0, dst, dst + num, pc + 1, 0, 0});
                    pc++;
                } else {
                    pc = dst;
                }
                break;
            }
            case OP_LOOP: {
                u32 dst = (inst >> 10) & 0xFFF, ireg = (inst >> 22) & 3;
                const s32* iv = u.i[ireg];
                al = iv[1];
                stack.push_back({1, dst + 1, dst + 1, pc + 1, iv[0], iv[2]});
                pc++;
                break;
            }
            case OP_JMPC:
            case OP_JMPU: {
                u32 dst = (inst >> 10) & 0xFFF;
                bool go;
                if (op == OP_JMPC)
                    go = cond(inst);
                else
                    go = (((u.b >> ((inst >> 22) & 0xF)) & 1) != 0) != ((inst & 1) != 0);
                pc = go ? dst : pc + 1;
                break;
            }
            case OP_EMIT:
            case OP_SETEMIT: pc++; break;
            default: {
                // format 1: dst 21-25, idx 19-20, src1 12-18 (wide), src2 7-11 (narrow), desc 0-6
                // format 1i (DPHI SGEI SLTI DSTI): src1 14-18 (narrow), src2 7-13 (wide, indexed)
                u32 desc = sh.opdesc[inst & 0x7F];
                u32 dst = (inst >> 21) & 0x1F, idx = (inst >> 19) & 3;
                bool inv = op == OP_DPHI || op == OP_DSTI || op == OP_SGEI || op == OP_SLTI;
                u32 s1, s2;
                if (!inv)
                    s1 = (inst >> 12) & 0x7F, s2 = (inst >> 7) & 0x1F;
                else
                    s1 = (inst >> 14) & 0x1F, s2 = (inst >> 7) & 0x7F;
                f32 a[4], b[4], r[4] = {0, 0, 0, 0};
                source(s1, inv ? 0 : idx, (desc >> 4) & 0x1FF, a);
                source(s2, inv ? idx : 0, (desc >> 13) & 0x1FF, b);
                u32 mask = desc & 0xF;
                switch (op) {
                case OP_ADD:
                    for (int k = 0; k < 4; k++)
                        r[k] = a[k] + b[k];
                    break;
                case OP_MUL:
                    for (int k = 0; k < 4; k++)
                        r[k] = a[k] * b[k];
                    break;
                case OP_DP3: {
                    f32 d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
                    for (int k = 0; k < 4; k++)
                        r[k] = d;
                    break;
                }
                case OP_DP4: {
                    f32 d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
                    for (int k = 0; k < 4; k++)
                        r[k] = d;
                    break;
                }
                case OP_DPH:
                case OP_DPHI: {
                    f32 d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + b[3];
                    for (int k = 0; k < 4; k++)
                        r[k] = d;
                    break;
                }
                case OP_DST:
                case OP_DSTI:
                    r[0] = 1, r[1] = a[1] * b[1], r[2] = a[2], r[3] = b[3];
                    break;
                case OP_EX2:
                    for (int k = 0; k < 4; k++)
                        r[k] = std::exp2(a[0]);
                    break;
                case OP_LG2:
                    for (int k = 0; k < 4; k++)
                        r[k] = std::log2(a[0]);
                    break;
                case OP_LITP:
                    r[0] = a[0] > 0 ? a[0] : 0, r[1] = a[1] < -128 ? -128 : a[1] > 128 ? 128 : a[1],
                    r[2] = a[2] > 0 ? a[2] : 0, r[3] = 1;
                    break;
                case OP_SGE:
                case OP_SGEI:
                    for (int k = 0; k < 4; k++)
                        r[k] = a[k] >= b[k] ? 1.0f : 0.0f;
                    break;
                case OP_SLT:
                case OP_SLTI:
                    for (int k = 0; k < 4; k++)
                        r[k] = a[k] < b[k] ? 1.0f : 0.0f;
                    break;
                case OP_FLR:
                    for (int k = 0; k < 4; k++)
                        r[k] = std::floor(a[k]);
                    break;
                case OP_MAX:
                    for (int k = 0; k < 4; k++)
                        r[k] = a[k] > b[k] ? a[k] : b[k];
                    break;
                case OP_MIN:
                    for (int k = 0; k < 4; k++)
                        r[k] = a[k] < b[k] ? a[k] : b[k];
                    break;
                case OP_RCP:
                    for (int k = 0; k < 4; k++)
                        r[k] = 1.0f / a[0];
                    break;
                case OP_RSQ:
                    for (int k = 0; k < 4; k++)
                        r[k] = 1.0f / std::sqrt(a[0]);
                    break;
                case OP_MOV:
                    for (int k = 0; k < 4; k++)
                        r[k] = a[k];
                    break;
                case OP_MOVA:
                    if (mask & 8)
                        a0[0] = (s32)a[0];
                    if (mask & 4)
                        a0[1] = (s32)a[1];
                    pc++;
                    continue;
                default:
                    pc++;
                    continue;
                }
                dest(dst, mask, r);
                pc++;
                break;
            }
            }
        }
    }
};

}  // namespace

void pica_vsh_run(const PicaShader* s, const PicaUniforms& u, const f32 in[16][4], f32 out[16][4]) {
    Machine m(*s, u, in, out);
    m.run();
}

}  // namespace guest
