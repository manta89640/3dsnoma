// GX TEV -> PICA texture combiners (tev_combiner.h). Moved here from platform/3ds/gx_pica.cpp so that
// the native g3d's materials get the very translation the GX path uses: the logic is that code's,
// with citro3d's C3D_TexEnv setters replaced by the stage's fields (the same values).
#include "tev_combiner.h"

#include <algorithm>

namespace engine::gx {

namespace {

inline u32 bits(u32 v, int lo, int n) { return (v >> lo) & ((1u << n) - 1); }

// A value a GX stage reads, as a PICA combiner source and operand.
struct In {
    enum Kind { ZERO, ONE, CONST, SRC } kind = ZERO;
    u8 src = SRC_PRIMARY_COLOR;
    u8 op = 0;               // operand (CombinerOpRgb or CombinerOpA)
    u32 value = 0;           // CONST: 0xAABBGGRR (the combiner constant's layout)
    bool pair_prev = false;  // PREVIOUS meaning the first stage of a two-stage pair
};

u32 pack(const s32* c) {
    auto u = [](s32 v) { return (u32)(v < 0 ? 0 : v > 255 ? 255 : v); };
    return u(c[0]) | u(c[1]) << 8 | u(c[2]) << 16 | u(c[3]) << 24;
}

// GX's konst selections 0-7: the fractions 8/8 ... 1/8
const s32 k_konst[8] = {255, 223, 191, 159, 128, 96, 64, 32};

struct Builder {
    const TevRegs& tev;
    Combiners& out;
    int stage = 0;  // next PICA stage
    // where the GX registers live: kind CONST (never written) or the stage that wrote them
    int reg_stage[4][2];  // [register][0 color, 1 alpha] -> PICA stage index, -1 = constant
    Builder(const TevRegs& t, Combiners& o) : tev(t), out(o) {}
};

// texture unit for (map, coord), or -1
int unit_for(Builder& b, u32 map, u32 coord) {
    for (int i = 0; i < 3; i++)
        if (b.out.unit[i].map == (int)map && b.out.unit[i].coord == (int)coord)
            return i;
    for (int i = 0; i < 3; i++)
        if (b.out.unit[i].map < 0) {
            b.out.unit[i].map = (s8)map, b.out.unit[i].coord = (s8)coord;
            return i;
        }
    return -1;
}

u8 tex_src(int unit) { return unit == 0 ? SRC_TEXTURE0 : unit == 1 ? SRC_TEXTURE1 : SRC_TEXTURE2; }

struct StageCtx {
    int tex_unit;   // -1: no texture
    s32 konst_c[3], konst_a;
};

// a register written by stage `s` is read by stage `at`: PREVIOUS or the combiner buffer
In reg_source(Builder& b, int s, int at, bool alpha, u8 channel_op) {
    In in;
    in.kind = In::SRC;
    if (s == at - 1) {
        in.src = SRC_PREVIOUS;
    } else {
        in.src = SRC_PREVIOUS_BUFFER;
        if (s <= 3) {
            if (alpha)
                b.out.buf_a |= 1u << s;
            else
                b.out.buf_rgb |= 1u << s;
        }
    }
    in.op = channel_op;
    return in;
}

// GX color argument -> In
In color_arg(Builder& b, u32 arg, const StageCtx& sc, int at) {
    In in;
    auto from_reg = [&](int r, bool alpha_of) -> In {
        int s = b.reg_stage[r][alpha_of ? 1 : 0];
        if (s < 0) {
            In c;
            c.kind = In::CONST;
            const s32* v = b.tev.reg[r];
            s32 rgba[4] = {alpha_of ? v[3] : v[0], alpha_of ? v[3] : v[1], alpha_of ? v[3] : v[2], v[3]};
            c.value = pack(rgba);
            return c;
        }
        return reg_source(b, s, at, alpha_of, alpha_of ? OP_RGB_SRC_ALPHA : OP_RGB_SRC_COLOR);
    };
    switch (arg) {
    case 0: return from_reg(0, false);
    case 1: return from_reg(0, true);
    case 2: return from_reg(1, false);
    case 3: return from_reg(1, true);
    case 4: return from_reg(2, false);
    case 5: return from_reg(2, true);
    case 6: return from_reg(3, false);
    case 7: return from_reg(3, true);
    case 8:
    case 9:
        if (sc.tex_unit < 0) {
            in.kind = In::ONE;
            return in;
        }
        in.kind = In::SRC;
        in.src = tex_src(sc.tex_unit);
        in.op = arg == 8 ? OP_RGB_SRC_COLOR : OP_RGB_SRC_ALPHA;
        return in;
    case 10:
    case 11:
        in.kind = In::SRC;
        in.src = SRC_PRIMARY_COLOR;
        in.op = arg == 10 ? OP_RGB_SRC_COLOR : OP_RGB_SRC_ALPHA;
        return in;
    case 12: in.kind = In::ONE; return in;
    case 13: {
        in.kind = In::CONST;
        s32 h[4] = {128, 128, 128, 128};
        in.value = pack(h);
        return in;
    }
    case 14: {
        in.kind = In::CONST;
        s32 k[4] = {sc.konst_c[0], sc.konst_c[1], sc.konst_c[2], sc.konst_a};
        in.value = pack(k);
        return in;
    }
    default: return in;  // ZERO
    }
}

In alpha_arg(Builder& b, u32 arg, const StageCtx& sc, int at) {
    In in;
    auto from_reg = [&](int r) -> In {
        int s = b.reg_stage[r][1];
        if (s < 0) {
            In c;
            c.kind = In::CONST;
            s32 v = b.tev.reg[r][3];
            s32 rgba[4] = {v, v, v, v};
            c.value = pack(rgba);
            return c;
        }
        return reg_source(b, s, at, true, OP_A_SRC_ALPHA);
    };
    switch (arg) {
    case 0: return from_reg(0);
    case 1: return from_reg(1);
    case 2: return from_reg(2);
    case 3: return from_reg(3);
    case 4:
        if (sc.tex_unit < 0) {
            in.kind = In::ONE;
            return in;
        }
        in.kind = In::SRC;
        in.src = tex_src(sc.tex_unit);
        in.op = OP_A_SRC_ALPHA;
        return in;
    case 5:
        in.kind = In::SRC;
        in.src = SRC_PRIMARY_COLOR;
        in.op = OP_A_SRC_ALPHA;
        return in;
    case 6: {
        in.kind = In::CONST;
        s32 k[4] = {sc.konst_a, sc.konst_a, sc.konst_a, sc.konst_a};
        in.value = pack(k);
        return in;
    }
    default: return in;
    }
}

// One channel (color or alpha) of a PICA stage being built.
struct Half {
    u8 func = FUNC_REPLACE;
    In s[3];
    u8 scale = SCALE_1;
    bool used = false;
};

// a channel's part of a constant input (rgb or alpha): the stage's constant color holds both
u32 channel_mask(bool alpha) { return alpha ? 0xFF000000u : 0x00FFFFFFu; }
u32 const_value(const In& i) { return i.kind == In::ONE ? 0xFFFFFFFF : i.kind == In::ZERO ? 0 : i.value; }
bool is_zero(const In& i, bool alpha) {
    return i.kind == In::ZERO || (i.kind == In::CONST && (i.value & channel_mask(alpha)) == 0);
}
bool is_one(const In& i, bool alpha) {
    return i.kind == In::ONE || (i.kind == In::CONST && (alpha ? (i.value >> 24) == 255 : (i.value & 0xFFFFFF) == 0xFFFFFF));
}

// Plan one GX combiner (a, b, c, d, op, bias, scale) as <= 2 PICA halves. Returns the number of
// PICA stages it takes (1 or 2); the second stage reads the first as PREVIOUS.
int plan(In a, In b, In c, In d, bool sub, u32 bias, u32 scale, bool alpha, Half out[2]) {
    In prev;
    prev.kind = In::SRC;
    prev.src = SRC_PREVIOUS;
    prev.op = alpha ? OP_A_SRC_ALPHA : OP_RGB_SRC_COLOR;
    prev.pair_prev = true;
    // lerp(a, b, c) simplified to x (single term) or a product, or a full interpolation
    In x;
    enum { NONE, SINGLE, PRODUCT, LERP } shape;
    In p0, p1;
    if (a.kind != In::SRC && b.kind != In::SRC && c.kind != In::SRC) {
        // all constants: one constant (as GX computes it)
        u32 va = const_value(a), vb = const_value(b), vc = const_value(c), v = 0;
        for (int sh = 0; sh < 32; sh += 8) {
            u32 ka = va >> sh & 255, kb = vb >> sh & 255, kc = (vc >> sh & 255) + (vc >> sh >> 7 & 1);
            v |= (ka * (256 - kc) + kb * kc) >> 8 << sh;
        }
        x.kind = In::CONST, x.value = v;
        shape = is_zero(x, alpha) ? NONE : SINGLE;
    } else if (is_zero(c, alpha)) {
        shape = is_zero(a, alpha) ? NONE : SINGLE, x = a;
    } else if (is_one(c, alpha)) {
        shape = is_zero(b, alpha) ? NONE : SINGLE, x = b;
    } else if (is_zero(a, alpha)) {
        if (is_zero(b, alpha))
            shape = NONE;
        else if (is_one(b, alpha))
            shape = SINGLE, x = c;
        else
            shape = PRODUCT, p0 = b, p1 = c;
    } else if (is_zero(b, alpha)) {
        // a * (1 - c)
        shape = PRODUCT, p0 = a, p1 = c;
        p1.op = alpha ? (p1.op == OP_A_SRC_ALPHA ? OP_A_ONE_MINUS_SRC_ALPHA : p1.op)
                      : (p1.op == OP_RGB_SRC_COLOR ? OP_RGB_ONE_MINUS_SRC_COLOR : OP_RGB_ONE_MINUS_SRC_ALPHA);
        if (p1.kind == In::CONST) {  // 1 - constant
            u32 v = p1.value;
            p1.value = (255 - (v & 255)) | (255 - ((v >> 8) & 255)) << 8 | (255 - ((v >> 16) & 255)) << 16 |
                       (255 - (v >> 24)) << 24;
            p1.op = alpha ? OP_A_SRC_ALPHA : OP_RGB_SRC_COLOR;
        }
    } else {
        shape = LERP;
    }
    bool has_d = !is_zero(d, alpha);
    // A stage has one constant color: a lerp between two different constants (the year
    // picker's digits: lerp(C0, C1, texture)) takes one of them from a stage of its own first
    auto distinct_consts = [&](const In& p, const In& q) {
        return p.kind != In::SRC && q.kind != In::SRC && ((const_value(p) ^ const_value(q)) & channel_mask(alpha)) != 0;
    };
    bool hoist_a = false, hoist_b = false;
    if (shape == LERP && !sub && !has_d) {
        if (distinct_consts(a, b) || distinct_consts(a, c))
            hoist_a = true;
        else if (distinct_consts(b, c))
            hoist_b = true;
    }
    u8 sc = scale == 1 ? SCALE_2 : scale == 2 ? SCALE_4 : SCALE_1;
    int n = 0;
    auto half = [&](u8 f, In s0, In s1 = In(), In s2 = In()) {
        out[n].func = f;
        out[n].s[0] = s0, out[n].s[1] = s1, out[n].s[2] = s2;
        out[n].used = true;
        n++;
    };
    if (sub) {
        // d - x
        if (shape == NONE)
            half(FUNC_REPLACE, d);
        else if (shape == SINGLE)
            half(FUNC_SUBTRACT, d, x);
        else {
            if (shape == PRODUCT)
                half(FUNC_MODULATE, p0, p1);
            else
                half(FUNC_INTERPOLATE, b, a, c);
            half(FUNC_SUBTRACT, d, prev);
        }
    } else if (bias == 2 && has_d && shape == SINGLE) {
        half(FUNC_ADD_SIGNED, d, x);  // d + x - 0.5
        bias = 0;
    } else {
        switch (shape) {
        case NONE: half(FUNC_REPLACE, d); break;
        case SINGLE:
            if (has_d)
                half(FUNC_ADD, d, x);
            else
                half(FUNC_REPLACE, x);
            break;
        case PRODUCT:
            if (has_d)
                half(FUNC_MULTIPLY_ADD, p0, p1, d);
            else
                half(FUNC_MODULATE, p0, p1);
            break;
        case LERP:
            if (hoist_a)
                half(FUNC_REPLACE, a), half(FUNC_INTERPOLATE, b, prev, c);
            else if (hoist_b)
                half(FUNC_REPLACE, b), half(FUNC_INTERPOLATE, prev, a, c);
            else
                half(FUNC_INTERPOLATE, b, a, c);
            if (has_d)
                half(FUNC_ADD, prev, d);
            break;
        }
    }
    if (bias == 1 || bias == 2) {  // +-0.5 that couldn't be folded: one more stage
        In h;
        h.kind = In::CONST;
        h.value = 0x80808080;
        if (n < 2)
            half(bias == 1 ? FUNC_ADD : FUNC_SUBTRACT, prev, h);
    }
    if (n)
        out[n - 1].scale = sc;
    return n ? n : (half(FUNC_REPLACE, d), 1);
}

// the inputs a function reads
int used_inputs(u8 f) {
    switch (f) {
    case FUNC_REPLACE: return 1;
    case FUNC_INTERPOLATE:
    case FUNC_MULTIPLY_ADD:
    case FUNC_ADD_MULTIPLY: return 3;
    default: return 2;
    }
}

// the constants a half uses (ZERO and ONE included), merged into the stage's one constant
// color; false if they conflict. have: 1 = rgb set, 2 = alpha set
bool merge_consts(const Half& h, bool alpha, u32* color, u32* have) {
    for (int i = 0; i < used_inputs(h.func); i++) {
        const In& in = h.s[i];
        if (in.kind == In::SRC)
            continue;
        u32 v = const_value(in);
        u32 mask = channel_mask(alpha);
        u32 bit = alpha ? 2 : 1;
        if ((*have & bit) && ((*color ^ v) & mask))
            return false;
        *color = (*color & ~mask) | (v & mask);
        *have |= bit;
    }
    return true;
}

void apply_half(CombinerStage& st, const Half& h, bool alpha) {
    u8* src = alpha ? st.src_a : st.src_rgb;
    u8* ops = alpha ? st.op_a : st.op_rgb;
    for (int i = 0; i < 3; i++) {
        const In& in = h.s[i];
        if (in.kind == In::SRC) {
            src[i] = in.src;
            ops[i] = in.op;
        } else {
            // constants and ONE come from the stage's constant color; ZERO is the "one minus"
            // of a constant one... use the constant with an operand that gives 0: there is
            // none, so a zero input only appears where the function ignores it
            src[i] = SRC_CONSTANT;
            ops[i] = alpha ? OP_A_SRC_ALPHA : OP_RGB_SRC_COLOR;
        }
    }
    if (alpha)
        st.func_a = h.func, st.scale_a = h.scale;
    else
        st.func_rgb = h.func, st.scale_rgb = h.scale;
}

// a stage that passes the result on (what citro3d's C3D_TexEnvInit leaves, with every input
// PREVIOUS: its constant colour stays white)
CombinerStage pass_stage() {
    CombinerStage s;
    for (int i = 0; i < 3; i++)
        s.src_rgb[i] = s.src_a[i] = SRC_PREVIOUS, s.op_rgb[i] = s.op_a[i] = 0;
    s.func_rgb = s.func_a = FUNC_REPLACE;
    s.scale_rgb = s.scale_a = SCALE_1;
    s.konst = 0xFFFFFFFF;
    return s;
}

}  // namespace

void tev_to_combiners(const TevRegs& tev, Combiners& out) {
    out = Combiners();
    Builder b(tev, out);
    u32 num = tev.n_stages < 1 ? 1 : tev.n_stages > 16 ? 16 : tev.n_stages;
    for (int r = 0; r < 4; r++)
        b.reg_stage[r][0] = b.reg_stage[r][1] = -1;
    for (u32 i = 0; i < num && !out.overflow; i++) {
        u32 ce = tev.color_env[i], ae = tev.alpha_env[i];
        u32 tref = tev.tref[i / 2] >> ((i & 1) * 12);
        u32 ksel = tev.ksel[i / 2] >> ((i & 1) * 10);
        StageCtx sc;
        sc.tex_unit = bits(tref, 6, 1) ? unit_for(b, bits(tref, 0, 3), bits(tref, 3, 3)) : -1;
        u32 kc = bits(ksel, 4, 5), ka = bits(ksel, 9, 5);
        for (int k = 0; k < 3; k++) {
            if (kc < 8)
                sc.konst_c[k] = k_konst[kc];
            else if (kc >= 12 && kc < 16)
                sc.konst_c[k] = tev.konst[kc - 12][k];
            else if (kc >= 16)
                sc.konst_c[k] = tev.konst[(kc - 16) & 3][(kc - 16) >> 2];
            else
                sc.konst_c[k] = 0;
        }
        sc.konst_a = ka < 8 ? k_konst[ka] : ka >= 16 ? tev.konst[(ka - 16) & 3][(ka - 16) >> 2] : 0;

        int at = b.stage;
        Half ch[2], ah[2];
        int nc, na;
        if (bits(ce, 16, 2) == 3 || bits(ae, 16, 2) == 3)
            out.unsupported++;  // compare modes: approximated by the regular formula
        nc = plan(color_arg(b, bits(ce, 12, 4), sc, at), color_arg(b, bits(ce, 8, 4), sc, at),
                  color_arg(b, bits(ce, 4, 4), sc, at), color_arg(b, bits(ce, 0, 4), sc, at), bits(ce, 18, 1),
                  bits(ce, 16, 2) == 3 ? 0 : bits(ce, 16, 2), bits(ce, 16, 2) == 3 ? 0 : bits(ce, 20, 2), false, ch);
        na = plan(alpha_arg(b, bits(ae, 13, 3), sc, at), alpha_arg(b, bits(ae, 10, 3), sc, at),
                  alpha_arg(b, bits(ae, 7, 3), sc, at), alpha_arg(b, bits(ae, 4, 3), sc, at), bits(ae, 18, 1),
                  bits(ae, 16, 2) == 3 ? 0 : bits(ae, 16, 2), bits(ae, 16, 2) == 3 ? 0 : bits(ae, 20, 2), true, ah);
        int n = std::max(nc, na);
        if (b.stage + n > 6) {
            out.overflow = true;
            out.unsupported++;
            break;
        }
        for (int k = 0; k < n; k++) {
            CombinerStage& st = out.stage[b.stage + k];
            u32 konst = 0;
            // the half that is shorter passes its result on
            Half c = k < nc ? ch[k] : Half(), a = k < na ? ah[k] : Half();
            // (its PREVIOUS is this pair's first stage, not the stage before the pair)
            if (k >= nc) {
                c.func = FUNC_REPLACE;
                c.s[0].kind = In::SRC, c.s[0].src = SRC_PREVIOUS, c.s[0].op = OP_RGB_SRC_COLOR;
                c.s[0].pair_prev = true;
            }
            if (k >= na) {
                a.func = FUNC_REPLACE;
                a.s[0].kind = In::SRC, a.s[0].src = SRC_PREVIOUS, a.s[0].op = OP_A_SRC_ALPHA;
                a.s[0].pair_prev = true;
            }
            if (k == 1) {
                // the second stage of a pair: PREVIOUS now means the first stage's result; an
                // input that meant the stage before the pair reads it from the combiner buffer
                // (which a stage sees with one stage of delay)
                for (Half* h : {&c, &a})
                    for (In& x : h->s)
                        if (x.kind == In::SRC && x.src == SRC_PREVIOUS && !x.pair_prev) {
                            if (at - 1 >= 0 && at - 1 <= 3) {
                                x.src = SRC_PREVIOUS_BUFFER;
                                if (h == &a)
                                    out.buf_a |= 1u << (at - 1);
                                else
                                    out.buf_rgb |= 1u << (at - 1);
                            } else {
                                out.unsupported++;
                            }
                        }
            }
            u32 have_bits = 0;
            // (both always: a conflict in the color half mustn't leave the alpha's constant out)
            bool merged_c = merge_consts(c, false, &konst, &have_bits);
            bool merged_a = merge_consts(a, true, &konst, &have_bits);
            if (!merged_c || !merged_a)
                out.unsupported++;  // two different constants in one stage: one of them wins
            apply_half(st, c, false);
            apply_half(st, a, true);
            st.konst = konst;
        }
        b.stage += n;
        // where this GX stage's outputs went
        u32 cdest = bits(ce, 22, 2), adest = bits(ae, 22, 2);
        b.reg_stage[cdest][0] = b.stage - 1;
        b.reg_stage[adest][1] = b.stage - 1;
    }
    out.n_stages = (u8)b.stage;
    // unused stages pass the result on
    for (int s = b.stage; s < 6; s++)
        out.stage[s] = pass_stage();
}

void remap_luminance(CombinerStage stage[6], u32 lum_units) {
    if (!(lum_units & 7))
        return;
    for (int i = 0; i < 6; i++) {
        CombinerStage& e = stage[i];
        for (int k = 0; k < 3; k++) {
            u32 src = e.src_rgb[k];
            if (src >= SRC_TEXTURE0 && src <= SRC_TEXTURE2 && (lum_units >> (src - SRC_TEXTURE0) & 1) &&
                (e.op_rgb[k] == OP_RGB_SRC_ALPHA || e.op_rgb[k] == OP_RGB_ONE_MINUS_SRC_ALPHA))
                e.op_rgb[k] += 2;  // -> SRC_R, ONE_MINUS_SRC_R
            src = e.src_a[k];
            if (src >= SRC_TEXTURE0 && src <= SRC_TEXTURE2 && (lum_units >> (src - SRC_TEXTURE0) & 1) &&
                (e.op_a[k] == OP_A_SRC_ALPHA || e.op_a[k] == OP_A_ONE_MINUS_SRC_ALPHA))
                e.op_a[k] += 2;  // -> SRC_R, ONE_MINUS_SRC_R
        }
    }
}

void combiner_words(const CombinerStage& s, u32 w[5]) {
    auto three = [](const u8* v) { return (u32)v[0] | (u32)v[1] << 4 | (u32)v[2] << 8; };
    w[0] = three(s.src_rgb) | three(s.src_a) << 16;
    w[1] = three(s.op_rgb) | three(s.op_a) << 12;
    w[2] = (u32)s.func_rgb | (u32)s.func_a << 16;
    w[3] = s.konst;
    w[4] = (u32)s.scale_rgb | (u32)s.scale_a << 16;
}

}  // namespace engine::gx
